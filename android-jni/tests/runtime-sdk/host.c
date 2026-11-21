#define _GNU_SOURCE
#include "runtime_bridge.h"
#include "signal_bootstrap.h"
#include "signal-router.h"
#include "signal_channel.h"
#include "signal-relay.h"
#include "signals.h"
#ifdef GW_TEST_NATIVE_UNWIND
#include "nonlocal.h"
#endif
#include <assert.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fenv.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "nonlocal_probe.h"
#include "adopted-signals.h"
#include "guest-fd-probe.h"
#include "jni-recovery.h"
#include "vm-signals.h"
#include "internal/domain_unwind.h"
#ifdef __ANDROID__
#include "vm-start.h"
#endif

struct application {
  JavaVM *vm;
  int worker;
  int worker_signals;
  int adopted_signals;
  int nonlocal;
  int router_quarantine;
  int held_guest_fd;
  const char *vm_signal_directory;
  const struct gw_loader_api_v1 *loader;
  uint64_t module;
};

static _Atomic int worker_host_signal_count;
static _Thread_local sigjmp_buf adopted_jump;
static _Thread_local volatile sig_atomic_t adopted_jump_active;
static _Thread_local struct adopted_signal_probe *adopted_guest_jump;
static _Thread_local struct adopted_signal_probe *adopted_host_nested;
#ifndef __ANDROID__
static _Thread_local ucontext_t adopted_native_context;
static _Thread_local ucontext_t adopted_host_fiber;
static _Thread_local ucontext_t adopted_host_suspended;
static _Thread_local volatile sig_atomic_t adopted_context_active;
static _Thread_local volatile sig_atomic_t adopted_host_fiber_ran;
static _Thread_local void *adopted_fiber_stack;
#endif

static void worker_host_signal(int number) {
  assert(number == SIGUSR1 && signal_host_relay_running());
  atomic_fetch_add_explicit(&worker_host_signal_count, 1, memory_order_relaxed);
}

#ifndef __ANDROID__
static void run_on_host_stack(void) {
  char local;
  assert((uintptr_t)&local - (uintptr_t)adopted_fiber_stack < 65536);
  adopted_host_fiber_ran = 1;
  assert(!setcontext(&adopted_host_suspended));
  assert(!"Host swapcontext failed to resume");
}

#endif

static void adopted_swap_host(struct adopted_signal_probe *probe) {
  int invocation = atomic_fetch_add(&probe->host_entries, 1);
  if (probe->mode == SIGNAL_RETIRE_CHAIN && invocation == 1) {
    atomic_store_explicit(&probe->requested, 3, memory_order_release);
    while (atomic_load_explicit(&probe->handled, memory_order_acquire) < 3) {
      sched_yield();
    }
    assert(!"Innermost Host callback resumed after retirement");
  }
  if (invocation) {
    assert(probe->swap_phase == 2 * invocation - 1 && !probe->host_returned);
    assert(signal_host_retirement_busy());
    return;
  }

  volatile unsigned char retained[4096];
  for (size_t i = 0; i < sizeof retained; ++i) {
    retained[i] = (unsigned char)(i ^ 0x75);
  }
  atomic_store_explicit(&probe->requested, 2, memory_order_release);
  while (atomic_load_explicit(&probe->handled, memory_order_acquire) < 2) {
    sched_yield();
  }
  if (adopted_retires_saved(probe->mode)) {
    assert(!"Retired Host callback resumed");
  }
  assert(probe->swap_phase == 2 * SIGNAL_SWAP_ROUNDS + 1);
  assert(probe->host_entries == SIGNAL_SWAP_ROUNDS + 1);
  for (size_t i = 0; i < sizeof retained; ++i) {
    assert(retained[i] == (unsigned char)(i ^ 0x75));
  }
  probe->host_returned = 1;
}

