/*
 * Prototypes for functions that exist on the PS5 target but that the SDK's
 * headers do not declare. Python's configure links against them (their
 * symbols are present), so it defines HAVE_* for them - but the call sites
 * then trip -Werror=implicit-function-declaration. Force-included into every
 * cross-compiled translation unit (CPPFLAGS -include), never into the host
 * build.
 *
 *  getentropy         - provided by Kodi's C library shim
 *                       (shims/native-app/libc_posix.c, kern.arandom), which
 *                       configure is told about via ac_cv_func_getentropy=yes.
 *                       Load-bearing: Python's hash-seed randomization calls
 *                       it at interpreter startup; without a working entropy
 *                       source Python fails to initialize.
 *  pthread_getname_np - exported by the SDK's libkernel stubs; used by
 *                       traceback.c to name threads in dumps.
 */
#pragma once

#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

int getentropy(void* buf, size_t buflen);
int pthread_getname_np(pthread_t thread, char* name, size_t len);

#ifdef __cplusplus
}
#endif
