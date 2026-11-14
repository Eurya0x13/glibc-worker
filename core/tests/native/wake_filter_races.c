#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef GW_FUTEX_WAKE_FILTER
#define GW_FUTEX_WAKE_FILTER 0
#endif
static long observed_syscall(long number, ...);
#if GW_FUTEX_WAKE_FILTER
static void event_test_hook(int point, void *event);
#define GW_EVENT_TEST_HOOK(point, event) event_test_hook(point, event)
#endif
#define syscall observed_syscall
#include "wait_event.h"
#undef syscall

#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current;
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current;
#endif

static unsigned checks;
#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      fprintf(stderr, "WAKE_FILTER_FAIL line=%d expr=%s\n", __LINE__, #value); \
      exit(1);                                                                 \
    }                                                                          \
    checks++;                                                                  \
  } while (0)

static _Atomic uint64_t wait_calls, wake_calls, wake_threads, active_waits,
    interrupted_waits;
static _Thread_local int thread_role;

/* Observe actual syscalls from the maintained header, including profile=0.
 * This wrapper implements no event state transitions and injects no return. */
static long observed_syscall(long number, ...) {
  if (number != SYS_futex) {
    abort();
  }
  va_list args;
  va_start(args, number);
  _Atomic uint32_t *word = va_arg(args, _Atomic uint32_t *);
  int operation = va_arg(args, int);
  unsigned value = va_arg(args, unsigned);
  void *timeout = va_arg(args, void *);
  void *second = va_arg(args, void *);
  int third = va_arg(args, int);
  va_end(args);

  if (operation == FUTEX_WAIT_PRIVATE) {
    atomic_fetch_add(&wait_calls, 1);
    atomic_fetch_add(&active_waits, 1);
  } else if (operation == FUTEX_WAKE_PRIVATE) {
    atomic_fetch_add(&wake_calls, 1);
  } else {
    abort();
  }
  long result = syscall(number, word, operation, value, timeout, second, third);
  int saved = errno;

  if (operation == FUTEX_WAIT_PRIVATE) {
    atomic_fetch_sub(&active_waits, 1);
    if (result < 0 && saved == EINTR) {
      atomic_fetch_add(&interrupted_waits, 1);
    }
  } else if (result >= 0) {
    atomic_fetch_add(&wake_threads, (uint64_t)result);
  }
  errno = saved;
  return result;
}

static uint64_t now_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value)) {
    abort();
  }

  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static void await_at_least(_Atomic int *value, int expected) {
  uint64_t deadline = now_ns() + UINT64_C(3000000000);

  while (atomic_load(value) < expected) {
    CHECK(now_ns() < deadline);
    usleep(50);
  }
}

static void await_waiters(unsigned expected) {
  uint64_t deadline = now_ns() + UINT64_C(3000000000);

  while (atomic_load(&active_waits) < expected) {
    CHECK(now_ns() < deadline);
    usleep(50);
  }
}

#if GW_FUTEX_WAKE_FILTER
static void await_wait_calls(uint64_t expected) {
  uint64_t deadline = now_ns() + UINT64_C(3000000000);

  while (atomic_load(&wait_calls) < expected) {
    CHECK(now_ns() < deadline);
    usleep(50);
  }
}

static struct {
  _Atomic int point, role, reached, released;
  _Atomic uintptr_t event;
} gate;

static void event_test_hook(int point, void *event) {
  if (atomic_load(&gate.point) != point ||
      atomic_load(&gate.role) != thread_role ||
      atomic_load(&gate.event) != (uintptr_t)event ||
      atomic_exchange(&gate.reached, 1)) {
    return;
  }

  while (!atomic_load(&gate.released)) {
    sched_yield();
  }
}

static void arm_gate(int point, int role, struct fb_event *event) {
  atomic_store(&gate.point, 0);
  atomic_store(&gate.role, role);
  atomic_store(&gate.event, (uintptr_t)event);
  atomic_store(&gate.reached, 0);
  atomic_store(&gate.released, 0);

  atomic_store(&gate.point, point);
}

static void release_gate(void) {
  atomic_store(&gate.released, 1);
  atomic_store(&gate.point, 0);
}
#endif

struct waiter {
  struct fb_event *event;
  uint32_t observed;
  int role, active;
  _Atomic int begin, started, done;
  _Atomic int *stop;
};

static void *waiter_main(void *opaque) {
  struct waiter *waiter = opaque;
  thread_role = waiter->role;
  atomic_store(&waiter->started, 1);
  while (!atomic_load(&waiter->begin)) {
    sched_yield();
  }
  errno = -305;

  for (;;) {
    uint32_t observed =
        waiter->stop ? fb_event_sample(waiter->event) : waiter->observed;
    if (waiter->stop &&
        atomic_load_explicit(waiter->stop, memory_order_acquire)) {
      break;
    }
    if (waiter->active) {
      fb_event_wait_active(waiter->event, observed);
    } else {
      fb_event_wait(waiter->event, observed);
    }
    if (errno != -305) {
      abort();
    }
    if (!waiter->stop) {
      break;
    }
  }
  atomic_store(&waiter->done, 1);
  return NULL;
}

