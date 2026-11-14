#ifndef GW_RPC_METRICS_INTERNAL_H
#define GW_RPC_METRICS_INTERNAL_H
#include "rpc_metrics.h"
#if GW_RPC_PROFILE
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "rpc_handoff_internal.h"

// clang-format off
#define GW_RPC_SCALARS(X) \
  X(started) \
  X(completed) \
  X(inflight) \
  X(elapsed_ns) \
  X(active_waits) \
  X(ready_before_spin) \
  X(spin_hits) \
  X(spin_exhausted) \
  X(spin_ns) \
  X(wait_syscalls) \
  X(wait_success) \
  X(wait_eagain) \
  X(wait_eintr) \
  X(wake_syscalls) \
  X(wake_threads) \
  X(syscall_errors) \
  X(clock_errors)
// clang-format on

/* Wall/CPU cost accumulators reused for stages, api slots and notify
 * outcomes. One writer per embedding; atomic members keep the live snapshot
 * data-race-free. */
struct gw_rpc_cost_row {
  _Atomic uint64_t calls, samples, wall_ns, cpu_ns, clock_errors;
};

/* Per-api-slot variant: served_calls counts Host-dispatched requests,
 * executed_calls counts entries into the primary JNI call/helper. */
struct gw_rpc_api_row {
  _Atomic uint64_t served_calls, executed_calls, samples, wall_ns, cpu_ns,
      clock_errors;
};

/* One writer per libc domain/context; never reset when a slot is reused. Every
 * sampled field is atomic to keep live export data-race-free. Relaxed stores
 * avoid contended RMW operations; domains use separate rows. */
struct gw_rpc_row {
#define GW_RPC_FIELD(name) _Atomic uint64_t name;
  GW_RPC_SCALARS(GW_RPC_FIELD)
#undef GW_RPC_FIELD
  _Atomic uint64_t max_ns;
  _Atomic uint64_t latency_ns[GW_RPC_HISTOGRAM_BINS];
  struct gw_rpc_cost_row stage[GW_RPC_STAGE_COUNT];
  struct gw_rpc_api_row api[GW_RPC_API_SLOTS];
  struct gw_rpc_cost_row notify[GW_RPC_NOTIFY_OUTCOME_COUNT];
  /* Single-writer sampling state; never read by snapshot. */
  uint64_t sample_state;
  struct gw_handoff_row handoff;
} __attribute__((aligned(64)));

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
               "RPC metrics need lock-free 64-bit counters");
extern _Thread_local struct gw_rpc_row *gw_rpc_current
    __attribute__((visibility("hidden")));

/* Single-writer relaxed increment, deliberately not an RMW: snapshots read
 * the same words without contending with the writer. Requires exactly one
 * writer per counter. */
static inline void gw_rpc_add(_Atomic uint64_t *value, uint64_t amount) {
  atomic_store_explicit(
      value, atomic_load_explicit(value, memory_order_relaxed) + amount,
      memory_order_relaxed);
}

/* Fast counter bump for the current row; a no-op when profiling is off or
 * this thread has not acquired a row yet. */
#define GW_RPC_COUNT(field, amount)                 \
  do {                                              \
    if (gw_rpc_current) {                           \
      gw_rpc_add(&gw_rpc_current->field, (amount)); \
    }                                               \
  } while (0)

/* Scope links are stack allocated. Enter/end, including the unsampled case,
 * are compiled out of production callers. Pause/resume only reads clocks when
 * a sampled parent is active; no child work is charged to that parent. */
struct gw_rpc_scope {
  struct gw_rpc_scope *parent;
  struct gw_rpc_row *row;
  struct gw_rpc_cost_row *classified;
  unsigned stage;
  int api, sampled, valid, clock_failed;
  uint64_t wall_start, cpu_start, wall_ns, cpu_ns;
};
extern _Thread_local struct gw_rpc_scope *gw_rpc_scope_current
    __attribute__((visibility("hidden")));

/* Reads the wall+CPU pair while preserving the caller's errno. Returns 0
 * when either clock failed: no sentinel is written, so callers must treat
 * the whole interval as invalid. */
