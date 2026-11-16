#define _GNU_SOURCE
#include "signal-router.h"
#include "signals.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

typedef int (*mask_backend)(int, const sigset_t *, sigset_t *);
static _Atomic(mask_backend) native_mask;

static mask_backend backend(void) {
  mask_backend function =
      atomic_load_explicit(&native_mask, memory_order_acquire);
  if (!function) {
#ifdef __ANDROID__
    /* Bionic pthread_sigmask calls the public sigprocmask symbol. Resolving
     * that wrapper would re-enter our interposer indefinitely. LP64's bottom
     * sigprocmask64 is a direct, reserved-signal-filtered kernel operation.
     * https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/sigprocmask.cpp
     */
    function = (mask_backend)dlsym(RTLD_NEXT, "sigprocmask64");
#else
    function = (mask_backend)dlsym(RTLD_NEXT, "pthread_sigmask");
#endif
    if (function) {
      atomic_store_explicit(&native_mask, function, memory_order_release);
    }
  }
  return function;
}

int signal_mask_prepare(void) {
  return backend() ? 0 : ENOSYS;
}

static int apply(mask_backend function, int how, const sigset_t *set,
                 sigset_t *oldset) {
  int result = function(how, set, oldset);
#ifdef __ANDROID__
  return result ? errno : 0;
#else
  return result;
#endif
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  int saved_errno = errno;
  mask_backend function = backend();
  if (!function) {
    errno = saved_errno;
    return ENOSYS;
  }
  if (!signal_router_fault_notifications()) {
    int result = apply(function, how, set, oldset);
    errno = saved_errno;
    return result;
  }
  if (set && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
    errno = saved_errno;
    return EINVAL;
  }
  uint64_t input = 0;
  if (set) {
    memcpy(&input, set, sizeof input);
  }
  sigset_t all, previous;
  sigfillset(&all);
  int result = apply(function, SIG_SETMASK, &all, &previous);
  if (result) {
    errno = saved_errno;
    return result;
  }
  uint64_t physical;
  memcpy(&physical, &previous, sizeof physical);
  uint64_t logical = signal_host_logical_mask(physical), next = logical;
  if (set) {
    next = how == SIG_BLOCK     ? logical | input
           : how == SIG_UNBLOCK ? logical & ~input
                                : input;
  }
  next = signal_host_physical_mask(next);
  sigset_t requested;
  sigemptyset(&requested);
  memcpy(&requested, &next, sizeof next);
  if (oldset) {
    /* Publish before unmasking: a newly delivered handler may leave through
     * siglongjmp and must still observe the completed mask operation's output.
     */
    sigemptyset(oldset);
    memcpy(oldset, &logical, sizeof logical);
  }
  result = apply(function, SIG_SETMASK, &requested, NULL);
  if (result) {
    apply(function, SIG_SETMASK, &previous, NULL);
  }
  errno = saved_errno;
  return result;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  int result = pthread_sigmask(how, set, oldset);
  if (result) {
    errno = result;
    return -1;
  }
  return 0;
}

int sigpending(sigset_t *set) {
  if (!signal_router_fault_notifications()) {
    return syscall(SYS_rt_sigpending, set, sizeof(uint64_t));
  }
  if (signal_router_error()) {
    errno = EIO;
    return -1;
  }

  uint64_t mask = signal_router_selection_begin(), pending;
  int result = syscall(SYS_rt_sigpending, &pending, sizeof pending);
  if (!result) {
    pending = signal_host_pending(pending);
    /* Validate/copy through the kernel before touching caller storage. Only
     * the Linux kernel sigset bytes are output, as in native sigpending.
     * Later arrivals need not appear in our earlier coherent snapshot. */
    result = syscall(SYS_rt_sigpending, set, sizeof pending);
    if (!result) {
      memcpy(set, &pending, sizeof pending);
    }
  }

  int error = errno;
  signal_router_selection_end(mask);
  errno = error;
  return result;
}

#ifdef __ANDROID__
int pthread_sigmask64(int how, const sigset64_t *set, sigset64_t *oldset) {
  _Static_assert(sizeof(sigset64_t) == sizeof(sigset_t),
                 "LP64 signal mask ABI");
  sigset_t input, output;
  if (set) {
    memcpy(&input, set, sizeof input);
  }
  int result =
      pthread_sigmask(how, set ? &input : NULL, oldset ? &output : NULL);
  if (!result && oldset) {
    memcpy(oldset, &output, sizeof output);
  }
  return result;
}

int sigprocmask64(int how, const sigset64_t *set, sigset64_t *oldset) {
  int result = pthread_sigmask64(how, set, oldset);
  if (result) {
    errno = result;
    return -1;
  }
  return 0;
}

int sigpending64(sigset64_t *set) {
  sigset_t output;
  int result = sigpending(&output);
  if (!result) {
    memcpy(set, &output, sizeof output);
  }
  return result;
}
#endif