static void adopted_host_jump(int number) {
  assert(number == SIGWINCH);
  struct adopted_signal_probe *probe = adopted_host_nested;
  if (adopted_jump_active &&
      (!probe || probe->mode != SIGNAL_RETIRE_HOST_JUMP || probe->swap_phase)) {
    siglongjmp(adopted_jump, 39);
  }
#ifndef __ANDROID__
  if (adopted_context_active) {
    assert(!swapcontext(&adopted_host_suspended, &adopted_host_fiber));
    assert(adopted_host_fiber_ran);
    ucontext_t copy = adopted_native_context;
#if defined(__x86_64__)
    copy.uc_mcontext.fpregs = &copy.__fpregs_mem;
#endif
    assert(!setcontext(&copy));
    assert(!"Host context restore returned to Guest signal scope");
  }
#endif
  if (probe) {
    assert(syscall(SYS_gettid) == probe->tid);
    if (adopted_scope_swap(probe->mode)) {
      adopted_swap_host(probe);
      return;
    }
    atomic_store_explicit(&probe->requested, 2, memory_order_release);
    while (atomic_load_explicit(&probe->handled, memory_order_acquire) <
           adopted_activations(probe->mode)) {
      sched_yield();
    }
    assert(!adopted_guest_target(probe->mode) &&
           !adopted_parent_target(probe->mode));
    probe->host_returned = 1;
  }
}

#ifndef __ANDROID__
static void await_adopted_context(struct adopted_signal_probe *probe) {
  adopted_fiber_stack = malloc(65536);
  assert(adopted_fiber_stack);
  adopted_host_fiber_ran = 0;
  assert(!getcontext(&adopted_host_fiber));
  adopted_host_fiber.uc_stack =
      (stack_t){.ss_sp = adopted_fiber_stack, .ss_size = 65536};
  adopted_host_fiber.uc_link = NULL;
  makecontext(&adopted_host_fiber, run_on_host_stack, 0);
  assert(!getcontext(&adopted_native_context));
  if (!adopted_context_active) {
    adopted_context_active = 1;
    atomic_store_explicit(&probe->requested, 1, memory_order_release);
    for (;;) {
      sched_yield();
    }
  }
  adopted_context_active = 0;
  free(adopted_fiber_stack);
  adopted_fiber_stack = NULL;
  assert(!signal_host_retirement_busy());
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(!sigismember(&mask, SIGWINCH));
}
#endif

static void await_adopted_signal(JNIEnv *env, jclass cls,
                                 struct adopted_signal_probe *probe) {
  if (probe->mode == SIGNAL_JAVA || probe->mode == SIGNAL_CONTEXT_EDIT) {
    jobject shared = (*env)->NewDirectByteBuffer(env, probe, sizeof *probe);
    jmethodID observe = (*env)->GetStaticMethodID(
        env, cls, "observeAdoptedSignal", "(Ljava/nio/ByteBuffer;)I");
    assert(shared && observe);
    assert((*env)->CallStaticIntMethod(env, cls, observe, shared) == 1);
    assert(!(*env)->ExceptionCheck(env));
    (*env)->DeleteLocalRef(env, shared);
  } else if (probe->mode == SIGNAL_HOST_JUMP ||
             probe->mode == SIGNAL_RETIRE_HOST_JUMP) {
    int result = sigsetjmp(adopted_jump, 1);
    if (!result) {
      adopted_jump_active = 1;
      atomic_store_explicit(&probe->requested, 1, memory_order_release);
      for (;;) {
        sched_yield();
      }
    }
    adopted_jump_active = 0;
    assert(result == 39 && !signal_host_retirement_busy());
#ifndef __ANDROID__
  } else if (probe->mode == SIGNAL_HOST_CONTEXT) {
    await_adopted_context(probe);
#endif
  } else if (adopted_nested(probe->mode)) {
    atomic_store_explicit(&probe->requested, 1, memory_order_release);
    while (atomic_load_explicit(&probe->handled, memory_order_acquire) < 2) {
      sched_yield();
    }
    if (adopted_parent_target(probe->mode)) {
      assert(probe->parent_resumed && !probe->host_returned);
    } else if (adopted_retires_saved(probe->mode)) {
      /* handled is published on entry. The interrupted Host cannot execute
       * this check until the older Guest handler has completed retirement. */
      assert(probe->swap_phase == 1 && !probe->host_returned);
    } else {
      assert(probe->host_returned);
    }
  } else if (probe->mode == SIGNAL_SLEEP_ABSOLUTE) {
    struct timespec deadline, now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &deadline));
    deadline.tv_nsec += 350000000;
    if (deadline.tv_nsec >= 1000000000) {
      ++deadline.tv_sec;
      deadline.tv_nsec -= 1000000000;
    }
    assert(!clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL));
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec > deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec));
  } else {
    const struct timespec duration = {.tv_nsec = 350000000};
    int result = probe->mode == SIGNAL_POLL ? ppoll(NULL, 0, &duration, NULL)
                                            : nanosleep(&duration, NULL);
    assert(!result);
  }
  assert(atomic_load_explicit(&probe->handled, memory_order_acquire) ==
         adopted_activations(probe->mode));
}