static pthread_t start_waiter(struct waiter *waiter) {
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, waiter_main, waiter) == 0);
  await_at_least(&waiter->started, 1);
  return thread;
}

static void join_waiter(pthread_t thread, struct waiter *waiter) {
  await_at_least(&waiter->done, 1);
  CHECK(pthread_join(thread, NULL) == 0);
}

static void early_notification(void) {
  struct fb_event event = {0};
  struct waiter waiter = {
      .event = &event, .observed = fb_event_sample(&event), .role = 1};
  pthread_t thread = start_waiter(&waiter);
  uint64_t before_wait = atomic_load(&wait_calls),
           before_wake = atomic_load(&wake_calls);
  fb_event_notify(&event);
  atomic_store(&waiter.begin, 1);
  join_waiter(thread, &waiter);

  CHECK(atomic_load(&wake_calls) - before_wake ==
        (GW_FUTEX_WAKE_FILTER ? 0u : 1u));
  CHECK(atomic_load(&wait_calls) - before_wait ==
        (GW_FUTEX_WAKE_FILTER ? 0u : 1u));
}

static uint64_t empty_notifications(void) {
  struct fb_event event = {0};
  uint64_t before = atomic_load(&wake_calls);
  errno = 73;

  for (int i = 0; i < 10000; i++) {
    fb_event_notify(&event);
    CHECK(errno == 73);
  }
  uint64_t wakes = atomic_load(&wake_calls) - before;
  CHECK(wakes == (GW_FUTEX_WAKE_FILTER ? 0u : 10000u));
  CHECK(fb_event_sample(&event) == (GW_FUTEX_WAKE_FILTER ? 20000u : 10000u));

  atomic_store(&event.sequence,
               GW_FUTEX_WAKE_FILTER ? UINT32_MAX - 1 : UINT32_MAX);
  uint32_t previous = fb_event_sample(&event);
  fb_event_notify(&event);
  CHECK(fb_event_sample(&event) == 0);
  fb_event_wait(&event, previous);
  CHECK(errno == 73);
  return wakes;
}

#if GW_FUTEX_WAKE_FILTER
static void before_kernel_wait(int point) {
  struct fb_event event = {0};
  struct waiter waiter = {.event = &event, .role = 1, .begin = 1};
  arm_gate(point, 1, &event);
  pthread_t thread = start_waiter(&waiter);
  await_at_least(&gate.reached, 1);
  CHECK(atomic_load(&event.sequence) & 1);
  uint64_t wakes = atomic_load(&wake_calls), waits = atomic_load(&wait_calls);

  errno = 73;
  fb_event_notify(&event);
  CHECK(errno == 73);
  CHECK(atomic_load(&wake_calls) == wakes + 1);
  CHECK(atomic_load(&event.sequence) == 2);

  release_gate();
  join_waiter(thread, &waiter);
  CHECK(atomic_load(&wait_calls) ==
        waits + 1); /* Real kernel EAGAIN, not a mocked return. */
}

static _Atomic int signal_count;

static void signal_handler(int number) {
  (void)number;
  atomic_fetch_add(&signal_count, 1);
}

