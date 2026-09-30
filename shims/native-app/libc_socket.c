/*
 * Python-only BSD socket layer over Sony's libSceNet.
 *
 * The PS5 sandbox denies the raw socket()/connect() syscalls (Python got
 * EACCES on every connection); sceNet* is the allowed path (Kodi's curl uses
 * it). These ps5_* functions are called ONLY from Python's socket module,
 * which pacbrew/python3/ps5_pysocket.h redirects into here at compile time -
 * so nothing else in the process (Kodi, curl, libc) is affected. Every fd
 * these return is a sceNet socket, so no fd-type tracking is needed.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef struct SceNetSockaddr
{
  uint8_t sa_len;
  uint8_t sa_family;
  char sa_data[14];
} SceNetSockaddr;

int sceNetSocket(const char* name, int family, int type, int protocol);
int sceNetSocketClose(int s);
int sceNetConnect(int s, const SceNetSockaddr* addr, int addrlen);
int sceNetBind(int s, const SceNetSockaddr* addr, int addrlen);
int sceNetListen(int s, int backlog);
int sceNetSendto(int s, const void* buf, size_t len, int flags, const SceNetSockaddr* to, int tolen);
int sceNetRecvfrom(int s, void* buf, size_t len, int flags, SceNetSockaddr* from, int* fromlen);
int sceNetSetsockopt(int s, int level, int optname, const void* optval, int optlen);
int sceNetGetsockopt(int s, int level, int optname, void* optval, int* optlen);
int sceNetGetsockname(int s, SceNetSockaddr* name, int* namelen);
int sceNetGetpeername(int s, SceNetSockaddr* name, int* namelen);
int sceNetShutdown(int s, int how);
int* sceNetErrnoLoc(void);

struct sockaddr;

static int sce_fail(void)
{
  int* e = sceNetErrnoLoc();
  int se = e ? *e : 0;
  switch (se & 0xff)
  {
    case 0x20: errno = EPIPE; break;
    case 0x23: errno = EWOULDBLOCK; break;
    case 0x24: errno = EINPROGRESS; break;
    case 0x25: errno = EALREADY; break;
    case 0x28: errno = EISCONN; break;
    case 0x27: errno = ENOTCONN; break;
    case 0x3d: errno = ECONNRESET; break;
    case 0x3e: errno = ECONNREFUSED; break;
    case 0x40: errno = ETIMEDOUT; break;
    default:   errno = se ? EIO : ECONNREFUSED; break;
  }
  return -1;
}

static int to_sce(const struct sockaddr* sa, unsigned int len, SceNetSockaddr* out, int* outlen)
{
  if (!sa || len > sizeof(SceNetSockaddr))
    return -1;
  memcpy(out, sa, len);
  out->sa_len = (uint8_t)len;
  *outlen = (int)len;
  return 0;
}
static void from_sce(const SceNetSockaddr* in, int inlen, struct sockaddr* sa, unsigned int* len)
{
  if (!sa || !len)
    return;
  int n = inlen < (int)*len ? inlen : (int)*len;
  memcpy(sa, in, n);
  *len = (unsigned int)n;
}

int ps5_socket(int domain, int type, int protocol)
{
  int s = sceNetSocket("python", domain, type, protocol);
  return s < 0 ? sce_fail() : s;
}
int ps5_close(int fd)
{
  return sceNetSocketClose(fd) < 0 ? sce_fail() : 0;
}
int ps5_connect(int s, const struct sockaddr* addr, unsigned int len)
{
  SceNetSockaddr sa; int sl;
  if (to_sce(addr, len, &sa, &sl)) { errno = EINVAL; return -1; }
  return sceNetConnect(s, &sa, sl) < 0 ? sce_fail() : 0;
}
int ps5_bind(int s, const struct sockaddr* addr, unsigned int len)
{
  SceNetSockaddr sa; int sl;
  if (to_sce(addr, len, &sa, &sl)) { errno = EINVAL; return -1; }
  return sceNetBind(s, &sa, sl) < 0 ? sce_fail() : 0;
}
int ps5_listen(int s, int backlog)
{
  return sceNetListen(s, backlog) < 0 ? sce_fail() : 0;
}
long ps5_sendto(int s, const void* buf, unsigned long len, int flags, const struct sockaddr* to, unsigned int tolen)
{
  SceNetSockaddr sa; int sl = 0; const SceNetSockaddr* pto = NULL;
  if (to && to_sce(to, tolen, &sa, &sl) == 0) pto = &sa;
  int r = sceNetSendto(s, buf, (size_t)len, flags, pto, sl);
  return r < 0 ? sce_fail() : r;
}
long ps5_recvfrom(int s, void* buf, unsigned long len, int flags, struct sockaddr* from, unsigned int* fromlen)
{
  SceNetSockaddr sa; int sl = (int)sizeof sa;
  int r = sceNetRecvfrom(s, buf, (size_t)len, flags, from ? &sa : NULL, from ? &sl : NULL);
  if (r < 0) return sce_fail();
  if (from) from_sce(&sa, sl, from, fromlen);
  return r;
}
long ps5_send(int s, const void* buf, unsigned long len, int flags)
{
  return ps5_sendto(s, buf, len, flags, NULL, 0);
}
long ps5_recv(int s, void* buf, unsigned long len, int flags)
{
  return ps5_recvfrom(s, buf, len, flags, NULL, NULL);
}
int ps5_setsockopt(int s, int level, int opt, const void* val, unsigned int len)
{
  return sceNetSetsockopt(s, level, opt, val, (int)len) < 0 ? sce_fail() : 0;
}
int ps5_getsockopt(int s, int level, int opt, void* val, unsigned int* len)
{
  int l = (int)*len; int r = sceNetGetsockopt(s, level, opt, val, &l); *len = (unsigned int)l;
  return r < 0 ? sce_fail() : 0;
}
int ps5_getsockname(int s, struct sockaddr* name, unsigned int* len)
{
  SceNetSockaddr sa; int l = (int)sizeof sa;
  if (sceNetGetsockname(s, &sa, &l) < 0) return sce_fail();
  from_sce(&sa, l, name, len); return 0;
}
int ps5_getpeername(int s, struct sockaddr* name, unsigned int* len)
{
  SceNetSockaddr sa; int l = (int)sizeof sa;
  if (sceNetGetpeername(s, &sa, &l) < 0) return sce_fail();
  from_sce(&sa, l, name, len); return 0;
}
int ps5_shutdown(int s, int how)
{
  return sceNetShutdown(s, how) < 0 ? sce_fail() : 0;
}
