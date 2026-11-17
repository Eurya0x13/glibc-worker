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
#include <time.h>

/* Only explicit bootstrap consumers link this object. Inactive-channel calls
 * retain their native libc backend. Active-channel LP64 calls publish
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
static int mask_wait_enabled(void) {
  return signal_router_fault_notifications() != NULL;
}

static int suspend_enabled(const sigset_t *mask) {
  return mask && mask_wait_enabled();
}

static void cancellation_point(void) {
#ifndef __ANDROID__
  pthread_testcancel();
#endif
}

static void *begin_wait(const sigset_t *mask, int filter) {
  cancellation_point();
  uint64_t bits = 0;
  if (mask) {
    memcpy(&bits, mask, sizeof bits);
  }
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
  void *record = mask ? gw_signal_mask_wait_begin(bits)
                      : gw_signal_mask_wait_current_begin();
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

struct retry_budget {
  struct timespec start, duration;
  int timed;
};

static int begin_budget(struct retry_budget *budget,
                        const struct timespec *duration) {
  budget->timed = duration && duration->tv_sec >= 0 && duration->tv_nsec >= 0 &&
                  duration->tv_nsec < 1000000000;
  if (!budget->timed) {
    return 0; /* Infinite waits or invalid inputs left to the native syscall. */
  }

  budget->duration = *duration;
  return clock_gettime(CLOCK_MONOTONIC, &budget->start);
}

static int remaining_budget(const struct retry_budget *budget,
                            struct timespec *remaining) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    return -1;
  }
  if (now.tv_sec < budget->start.tv_sec ||
      (now.tv_sec == budget->start.tv_sec &&
       now.tv_nsec < budget->start.tv_nsec)) {
    errno = EIO;
    return -1;
  }

  struct timespec elapsed = {now.tv_sec - budget->start.tv_sec,
                             now.tv_nsec - budget->start.tv_nsec};
  if (elapsed.tv_nsec < 0) {
    --elapsed.tv_sec;
    elapsed.tv_nsec += 1000000000;
  }
  if (elapsed.tv_sec > budget->duration.tv_sec ||
      (elapsed.tv_sec == budget->duration.tv_sec &&
       elapsed.tv_nsec >= budget->duration.tv_nsec)) {
    *remaining = (struct timespec){0};
    return 0;
  }

  *remaining = (struct timespec){budget->duration.tv_sec - elapsed.tv_sec,
                                 budget->duration.tv_nsec - elapsed.tv_nsec};
  if (remaining->tv_nsec < 0) {
    --remaining->tv_sec;
    remaining->tv_nsec += 1000000000;
  }
  return 0;
}

static int transparent_wait(void *record, long number, long arguments[6],
                            const struct timespec *duration, int timeout_index,
                            int milliseconds, struct timespec *remaining) {
  int saved_errno = errno;
  struct retry_budget budget;
  if (begin_budget(&budget, duration)) {
    return end_wait(record, -1);
  }

  struct timespec left;
  int result;
  for (;;) {
    result = gw_signal_wait_result(gw_signal_wait_syscall(
        record, number, arguments[0], arguments[1], arguments[2], arguments[3],
        arguments[4], arguments[5]));
    if (!gw_signal_mask_wait_retry(record, result, errno)) {
      break;
    }

    /* Linux updates some relative syscall timeouts before entering a handler;
     * that remainder excludes time spent in the handler. Recompute from one
     * monotonic budget so hidden work never resets/extends the Host wait.
     * Subtract elapsed time instead of adding an overflowing absolute time.
     * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
     * See poll_select_finish() and poll_select_set_timeout(). */
    if (budget.timed) {
      if (remaining_budget(&budget, &left)) {
        result = -1;
        break;
      }
      arguments[timeout_index] =
          milliseconds ? left.tv_sec * 1000 + (left.tv_nsec + 999999) / 1000000
                       : (long)&left;
    }
    extern void gw_signal_mask_wait_retry_observe(long, const struct timespec *)
        __attribute__((weak));
    if (gw_signal_mask_wait_retry_observe) {
      gw_signal_mask_wait_retry_observe(number, budget.timed ? &left : NULL);
    }
  }

  if (remaining) {
    *remaining = *(struct timespec *)arguments[timeout_index];
  }
  if (result >= 0) {
    errno = saved_errno;
  }
  return end_wait(record, result);
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
  if (!suspend_enabled(mask)) {
    return function(mask);
  }

  void *record = begin_wait(mask, 1);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
  long arguments[] = {(long)&bits, sizeof bits, 0, 0, 0, 0};
  return transparent_wait(record, SYS_rt_sigsuspend, arguments, NULL, -1, 0,
                          NULL);
}

