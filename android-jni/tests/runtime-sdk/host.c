#define _GNU_SOURCE
#include "runtime_bridge.h"
#include "signal-router.h"
#include "signal_channel.h"
#include "signal-relay.h"
#ifdef GW_TEST_NATIVE_UNWIND
#include "nonlocal.h"
#endif
#ifdef GW_TEST_LATE_SIGNALS
#include "signal_profile.h"
#endif
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include "nonlocal_probe.h"
#include "internal/domain_unwind.h"
#ifdef __ANDROID__
#include "vm-start.h"
#endif

struct application {
  JavaVM *vm;
  int worker;
  int nonlocal;
  int router_quarantine;
  const struct gw_loader_api_v1 *loader;
  uint64_t module;
};

static _Thread_local sigjmp_buf escape;
static _Thread_local volatile sig_atomic_t escaped;
static _Thread_local jint (*native_entry)(JNIEnv *, jclass, jint);
static _Thread_local int transfers;
static _Thread_local JNIEnv *route_env;
static _Thread_local jclass route_class;
static _Thread_local int route_depth, route_level;
#ifdef GW_TEST_NATIVE_UNWIND
static _Thread_local sigjmp_buf consume_target;
static _Thread_local volatile sig_atomic_t consume_inject, consume_jumps;

static void consume_escape(int number) {
  assert(number == SIGUSR1);
  ++consume_jumps;
  siglongjmp(consume_target, 29);
}

void gw_signal_consume_test_point(void) {
  if (consume_inject) {
    consume_inject = 0;
    assert(!raise(SIGUSR1));
  }
}
#endif
static _Thread_local uintptr_t alternate_low, alternate_high;
static _Thread_local volatile sig_atomic_t expect_alternate, alternate_hits;
static _Thread_local volatile sig_atomic_t inject_point;
static _Thread_local int window_point;
static _Thread_local volatile sig_atomic_t check_host_signal_mask;
static _Thread_local volatile sig_atomic_t host_signal_mask_error;
static _Thread_local volatile sig_atomic_t signal_returns;
static _Thread_local volatile sig_atomic_t returned_handler;
static _Thread_local volatile sig_atomic_t expect_nodefer;
static _Thread_local volatile sig_atomic_t expect_host_usr1_blocked;
static _Thread_local volatile sig_atomic_t queued_count;
static _Thread_local volatile sig_atomic_t queued_values[3];
static _Thread_local volatile sig_atomic_t queued_codes[3];
static _Thread_local volatile sig_atomic_t queued_pids[3];
static _Thread_local volatile sig_atomic_t queued_uids[3];
static _Thread_local volatile sig_atomic_t host_traps;
static _Thread_local volatile sig_atomic_t stack_signal_hits;
static _Thread_local sigjmp_buf stack_escape;
static _Thread_local volatile sig_atomic_t stack_jumps, stack_expect_disabled;
static _Thread_local volatile sig_atomic_t numbered_signals[2];
static _Thread_local volatile sig_atomic_t wait_interrupts;
static _Thread_local volatile sig_atomic_t wait_escape_active;
static _Thread_local sigjmp_buf wait_escape;
static _Thread_local volatile sig_atomic_t deferred_faults;
static _Thread_local volatile sig_atomic_t async_fault_active, async_fault_hits;
static _Thread_local volatile sig_atomic_t unreserved_signal_hits;
static struct sigaction original_segv;
static _Thread_local volatile int relay_canary = 0x7193;
static _Atomic unsigned relay_deliveries;
static _Atomic long relay_tid;

#ifdef GW_TEST_LATE_SIGNALS
/* This explicitly participating consumer starts with native registrations.
 * A late profile patches reviewed VM/libc imports, not every application's
 * native call site. Route our own registrations only after installation. */
static signal_router_backend consumer_action = sigaction;
#define sigaction(...) consumer_action(__VA_ARGS__)
#endif

#ifdef __ANDROID__
static _Atomic unsigned private_calls;

static int private_host_callback(void *argument) {
  assert(argument == &private_calls && relay_canary == 0x7193);
  atomic_fetch_add_explicit(&private_calls, 1, memory_order_relaxed);
  return 1;
}

static void private_host_registration(void) {
  int (*run_on_all)(int (*)(void *), void *) =
      dlsym(RTLD_NEXT, "android_run_on_all_threads");
  assert(run_on_all && !signal_router_verify());
  assert(run_on_all(private_host_callback, &private_calls));
  assert(atomic_load(&private_calls) > 1 && !signal_router_verify());
  puts(
      "{\"stage\":\"bionic-private-registration\",\"result\":\"PASS\","
      "\"vm_running\":true,\"host_tls\":true}");
}
#endif

static void host_relay(int number, siginfo_t *information, void *frame) {
  assert(number == SIGURG || number == SIGTRAP);
  assert(frame);
  assert(relay_canary == 0x7193);
  assert(information->si_code == SI_QUEUE && information->si_pid == getpid() &&
         information->si_uid == getuid());

  long tid = syscall(SYS_gettid);
  assert(tid != information->si_value.sival_int);
  long previous = atomic_load(&relay_tid);
  assert(!previous || previous == tid);
  atomic_store(&relay_tid, tid);

  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, number) && sigismember(&mask, SIGWINCH));
  sigaddset(&((ucontext_t *)frame)->uc_sigmask, SIGWINCH);

  atomic_fetch_add(&relay_deliveries, 1);
  errno = ERANGE;
}

static void host_gnu_thread_relay(JNIEnv *env, jclass cls) {
  struct sigaction action = {.sa_sigaction = host_relay,
                             .sa_flags = SA_SIGINFO | SA_RESTART},
                   previous, trap_previous;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGWINCH);
  assert(!sigaction(SIGURG, &action, &previous));
  assert(!sigaction(SIGTRAP, &action, &trap_previous));

  assert(native_entry(env, cls, 88) == 73);
  assert(atomic_load(&relay_deliveries) == 64);

  assert(!sigaction(SIGURG, &previous, NULL));
  assert(!sigaction(SIGTRAP, &trap_previous, NULL));

  puts(
      "{\"stage\":\"gnu-host-relay\",\"threads\":8,\"deliveries\":64,"
      "\"host_tls\":true,\"guest_tls_errno\":true,\"original_siginfo\":true,"
      "\"different_thread\":true,\"source_context_edits\":true}");
}

static void asynchronous_fault(int number, siginfo_t *information,
                               void *frame) {
  if (async_fault_active && information->si_code == SI_QUEUE &&
      information->si_value.sival_int == 0x7193) {
    sigset_t mask;
    if (number != SIGSEGV || information->si_pid != getpid() ||
        information->si_uid != getuid() ||
        sigismember(&((ucontext_t *)frame)->uc_sigmask, SIGSEGV) ||
        pthread_sigmask(SIG_SETMASK, NULL, &mask) ||
        sigismember(&mask, SIGSEGV) != 1) {
      _exit(173);
    }
    ++async_fault_hits;
    return;
  }
  /* Installed before VM creation: HotSpot owns the outer action and chains
   * only faults it does not recognize. Other packets retain the pre-VM action.
   * https://github.com/openjdk/jdk17u/blob/master/src/hotspot/os/posix/signals_posix.cpp
   * See set_signal_handler(), call_chained_handler() and chained_handler(). */
  if (original_segv.sa_handler == SIG_IGN) {
    return;
  }
  if (original_segv.sa_handler == SIG_DFL) {
    signal_router_default(number);
  }
  if (original_segv.sa_flags & SA_SIGINFO) {
    original_segv.sa_sigaction(number, information, frame);
  } else {
    original_segv.sa_handler(number);
  }
}

