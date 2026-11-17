#define _GNU_SOURCE
#include "signal-targets.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Atomic int notified;
static int wake_failure;
static struct gw_signal_target *last_target;

static int notify(void *context, struct gw_signal_target *target) {
  assert(context == &notified && target->tid > 0);
  last_target = target;
  atomic_fetch_add(&notified, 1);
  return wake_failure;
}

static siginfo_t packet(int value) {
  siginfo_t info = {.si_signo = 56, .si_code = SI_QUEUE};
  info.si_pid = getpid();
  info.si_uid = getuid();
  info.si_value.sival_int = value;
  return info;
}

static void consume(struct gw_signal_target *target, int value,
                    enum gw_signal_queue_source expected) {
  siginfo_t observed;
  enum gw_signal_queue_source source;
  assert(gw_signal_target_deliver(target, &observed, &source) == 56);
  assert(observed.si_code == SI_QUEUE && observed.si_pid == getpid() &&
         observed.si_uid == getuid() && observed.si_value.sival_int == value);
  assert(source == expected);
}

static void set_mask(struct gw_signal_target *target, uint64_t mask) {
  struct gw_signal_publication result = gw_signal_target_mask(target, mask);
  assert(!result.status && !result.wake_error);
}

static void identity_and_growth(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, 0, 4096, notify, &notified);

  enum { COUNT = 2048 };
  struct gw_signal_target *targets[COUNT];
  for (unsigned i = 0; i < COUNT; ++i) {
    assert(!gw_signal_target_open(&registry, 64 * i + 1, i + 1, 0, NULL,
                                  &targets[i]));
    assert(!gw_signal_target_pin(targets[i]));
  }

  for (unsigned i = COUNT; i-- > 0;) {
    siginfo_t info = packet(i);
    struct gw_signal_publication result = gw_signal_targets_send(
        &registry, targets[i]->key, targets[i]->tid, &info);
    assert(!result.status && !result.wake_error && last_target == targets[i]);
    consume(targets[i], i, GW_SIGNAL_THREAD);
  }
  struct gw_signal_target *duplicate = NULL;
  assert(gw_signal_target_open(&registry, 1, 1, 0, NULL, &duplicate) ==
         EALREADY);
  assert(!duplicate);
  siginfo_t info = packet(73);
  assert(gw_signal_targets_send(&registry, 1, 99, &info).status == ESRCH);

  struct gw_signal_target *old = targets[0];
  gw_signal_target_close(old);
  assert(gw_signal_target_release(old) == EBUSY);
  assert(gw_signal_target_pin(old) == ESRCH);
  assert(!gw_signal_target_retain(old));
  assert(!gw_signal_target_open(&registry, 1, 99, 0, NULL, &targets[0]));
  assert(targets[0] != old);
  assert(!gw_signal_targets_send(&registry, 1, 99, &info).status);
  assert(!gw_signal_target_pending(old));
  consume(targets[0], 73, GW_SIGNAL_THREAD);
  assert(!gw_signal_target_unpin(old));
  assert(gw_signal_target_release(old) == EBUSY);
  assert(!gw_signal_target_unpin(old));
  assert(gw_signal_target_retain(old) == EINVAL);
  assert(gw_signal_target_mask(old, 0).status == ESRCH);
  assert(!gw_signal_target_release(old));
  assert(gw_signal_target_release(targets[0]) == EBUSY);

  gw_signal_targets_close(&registry);
  assert(gw_signal_targets_destroy(&registry) == EBUSY);
  for (unsigned i = 0; i < COUNT; ++i) {
    if (i) {
      assert(!gw_signal_target_unpin(targets[i]));
    }
    assert(!gw_signal_target_release(targets[i]));
  }
  assert(!gw_signal_targets_destroy(&registry));
}

