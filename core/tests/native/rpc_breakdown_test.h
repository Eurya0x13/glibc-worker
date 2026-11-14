#ifndef GW_RPC_BREAKDOWN_TEST_H
#define GW_RPC_BREAKDOWN_TEST_H
#include "rpc_metrics_internal.h"
#include <stdio.h>
#include <stdlib.h>

extern unsigned rpc_breakdown_checks;
#define COST_CHECK(value)                                             \
  do {                                                                \
    if (!(value)) {                                                   \
      fprintf(stderr, "RPC_BREAKDOWN_FAIL file=%s line=%d expr=%s\n", \
              __FILE__, __LINE__, #value);                            \
      exit(1);                                                        \
    }                                                                 \
    rpc_breakdown_checks++;                                           \
  } while (0)

#if GW_RPC_PROFILE
/* Ask the production selector which seed gives the next decision. The test
 * neither copies the PRNG nor changes the production sampling probability. */
static inline void force_next_sample(struct gw_rpc_row *row, int selected) {
  struct gw_rpc_row probe = {0};
  for (uint64_t seed = 1; seed < 1000000; seed++) {
    probe.sample_state = seed;
    if (!!gw_rpc_choose_sample(&probe) == !!selected) {
      row->sample_state = seed;
      return;
    }
  }
  abort();
}

static inline struct gw_rpc_stage_cost read_cost(const struct gw_rpc_row *row,
                                                 unsigned stage) {
  struct gw_rpc_stage_cost result = {0};
  gw_rpc_cost_accumulate(&result, &row->stage[stage]);
  return result;
}

static inline struct gw_rpc_stage_cost read_notify(const struct gw_rpc_row *row,
                                                   unsigned outcome) {
  struct gw_rpc_stage_cost result = {0};
  gw_rpc_cost_accumulate(&result, &row->notify[outcome]);
  return result;
}

/* Called only after all classified scopes have ended, with one writer
 * quiescent. The outcome buckets duplicate the existing NOTIFY sample,
 * including errors. */
static inline void expect_notify_conservation(const struct gw_rpc_row *row) {
  struct gw_rpc_stage_cost total = read_cost(row, GW_RPC_NOTIFY);
  struct gw_rpc_stage_cost zero = read_notify(row, GW_RPC_NOTIFY_ZERO_RETURN);
  struct gw_rpc_stage_cost positive =
      read_notify(row, GW_RPC_NOTIFY_POSITIVE_RETURN);
  COST_CHECK(total.calls == zero.calls + positive.calls);
  COST_CHECK(total.samples == zero.samples + positive.samples);
  COST_CHECK(total.wall_ns == zero.wall_ns + positive.wall_ns);
  COST_CHECK(total.cpu_ns == zero.cpu_ns + positive.cpu_ns);
  COST_CHECK(total.clock_errors == zero.clock_errors + positive.clock_errors);
}

static inline void expect_cost(const struct gw_rpc_row *row, unsigned stage,
                               uint64_t calls, uint64_t samples, uint64_t wall,
                               uint64_t cpu) {
  struct gw_rpc_stage_cost cost = read_cost(row, stage);
  COST_CHECK(cost.calls == calls);
  COST_CHECK(cost.samples == samples);
  COST_CHECK(cost.wall_ns == wall);
  COST_CHECK(cost.cpu_ns == cpu);
  COST_CHECK(cost.clock_errors == 0);
}

void rpc_breakdown_fake_cases(void);
#endif
#endif
