#include "platform/ps5/elf/ElfLoader.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// ELF64 (x86-64) types and constants, defined here so the file is identical on
// the Linux host and the PS5 (FreeBSD) sysroot, whose <elf.h> differ.
namespace
{
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

struct Ehdr { u8 e_ident[16]; u16 e_type, e_machine; u32 e_version; u64 e_entry, e_phoff, e_shoff;
              u32 e_flags; u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; };
struct Phdr { u32 p_type, p_flags; u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };
struct Dyn  { i64 d_tag; u64 d_val; };
struct Sym  { u32 st_name; u8 st_info, st_other; u16 st_shndx; u64 st_value, st_size; };
struct Rela { u64 r_offset; u64 r_info; i64 r_addend; };

constexpr u16 ET_DYN = 3;
constexpr u16 EM_X86_64 = 62;
constexpr u32 PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

constexpr i64 DT_NULL=0, DT_HASH=4, DT_STRTAB=5, DT_SYMTAB=6, DT_RELA=7, DT_RELASZ=8,
              DT_RELAENT=9, DT_SYMENT=11, DT_INIT=12, DT_PLTGOT=3, DT_PLTRELSZ=2,
              DT_JMPREL=23, DT_INIT_ARRAY=25, DT_INIT_ARRAYSZ=27, DT_GNU_HASH=0x6ffffef5;

constexpr u16 SHN_UNDEF = 0;
constexpr u8 STB_WEAK = 2;

// x86-64 relocation types (ABI-fixed).
constexpr u32 R_X86_64_64=1, R_X86_64_GLOB_DAT=6, R_X86_64_JUMP_SLOT=7, R_X86_64_RELATIVE=8,
              R_X86_64_DTPMOD64=16, R_X86_64_DTPOFF64=17, R_X86_64_TPOFF64=18;

inline u32 R_SYM(u64 i) { return static_cast<u32>(i >> 32); }
inline u32 R_TYPE(u64 i) { return static_cast<u32>(i & 0xffffffff); }

size_t page() { long p = sysconf(_SC_PAGESIZE); return p > 0 ? static_cast<size_t>(p) : 0x4000; }
u64 trunc_page(u64 x, size_t pg) { return x & ~static_cast<u64>(pg - 1); }
u64 round_page(u64 x, size_t pg) { return (x + pg - 1) & ~static_cast<u64>(pg - 1); }
} // namespace

