#include "platform/ps5/elf/HostExports.h"

#include <cstdint>
#include <cstring>

// The export table maps the C/C++ runtime + libc symbol names a binary add-on
// imports to the eboot's own definitions. Its real contents are generated per
// set of supported add-ons by tools/ps5-gen-addon-exports.py (as an assembly
// file that can name mangled C++ symbols) and linked in as a strong
// kodi_addon_import_table / kodi_addon_import_count that overrides the weak fallback below.
//
// Until such a table is generated (e.g. before any binary add-on is built),
// this weak, empty default lets the eboot link and run: Python and every other
// part of Kodi need no exports, and a binary add-on simply fails to resolve its
// imports (reported by the loader) rather than the whole build failing to link.
extern "C"
{
struct HE
{
  const char* name;
  void* addr;
};

__attribute__((weak)) HE kodi_addon_import_table[] = {{nullptr, nullptr}};
__attribute__((weak)) unsigned long kodi_addon_import_count = 0;
}

void* host_export_resolver(const char* name, void*)
{
  for (HE* e = kodi_addon_import_table; e->name; ++e)
    if (std::strcmp(e->name, name) == 0)
      return e->addr;
  return nullptr;
}
