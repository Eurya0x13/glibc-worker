#define _GNU_SOURCE
#include "rpc_breakdown_test.h"
#include "wait_event.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

unsigned rpc_breakdown_checks;
#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current;
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current;

static uint64_t clock_ns(clockid_t clock) {
  struct timespec value;
  if (clock_gettime(clock, &value)) {
    abort();
  }
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
         (uint64_t)value.tv_nsec;
}

static void real_clock_cases(void) {
  struct gw_rpc_row row = {0};
  gw_rpc_current = &row;
  struct gw_rpc_scope sleeping, busy;

  force_next_sample(&row, 1);
  errno = -305;
  gw_rpc_scope_enter(&sleeping, GW_RPC_FUTEX_WAIT, -1);
  COST_CHECK(errno == -305);
  usleep(30000);
  gw_rpc_scope_end(&sleeping);
  COST_CHECK(errno == -305);

  struct gw_rpc_stage_cost sleep_cost = read_cost(&row, GW_RPC_FUTEX_WAIT);
  COST_CHECK(sleep_cost.calls == 1 && sleep_cost.samples == 1);
  COST_CHECK(sleep_cost.wall_ns >= 20000000);
  COST_CHECK(sleep_cost.cpu_ns < sleep_cost.wall_ns / 2);

  force_next_sample(&row, 1);
  gw_rpc_scope_enter(&busy, GW_RPC_GUEST_NATIVE_EXECUTE, -1);
  uint64_t before = clock_ns(CLOCK_THREAD_CPUTIME_ID);
  volatile uint64_t value = 7;
  do {
    for (int i = 0; i < 128; i++) {
      value = (value * UINT64_C(6364136223846793005)) ^ (value >> 9);
    }
  } while (clock_ns(CLOCK_THREAD_CPUTIME_ID) - before < 3000000);
  gw_rpc_scope_end(&busy);

  struct gw_rpc_stage_cost busy_cost =
      read_cost(&row, GW_RPC_GUEST_NATIVE_EXECUTE);
  COST_CHECK(busy_cost.calls == 1 && busy_cost.samples == 1);
  COST_CHECK(busy_cost.cpu_ns >= 3000000);
  COST_CHECK(busy_cost.cpu_ns > sleep_cost.cpu_ns);
  COST_CHECK(busy_cost.cpu_ns <= busy_cost.wall_ns + 1000000);
  COST_CHECK(sleep_cost.clock_errors == 0 && busy_cost.clock_errors == 0);
  COST_CHECK(gw_rpc_scope_current == NULL);

  printf(
      "RPC_BREAKDOWN_REAL sleep_wall_ns=%llu sleep_cpu_ns=%llu "
      "busy_wall_ns=%llu "
      "busy_cpu_ns=%llu result=PASS\n",
      (unsigned long long)sleep_cost.wall_ns,
      (unsigned long long)sleep_cost.cpu_ns,
      (unsigned long long)busy_cost.wall_ns,
      (unsigned long long)busy_cost.cpu_ns);
  gw_rpc_current = NULL;
}

struct notify_waiter {
  struct fb_event event;
  _Atomic int ready, stop;
};

static void *wait_for_notifications(void *argument) {
  struct notify_waiter *waiter = argument;
  /* This thread has no metrics row. Only the notifying thread writes row. */
  errno = 73;

  for (;;) {
    uint32_t observed = fb_event_sample(&waiter->event);
    atomic_store_explicit(&waiter->ready, 1, memory_order_release);
    if (atomic_load_explicit(&waiter->stop, memory_order_acquire)) {
      break;
    }
    fb_event_wait(&waiter->event, observed);
    if (errno != 73) {
      abort();
    }
  }
  return NULL;
}

static void sampled_notify(struct gw_rpc_row *row, struct fb_event *event) {
  force_next_sample(row, 1);
  errno = -305;
  fb_event_notify(event);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
}

static void real_notify_conservation(const struct gw_rpc_row *row) {
#if GW_FUTEX_WAKE_FILTER
  struct gw_rpc_stage_cost total = read_cost(row, GW_RPC_NOTIFY);
  struct gw_rpc_stage_cost zero = read_notify(row, GW_RPC_NOTIFY_ZERO_RETURN);
  struct gw_rpc_stage_cost positive =
      read_notify(row, GW_RPC_NOTIFY_POSITIVE_RETURN);

  COST_CHECK(zero.calls + positive.calls == atomic_load(&row->wake_syscalls));
  COST_CHECK(total.calls >= zero.calls + positive.calls);
  COST_CHECK(total.samples >= zero.samples + positive.samples);
  COST_CHECK(total.wall_ns >= zero.wall_ns + positive.wall_ns);
  COST_CHECK(total.cpu_ns >= zero.cpu_ns + positive.cpu_ns);
  COST_CHECK(total.clock_errors == zero.clock_errors + positive.clock_errors);
#else
  expect_notify_conservation(row);
#endif
}

