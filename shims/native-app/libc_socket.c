/*
 * BSD sockets for a native title, routed through Sony's libSceNet.
 *
 * Python's socket module calls the BSD socket()/connect()/send()/recv()/...
 * which the SDK binds to the raw libkernel syscalls. A title's sandbox denies
 * those (connect() -> EACCES: every outbound connection from Python failed
 * "[Errno 13] Permission denied"). Kodi's own networking works because curl
 * goes through libSceNet's sceNet* API, which titles are allowed to use. This
 * shim makes the BSD names call the sceNet* equivalents too, so Python's
 * sockets - and therefore urllib/requests in add-ons - work.
 *
 * Linked with --wrap for each name (scripts/30-deploy.sh). Only the operations
 * Python needs for outbound TCP/UDP clients are routed; the rest fall through
 * to __real_* (the libkernel syscall) unchanged, which is correct for AF_UNIX
 * socketpairs (used by Kodi's wake-up pipes) that the sandbox does permit.
 *
 * sceNet returns -1 on error and sets a Sce error code fetched with
 * sceNetErrnoLoc(); map the common ones to POSIX errno so Python behaves.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

/* ---- libSceNet (declared here; no SDK header ships these) ---------------- */
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
int sceNetAccept(int s, SceNetSockaddr* addr, int* addrlen);
int sceNetSendto(int s, const void* buf, size_t len, int flags, const SceNetSockaddr* to, int tolen);
int sceNetRecvfrom(int s, void* buf, size_t len, int flags, SceNetSockaddr* from, int* fromlen);
int sceNetSetsockopt(int s, int level, int optname, const void* optval, int optlen);
int sceNetGetsockopt(int s, int level, int optname, void* optval, int* optlen);
int sceNetGetsockname(int s, SceNetSockaddr* name, int* namelen);
int sceNetGetpeername(int s, SceNetSockaddr* name, int* namelen);
int sceNetShutdown(int s, int how);
int* sceNetErrnoLoc(void);

/* Real libkernel syscalls, under their --wrap names. */
int __real_socket(int, int, int);
int __real_connect(int, const struct sockaddr*, socklen_t);
int __real_bind(int, const struct sockaddr*, socklen_t);
int __real_listen(int, int);
int __real_accept(int, struct sockaddr*, socklen_t*);
ssize_t __real_sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t __real_recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);
int __real_setsockopt(int, int, int, const void*, socklen_t);
int __real_getsockopt(int, int, int, void*, socklen_t*);
int __real_getsockname(int, struct sockaddr*, socklen_t*);
int __real_getpeername(int, struct sockaddr*, socklen_t*);
int __real_shutdown(int, int);
int __real_close(int);

/* Sockets we created via sceNet: their fds must be closed/read/written through
 * sceNet, and are indistinguishable from libkernel fds otherwise. Track them. */
#define MAX_TRACKED 256
static int g_scefds[MAX_TRACKED];
static int g_scefds_n = 0;

static void track(int fd)
{
  if (g_scefds_n < MAX_TRACKED)
    g_scefds[g_scefds_n++] = fd;
}
static int is_sce(int fd)
{
  for (int i = 0; i < g_scefds_n; i++)
    if (g_scefds[i] == fd)
      return 1;
  return 0;
}
static void untrack(int fd)
{
  for (int i = 0; i < g_scefds_n; i++)
    if (g_scefds[i] == fd)
    {
      g_scefds[i] = g_scefds[--g_scefds_n];
      return;
    }
}

