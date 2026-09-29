#include "platform/ps5/elf/HostExports.h"
#include <cstring>
#include <cstdint>
extern "C" { struct HE { const char* name; void* addr; }; extern HE g_host_exports[]; }
void* host_export_resolver(const char* name, void*)
{
  for (HE* e = g_host_exports; e->name; ++e)
    if (std::strcmp(e->name, name) == 0)
      return e->addr;
  return nullptr;
}