static inline int gw_rpc_cost_clock(uint64_t *wall, uint64_t *cpu) {
  int saved = errno;
  struct timespec w, c;
  int wr = clock_gettime(CLOCK_MONOTONIC, &w);
  int cr = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
  errno = saved;
  if (wr || cr) {
    return 0;
  }

  *wall = (uint64_t)w.tv_sec * UINT64_C(1000000000) + (uint64_t)w.tv_nsec;
  *cpu = (uint64_t)c.tv_sec * UINT64_C(1000000000) + (uint64_t)c.tv_nsec;
  return 1;
}

/* xorshift64 over the row's private state, seeded from the row address and
 * never reset; a sample is taken when the top GW_RPC_SAMPLE_SHIFT bits come
 * out zero (~1/64 of entries), independently of nesting or call position. */
static inline int gw_rpc_choose_sample(struct gw_rpc_row *row) {
  uint64_t x = row->sample_state;
  if (!x) {
    x = UINT64_C(0x9e3779b97f4a7c15) ^ (uintptr_t)row;
  }

  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  row->sample_state = x;
  return ((x * UINT64_C(2685821657736338717)) >> (64 - GW_RPC_SAMPLE_SHIFT)) ==
         0;
}

/* Permanently invalidates the scope after a clock failure and charges one
 * clock_error to every destination; the scope still counts as a call but
 * never as a sample. */
static inline void gw_rpc_scope_error(struct gw_rpc_scope *scope) {
  scope->valid = 0;
  scope->clock_failed = 1;
  gw_rpc_add(&scope->row->stage[scope->stage].clock_errors, 1);
  if (scope->api >= 0) {
    gw_rpc_add(&scope->row->api[scope->api].clock_errors, 1);
  }
}

/* Adds the interval since resume to the scope's own totals; only a sampled,
 * still-valid scope on its original row pays for the clock reads. */
static inline void gw_rpc_scope_pause(struct gw_rpc_scope *scope) {
  if (!scope || !scope->sampled || !scope->valid) {
    return;
  }

  /* Clearing/changing TLS retires this writer before slot handover. */
  if (scope->row != gw_rpc_current) {
    scope->valid = 0;
    return;
  }

  uint64_t wall, cpu;
  if (!gw_rpc_cost_clock(&wall, &cpu) || wall < scope->wall_start ||
      cpu < scope->cpu_start) {
    gw_rpc_scope_error(scope);
    return;
  }

  scope->wall_ns += wall - scope->wall_start;
  scope->cpu_ns += cpu - scope->cpu_start;
}

/* Re-arms the interval start after nested child work. A clock failure here
 * invalidates the scope permanently; pause skips invalid scopes. */
static inline void gw_rpc_scope_resume(struct gw_rpc_scope *scope) {
  if (scope && scope->row == gw_rpc_current && scope->sampled && scope->valid &&
      !gw_rpc_cost_clock(&scope->wall_start, &scope->cpu_start)) {
    gw_rpc_scope_error(scope);
  }
}

/* Pushes a scope onto the TLS chain, pausing the parent first so child time
 * stays excluded from the parent's sample. A missing row or an out-of-range
 * stage/api preserves the nesting (parent bookkeeping stays correct) but
 * disables counting and sampling for this scope. */
static inline void gw_rpc_scope_enter(struct gw_rpc_scope *scope,
                                      unsigned stage, int api) {
  *scope = (struct gw_rpc_scope){.parent = gw_rpc_scope_current,
                                 .row = gw_rpc_current,
                                 .stage = stage,
                                 .api = api};
  gw_rpc_scope_pause(scope->parent);
  gw_rpc_scope_current = scope;

  if (!scope->row || stage >= GW_RPC_STAGE_COUNT ||
      api >= (int)GW_RPC_API_SLOTS) {
    scope->row = NULL;
    return;
  }

  gw_rpc_add(&scope->row->stage[stage].calls, 1);
  if (api >= 0) {
    gw_rpc_add(&scope->row->api[api].executed_calls, 1);
  }

  scope->sampled = gw_rpc_choose_sample(scope->row);
  scope->valid = 1;
  gw_rpc_scope_resume(scope);
}

