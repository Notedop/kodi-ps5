#include "platform/ps5/elf/HostExports.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <link.h>

// Resolve a binary add-on's undefined symbols against the eboot's OWN exported
// symbols, read at run time from the eboot's dynamic symbol table. No
// generated table and no extra linked object: the FSELF converter segfaults on
// any added defined-data object (bisected at length - see git history), so the
// thing it crashes on is never created. A title has no runtime loader/dlsym,
// but the eboot is an ordinary PIE whose _DYNAMIC array is linked in and whose
// .dynsym/.dynstr/.hash the kernel maps; we walk them directly.
//
// The add-on imports only C/C++ runtime symbols (libc, libc++, libunwind),
// which are statically in the eboot, so a linear scan of the eboot's dynsym
// finds them. The scan runs once per add-on load - cost is irrelevant.

namespace
{
struct SelfSyms
{
  const ElfW(Sym)* symtab = nullptr;
  const char* strtab = nullptr;
  const uint32_t* gnu_hash = nullptr;
  const ElfW(Word)* elf_hash = nullptr;
  uintptr_t base = 0;
  uint32_t count = 0;
  bool ready = false;
};
SelfSyms g_self;

uintptr_t page_down(uintptr_t a) { return a & ~uintptr_t(0xfff); }

// The mapped ELF header is the image base: scan down from _DYNAMIC for the
// ELF magic on a page boundary.
const ElfW(Ehdr)* find_ehdr(uintptr_t from)
{
  for (uintptr_t p = page_down(from); p; p -= 0x1000)
  {
    const unsigned char* m = reinterpret_cast<const unsigned char*>(p);
    if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')
      return reinterpret_cast<const ElfW(Ehdr)*>(p);
    if (from - p > (256u << 20))
      break;
  }
  return nullptr;
}

// Highest symbol index + 1 from the GNU hash table (bounded chain walk).
uint32_t count_from_gnu_hash(const uint32_t* gh)
{
  if (!gh)
    return 0;
  const uint32_t nbuckets = gh[0];
  const uint32_t symoffset = gh[1];
  const uint32_t bloom_size = gh[2];
  const uintptr_t* bloom = reinterpret_cast<const uintptr_t*>(gh + 4);
  const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_size);
  const uint32_t* chain = buckets + nbuckets;
  uint32_t last = 0;
  for (uint32_t i = 0; i < nbuckets; ++i)
    if (buckets[i] > last)
      last = buckets[i];
  if (last < symoffset)
    return symoffset;
  uint32_t ix = last;
  while (!(chain[ix - symoffset] & 1))
    ++ix;
  return ix + 1;
}

void init()
{
  if (g_self.ready)
    return;
  g_self.ready = true;

  const uintptr_t dyn_run = reinterpret_cast<uintptr_t>(&_DYNAMIC[0]);
  const ElfW(Ehdr)* eh = find_ehdr(dyn_run);
  if (!eh)
    return;
  const uintptr_t imgbase = reinterpret_cast<uintptr_t>(eh);

  // Load bias = runtime(_DYNAMIC) - link-time vaddr(PT_DYNAMIC). For the
  // statically-linked eboot the kernel applies one bias to the whole image and
  // leaves DT_* d_ptr at link-time values, so we add the bias to each.
  const ElfW(Phdr)* ph = reinterpret_cast<const ElfW(Phdr)*>(imgbase + eh->e_phoff);
  uintptr_t dyn_vaddr = 0;
  for (unsigned i = 0; i < eh->e_phnum; ++i)
    if (ph[i].p_type == PT_DYNAMIC)
    {
      dyn_vaddr = ph[i].p_vaddr;
      break;
    }
  const uintptr_t bias = dyn_vaddr ? dyn_run - dyn_vaddr : imgbase;
  g_self.base = bias;

  // DT_* d_ptr may be link-time (static eboot: add the bias) or already mapped
  // (some runtimes pre-relocate them: add nothing). Decide per the first tag
  // seen: if d_ptr already lands inside the mapped image, it is pre-relocated.
  auto fix = [&](uintptr_t p) -> uintptr_t {
    if (p >= imgbase && p < imgbase + (uintptr_t(1) << 33))
      return p; // already a mapped address
    return bias + p; // link-time offset
  };
  for (const ElfW(Dyn)* d = _DYNAMIC; d->d_tag != DT_NULL; ++d)
  {
    switch (d->d_tag)
    {
      case DT_SYMTAB:   g_self.symtab = reinterpret_cast<const ElfW(Sym)*>(fix(d->d_un.d_ptr)); break;
      case DT_STRTAB:   g_self.strtab = reinterpret_cast<const char*>(fix(d->d_un.d_ptr)); break;
      case DT_GNU_HASH: g_self.gnu_hash = reinterpret_cast<const uint32_t*>(fix(d->d_un.d_ptr)); break;
      case DT_HASH:     g_self.elf_hash = reinterpret_cast<const ElfW(Word)*>(fix(d->d_un.d_ptr)); break;
      default: break;
    }
  }
  // st_value is link-time; bias it the same way a mapped table would be. When
  // the tables were pre-relocated, the symbols' st_value still needs the bias.
  (void)imgbase;
  g_self.count = g_self.elf_hash ? g_self.elf_hash[1] : count_from_gnu_hash(g_self.gnu_hash);
}
} // namespace

void* host_export_resolver(const char* name, void*)
{
  init();
  if (!g_self.symtab || !g_self.strtab || !g_self.count)
    return nullptr;
  for (uint32_t i = 0; i < g_self.count; ++i)
  {
    const ElfW(Sym)& s = g_self.symtab[i];
    if (s.st_shndx == SHN_UNDEF || s.st_value == 0)
      continue;
    if (std::strcmp(g_self.strtab + s.st_name, name) == 0)
      return reinterpret_cast<void*>(g_self.base + s.st_value);
  }
  return nullptr;
}