static void install_asynchronous_fault(void) {
  struct sigaction action = {.sa_sigaction = asynchronous_fault,
                             .sa_flags = SA_SIGINFO | SA_RESTART};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGSEGV);
  assert(!sigaction(SIGSEGV, &action, &original_segv));
}

static void host_unblocked_fault(JNIEnv *env, jclass cls) {
  struct sigaction expected, observed;
  assert(!sigaction(SIGSEGV, NULL, &expected));
  sigset_t set, inherited;
  sigemptyset(&set);
  sigaddset(&set, SIGSEGV);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &inherited));
  async_fault_active = 1;
  assert(native_entry(env, cls, 85) == 73);
  async_fault_active = 0;
  assert(async_fault_hits == 1);
  assert(!sigaction(SIGSEGV, NULL, &observed));
  assert(observed.sa_sigaction == expected.sa_sigaction &&
         observed.sa_flags == expected.sa_flags);
  for (int number = 1; number < NSIG; ++number) {
    assert(sigismember(&observed.sa_mask, number) ==
           sigismember(&expected.sa_mask, number));
  }
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  puts(
      "{\"stage\":\"host-async-fault\",\"signal\":11,\"delivered\":true,"
      "\"metadata\":true}");
}

static void deferred_fault(int number, siginfo_t *information, void *frame) {
  sigset_t mask;
  if (number != SIGTRAP || information->si_code != SI_QUEUE ||
      information->si_pid != getpid() || information->si_uid != getuid() ||
      information->si_value.sival_int != 0x7193 ||
      sigismember(&((ucontext_t *)frame)->uc_sigmask, SIGTRAP) ||
      pthread_sigmask(SIG_SETMASK, NULL, &mask) ||
      sigismember(&mask, SIGTRAP) != 1) {
    _exit(172);
  }
  ++deferred_faults;
}

static void unreserved_signal(int number, siginfo_t *information, void *frame) {
  (void)frame;
  sigset_t mask;
  if (number != SIGUSR1 || information->si_code != SI_QUEUE ||
      information->si_pid != getpid() || information->si_uid != getuid() ||
      information->si_value.sival_int != 0x7193 ||
      pthread_sigmask(SIG_SETMASK, NULL, &mask) ||
      sigismember(&mask, SIGUSR1) != 1) {
    _exit(174);
  }
  ++unreserved_signal_hits;
}

static void host_unreserved_signal(JNIEnv *env, jclass cls) {
  uint64_t reservations = signal_router_reserved();
  assert(!(reservations & (UINT64_C(1) << (SIGUSR1 - 1))));
  struct sigaction original;
  assert(!sigaction(SIGUSR1, NULL, &original));
  assert(original.sa_handler == SIG_DFL || original.sa_handler == SIG_IGN);
  struct sigaction action = {.sa_sigaction = unreserved_signal,
                             .sa_flags = SA_SIGINFO | SA_RESTART};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, NULL));
  sigset_t signal, inherited;
  sigemptyset(&signal);
  sigaddset(&signal, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &signal, &inherited));
  assert(native_entry(env, cls, 86) == 73);
  assert(unreserved_signal_hits == 1);
  assert(signal_router_reserved() == reservations);
  assert(native_entry(env, cls, 0) == 73);
  assert(!sigaction(SIGUSR1, &original, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  puts(
      "{\"stage\":\"host-unreserved-signal\",\"signal\":10,\"host_tls\":true,"
      "\"metadata\":true,\"guest_return\":true,\"exclusive\":false}");
}

static void wait_interrupt(int number) {
  if (number != 53) {
    _exit(171);
  }
  ++wait_interrupts;
  if (wait_escape_active) {
    wait_escape_active = 0;
    siglongjmp(wait_escape, 29);
  }
}

static void numbered_signal(int number) {
  if (number != 50 && number != 51) {
    _exit(159);
  }
  ++numbered_signals[number - 50];
}

static void host_trap(int number) {
  if (number == SIGTRAP) {
    ++host_traps;
  }
}

void gw_domain_nonlocal_test_point(int point) {
  if (point == GW_UNWIND_RPC_READY && route_depth) {
    ++route_level;
    assert(route_level <= route_depth);
    native_entry(route_env, route_class, route_level == route_depth ? 92 : 93);
    assert(!"C callback returned after Guest-target transfer");
  }
  if (inject_point == point) {
    inject_point = 0;
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                    SDK_HOST_ESCAPE_SIGNAL));
  }
}

static void escape_handler(int number) {
  if (number == SDK_HOST_ESCAPE_SIGNAL) {
    volatile char stack_byte;
    uintptr_t sp = (uintptr_t)&stack_byte;
    int on_alternate = sp >= alternate_low && sp < alternate_high;
    if (on_alternate != expect_alternate) {
      _exit(124);
    }
    alternate_hits += on_alternate;
    if (check_host_signal_mask) {
      sigset_t current;
      if (sigprocmask(SIG_SETMASK, NULL, &current) ||
          sigismember(&current, SIGUSR1) != expect_host_usr1_blocked ||
          !sigismember(&current, SIGWINCH) ||
          sigismember(&current, SDK_HOST_ESCAPE_SIGNAL) == expect_nodefer) {
        host_signal_mask_error = 1;
      }
    }
    if (signal_returns) {
      returned_handler = 1;
      return;
    }
    /* A local handler jump must preserve the surrounding bridge boundaries. */
    sigjmp_buf local;
    if (sigsetjmp(local, 1) == 0) {
      siglongjmp(local, 1);
    }
    escaped = 1;
    siglongjmp(escape, 37);
  }
}

static void queued_handler(int number, siginfo_t *info, void *context) {
  (void)context;
  int index = queued_count;
  if (index >= 3) {
    _exit(124);
  }
  queued_values[index] = info->si_value.sival_int;
  queued_codes[index] = info->si_code;
  queued_pids[index] = info->si_pid;
  queued_uids[index] = info->si_uid;
  queued_count = index + 1;
  escape_handler(number);
}

static jint recover_once(JNIEnv *env, jclass cls, int save_mask) {
  escaped = 0;
  int transferred = sigsetjmp(escape, save_mask);
  if (!transferred) {
    (void)native_entry(env, cls, 1);
    assert(!"Guest returned after the requested nonlocal jump");
  }
  assert(transferred == 37 && escaped);
  stack_t stack;
  assert(!sigaltstack(NULL, &stack));
  assert(!(stack.ss_flags & SS_ONSTACK));
  ++transfers;
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SDK_HOST_ESCAPE_SIGNAL) == !save_mask);
  sigdelset(&mask, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_SETMASK, &mask, NULL));
  assert(native_entry(env, cls, 0) == 73);
  assert(!(*env)->ExceptionCheck(env));
  return 73;
}

