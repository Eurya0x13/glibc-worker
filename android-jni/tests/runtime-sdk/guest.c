#define _GNU_SOURCE
#include <jni.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <setjmp.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "nonlocal_probe.h"

static __thread int calls;
static __thread int nonlocal_calls;
static __thread int unreserved_canary;

struct context_probe {
  ucontext_t caller, child;
  int step, failures;
};
static __thread struct context_probe *running_context;

static int reserved_is_blocked(void) {
  sigset_t observed;
  return pthread_sigmask(SIG_SETMASK, NULL, &observed)
             ? -1
             : sigismember(&observed, SDK_HOST_ESCAPE_SIGNAL);
}

static void context_child(void) {
  struct context_probe *state = running_context;
  sigset_t signal;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  state->failures += reserved_is_blocked() != 0;
  state->failures += pthread_sigmask(SIG_BLOCK, &signal, NULL) != 0;
  state->step = 1;
  if (swapcontext(&state->child, &state->caller)) {
    _exit(127);
  }
  state->failures += reserved_is_blocked() != 1;
  state->failures += syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                             SDK_HOST_ESCAPE_SIGNAL) != 0;
  state->step = 2;
}

static jint context_switch_probe(void) {
  struct context_probe state = {0};
  struct context_probe *previous_context = running_context;
  sigset_t signal, previous;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  if (pthread_sigmask(SIG_BLOCK, &signal, &previous) ||
      getcontext(&state.child)) {
    return -23;
  }
  const size_t size = 256 * 1024;
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *stack = mmap(NULL, size + 2 * page, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    return -24;
  }
  if (mprotect(stack + page, size, PROT_READ | PROT_WRITE)) {
    munmap(stack, size + 2 * page);
    return -25;
  }
  state.child.uc_stack = (stack_t){.ss_sp = stack + page, .ss_size = size};
  state.child.uc_link = &state.caller;
  sigdelset(&state.child.uc_sigmask, SDK_HOST_ESCAPE_SIGNAL);
  running_context = &state;
  makecontext(&state.child, context_child, 0);
  if (swapcontext(&state.caller, &state.child)) {
    _exit(127);
  }
  state.failures += state.step != 1 || reserved_is_blocked() != 1;
  state.failures += pthread_sigmask(SIG_UNBLOCK, &signal, NULL) != 0;
  if (swapcontext(&state.caller, &state.child)) {
    _exit(127);
  }
  state.failures += state.step != 2 || reserved_is_blocked() != 0;
  running_context = previous_context;
  state.failures += pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0;
  state.failures += munmap(stack, size + 2 * page) != 0;
  return state.failures ? -26 : 73;
}

static jint reserved_mask_probe(int mode) {
  sigset_t signal, previous, observed;
  sigemptyset(&signal);
  sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
  int status = mode == 17 ? sigprocmask(SIG_BLOCK, &signal, &previous)
                          : pthread_sigmask(SIG_BLOCK, &signal, &previous);
  if (status) {
    return -17;
  }
  if (mode == 18) {
    sigjmp_buf target;
    if (!sigsetjmp(target, 1)) {
      if (pthread_sigmask(SIG_UNBLOCK, &signal, NULL)) {
        return -18;
      }
      siglongjmp(target, 1);
    }
  }
  if (mode == 19) {
    ucontext_t target;
    volatile sig_atomic_t resumed = 0;
    if (getcontext(&target)) {
      return -20;
    }
    if (!resumed) {
      resumed = 1;
      if (pthread_sigmask(SIG_UNBLOCK, &signal, NULL) || setcontext(&target)) {
        return -21;
      }
      return -22;
    }
  }
  int sent = syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                     SDK_HOST_ESCAPE_SIGNAL);
  status = pthread_sigmask(SIG_SETMASK, NULL, &observed);
  int blocked = sigismember(&observed, SDK_HOST_ESCAPE_SIGNAL);
  int restored = pthread_sigmask(SIG_SETMASK, &previous, NULL);
  if (sent || status || restored || blocked != 1) {
    dprintf(2, "RESERVED_MASK mode=%d blocked=%d sent=%d query=%d restore=%d\n",
            mode, blocked, sent, status, restored);
  }
  return !sent && !status && !restored && blocked == 1 ? 73 : -19;
}

/* Invoked directly through the installed loader's JNI trampoline. The Host
 * checkpoint is outside this native entry, without Java frames in between. */
static __thread sigjmp_buf cross_targets[300];
static __thread int cross_count;
static __thread int cross_checked;
_Noreturn void sdk_guest_checked_jump(void *, int);

