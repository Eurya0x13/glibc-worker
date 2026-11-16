#define _GNU_SOURCE
#include "nonlocal_probe.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef __sighandler_t (*registration)(int, __sighandler_t);
static __thread sigjmp_buf checkpoint;
static __thread volatile sig_atomic_t handled;
static __thread int nodefer;
static __thread uintptr_t owner;

static void handler(int number) {
  sigset_t mask;
  if (number != SIGTRAP || (uintptr_t)pthread_self() != owner ||
      errno != EDOM || pthread_sigmask(SIG_SETMASK, NULL, &mask) ||
      sigismember(&mask, number) != !nodefer) {
    _exit(151);
  }
  ++handled;
  siglongjmp(checkpoint, 7);
}

static void *lookup(const char *name) {
  void *entry = dlsym(RTLD_DEFAULT, name);
  if (!entry) {
    _exit(152);
  }
  return entry;
}

static void trigger(void) {
  handled = 0;
  int result = sigsetjmp(checkpoint, 1);
  if (!result) {
    errno = EDOM;
#if defined(__x86_64__)
    __asm__ volatile("int3" ::: "memory");
#else
    __asm__ volatile("brk #0" ::: "memory");
#endif
    _exit(153);
  }
  if (result != 7 || handled != 1) {
    _exit(154);
  }
}

static struct sigaction query(void) {
  struct sigaction action;
  if (sigaction(SIGTRAP, NULL, &action)) {
    _exit(155);
  }
  return action;
}

static void record(const char *name, int faults) {
  dprintf(STDOUT_FILENO,
          "{\"stage\":\"guest-registration\",\"alias\":\"%s\","
          "\"value\":73,\"faults\":%d}\n",
          name, faults);
}

int sdk_guest_registration_probe(void) {
  const char *aliases[] = {"signal", "bsd_signal", "ssignal", "sysv_signal",
                           "__sysv_signal"};
  struct sigaction original = query(), reset = {.sa_handler = SIG_DFL};
  sigemptyset(&reset.sa_mask);
  sigset_t enabled, inherited;
  sigemptyset(&enabled);
  sigaddset(&enabled, SIGTRAP);
  if (pthread_sigmask(SIG_UNBLOCK, &enabled, &inherited)) {
    return -81;
  }
  owner = (uintptr_t)pthread_self();
  for (unsigned i = 0; i < sizeof aliases / sizeof aliases[0]; ++i) {
    registration entry = lookup(aliases[i]);
    nodefer = i >= 3;
    if (sigaction(SIGTRAP, &reset, NULL) ||
        entry(SIGTRAP, handler) != SIG_DFL) {
      return -82;
    }
    struct sigaction installed = query();
    if (!!(installed.sa_flags & SA_NODEFER) != nodefer ||
        !!(installed.sa_flags & SA_RESETHAND) != nodefer ||
        !!(installed.sa_flags & SA_RESTART) != !nodefer) {
      return -83;
    }
    trigger();
    struct sigaction consumed = query();
    if (consumed.sa_handler != (nodefer ? SIG_DFL : handler) ||
        consumed.sa_flags != installed.sa_flags) {
      return -84;
    }
    for (int number = 1; number < NSIG; ++number) {
      if (sigismember(&consumed.sa_mask, number) !=
          sigismember(&installed.sa_mask, number)) {
        return -84;
      }
    }
    record(aliases[i], 1);
  }
  nodefer = 0;
  struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  int (*action_alias)(int, const struct sigaction *, struct sigaction *) =
      lookup("__sigaction");
  if (action_alias(SIGTRAP, &action, NULL) || query().sa_handler != handler) {
    return -85;
  }
  trigger();
  record("__sigaction", 1);
  int (*interrupt)(int, int) = lookup("siginterrupt");
  registration bsd = lookup("signal");
  if (interrupt(SIGTRAP, 1) || bsd(SIGTRAP, handler) != handler ||
      (query().sa_flags & SA_RESTART) || interrupt(SIGTRAP, 0) ||
      !(query().sa_flags & SA_RESTART)) {
    return -86;
  }
  trigger();
  record("siginterrupt", 1);
  int (*ignore)(int) = lookup("sigignore");
  if (ignore(SIGTRAP) || query().sa_handler != SIG_IGN) {
    return -87;
  }
  record("sigignore", 0);
  registration set = lookup("sigset");
  if (set(SIGTRAP, SIG_HOLD) != SIG_IGN || set(SIGTRAP, SIG_HOLD) != SIG_HOLD ||
      set(SIGTRAP, handler) != SIG_HOLD || query().sa_handler != handler) {
    return -88;
  }
  trigger();
  record("sigset", 1);
  if (sigaction(SIGTRAP, &original, NULL) ||
      pthread_sigmask(SIG_SETMASK, &inherited, NULL)) {
    return -89;
  }
  return 73;
}
