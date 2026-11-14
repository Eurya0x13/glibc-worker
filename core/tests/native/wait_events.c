#define _GNU_SOURCE
#include "wait_event.h"
#include <pthread.h>
#include <signal.h>
#include <string.h>

static int checks;
#define CHECK(value)                                          \
  do {                                                        \
    if (!(value)) {                                           \
      fprintf(stderr, "WAIT_EVENT_FAIL line=%d\n", __LINE__); \
      exit(1);                                                \
    }                                                         \
    checks++;                                                 \
  } while (0)

static struct fb_event request, response;
static _Atomic int sent, received, ready, stop, returns;
static int payload;
static _Atomic int signals;
#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current;
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current;
static struct gw_rpc_row metrics[7];
static _Atomic int parked_row = 2;
#endif

static void signal_handler(int signal) {
  (void)signal;
  atomic_fetch_add_explicit(&signals, 1, memory_order_relaxed);
}

static void *echo(void *unused) {
  (void)unused;
#if GW_RPC_PROFILE
  gw_rpc_current = &metrics[1];
#endif

  for (int i = 1; i <= 20000; i++) {
    for (;;) {
      uint32_t seen = fb_event_sample(&request);
      if (atomic_load_explicit(&sent, memory_order_acquire) == i) {
        break;
      }
      fb_event_wait(&request, seen);
    }
    if (payload != (i ^ 0x13579)) {
      abort();
    }
    atomic_store_explicit(&received, i, memory_order_release);
    fb_event_notify(&response);
  }
  return NULL;
}

static void *park(void *unused) {
#if GW_RPC_PROFILE
  gw_rpc_current = &metrics[atomic_fetch_add(&parked_row, 1)];
#endif
  atomic_fetch_add(&ready, 1);
  errno = 73;

  for (;;) {
    uint32_t seen = fb_event_sample(&request);
    if (atomic_load_explicit(&stop, memory_order_acquire)) {
      break;
    }
    if (unused) {
      fb_event_wait_active(&request, seen);
    } else {
      fb_event_wait(&request, seen);
    }
    if (errno != 73) {
      abort();
    }
    atomic_fetch_add(&returns, 1);
  }
  return NULL;
}