static void real_notify_cases(void) {
  struct gw_rpc_row row = {0};
  struct notify_waiter waiter = {0};
  gw_rpc_current = &row;

  /* No thread exists yet. For the candidate, a retired waiter can leave a
   * sticky hint: force that legal state to exercise an actual zero WAKE. */
#if GW_FUTEX_WAKE_FILTER
  atomic_fetch_or(&waiter.event.sequence, 1);
#endif
  sampled_notify(&row, &waiter.event);
  struct gw_rpc_stage_cost zero = read_notify(&row, GW_RPC_NOTIFY_ZERO_RETURN);
  struct gw_rpc_stage_cost positive =
      read_notify(&row, GW_RPC_NOTIFY_POSITIVE_RETURN);
  COST_CHECK(zero.calls == 1 && zero.samples == 1);
  COST_CHECK(positive.calls == 0 && positive.samples == 0);
  real_notify_conservation(&row);

  pthread_t worker;
  COST_CHECK(pthread_create(&worker, NULL, wait_for_notifications, &waiter) ==
             0);
  while (!atomic_load_explicit(&waiter.ready, memory_order_acquire)) {
    usleep(100);
  }
  /* Readiness alone does not prove the waiter is in the kernel. Repeat a
   * sampled real WAKE until its observed return proves it woke a thread. */
  for (unsigned attempt = 0; attempt < 2000; attempt++) {
    usleep(500);
    sampled_notify(&row, &waiter.event);
    positive = read_notify(&row, GW_RPC_NOTIFY_POSITIVE_RETURN);
    if (positive.calls) {
      break;
    }
  }
  atomic_store_explicit(&waiter.stop, 1, memory_order_release);
  sampled_notify(&row, &waiter.event);
  COST_CHECK(pthread_join(worker, NULL) == 0);

  zero = read_notify(&row, GW_RPC_NOTIFY_ZERO_RETURN);
  positive = read_notify(&row, GW_RPC_NOTIFY_POSITIVE_RETURN);
  struct gw_rpc_stage_cost total = read_cost(&row, GW_RPC_NOTIFY);
  COST_CHECK(zero.calls >= 1 && positive.calls >= 1);
  COST_CHECK(zero.samples == zero.calls && positive.samples == positive.calls);
  COST_CHECK(total.clock_errors == 0);
  COST_CHECK(atomic_load(&row.wake_syscalls) == zero.calls + positive.calls);
  COST_CHECK(atomic_load(&row.wake_threads) >= positive.calls);
  COST_CHECK(atomic_load(&row.syscall_errors) == 0);
  real_notify_conservation(&row);

  printf(
      "RPC_NOTIFY_REAL zero_calls=%llu zero_samples=%llu positive_calls=%llu "
      "positive_samples=%llu result=PASS\n",
      (unsigned long long)zero.calls, (unsigned long long)zero.samples,
      (unsigned long long)positive.calls, (unsigned long long)positive.samples);
  gw_rpc_current = NULL;
}
#endif

int main(void) {
  alarm(10);

#if GW_RPC_PROFILE
  rpc_breakdown_fake_cases();
  real_clock_cases();
  real_notify_cases();
#else
  int evaluated = 0;
  errno = -305;
  GW_RPC_SCOPE(ignored, evaluated++, evaluated++);
  GW_RPC_SCOPE_END(ignored);
  COST_CHECK(evaluated == 0);
  COST_CHECK(errno == -305);
  struct fb_event event = {0};
  fb_event_notify(&event);
  COST_CHECK(fb_event_sample(&event) == (GW_FUTEX_WAKE_FILTER ? 2u : 1u));
  COST_CHECK(errno == -305);
#endif

  printf("RPC_BREAKDOWN_TEST checks=%u profile=%d spin_ns=%d result=PASS\n",
         rpc_breakdown_checks, GW_RPC_PROFILE, GW_RPC_SPIN_NS);
  return 0;
}