static void interrupted_exit(void) {
  struct sigaction action = {.sa_handler = signal_handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  struct fb_event event = {0};
  struct waiter waiter = {.event = &event, .role = 1, .begin = 1};
  arm_gate(3, 1, &event);
  pthread_t thread = start_waiter(&waiter);
  await_waiters(1);

  uint64_t before = atomic_load(&interrupted_waits);
  uint64_t deadline = now_ns() + UINT64_C(3000000000);
  while (!atomic_load(&gate.reached)) {
    CHECK(now_ns() < deadline);
    CHECK(pthread_kill(thread, SIGUSR1) == 0);
    usleep(100);
  }

  CHECK(atomic_load(&signal_count) > 0 &&
        atomic_load(&interrupted_waits) == before + 1);
  CHECK(atomic_load(&event.sequence) &
        1); /* A returning waiter never unregisters the hint. */

  uint64_t wakes = atomic_load(&wake_calls);
  fb_event_notify(&event); /* Legal stale hint: a genuine zero-return WAKE. */
  CHECK(atomic_load(&wake_calls) == wakes + 1 &&
        atomic_load(&event.sequence) == 2);
  release_gate();
  join_waiter(thread, &waiter);
}

static long raw_wake(struct fb_event *event) {
  return syscall(SYS_futex, &event->sequence, FUTEX_WAKE_PRIVATE, INT_MAX, NULL,
                 NULL, 0);
}

static void departing_waiter(void) {
  struct fb_event event = {0};
  struct waiter first = {.event = &event, .role = 1, .begin = 1};
  arm_gate(3, 1, &event);
  pthread_t a = start_waiter(&first);

  uint64_t deadline = now_ns() + UINT64_C(3000000000);
  long woken = 0;
  while (!woken) {
    CHECK(now_ns() < deadline);
    woken = raw_wake(&event);
    CHECK(woken >= 0);
    usleep(50);
  }
  CHECK(woken == 1); /* Proves the first waiter actually reached the kernel. */
  await_at_least(&gate.reached, 1);

  struct waiter second = {.event = &event, .role = 2, .begin = 1};
  pthread_t b = start_waiter(&second);
  await_waiters(1);
  release_gate();
  join_waiter(a, &first);

  CHECK(atomic_load(&event.sequence) &
        1); /* The departing waiter cannot erase peer interest. */
  fb_event_notify(&event);
  join_waiter(b, &second);
}

static void *notifier_main(void *opaque) {
  thread_role = 3;
  fb_event_notify(opaque);
  return NULL;
}

static void registering_during_notification(void) {
  struct fb_event event = {0};
  struct waiter first = {.event = &event, .role = 1, .begin = 1};
  pthread_t a = start_waiter(&first);
  await_waiters(1);
  arm_gate(4, 3, &event);
  pthread_t notifier;
  CHECK(pthread_create(&notifier, NULL, notifier_main, &event) == 0);
  await_at_least(&gate.reached, 1);
  CHECK(fb_event_sample(&event) == 2);

  struct waiter second = {.event = &event,
                          .observed = fb_event_sample(&event),
                          .role = 2,
                          .begin = 1};
  uint64_t before = atomic_load(&wait_calls);
  pthread_t b = start_waiter(&second);
  await_wait_calls(before + 1);

  /* A new epoch waiter is entitled to the next notification, not necessarily
   * the in-progress one. Its hint must survive that older notifier's WAKE. */
  release_gate();
  CHECK(pthread_join(notifier, NULL) == 0);
  join_waiter(a, &first);
  CHECK(atomic_load(&event.sequence) & 1);
  fb_event_notify(&event);
  join_waiter(b, &second);
}

static void registering_during_skipped_notification(void) {
  struct fb_event event = {0};
  struct waiter old = {.event = &event, .role = 1};
  pthread_t a =
      start_waiter(&old); /* Has an old epoch, but has not registered. */
  arm_gate(4, 3, &event);
  uint64_t before_wait = atomic_load(&wait_calls),
           before_wake = atomic_load(&wake_calls);

  pthread_t notifier;
  CHECK(pthread_create(&notifier, NULL, notifier_main, &event) == 0);
  await_at_least(&gate.reached, 1);
  atomic_store(&old.begin, 1);
  join_waiter(a, &old);
  CHECK(atomic_load(&wait_calls) ==
        before_wait); /* Epoch check avoids sleeping on old work. */

  struct waiter current = {.event = &event,
                           .observed = fb_event_sample(&event),
                           .role = 2,
                           .begin = 1};
  pthread_t b = start_waiter(&current);
  await_wait_calls(before_wait + 1);
  CHECK(atomic_load(&event.sequence) == 3);

  release_gate();
  CHECK(pthread_join(notifier, NULL) == 0);
  CHECK(atomic_load(&wake_calls) ==
        before_wake); /* Old hint was zero, despite new interest. */
  CHECK(atomic_load(&event.sequence) ==
        3); /* Old notifier must not erase new interest. */
  fb_event_notify(&event);
  join_waiter(b, &current);
}
#endif

static void shutdown_broadcast(void) {
  struct fb_event event = {0};
  _Atomic int stop = 0;
  struct waiter waiters[8] = {0};
  pthread_t threads[8];
  for (unsigned i = 0; i < 8; i++) {
    waiters[i] = (struct waiter){
        .event = &event, .role = (int)i + 1, .begin = 1, .stop = &stop};
    threads[i] = start_waiter(&waiters[i]);
  }

  await_waiters(8);
  usleep(1000);
  uint64_t before = atomic_load(&wake_calls),
           woken = atomic_load(&wake_threads);
  atomic_store_explicit(&stop, 1, memory_order_release);
  fb_event_notify(&event);
  for (unsigned i = 0; i < 8; i++) {
    join_waiter(threads[i], &waiters[i]);
  }

  CHECK(atomic_load(&wake_calls) == before + 1);
  CHECK(atomic_load(&wake_threads) > woken);
  CHECK(atomic_load(&active_waits) == 0);
}

struct reentry {
  struct fb_event host, guest;
  _Atomic int request, callback, callback_done, response;
  int argument, result;
};

static void *guest_main(void *opaque) {
  struct reentry *state = opaque;

  for (int i = 1; i <= 5000; i++) {
    for (;;) {
      uint32_t seen = fb_event_sample(&state->guest);
      if (atomic_load_explicit(&state->request, memory_order_acquire) == i) {
        break;
      }
      fb_event_wait(&state->guest, seen);
    }
    state->argument = i ^ 0x7351;
    atomic_store_explicit(&state->callback, i, memory_order_release);
    fb_event_notify(&state->host);
    for (;;) {
      uint32_t seen = fb_event_sample(&state->guest);
      if (atomic_load_explicit(&state->callback_done, memory_order_acquire) ==
          i) {
        break;
      }
      fb_event_wait_active(&state->guest, seen);
    }
    if (state->result != ((i ^ 0x7351) + 19)) {
      abort();
    }
    atomic_store_explicit(&state->response, i, memory_order_release);
    fb_event_notify(&state->host);
  }
  return NULL;
}

static void callback_reentry(void) {
  struct reentry state = {0};
  pthread_t guest;
  CHECK(pthread_create(&guest, NULL, guest_main, &state) == 0);

  for (int i = 1; i <= 5000; i++) {
    atomic_store_explicit(&state.request, i, memory_order_release);
    fb_event_notify(&state.guest);
    for (;;) {
      uint32_t seen = fb_event_sample(&state.host);
      if (atomic_load_explicit(&state.response, memory_order_acquire) == i) {
        break;
      }
      if (atomic_load_explicit(&state.callback, memory_order_acquire) == i &&
          atomic_load_explicit(&state.callback_done, memory_order_relaxed) !=
              i) {
        state.result = state.argument + 19;
        atomic_store_explicit(&state.callback_done, i, memory_order_release);
        fb_event_notify(&state.guest);
      }
      fb_event_wait_active(&state.host, seen);
    }
  }

  CHECK(pthread_join(guest, NULL) == 0);
  CHECK(atomic_load(&state.callback_done) == 5000 &&
        atomic_load(&state.response) == 5000);
}

struct producers {
  struct fb_event event;
  _Atomic int published;
};

static void *producer_main(void *opaque) {
  struct producers *state = opaque;
  errno = -305;

  for (unsigned i = 0; i < 5000; i++) {
    atomic_fetch_add_explicit(&state->published, 1, memory_order_release);
    fb_event_notify(&state->event);
    if (errno != -305) {
      abort();
    }
    if (!(i & 63)) {
      sched_yield();
    }
  }
  return NULL;
}

static void concurrent_notifiers(void) {
  struct producers state = {0};
  uint32_t initial = GW_FUTEX_WAKE_FILTER ? UINT32_MAX - 3 : UINT32_MAX - 19999;
  atomic_store(&state.event.sequence, initial);
  uint64_t before = atomic_load(&wake_calls);

  pthread_t workers[4];
  for (unsigned i = 0; i < 4; i++) {
    CHECK(pthread_create(&workers[i], NULL, producer_main, &state) == 0);
  }
  for (;;) {
    uint32_t observed = fb_event_sample(&state.event);
    if (atomic_load_explicit(&state.published, memory_order_acquire) == 20000) {
      break;
    }
    fb_event_wait_active(&state.event, observed);
  }
  for (unsigned i = 0; i < 4; i++) {
    CHECK(pthread_join(workers[i], NULL) == 0);
  }

  uint32_t expected = initial + (GW_FUTEX_WAKE_FILTER ? 2u : 1u) * 20000;
  CHECK(fb_event_sample(&state.event) == expected);
  uint64_t wakes = atomic_load(&wake_calls) - before;
  CHECK(GW_FUTEX_WAKE_FILTER ? wakes <= 20000 : wakes == 20000);
  CHECK(atomic_load(&active_waits) == 0);
}

int main(void) {
  alarm(30);
  uint64_t empty_wakes = empty_notifications();
  early_notification();

#if GW_FUTEX_WAKE_FILTER
  before_kernel_wait(1);
  before_kernel_wait(2);
  interrupted_exit();
  departing_waiter();
  registering_during_notification();
  registering_during_skipped_notification();
#endif

  shutdown_broadcast();
  callback_reentry();
  concurrent_notifiers();

  printf(
      "WAKE_FILTER_TEST checks=%u filter=%d profile=%d spin_ns=%d "
      "empty_notify=10000 "
      "empty_wake=%llu callbacks=5000 shutdown_waiters=8 "
      "concurrent_notifiers=4 result=PASS\n",
      checks, GW_FUTEX_WAKE_FILTER, GW_RPC_PROFILE, GW_RPC_SPIN_NS,
      (unsigned long long)empty_wakes);
  return 0;
}
