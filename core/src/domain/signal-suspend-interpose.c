#define _GNU_SOURCE
#include "signal-router.h"
#include "signals.h"
#include "signal-select.h"
#include "signal-libc-mask.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/select.h>

/* Only explicit bootstrap consumers link this object. Inactive or NULL-mask
 * calls retain their native libc backend. Active-channel LP64 calls publish
 * a stable row around their atomic kernel mask-wait and preserve cancellation
 * checks without exposing automatic cleanup records to user handlers.
 * Bionic LP64 aliases terminate in the 64-bit entry rather than calling back
 * through our public wrapper:
 * https://android.googlesource.com/platform/bionic/+/main/libc/bionic/poll.cpp
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/ppoll.c
 */
/* A wide outer fault mask must not erase the execution mask of an atomic
 * wait. Publish a stable row while delivery is blocked, then let the raw LP64
 * syscall install its temporary mask atomically. Native GNU cancellation is
 * checked under a bounded cleanup during masked setup and after retirement.
 * Private/raw libc wait paths are outside the explicit bootstrap interposition
 * contract.
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/ppoll.c
 * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
 */
static int mask_wait_enabled(const sigset_t *mask) {
  return mask && signal_router_fault_notifications();
}

static void cancellation_point(void) {
#ifndef __ANDROID__
  pthread_testcancel();
#endif
}

static void *begin_wait(const sigset_t *mask, int filter) {
  cancellation_point();
  uint64_t bits;
  memcpy(&bits, mask, sizeof bits);
#ifdef __ANDROID__
  /* Bionic filters sigsuspend/poll/select, but its epoll wrappers pass masks
   * unchanged. Keep that per-interface distinction, including timer bit 32.
   * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/bionic/sys_epoll.cpp
   */
  if (filter) {
    bits = gw_signal_libc_mask(bits, SIG_SETMASK);
  }
#else
  (void)filter;
#endif
  void *record = gw_signal_mask_wait_begin(bits);
  if (record) {
    extern void gw_signal_mask_wait_test_point(int) __attribute__((weak));
    if (gw_signal_mask_wait_test_point) {
      gw_signal_mask_wait_test_point(1);
    }
    gw_signal_mask_wait_cancel_check(record);
    if (gw_signal_mask_wait_test_point) {
      gw_signal_mask_wait_test_point(2);
    }
  }
  return record;
}

static int end_wait(void *record, int result) {
  int error = errno;
  result = gw_signal_mask_wait_end(record, result, error);
  cancellation_point();
  return result;
}

static const struct timespec *copy_timeout(const struct timespec *timeout,
                                           struct timespec *copy) {
  if (timeout) {
    *copy = *timeout;
    return copy;
  }
  return NULL;
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
  if (!mask_wait_enabled(mask)) {
    return function(mask);
  }

  void *record = begin_wait(mask, 1);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
  int result = gw_signal_wait_result(gw_signal_wait_syscall(
      record, SYS_rt_sigsuspend, (long)&bits, sizeof bits, 0, 0, 0, 0));
  return end_wait(record, result);
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask) {
  typedef int (*function_type)(struct pollfd *, nfds_t, const struct timespec *,
                               const sigset_t *);
  function_type function = (function_type)resolve(&caches[1], NATIVE(ppoll));
  if (!function) {
    return -1;
  }
  if (!mask_wait_enabled(mask)) {
    signal_host_prune_notifications();
    return function(fds, count, timeout, mask);
  }

  struct timespec left;
  timeout = copy_timeout(timeout, &left);
  void *record = begin_wait(mask, 1);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
  int result = gw_signal_wait_result(
      gw_signal_wait_syscall(record, SYS_ppoll, (long)fds, count, (long)timeout,
                             (long)&bits, sizeof bits, 0));
  return end_wait(record, result);
}

int pselect(int count, fd_set *read, fd_set *write, fd_set *except,
            const struct timespec *timeout, const sigset_t *mask) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               const struct timespec *, const sigset_t *);
  function_type function = (function_type)resolve(&caches[2], NATIVE(pselect));
  if (!function) {
    return -1;
  }
  if (!mask_wait_enabled(mask)) {
    signal_host_prune_notifications();
    return function(count, read, write, except, timeout, mask);
  }

  struct timespec left;
  timeout = copy_timeout(timeout, &left);
  void *record = begin_wait(mask, 1);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);

  struct {
    const uint64_t *mask;
    size_t size;
  } data = {&bits, sizeof bits};

  int result = gw_signal_wait_result(gw_signal_wait_syscall(
      record, SYS_pselect6, count, (long)read, (long)write, (long)except,
      (long)timeout, (long)&data));
  return end_wait(record, result);
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
  if (!mask_wait_enabled(mask)) {
    signal_host_prune_notifications();
    return function(fd, events, count, timeout, mask);
  }

  void *record = begin_wait(mask, 0);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
  int result = gw_signal_wait_result(
      gw_signal_wait_syscall(record, SYS_epoll_pwait, fd, (long)events, count,
                             timeout, (long)&bits, sizeof bits));
  return end_wait(record, result);
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
  if (!mask_wait_enabled(mask)) {
    signal_host_prune_notifications();
    return function(fd, events, count, timeout, mask);
  }

  struct timespec left;
  timeout = copy_timeout(timeout, &left);
  void *record = begin_wait(mask, 0);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
#ifdef SYS_epoll_pwait2
  int result = gw_signal_wait_result(
      gw_signal_wait_syscall(record, SYS_epoll_pwait2, fd, (long)events, count,
                             (long)timeout, (long)&bits, sizeof bits));
#else
  errno = ENOSYS;
  int result = -1;
#endif
  return end_wait(record, result);
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