static jint recover_window(JNIEnv *env, jclass cls, int point) {
  int before = native_entry(env, cls, 3);
  escaped = 0;
  int transferred = sigsetjmp(escape, 1);
  if (!transferred) {
    inject_point = point;
    (void)native_entry(env, cls, 0);
    assert(!"Window signal did not abandon the native invocation");
  }
  assert(transferred == 37 && escaped && !inject_point);
  int after = native_entry(env, cls, 3);
  int executed = point >= GW_UNWIND_COMMAND_FINISHED;
  assert(after == before + executed);
  ++transfers;
  assert(native_entry(env, cls, 0) == 73);
  assert(!(*env)->ExceptionCheck(env));
  return 73;
}

static jint host_nested(JNIEnv *env, jclass cls) {
  /* This checkpoint is inside the Host callback. Java frames outside this
   * callback survive; the test does not jump over them. */
  return window_point ? recover_window(env, cls, window_point)
                      : recover_once(env, cls, 1);
}

static jint host_mask(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  sigset_t current;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &current));
  int blocked = sigismember(&current, SIGUSR1);
  int guest_blocked = native_entry(env, cls, 8);
  int guest_change = native_entry(env, cls, 9);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &current));
  return blocked == 0 && guest_blocked == 1 && !guest_change &&
                 !sigismember(&current, SIGUSR1)
             ? 0
             : -1;
}

static jint host_mask_blocked(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  sigset_t current;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &current));
  return sigismember(&current, SIGUSR1);
}

static void recover_floating_point(JNIEnv *env, jclass cls) {
  int previous = fegetround();
  assert(previous != -1 && !fesetround(FE_UPWARD));
  escaped = 0;
  if (!sigsetjmp(escape, 1)) {
    (void)native_entry(env, cls, 4);
    assert(!"Guest floating-point probe returned instead of jumping");
  }
  assert(escaped && fegetround() == FE_UPWARD);
  assert(native_entry(env, cls, 5) == 1);
  assert(fegetround() == FE_UPWARD);
  assert(!native_entry(env, cls, 6));
  assert(!fesetround(previous));
  ++transfers;
}

static void recover_guest_signal_mask(JNIEnv *env, jclass cls) {
  escaped = 0;
  host_signal_mask_error = 0;
  check_host_signal_mask = 1;
  if (!sigsetjmp(escape, 1)) {
    (void)native_entry(env, cls, 11);
    assert(!"Guest signal probe returned instead of jumping");
  }
  check_host_signal_mask = 0;
  int guest_mask = native_entry(env, cls, 8);
  if (host_signal_mask_error || guest_mask != 1) {
    fprintf(stderr, "SIGNAL_MASK_RECOVERY host_error=%d guest=%d\n",
            host_signal_mask_error, guest_mask);
  }
  assert(escaped && !host_signal_mask_error && guest_mask == 1);
  assert(!native_entry(env, cls, 9));
  ++transfers;
}

static void return_guest_signal_mask(JNIEnv *env, jclass cls, int mode) {
  returned_handler = 0;
  host_signal_mask_error = 0;
  check_host_signal_mask = 1;
  signal_returns = 1;
  assert(native_entry(env, cls, mode) == 73);
  signal_returns = 0;
  check_host_signal_mask = 0;
  assert(returned_handler && !host_signal_mask_error);
}

static void pending_host_signal(JNIEnv *env, jclass cls) {
  sigset_t signal, previous, pending;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_BLOCK, &signal, &previous));
  signal_returns = 1;
  returned_handler = 0;
  assert(native_entry(env, cls, 15) == 73);
  assert(!sigpending(&pending));
  int delivered_early = returned_handler;
  int is_pending = sigismember(&pending, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  int delivered_after_unblock = returned_handler;
  signal_returns = 0;
  if (delivered_early || !is_pending || !delivered_after_unblock) {
    fprintf(stderr,
            "PENDING_HOST_SIGNAL early=%d pending=%d after_unblock=%d\n",
            delivered_early, is_pending, delivered_after_unblock);
    _exit(2);
  }
}

static void pending_queued_signals(JNIEnv *env, jclass cls) {
  struct sigaction action = {.sa_sigaction = queued_handler,
                             .sa_flags = SA_SIGINFO},
                   previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, &previous));
  sigset_t signal, inherited, pending;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_BLOCK, &signal, &inherited));
  signal_returns = 1;
  queued_count = 0;
  assert(native_entry(env, cls, 21) == 73);
  assert(!sigpending(&pending) && !queued_count);
  assert(sigismember(&pending, SDK_HOST_ESCAPE_SIGNAL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(queued_count == 3);
  for (int i = 0; i < 3; ++i) {
    assert(queued_values[i] == 101 + i && queued_codes[i] == SI_QUEUE);
    assert(queued_pids[i] == getpid() && queued_uids[i] == (int)getuid());
  }
  action.sa_flags |= SA_RESETHAND;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  assert(!pthread_sigmask(SIG_BLOCK, &signal, &inherited));
  queued_count = 0;
  assert(native_entry(env, cls, 15) == 73);
  struct sigaction observed;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, NULL, &observed));
  assert(observed.sa_sigaction == queued_handler && !queued_count);
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(queued_count == 1);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, NULL, &observed));
  assert(observed.sa_handler == SIG_DFL);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &previous, NULL));
  signal_returns = 0;
}

static int guest_waits(JNIEnv *env, jclass cls) {
  struct sigaction action = {.sa_sigaction = queued_handler,
                             .sa_flags = SA_SIGINFO},
                   previous, interrupt = {.sa_handler = wait_interrupt},
                   interrupt_previous;
  sigemptyset(&action.sa_mask);
  sigemptyset(&interrupt.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, &previous));
  assert(!sigaction(53, &interrupt, &interrupt_previous));
  sigset_t signal, inherited, pending;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_BLOCK, &signal, &inherited));
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, 53);
  assert(!pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL));
  signal_returns = 1;
  int skipped = 0;
  for (int mode = 60; mode <= 78; ++mode) {
    queued_count = 0;
    wait_interrupts = 0;
    for (int value = 101; value <= 103; ++value) {
      siginfo_t information = {.si_signo = SDK_HOST_ESCAPE_SIGNAL,
                               .si_code = SI_QUEUE};
      information.si_pid = getpid();
      information.si_uid = getuid();
      information.si_value.sival_int = value;
      assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                      SDK_HOST_ESCAPE_SIGNAL, &information));
    }
    int observed = native_entry(env, cls, mode);
    printf(
        "{\"stage\":\"guest-wait\",\"mode\":%d,\"value\":%d,"
        "\"interrupts\":%d,\"skip\":%s}\n",
        mode, observed, wait_interrupts,
        observed == 74 ? "\"ENOSYS\"" : "null");
    fflush(stdout);
    if (observed == 74 && (mode == 67 || mode == 72)) {
      ++skipped;
    } else {
      assert(observed == 73);
      if (mode >= 68 && mode <= 72) {
        assert(wait_interrupts);
      }
    }
    assert(!(*env)->ExceptionCheck(env));
    assert(!queued_count && !sigpending(&pending));
    assert(sigismember(&pending, SDK_HOST_ESCAPE_SIGNAL));
    assert(!pthread_sigmask(SIG_UNBLOCK, &signal, NULL));
    assert(queued_count == 3);
    for (int i = 0; i < 3; ++i) {
      assert(queued_values[i] == 101 + i && queued_codes[i] == SI_QUEUE);
      assert(queued_pids[i] == getpid() && queued_uids[i] == (int)getuid());
    }
    assert(!pthread_sigmask(SIG_BLOCK, &signal, NULL));
    assert(native_entry(env, cls, 0) == 73);
  }
  for (int save = 0; save <= 1; ++save) {
    wait_escape_active = 1;
    int transferred = sigsetjmp(wait_escape, save);
    if (!transferred) {
      native_entry(env, cls, 79);
      assert(!"Guest wait returned without transferring to Host");
    }
    assert(transferred == 29 && !wait_escape_active);
    sigset_t observed;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
    assert(sigismember(&observed, 53) == !save);
    assert(!pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL));
    assert(native_entry(env, cls, 80) == 73);
    assert(native_entry(env, cls, 0) == 73);
    assert(!(*env)->ExceptionCheck(env));
    printf(
        "{\"stage\":\"wait-unwind\",\"saved_mask\":%d,"
        "\"kernel_wait\":true,\"subsequent_call\":true}\n",
        save);
    fflush(stdout);
  }
  assert(!sigaction(53, &interrupt_previous, NULL));
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  signal_returns = 0;
  return skipped;
}

