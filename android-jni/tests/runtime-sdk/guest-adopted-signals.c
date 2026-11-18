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
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

struct guest_observation {
  struct adopted_signal_probe *probe;
  pthread_t sender, recipient;
  struct sigaction previous;
  sigset_t inherited;
  sigjmp_buf continuation;
  ucontext_t context, resumed_parent;
  ucontext_t suspended_handler, prepared_fiber, suspended_middle;
  void *fiber_stack;
  volatile sig_atomic_t fiber_ran;
  int original_rounding;
  stack_t alternate, previous_stack, inner_alternate;
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

static void edit_context_rounding(ucontext_t *frame) {
#if defined(__x86_64__)
  struct _libc_fpstate *fp = frame->uc_mcontext.fpregs;
  assert(fp);
  fp->cwd = (fp->cwd & ~FE_TOWARDZERO) | FE_DOWNWARD;
  fp->mxcsr = (fp->mxcsr & ~(FE_TOWARDZERO << 3)) | (FE_DOWNWARD << 3);
#else
  /* GNU getcontext places the FPSIMD extension first in its saved record.
   * https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/aarch64/getcontext.S
   */
  struct fpsimd_context *fp = (void *)frame->uc_mcontext.__reserved;
  assert(fp->head.magic == FPSIMD_MAGIC && fp->head.size == sizeof *fp);
  fp->fpcr = (fp->fpcr & ~FE_TOWARDZERO) | FE_DOWNWARD;
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
  assert(!swapcontext(&o->suspended_handler, &o->prepared_fiber));
  assert(o->fiber_ran == 1);
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

/* No newer ordinary-stack frame lives below the outer getcontext capture
 * when the kernel lands on the AUTODISARM alternate stack. */
static inline __attribute__((always_inline)) long send_host_in_frame(long pid,
                                                                     long tid) {
#if defined(__x86_64__)
  long result = SYS_tgkill;
  __asm__ volatile("syscall"
                   : "+a"(result)
                   : "D"(pid), "S"(tid), "d"((long)SIGWINCH)
                   : "rcx", "r11", "memory");
  return result;
#else
  register long result __asm__("x0") = pid;
  register long thread __asm__("x1") = tid;
  register long number __asm__("x2") = SIGWINCH;
  register long syscall_number __asm__("x8") = SYS_tgkill;
  __asm__ volatile("svc #0"
                   : "+r"(result)
                   : "r"(thread), "r"(number), "r"(syscall_number)
                   : "memory", "cc");
  return result;
#endif
}

static void check_swap_state(int rounding, int blocked) {
  stack_t current;
  sigset_t mask;
  assert(!sigaltstack(NULL, &current) && (current.ss_flags & SS_DISABLE));
  assert(fegetround() == rounding);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGURG) == blocked);
}

static void resume_outer_swap(struct guest_observation *o) {
  struct sigaction action;
  assert(!sigaction(SIGUSR1, NULL, &action));
  action.sa_flags |= SA_ONSTACK;
  assert(!sigaction(SIGUSR1, &action, NULL));

  sigset_t mask, original;
  sigemptyset(&mask);
  sigaddset(&mask, SIGURG);
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask, &original));
  int rounding = fegetround();
  assert(!fesetround(FE_DOWNWARD));
  long pid = getpid(), tid = o->probe->tid;
  volatile unsigned canary = 0x71379u;
  assert(!getcontext(&o->context));
  if (!o->probe->swap_phase) {
    assert(!send_host_in_frame(pid, tid));
    assert(!adopted_retires_saved(o->probe->mode));
    assert(o->probe->swap_phase == 2 * SIGNAL_SWAP_ROUNDS + 1);
    assert(o->probe->host_returned && canary == 0x71379u);
    stack_t stack;
    assert(!sigaltstack(NULL, &stack) && stack.ss_sp == o->alternate.ss_sp &&
           stack.ss_flags == o->alternate.ss_flags);
    assert(!pthread_sigmask(SIG_SETMASK, &original, NULL));
    assert(!fesetround(rounding));
    return;
  }

  if (adopted_retires_saved(o->probe->mode)) {
    assert(o->probe->swap_phase == 1 && !o->probe->host_returned);
    assert(canary == 0x71379u);
    check_swap_state(FE_DOWNWARD, 0);
    if (o->probe->mode == SIGNAL_RETIRE_HOST_JUMP) {
      signal_host(o);
      assert(!"Host escape returned to the older Guest handler");
    }
    return;
  }

  for (int round = 0; round < SIGNAL_SWAP_ROUNDS; ++round) {
    assert(o->probe->swap_phase == 2 * round + 1 && !o->probe->host_returned);
    assert(canary == 0x71379u);
    check_swap_state(FE_DOWNWARD, 0);
    signal_host(o); /* Runs a distinct Host callback below the retained one. */
    o->probe->swap_phase = 2 * round + 2;
    if (round + 1 == SIGNAL_SWAP_ROUNDS) {
      setcontext(&o->suspended_handler);
      _exit(125);
    }
    assert(!swapcontext(&o->resumed_parent, &o->suspended_handler));
  }
}

