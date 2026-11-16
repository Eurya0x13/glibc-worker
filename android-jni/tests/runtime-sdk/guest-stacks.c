#define _GNU_SOURCE
#include "nonlocal_probe.h"
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define TEST_AUTODISARM (UINT32_C(1) << 31)

struct stack_probe {
  int mode, hits, stage;
  int use_alternate, autodisarm;
  uintptr_t self;
  char *mapping;
  size_t mapping_size;
  stack_t alternate;
  stack_t previous_stack;
  sigset_t previous_mask;
  struct sigaction previous_action;
  struct stack_probe *previous;
  sigjmp_buf target;
  ucontext_t resume, handler, resumed_caller;
};

static __thread struct stack_probe *active;
static __thread struct stack_probe abandoned;
static __thread int stack_tls = 0x7195;

static inline __attribute__((always_inline)) void trap_instruction(void) {
#if defined(__x86_64__)
  __asm__ volatile("int3" ::: "memory");
#else
  __asm__ volatile("brk #0" ::: "memory");
#endif
}

static int inside(const stack_t *stack, uintptr_t pointer) {
  return pointer - (uintptr_t)stack->ss_sp < stack->ss_size;
}

static void advance_trap(void *frame) {
#if defined(__aarch64__)
  ((ucontext_t *)frame)->uc_mcontext.pc += 4;
#else
  (void)frame;
#endif
}

static void stack_handler(int number, siginfo_t *information, void *frame) {
  struct stack_probe *state = active;
  char local;
  stack_t current;
  int away = state->mode == 44 && state->stage == 2;
  int expected_alternate = state->use_alternate && !away;
  if (number != SIGTRAP || information->si_code <= 0 || stack_tls != 0x7195 ||
      pthread_self() != (pthread_t)state->self ||
      inside(&state->alternate, (uintptr_t)&local) != expected_alternate ||
      sigaltstack(NULL, &current)) {
    _exit(147);
  }
  ++state->hits;
  if (state->autodisarm || away) {
    if (!(current.ss_flags & SS_DISABLE)) {
      _exit(148);
    }
  } else if (current.ss_sp != state->alternate.ss_sp ||
             current.ss_size != state->alternate.ss_size ||
             !!(current.ss_flags & SS_ONSTACK) != expected_alternate) {
    _exit(149);
  }
  const ucontext_t *interrupted = frame;
  if (away ? !(interrupted->uc_stack.ss_flags & SS_DISABLE)
           : interrupted->uc_stack.ss_sp != state->alternate.ss_sp) {
    _exit(150);
  }
  if (state->mode == 42 || state->mode == 45 || state->mode == 46) {
    siglongjmp(state->target, 23);
  }
  if (state->mode == 43) {
    state->stage = 3;
    setcontext(&state->resume);
    _exit(151);
  }
  if (state->mode == 44 && !away) {
    state->stage = 2;
    if (swapcontext(&state->handler, &state->resume)) {
      _exit(152);
    }
    state->stage = 3;
  }
  if (state->mode == 47 && state->hits == 1) {
    trap_instruction();
    if (state->hits != 2) {
      _exit(153);
    }
  }
  if ((state->mode == 49 || state->mode == 50 || state->mode == 52) &&
      syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
              SDK_HOST_ESCAPE_SIGNAL)) {
    _exit(156);
  }
  if (state->mode == 50 || state->mode == 52) {
    _exit(158);
  }
  advance_trap(frame);
}

static int host_roundtrip(JNIEnv *env, jclass cls) {
  if (!env) {
    return 0;
  }
  jmethodID method = (*env)->GetStaticMethodID(env, cls, "hostStack", "()I");
  if (!method || (*env)->ExceptionCheck(env)) {
    return -1;
  }
  jint result = (*env)->CallStaticIntMethod(env, cls, method);
  return (*env)->ExceptionCheck(env) || result != 73 ? -1 : 0;
}

static int stack_matches(struct stack_probe *state, int disabled) {
  stack_t current;
  if (sigaltstack(NULL, &current) ||
      !!(current.ss_flags & SS_DISABLE) != disabled ||
      (current.ss_flags & SS_ONSTACK)) {
    return 0;
  }
  return disabled || (current.ss_sp == state->alternate.ss_sp &&
                      current.ss_size == state->alternate.ss_size &&
                      current.ss_flags == state->alternate.ss_flags);
}