/* Called after a successful WAKE, before ending its existing scope. This
 * selects a second accounting destination without another clock read. */
static inline void gw_rpc_notify_classify(struct gw_rpc_scope *scope, long rc) {
  if (!scope->row || scope->row != gw_rpc_current || rc < 0) {
    return;
  }
  unsigned outcome =
      rc == 0 ? GW_RPC_NOTIFY_ZERO_RETURN : GW_RPC_NOTIFY_POSITIVE_RETURN;
  scope->classified = &scope->row->notify[outcome];
  gw_rpc_add(&scope->classified->calls, 1);
}

/* Pops the scope and flushes a valid sample to the stage, the classified
 * notify outcome and the api slot. Only the row that entered may write:
 * after a row handover the unfinished sample is discarded instead of
 * landing in a slot that may already be reused. */
static inline void gw_rpc_scope_end(struct gw_rpc_scope *scope) {
  gw_rpc_scope_pause(scope);

  if (scope->row && scope->row == gw_rpc_current && scope->classified &&
      scope->clock_failed) {
    gw_rpc_add(&scope->classified->clock_errors, 1);
  }

  if (scope->row && scope->row == gw_rpc_current && scope->sampled &&
      scope->valid) {
    struct gw_rpc_cost_row *cost = &scope->row->stage[scope->stage];
    gw_rpc_add(&cost->samples, 1);
    gw_rpc_add(&cost->wall_ns, scope->wall_ns);
    gw_rpc_add(&cost->cpu_ns, scope->cpu_ns);

    if (scope->classified) {
      gw_rpc_add(&scope->classified->samples, 1);
      gw_rpc_add(&scope->classified->wall_ns, scope->wall_ns);
      gw_rpc_add(&scope->classified->cpu_ns, scope->cpu_ns);
    }

    if (scope->api >= 0) {
      struct gw_rpc_api_row *api = &scope->row->api[scope->api];
      gw_rpc_add(&api->samples, 1);
      gw_rpc_add(&api->wall_ns, scope->wall_ns);
      gw_rpc_add(&api->cpu_ns, scope->cpu_ns);
    }
  }

  gw_rpc_scope_current = scope->parent;
  gw_rpc_scope_resume(scope->parent);
}

/* Counts a Host-dispatched request into its api slot; safe to call before
 * this thread owns a row. */
static inline void gw_rpc_served(unsigned api) {
  if (gw_rpc_current && api < GW_RPC_API_SLOTS) {
    gw_rpc_add(&gw_rpc_current->api[api].served_calls, 1);
  }
}

/* Startup only, before ready/measurement: minimum raw empty interval. */
static inline void gw_rpc_calibrate(struct gw_rpc_stage_cost *out) {
  memset(out, 0, sizeof *out);
  out->wall_ns = out->cpu_ns = UINT64_MAX;

  for (unsigned i = 0; i < 32; i++) {
    uint64_t w0, c0, w1, c1;
    out->calls++;
    if (!gw_rpc_cost_clock(&w0, &c0) || !gw_rpc_cost_clock(&w1, &c1) ||
        w1 < w0 || c1 < c0) {
      out->clock_errors++;
      continue;
    }

    out->samples++;
    if (w1 - w0 < out->wall_ns) {
      out->wall_ns = w1 - w0;
    }
    if (c1 - c0 < out->cpu_ns) {
      out->cpu_ns = c1 - c0;
    }
  }

  if (!out->samples) {
    out->wall_ns = out->cpu_ns = 0;
  }
}

/* Relaxed merge of one cost row into a POD total: live values, no
 * transaction across fields. */
static inline void gw_rpc_cost_accumulate(struct gw_rpc_stage_cost *out,
                                          const struct gw_rpc_cost_row *row) {
#define GW_RPC_COST_SUM(name) \
  out->name += atomic_load_explicit(&row->name, memory_order_relaxed)
  GW_RPC_COST_SUM(calls);
  GW_RPC_COST_SUM(samples);
  GW_RPC_COST_SUM(wall_ns);
  GW_RPC_COST_SUM(cpu_ns);
  GW_RPC_COST_SUM(clock_errors);

#undef GW_RPC_COST_SUM
}

