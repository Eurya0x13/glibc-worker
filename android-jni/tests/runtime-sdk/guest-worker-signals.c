#define _GNU_SOURCE
#include <jni.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

enum { RT_PACKET = 56, WORKER_CHILDREN = 32, CANCELLED_CHILDREN = 64 };

static __thread uintptr_t alternate;
static __thread volatile sig_atomic_t handled;
static __thread int worker_tid;

static void guest_handler(int number, siginfo_t *packet, void *frame) {
  int saved = errno;
  assert(number == SIGUSR1 && packet->si_signo == SIGUSR1);
  assert(packet->si_code == SI_TKILL && packet->si_pid == getpid());
  assert(frame && syscall(SYS_gettid) == worker_tid);
  char local;
  assert((uintptr_t)&local - alternate < 65536);
  stack_t stack;
  sigset_t mask;
  assert(!sigaltstack(NULL, &stack) && stack.ss_flags == SS_ONSTACK);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGUSR1));
  assert(((ucontext_t *)frame)->uc_stack.ss_sp == (void *)alternate);
  ++handled;
  errno = saved;
}

static void check_one_shot(void) {
  alternate = (uintptr_t)malloc(65536);
  assert(alternate);
  stack_t stack = {.ss_sp = (void *)alternate, .ss_size = 65536};
  stack_t previous_stack;
  assert(!sigaltstack(&stack, &previous_stack));
  struct sigaction action = {
      .sa_sigaction = guest_handler,
      .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, &previous));
  assert(previous.sa_handler == SIG_DFL);

  sigset_t set, inherited;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &inherited));
  worker_tid = (int)syscall(SYS_gettid);
  handled = 0;
  errno = EDOM;
  int sent = raise(SIGUSR1), send_error = errno;
  if (sent || handled != 1 || send_error != EDOM) {
    uint64_t kernel = 0, native_pending = 0;
    syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &kernel, sizeof kernel);
    syscall(SYS_rt_sigpending, &native_pending, sizeof native_pending);
    fprintf(stderr,
            "worker raise result=%d errno=%d handled=%d mask=%llx "
            "pending=%llx\n",
            sent, send_error, (int)handled, (unsigned long long)kernel,
            (unsigned long long)native_pending);
  }
  assert(!sent && handled == 1 && send_error == EDOM);
  assert(!sigaction(SIGUSR1, NULL, &action) && action.sa_handler == SIG_DFL);

  /* The same number from the kernel belongs to Host. Its actual handler
   * runs on the dedicated Host dispatcher and completes before this call. */
  assert(!syscall(SYS_tgkill, getpid(), worker_tid, SIGUSR1));
  assert(handled == 1);
  assert(!sigaction(SIGUSR1, &previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!sigaltstack(&previous_stack, NULL));
  free((void *)alternate);
  alternate = 0;
}

static sigset_t wait_set(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, RT_PACKET);
  return set;
}

static void *waiting_child(void *argument) {
  sigset_t set = wait_set(), actual;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &actual));
  assert(sigismember(&actual, RT_PACKET));
  pid_t tid = (pid_t)syscall(SYS_gettid);
  assert(tid != (pid_t)(intptr_t)argument);
  siginfo_t packet;
  assert(sigwaitinfo(&set, &packet) == RT_PACKET && packet.si_code == SI_QUEUE);
  assert(packet.si_pid == getpid() && packet.si_uid == getuid());
  return (void *)(intptr_t)packet.si_value.sival_int;
}

static void *cancelled_child(void *argument) {
  (void)argument;
  sigset_t set = wait_set();
  sigwaitinfo(&set, NULL);
  return NULL;
}

static void check_child_admission(void) {
  sigset_t set = wait_set(), previous;
  assert(!pthread_sigmask(SIG_BLOCK, &set, &previous));
  struct sigaction ignored = {.sa_handler = SIG_IGN}, old;
  sigemptyset(&ignored.sa_mask);
  assert(!sigaction(RT_PACKET, &ignored, &old));
  for (int i = 0; i < WORKER_CHILDREN; ++i) {
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, waiting_child,
                           (void *)(intptr_t)worker_tid));
    union sigval value = {.sival_int = 3700 + i};
    assert(!pthread_sigqueue(thread, RT_PACKET, value));
    void *result;
    assert(!pthread_join(thread, &result) &&
           (intptr_t)result == value.sival_int);
  }
  for (int i = 0; i < CANCELLED_CHILDREN; ++i) {
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, cancelled_child, NULL));
    assert(!pthread_cancel(thread));
    void *result;
    assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  }

  assert(!pthread_kill(pthread_self(), RT_PACKET));
  sigset_t pending;
  int consumed = 0;
  assert(!sigpending(&pending) && sigismember(&pending, RT_PACKET));
  assert(!sigwait(&set, &consumed) && consumed == RT_PACKET);
  assert(!sigpending(&pending) && !sigismember(&pending, RT_PACKET));
  assert(!sigaction(RT_PACKET, &old, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
}

jint sdk_worker_signal_probe(JNIEnv *env, jclass cls) {
  (void)cls;
  check_one_shot();
  check_child_admission();

  assert(raise(SIGUSR2) == -1 && errno == EBUSY);
  return (*env)->GetVersion(env) >= JNI_VERSION_1_6
             ? (WORKER_CHILDREN << 16) | CANCELLED_CHILDREN
             : -1;
}
