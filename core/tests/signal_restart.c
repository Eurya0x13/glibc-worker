#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

static struct signal_slot *recipient;
static _Atomic int delivered;
static int pipes[2], deferred;
static long target_tid;

static long long now(void) {
  struct timespec time;
  assert(!clock_gettime(CLOCK_MONOTONIC, &time));
  return (long long)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void receive(int number, siginfo_t *information, void *context) {
  (void)context;
  assert(number == SIGTRAP && information->si_code == SI_QUEUE);
  assert(information->si_pid == getpid() && information->si_uid == getuid());
  assert(information->si_value.sival_int == 7731);
  atomic_store_explicit(&delivered, 1, memory_order_release);
}

static void *sender(void *unused) {
  (void)unused;
  char path[128];
  int length =
      snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", target_tid);
  assert(length > 0 && (size_t)length < sizeof path);
  int state = open(path, O_RDONLY | O_CLOEXEC);
  assert(state >= 0);
  long long deadline = now() + 5000000000LL;
  for (;;) {
    char text[256] = {0};
    ssize_t size = pread(state, text, sizeof text - 1, 0);
    assert(size > 0);
    long call;
    unsigned long argument;
    if (sscanf(text, "%ld %lx", &call, &argument) == 2 && call == SYS_read &&
        argument == (unsigned)pipes[0]) {
      break;
    }
    assert(now() < deadline);
    sched_yield();
  }
  assert(!close(state));
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = 7731;
  if (deferred) {
    /* Controlled producer fixture: publish a complete packet and its armed
     * level under the same selection lock the recipient acquires. Only then
     * send its wake. This tests an actual interrupted read, without a preceding
     * SA_RESTART handler already restarting that read on the helper's behalf.
     * It does not introduce a public cross-thread queue producer API. */
    uint64_t mask = signal_router_selection_begin();
    assert(
        !gw_signal_pending_put(&recipient->deferred->pending, &information, 0));
    recipient->deferred->armed |= UINT64_C(1) << (SIGTRAP - 1);
    int notification = recipient->deferred->notification[4];
    signal_router_selection_end(mask);
    assert(!syscall(SYS_tgkill, getpid(), target_tid, notification));
  } else {
    assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), target_tid,
                    information.si_signo, &information));
  }
  while (!atomic_load_explicit(&delivered, memory_order_acquire)) {
    assert(now() < deadline);
    sched_yield();
  }
  char byte = 'k';
  assert(write(pipes[1], &byte, 1) == 1);
  return NULL;
}

static void run_case(int use_deferred, int restart) {
  deferred = use_deferred;
  target_tid = syscall(SYS_gettid);
  if (deferred) {
    assert(!signal_router_init());
    const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                                 SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
    assert(!signal_router_defer_faults(notifications));
  }
  struct sigaction action = {
      .sa_sigaction = receive,
      .sa_flags = SA_SIGINFO | (restart ? SA_RESTART : 0)};
  sigemptyset(&action.sa_mask);
  signal_router_backend install =
      deferred ? sigaction
               : (signal_router_backend)dlsym(RTLD_NEXT, "sigaction");
  assert(install && !install(SIGTRAP, &action, NULL));
  struct exchange exchange = {.host_tid = target_tid};
  if (deferred) {
    recipient = signal_acquire(&exchange);
    assert(recipient);
  }
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, SIGTRAP);
  sigaddset(&unblocked, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL));
  assert(!pipe2(pipes, O_CLOEXEC));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, sender, NULL));
  char byte;
  errno = EDOM;
  ssize_t result = read(pipes[0], &byte, 1);
  int error = errno;
  assert(!pthread_join(thread, NULL));
  assert(atomic_load(&delivered) == 1);
  if ((restart && (result != 1 || byte != 'k' || error != EDOM)) ||
      (!restart && (result != -1 || error != EINTR))) {
    fprintf(stderr,
            "restart mismatch: deferred=%d restart=%d result=%zd errno=%d\n",
            deferred, restart, result, error);
  }
  if (restart) {
    assert(result == 1 && byte == 'k' && error == EDOM);
  } else {
    assert(result == -1 && error == EINTR);
  }
  assert(!close(pipes[0]) && !close(pipes[1]));
  if (deferred) {
    assert(!signal_prepare_release(recipient, 0));
    signal_release(recipient);
    assert(!signal_router_restore());
  }
}

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  for (int mode = 0; mode < 4; ++mode) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      run_case(mode / 2, mode & 1);
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  puts(
      "{\"result\":\"PASS\",\"native_restart\":true,\"native_eintr\":true,"
      "\"deferred_restart\":true,\"deferred_eintr\":true}");
}
