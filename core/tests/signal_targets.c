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

static void queue_packet(struct gw_signal_targets *registry,
                         struct gw_signal_target *target, int number,
                         int value) {
  siginfo_t info = packet(value);
  info.si_signo = number;
  struct gw_signal_publication result = gw_signal_targets_send(
      registry, target ? target->key : 0, target ? target->tid : 0, &info);
  assert(!result.status && !result.wake_error);
}

static void take_blocked(struct gw_signal_target *target, int number, int value,
                         enum gw_signal_queue_source expected) {
  siginfo_t information;
  enum gw_signal_queue_source source;
  assert(!gw_signal_target_deliver(target, &information, &source));
  assert(source == GW_SIGNAL_NONE);
  assert(gw_signal_target_take(target, UINT64_C(1) << (number - 1),
                               &information, &source) == number);
  assert(source == expected && information.si_value.sival_int == value);
}

static void consuming_and_discard(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, UINT64_C(1) << 49, 6, notify, &notified);
  struct gw_signal_target *a, *b;
  assert(!gw_signal_target_open(&registry, 1, 1, UINT64_MAX, NULL, &a));
  assert(!gw_signal_target_open(&registry, 2, 2, UINT64_MAX, NULL, &b));
  int before = atomic_load(&notified);
  queue_packet(&registry, NULL, 56, 1);
  queue_packet(&registry, a, 56, 2);
  queue_packet(&registry, b, 56, 3);
  queue_packet(&registry, a, 57, 4);
  queue_packet(&registry, b, 57, 5);
  queue_packet(&registry, NULL, 57, 6);
  queue_packet(&registry, a, SIGUSR1, 7);
  queue_packet(&registry, b, SIGUSR1, 8);
  queue_packet(&registry, NULL, SIGUSR1, 9);
  assert(registry.inbox.queued == 6);

  take_blocked(a, 56, 2, GW_SIGNAL_THREAD);
  assert(!gw_signal_targets_discard(&registry, 56));
  assert(registry.inbox.queued == 3);
  assert(!(gw_signal_target_pending(a) & (UINT64_C(1) << 55)));
  assert(!(gw_signal_target_pending(b) & (UINT64_C(1) << 55)));
  take_blocked(a, 57, 4, GW_SIGNAL_THREAD);
  take_blocked(a, 57, 6, GW_SIGNAL_PROCESS);
  assert(!gw_signal_targets_discard(&registry, SIGUSR1));
  assert(registry.inbox.queued == 1 && !gw_signal_target_pending(a));
  take_blocked(b, 57, 5, GW_SIGNAL_THREAD);
  assert(!registry.inbox.queued && !gw_signal_target_pending(b));

  for (int i = 0; i < 3; ++i) {
    queue_packet(&registry, a, 56, 10 + i);
  }
  queue_packet(&registry, NULL, 56, 13);
  queue_packet(&registry, b, 57, 14);
  queue_packet(&registry, b, 57, 15);
  siginfo_t info = packet(16);
  assert(gw_signal_targets_send(&registry, 1, 1, &info).status == EAGAIN);
  assert(!gw_signal_targets_discard(&registry, 56));
  assert(registry.inbox.queued == 2);
  queue_packet(&registry, a, 56, 17);
  take_blocked(b, 57, 14, GW_SIGNAL_THREAD);
  assert(!gw_signal_targets_discard(&registry, 57));
  assert(registry.inbox.queued == 1);
  assert(gw_signal_targets_discard(&registry, 50) == EBUSY);
  assert(gw_signal_targets_discard(&registry, 0) == EINVAL);
  assert(gw_signal_targets_discard(&registry, 32) == EINVAL);
  assert(gw_signal_targets_discard(&registry, 65) == EINVAL);
  assert(registry.inbox.queued == 1);
  take_blocked(a, 56, 17, GW_SIGNAL_THREAD);
  assert(atomic_load(&notified) == before);

  gw_signal_targets_close(&registry);
  assert(gw_signal_targets_discard(&registry, 56) == ESRCH);
  assert(!gw_signal_target_release(a) && !gw_signal_target_release(b));
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

static void concurrent_discard(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, 0, 50000, notify, &notified);
  struct gw_signal_target *target;
  assert(!gw_signal_target_open(&registry, 1, 1, 0, NULL, &target));
  struct sender senders[4];
  pthread_t threads[4];
  for (int i = 0; i < 4; ++i) {
    senders[i] = (struct sender){
        .registry = &registry, .key = i < 2 ? 1 : 0, .tid = i < 2 ? 1 : 0};
    assert(!pthread_create(&threads[i], NULL, send_many, &senders[i]));
  }
  for (int i = 0; i < 4; ++i) {
    while (!atomic_load_explicit(&senders[i].started, memory_order_acquire)) {
      sched_yield();
    }
  }
  for (int i = 0; i < 1000; ++i) {
    assert(!gw_signal_targets_discard(&registry, 56));
  }
  for (int i = 0; i < 4; ++i) {
    assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&senders[i].accepted) == 10000);
    assert(!atomic_load(&senders[i].closed));
  }
  assert(!gw_signal_targets_discard(&registry, 56));
  assert(!registry.inbox.queued && !gw_signal_target_pending(target));
  queue_packet(&registry, target, 56, 73);
  consume(target, 73, GW_SIGNAL_THREAD);
  gw_signal_targets_close(&registry);
  assert(!gw_signal_target_release(target));
  assert(!gw_signal_targets_destroy(&registry));
}