static jint host_stack(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  stack_t stack;
  return !sigaltstack(NULL, &stack) && !(stack.ss_flags & SS_DISABLE) &&
                 (uintptr_t)stack.ss_sp == alternate_low &&
                 (uintptr_t)stack.ss_sp + stack.ss_size == alternate_high
             ? 73
             : -1;
}

static void stack_host_signal(int number, siginfo_t *information, void *frame) {
  char local;
  uintptr_t sp = (uintptr_t)&local;
  int on_alternate = sp >= alternate_low && sp < alternate_high;
  stack_t stack;
  if (number != SDK_HOST_ESCAPE_SIGNAL || !information || !frame ||
      on_alternate != expect_alternate || sigaltstack(NULL, &stack) ||
      (stack_expect_disabled
           ? !(stack.ss_flags & SS_DISABLE)
           : !!(stack.ss_flags & SS_ONSTACK) != expect_alternate ||
                 (uintptr_t)stack.ss_sp != alternate_low ||
                 (uintptr_t)stack.ss_sp + stack.ss_size != alternate_high)) {
    _exit(157);
  }
  ++stack_signal_hits;
  if (stack_jumps) {
    siglongjmp(stack_escape, 1);
  }
}

static void recover_stack_unwind(JNIEnv *env, jclass cls, int mode,
                                 int autodisarm, int alternate) {
  stack_t previous, configured, observed;
  assert(!sigaltstack(NULL, &previous));
  configured = previous;
  configured.ss_flags = autodisarm ? (UINT32_C(1) << 31) : 0;
  assert(!sigaltstack(&configured, NULL));
  expect_alternate = alternate;
  stack_expect_disabled = autodisarm && alternate;
  struct sigaction action = {
      .sa_sigaction = stack_host_signal,
      .sa_flags = SA_SIGINFO | (alternate ? SA_ONSTACK : 0)};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  stack_jumps = 1;
  if (!sigsetjmp(stack_escape, 1)) {
    native_entry(env, cls, mode);
    assert(0 && "Host signal failed to abandon the Guest handler");
  }
  stack_jumps = 0;
  assert(!sigaltstack(NULL, &observed));
  assert(!!(observed.ss_flags & SS_DISABLE) == stack_expect_disabled);
  assert(native_entry(env, cls, 51) == 73);
  assert(!(*env)->ExceptionCheck(env));
  assert(!sigaltstack(NULL, &observed));
  assert(!!(observed.ss_flags & SS_DISABLE) == stack_expect_disabled);
  assert(!sigaltstack(&previous, NULL));
  stack_expect_disabled = 0;
}

static void host_numbered_signals(JNIEnv *env, jclass cls) {
  for (int number = 50; number <= 51; ++number) {
    struct sigaction action = {.sa_handler = numbered_signal}, previous;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(number, &action, &previous));
    sigset_t set, inherited, pending;
    sigemptyset(&set);
    sigaddset(&set, number);
    assert(!pthread_sigmask(SIG_BLOCK, &set, &inherited));
    assert(native_entry(env, cls, number + 3) == 73);
    assert(!numbered_signals[number - 50]);
    assert(!sigpending(&pending) && sigismember(&pending, number));
    assert(!pthread_sigmask(SIG_UNBLOCK, &set, NULL));
    assert(numbered_signals[number - 50] == 1);
    assert(!sigaction(number, &previous, NULL));
    assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  }
}

static void host_descriptor_unwind(JNIEnv *env, jclass cls, int descriptor) {
#ifdef GW_TEST_NATIVE_UNWIND
  extern int gw_domain_release_context(void);
  struct sigaction action = {.sa_handler = consume_escape}, previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, &previous));
  for (int vector = 0; vector < 2; ++vector) {
    assert(native_entry(env, cls, 84) == 73);
    int result = sigsetjmp(consume_target, 1);
    if (!result) {
      consume_inject = 1;
      struct signalfd_siginfo information;
      if (vector) {
        struct iovec part = {&information, sizeof information};
        readv(descriptor, &part, 1);
      } else {
        read(descriptor, &information, sizeof information);
      }
      assert(!"descriptor read returned after a native jump");
    }
    assert(result == 29 && !gw_nonlocal_pending());
    /* The handler invokes no JNI. Retire the read on the normal Host stack
     * before the next VM entry and context release. */
    assert(full_host_state() == GW_HOST_READY && !(*env)->ExceptionCheck(env));
    assert(!gw_domain_release_context());
    assert(native_entry(env, cls, 0) == 73);
    assert(native_entry(env, cls, 84) == 73);
    struct signalfd_siginfo information;
    assert(read(descriptor, &information, sizeof information) ==
           sizeof information);
    assert(information.ssi_signo == SIGTRAP && information.ssi_int == 0x7193);
    assert(!gw_domain_release_context());
    assert(native_entry(env, cls, 0) == 73);
  }
  assert(consume_jumps == 2);
  assert(!sigaction(SIGUSR1, &previous, NULL));
  puts(
      "{\"stage\":\"host-signalfd-unwind\",\"read\":true,\"readv\":true,"
      "\"context_release\":true,\"jni_retry\":true}");
#else
  (void)env;
  (void)cls;
  (void)descriptor;
#endif
}