static void adopted_signals(JNIEnv *env, jclass cls) {
  jmethodID prepare =
      (*env)->GetStaticMethodID(env, cls, "prepareAdoptedSignal", "(J)V");
  jmethodID finish =
      (*env)->GetStaticMethodID(env, cls, "finishAdoptedSignal", "()I");
  jmethodID jump =
      (*env)->GetStaticMethodID(env, cls, "jumpAdoptedSignal", "(J)I");
  assert(prepare && finish && jump);
  for (int mode = SIGNAL_JAVA; mode < SIGNAL_CASES; ++mode) {
#ifdef __ANDROID__
    if (mode == SIGNAL_HOST_CONTEXT) {
      continue; /* Bionic has no public getcontext/setcontext pair. */
    }
#endif
    struct adopted_signal_probe probe = {.mode = mode,
                                         .tid = (int)syscall(SYS_gettid)};
    struct sigaction original;
    if (adopted_scope_swap(mode)) {
      assert(!sigaction(SIGWINCH, NULL, &original));
      struct sigaction action = original;
      action.sa_flags |= SA_ONSTACK;
      if (mode == SIGNAL_RETIRE_CHAIN) {
        action.sa_flags |= SA_NODEFER;
      }
      assert(!sigaction(SIGWINCH, &action, NULL));
    }
    jlong address = (jlong)(uintptr_t)&probe;
    adopted_host_nested = adopted_nested(mode) ? &probe : NULL;
    if (adopted_guest_target(mode)) {
      adopted_guest_jump = &probe;
      assert((*env)->CallStaticIntMethod(env, cls, jump, address) == 73);
      assert(!adopted_guest_jump && !signal_host_retirement_busy());
    } else {
      (*env)->CallStaticVoidMethod(env, cls, prepare, address);
      assert(!(*env)->ExceptionCheck(env));

      errno = EDOM;
      await_adopted_signal(env, cls, &probe);
      if (mode != SIGNAL_JAVA && mode != SIGNAL_CONTEXT_EDIT) {
        assert(errno == EDOM);
      }
      assert((*env)->CallStaticIntMethod(env, cls, finish) == 73);
    }
    if (adopted_scope_swap(mode)) {
      assert(!sigaction(SIGWINCH, &original, NULL));
      assert(!signal_host_retirement_busy());
    }
    if (mode == SIGNAL_PARENT_SWAP) {
      printf(
          "{\"stage\":\"adopted-scope-resume\",\"round_trips\":%d,"
          "\"host_reentries\":%d,\"host_returned\":true}\n",
          SIGNAL_SWAP_ROUNDS, probe.host_entries - 1);
    } else if (mode == SIGNAL_PARENT_SWAP_OVERLAP) {
      assert(probe.overlap_rejected && probe.host_returned &&
             !signal_host_retirement_busy());
      puts("{\"stage\":\"adopted-scope-overlap\",\"rejected\":true}");
    } else if (adopted_retires_saved(mode)) {
      int retired = adopted_activations(mode) - 1;
      assert(probe.host_entries == retired && !probe.host_returned &&
             probe.stale_rejections == retired);
      printf(
          "{\"stage\":\"adopted-scope-retire\",\"exit\":\"%s\","
          "\"stale_rejected\":%d,\"host_callbacks_retired\":%d}\n",
          mode == SIGNAL_RETIRE_HOST_JUMP ? "host-jump" : "return", retired,
          retired);
    }
    adopted_host_nested = NULL;
    assert(!(*env)->ExceptionCheck(env));
    printf(
        "{\"stage\":\"adopted-signal\",\"mode\":%d,\"same_tid\":true,"
        "\"owned_context\":true,\"post_signal_jni\":true}\n",
        mode);
  }
}

