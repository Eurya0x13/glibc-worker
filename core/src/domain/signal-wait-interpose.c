#define _GNU_SOURCE
#include "signal-router.h"
#include "signals.h"
#include "signal-consume.h"
#include "nonlocal-mask.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef int (*wait_backend)(const sigset_t *, siginfo_t *,
                            const struct timespec *);
static _Atomic(wait_backend) native_wait;

static wait_backend backend(void) {
  wait_backend function =
      atomic_load_explicit(&native_wait, memory_order_acquire);
  if (!function) {
    /* Bionic's ordinary entry delegates through its public 64-bit alias.
     * Resolve the bottom entry to avoid recursion through our LP64 aliases.
     * https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/signal.cpp
     */
    function = (wait_backend)dlsym(RTLD_NEXT,
#ifdef __ANDROID__
                                   "sigtimedwait64"
#else
                                   "sigtimedwait"
#endif
    );
    if (function) {
      atomic_store_explicit(&native_wait, function, memory_order_release);
    }
  }
  return function;
}

int signal_wait_prepare(void) {
  return backend() ? 0 : ENOSYS;
}

static struct timespec remaining(struct timespec deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    _exit(105);
  }
  struct timespec result = {.tv_sec = deadline.tv_sec - now.tv_sec,
                            .tv_nsec = deadline.tv_nsec - now.tv_nsec};
  if (result.tv_nsec < 0) {
    --result.tv_sec;
    result.tv_nsec += 1000000000;
  }
  return result.tv_sec < 0 ? (struct timespec){0} : result;
}

struct wait_arguments {
  wait_backend function;
  const sigset_t *set;
  siginfo_t information;
  const struct timespec *timeout;
};

static long invoke_wait(void *argument) {
  struct wait_arguments *args = argument;
  return args->function(args->set, &args->information, args->timeout);
}

static long decode_wait(void *argument, long result) {
  struct wait_arguments *args = argument;
  if (result < 0) {
    return result;
  }
  int original = gw_signal_consume_notification(result, &args->information);
  return original < 0 ? result : original;
}

int sigtimedwait(const sigset_t *set, siginfo_t *information,
                 const struct timespec *timeout) {
  wait_backend function = backend();
  if (!function) {
    errno = ENOSYS;
    return -1;
  }
  if (!signal_router_fault_notifications() ||
      (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                   timeout->tv_nsec >= 1000000000))) {
    return function(set, information, timeout);
  }
  uint64_t wanted;
  memcpy(&wanted, set, sizeof wanted);
  wanted = signal_host_wait_mask(wanted);
  sigset_t physical;
  sigemptyset(&physical);
  memcpy(&physical, &wanted, sizeof wanted);
  struct timespec deadline = {0}, left;
  if (timeout) {
    if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
      return -1;
    }
    /* Native timeouts near TIME_T_MAX must not overflow an absolute deadline.
     * Such a timeout is effectively unbounded over this wrapper's lifetime.
     */
    if (timeout->tv_sec > INT64_MAX - deadline.tv_sec - 1) {
      deadline = (struct timespec){.tv_sec = INT64_MAX, .tv_nsec = 999999999};
    } else {
      deadline.tv_sec += timeout->tv_sec;
      deadline.tv_nsec += timeout->tv_nsec;
      if (deadline.tv_nsec >= 1000000000) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
      }
    }
  }
  for (;;) {
    if (timeout) {
      left = remaining(deadline);
    }
    struct wait_arguments args = {.function = function,
                                  .set = &physical,
                                  .timeout = timeout ? &left : NULL};
    int result = gw_signal_consume_call(invoke_wait, decode_wait, &args);
    if (result < 0) {
      return result;
    }
    if (result == 0) {
      continue; /* Cancelled level notification; preserve the original deadline.
                 */
    }
    if (information) {
      *information = args.information;
    }
    return result;
  }
}

int sigwaitinfo(const sigset_t *set, siginfo_t *information) {
  return sigtimedwait(set, information, NULL);
}

int sigwait(const sigset_t *set, int *number) {
  int saved_errno = errno, result;
  do {
    result = sigtimedwait(set, NULL, NULL);
  } while (result < 0 && errno == EINTR);
  int error = result < 0 ? errno : 0;
  if (!error) {
    *number = result;
  }
  errno = saved_errno;
  return error;
}

#ifdef __ANDROID__
int sigtimedwait64(const sigset64_t *set, siginfo_t *information,
                   const struct timespec *timeout) {
  sigset_t input;
  if (set) {
    memcpy(&input, set, sizeof input);
  }
  return sigtimedwait(set ? &input : NULL, information, timeout);
}

int sigwaitinfo64(const sigset64_t *set, siginfo_t *information) {
  return sigtimedwait64(set, information, NULL);
}

int sigwait64(const sigset64_t *set, int *number) {
  sigset_t input;
  if (set) {
    memcpy(&input, set, sizeof input);
  }
  return sigwait(set ? &input : NULL, number);
}
#endif
