#define _GNU_SOURCE
#include "rpc_metrics.h"
#if GW_RPC_PROFILE
#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* Only the clock provider is injected. All selection, nesting, validity,
 * accounting, and histogram logic below comes from the maintained header. */
static uint64_t wall_now, cpu_now;
static int fail_wall, fail_cpu;
static unsigned wall_reads, cpu_reads;

static int injected_clock(clockid_t clock, struct timespec *out) {
  uint64_t value;
  if (clock == CLOCK_MONOTONIC) {
    wall_reads++;
    if (fail_wall) {
      fail_wall--;
      errno = EIO;
      return -1;
    }
    value = wall_now;
  } else if (clock == CLOCK_THREAD_CPUTIME_ID) {
    cpu_reads++;
    if (fail_cpu) {
      fail_cpu--;
      errno = EIO;
      return -1;
    }
    value = cpu_now;
  } else {
    abort();
  }

  out->tv_sec = value / UINT64_C(1000000000);
  out->tv_nsec = value % UINT64_C(1000000000);
  errno = EBUSY; /* Successful diagnostics also must leave the caller's errno
                    unchanged. */
  return 0;
}

#define clock_gettime injected_clock
#include "rpc_breakdown_test.h"
#undef clock_gettime

static void reset(struct gw_rpc_row *row) {
  *row = (struct gw_rpc_row){0};
  gw_rpc_current = row;
  gw_rpc_scope_current = NULL;
  wall_now = UINT64_C(1000000000);
  cpu_now = UINT64_C(100000000);
  fail_wall = fail_cpu = 0;
  wall_reads = cpu_reads = 0;
  errno = -305;
}

static void advance(uint64_t wall, uint64_t cpu) {
  wall_now += wall;
  cpu_now += cpu;
}

static void nested_case(int parent_sampled, int child_sampled) {
  struct gw_rpc_row row;
  reset(&row);
  struct gw_rpc_scope parent, child;
  force_next_sample(&row, parent_sampled);

  gw_rpc_scope_enter(&parent, GW_RPC_HOST_JNI_GLUE, -1);
  COST_CHECK(errno == -305);
  advance(10, 4);
  gw_rpc_served(31);
  force_next_sample(&row, child_sampled);
  gw_rpc_scope_enter(&child, GW_RPC_HOST_JNI_EXECUTE, 31);
  COST_CHECK(errno == -305);
  advance(80, 30);
  gw_rpc_scope_end(&child);
  COST_CHECK(errno == -305);
  advance(20, 6);
  gw_rpc_scope_end(&parent);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);

  expect_cost(&row, GW_RPC_HOST_JNI_GLUE, 1, parent_sampled,
              parent_sampled ? 30 : 0, parent_sampled ? 10 : 0);
  expect_cost(&row, GW_RPC_HOST_JNI_EXECUTE, 1, child_sampled,
              child_sampled ? 80 : 0, child_sampled ? 30 : 0);
  COST_CHECK(atomic_load(&row.api[31].served_calls) == 1);
  COST_CHECK(atomic_load(&row.api[31].executed_calls) == 1);
  COST_CHECK(atomic_load(&row.api[31].samples) == (uint64_t)child_sampled);
  COST_CHECK(atomic_load(&row.api[31].wall_ns) == (child_sampled ? 80u : 0u));
  COST_CHECK(atomic_load(&row.api[31].cpu_ns) == (child_sampled ? 30u : 0u));
}

static void reentry_boundary(void) {
  struct gw_rpc_row row;
  reset(&row);
  struct gw_rpc_scope outer, boundary, inner;
  force_next_sample(&row, 1);

  gw_rpc_scope_enter(&outer, GW_RPC_HOST_JNI_EXECUTE, 31);
  advance(10, 4);
  gw_rpc_scope_enter(&boundary, GW_RPC_STAGE_COUNT, -1);
  advance(100, 30); /* Reentrant RPC gap with no measured stage. */
  force_next_sample(&row, 1);
  gw_rpc_scope_enter(&inner, GW_RPC_HOST_JNI_EXECUTE, 31);
  advance(15, 7);
  gw_rpc_scope_end(&inner);
  advance(100, 40); /* Another uninstrumented part of the nested RPC. */
  gw_rpc_scope_end(&boundary);
  advance(20, 6);
  gw_rpc_scope_end(&outer);

  expect_cost(&row, GW_RPC_HOST_JNI_EXECUTE, 2, 2, 45, 17);
  COST_CHECK(gw_rpc_scope_current == NULL && errno == -305);
}

static void sleeping_and_unsampled(void) {
  struct gw_rpc_row row;
  reset(&row);
  struct gw_rpc_scope scope;
  force_next_sample(&row, 1);
  gw_rpc_scope_enter(&scope, GW_RPC_FUTEX_WAIT, -1);
  advance(50000000, 0);
  gw_rpc_scope_end(&scope);
  expect_cost(&row, GW_RPC_FUTEX_WAIT, 1, 1, 50000000, 0);

  reset(&row);
  force_next_sample(&row, 0);
  gw_rpc_scope_enter(&scope, GW_RPC_NOTIFY, -1);
  advance(100, 75);
  gw_rpc_scope_end(&scope);
  expect_cost(&row, GW_RPC_NOTIFY, 1, 0, 0, 0);

  COST_CHECK(wall_reads == 0 && cpu_reads == 0);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
}

