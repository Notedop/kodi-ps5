# In-process ELF loader (binary add-ons)

A PS5 title has no system dynamic loader, so binary add-ons (shared objects
Kodi normally `dlopen`s) cannot be loaded the usual way. The on-console
`kodi-jitprobe` established that a title *can* execute memory it writes if it
respects W^X: map a page RW, write code, `mprotect` it R+X, then call. (RWX in
one step faults; the Sce JIT path returned EINVAL. Route B - RW then +X - is
the one that works, and is what this loader uses.)

## Status

- [x] **Memory + relocation core** (`ElfLoader.{h,cpp}`). Maps `PT_LOAD`
      segments RW, applies `R_X86_64_RELATIVE`, `_64`, `_GLOB_DAT`,
      `_JUMP_SLOT`, resolves undefined symbols via a host callback, flips code
      to R+X, runs `DT_INIT`/`DT_INIT_ARRAY`. Verified on the host against a
      synthetic `.so` exercising every reloc kind, a data global, a
      host-imported function, and a constructor (all checks pass).
- [ ] **TLS relocations** (`DTPMOD64`/`DTPOFF64`/`TPOFF64`): counted and
      reported by `Stats`, not yet applied. Needed for add-ons that use
      `__thread`. Requires allocating a TLS block and wiring the module id.
- [x] **Host symbol table** (`HostExports.{h,cpp}` + `tools/ps5-gen-addon-exports.py`).
      Confirmed add-ons call the host through the `AddonGlobalInterface`
      function-pointer struct (resolved at `ADDON_Create`), *not* named imports
      - so the only named symbols an add-on needs from the eboot are the C/C++
      runtime + libc. The generator turns an add-on's undefined-symbol list into
      an **assembly** export table (asm can name mangled C++ symbols like
      `_Znwm`), linked into the eboot so addresses bind at link time - no
      `--export-dynamic` and no runtime dynsym query needed, and any missing
      symbol is a link error, not a runtime crash. Verified on the host: an
      add-on importing `operator new/delete` and `memcpy` binds them to the
      host's own instances (shared allocator) and runs correctly.
- [x] **Wired into the loader factory** (`PS5AddonLoader.{h,cpp}` +
      `patches/kodi/0017-ps5-binary-addon-inprocess-loader.patch`).
      `CPS5AddonLoader` implements `LibraryLoader` over `ps5elf`; the
      `DllLoaderContainer` factory selects it on PS5 instead of the dlopen
      `SoLoader`, so `CAddonDll`/`DllAddon` reach it unchanged. It reads the
      add-on .so, loads+relocates it with the `HostExports` resolver, runs the
      constructors, and returns `ADDON_*` through `ResolveExport`. Compiles
      against the real Kodi interfaces; runtime-testable once an add-on .so is
      cross-built.
- [ ] **First target**: build `inputstream.adaptive` for the PS5 sysroot and
      load it through the chain end to end.

## Testing

The core is host-testable because ELF64/x86-64 relocation logic and
`mmap`+`mprotect` are identical on the build host and PS5. See the harness in
the project notes; it loads a synthetic `.so` and checks a computed result.