namespace ps5elf
{

struct Image
{
  u8* base = nullptr;      // load bias (mapping - trunc(min_vaddr))
  u8* map = nullptr;       // actual mmap start
  size_t span = 0;         // mmap length
  const Phdr* phdr = nullptr;
  u16 phnum = 0;
  const Sym* symtab = nullptr;
  const char* strtab = nullptr;
  u32 symcount = 0;
  void (*init)() = nullptr;
  void (**init_array)() = nullptr;
  size_t init_arrayn = 0;
  HostResolver resolver = nullptr;
  void* user = nullptr;
  Stats st{};
};

namespace
{
u32 gnu_hash_symcount(const u32* gh)
{
  // Derive the highest symbol index + 1 from a GNU hash table.
  const u32 nbuckets = gh[0];
  const u32 symoffset = gh[1];
  const u32 bloom_size = gh[2];
  const u32* buckets = gh + 4 + bloom_size * 2; // 64-bit bloom words
  const u32* chain = buckets + nbuckets;
  u32 last = 0;
  for (u32 i = 0; i < nbuckets; ++i)
    if (buckets[i] > last)
      last = buckets[i];
  if (last < symoffset)
    return symoffset;
  // walk the chain of the highest bucket to its terminator (low bit set)
  u32 idx = last;
  while (!(chain[idx - symoffset] & 1))
    ++idx;
  return idx + 1;
}

// Resolve the symbol a relocation refers to. Returns false on hard failure.
bool resolve(Image* img, u32 symidx, u64* out, char* err, size_t errlen)
{
  const Sym& s = img->symtab[symidx];
  if (s.st_shndx != SHN_UNDEF)
  {
    *out = reinterpret_cast<u64>(img->base) + s.st_value; // defined here
    return true;
  }
  const char* name = img->strtab + s.st_name;
  void* h = img->resolver ? img->resolver(name, img->user) : nullptr;
  if (h)
  {
    *out = reinterpret_cast<u64>(h);
    return true;
  }
  if ((s.st_info >> 4) == STB_WEAK)
  {
    *out = 0; // unresolved weak -> 0, legal
    return true;
  }
  std::snprintf(err, errlen, "undefined symbol: %s", name);
  return false;
}

bool apply_rela(Image* img, const Rela* r, size_t n, char* err, size_t errlen)
{
  const u64 bias = reinterpret_cast<u64>(img->base);
  for (size_t i = 0; i < n; ++i)
  {
    const u32 type = R_TYPE(r[i].r_info);
    const u32 sym = R_SYM(r[i].r_info);
    u64* where = reinterpret_cast<u64*>(bias + r[i].r_offset);
    switch (type)
    {
      case R_X86_64_RELATIVE:
        *where = bias + static_cast<u64>(r[i].r_addend);
        img->st.relative++;
        break;
      case R_X86_64_64:
      case R_X86_64_GLOB_DAT:
      {
        u64 v = 0;
        if (!resolve(img, sym, &v, err, errlen))
          return false;
        *where = v + static_cast<u64>(r[i].r_addend);
        img->st.glob_dat++;
        break;
      }
      case R_X86_64_JUMP_SLOT:
      {
        u64 v = 0;
        if (!resolve(img, sym, &v, err, errlen))
          return false;
        *where = v;
        img->st.jump_slot++;
        break;
      }
      case R_X86_64_DTPMOD64:
      case R_X86_64_DTPOFF64:
      case R_X86_64_TPOFF64:
        img->st.tls++; // not yet supported; left as-is, reported by stats
        break;
      default:
        img->st.other++;
        break;
    }
  }
  return true;
}
} // namespace

Image* load(const void* image, size_t image_len, HostResolver resolver, void* user,
            char* err, size_t errlen)
{
  auto fail = [&](const char* m) -> Image* { std::snprintf(err, errlen, "%s", m); return nullptr; };
  if (image_len < sizeof(Ehdr))
    return fail("image too small");
  const auto* e = static_cast<const Ehdr*>(image);
  if (std::memcmp(e->e_ident, "\x7f""ELF", 4) != 0)
    return fail("not an ELF file");
  if (e->e_ident[4] != 2 /*ELFCLASS64*/ || e->e_ident[5] != 1 /*little-endian*/)
    return fail("not ELF64 LE");
  if (e->e_type != ET_DYN)
    return fail("not a shared object (ET_DYN)");
  if (e->e_machine != EM_X86_64)
    return fail("not x86-64");

  const size_t pg = page();
  const auto* ph = reinterpret_cast<const Phdr*>(static_cast<const u8*>(image) + e->e_phoff);

  // span of all PT_LOAD
  u64 min_v = ~0ull, max_v = 0;
  const Phdr* dynph = nullptr;
  bool has_tls = false;
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type == PT_LOAD)
    {
      min_v = ph[i].p_vaddr < min_v ? ph[i].p_vaddr : min_v;
      u64 end = ph[i].p_vaddr + ph[i].p_memsz;
      max_v = end > max_v ? end : max_v;
    }
    else if (ph[i].p_type == PT_DYNAMIC)
      dynph = &ph[i];
    else if (ph[i].p_type == PT_TLS)
      has_tls = true;
  }
  if (min_v == ~0ull || !dynph)
    return fail("no PT_LOAD or no PT_DYNAMIC");

  const u64 base_v = trunc_page(min_v, pg);
  const size_t span = round_page(max_v, pg) - base_v;
  void* m = mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    return fail("reservation mmap failed");

  auto* img = new Image();
  img->map = static_cast<u8*>(m);
  img->span = span;
  img->base = static_cast<u8*>(m) - base_v; // load bias
  img->resolver = resolver;
  img->user = user;
  (void)has_tls;

  // Copy each PT_LOAD in, RW (route B: write first, flip to X later).
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type != PT_LOAD)
      continue;
    u8* seg = img->base + ph[i].p_vaddr;
    u8* segpg = reinterpret_cast<u8*>(trunc_page(reinterpret_cast<u64>(seg), pg));
    size_t seglen = round_page(reinterpret_cast<u64>(seg) + ph[i].p_memsz, pg)
                    - reinterpret_cast<u64>(segpg);
    if (mprotect(segpg, seglen, PROT_READ | PROT_WRITE) != 0)
    {
      std::snprintf(err, errlen, "mprotect RW failed: %s", std::strerror(errno));
      munmap(m, span);
      delete img;
      return nullptr;
    }
    std::memcpy(seg, static_cast<const u8*>(image) + ph[i].p_offset, ph[i].p_filesz);
    if (ph[i].p_memsz > ph[i].p_filesz) // .bss
      std::memset(seg + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
  }

  // Parse PT_DYNAMIC.
  const auto* dyn = reinterpret_cast<const Dyn*>(img->base + dynph->p_vaddr);
  const Rela* rela = nullptr; size_t relasz = 0;
  const Rela* jmprel = nullptr; size_t pltrelsz = 0;
  const u32* sysv_hash = nullptr; const u32* gnu_hash = nullptr;
  for (const Dyn* d = dyn; d->d_tag != DT_NULL; ++d)
  {
    switch (d->d_tag)
    {
      case DT_SYMTAB: img->symtab = reinterpret_cast<const Sym*>(img->base + d->d_val); break;
      case DT_STRTAB: img->strtab = reinterpret_cast<const char*>(img->base + d->d_val); break;
      case DT_RELA:   rela = reinterpret_cast<const Rela*>(img->base + d->d_val); break;
      case DT_RELASZ: relasz = d->d_val; break;
      case DT_JMPREL: jmprel = reinterpret_cast<const Rela*>(img->base + d->d_val); break;
      case DT_PLTRELSZ: pltrelsz = d->d_val; break;
      case DT_HASH:   sysv_hash = reinterpret_cast<const u32*>(img->base + d->d_val); break;
      case DT_GNU_HASH: gnu_hash = reinterpret_cast<const u32*>(img->base + d->d_val); break;
      case DT_INIT:   img->init = reinterpret_cast<void(*)()>(img->base + d->d_val); break;
      case DT_INIT_ARRAY: img->init_array = reinterpret_cast<void(**)()>(img->base + d->d_val); break;
      case DT_INIT_ARRAYSZ: img->init_arrayn = d->d_val / sizeof(void*); break;
      default: break;
    }
  }
  if (!img->symtab || !img->strtab)
  {
    munmap(m, span); delete img; return fail("missing DT_SYMTAB/DT_STRTAB");
  }
  if (sysv_hash)
    img->symcount = sysv_hash[1]; // nchain
  else if (gnu_hash)
    img->symcount = gnu_hash_symcount(gnu_hash);
  // symcount only needed by symbol(); relocations index directly.

  // Apply relocations.
  if (rela && !apply_rela(img, rela, relasz / sizeof(Rela), err, errlen))
  {
    munmap(m, span); delete img; return nullptr;
  }
  if (jmprel && !apply_rela(img, jmprel, pltrelsz / sizeof(Rela), err, errlen))
  {
    munmap(m, span); delete img; return nullptr;
  }

  // Final protections per p_flags (route B flip: code becomes R+X now).
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type != PT_LOAD)
      continue;
    int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) |
               ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
               ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
    u8* seg = img->base + ph[i].p_vaddr;
    u8* segpg = reinterpret_cast<u8*>(trunc_page(reinterpret_cast<u64>(seg), pg));
    size_t seglen = round_page(reinterpret_cast<u64>(seg) + ph[i].p_memsz, pg)
                    - reinterpret_cast<u64>(segpg);
    if (mprotect(segpg, seglen, prot) != 0)
    {
      std::snprintf(err, errlen, "final mprotect failed: %s", std::strerror(errno));
      munmap(m, span); delete img; return nullptr;
    }
  }
  return img;
}

void run_init(Image* img)
{
  if (!img) return;
  if (img->init) img->init();
  for (size_t i = 0; i < img->init_arrayn; ++i)
    if (img->init_array[i]) img->init_array[i]();
}

void* symbol(Image* img, const char* name)
{
  if (!img || !img->symtab || !img->strtab) return nullptr;
  for (u32 i = 0; i < img->symcount; ++i)
  {
    const Sym& s = img->symtab[i];
    if (s.st_shndx != SHN_UNDEF && std::strcmp(img->strtab + s.st_name, name) == 0)
      return img->base + s.st_value;
  }
  return nullptr;
}

Stats stats(const Image* img) { return img ? img->st : Stats{}; }

void unload(Image* img)
{
  if (!img) return;
  if (img->map) munmap(img->map, img->span);
  delete img;
}

} // namespace ps5elf