static jint cross_guest_target(JNIEnv *env, int mode) {
  sigset_t one, inherited, observed;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR1);
  if (pthread_sigmask(SIG_UNBLOCK, &one, &inherited)) {
    return -90;
  }
  int saved = mode == 90 || mode == 94 || mode == 96;
  cross_checked = mode == 96;
  cross_count = mode == 95 ? 300 : 1;
  for (volatile int i = 0; i < cross_count; ++i) {
    int value = sigsetjmp(cross_targets[i], saved);
    if (value) {
      int ok = !pthread_sigmask(SIG_SETMASK, NULL, &observed) &&
               sigismember(&observed, SIGUSR1) == !saved && value == 37;
      return !pthread_sigmask(SIG_SETMASK, &inherited, NULL) && ok ? 73 : -91;
    }
  }
  if (pthread_sigmask(SIG_BLOCK, &one, NULL)) {
    return -92;
  }
  (*env)->GetVersion(env);
  return -93; /* The Host C callback must be abandoned, not returned. */
}

JNIEXPORT jint JNICALL sdk_nonlocal_probe(JNIEnv *env, jclass cls, jint mode) {
  (void)env;
  (void)cls;
  if (mode >= 97 && mode <= 100) {
    return sdk_guest_signal_unwind_probe(mode);
  }
  if (mode == 87 || mode == 88) {
    return sdk_guest_fault_probe(mode);
  }
  if (mode == 90 || mode == 91 || mode == 94 || mode == 95 || mode == 96) {
    return cross_guest_target(env, mode);
  }
  if (mode == 92) {
    if (cross_checked) {
      sdk_guest_checked_jump(cross_targets[cross_count - 1], 37);
    }
    siglongjmp(cross_targets[cross_count - 1], 37);
  }
  if (mode == 93) {
    (*env)->GetVersion(env);
    return -94;
  }
  if (mode == 81) {
    return sdk_guest_registration_probe();
  }
  if (mode == 85) {
    siginfo_t information = {.si_signo = SIGSEGV, .si_code = SI_QUEUE};
    information.si_pid = getpid();
    information.si_uid = getuid();
    information.si_value.sival_int = 0x7193;
    return syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                   SIGSEGV, &information)
               ? -85
               : 73;
  }
  if (mode == 86) {
    sigset_t signal, inherited;
    sigemptyset(&signal);
    sigaddset(&signal, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &signal, &inherited)) {
      return -86;
    }
    unreserved_canary = 0x7193;
    siginfo_t information = {.si_signo = SIGUSR1, .si_code = SI_QUEUE};
    information.si_pid = getpid();
    information.si_uid = getuid();
    information.si_value.sival_int = 0x7193;
    errno = EDOM;
    int result = syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                         SIGUSR1, &information);
    int intact = unreserved_canary == 0x7193 && errno == EDOM;
    int restored = pthread_sigmask(SIG_SETMASK, &inherited, NULL);
    return !result && intact && !restored ? 73 : -86;
  }
  if (mode == 82 || mode == 83 || mode == 84) {
    return sdk_guest_fault_probe(mode);
  }
  if (mode >= 60 && mode <= 80) {
    return sdk_guest_wait_probe(mode);
  }
  if (mode >= 22 && mode <= 37) {
    return sdk_guest_fault_probe(mode);
  }
  if (mode >= 40 && mode <= 52) {
    return sdk_guest_stack_probe(env, cls, mode);
  }
  if (mode == 53 || mode == 54) {
    return syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                   mode == 53 ? 50 : 51)
               ? -58
               : 73;
  }
  if (mode == 20) {
    return context_switch_probe();
  }
  if (mode == 3) {
    return nonlocal_calls;
  }
  if (mode == 5) {
    return fegetround() == FE_DOWNWARD;
  }
  if (mode == 6) {
    return fesetround(FE_TONEAREST);
  }
  if (mode >= 16 && mode <= 19) {
    return reserved_mask_probe(mode);
  }
  if (mode == 8) {
    sigset_t current;
    return pthread_sigmask(SIG_SETMASK, NULL, &current)
               ? -9
               : sigismember(&current, SIGUSR1);
  }
  if (mode == 9 || mode == 10) {
    sigset_t signal;
    sigemptyset(&signal);
    sigaddset(&signal, SIGUSR1);
    return pthread_sigmask(mode == 9 ? SIG_UNBLOCK : SIG_BLOCK, &signal, NULL);
  }
  if (mode == 15 || mode == 21) {
    sigset_t signal, previous;
    sigemptyset(&signal);
    sigaddset(&signal, SDK_HOST_ESCAPE_SIGNAL);
    if (pthread_sigmask(SIG_UNBLOCK, &signal, &previous)) {
      return -15;
    }
    int sent = 0;
    if (mode == 21) {
      for (int value = 101; value <= 103; ++value) {
        siginfo_t info = {0};
        info.si_signo = SDK_HOST_ESCAPE_SIGNAL;
        info.si_code = SI_QUEUE;
        info.si_pid = getpid();
        info.si_uid = getuid();
        info.si_value.sival_int = value;
        sent |= syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                        SDK_HOST_ESCAPE_SIGNAL, &info);
      }
    } else {
      sent = syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                     SDK_HOST_ESCAPE_SIGNAL);
    }
    int restored = pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return !sent && !restored ? 73 : -16;
  }
  if (mode == 11 || mode == 12 || mode == 14) {
    sigset_t signal, previous, current;
    sigemptyset(&signal);
    sigaddset(&signal, SIGUSR1);
    if (pthread_sigmask(mode == 14 ? SIG_UNBLOCK : SIG_BLOCK, &signal,
                        &previous)) {
      return -10;
    }
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SDK_HOST_ESCAPE_SIGNAL);
    if (mode == 12 || mode == 14) {
      int status = pthread_sigmask(SIG_SETMASK, NULL, &current);
      int blocked = sigismember(&current, SIGUSR1);
      int restored = pthread_sigmask(SIG_SETMASK, &previous, NULL);
      return !status && blocked == (mode == 12) && !restored ? 73 : -12;
    }
    return -11;
  }
  if (mode == 7) {
    sigset_t signal, previous, observed;
    sigemptyset(&signal);
    sigaddset(&signal, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &signal, &previous)) {
      return -7;
    }
    jmethodID callback = (*env)->GetStaticMethodID(env, cls, "hostMask", "()I");
    jint host_blocked =
        callback ? (*env)->CallStaticIntMethod(env, cls, callback) : -1;
    int status = pthread_sigmask(SIG_SETMASK, NULL, &observed);
    int guest_blocked = sigismember(&observed, SIGUSR1);
    int restored = pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (status || restored || host_blocked != 0 || guest_blocked != 0) {
      dprintf(2, "MASK_ISOLATION host=%d guest=%d query=%d restore=%d\n",
              host_blocked, guest_blocked, status, restored);
    }
    return !status && !restored && host_blocked == 0 && guest_blocked == 0 ? 73
                                                                           : -8;
  }
  if (mode == 13) {
    sigset_t current;
    int status = pthread_sigmask(SIG_SETMASK, NULL, &current);
    int before = sigismember(&current, SIGUSR1);
    jmethodID callback =
        (*env)->GetStaticMethodID(env, cls, "hostMaskBlocked", "()I");
    int host_blocked =
        callback ? (*env)->CallStaticIntMethod(env, cls, callback) : -1;
    status |= pthread_sigmask(SIG_SETMASK, NULL, &current);
    int after = sigismember(&current, SIGUSR1);
    return !status && before == 0 && after == 0 && host_blocked == 1 ? 73 : -13;
  }
  ++nonlocal_calls;
  if (mode == 1 || mode == 4) {
    if (mode == 4 && fesetround(FE_DOWNWARD)) {
      return -4;
    }
    /* Untagged tgkill is Host-owned by the domain routing contract. */
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SDK_HOST_ESCAPE_SIGNAL);
    return -1; /* A successful nonlocal transfer must not return here. */
  }
  if (mode == 2) {
    jmethodID nested = (*env)->GetStaticMethodID(env, cls, "hostNested", "()I");
    if (!nested) {
      return -2;
    }
    jint result = (*env)->CallStaticIntMethod(env, cls, nested);
    return (*env)->ExceptionCheck(env) ? -3 : result;
  }
  return 73;
}

