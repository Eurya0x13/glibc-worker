#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int test_clock_gettime(clockid_t, struct timespec *);
#define clock_gettime test_clock_gettime
#include "rpc_metrics_internal.h"
#undef clock_gettime

#define CHECK(test)                                                           \
  do {                                                                        \
    if (!(test)) {                                                            \
      fprintf(stderr, "handoff assertion at line %d: %s\n", __LINE__, #test); \
      abort();                                                                \
    }                                                                         \
  } while (0)

#if GW_RPC_PROFILE
struct clock_step {
  clockid_t clock;
  uint64_t ns;
  int fail;
};
static _Thread_local const struct clock_step *script;
static _Thread_local unsigned script_position;

static int test_clock_gettime(clockid_t clock, struct timespec *value) {
  if (!script) {
    return clock_gettime(clock, value);
  }

  const struct clock_step *step = &script[script_position++];
  CHECK(step->clock == clock);
  if (step->fail) {
    errno = EIO;
    return -1;
  }

  value->tv_sec = (time_t)(step->ns / UINT64_C(1000000000));
  value->tv_nsec = (long)(step->ns % UINT64_C(1000000000));
  return 0;
}
static struct gw_handoff_row row;
static _Atomic unsigned request, response;
static _Atomic int stopped;

static void check_record(const struct gw_handoff_record *record) {
  CHECK(record->total_ns == record->end_ns - record->start_ns);
  CHECK(record->total_ns ==
        record->outbound_ns + record->execute_ns + record->resume_ns);
  CHECK(record->transport_ns == record->outbound_ns + record->resume_ns);
  CHECK(record->function == (record->request ^ 0x13579u));
  CHECK(record->context == 3 && record->depth == 0);
}

static void *peer(void *unused) {
  (void)unused;

  for (unsigned i = 1; i <= 20000; i++) {
    while (atomic_load_explicit(&request, memory_order_acquire) != i) {
      sched_yield();
    }
    gw_handoff_dispatch(&row, 0);
    gw_handoff_complete(&row, 0);
    atomic_store_explicit(&response, i, memory_order_release);
  }
  return NULL;
}

static void *reader(void *unused) {
  (void)unused;

  do {
    struct gw_handoff_summary snapshot = {0};
    gw_handoff_accumulate(&snapshot, &row);
    for (unsigned i = 0; i < GW_HANDOFF_KEEP; i++) {
      if (snapshot.worst[i].start_ns) {
        check_record(&snapshot.worst[i]);
      }
    }
  } while (!atomic_load_explicit(&stopped, memory_order_acquire));
  return NULL;
}

int main(void) {
  struct gw_handoff_row local = {0};
  const struct clock_step valid[] = {
      {CLOCK_MONOTONIC, 100, 0},          {CLOCK_MONOTONIC, 110, 0},
      {CLOCK_THREAD_CPUTIME_ID, 1000, 0}, {CLOCK_THREAD_CPUTIME_ID, 1200, 0},
      {CLOCK_MONOTONIC, 410, 0},          {CLOCK_MONOTONIC, 450, 0}};
  script = valid;

  errno = EPERM;
  do {
    gw_handoff_begin(&local, 0);
  } while (!local.exchange[0].sampled);
  /* A nested exchange must not overwrite its parent's timestamps. */
  local.exchange[1] = (struct gw_handoff_exchange){.sampled = 1, .start = 33};
  gw_handoff_dispatch(&local, 0);
  gw_handoff_complete(&local, 0);
  gw_handoff_finish(&local, 0, (struct gw_handoff_record){.api = 7});
  CHECK(errno == EPERM && script_position == 6);
  CHECK(local.exchange[1].start == 33);
  CHECK(local.samples == 1 && local.total_ns == 350 &&
        local.transport_ns == 50);
  CHECK(local.outbound_ns == 10 && local.execute_ns == 300 &&
        local.resume_ns == 40);
  CHECK(local.execute_cpu_ns == 200 && local.clock_errors == 0);

  for (unsigned i = 0; i < GW_HANDOFF_COMPONENTS; i++) {
    uint64_t sum = 0;
    for (unsigned j = 0; j < GW_HANDOFF_BINS; j++) {
      sum += local.histogram[i][j];
    }
    CHECK(sum == 1);
  }

  const struct clock_step failure[] = {{CLOCK_MONOTONIC, 0, 1},
                                       {CLOCK_MONOTONIC, 500, 0}};
  script = failure;
  script_position = 0;
  do {
    gw_handoff_begin(&local, 0);
  } while (!local.exchange[0].sampled);
  gw_handoff_dispatch(&local, 0);
  gw_handoff_complete(&local, 0);
  gw_handoff_finish(&local, 0, (struct gw_handoff_record){0});
  CHECK(errno == EPERM && local.samples == 1 && local.clock_errors == 1);
  script = NULL;
  local.exchange[0] =
      (struct gw_handoff_exchange){.sampled = 1, .start = 100, .dispatch = 90};
  gw_handoff_finish(&local, 0, (struct gw_handoff_record){0});
  CHECK(local.clock_errors == 2);

  pthread_t worker, exporter;
  CHECK(!pthread_create(&worker, NULL, peer, NULL));
  CHECK(!pthread_create(&exporter, NULL, reader, NULL));
  for (unsigned i = 1; i <= 20000; i++) {
    gw_handoff_begin(&row, 0);
    atomic_store_explicit(&request, i, memory_order_release);
    while (atomic_load_explicit(&response, memory_order_acquire) != i) {
      sched_yield();
    }
    gw_handoff_finish(
        &row, 0,
        (struct gw_handoff_record){
            .function = i ^ 0x13579u, .request = i, .context = 3, .depth = 0});
  }
  CHECK(!pthread_join(worker, NULL));
  atomic_store_explicit(&stopped, 1, memory_order_release);
  CHECK(!pthread_join(exporter, NULL));
  CHECK(row.calls == 20000 && row.samples >= 128 && row.clock_errors == 0);

  struct gw_handoff_summary snapshot = {0};
  gw_handoff_accumulate(&snapshot, &row);
  CHECK(snapshot.samples == row.samples);
  CHECK(snapshot.total_ns ==
        snapshot.outbound_ns + snapshot.execute_ns + snapshot.resume_ns);
  for (unsigned i = 0; i < GW_HANDOFF_KEEP; i++) {
    check_record(&snapshot.worst[i]);
  }

  printf(
      "RPC_HANDOFF exchanges=20000 samples=%llu coherent_export=PASS "
      "result=PASS\n",
      (unsigned long long)row.samples);
  return 0;
}
#else
static int test_clock_gettime(clockid_t clock, struct timespec *value) {
  return clock_gettime(clock, value);
}

int main(void) {
  /* Verify that the ordinary include does not pull in diagnostic helpers. */
  struct timespec now;
  CHECK(!test_clock_gettime(CLOCK_MONOTONIC, &now));
  puts("RPC_HANDOFF profile=0 disabled=PASS result=PASS");
  return 0;
}
#endif
