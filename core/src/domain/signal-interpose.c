#define _GNU_SOURCE
#include "signal-router.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>

/* Opt-in executable interposition for bootstrap-owned integrations. The
 * regular SDK never exports libc names merely by linking the domain target.
 * Late VM integrations route reviewed imports explicitly instead. */
/* Archive retention must use a project-owned symbol: a weak sigaction supplied
 * by a sanitizer runtime otherwise satisfies -u,sigaction without extracting
 * this object. The opt-in CMake target forces this anchor, not a libc name. */
void gw_signal_registration_interpose_anchor(void) {}

int sigaction(int number, const struct sigaction *action,
              struct sigaction *previous) {
  return signal_router_action(number, action, previous);
}

/* glibc remembers siginterrupt() for subsequent BSD signal() registrations.
 * Bionic always selects SA_RESTART and an empty action mask. Keep the owning
 * libc's semantics while ensuring its hidden __sigaction calls cannot replace
 * our kernel entry behind the router.
 * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/posix/signal.c
 * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/posix/sigintr.c
 * https://android.googlesource.com/platform/bionic/+/main/libc/bionic/signal.cpp
 */
#ifdef __GLIBC__
static _Atomic uint64_t interrupted;
#endif

static __sighandler_t register_handler(int number, __sighandler_t handler,
                                       unsigned flags, int bsd) {
  struct sigaction action = {.sa_handler = handler, .sa_flags = flags};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
#ifdef __GLIBC__
  if (number <= 0 || number >= NSIG || number == 32 || number == 33 ||
      (bsd && handler == SIG_ERR)) {
    errno = EINVAL;
    return SIG_ERR;
  }
  if (bsd) {
    sigaddset(&action.sa_mask, number);
    if (atomic_load_explicit(&interrupted, memory_order_relaxed) &
        (UINT64_C(1) << (number - 1))) {
      action.sa_flags &= ~SA_RESTART;
    }
  }
#else
  (void)bsd;
#endif
  return signal_router_action(number, &action, &previous) ? SIG_ERR
                                                          : previous.sa_handler;
}

__sighandler_t signal(int number, __sighandler_t handler) {
  return register_handler(number, handler, SA_RESTART, 1);
}

int siginterrupt(int number, int interrupt) {
  struct sigaction action;
  if (signal_router_action(number, NULL, &action)) {
    return -1;
  }
#ifdef __GLIBC__
  uint64_t bit = UINT64_C(1) << (number - 1);
  if (interrupt) {
    atomic_fetch_or_explicit(&interrupted, bit, memory_order_relaxed);
  } else {
    atomic_fetch_and_explicit(&interrupted, ~bit, memory_order_relaxed);
  }
#endif
  if (interrupt) {
    action.sa_flags &= ~SA_RESTART;
  } else {
    action.sa_flags |= SA_RESTART;
  }
  return signal_router_action(number, &action, NULL);
}

int sigignore(int number) {
  struct sigaction action = {.sa_handler = SIG_IGN};
  sigemptyset(&action.sa_mask);
  return signal_router_action(number, &action, NULL);
}

__sighandler_t sigset(int number, __sighandler_t handler) {
  struct sigaction action = {.sa_handler = handler}, previous;
  sigset_t set, original;
  sigemptyset(&set);
  if (sigaddset(&set, number)) {
    return SIG_ERR;
  }
  sigemptyset(&action.sa_mask);
#ifdef __GLIBC__
  if (handler == SIG_HOLD) {
    /* GNU sigset blocks first. Bionic queries first; failed registrations
     * must retain the owning libc's mask side effects too.
     * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/posix/sigset.c */
    if (sigprocmask(SIG_BLOCK, &set, &original)) {
      return SIG_ERR;
    }
    if (sigismember(&original, number)) {
      return SIG_HOLD;
    }
    return signal_router_action(number, NULL, &previous) ? SIG_ERR
                                                         : previous.sa_handler;
  }
#endif
  if (signal_router_action(number, handler == SIG_HOLD ? NULL : &action,
                           &previous) ||
      sigprocmask(handler == SIG_HOLD ? SIG_BLOCK : SIG_UNBLOCK, &set,
                  &original)) {
    return SIG_ERR;
  }
  return sigismember(&original, number) ? SIG_HOLD : previous.sa_handler;
}

#ifdef __GLIBC__
__sighandler_t bsd_signal(int number, __sighandler_t handler) {
  return register_handler(number, handler, SA_RESTART, 1);
}

__sighandler_t ssignal(int number, __sighandler_t handler) {
  return register_handler(number, handler, SA_RESTART, 1);
}

__sighandler_t sysv_signal(int number, __sighandler_t handler) {
  return register_handler(number, handler,
                          SA_RESETHAND | SA_NODEFER | SA_INTERRUPT, 0);
}

__sighandler_t __sysv_signal(int number, __sighandler_t handler) {
  return sysv_signal(number, handler);
}

int __sigaction(int number, const struct sigaction *action,
                struct sigaction *previous) {
  return signal_router_action(number, action, previous);
}
#endif

#ifdef __ANDROID__
int sigaction64(int number, const struct sigaction64 *action,
                struct sigaction64 *previous) {
  return signal_router_action64(number, action, previous);
}
#endif