static void failures(void) {
  for (int at_end = 0; at_end < 2; at_end++) {
    for (int cpu_failure = 0; cpu_failure < 2; cpu_failure++) {
      struct gw_rpc_row row;
      reset(&row);
      struct gw_rpc_scope scope;
      force_next_sample(&row, 1);
      gw_rpc_served(31);
      if (!at_end) {
        if (cpu_failure) {
          fail_cpu = 1;
        } else {
          fail_wall = 1;
        }
      }
      gw_rpc_scope_enter(&scope, GW_RPC_HOST_JNI_EXECUTE, 31);
      advance(25, 10);
      if (at_end) {
        if (cpu_failure) {
          fail_cpu = 1;
        } else {
          fail_wall = 1;
        }
      }
      gw_rpc_scope_end(&scope);

      struct gw_rpc_stage_cost cost = read_cost(&row, GW_RPC_HOST_JNI_EXECUTE);
      COST_CHECK(cost.calls == 1 && cost.samples == 0);
      COST_CHECK(cost.wall_ns == 0 && cost.cpu_ns == 0 &&
                 cost.clock_errors > 0);
      COST_CHECK(atomic_load(&row.api[31].served_calls) == 1);
      COST_CHECK(atomic_load(&row.api[31].executed_calls) == 1);
      COST_CHECK(atomic_load(&row.api[31].samples) == 0 &&
                 atomic_load(&row.api[31].clock_errors) > 0);
      COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
    }
  }

  /* Failure while pausing a parent must not charge either clock to it. */
  struct gw_rpc_row row;
  reset(&row);
  struct gw_rpc_scope parent, child;
  force_next_sample(&row, 1);
  gw_rpc_scope_enter(&parent, GW_RPC_HOST_JNI_GLUE, -1);
  advance(10, 4);
  fail_cpu = 1;
  force_next_sample(&row, 0);
  gw_rpc_scope_enter(&child, GW_RPC_HOST_JNI_EXECUTE, -1);
  advance(80, 30);
  gw_rpc_scope_end(&child);
  advance(20, 6);
  gw_rpc_scope_end(&parent);

  struct gw_rpc_stage_cost cost = read_cost(&row, GW_RPC_HOST_JNI_GLUE);
  COST_CHECK(cost.calls == 1 && cost.samples == 0 && cost.clock_errors > 0);
  COST_CHECK(cost.wall_ns == 0 && cost.cpu_ns == 0);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
}

static void retired_writer(void) {
  struct gw_rpc_row old, replacement;
  reset(&old);
  replacement = (struct gw_rpc_row){0};
  struct gw_rpc_scope retired, current;
  force_next_sample(&old, 1);
  gw_rpc_scope_enter(&retired, GW_RPC_NOTIFY, -1);
  advance(10, 4);

  gw_rpc_current =
      &replacement; /* Ownership was handed off; no writes to the old row. */
  force_next_sample(&replacement, 1);
  gw_rpc_scope_enter(&current, GW_RPC_NOTIFY, -1);
  advance(20, 6);
  gw_rpc_scope_end(&current);

  unsigned reads = wall_reads + cpu_reads;
  gw_rpc_scope_end(&retired);
  COST_CHECK(wall_reads + cpu_reads == reads);
  expect_cost(&old, GW_RPC_NOTIFY, 1, 0, 0, 0);
  expect_cost(&replacement, GW_RPC_NOTIFY, 1, 1, 20, 6);
  COST_CHECK(gw_rpc_scope_current == NULL && errno == -305);
}

static void selection_and_calls(void) {
  struct gw_rpc_row row;
  reset(&row);
  row.sample_state = 1;
  for (unsigned i = 0; i < 65536; i++) {
    struct gw_rpc_scope scope;
    gw_rpc_served(31);
    gw_rpc_scope_enter(&scope, GW_RPC_HOST_JNI_EXECUTE, 31);
    advance(7, 3);
    gw_rpc_scope_end(&scope);
  }

  struct gw_rpc_stage_cost cost = read_cost(&row, GW_RPC_HOST_JNI_EXECUTE);
  COST_CHECK(cost.calls == 65536);
  COST_CHECK(cost.samples > 512 && cost.samples < 2048);
  COST_CHECK(cost.wall_ns == cost.samples * 7 &&
             cost.cpu_ns == cost.samples * 3);
  COST_CHECK(cost.clock_errors == 0);
  COST_CHECK(atomic_load(&row.api[31].served_calls) == 65536);
  COST_CHECK(atomic_load(&row.api[31].executed_calls) == 65536);
  COST_CHECK(atomic_load(&row.api[31].samples) == cost.samples);
  COST_CHECK(atomic_load(&row.api[31].wall_ns) == cost.wall_ns &&
             atomic_load(&row.api[31].cpu_ns) == cost.cpu_ns);
  /* Served requests are counted independently of a selected timing scope. */
  gw_rpc_served(31);
  COST_CHECK(atomic_load(&row.api[31].served_calls) == 65537 &&
             atomic_load(&row.api[31].executed_calls) == 65536 &&
             atomic_load(&row.api[31].samples) == cost.samples);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
}