/* The scope object lives on the caller's stack; with GW_RPC_PROFILE off
 * both macro and object vanish from the build. */
#define GW_RPC_SCOPE(name, stage, api) \
  struct gw_rpc_scope name;            \
  gw_rpc_scope_enter(&name, stage, api)
#define GW_RPC_SCOPE_END(name) gw_rpc_scope_end(&name)

/* Monotonic clock in ns; on failure returns UINT64_MAX and charges
 * clock_errors to row when present. Preserves the caller's errno. */
static inline uint64_t gw_rpc_clock(struct gw_rpc_row *row) {
  int saved = errno;
  struct timespec now;
  int rc = clock_gettime(CLOCK_MONOTONIC, &now);
  errno = saved;
  if (rc) {
    if (row) {
      gw_rpc_add(&row->clock_errors, 1);
    }
    return UINT64_MAX;
  }
  return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

/* Handle pairing gw_rpc_begin with exactly one gw_rpc_end; row == NULL
 * marks an inactive interval. */
struct gw_rpc_token {
  struct gw_rpc_row *row;
  uint64_t start_ns;
};

/* Marks a request started and in-flight. Unlike the scope machinery this
 * path is not sampled: a begin/end pair on an owned row always pays two
 * clock reads; with no row the call does nothing. */
static inline struct gw_rpc_token gw_rpc_begin(void) {
  struct gw_rpc_row *row = gw_rpc_current;
  struct gw_rpc_token token = {.row = row};
  if (row) {
    gw_rpc_add(&row->started, 1);
    gw_rpc_add(&row->inflight, 1);
    token.start_ns = gw_rpc_clock(row);
  }
  return token;
}

/* Pairs with begin: records the inclusive RPC latency into elapsed_ns, the
 * log2 histogram and keep-max max_ns. A clock failure or non-monotonic pair
 * drops only the sample; completed is still counted and inflight is
 * decremented (a wrapping add) in every case. */
static inline void gw_rpc_end(struct gw_rpc_token token) {
  struct gw_rpc_row *row = token.row;
  if (!row) {
    return;
  }

  uint64_t end = gw_rpc_clock(row);

  if (token.start_ns != UINT64_MAX && end != UINT64_MAX &&
      end >= token.start_ns) {
    uint64_t elapsed = end - token.start_ns;
    unsigned bucket = elapsed ? 63u - (unsigned)__builtin_clzll(elapsed) : 0;
    gw_rpc_add(&row->elapsed_ns, elapsed);
    gw_rpc_add(&row->latency_ns[bucket], 1);
    if (elapsed > atomic_load_explicit(&row->max_ns, memory_order_relaxed)) {
      atomic_store_explicit(&row->max_ns, elapsed, memory_order_relaxed);
    }
  }

  gw_rpc_add(&row->completed, 1);
  gw_rpc_add(&row->inflight, UINT64_MAX);
}

/* Relaxed merge of one row's counters into a snapshot: scalars are summed,
 * max_ns is kept as the maximum across rows. Live values, not a
 * transaction across fields. */
static inline void gw_rpc_accumulate(struct gw_rpc_counters *out,
                                     const struct gw_rpc_row *row) {
#define GW_RPC_SUM(name) \
  out->name += atomic_load_explicit(&row->name, memory_order_relaxed);
  GW_RPC_SCALARS(GW_RPC_SUM)
#undef GW_RPC_SUM

  uint64_t maximum = atomic_load_explicit(&row->max_ns, memory_order_relaxed);
  if (maximum > out->max_ns) {
    out->max_ns = maximum;
  }

  for (unsigned i = 0; i < GW_RPC_HISTOGRAM_BINS; i++) {
    out->latency_ns[i] +=
        atomic_load_explicit(&row->latency_ns[i], memory_order_relaxed);
  }
}
#else
#define GW_RPC_COUNT(field, amount) ((void)0)
#define GW_RPC_SCOPE(name, stage, api) ((void)0)
#define GW_RPC_SCOPE_END(name) ((void)0)
#endif
#endif