static int run_transfer(struct stack_probe *state, JNIEnv *env, jclass cls) {
  if (getcontext(&state->resume)) {
    return -42;
  }
  if (!state->stage) {
    state->stage = 1;
    int value = sigsetjmp(state->target, state->mode != 45);
    if (!value) {
      trap_instruction();
      if (state->mode == 42 || state->mode == 45 || state->mode == 46) {
        return -43;
      }
    } else if (value != 23) {
      return -44;
    }
  } else if (state->mode == 44 && state->stage == 2) {
    /* A native AUTODISARM handler is suspended on its own alternate stack.
     * Keep its kernel frame intact while running ordinary Guest/Host work.
     * https://man7.org/linux/man-pages/man2/sigaltstack.2.html */
    if (!stack_matches(state, 1) || host_roundtrip(env, cls) ||
        !stack_matches(state, 1)) {
      return -45;
    }
    trap_instruction();
    if (swapcontext(&state->resumed_caller, &state->handler)) {
      return -46;
    }
    return -47;
  } else if (state->mode != 43 || state->stage != 3) {
    return -48;
  }
  if (state->hits != (state->mode == 44 || state->mode == 47 ? 2 : 1)) {
    return -49;
  }
  int disabled = state->mode == 46;
  sigset_t mask;
  if (!stack_matches(state, disabled) ||
      pthread_sigmask(SIG_SETMASK, NULL, &mask) ||
      sigismember(&mask, SIGTRAP) != (state->mode == 45) ||
      host_roundtrip(env, cls) || !stack_matches(state, disabled)) {
    return -50;
  }
  return 73;
}

static void *thread_stack(void *unused) {
  (void)unused;
  const int modes[] = {40, 41, 44, 45, 46, 47};
  for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
    int result = sdk_guest_stack_probe(NULL, NULL, modes[i]);
    if (result != 73) {
      return (void *)(intptr_t)result;
    }
  }
  return (void *)(intptr_t)73;
}

static int stack_begin(struct stack_probe *state, int mode) {
  *state =
      (struct stack_probe){.mode = mode,
                           .use_alternate = mode != 41,
                           .autodisarm = mode == 44 || mode == 46 || mode == 50,
                           .self = (uintptr_t)pthread_self()};
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  state->mapping_size = 128 * 1024 + 2 * page;
  state->mapping = mmap(NULL, state->mapping_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (state->mapping == MAP_FAILED ||
      mprotect(state->mapping + page, 128 * 1024, PROT_READ | PROT_WRITE)) {
    return -52;
  }
  state->alternate =
      (stack_t){.ss_sp = state->mapping + page,
                .ss_size = 128 * 1024,
                .ss_flags = state->autodisarm ? TEST_AUTODISARM : 0};
  sigset_t enabled;
  sigemptyset(&enabled);
  sigaddset(&enabled, SIGTRAP);
  struct sigaction action = {.sa_sigaction = stack_handler,
                             .sa_flags = SA_SIGINFO};
  action.sa_flags |= state->use_alternate ? SA_ONSTACK : 0;
  action.sa_flags |= mode == 47 ? SA_NODEFER : 0;
  sigemptyset(&action.sa_mask);
  if (sigaltstack(&state->alternate, &state->previous_stack) ||
      pthread_sigmask(SIG_UNBLOCK, &enabled, &state->previous_mask) ||
      sigaction(SIGTRAP, &action, &state->previous_action)) {
    _exit(154);
  }
  state->previous = active;
  active = state;
  return 0;
}

static void stack_finish(struct stack_probe *state) {
  active = state->previous;
  if (sigaction(SIGTRAP, &state->previous_action, NULL) ||
      pthread_sigmask(SIG_SETMASK, &state->previous_mask, NULL) ||
      sigaltstack(&state->previous_stack, NULL) ||
      munmap(state->mapping, state->mapping_size)) {
    _exit(155);
  }
}

int sdk_guest_stack_probe(void *environment, void *type, int mode) {
  if (mode == 48) {
    pthread_t thread;
    void *result;
    return pthread_create(&thread, NULL, thread_stack, NULL) ||
                   pthread_join(thread, &result)
               ? -51
               : (int)(intptr_t)result;
  }
  if (mode == 51) {
    if (active != &abandoned || abandoned.hits != 1) {
      return -54;
    }
    int valid = stack_matches(&abandoned, abandoned.autodisarm);
    stack_finish(&abandoned);
    return valid ? 73 : -55;
  }
  if (mode == 50 || mode == 52) {
    if (stack_begin(&abandoned, mode)) {
      return -56;
    }
    trap_instruction();
    return -57;
  }
  struct stack_probe state;
  int error = stack_begin(&state, mode);
  if (error) {
    return error;
  }
  int result = host_roundtrip(environment, type) || !stack_matches(&state, 0)
                   ? -53
                   : run_transfer(&state, environment, type);
  stack_finish(&state);
  return result;
}