static void vm_suspend_and_sample(JNIEnv *env, const char *directory) {
  extern _Atomic int domain_sr_deliveries;
  struct sdk_vm_signals control = {0};
  jclass cls = (*env)->FindClass(env, "SdkVmSignals");
  assert(cls);
  jmethodID run = (*env)->GetStaticMethodID(
      env, cls, "run", "(Ljava/nio/ByteBuffer;JLjava/lang/String;)[I");
  jobject shared = (*env)->NewDirectByteBuffer(env, &control, sizeof control);
  jstring path = (*env)->NewStringUTF(env, directory);
  assert(run && shared && path);

  int before = atomic_load(&domain_sr_deliveries);
  jintArray result = (*env)->CallStaticObjectMethod(
      env, cls, run, shared, (jlong)(uintptr_t)&control, path);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    abort();
  }
  jint samples[2];
  assert(result && (*env)->GetArrayLength(env, result) == 2);
  (*env)->GetIntArrayRegion(env, result, 0, 2, samples);
  assert(!(*env)->ExceptionCheck(env));
  int delivered = atomic_load(&domain_sr_deliveries) - before;
  assert(control.stage == SDK_VM_STACKS_RELEASED && samples[0] > 0 &&
         samples[1] > 0 && delivered >= samples[1]);
  printf(
      "{\"stage\":\"vm-suspend-and-sample\",\"parked_frames\":2,"
      "\"sr_deliveries\":%d,\"native_samples\":%d,\"java_samples\":%d,"
      "\"jni_return_suspended\":true,\"stacks_released\":true}\n",
      delivered, samples[0], samples[1]);

  (*env)->DeleteLocalRef(env, result);
  (*env)->DeleteLocalRef(env, path);
  (*env)->DeleteLocalRef(env, shared);
  (*env)->DeleteLocalRef(env, cls);
}

static long guest_owner_tid(void) {
  DIR *tasks = opendir("/proc/self/task");
  assert(tasks);
  long owner = 0;
  struct dirent *task;
  while ((task = readdir(tasks))) {
    char *end;
    long tid = strtol(task->d_name, &end, 10);
    if (*end || tid <= 0) {
      continue;
    }
    char path[96], name[64];
    assert(snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid) > 0);
    FILE *file = fopen(path, "re");
    if (!file) {
      continue; /* An unrelated VM thread can exit during enumeration. */
    }
    int matches =
        fgets(name, sizeof name, file) && !strcmp(name, "gw-guest-owner\n");
    assert(!fclose(file));
    if (matches) {
      assert(!owner);
      owner = tid;
    }
  }
  assert(!closedir(tasks));
  return owner;
}

static void check_owner_mask(long tid) {
  char path[96];
  assert(snprintf(path, sizeof path, "/proc/self/task/%ld/status", tid) > 0);
  FILE *file = fopen(path, "re");
  assert(file);
  char *line = NULL;
  size_t capacity = 0;
  unsigned long long blocked = 0;
  int found = 0;
  while (getline(&line, &capacity, file) != -1) {
    if (sscanf(line, "SigBlk: %llx", &blocked) == 1) {
      found = 1;
      break;
    }
  }
  free(line);
  assert(!fclose(file) && found);
  uint64_t expected = UINT64_MAX & ~((UINT64_C(1) << (SIGKILL - 1)) |
                                     (UINT64_C(1) << (SIGSTOP - 1)));
  assert(blocked == expected);
}

static void check_owner_quiescent(long tid) {
  assert(tid > 0 && guest_owner_tid() == tid);
  check_owner_mask(tid);
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    char path[96], line[512];
    assert(snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", tid) > 0);
    FILE *file = fopen(path, "re");
    assert(file);
    assert(fgets(line, sizeof line, file));
    assert(!fclose(file));
    long number;
    unsigned long long address, operation, expected;
    if (sscanf(line, "%ld %llx %llx %llx", &number, &address, &operation,
               &expected) == 4 &&
        number == SYS_futex && operation == FUTEX_WAIT_PRIVATE && !expected) {
      break;
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 3);
    sched_yield();
  }
  check_owner_mask(tid);
}

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
  if (point == GW_UNWIND_RPC_READY && adopted_guest_jump) {
    struct adopted_signal_probe *probe = adopted_guest_jump;
    adopted_guest_jump = NULL;
    atomic_store_explicit(&probe->requested, 1, memory_order_release);
    while (!atomic_load_explicit(&probe->handled, memory_order_acquire)) {
      sched_yield();
    }
  }
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

