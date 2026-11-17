#define _GNU_SOURCE
#include "adopted-signals.h"
#include <jni.h>
#include <assert.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

struct guest_observation {
  struct adopted_signal_probe *probe;
  pthread_t sender, recipient;
  struct sigaction previous;
  sigset_t inherited;
  sigjmp_buf continuation;
  ucontext_t context;
  ucontext_t suspended_handler;
  void *fiber_stack;
  volatile sig_atomic_t fiber_ran;
  stack_t alternate, previous_stack;
  uintptr_t tp, stack, outer_sp;
  size_t size;
};

static __thread struct guest_observation observation;

static uintptr_t signal_sp(const ucontext_t *frame) {
#if defined(__x86_64__)
  return frame->uc_mcontext.gregs[REG_RSP];
#else
  return frame->uc_mcontext.sp;
#endif
}

static void run_on_guest_stack(void) {
  struct guest_observation *o = &observation;
  char local;
  assert((uintptr_t)&local - (uintptr_t)o->fiber_stack < 65536);
  assert(pthread_equal(pthread_self(), o->recipient));
  ++o->fiber_ran;
}

static _Noreturn void restore_saved_context(struct guest_observation *o) {
  ucontext_t fiber;
  assert(!getcontext(&fiber));
  fiber.uc_stack = (stack_t){.ss_sp = o->fiber_stack, .ss_size = 65536};
  fiber.uc_link = &o->suspended_handler;
  makecontext(&fiber, run_on_guest_stack, 0);
  assert(!swapcontext(&o->suspended_handler, &fiber) && o->fiber_ran == 1);
  assert(!fesetround(FE_UPWARD));

  /* The restore input itself is on the signal stack that will be retired. */
  ucontext_t copy = o->context;
#if defined(__x86_64__)
  copy.uc_mcontext.fpregs = &copy.__fpregs_mem;
#endif
  assert(!setcontext(&copy));
  assert(!"setcontext returned to a discarded signal scope");
  _exit(125);
}

static void signal_host(const struct guest_observation *o) {
  assert(!syscall(SYS_tgkill, getpid(), o->probe->tid, SIGWINCH));
}

static void resume_parent_signal(struct guest_observation *o) {
  if (o->probe->mode == SIGNAL_PARENT_JUMP) {
    int value = sigsetjmp(o->continuation, 1);
    if (!value) {
      signal_host(o);
    }
    assert(value == 41);
  } else {
    int rounding = fegetround();
    volatile sig_atomic_t entered = 0;
    assert(!fesetround(FE_DOWNWARD) && !getcontext(&o->context));
    if (!entered) {
      entered = 1;
      signal_host(o);
      assert(!"Parent context did not abandon the Host signal handler");
    }
    assert(o->fiber_ran == 1 && fegetround() == FE_DOWNWARD);
    assert(!fesetround(rounding));
  }
  assert(atomic_load(&o->probe->handled) == 2 && !o->probe->host_returned);
  o->probe->parent_resumed = 1;
}

static void guest_signal(int number, siginfo_t *packet, void *frame) {
  struct guest_observation *o = &observation;
  assert(o->probe && number == SIGUSR1 && packet->si_signo == SIGUSR1);
  int mode = o->probe->mode;
  int nested = packet->si_value.sival_int == 9185;
  assert(packet->si_code == SI_QUEUE &&
         (nested ? adopted_nested(mode) : packet->si_value.sival_int == 9184));
  assert(syscall(SYS_gettid) == o->probe->tid);
  assert((uintptr_t)__builtin_thread_pointer() == o->tp);
  assert(pthread_equal(pthread_self(), o->recipient));
  volatile char local = 41;
  uintptr_t low = o->stack;
  size_t size = o->size;
  if (mode == SIGNAL_NESTED_ALT) {
    low = (uintptr_t)o->alternate.ss_sp;
    size = o->alternate.ss_size;
  }
  assert(frame && (uintptr_t)&local - low < size);
  if (nested) {
    assert(signal_sp(frame) - low < size);
    assert((uintptr_t)&local < signal_sp(frame) &&
           signal_sp(frame) < o->outer_sp);
  } else {
    assert(signal_sp(frame) - o->stack < o->size);
  }
  atomic_fetch_add_explicit(&o->probe->handled, 1, memory_order_release);

  if (mode == SIGNAL_HOST_JUMP || mode == SIGNAL_HOST_CONTEXT ||
      (adopted_nested(mode) && !nested)) {
    o->outer_sp = (uintptr_t)&local;
    if (adopted_parent_target(mode)) {
      resume_parent_signal(o);
    } else {
      signal_host(o);
      assert(mode == SIGNAL_NESTED || mode == SIGNAL_NESTED_ALT);
      assert(o->probe->host_returned && atomic_load(&o->probe->handled) == 2);
    }
    assert(local == 41);
    return;
  }
  if (adopted_context(mode) || mode == SIGNAL_PARENT_CONTEXT) {
    restore_saved_context(o);
  } else if (adopted_guest_target(mode) || mode == SIGNAL_PARENT_JUMP) {
    sigjmp_buf local;
    if (!sigsetjmp(local, 1)) {
      siglongjmp(local, 1);
    }
    siglongjmp(o->continuation, 41);
  }
}

