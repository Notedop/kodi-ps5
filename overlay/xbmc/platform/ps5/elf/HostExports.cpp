#include "platform/ps5/elf/HostExports.h"

#include <cstdint>
#include <cstring>

// Maps the C/C++ runtime + libc symbol names a binary add-on imports to the
// eboot's own definitions. The real table is generated per set of supported
// add-ons by tools/ps5-gen-addon-exports.py and linked in as a strong
// kodi_addon_import_table / _count / _fill overriding the weak defaults here.
//
// The table is filled lazily by kodi_addon_import_fill() on first resolve -
// deliberately NOT a static constructor: the FSELF converter segfaults on any
// eboot object that adds an .init_array entry. The generated file therefore
// carries no constructor and no relocations (addresses are taken at run time).
//
// With no generated table (default build) the weak empty versions below let
// the eboot link and run; a binary add-on then fails to resolve its imports
// (reported by the loader) rather than the build failing.
extern "C"
{
struct HE
{
  const char* name;
  void* addr;
};

__attribute__((weak)) HE kodi_addon_import_table[] = {{nullptr, nullptr}};
__attribute__((weak)) unsigned long kodi_addon_import_count = 0;
__attribute__((weak)) void kodi_addon_import_fill(void) {}
}

void* host_export_resolver(const char* name, void*)
{
  static bool filled = false;
  if (!filled)
  {
    kodi_addon_import_fill(); // no-op for the weak default; populates the generated table
    filled = true;
  }
  for (HE* e = kodi_addon_import_table; e->name; ++e)
    if (std::strcmp(e->name, name) == 0)
      return e->addr;
  return nullptr;
}