static void host_descriptor_ignore(JNIEnv *env, jclass cls, int descriptor) {
  assert(native_entry(env, cls, 84) == 73);
  struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
  sigemptyset(&ignore.sa_mask);
  assert(!sigaction(SIGTRAP, &ignore, &previous));
  /* Host ignore must discard asynchronous Host arrivals while leaving the
   * independent synchronous Guest trap disposition and recovery intact. */
  assert(native_entry(env, cls, 84) == 73 && !(*env)->ExceptionCheck(env));
  assert(!sigaction(SIGTRAP, &previous, NULL));
  sigset_t pending;
  assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
  struct pollfd ready = {.fd = descriptor, .events = POLLIN};
  assert(poll(&ready, 1, 0) == 0);
  extern int gw_domain_release_context(void);
  assert(!gw_domain_release_context());
  assert(native_entry(env, cls, 84) == 73);
  struct signalfd_siginfo information;
  assert(read(descriptor, &information, sizeof information) ==
         sizeof information);
  assert(information.ssi_signo == SIGTRAP && information.ssi_int == 0x7193);
  assert(!gw_domain_release_context());
  assert(native_entry(env, cls, 0) == 73 && !(*env)->ExceptionCheck(env));
  puts(
      "{\"stage\":\"host-ignore-pending\",\"discarded\":true,"
      "\"guest_trap\":true,\"fresh_read\":true,\"context_release\":true,"
      "\"jni_retry\":true}");
}

static void host_signal_descriptor(JNIEnv *env, jclass cls,
                                   const sigset_t *set) {
  int descriptor = signalfd(-1, set, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  int alias = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
  assert(alias >= 0);

  for (int vector = 0; vector < 2; ++vector) {
    assert(native_entry(env, cls, 84) == 73);
    struct pollfd ready = {.fd = alias, .events = POLLIN};
    assert(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN));

    struct signalfd_siginfo information;
    errno = EDOM;
    if (!vector) {
      assert(read(alias, &information, sizeof information) ==
             sizeof information);
    } else {
      /* Legacy Android kernels require record boundaries in each vector. */
      struct signalfd_siginfo unused;
      struct iovec parts[] = {{&information, sizeof information},
                              {&unused, sizeof unused}};
      assert(readv(descriptor, parts, 2) == sizeof information);
    }

    assert(errno == EDOM);
    assert(information.ssi_signo == SIGTRAP &&
           information.ssi_code == SI_QUEUE);
    assert(information.ssi_pid == (unsigned)getpid() &&
           information.ssi_uid == getuid() && information.ssi_int == 0x7193);

    assert(poll(&ready, 1, 0) == 0);
    assert(native_entry(env, cls, 0) == 73);
  }

  host_descriptor_ignore(env, cls, descriptor);
  host_descriptor_unwind(env, cls, descriptor);
  assert(!close(descriptor) && !close(alias));

  descriptor = signalfd(-1, set, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  FILE *stream = fdopen(descriptor, "rb");
  assert(stream && fileno(stream) == descriptor);

  assert(native_entry(env, cls, 84) == 73);
  struct signalfd_siginfo information;
  assert(fread(&information, sizeof information, 1, stream) == 1);
  assert(information.ssi_signo == SIGTRAP && information.ssi_code == SI_QUEUE &&
         information.ssi_int == 0x7193);
  assert(information.ssi_pid == (unsigned)getpid() &&
         information.ssi_uid == getuid());

  assert(native_entry(env, cls, 0) == 73);

  assert(freopen("/dev/null", "r", stream) == stream);
  assert(fileno(stream) == descriptor);
  assert(fgetc(stream) == EOF && feof(stream));

  assert(!fclose(stream));
  assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);

  puts(
      "{\"stage\":\"host-signalfd-stdio\",\"native_file\":true,"
      "\"metadata\":true,\"fileno\":true,\"freopen\":true,\"close\":true,"
      "\"jni_retry\":true}");
  puts(
      "{\"stage\":\"host-signalfd\",\"read\":true,\"readv\":true,"
      "\"readiness\":true,\"metadata\":true,\"alias\":true,"
      "\"guest_return\":true}");
}

static void host_fault_masks(JNIEnv *env, jclass cls) {
  if (!getenv("GW_TEST_HOST_FAULT_MASK")) {
    return;
  }
  struct sigaction action = {.sa_sigaction = deferred_fault,
                             .sa_flags = SA_SIGINFO | SA_RESTART};
  struct sigaction inherited;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGTRAP, &action, &inherited));
  sigset_t blocked, previous;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTRAP);
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, &previous));
  puts("{\"stage\":\"host-fault-mask\",\"armed\":true}");
  fflush(stdout);
  int value = native_entry(env, cls, 84);
  assert(value == 73 && !(*env)->ExceptionCheck(env));
  sigset_t pending, current;
  assert(!deferred_faults);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &current));
  assert(sigismember(&current, SIGTRAP) == 1);
  assert(!sigpending(&pending) && sigismember(&pending, SIGTRAP) == 1);
  extern int gw_domain_release_context(void);
  assert(gw_domain_release_context() == EBUSY);
  assert(full_host_state() == GW_HOST_READY);
  for (int i = 0; i < 6; ++i) {
    assert(!sigismember(&pending, SIGRTMIN + i));
  }
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  assert(deferred_faults == 1);
  assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
  assert(native_entry(env, cls, 0) == 73);
  assert(!gw_domain_release_context());
  assert(native_entry(env, cls, 0) == 73);
  for (int wait = 0; wait < 3; ++wait) {
    assert(!pthread_sigmask(SIG_BLOCK, &blocked, NULL));
    assert(native_entry(env, cls, 84) == 73);
    siginfo_t information;
    errno = EDOM;
    if (wait == 0) {
      assert(sigwaitinfo(&blocked, &information) == SIGTRAP);
    } else if (wait == 1) {
      struct timespec timeout = {.tv_sec = 1};
      assert(sigtimedwait(&blocked, &information, &timeout) == SIGTRAP);
    } else {
      int number = 0;
      assert(!sigwait(&blocked, &number) && number == SIGTRAP);
    }
    assert(errno == EDOM);
    if (wait != 2) {
      assert(information.si_signo == SIGTRAP &&
             information.si_code == SI_QUEUE);
      assert(information.si_pid == getpid() && information.si_uid == getuid());
      assert(information.si_value.sival_int == 0x7193);
    }
    assert(deferred_faults == 1);
    assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
    struct timespec zero = {0};
    assert(sigtimedwait(&blocked, NULL, &zero) == -1 && errno == EAGAIN);
    assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  }
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, NULL));
  host_signal_descriptor(env, cls, &blocked);
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  assert(deferred_faults == 1);
  assert(!sigaction(SIGTRAP, &inherited, NULL));
  puts(
      "{\"stage\":\"host-fault-deferred\",\"deliveries\":1,"
      "\"first_siginfo\":true,\"logical_pending\":true,\"consuming_waits\":3,"
      "\"retirement_busy\":true,\"retirement_retry\":true}");
  puts("{\"stage\":\"host-fault-mask\",\"completed\":true,\"value\":73}");
  fflush(stdout);
}