static void resume_inner_swap(struct guest_observation *o) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGURG);
  assert(!pthread_sigmask(SIG_BLOCK, &mask, NULL));
  assert(!fesetround(FE_UPWARD));
  volatile unsigned canary = 0x91451u;
  for (int round = 0; round < SIGNAL_SWAP_ROUNDS; ++round) {
    o->probe->swap_phase = 2 * round + 1;
    const ucontext_t *outer = round ? &o->resumed_parent : &o->context;
    assert(!swapcontext(&o->suspended_handler, outer));
    assert(!adopted_retires_saved(o->probe->mode));
    assert(o->probe->swap_phase == 2 * round + 2 && canary == 0x91451u);
    assert(!o->probe->host_returned);
    check_swap_state(FE_UPWARD, 1);
  }
  o->probe->swap_phase = 2 * SIGNAL_SWAP_ROUNDS + 1;
}

static void try_overlapping_swap(struct guest_observation *o, int nested) {
  if (!nested) {
    volatile sig_atomic_t entered = 0;
    assert(!getcontext(&o->context) && !entered);
    entered = 1;
    signal_host(o);
    assert(o->probe->overlap_rejected && o->probe->host_returned);
    return;
  }

  int rounding = fegetround();
  sigset_t mask, old_mask, observed;
  sigemptyset(&mask);
  sigaddset(&mask, SIGURG);
  assert(!pthread_sigmask(SIG_BLOCK, &mask, &old_mask));
  assert(!fesetround(FE_UPWARD));

#if defined(__x86_64__)
  register uintptr_t canary __asm__("r12") = 0x71379u;
  __asm__ volatile("" : "+r"(canary));
#endif
  int error = swapcontext(&o->suspended_handler, &o->context);
#if defined(__x86_64__)
  __asm__ volatile("" : "+r"(canary));
  assert(canary == 0x71379u);
#endif
  assert(error == -1 && errno == EINVAL);
  assert(fegetround() == FE_UPWARD);
  assert(!pthread_sigmask(SIG_SETMASK, &old_mask, &observed));
  assert(sigismember(&observed, SIGURG) == 1);
  assert(!fesetround(rounding));
  o->probe->overlap_rejected = 1;
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
  int layer = packet->si_value.sival_int - 9184;
  int nested = layer > 0;
  assert(packet->si_code == SI_QUEUE && layer >= 0 &&
         layer < adopted_activations(mode));
  assert(syscall(SYS_gettid) == o->probe->tid);
  assert((uintptr_t)__builtin_thread_pointer() == o->tp);
  assert(pthread_equal(pthread_self(), o->recipient));
  volatile char local = 41;
  uintptr_t low = o->stack;
  size_t size = o->size;
  if (mode == SIGNAL_NESTED_ALT || (adopted_scope_swap(mode) && nested)) {
    const stack_t *stack = layer == 2 ? &o->inner_alternate : &o->alternate;
    low = (uintptr_t)stack->ss_sp;
    size = stack->ss_size;
  }
  assert(frame && (uintptr_t)&local - low < size);
  if (nested && !adopted_scope_swap(mode)) {
    assert(signal_sp(frame) - low < size);
    assert((uintptr_t)&local < signal_sp(frame) &&
           signal_sp(frame) < o->outer_sp);
  } else if (layer == 2) {
    assert(signal_sp(frame) - (uintptr_t)o->alternate.ss_sp <
           o->alternate.ss_size);
  } else {
    assert(signal_sp(frame) - o->stack < o->size);
  }
  if (mode == SIGNAL_CONTEXT_EDIT) {
    assert(fegetround() == FE_TONEAREST);
    edit_context_rounding(frame);
  }
  atomic_fetch_add_explicit(&o->probe->handled, 1, memory_order_release);

  if (mode == SIGNAL_PARENT_SWAP_OVERLAP) {
    if (!nested) {
      o->outer_sp = (uintptr_t)&local;
    }
    try_overlapping_swap(o, nested);
    assert(local == 41);
    return;
  }

  if (mode == SIGNAL_RETIRE_CHAIN && layer == 1) {
    assert(!sigaltstack(&o->inner_alternate, NULL));
    assert(!getcontext(&o->suspended_middle));
    signal_host(o);
    assert(!"Middle Guest activation resumed after retirement");
  }

  if (mode == SIGNAL_HOST_JUMP || mode == SIGNAL_HOST_CONTEXT ||
      (adopted_nested(mode) && !nested)) {
    o->outer_sp = (uintptr_t)&local;
    if (adopted_scope_swap(mode)) {
      resume_outer_swap(o);
    } else if (adopted_parent_target(mode)) {
      resume_parent_signal(o);
    } else {
      signal_host(o);
      assert(mode == SIGNAL_NESTED || mode == SIGNAL_NESTED_ALT);
      assert(o->probe->host_returned && atomic_load(&o->probe->handled) == 2);
    }
    assert(local == 41);
    return;
  }
  if (adopted_scope_swap(mode)) {
    resume_inner_swap(o);
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
  if (probe->mode == SIGNAL_POLL || probe->mode == SIGNAL_SLEEP ||
      probe->mode == SIGNAL_SLEEP_ABSOLUTE) {
    wait_for_host_syscall(probe);
  } else {
    while (!atomic_load_explicit(&probe->requested, memory_order_acquire)) {
      sched_yield();
    }
  }
  assert(!pthread_sigqueue(state->recipient, SIGUSR1,
                           (union sigval){.sival_int = 9184}));
  for (int layer = 1; layer < adopted_activations(probe->mode); ++layer) {
    while (atomic_load_explicit(&probe->requested, memory_order_acquire) <=
           layer) {
      sched_yield();
    }
    assert(!pthread_sigqueue(state->recipient, SIGUSR1,
                             (union sigval){.sival_int = 9184 + layer}));
  }
  return NULL;
}

void sdk_adopted_signal_prepare(JNIEnv *env, jclass cls, jlong address) {
  (void)env;
  (void)cls;
  struct guest_observation *o = &observation;
  assert(!o->probe);
  o->probe = (void *)(uintptr_t)address;
  if (o->probe->mode == SIGNAL_JAVA) {
    extern void sdk_guest_signalfd_probe(void);
    sdk_guest_signalfd_probe();
  }
  assert(o->probe->tid == syscall(SYS_gettid));
  o->recipient = pthread_self();
  o->tp = (uintptr_t)__builtin_thread_pointer();
  if (o->probe->mode == SIGNAL_CONTEXT_EDIT) {
    o->original_rounding = fegetround();
    assert(!fesetround(FE_TONEAREST));
  }
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
  if (o->probe->mode == SIGNAL_NESTED_ALT ||
      adopted_scope_swap(o->probe->mode)) {
    o->alternate = (stack_t){.ss_size = 65536 * 2};
    if (adopted_scope_swap(o->probe->mode)) {
      o->alternate.ss_flags = (int)(UINT32_C(1) << 31); /* SS_AUTODISARM */
    } else {
      action.sa_flags |= SA_ONSTACK;
    }
    o->alternate.ss_sp = malloc(o->alternate.ss_size);
    assert(o->alternate.ss_sp &&
           !sigaltstack(&o->alternate, &o->previous_stack));
  }
  if (o->probe->mode == SIGNAL_RETIRE_CHAIN) {
    o->inner_alternate = o->alternate;
    o->inner_alternate.ss_sp = malloc(o->inner_alternate.ss_size);
    assert(o->inner_alternate.ss_sp);
  }
  if (adopted_context(o->probe->mode) ||
      o->probe->mode == SIGNAL_PARENT_CONTEXT) {
    o->fiber_stack = malloc(65536);
    o->fiber_ran = 0;
    assert(o->fiber_stack);
    assert(!getcontext(&o->prepared_fiber));
    o->prepared_fiber.uc_stack =
        (stack_t){.ss_sp = o->fiber_stack, .ss_size = 65536};
    o->prepared_fiber.uc_link = &o->suspended_handler;
    makecontext(&o->prepared_fiber, run_on_guest_stack, 0);
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
  if (o->probe->mode == SIGNAL_CONTEXT_EDIT) {
    assert(fegetround() == FE_DOWNWARD && !fesetround(o->original_rounding));
  }
  assert(!pthread_join(o->sender, NULL));
  assert(atomic_load(&o->probe->handled) ==
         adopted_activations(o->probe->mode));
  if (adopted_retires_saved(o->probe->mode)) {
    assert(setcontext(&o->suspended_handler) == -1 && errno == ENOTSUP);
    ++o->probe->stale_rejections;
  }
  if (o->probe->mode == SIGNAL_RETIRE_CHAIN) {
    assert(setcontext(&o->suspended_middle) == -1 && errno == ENOTSUP);
    ++o->probe->stale_rejections;
  }
  assert(!sigaction(SIGUSR1, &o->previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &o->inherited, NULL));
  if (o->probe->mode == SIGNAL_NESTED_ALT ||
      adopted_scope_swap(o->probe->mode)) {
    assert(!sigaltstack(&o->previous_stack, NULL));
    free(o->alternate.ss_sp);
  }
  if (o->probe->mode == SIGNAL_RETIRE_CHAIN) {
    free(o->inner_alternate.ss_sp);
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
