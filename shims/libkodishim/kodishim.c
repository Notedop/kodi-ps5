/* Symbols libpython references that the SDK's stub libraries do not provide,
 * so the kodi.bin link (which sees only the SDK stubs) can resolve them.
 *
 * The final eboot is linked by scripts/30-deploy.sh with the native-app shims
 * (shims/native-app/libc_posix.c) as plain objects, whose getentropy() takes
 * precedence over this archive's copy: an archive member is only extracted
 * for a symbol still undefined, so there is no duplicate. explicit_bzero has
 * no other provider and comes from here in both links. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

static int kernel_random(void* buf, size_t len)
{
  unsigned char* p = (unsigned char*)buf;
  while (len > 0)
  {
    int mib[2] = {CTL_KERN, KERN_ARND};
    size_t chunk = len > 256 ? 256 : len;
    size_t got = chunk;
    if (sysctl(mib, 2, p, &got, NULL, 0) != 0 || got == 0)
      break;
    p += got;
    len -= got;
  }
  if (len == 0)
    return 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  while (len > 0)
  {
    ssize_t n = read(fd, p, len);
    if (n <= 0)
      break;
    p += n;
    len -= (size_t)n;
  }
  close(fd);
  return len == 0 ? 0 : -1;
}

int getentropy(void* buf, size_t len)
{
  if (len > 256 || kernel_random(buf, len) != 0)
  {
    errno = EIO;
    return -1;
  }
  return 0;
}

/* Must not be optimized away: write through a volatile pointer. */
void explicit_bzero(void* buf, size_t len)
{
  volatile unsigned char* p = (volatile unsigned char*)buf;
  while (len--)
    *p++ = 0;
}