static void wait_for_host_syscall(const struct adopted_signal_probe *probe) {
  long expected = probe->mode == SIGNAL_POLL ? SYS_ppoll : SYS_clock_nanosleep;
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    char path[96];
    assert(snprintf(path, sizeof path, "/proc/self/task/%d/syscall",
                    probe->tid) > 0);
    FILE *stream = fopen(path, "r");
    assert(stream);
    long syscall_number;
    int matched = fscanf(stream, "%ld", &syscall_number) == 1 &&
                  syscall_number == expected;
    assert(!fclose(stream));
    if (matched) {
      return;
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void *send_from_native(void *argument) {
  const struct guest_observation *state = argument;
  struct adopted_signal_probe *probe = state->probe;
  if (probe->mode == SIGNAL_POLL || probe->mode == SIGNAL_SLEEP) {
    wait_for_host_syscall(probe);
  } else {
    while (!atomic_load_explicit(&probe->requested, memory_order_acquire)) {
      sched_yield();
    }
  }
  assert(!pthread_sigqueue(state->recipient, SIGUSR1,
                           (union sigval){.sival_int = 9184}));
  if (adopted_nested(probe->mode)) {
    while (atomic_load_explicit(&probe->requested, memory_order_acquire) < 2) {
      sched_yield();
    }
    assert(!pthread_sigqueue(state->recipient, SIGUSR1,
                             (union sigval){.sival_int = 9185}));
  }
  return NULL;
}

void sdk_adopted_signal_prepare(JNIEnv *env, jclass cls, jlong address) {
  (void)env;
  (void)cls;
  struct guest_observation *o = &observation;
  assert(!o->probe);
  o->probe = (void *)(uintptr_t)address;
  assert(o->probe->tid == syscall(SYS_gettid));
  o->recipient = pthread_self();
  o->tp = (uintptr_t)__builtin_thread_pointer();
  pthread_attr_t attributes;
  void *stack;
  assert(!pthread_getattr_np(o->recipient, &attributes));
  assert(!pthread_attr_getstack(&attributes, &stack, &o->size));
  o->stack = (uintptr_t)stack;
  assert(!pthread_attr_destroy(&attributes));

  struct sigaction action = {.sa_sigaction = guest_signal,
                             .sa_flags = SA_SIGINFO};
  if (adopted_nested(o->probe->mode)) {
    action.sa_flags |= SA_NODEFER;
  }
  if (o->probe->mode == SIGNAL_NESTED_ALT) {
    o->alternate = (stack_t){.ss_size = 65536 * 2};
    o->alternate.ss_sp = malloc(o->alternate.ss_size);
    assert(o->alternate.ss_sp &&
           !sigaltstack(&o->alternate, &o->previous_stack));
    action.sa_flags |= SA_ONSTACK;
  }
  if (adopted_context(o->probe->mode) ||
      o->probe->mode == SIGNAL_PARENT_CONTEXT) {
    o->fiber_stack = malloc(65536);
    o->fiber_ran = 0;
    assert(o->fiber_stack);
  }
  sigemptyset(&action.sa_mask);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  assert(!sigaction(SIGUSR1, &action, &o->previous));
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &o->inherited));
  assert(!pthread_create(&o->sender, NULL, send_from_native, o));
}

jint sdk_adopted_signal_finish(JNIEnv *env, jclass cls) {
  (void)cls;
  struct guest_observation *o = &observation;
  assert(o->probe && pthread_equal(o->recipient, pthread_self()));
  assert(!pthread_join(o->sender, NULL));
  assert(atomic_load(&o->probe->handled) ==
         (adopted_nested(o->probe->mode) ? 2 : 1));
  assert(!sigaction(SIGUSR1, &o->previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &o->inherited, NULL));
  if (o->probe->mode == SIGNAL_NESTED_ALT) {
    assert(!sigaltstack(&o->previous_stack, NULL));
    free(o->alternate.ss_sp);
  }
  if (o->fiber_stack) {
    free(o->fiber_stack);
    o->fiber_stack = NULL;
  }
  o->probe = NULL;
  return (*env)->GetVersion(env) >= JNI_VERSION_1_6 ? 73 : -1;
}

static jint guest_context_target(JNIEnv *env, jclass cls) {
  struct guest_observation *o = &observation;
  int rounding = fegetround();
  sigset_t set, original, observed;
  sigemptyset(&set);
  sigaddset(&set, SIGURG);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &original));
  assert(!fesetround(FE_DOWNWARD));
  volatile sig_atomic_t called = 0;
  assert(!getcontext(&o->context));
  if (!called) {
    called = 1;
    assert(!pthread_sigmask(SIG_BLOCK, &set, NULL));
    assert(!fesetround(FE_UPWARD));
    (*env)->GetVersion(env);
    return -42;
  }

  assert(o->fiber_ran == 1 && fegetround() == FE_DOWNWARD);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(!sigismember(&observed, SIGURG));
  assert(!pthread_sigmask(SIG_SETMASK, &original, NULL));
  assert(!fesetround(rounding));
  return sdk_adopted_signal_finish(env, cls);
}

jint sdk_adopted_signal_jump(JNIEnv *env, jclass cls, jlong address) {
  sdk_adopted_signal_prepare(env, cls, address);
  struct guest_observation *o = &observation;
  if (adopted_context(o->probe->mode)) {
    return guest_context_target(env, cls);
  }
  int save = o->probe->mode != SIGNAL_GUEST_JUMP_UNSAVED;
  int value = sigsetjmp(o->continuation, save);
  if (!value) {
    (*env)->GetVersion(env);
    return -41;
  }

  sigset_t mask;
  assert(value == 41 && !pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGUSR1) == !save);
  return sdk_adopted_signal_finish(env, cls);
}