static void guest_signal_unwind(JNIEnv *env, jclass cls) {
  for (int mode = 97; mode <= 100; ++mode) {
    int value = native_entry(env, cls, mode);
    fprintf(stderr, "GUEST_SIGNAL_UNWIND mode=%d value=%d\n", mode, value);
    assert(value == 73 && !(*env)->ExceptionCheck(env));
    assert(native_entry(env, cls, 0) == 73);
    printf(
        "{\"stage\":\"guest-signal-unwind\",\"mode\":%d,\"value\":%d,"
        "\"handler_cleanup\":true,\"body_cleanup\":true,"
        "\"subsequent_call\":true,\"java_frames_crossed\":false}\n",
        mode, value);
    fflush(stdout);
  }
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
  assert(native_entry(env, cls, 87) == 73 && !(*env)->ExceptionCheck(env));
  assert(native_entry(env, cls, 101) == 73 && !(*env)->ExceptionCheck(env));
  assert(native_entry(env, cls, 0) == 73);
  guest_signal_unwind(env, cls);
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
    sdk_run_jni_recovery(env, cls, app->loader, app->module);
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
  if (app->worker_signals) {
    jmethodID signals =
        (*env)->GetStaticMethodID(env, cls, "workerSignals", "()I");
    assert(signals);
    int children = (*env)->CallStaticIntMethod(env, cls, signals);
    assert(children >= 0 && !(*env)->ExceptionCheck(env));
    assert(atomic_load(&worker_host_signal_count) == 1);
    printf(
        "{\"stage\":\"worker-signals\",\"native_children\":%d,"
        "\"cancelled_children\":%d,"
        "\"alternate_stack\":true,\"one_shot\":true,\"consuming_waits\":true,"
        "\"host_parallel_disposition\":true,\"native_cancel\":true}\n",
        children >> 16, children & UINT16_MAX);
  }
  if (app->adopted_signals) {
    adopted_signals(env, cls);
#ifdef GW_TEST_NATIVE_UNWIND
    sdk_run_jni_recovery_async(env, cls, app->loader, app->module);
#endif
  }
  if (app->vm_signal_directory) {
    vm_suspend_and_sample(env, app->vm_signal_directory);
  }
  if (app->adopted_signals || app->worker_signals) {
    extern void sdk_host_guest_fds(JNIEnv *, jclass, int, int);
    sdk_host_guest_fds(env, cls, worker, app->adopted_signals);
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
  if (app->adopted_signals || app->worker_signals) {
    extern int sdk_host_held_guest_fd(JNIEnv *, jclass);
    app->held_guest_fd = sdk_host_held_guest_fd(env, cls);
  }
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

static void reclaim_fixture_action(int number) {
  (void)number;
}

static void check_live_reclaim(void) {
  struct sigaction action = {.sa_handler = reclaim_fixture_action,
                             .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGALRM);
  assert(!sigaction(48, &action, NULL));
  action.sa_handler = SIG_DFL;
  assert(!sigaction(48, &action, NULL));

  unsigned reclaimed = 0;
  int result = gw_host_signal_reclaim(1000, &reclaimed);
  if (result) {
    fprintf(stderr, "Live runtime reclaim: %s (%d)\n", strerror(result),
            result);
  }
  assert(!result && reclaimed);
}

/* argv: libjvm classpath loader library-path service module directory backend.
 * This consumer uses installed headers/libraries, with no source-tree include.
 */
int main(int argc, char **argv) {
  assert(argc == 9);
  int test_adopted_signals = !strcmp(argv[8], "adopted-signals");
  int test_vm_signals = !strcmp(argv[8], "vm-signals");
  int test_guest_signalfd =
      test_adopted_signals || !strcmp(argv[8], "worker-signals");
  if (test_guest_signalfd) {
    extern void sdk_job_control_child(void);
    sdk_job_control_child();
  }
#ifdef GW_TEST_NATIVE_UNWIND
  assert(!gw_nonlocal_init());
#endif
  assert(!gw_host_signal_bootstrap());
  if (getenv("GW_TEST_HOST_FAULT_MASK") || test_adopted_signals) {
    const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                                 SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
    assert(!gw_host_signal_channel_enable(notifications, 6));
  }
  if (test_vm_signals) {
    assert(!gw_host_signal_reclaim_enable(47));
  }
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
#ifdef __ANDROID__
  private_host_registration();
#endif
  struct gw_host_bridge_config bridge = {
      .abi_version = GW_HOST_BRIDGE_CONFIG_V2,
      .struct_size = sizeof bridge,
      .flags = GW_HOST_DIRECT_IMAGES,
      .native_directory = argv[7],
      .image_catalog = argv[7]};
  struct gw_signal_contract signals;
  assert(!gw_host_signal_contract_init(&signals));
  struct gw_host_runtime_config config = {.version = GW_HOST_RUNTIME_CONFIG_V1,
                                          .size = sizeof config,
                                          .loader_path = argv[3],
                                          .library_path = argv[4],
                                          .service_path = argv[5],
                                          .bridge = &bridge,
                                          .signals = &signals};
  int worker_signals = !strcmp(argv[8], "worker-signals");
  int worker = worker_signals || !strcmp(argv[8], "worker") ||
               !strcmp(argv[8], "worker-borrowed-relay");
  if (worker) {
    config.backend = GW_EXECUTION_WORKER;
  }
  if (worker_signals) {
    config.version = GW_HOST_RUNTIME_CONFIG_V2;
    config.guest_notification = 54;
    struct sigaction action = {.sa_handler = worker_host_signal};
    sigemptyset(&action.sa_mask);
    assert(!signal_router_action(SIGUSR1, &action, NULL));
  }
  if (test_adopted_signals) {
    config.version = GW_HOST_RUNTIME_CONFIG_V2;
    config.guest_notification = 54;
    struct sigaction action = {.sa_handler = adopted_host_jump};
    sigemptyset(&action.sa_mask);
    assert(!signal_router_action(SIGWINCH, &action, NULL));
  }

  int borrowed_relay = strstr(argv[8], "borrowed-relay") != NULL;
  assert(!signal_host_relay_running());
  if (borrowed_relay) {
    assert(!signal_host_relay_start());
  }
  retry_preclaim_failure(env, &config, borrowed_relay);

  sigset_t original_guest_signals;
  if (test_guest_signalfd) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SDK_FD_PACKET);
    sigaddset(&blocked, SDK_FD_OTHER);
    assert(!pthread_sigmask(SIG_BLOCK, &blocked, &original_guest_signals));
  }
  int result = gw_host_runtime_start(env, &config);
  if (test_guest_signalfd) {
    assert(!pthread_sigmask(SIG_SETMASK, &original_guest_signals, NULL));
  }
  if (getenv("GW_EXPECT_RUNTIME_REJECTION")) {
    assert(result == ELIBBAD || result == EPROTO);
    if (!strcmp(argv[8], "reject-service")) {
      assert(result == ELIBBAD && !guest_owner_tid());
    }
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
  long owner_tid = guest_owner_tid();
  assert(owner_tid > 0);
  if (test_vm_signals) {
    check_live_reclaim();
  }
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
      .worker_signals = worker_signals,
      .adopted_signals = test_adopted_signals,
      .nonlocal = nonlocal,
      .router_quarantine = !strcmp(argv[8], "router-quarantine"),
      .held_guest_fd = -1,
      .vm_signal_directory = test_vm_signals ? argv[7] : NULL,
      .loader = loader,
      .module = library.handle};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, application_run, &app));
  assert(!pthread_join(thread, NULL));
  if (!nonlocal) {
    assert(loader->close(env, library.handle, &error) == GW_OK);
  }
  assert(!(*env)->ExceptionCheck(env));
  if (test_guest_signalfd) {
    assert(app.held_guest_fd >= 0);
    assert(gw_host_runtime_shutdown() == EBUSY);
    assert(full_host_state() == GW_HOST_READY && signal_host_relay_running());
    /* The raw close bypasses descriptor bookkeeping. The next shutdown
     * attempt must observe that the kernel's final reference is gone. */
    assert(!syscall(SYS_close, app.held_guest_fd));
    puts(
        "{\"stage\":\"runtime-busy-descriptor\",\"retry_after_raw_close\":"
        "true}");
  }
  assert(!gw_host_runtime_shutdown());
  assert(gw_host_runtime_shutdown() == EALREADY);
  check_owner_quiescent(owner_tid);
  assert(full_host_state() == GW_HOST_STOPPED);
  assert(signal_host_relay_running() == borrowed_relay);
  if (borrowed_relay) {
    assert(!signal_host_relay_stop());
  }
  printf(
      "{\"stage\":\"runtime-relay-lifecycle\",\"preclaim_failures\":2,"
      "\"retry\":true,\"borrowed\":%s,\"shutdown_preserved_owner\":true,"
      "\"masks_preserved\":true,\"stopped_owner_masked\":true,"
      "\"stopped_owner_parked\":true}\n",
      borrowed_relay ? "true" : "false");

  assert((*vm)->DestroyJavaVM(vm) == JNI_OK);
  assert(!gw_host_signal_restore());
  printf(
      "{\"result\":\"PASS\",\"backend\":\"%s\",\"array_bytes\":%d,"
      "\"nonlocal\":%s}\n",
      worker ? "worker" : "same-thread", nonlocal ? 0 : 1048576,
      nonlocal ? "true" : "false");
  /* The Guest supervisor and idle VM bindings deliberately remain pinned. */
  fflush(NULL);
  _exit(0);
}