static void nonlocal_run(struct application *app, JNIEnv *env, jclass cls) {
  void *entry = NULL;
  struct gw_loader_error error = {0};
  assert(app->loader->find(env, app->module, "sdk_nonlocal_probe", "(I)I",
                           &entry, &error) == GW_OK);
  native_entry = (void *)entry;
  assert(native_entry(env, cls, 0) == 73);
  host_gnu_thread_relay(env, cls);
  host_unblocked_fault(env, cls);
  host_unreserved_signal(env, cls);
  host_fault_masks(env, cls);
  stack_t alternate;
  assert(!sigaltstack(NULL, &alternate));
  assert(!(alternate.ss_flags & SS_DISABLE));
  alternate_low = (uintptr_t)alternate.ss_sp;
  alternate_high = alternate_low + alternate.ss_size;
  JNINativeMethod stack = {"hostStack", "()I", (void *)host_stack};
  assert((*env)->RegisterNatives(env, cls, &stack, 1) == JNI_OK);
  JNINativeMethod nested = {"hostNested", "()I", (void *)host_nested};
  assert((*env)->RegisterNatives(env, cls, &nested, 1) == JNI_OK);
  JNINativeMethod mask = {"hostMask", "()I", (void *)host_mask};
  assert((*env)->RegisterNatives(env, cls, &mask, 1) == JNI_OK);
  JNINativeMethod mask_blocked = {"hostMaskBlocked", "()I",
                                  (void *)host_mask_blocked};
  assert((*env)->RegisterNatives(env, cls, &mask_blocked, 1) == JNI_OK);
  sigset_t mask_test, inherited_mask;
  sigemptyset(&mask_test);
  sigaddset(&mask_test, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask_test, &inherited_mask));
  assert(native_entry(env, cls, 7) == 73);
  assert(!native_entry(env, cls, 10));
  assert(native_entry(env, cls, 8) == 1);
  assert(!host_mask(env, cls));
  assert(native_entry(env, cls, 8) == 0);
  assert(!pthread_sigmask(SIG_BLOCK, &mask_test, NULL));
  assert(native_entry(env, cls, 13) == 73);
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask_test, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited_mask, NULL));
  struct sigaction action = {.sa_handler = escape_handler};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, NULL, &previous));
  assert(previous.sa_handler == SIG_DFL);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  sigset_t enabled, inherited;
  sigemptyset(&enabled);
  sigaddset(&enabled, SDK_HOST_ESCAPE_SIGNAL);
  assert(!pthread_sigmask(SIG_UNBLOCK, &enabled, &inherited));
  for (int round = 0; round < 32; ++round) {
    expect_alternate = (round / 2) % 2;
    action.sa_flags = expect_alternate ? SA_ONSTACK : 0;
    assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
    assert(recover_once(env, cls, round % 2) == 73);
    assert(native_entry(env, cls, 2) == 73);
    assert(!(*env)->ExceptionCheck(env));
  }
  assert(transfers == 64 && alternate_hits == 32);
  action.sa_flags = 0;
  expect_alternate = 0;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  const char *windows = getenv("GW_TEST_UNWIND_WINDOWS");
  int window_tests = windows && !strcmp(windows, "1");
  if (window_tests) {
    for (int point = GW_UNWIND_TRAMPOLINE_READY;
         point <= GW_UNWIND_TRAMPOLINE_CLEANED; ++point) {
      assert(recover_window(env, cls, point) == 73);
      window_point = point;
      assert(native_entry(env, cls, 2) == 73);
      assert(!(*env)->ExceptionCheck(env));
      window_point = 0;
    }
  }
  assert(transfers == 64 + 8 * window_tests);
  recover_floating_point(env, cls);
  sigaddset(&action.sa_mask, SIGWINCH);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  return_guest_signal_mask(env, cls, 12);
  for (int mode = 16; mode <= 20; ++mode) {
    return_guest_signal_mask(env, cls, mode);
  }
  action.sa_flags = SA_NODEFER;
  expect_nodefer = 1;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  return_guest_signal_mask(env, cls, 12);
  expect_nodefer = 0;
  action.sa_flags = 0;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, NULL));
  assert(!pthread_sigmask(SIG_BLOCK, &mask_test, NULL));
  expect_host_usr1_blocked = 1;
  return_guest_signal_mask(env, cls, 14);
  expect_host_usr1_blocked = 0;
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask_test, NULL));
  recover_guest_signal_mask(env, cls);
  const int guest_faults[] = {22, 23, 24, 25, 26, 27, 28,
                              29, 30, 31, 32, 36, 37};
  for (unsigned i = 0; i < sizeof guest_faults / sizeof guest_faults[0]; ++i) {
    int observed = native_entry(env, cls, guest_faults[i]);
    if (observed != 73) {
      fprintf(stderr, "GUEST_FAULT_RECOVERY mode=%d result=%d\n",
              guest_faults[i], observed);
    }
    assert(observed == 73 && !(*env)->ExceptionCheck(env));
    assert(native_entry(env, cls, 0) == 73);
  }
  int fpe = native_entry(env, cls, 87);
  assert((fpe == 73 || fpe == 74) && !(*env)->ExceptionCheck(env));
  assert(native_entry(env, cls, 0) == 73);
  struct sigaction trap_previous,
      trap_action = {.sa_handler = host_trap, .sa_flags = SA_RESETHAND};
  sigemptyset(&trap_action.sa_mask);
  assert(!sigaction(SIGTRAP, &trap_action, &trap_previous));
  assert(native_entry(env, cls, 81) == 73);
  assert(native_entry(env, cls, 0) == 73);
  assert(native_entry(env, cls, 28) == 73);
  struct sigaction trap_observed;
  assert(!sigaction(SIGTRAP, NULL, &trap_observed));
  assert(trap_observed.sa_handler == host_trap && !host_traps);
  assert(!raise(SIGTRAP) && host_traps == 1);
  assert(!sigaction(SIGTRAP, NULL, &trap_observed));
  assert(trap_observed.sa_handler == SIG_DFL);
  assert(!signal_router_verify());
  trap_action.sa_handler = SIG_IGN;
  trap_action.sa_flags = 0;
  assert(!sigaction(SIGTRAP, &trap_action, NULL));
  assert(native_entry(env, cls, 28) == 73);
  assert(!raise(SIGTRAP) && host_traps == 1);
  assert(!sigaction(SIGTRAP, &trap_previous, NULL));
  for (int mode = 40; mode <= 48; ++mode) {
    assert(host_stack(env, cls) == 73);
    int observed = native_entry(env, cls, mode);
    if (observed != 73) {
      fprintf(stderr, "GUEST_STACK_RECOVERY mode=%d result=%d\n", mode,
              observed);
    }
    assert(observed == 73 && !(*env)->ExceptionCheck(env));
    assert(host_stack(env, cls) == 73);
    assert(native_entry(env, cls, 0) == 73);
  }
  struct sigaction stack_previous,
      stack_action = {.sa_sigaction = stack_host_signal,
                      .sa_flags = SA_SIGINFO};
  sigemptyset(&stack_action.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, NULL, &stack_previous));
  for (int alternate = 0; alternate <= 1; ++alternate) {
    expect_alternate = alternate;
    stack_action.sa_flags = SA_SIGINFO | (alternate ? SA_ONSTACK : 0);
    assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &stack_action, NULL));
    assert(native_entry(env, cls, 49) == 73);
    assert(host_stack(env, cls) == 73);
  }
  assert(stack_signal_hits == 2);
  for (int mode = 50; mode <= 52; mode += 2) {
    for (int autodisarm = 0; autodisarm <= 1; ++autodisarm) {
      for (int alternate = 0; alternate <= 1; ++alternate) {
        recover_stack_unwind(env, cls, mode, autodisarm, alternate);
      }
    }
  }
  assert(stack_signal_hits == 10);
  expect_alternate = 0;
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &stack_previous, NULL));
  host_numbered_signals(env, cls);
  int wait_skipped = guest_waits(env, cls);
  int pending_test = getenv("GW_TEST_SIGNAL_PENDING") != NULL;
  if (pending_test) {
    pending_host_signal(env, cls);
    pending_queued_signals(env, cls);
  }
  assert(transfers == 66 + 8 * window_tests);