int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
          const sigset_t *mask) {
  typedef int (*function_type)(struct pollfd *, nfds_t, const struct timespec *,
                               const sigset_t *);
  function_type function = (function_type)resolve(&caches[1], NATIVE(ppoll));
  if (!function) {
    return -1;
  }
  if (!mask_wait_enabled()) {
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
  long arguments[] = {(long)fds,   count,       (long)timeout,
                      (long)&bits, sizeof bits, 0};
  return transparent_wait(record, SYS_ppoll, arguments, timeout, 2, 0, NULL);
}

int pselect(int count, fd_set *read, fd_set *write, fd_set *except,
            const struct timespec *timeout, const sigset_t *mask) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               const struct timespec *, const sigset_t *);
  function_type function = (function_type)resolve(&caches[2], NATIVE(pselect));
  if (!function) {
    return -1;
  }
  if (!mask_wait_enabled()) {
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

  long arguments[] = {count,        (long)read,    (long)write,
                      (long)except, (long)timeout, (long)&data};
  return transparent_wait(record, SYS_pselect6, arguments, timeout, 4, 0, NULL);
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
  if (!mask_wait_enabled()) {
    signal_host_prune_notifications();
    return function(fd, events, count, timeout, mask);
  }

  void *record = begin_wait(mask, 0);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);
  struct timespec duration = {timeout / 1000, (timeout % 1000) * 1000000L};
  long arguments[] = {fd,      (long)events, count,
                      timeout, (long)&bits,  sizeof bits};
  return transparent_wait(record, SYS_epoll_pwait, arguments,
                          timeout < 0 ? NULL : &duration, 3, 1, NULL);
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
  if (!mask_wait_enabled()) {
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
  long arguments[] = {fd,          (long)events, count, (long)timeout,
                      (long)&bits, sizeof bits};
  return transparent_wait(record, SYS_epoll_pwait2, arguments, timeout, 3, 0,
                          NULL);
#else
  errno = ENOSYS;
  return end_wait(record, -1);
#endif
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
  typedef int (*function_type)(struct pollfd *, nfds_t, int);
  function_type function = (function_type)resolve(&caches[5], "poll");
  if (!function) {
    return -1;
  }
  if (mask_wait_enabled()) {
    struct timespec duration = {timeout / 1000, (timeout % 1000) * 1000000L};
    return ppoll(fds, count, timeout < 0 ? NULL : &duration, NULL);
  }
  signal_host_prune_notifications();
  return function(fds, count, timeout);
}

/* Match the LP64 carrier's conversion before entering its underlying pselect6
 * syscall. GNU select normalizes an int32_t microsecond field and saturates
 * overflowing seconds; Bionic rejects an out-of-range microsecond field.
 * Invalid inputs delegate to libc, retaining its conversion/error side effects.
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/select.c
 * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/bionic/poll.cpp
 * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/bionic/bionic_time_conversions.cpp
 */
static int select_duration(const struct timeval *timeout,
                           struct timespec *duration) {
  if (!timeout) {
    return 1;
  }

#ifdef __GLIBC__
  int32_t microseconds = timeout->tv_usec;
  if (timeout->tv_sec < 0 || microseconds < 0) {
    return 0;
  }
  time_t extra = microseconds / 1000000;
  if (extra > INT64_MAX - timeout->tv_sec) {
    *duration = (struct timespec){INT64_MAX, 999999999};
  } else {
    *duration = (struct timespec){timeout->tv_sec + extra,
                                  (microseconds % 1000000) * 1000L};
  }
#else
  if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
      timeout->tv_usec >= 1000000) {
    return 0;
  }
  *duration = (struct timespec){timeout->tv_sec, timeout->tv_usec * 1000L};
#endif
  return 1;
}

int select(int count, fd_set *read, fd_set *write, fd_set *except,
           struct timeval *timeout) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               struct timeval *);
  function_type function = (function_type)resolve(&caches[6], "select");
  if (!function) {
    return -1;
  }
  struct timespec duration = {0};
  if (!mask_wait_enabled() || !select_duration(timeout, &duration)) {
    signal_host_prune_notifications();
    return function(count, read, write, except, timeout);
  }

  void *record = begin_wait(NULL, 1);
  if (!record) {
    return -1;
  }
  uint64_t bits = gw_signal_mask_wait_execution(record);

  struct {
    const uint64_t *mask;
    size_t size;
  } data = {&bits, sizeof bits};

  long arguments[] = {count,
                      (long)read,
                      (long)write,
                      (long)except,
                      timeout ? (long)&duration : 0,
                      (long)&data};
  struct timespec remaining = duration;
  int result = transparent_wait(record, SYS_pselect6, arguments,
                                timeout ? &duration : NULL, 4, 0,
                                timeout ? &remaining : NULL);
  if (timeout) {
    timeout->tv_sec = remaining.tv_sec;
    timeout->tv_usec = remaining.tv_nsec / 1000;
  }
  return result;
}

int epoll_wait(int fd, struct epoll_event *events, int count, int timeout) {
  typedef int (*function_type)(int, struct epoll_event *, int, int);
  function_type function = (function_type)resolve(&caches[7], "epoll_wait");
  if (!function) {
    return -1;
  }
  if (mask_wait_enabled()) {
    return epoll_pwait(fd, events, count, timeout, NULL);
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