struct activation {
  struct gw_signal_target *target;
  _Atomic int *start;
};

static void *activate(void *argument) {
  struct activation *activation = argument;
  while (!atomic_load_explicit(activation->start, memory_order_acquire)) {
    sched_yield();
  }
  assert(!gw_signal_target_activate(activation->target, 71));
  return NULL;
}

static void prepared_admission(void) {
  struct gw_signal_targets registry;
  gw_signal_targets_init(&registry, 0, 4, notify, &notified);
  struct gw_signal_target *a, *b, *duplicate = NULL;
  assert(gw_signal_target_prepare(&registry, 0, 0, NULL, &duplicate) == EINVAL);
  assert(!duplicate);
  assert(!gw_signal_target_prepare(&registry, 1, 0, NULL, &a));
  assert(!a->tid && a->pins == 1);
  assert(gw_signal_target_pin(a) == ESRCH);
  assert(!gw_signal_target_retain(a));
  assert(gw_signal_target_prepare(&registry, 1, 0, NULL, &duplicate) ==
         EALREADY);
  assert(!duplicate);
  assert(!gw_signal_target_prepare(&registry, 2, 0, NULL, &b));

  int before = atomic_load(&notified);
  queue_packet(&registry, NULL, 56, 901);
  set_mask(a, 0);
  assert(!gw_signal_targets_retry(&registry));
  assert(atomic_load(&notified) == before);
  siginfo_t information;
  enum gw_signal_queue_source source = GW_SIGNAL_PROCESS;
  assert(!gw_signal_target_deliver(a, &information, &source));
  assert(source == GW_SIGNAL_NONE);
  source = GW_SIGNAL_PROCESS;
  assert(!gw_signal_target_take(a, UINT64_MAX, &information, &source));
  assert(source == GW_SIGNAL_NONE && registry.inbox.queued == 1);
  assert(gw_signal_target_activate(a, 0) == EINVAL);

  _Atomic int start = 0;
  struct activation activation = {.target = a, .start = &start};
  pthread_t threads[2];
  assert(!pthread_create(&threads[0], NULL, activate, &activation));
  assert(!pthread_create(&threads[1], NULL, activate, &activation));
  atomic_store_explicit(&start, 1, memory_order_release);
  assert(!pthread_join(threads[0], NULL));
  assert(!pthread_join(threads[1], NULL));
  assert(a->tid == 71 && a->pins == 2);
  assert(atomic_load(&notified) == before + 1 && last_target == a);
  assert(gw_signal_target_activate(a, 72) == EALREADY);
  consume(a, 901, GW_SIGNAL_PROCESS);
  assert(!gw_signal_target_pin(a));
  assert(!gw_signal_target_unpin(a));

  /* A failed clone discards its prepared identity. Reusing the descriptor
   * must not let a delayed completion activate the replacement target. */
  gw_signal_target_close(b);
  assert(gw_signal_target_activate(b, 72) == ESRCH);
  assert(gw_signal_target_release(b) == EBUSY);
  assert(!gw_signal_target_prepare(&registry, 2, 0, NULL, &duplicate));
  assert(duplicate != b && !duplicate->tid);
  assert(gw_signal_target_activate(b, 72) == ESRCH);
  assert(!gw_signal_target_unpin(b));
  assert(!gw_signal_target_release(b));

  queue_packet(&registry, NULL, 56, 902);
  consume(a, 902, GW_SIGNAL_PROCESS);
  set_mask(a, UINT64_MAX);
  queue_packet(&registry, NULL, 56, 903);
  before = atomic_load(&notified);
  wake_failure = EAGAIN;
  assert(!gw_signal_target_activate(duplicate, 73));
  assert(atomic_load(&notified) == before + 1);
  assert(gw_signal_targets_error(&registry) == EAGAIN);
  wake_failure = 0;
  assert(!gw_signal_targets_retry(&registry));
  consume(duplicate, 903, GW_SIGNAL_PROCESS);

  gw_signal_targets_close(&registry);
  assert(gw_signal_target_activate(a, 71) == ESRCH);
  assert(!gw_signal_target_unpin(a) && !gw_signal_target_unpin(a));
  assert(!gw_signal_target_unpin(duplicate));
  assert(!gw_signal_target_release(a));
  assert(!gw_signal_target_release(duplicate));
  assert(!gw_signal_targets_destroy(&registry));
}

int main(void) {
  uint64_t all = UINT64_MAX, previous;
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, &previous, sizeof all));
  identity_and_growth();
  shared_and_failed_wake();
  consuming_and_discard();
  concurrent_close();
  concurrent_discard();
  prepared_admission();
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL, sizeof all));
  puts(
      "{\"result\":\"PASS\",\"targets\":2048,\"concurrent_senders\":4,"
      "\"discard_quota\":true,\"blocked_consuming\":true,"
      "\"prepared_admission\":true}");
}
