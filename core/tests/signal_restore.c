#define _GNU_SOURCE
#include "signal-router.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef __ANDROID__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#endif

static signal_router_backend native;
static int mode, armed, failed, gate_entered;
static volatile sig_atomic_t delivered;

static int backend(int number, const struct sigaction *action,
                   struct sigaction *old) {
  if (armed) {
    int trigger = mode == 0    ? !action && number == SIGUSR1
                  : mode == 1  ? action && number == SIGHUP
                  : mode == 3  ? action && number == SIGRTMIN
                  : mode == 5  ? action && number == SIGBUS
                  : mode == 7  ? action && number == SIGRTMIN + 1
                  : mode == 8  ? action && number == SIGRTMIN + 4
                  : mode == 9  ? !action && number == SIGSEGV
                  : mode >= 10 ? action && number == SIGBUS
                               : action && number == SIGTERM;
    if (!failed && trigger) {
      failed = 1;
      errno = EPERM;
      return -1;
    }
    if (failed && action &&
        ((mode == 4 && number == SIGUSR1) ||
         (mode == 7 && number == SIGRTMIN) ||
         (mode == 8 && number == SIGTRAP) ||
         (mode == 11 && number == SIGSEGV))) {
      errno = EACCES;
      return -1;
    }
  }
  return native(number, action, old);
}

static void receive(int number) {
  assert(number == SIGUSR1);
  ++delivered;
}

static int enter(void *unused) {
  (void)unused;
  ++gate_entered;
  return 0;
}

static void leave(void *unused) {
  (void)unused;
}

#ifndef __ANDROID__
static void reject_writable_mprotect(void) {
  struct sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, args[2])),
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_WRITE, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  struct sock_fprog program = {
      .len = sizeof instructions / sizeof *instructions,
      .filter = instructions};
  assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program));
}
#endif

static void equal_action(const struct sigaction *a, const struct sigaction *b) {
  assert(a->sa_sigaction == b->sa_sigaction && a->sa_flags == b->sa_flags);
  for (int signal = 1; signal < NSIG; ++signal) {
    assert(sigismember(&a->sa_mask, signal) ==
           sigismember(&b->sa_mask, signal));
  }
}

static void enable_wide_routes(const int *notifications) {
  struct sigaction action = {.sa_handler = receive, .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGALRM);
  assert(!sigaction(SIGSEGV, &action, NULL));
  assert(!sigaction(SIGBUS, &action, NULL));
  struct sigaction before_segv, before_bus, actual;
  assert(!native(SIGSEGV, NULL, &before_segv));
  assert(!native(SIGBUS, NULL, &before_bus));

  armed = 1;
  assert(signal_router_defer_faults(notifications) ==
         (mode == 11 ? EIO : EPERM));
  armed = 0;
  if (mode == 11) {
    assert(signal_router_error() == EIO);
    assert(signal_router_verify() == EIO && signal_router_restore() == EIO);
    return;
  }

  assert(!signal_router_error());
  assert(!signal_router_fault_notifications());
  assert(!native(SIGSEGV, NULL, &actual));
  equal_action(&before_segv, &actual);
  assert(!native(SIGBUS, NULL, &actual));
  equal_action(&before_bus, &actual);
  assert(!signal_router_defer_faults(notifications));
  assert(!signal_router_verify());
  assert(!signal_router_restore());
}

static void run_case(int index) {
  mode = index;
  native = (signal_router_backend)dlsym(RTLD_NEXT, "sigaction");
  assert(native && !signal_router_init_with_backend(backend));
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  if (mode >= 9) {
    enable_wide_routes(notifications);
    return;
  }
  if (mode == 7) {
    armed = 1;
    assert(signal_router_defer_faults(notifications) == EIO);
    assert(signal_router_error() == EIO && signal_router_enabled());
    assert(signal_router_verify() == EIO && signal_router_restore() == EIO);
    return;
  }
  assert(!signal_router_defer_faults(notifications));
  struct sigaction action = {.sa_handler = receive, .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGALRM);
  assert(!sigaction(SIGUSR1, &action, NULL));
  assert(!sigaction(SIGHUP, &action, NULL));
  assert(!sigaction(SIGTERM, &action, NULL));
  struct sigaction before[NSIG];
  for (int signal = 1; signal < NSIG; ++signal) {
    if (signal != SIGKILL && signal != SIGSTOP &&
        !(signal >= 32 && signal < SIGRTMIN)) {
      assert(!native(signal, NULL, &before[signal]));
    }
  }

  uint64_t mask, after;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, 8));
  armed = 1;
#ifndef __ANDROID__
  if (mode == 6) {
    reject_writable_mprotect();
  }
#endif
  int result;
  if (mode == 8) {
    assert(sigaction(SIGTRAP, &action, NULL) == -1);
    result = errno;
  } else {
    result = mode == 5 ? signal_router_guest_faults() : signal_router_restore();
  }
  assert(result == (mode == 4 || mode == 8 ? EIO : EPERM));
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &after, 8));
  assert(after == mask && signal_router_enabled());
  armed = 0;

  if (mode == 4 || mode == 5 || mode == 8) {
    assert(signal_router_error() == EIO);
    assert(signal_router_verify() == EIO);
    assert(signal_router_guest_ready() == EIO);
    assert(signal_router_init() == EIO && signal_router_restore() == EIO);
    assert(signal_router_guest_faults() == EIO);
    assert(signal_router_defer_faults(notifications) == EIO);
    assert(sigaction(SIGUSR1, NULL, &action) == -1 && errno == EIO);
    unsigned reclaimed = 0;
    struct signal_router_quiescence gate = {
        .version = GW_SIGNAL_ROUTER_QUIESCENCE_V1,
        .size = sizeof gate,
        .enter = enter,
        .leave = leave};
    assert(signal_router_reclaim(&gate, &reclaimed) == EIO);
    assert(!gate_entered && !reclaimed && signal_router_retained());
    return;
  }

  assert(!signal_router_error() && !signal_router_verify());
  for (int signal = 1; signal < NSIG; ++signal) {
    if (signal != SIGKILL && signal != SIGSTOP &&
        !(signal >= 32 && signal < SIGRTMIN)) {
      struct sigaction actual;
      assert(!native(signal, NULL, &actual));
      equal_action(&actual, &before[signal]);
    }
  }
  assert(!raise(SIGUSR1) && delivered == 1);
  if (mode != 6) {
    assert(!signal_router_restore());
    assert(!signal_router_enabled());
  }
}

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  const int count = 12;
  for (int i = 0; i < count; ++i) {
#ifdef __ANDROID__
    if (i == 6) {
      continue; /* No GNU stdio table writes in Bionic. */
    }
#endif
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      run_case(i);
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      fprintf(stderr, "restore case %d failed: status=%d\n", i, status);
      return 1;
    }
  }
  puts(
      "{\"result\":\"PASS\",\"query_failure\":true,"
      "\"first_write_failure\":true,\"partial_write_rollback\":true,"
      "\"helper_write_rollback\":true,\"rollback_failure_pins\":true,"
      "\"partial_guest_setup_pins\":true,\"native_masks_preserved\":true,"
      "\"recoverable_retry\":true,\"enable_failure_pins\":true,"
      "\"registration_rollback_pins\":true,"
      "\"wide_route_query_rollback\":true,\"wide_route_write_rollback\":true,"
      "\"wide_route_rollback_pins\":true,"
#ifdef __ANDROID__
      "\"native_stdio_no_rewrite\":true}"
#else
      "\"native_stdio_rollback\":true}"
#endif
  );
}