#ifdef GW_TEST_NATIVE_UNWIND
  if (window_tests) {
    route_env = env;
    route_class = cls;
    const int target_modes[] = {90, 91, 94, 95, 96};
    for (int round = 0; round < 2; ++round) {
      for (unsigned i = 0; i < sizeof target_modes / sizeof *target_modes;
           ++i) {
        int mode = target_modes[i];
        sigset_t before, after;
        assert(!pthread_sigmask(SIG_SETMASK, NULL, &before));
        route_depth = mode == 94 || mode == 96 ? 3 : mode == 95 ? 5 : 1;
        route_level = 0;
        int value = native_entry(env, cls, mode);
        int depth = route_depth;
        route_depth = 0;
        assert(value == 73 && route_level == depth);
        assert(!pthread_sigmask(SIG_SETMASK, NULL, &after));
        for (int number = 1; number < NSIG; ++number) {
          assert(sigismember(&before, number) == sigismember(&after, number));
        }
        assert(!(*env)->ExceptionCheck(env));
        assert(native_entry(env, cls, 0) == 73);
        printf(
            "{\"stage\":\"guest-target\",\"round\":%d,\"depth\":%d,"
            "\"captures\":%d,\"saved_mask\":%d,\"value\":%d,\"checked\":%s,"
            "\"subsequent_call\":true,\"host_mask_preserved\":true,"
            "\"java_frames_crossed\":false}\n",
            round, depth, mode == 95 ? 300 : 1,
            mode == 90 || mode == 94 || mode == 96, value,
            mode == 96 ? "true" : "false");
        fflush(stdout);
      }
    }
  }
#endif
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &previous, NULL));
  /* Emit before reentry: a hang must retain the successful transfer evidence
   * without being reported as a compatibility pass. */
  printf(
      "{\"stage\":\"nonlocal-transfer\",\"result\":\"OBSERVED\","
      "\"transfers\":%d,\"nested\":%d,\"alternate_stack\":32,"
      "\"publication_windows\":%d,\"floating_point\":true,"
      "\"cooperative_masks\":true,\"handler_mask\":true,"
      "\"reserved_masks\":true,\"context_masks\":true,"
      "\"guest_faults\":15,\"guest_fault_threads\":8,"
      "\"guest_signal_stacks\":19,\"stack_unwinds\":8,"
      "\"host_signal_numbers\":2,\"guest_wait_cases\":19,"
      "\"guest_registration_aliases\":9,"
      "\"wait_unwinds\":2,\"guest_wait_skipped\":%d,\"kernel_pending\":%s}\n",
      transfers, 32 + 4 * window_tests, 8 * window_tests, wait_skipped,
      pending_test ? "true" : "false");
  fflush(stdout);
  assert(native_entry(env, cls, 0) == 73);
  assert(!(*env)->ExceptionCheck(env));
  int closed = app->loader->close(env, app->module, &error);
  printf("{\"stage\":\"nonlocal-close\",\"status\":%d,\"error\":%d}\n", closed,
         error.code);
  fflush(stdout);
  assert(closed == GW_OK);
}

static void router_quarantine_case(struct application *app, JNIEnv *env,
                                   jclass cls) {
  struct gw_loader_error error = {0};
  void *entry = NULL;
  assert(app->loader->find(env, app->module, "sdk_nonlocal_probe", "(I)I",
                           &entry, &error) == GW_OK);
  jint (*call)(JNIEnv *, jclass, jint) = entry;
  assert(call(env, cls, 0) == 73 && !(*env)->ExceptionCheck(env));
  unsigned retained = signal_router_retained();
  assert(signal_router_quarantine() == EIO);

  assert(!call(env, cls, 0) && (*env)->ExceptionCheck(env));
  (*env)->ExceptionClear(env);
  assert(full_host_state() == GW_HOST_QUARANTINED);
  assert(app->loader->close(env, app->module, &error) == GW_ERROR);
  assert(error.code == GW_E_QUARANTINED);
  assert(gw_host_runtime_shutdown() == EIO);
  assert(signal_router_restore() == EIO);
  assert(signal_host_relay_running());
  assert(signal_router_retained() >= retained);
  puts(
      "{\"result\":\"PASS\",\"case\":\"router-quarantine\","
      "\"warmup\":true,\"admission_rejected\":true,\"close_rejected\":true,"
      "\"shutdown_rejected\":true,\"resources_retained\":true}");
  fflush(NULL);
  _exit(0); /* The test owner disposes of this intentionally pinned process. */
}

static void *application_run(void *argument) {
  struct application *app = argument;
  JavaVM *vm = app->vm;
  int worker = app->worker;
  JNIEnv *env;
  assert((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK);
  jclass cls = (*env)->FindClass(env, "SdkProbe");
  assert(cls);
  if (app->router_quarantine) {
    router_quarantine_case(app, env, cls);
  }
  const char *fatal_mode = getenv("GW_TEST_FATAL_MODE");
  if (fatal_mode) {
    int selected = atoi(fatal_mode);
    assert((selected >= 33 && selected <= 35) || selected == 82 ||
           selected == 83);
    struct gw_loader_error error = {0};
    void *entry = NULL;
    assert(app->loader->find(env, app->module, "sdk_nonlocal_probe", "(I)I",
                             &entry, &error) == GW_OK);
    jint (*call)(JNIEnv *, jclass, jint) = entry;
    printf("{\"stage\":\"guest-default\",\"mode\":%d,\"tid\":%ld}\n", selected,
           syscall(SYS_gettid));
    fflush(stdout);
    call(env, cls, selected);
    _exit(141);
  }
  if (app->nonlocal) {
    nonlocal_run(app, env, cls);
    (*env)->DeleteLocalRef(env, cls);
    assert((*vm)->DetachCurrentThread(vm) == JNI_OK);
    return NULL;
  }
  jmethodID probe = (*env)->GetStaticMethodID(env, cls, "probe", "(I[B)I");
  jmethodID tid = (*env)->GetStaticMethodID(env, cls, "tid", "()J");
  assert(probe && tid);
  jbyteArray data = (*env)->NewByteArray(env, 1024 * 1024);
  assert(data);
  for (int i = 1; i <= 3; ++i) {
    assert((*env)->CallStaticIntMethod(env, cls, probe, i, data) == i + 9);
    assert(!(*env)->ExceptionCheck(env));
    jbyte last = 0;
    (*env)->GetByteArrayRegion(env, data, 1024 * 1024 - 1, 1, &last);
    assert(last == i && !(*env)->ExceptionCheck(env));
  }
  jlong guest_tid = (*env)->CallStaticLongMethod(env, cls, tid);
  assert(!(*env)->ExceptionCheck(env));
  assert(worker ? guest_tid != syscall(SYS_gettid)
                : guest_tid == syscall(SYS_gettid));
  (*env)->DeleteLocalRef(env, data);
  (*env)->DeleteLocalRef(env, cls);
  assert((*vm)->DetachCurrentThread(vm) == JNI_OK);
  return NULL;
}

static void retry_preclaim_failure(JNIEnv *env,
                                   const struct gw_host_runtime_config *config,
                                   int borrowed_relay) {
  sigset_t before, after;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &before));

  for (int invalid = 0; invalid < 2; ++invalid) {
    struct gw_host_bridge_config bridge = *config->bridge;
    if (invalid) {
      bridge.flags |= UINT32_C(1) << 31;
    } else {
      bridge.abi_version = 0;
    }
    struct gw_host_runtime_config rejected = *config;
    rejected.bridge = &bridge;

    assert(gw_host_runtime_start(env, &rejected) == EIO);
    assert(full_host_state() == GW_HOST_NEW);
    assert(signal_host_relay_running() == borrowed_relay);
    assert(!(*env)->ExceptionCheck(env));
  }

  assert(!pthread_sigmask(SIG_SETMASK, NULL, &after));
  for (int number = 1; number < NSIG; ++number) {
    assert(sigismember(&before, number) == sigismember(&after, number));
  }
}

