/* pipe() for a PS5 title: the kernel exports pipe, but the call fails inside
 * the sandbox (curl's resolver wake-up, which is a pipe, could never start).
 * An AF_UNIX socketpair does everything a pipe is used for here: read, write,
 * poll, close - so it is the fallback whenever the real call fails.
 * Linked with --wrap=pipe (scripts/30-deploy.sh). */
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int __real_pipe(int fds[2]);

int __wrap_pipe(int fds[2])
{
  if (__real_pipe(fds) == 0)
    return 0;
  return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}