static void shared_and_failed_wake(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, UINT64_C(1) << 49, 3, notify, &notified);
  siginfo_t info = packet(18);
  int before = atomic_load(&notified);
  assert(!gw_signal_targets_send(&registry, 0, 0, &info).status);
  assert(atomic_load(&notified) == before);
  struct gw_signal_target *a, *b;
  assert(!gw_signal_target_open(&registry, 1, 1, UINT64_MAX, NULL, &a));
  assert(atomic_load(&notified) == before);
  assert(!gw_signal_target_open(&registry, 2, 2, 0, NULL, &b));
  assert(last_target == b && atomic_load(&notified) == before + 1);
  consume(b, 18, GW_SIGNAL_PROCESS);

  wake_failure = EAGAIN;
  info = packet(19);
  struct gw_signal_publication sent =
      gw_signal_targets_send(&registry, 2, 2, &info);
  assert(!sent.status && sent.wake_error == EAGAIN);
  assert(gw_signal_targets_error(&registry) == EAGAIN);
  assert(gw_signal_target_pending(b) == (UINT64_C(1) << 55));
  wake_failure = 0;
  assert(!gw_signal_targets_retry(&registry));
  assert(!gw_signal_targets_error(&registry));
  consume(b, 19, GW_SIGNAL_THREAD);

  set_mask(b, UINT64_MAX);
  info = packet(21);
  assert(!gw_signal_targets_send(&registry, 2, 2, &info).status);
  wake_failure = EAGAIN;
  struct gw_signal_publication changed = gw_signal_target_mask(b, 0);
  assert(!changed.status && changed.wake_error == EAGAIN);
  assert(!b->receiver.blocked && gw_signal_target_pending(b));
  wake_failure = 0;
  assert(!gw_signal_targets_retry(&registry));
  consume(b, 21, GW_SIGNAL_THREAD);

  set_mask(b, UINT64_MAX);
  before = atomic_load(&notified);
  info = packet(20);
  assert(!gw_signal_targets_send(&registry, 0, 0, &info).status);
  assert(atomic_load(&notified) == before);
  set_mask(a, 0);
  assert(last_target == a && atomic_load(&notified) == before + 1);
  consume(a, 20, GW_SIGNAL_PROCESS);
  info.si_signo = 50;
  assert(gw_signal_targets_send(&registry, 1, 1, &info).status == EBUSY);

  gw_signal_target_close(a);
  gw_signal_target_close(b);
  assert(gw_signal_targets_send(&registry, 2, 2, &info).status == ESRCH);
  assert(!gw_signal_target_release(a) && !gw_signal_target_release(b));
  gw_signal_targets_close(&registry);
  assert(!gw_signal_targets_destroy(&registry));
}

struct sender {
  struct gw_signal_targets *registry;
  uintptr_t key;
  int tid;
  _Atomic int started, accepted, closed;
};

static void *send_many(void *argument) {
  struct sender *sender = argument;
  atomic_store_explicit(&sender->started, 1, memory_order_release);
  for (int i = 0; i < 10000; ++i) {
    siginfo_t info = packet(i);
    int error = gw_signal_targets_send(sender->registry, sender->key,
                                       sender->tid, &info)
                    .status;
    assert(!error || error == ESRCH);
    atomic_fetch_add(error ? &sender->closed : &sender->accepted, 1);
  }
  return NULL;
}

static void concurrent_close(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, 0, 50000, notify, &notified);
  struct gw_signal_target *target;
  assert(!gw_signal_target_open(&registry, 1, 1, 0, NULL, &target));
  struct sender senders[4];
  pthread_t threads[4];
  for (int i = 0; i < 4; ++i) {
    senders[i] = (struct sender){.registry = &registry, .key = 1, .tid = 1};
    assert(!pthread_create(&threads[i], NULL, send_many, &senders[i]));
  }
  for (int i = 0; i < 4; ++i) {
    while (!atomic_load_explicit(&senders[i].started, memory_order_acquire)) {
      sched_yield();
    }
  }
  gw_signal_target_close(target);
  for (int i = 0; i < 4; ++i) {
    assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&senders[i].accepted) +
               atomic_load(&senders[i].closed) ==
           10000);
  }
  assert(!gw_signal_target_release(target));
  gw_signal_targets_close(&registry);
  assert(!gw_signal_targets_destroy(&registry));
}

int main(void) {
  uint64_t all = UINT64_MAX, previous;
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, &previous, sizeof all));
  identity_and_growth();
  shared_and_failed_wake();
  concurrent_close();
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL, sizeof all));
  puts("{\"result\":\"PASS\",\"targets\":2048,\"concurrent_senders\":4}");
}