static void notify_classification(void) {
  struct gw_rpc_row row;
  reset(&row);
  for (int selected = 0; selected < 2; selected++) {
    /* A positive returned thread count is one notification, even when the
     * kernel woke more than one thread. Use different costs per outcome. */
    const long returns[] = {0, 1, 4};
    for (unsigned i = 0; i < sizeof returns / sizeof *returns; i++) {
      struct gw_rpc_scope scope;
      force_next_sample(&row, selected);
      unsigned before_wall = wall_reads, before_cpu = cpu_reads;
      gw_rpc_scope_enter(&scope, GW_RPC_NOTIFY, -1);
      advance(20 + i, 5 + i);
      unsigned classify_wall = wall_reads, classify_cpu = cpu_reads;
      gw_rpc_notify_classify(&scope, returns[i]);
      COST_CHECK(wall_reads == classify_wall && cpu_reads == classify_cpu);
      gw_rpc_scope_end(&scope);
      COST_CHECK(wall_reads - before_wall == (selected ? 2u : 0u));
      COST_CHECK(cpu_reads - before_cpu == (selected ? 2u : 0u));
      COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
      expect_notify_conservation(&row);
    }
  }
  expect_cost(&row, GW_RPC_NOTIFY, 6, 3, 63, 18);
  struct gw_rpc_stage_cost zero = read_notify(&row, GW_RPC_NOTIFY_ZERO_RETURN);
  struct gw_rpc_stage_cost positive =
      read_notify(&row, GW_RPC_NOTIFY_POSITIVE_RETURN);
  COST_CHECK(zero.calls == 2 && zero.samples == 1 && zero.wall_ns == 20 &&
             zero.cpu_ns == 5);
  COST_CHECK(positive.calls == 4 && positive.samples == 2 &&
             positive.wall_ns == 43 && positive.cpu_ns == 13);

  /* Failure can precede classification or happen in the final clock pair.
   * Both buckets must receive the original scope error, never a new sample. */
  for (unsigned outcome = 0; outcome < GW_RPC_NOTIFY_OUTCOME_COUNT; outcome++) {
    for (int at_end = 0; at_end < 2; at_end++) {
      for (int cpu_failure = 0; cpu_failure < 2; cpu_failure++) {
        reset(&row);
        struct gw_rpc_scope scope;
        force_next_sample(&row, 1);
        if (!at_end) {
          if (cpu_failure) {
            fail_cpu = 1;
          } else {
            fail_wall = 1;
          }
        }
        gw_rpc_scope_enter(&scope, GW_RPC_NOTIFY, -1);
        advance(25, 10);
        unsigned before_wall = wall_reads, before_cpu = cpu_reads;
        gw_rpc_notify_classify(&scope,
                               outcome == GW_RPC_NOTIFY_ZERO_RETURN ? 0 : 4);
        COST_CHECK(wall_reads == before_wall && cpu_reads == before_cpu);
        if (at_end) {
          if (cpu_failure) {
            fail_cpu = 1;
          } else {
            fail_wall = 1;
          }
        }
        gw_rpc_scope_end(&scope);
        struct gw_rpc_stage_cost cost = read_notify(&row, outcome);
        COST_CHECK(cost.calls == 1 && cost.samples == 0);
        COST_CHECK(cost.wall_ns == 0 && cost.cpu_ns == 0 &&
                   cost.clock_errors == 1);
        expect_notify_conservation(&row);
        COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
      }
    }
  }

  /* An unbound or retired writer must not create a classification or read a
   * clock after ownership has moved, even if the syscall itself succeeded. */
  reset(&row);
  struct gw_rpc_scope scope;
  force_next_sample(&row, 1);
  gw_rpc_scope_enter(&scope, GW_RPC_NOTIFY, -1);
  gw_rpc_current = NULL;
  unsigned before_wall = wall_reads, before_cpu = cpu_reads;
  gw_rpc_notify_classify(&scope, 1);
  gw_rpc_scope_end(&scope);
  COST_CHECK(wall_reads == before_wall && cpu_reads == before_cpu);
  positive = read_notify(&row, GW_RPC_NOTIFY_POSITIVE_RETURN);
  COST_CHECK(positive.calls == 0 && positive.samples == 0 &&
             positive.clock_errors == 0);
  COST_CHECK(errno == -305 && gw_rpc_scope_current == NULL);
}

void rpc_breakdown_fake_cases(void) {
  nested_case(1, 1);
  nested_case(1, 0);
  nested_case(0, 1);
  reentry_boundary();
  sleeping_and_unsampled();
  failures();
  retired_writer();
  selection_and_calls();
  notify_classification();

  gw_rpc_current = NULL;
}
#endif
