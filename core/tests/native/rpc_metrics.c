#define _GNU_SOURCE
#include "rpc_metrics_internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int checks;
#define CHECK(value)                                           \
  do {                                                         \
    if (!(value)) {                                            \
      fprintf(stderr, "RPC_METRICS_FAIL line=%d\n", __LINE__); \
      exit(1);                                                 \
    }                                                          \
    checks++;                                                  \
  } while (0)

void rpc_metrics_peer_run(void);

#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current;
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current;
static struct gw_rpc_row rows[5];
static _Atomic int worker_failures;
int rpc_metrics_fault_probe(struct gw_rpc_counters *out);

static uint64_t histogram_count(const struct gw_rpc_counters *counter) {
  uint64_t count = 0;
  for (unsigned i = 0; i < GW_RPC_HISTOGRAM_BINS; i++) {
    count += counter->latency_ns[i];
  }

  return count;
}

static struct gw_rpc_counters snapshot(const struct gw_rpc_row *row) {
  struct gw_rpc_counters result = {0};
  gw_rpc_accumulate(&result, row);
  return result;
}

static uint64_t monotonic(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value)) {
    abort();
  }

  return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
         (uint64_t)value.tv_nsec;
}

static void *worker(void *opaque) {
  struct gw_rpc_row *row = opaque;
  if (gw_rpc_current != NULL) {
    atomic_fetch_add(&worker_failures, 1);
  }
  gw_rpc_current = row;

  for (int i = 0; i < 500; i++) {
    errno = -305;
    struct gw_rpc_token token = gw_rpc_begin();
    rpc_metrics_peer_run();
    gw_rpc_end(token);
    if (errno != -305) {
      atomic_fetch_add(&worker_failures, 1);
    }
  }
  return NULL;
}
#endif

int main(void) {
  alarm(10);
  int evaluated = 0;
  GW_RPC_COUNT(active_waits, evaluated++);
  CHECK(
      evaluated ==
      0); /* Disabled diagnostics and unbound rows have no argument effects. */
#if GW_RPC_PROFILE
  struct gw_rpc_token unbound = gw_rpc_begin();
  gw_rpc_end(unbound);
  CHECK(unbound.row == NULL);
  gw_rpc_current = &rows[0];
  errno = -305;
  struct gw_rpc_token outer = gw_rpc_begin();
  uint64_t lower_start = monotonic();
  struct gw_rpc_counters state = snapshot(&rows[0]);
  CHECK(state.started == 1 && state.completed == 0 && state.inflight == 1);
  usleep(3000);
  rpc_metrics_peer_run();
  state = snapshot(&rows[0]);
  CHECK(state.started == 2 && state.completed == 1 && state.inflight == 1);
  usleep(3000);
  uint64_t lower_end = monotonic();
  gw_rpc_end(outer);
  CHECK(errno == -305);
  state = snapshot(&rows[0]);
  CHECK(state.started == 2 && state.completed == 2 && state.inflight == 0);
  CHECK(histogram_count(&state) == 2);
  CHECK(state.max_ns >=
        lower_end - lower_start); /* Nested calls cannot replace outer start. */
  CHECK(state.elapsed_ns >= state.max_ns && state.clock_errors == 0);
  unsigned highest =
      state.max_ns ? 63u - (unsigned)__builtin_clzll(state.max_ns) : 0;
  CHECK(state.latency_ns[highest] != 0);
  for (unsigned i = highest + 1; i < GW_RPC_HISTOGRAM_BINS; i++) {
    CHECK(state.latency_ns[i] == 0);
  }

  pthread_t threads[4];
  for (int i = 0; i < 4; i++) {
    CHECK(pthread_create(&threads[i], NULL, worker, &rows[i + 1]) == 0);
  }
  for (int i = 0; i < 4; i++) {
    CHECK(pthread_join(threads[i], NULL) == 0);
  }
  CHECK(atomic_load(&worker_failures) == 0 && gw_rpc_current == &rows[0]);
  for (int i = 1; i < 5; i++) {
    state = snapshot(&rows[i]);
    CHECK(state.started == 1000 && state.completed == 1000 &&
          state.inflight == 0);
    CHECK(histogram_count(&state) == 1000 && state.clock_errors == 0);
  }
  state = snapshot(&rows[0]);
  CHECK(state.started == 2 && state.completed == 2 && state.inflight == 0);

  struct gw_rpc_counters aggregate = {0};
  for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    gw_rpc_accumulate(&aggregate, &rows[i]);
  }
  CHECK(aggregate.started == 4002 && aggregate.completed == 4002 &&
        aggregate.inflight == 0);
  CHECK(histogram_count(&aggregate) == 4002);
  struct gw_rpc_counters faults = {0};
  CHECK(rpc_metrics_fault_probe(&faults));
  CHECK(faults.started == 2 && faults.completed == 2 && faults.inflight == 0);
  CHECK(faults.clock_errors == 2 && histogram_count(&faults) == 1);
  CHECK(gw_rpc_current == &rows[0]);
#else
  errno = -305;
  rpc_metrics_peer_run();
  CHECK(errno == -305);
#endif
  printf("RPC_METRICS_TEST checks=%d profile=%d spin_ns=%d result=PASS\n",
         checks, GW_RPC_PROFILE, GW_RPC_SPIN_NS);
  return 0;
}