/* argv: libjvm classpath loader library-path service module directory backend.
 * This consumer uses installed headers/libraries, with no source-tree include.
 */
int main(int argc, char **argv) {
  assert(argc == 9);
#ifdef GW_TEST_NATIVE_UNWIND
  assert(!gw_nonlocal_init());
#endif
#ifndef GW_TEST_LATE_SIGNALS
  assert(!signal_router_init());
  assert(!signal_router_adopt());
  if (getenv("GW_TEST_HOST_FAULT_MASK")) {
    const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                                 SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
    assert(!gw_host_signal_channel_enable(notifications, 6));
  }
#endif
  if (!strcmp(argv[8], "nonlocal")) {
    install_asynchronous_fault();
  }
  JavaVM *vm;
  JNIEnv *env;
#ifdef __ANDROID__
  assert(!domain_vm_start(&vm, &env, 1));
#else
  void *jvm = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  assert(jvm);
  jint (*create)(JavaVM **, void **, void *) = dlsym(jvm, "JNI_CreateJavaVM");
  assert(create);
  char *classpath;
  assert(asprintf(&classpath, "-Djava.class.path=%s", argv[2]) > 0);
  JavaVMOption option[] = {{.optionString = classpath},
                           {.optionString = "-Xcheck:jni"}};
  JavaVMInitArgs args = {
      .version = JNI_VERSION_1_6, .nOptions = 2, .options = option};
  assert(create(&vm, (void **)&env, &args) == JNI_OK);
  free(classpath);
#endif
#ifdef GW_TEST_LATE_SIGNALS
  int installed = domain_late_signal_install(env);
  if (getenv("GW_EXPECT_SIGNAL_PROFILE_REJECTION")) {
    assert(installed == ENOTSUP);
    assert(!signal_router_enabled());
    puts("{\"result\":\"PASS\",\"case\":\"unconfigured-signal-profile\"}");
    fflush(NULL);
    _exit(0);
  }
  assert(!installed);
  consumer_action = signal_router_action;
#endif
#ifdef __ANDROID__
  private_host_registration();
#endif
  struct gw_host_bridge_config bridge = {
      .abi_version = GW_HOST_BRIDGE_CONFIG_V2,
      .struct_size = sizeof bridge,
      .flags = GW_HOST_DIRECT_IMAGES,
      .native_directory = argv[7],
      .image_catalog = argv[7]};
  struct gw_signal_contract signals = {
      .version = GW_SIGNAL_CONTRACT_V1,
      .size = sizeof signals,
      .host_reserved = signal_router_reserved()};
  struct gw_host_runtime_config config = {.version = GW_HOST_RUNTIME_CONFIG_V1,
                                          .size = sizeof config,
                                          .loader_path = argv[3],
                                          .library_path = argv[4],
                                          .service_path = argv[5],
                                          .bridge = &bridge,
                                          .signals = &signals};
  int worker =
      !strcmp(argv[8], "worker") || !strcmp(argv[8], "worker-borrowed-relay");
  if (worker) {
    config.backend = GW_EXECUTION_WORKER;
  }

  int borrowed_relay = strstr(argv[8], "borrowed-relay") != NULL;
  assert(!signal_host_relay_running());
  if (borrowed_relay) {
    assert(!signal_host_relay_start());
  }
  retry_preclaim_failure(env, &config, borrowed_relay);

  int result = gw_host_runtime_start(env, &config);
  if (getenv("GW_EXPECT_RUNTIME_REJECTION")) {
    assert(result == ELIBBAD || result == EPROTO);
    assert(full_host_state() == GW_HOST_QUARANTINED);
    assert(signal_host_relay_running());
    puts("{\"result\":\"PASS\",\"case\":\"artifact-rejection\"}");
    fflush(NULL);
    _exit(0);
  }
  if (result) {
    fprintf(stderr, "Runtime start: %s (%d)\n", strerror(result), result);
  }
  assert(!result);
  assert(full_host_state() == GW_HOST_READY);
  const struct gw_loader_api_v1 *loader =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof *loader);
  assert(loader);
  struct gw_loader_library library;
  struct gw_loader_error error = {0};
  assert(loader->open(env, argv[6], 0, &library, &error) == GW_OK);
  assert(!(*env)->ExceptionCheck(env));
  int nonlocal = !strcmp(argv[8], "nonlocal");
  struct application app = {
      .vm = vm,
      .worker = worker,
      .nonlocal = nonlocal,
      .router_quarantine = !strcmp(argv[8], "router-quarantine"),
      .loader = loader,
      .module = library.handle};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, application_run, &app));
  assert(!pthread_join(thread, NULL));
  if (!nonlocal) {
    assert(loader->close(env, library.handle, &error) == GW_OK);
  }
  assert(!(*env)->ExceptionCheck(env));
  assert(!gw_host_runtime_shutdown());
  assert(full_host_state() == GW_HOST_STOPPED);
  assert(signal_host_relay_running() == borrowed_relay);
  if (borrowed_relay) {
    assert(!signal_host_relay_stop());
  }
  printf(
      "{\"stage\":\"runtime-relay-lifecycle\",\"preclaim_failures\":2,"
      "\"retry\":true,\"borrowed\":%s,\"shutdown_preserved_owner\":true,"
      "\"masks_preserved\":true}\n",
      borrowed_relay ? "true" : "false");

  assert((*vm)->DestroyJavaVM(vm) == JNI_OK);
#ifdef GW_TEST_LATE_SIGNALS
  assert(!domain_late_signal_restore());
#else
  assert(!signal_router_restore());
#endif
  printf(
      "{\"result\":\"PASS\",\"backend\":\"%s\",\"array_bytes\":%d,"
      "\"nonlocal\":%s}\n",
      worker ? "worker" : "same-thread", nonlocal ? 0 : 1048576,
      nonlocal ? "true" : "false");
  /* The Guest supervisor and idle VM bindings deliberately remain pinned. */
  fflush(NULL);
  _exit(0);
}