static jlong tid(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return syscall(SYS_gettid);
}

static jint probe(JNIEnv *env, jclass cls, jint value, jbyteArray data) {
  errno = EDOM;
  ++calls;
  jmethodID reply = (*env)->GetStaticMethodID(env, cls, "reply", "(I)I");
  if (!reply) {
    return -1;
  }
  jint result = (*env)->CallStaticIntMethod(env, cls, reply, value);
  if ((*env)->ExceptionCheck(env)) {
    return -1;
  }
  jsize size = (*env)->GetArrayLength(env, data);
  jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
  if (!bytes) {
    return -1;
  }
  bytes[size - 1] = (jbyte)calls;
  (*env)->ReleaseByteArrayElements(env, data, bytes, 0);
  return !(*env)->ExceptionCheck(env) && errno == EDOM ? result : -1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6)) {
    return JNI_ERR;
  }
  /* Lifecycle calls have no native-method checkpoint to unwind. */
  if ((*env)->GetVersion(env) < JNI_VERSION_1_6) {
    return JNI_ERR;
  }
  jclass cls = (*env)->FindClass(env, "SdkProbe");
  if (!cls) {
    return JNI_ERR;
  }
  extern jint sdk_worker_signal_probe(JNIEnv *, jclass);
  JNINativeMethod methods[] = {
      {"probe", "(I[B)I", (void *)probe},
      {"tid", "()J", (void *)tid},
      {"workerSignals", "()I", (void *)sdk_worker_signal_probe}};
  int result = (*env)->RegisterNatives(env, cls, methods,
                                       sizeof methods / sizeof *methods);
  (*env)->DeleteLocalRef(env, cls);
  return result ? JNI_ERR : JNI_VERSION_1_6;
}