static int sce_fail(void)
{
  int* e = sceNetErrnoLoc();
  int se = e ? *e : 0;
  /* Common SCE_NET_E* codes -> POSIX. Values are the low bits of the Sce code. */
  switch (se & 0xff)
  {
    case 0x0d: errno = EACCES; break;      /* unlikely now, but map it */
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

/* BSD sockaddr and SceNetSockaddr have the same byte layout for IPv4/IPv6
 * (len,family,port,addr...) except BSD's sa_family is 1 byte here too on this
 * libc, so a direct copy of sa_len bytes works. Normalize length/family. */
static int to_sce(const struct sockaddr* sa, socklen_t len, SceNetSockaddr* out, int* outlen)
{
  if (!sa || len > (socklen_t)sizeof(SceNetSockaddr))
    return -1;
  memcpy(out, sa, len);
  out->sa_len = (uint8_t)len;
  *outlen = (int)len;
  return 0;
}
static void from_sce(const SceNetSockaddr* in, int inlen, struct sockaddr* sa, socklen_t* len)
{
  if (!sa || !len)
    return;
  int n = inlen < (int)*len ? inlen : (int)*len;
  memcpy(sa, in, n);
  *len = n;
}

int __wrap_socket(int domain, int type, int protocol)
{
  /* AF_UNIX (1) stays on the kernel path: socketpair works and Kodi uses it. */
  if (domain == AF_UNIX)
    return __real_socket(domain, type, protocol);
  int s = sceNetSocket("kodi", domain, type, protocol);
  if (s < 0)
    return sce_fail();
  track(s);
  return s;
}

int __wrap_connect(int s, const struct sockaddr* addr, socklen_t len)
{
  if (!is_sce(s))
    return __real_connect(s, addr, len);
  SceNetSockaddr sa;
  int sl;
  if (to_sce(addr, len, &sa, &sl) != 0)
  {
    errno = EINVAL;
    return -1;
  }
  return sceNetConnect(s, &sa, sl) < 0 ? sce_fail() : 0;
}

ssize_t __wrap_sendto(int s, const void* buf, size_t len, int flags, const struct sockaddr* to,
                      socklen_t tolen)
{
  if (!is_sce(s))
    return __real_sendto(s, buf, len, flags, to, tolen);
  SceNetSockaddr sa;
  int sl = 0;
  const SceNetSockaddr* pto = NULL;
  if (to && to_sce(to, tolen, &sa, &sl) == 0)
    pto = &sa;
  int r = sceNetSendto(s, buf, len, flags, pto, sl);
  return r < 0 ? sce_fail() : r;
}

ssize_t __wrap_recvfrom(int s, void* buf, size_t len, int flags, struct sockaddr* from,
                        socklen_t* fromlen)
{
  if (!is_sce(s))
    return __real_recvfrom(s, buf, len, flags, from, fromlen);
  SceNetSockaddr sa;
  int sl = (int)sizeof sa;
  int r = sceNetRecvfrom(s, buf, len, flags, from ? &sa : NULL, from ? &sl : NULL);
  if (r < 0)
    return sce_fail();
  if (from)
    from_sce(&sa, sl, from, fromlen);
  return r;
}

/* send/recv are sendto/recvfrom with no address; Python uses both. */
ssize_t __wrap_send(int s, const void* buf, size_t len, int flags)
{
  return __wrap_sendto(s, buf, len, flags, NULL, 0);
}
ssize_t __wrap_recv(int s, void* buf, size_t len, int flags)
{
  return __wrap_recvfrom(s, buf, len, flags, NULL, NULL);
}

int __wrap_setsockopt(int s, int level, int opt, const void* val, socklen_t len)
{
  if (!is_sce(s))
    return __real_setsockopt(s, level, opt, val, len);
  return sceNetSetsockopt(s, level, opt, val, (int)len) < 0 ? sce_fail() : 0;
}
int __wrap_getsockopt(int s, int level, int opt, void* val, socklen_t* len)
{
  if (!is_sce(s))
    return __real_getsockopt(s, level, opt, val, len);
  int l = (int)*len;
  int r = sceNetGetsockopt(s, level, opt, val, &l);
  *len = l;
  return r < 0 ? sce_fail() : 0;
}
int __wrap_getsockname(int s, struct sockaddr* name, socklen_t* len)
{
  if (!is_sce(s))
    return __real_getsockname(s, name, len);
  SceNetSockaddr sa;
  int l = (int)sizeof sa;
  if (sceNetGetsockname(s, &sa, &l) < 0)
    return sce_fail();
  from_sce(&sa, l, name, len);
  return 0;
}
int __wrap_getpeername(int s, struct sockaddr* name, socklen_t* len)
{
  if (!is_sce(s))
    return __real_getpeername(s, name, len);
  SceNetSockaddr sa;
  int l = (int)sizeof sa;
  if (sceNetGetpeername(s, &sa, &l) < 0)
    return sce_fail();
  from_sce(&sa, l, name, len);
  return 0;
}
int __wrap_bind(int s, const struct sockaddr* addr, socklen_t len)
{
  if (!is_sce(s))
    return __real_bind(s, addr, len);
  SceNetSockaddr sa;
  int sl;
  if (to_sce(addr, len, &sa, &sl) != 0)
  {
    errno = EINVAL;
    return -1;
  }
  return sceNetBind(s, &sa, sl) < 0 ? sce_fail() : 0;
}
int __wrap_listen(int s, int backlog)
{
  if (!is_sce(s))
    return __real_listen(s, backlog);
  return sceNetListen(s, backlog) < 0 ? sce_fail() : 0;
}
int __wrap_shutdown(int s, int how)
{
  if (!is_sce(s))
    return __real_shutdown(s, how);
  return sceNetShutdown(s, how) < 0 ? sce_fail() : 0;
}

/* close() must route sceNet sockets to sceNetSocketClose; everything else is a
 * normal fd. --wrap=close. */
int __wrap_close(int fd)
{
  if (is_sce(fd))
  {
    untrack(fd);
    return sceNetSocketClose(fd) < 0 ? sce_fail() : 0;
  }
  return __real_close(fd);
}
