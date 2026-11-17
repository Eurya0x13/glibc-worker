#define _GNU_SOURCE
#include "common.h"
#include "send-lifetime.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

static pthread_t target;
static _Atomic int target_tid, entered, may_exit, exiting, released;
/* pthread_kill is declared leaf by glibc headers. A callback reached through
 * that declaration must not rely on ordinary globals being reloaded across
 * the call. Use atomic observations, as for actual asynchronous delivery. */
static _Atomic int probes;

static void await(_Atomic int *word) {
  while (!atomic_load_explicit(word, memory_order_acquire)) {
    wait_word(word, 0);
  }
}

static void publish(_Atomic int *word) {
  atomic_store_explicit(word, 1, memory_order_release);
  wake(word);
}

static void thread_lifecycle(int starting) {
  if (!starting && syscall(SYS_gettid) == atomic_load(&target_tid)) {
    /* The patched NPTL callback runs after TLS destructors and immediately
     * before the native exit_lock transaction. No Host TLS is involved. */
    publish(&exiting);
  }
}

static void *exit_target(void *argument) {
  (void)argument;
  atomic_store_explicit(&target_tid, syscall(SYS_gettid), memory_order_release);
  wake(&target_tid);
  await(&may_exit);
  return &target;
}

static void await_exit_lock(void) {
  char path[96];
  assert(snprintf(path, sizeof path, "/proc/self/task/%d/syscall",
                  atomic_load(&target_tid)) > 0);
  struct timespec started, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &started));
  for (;;) {
    FILE *file = fopen(path, "r");
    assert(file);
    long number = -1;
    unsigned long address = 0, operation = 0;
    int fields = fscanf(file, "%ld %lx %lx", &number, &address, &operation);
    assert(!fclose(file));
    if (fields == 3 && number == SYS_futex && address &&
        (operation & FUTEX_CMD_MASK) == FUTEX_WAIT &&
        (operation & FUTEX_PRIVATE_FLAG)) {
      return;
    }

    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - started.tv_sec < 3);
    sched_yield();
  }
}

static void *exit_controller(void *argument) {
  (void)argument;
  await(&entered);
  publish(&may_exit);
  await(&exiting);
  /* After the lifecycle hook, exit_lock is the next NPTL blocking operation.
   * Observe the target parked in its actual futex instead of assuming it had
   * time to reach the lock merely because a userspace flag was published. */
  await_exit_lock();
  assert(pthread_tryjoin_np(target, NULL) == EBUSY);
  publish(&released);
  return NULL;
}

int test_send_lifetime_probe(void *thread, int tid,
                             const siginfo_t *information, int *handled) {
  if (!thread || thread != (void *)target) {
    return 0;
  }
  assert(information->si_signo == 0 && (information->si_code == SI_TKILL ||
                                        information->si_code == SI_QUEUE));
  assert(tid == atomic_load(&target_tid) && !atomic_load(&exiting));
  ++probes;
  publish(&entered);
  await(&released);
  /* With exit_lock held by this sender, the target cannot have completed
   * kernel exit and released its TID to another thread. */
  assert(!syscall(SYS_tgkill, getpid(), tid, 0));
  *handled = 0; /* Also exercise the native fallback inside the locked path. */
  errno = ERANGE;
  return 1;
}

static void exit_race(int queued) {
  atomic_store(&target_tid, 0);
  atomic_store(&entered, 0);
  atomic_store(&may_exit, 0);
  atomic_store(&exiting, 0);
  atomic_store(&released, 0);
  assert(!pthread_create(&target, NULL, exit_target, NULL));
  await(&target_tid);
  pthread_t controller;
  assert(!pthread_create(&controller, NULL, exit_controller, NULL));

  int before = probes;
  errno = EBADF;
  assert(!(queued ? pthread_sigqueue(target, 0, (union sigval){0})
                  : pthread_kill(target, 0)));
  int error = errno, observed = atomic_load(&probes);
  if (error != EBADF || observed != before + 1) {
    fprintf(stderr, "send exit race: queued=%d errno=%d probes=%d before=%d\n",
            queued, error, observed, before);
  }
  assert(error == EBADF && observed == before + 1);
  assert(!pthread_join(controller, NULL));

  int tid = atomic_load(&target_tid);
  while (!syscall(SYS_tgkill, getpid(), tid, 0)) {
    sched_yield();
  }
  assert(errno == ESRCH);
  /* The still-joinable pthread descriptor remains valid after kernel exit.
   * Modern pthread_kill returns zero; pthread_sigqueue returns ESRCH. Neither
   * may route a new packet through the retired target's cached TID. */
  errno = EBADF;
  assert(!pthread_kill(target, 0));
  assert(pthread_sigqueue(target, 0, (union sigval){0}) == ESRCH);
  assert(errno == EBADF && probes == before + 1);
  void *result;
  assert(!pthread_join(target, &result) && result == &target);
  target = 0;
}

void test_send_lifetimes(int (*install)(unsigned, test_signal_sender),
                         int (*lifecycle)(void (*)(int)),
                         test_signal_sender sender) {
  assert(install(0, sender) == EINVAL);
  assert(install(1, NULL) == EINVAL);
  assert(!install(1, sender));
  assert(install(1, sender) == EBUSY);
  assert(!lifecycle(thread_lifecycle));
  exit_race(0);
  exit_race(1);
}