int main(void) {
  /* Bounds every regression, including missing notifications. */
  alarm(15);
#if GW_RPC_PROFILE
  gw_rpc_current = &metrics[0];
#endif

  errno = 123;
  uint32_t before = fb_event_sample(&request);
  fb_event_notify(&request);
  fb_event_wait(&request, before);
  CHECK(errno ==
        123); /* Notification before syscall, EAGAIN, errno isolation. */
  before = fb_event_sample(&request);
  fb_event_notify(&request);
  errno = -305;
  fb_event_wait_active(&request, before);
  CHECK(errno ==
        -305); /* The active-wait early return preserves the caller's errno. */
  errno = 123;
  atomic_store(&request.sequence, UINT32_MAX);
  before = fb_event_sample(&request);
  fb_event_notify(&request);
  fb_event_wait(&request, before);
  CHECK(fb_event_sample(&request) == 0);

  pthread_t worker;
  CHECK(pthread_create(&worker, NULL, echo, NULL) == 0);
  for (int i = 1; i <= 20000; i++) {
    payload = i ^ 0x13579;
    atomic_store_explicit(&sent, i, memory_order_release);
    fb_event_notify(&request);
    for (;;) {
      uint32_t seen = fb_event_sample(&response);
      if (atomic_load_explicit(&received, memory_order_acquire) == i) {
        break;
      }
      fb_event_wait_active(&response, seen);
    }
  }
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(atomic_load(&received) == 20000 && errno == 123);

  struct sigaction action = {.sa_handler = signal_handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  CHECK(pthread_create(&worker, NULL, park, (void *)1) == 0);
  while (!atomic_load(&ready)) {
    usleep(100);
  }
  /* Wake without advancing the epoch: predicate must still be rechecked. */
  long woken = 0;
  for (int i = 0; i < 2000 && !woken; i++) {
    woken = syscall(SYS_futex, &request.sequence, FUTEX_WAKE_PRIVATE, INT_MAX,
                    NULL, NULL, 0);
    usleep(100);
  }
  CHECK(woken == 1);
  for (int i = 0; i < 2000 && !atomic_load(&returns); i++) {
    usleep(100);
  }
  CHECK(atomic_load(&returns) >= 1 && !atomic_load(&stop));
  int interrupted = 0;
  for (int i = 0; i < 2000 && atomic_load(&returns) < 2; i++) {
    if (pthread_kill(worker, SIGUSR1)) {
      break;
    }
    interrupted = 1;
    usleep(100);
  }
  CHECK(interrupted);
  CHECK(atomic_load(&signals) >= 1 && atomic_load(&returns) >= 2);
  atomic_store_explicit(&stop, 1, memory_order_release);
  fb_event_notify(&request);
  CHECK(pthread_join(worker, NULL) == 0);

  /* A controller shutdown may have several sleepers on one notification. */
  atomic_store(&stop, 0);
  atomic_store(&ready, 0);
  pthread_t sleepers[4];
  for (int i = 0; i < 4; i++) {
    CHECK(pthread_create(&sleepers[i], NULL, park, NULL) == 0);
  }
  while (atomic_load(&ready) != 4) {
    usleep(100);
  }
  usleep(20000);
  atomic_store_explicit(&stop, 1, memory_order_release);
  fb_event_notify(&request);
  for (int i = 0; i < 4; i++) {
    CHECK(pthread_join(sleepers[i], NULL) == 0);
  }

#if GW_RPC_PROFILE
  struct gw_rpc_counters totals = {0};
  for (unsigned i = 0; i < sizeof metrics / sizeof metrics[0]; i++) {
    gw_rpc_accumulate(&totals, &metrics[i]);
  }

  CHECK(totals.active_waits ==
        totals.ready_before_spin + totals.spin_hits + totals.spin_exhausted);
  CHECK(totals.wait_syscalls ==
        totals.wait_success + totals.wait_eagain + totals.wait_eintr);
  uint64_t notifications = 0;
  for (unsigned i = 0; i < sizeof metrics / sizeof metrics[0]; i++) {
    notifications += atomic_load(&metrics[i].stage[GW_RPC_NOTIFY].calls);
  }
  CHECK(notifications ==
        40005); /* Two per exchange plus five setup/stop notifications. */

#if GW_FUTEX_WAKE_FILTER
  CHECK(totals.wake_syscalls > 0 && totals.wake_syscalls <= notifications);
  CHECK(totals.wait_eintr >= 1 && totals.wait_success >= 1);
#else
  CHECK(totals.wake_syscalls == notifications);
  CHECK(totals.wait_eagain >= 2 && totals.wait_eintr >= 1 &&
        totals.wait_success >= 1);
#endif
  CHECK(totals.syscall_errors == 0 && totals.clock_errors == 0);
  CHECK(totals.spin_exhausted > 0);
#if !GW_FUTEX_WAKE_FILTER
  CHECK(totals.wait_syscalls >= totals.spin_exhausted);
#endif
#if GW_RPC_SPIN_NS == 0
  CHECK(totals.spin_hits == 0 && totals.spin_ns == 0);
#else
  CHECK(totals.spin_ns > 0);
#endif

  printf(
      "WAIT_EVENT_METRICS active=%llu ready=%llu spin_hit=%llu parked=%llu "
      "wait=%llu "
      "notify=%llu wake=%llu profile=1 spin_ns=%d filter=%d result=PASS\n",
      (unsigned long long)totals.active_waits,
      (unsigned long long)totals.ready_before_spin,
      (unsigned long long)totals.spin_hits,
      (unsigned long long)totals.spin_exhausted,
      (unsigned long long)totals.wait_syscalls,
      (unsigned long long)notifications,
      (unsigned long long)totals.wake_syscalls, GW_RPC_SPIN_NS,
      GW_FUTEX_WAKE_FILTER);
#endif
  printf("WAIT_EVENT_TEST checks=%d exchanges=20000 result=PASS\n", checks);
  return 0;
}
