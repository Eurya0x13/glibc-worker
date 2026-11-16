#define _GNU_SOURCE
#include "signal-router.h"
#include "signals.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>

/* Only explicit bootstrap consumers link this object. Keep the native libc's
 * atomic wait, cancellation entry, timeout handling and reserved-signal filter.
 * We change the temporary mask argument, never the calling thread's mask.
 * Bionic LP64 aliases terminate in the 64-bit entry rather than calling back
 * through our public wrapper:
 * https://android.googlesource.com/platform/bionic/+/main/libc/bionic/poll.cpp
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/ppoll.c
 */
static const sigset_t *physical_mask(const sigset_t *logical,
                                     sigset_t *physical) {
  if (!logical || !signal_router_fault_notifications()) {
    return logical;
  }
  uint64_t bits;
  memcpy(&bits, logical, sizeof bits);
  bits = signal_host_physical_mask(bits);
  sigemptyset(physical);
  memcpy(physical, &bits, sizeof bits);
  return physical;
}

static char unavailable;
static _Atomic(void *) caches[8];

static void *resolve(_Atomic(void *) *cache, const char *name) {
  void *function = atomic_load_explicit(cache, memory_order_acquire);
  if (!function) {
    function = dlsym(RTLD_NEXT, name);
    if (function) {
      atomic_store_explicit(cache, function, memory_order_release);
    } else {
      function = &unavailable;
      atomic_store_explicit(cache, function, memory_order_release);
    }
  }
  if (function == &unavailable) {
    errno = ENOSYS;
    return NULL;
  }
  return function;
}

#ifdef __ANDROID__
#define NATIVE(name) #name "64"
#else
#define NATIVE(name) #name
#endif

int signal_suspend_prepare(void) {
  const char *names[] = {NATIVE(sigsuspend),
                         NATIVE(ppoll),
                         NATIVE(pselect),
                         NATIVE(epoll_pwait),
#ifdef __ANDROID__
                         "epoll_pwait2_64",
#else
                         "epoll_pwait2",
#endif
                         "poll",
                         "select",
                         "epoll_wait"};
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
    if (!resolve(&caches[i], names[i]) && i != 4) {
      return ENOSYS;
    }
  }
  return 0; /* epoll_pwait2 is optional and has a cached ENOSYS path. */
}

int sigsuspend(const sigset_t *mask) {
  typedef int (*function_type)(const sigset_t *);
  function_type function =
      (function_type)resolve(&caches[0], NATIVE(sigsuspend));
  if (!function) {
    return -1;
  }
  sigset_t physical;
  return function(physical_mask(mask, &physical));
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask) {
  typedef int (*function_type)(struct pollfd *, nfds_t, const struct timespec *,
                               const sigset_t *);
  function_type function = (function_type)resolve(&caches[1], NATIVE(ppoll));
  if (!function) {
    return -1;
  }
  sigset_t physical;
  signal_host_prune_notifications();
  return function(fds, count, timeout, physical_mask(mask, &physical));
}

int pselect(int count, fd_set *read, fd_set *write, fd_set *except,
            const struct timespec *timeout, const sigset_t *mask) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               const struct timespec *, const sigset_t *);
  function_type function = (function_type)resolve(&caches[2], NATIVE(pselect));
  if (!function) {
    return -1;
  }
  sigset_t physical;
  signal_host_prune_notifications();
  return function(count, read, write, except, timeout,
                  physical_mask(mask, &physical));
}

int epoll_pwait(int fd, struct epoll_event *events, int count, int timeout,
                const sigset_t *mask) {
  typedef int (*function_type)(int, struct epoll_event *, int, int,
                               const sigset_t *);
  function_type function =
      (function_type)resolve(&caches[3], NATIVE(epoll_pwait));
  if (!function) {
    return -1;
  }
  sigset_t physical;
  signal_host_prune_notifications();
  return function(fd, events, count, timeout, physical_mask(mask, &physical));
}

int epoll_pwait2(int fd, struct epoll_event *events, int count,
                 const struct timespec *timeout, const sigset_t *mask) {
  typedef int (*function_type)(int, struct epoll_event *, int,
                               const struct timespec *, const sigset_t *);
  function_type function = (function_type)resolve(&caches[4],
#ifdef __ANDROID__
                                                  "epoll_pwait2_64"
#else
                                                  "epoll_pwait2"
#endif
  );
  if (!function) {
    return -1;
  }
  sigset_t physical;
  signal_host_prune_notifications();
  return function(fd, events, count, timeout, physical_mask(mask, &physical));
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
  typedef int (*function_type)(struct pollfd *, nfds_t, int);
  function_type function = (function_type)resolve(&caches[5], "poll");
  if (!function) {
    return -1;
  }
  signal_host_prune_notifications();
  return function(fds, count, timeout);
}

int select(int count, fd_set *read, fd_set *write, fd_set *except,
           struct timeval *timeout) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               struct timeval *);
  function_type function = (function_type)resolve(&caches[6], "select");
  if (!function) {
    return -1;
  }
  signal_host_prune_notifications();
  return function(count, read, write, except, timeout);
}

int epoll_wait(int fd, struct epoll_event *events, int count, int timeout) {
  typedef int (*function_type)(int, struct epoll_event *, int, int);
  function_type function = (function_type)resolve(&caches[7], "epoll_wait");
  if (!function) {
    return -1;
  }
  signal_host_prune_notifications();
  return function(fd, events, count, timeout);
}

#ifdef __ANDROID__
/* sigset_t and sigset64_t are identical only for the supported LP64 targets.
 * Native resolution above always skips this object and its aliases. */
_Static_assert(sizeof(sigset64_t) == sizeof(sigset_t), "LP64 signal mask ABI");

int sigsuspend64(const sigset64_t *mask) {
  return sigsuspend((const sigset_t *)mask);
}

int ppoll64(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
            const sigset64_t *mask) {
  return ppoll(fds, count, timeout, (const sigset_t *)mask);
}

int pselect64(int count, fd_set *read, fd_set *write, fd_set *except,
              const struct timespec *timeout, const sigset64_t *mask) {
  return pselect(count, read, write, except, timeout, (const sigset_t *)mask);
}

int epoll_pwait64(int fd, struct epoll_event *events, int count, int timeout,
                  const sigset64_t *mask) {
  return epoll_pwait(fd, events, count, timeout, (const sigset_t *)mask);
}

int epoll_pwait2_64(int fd, struct epoll_event *events, int count,
                    const struct timespec *timeout, const sigset64_t *mask) {
  return epoll_pwait2(fd, events, count, timeout, (const sigset_t *)mask);
}
#endif
