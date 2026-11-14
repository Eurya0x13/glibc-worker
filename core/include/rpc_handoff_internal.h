#ifndef GW_RPC_HANDOFF_INTERNAL_H
#define GW_RPC_HANDOFF_INTERNAL_H
#include "rpc_handoff.h"
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

/* Request/response release/acquire publishes this scratch slot between exactly
 * one caller and one callee. A different slot is used at every reentry depth.
 * Live exporters never inspect these non-atomic scratch fields. */
struct gw_handoff_exchange {
  int sampled, failed;
  uint64_t start, dispatch, complete, cpu_start, cpu_end;
};

/* gw_handoff_record with every member atomic so a live exporter can copy it
 * lock-free; version is the seqlock that makes the relaxed payload copy
 * coherent (see gw_handoff_read). One writer per record. */
struct gw_handoff_atomic_record {
  _Atomic uint64_t version;
#define GW_HANDOFF_ATOMIC(name) _Atomic uint64_t name;
  GW_HANDOFF_RECORD(GW_HANDOFF_ATOMIC)
#undef GW_HANDOFF_ATOMIC
};

/* Per-context row owned by exactly one calling domain; the peer writes into
 * it only through the exchange stamps (gw_rpc_peer_handoff). Counts and the
 * histogram are atomic for live export; the worst records are versioned
 * snapshots; exchange/sample_state are single-writer scratch that a live
 * exporter never reads. */
struct gw_handoff_row {
#define GW_HANDOFF_ATOMIC(name) _Atomic uint64_t name;
  GW_HANDOFF_COUNTS(GW_HANDOFF_ATOMIC)
#undef GW_HANDOFF_ATOMIC
  _Atomic uint64_t histogram[GW_HANDOFF_COMPONENTS][GW_HANDOFF_BINS];
  struct gw_handoff_atomic_record worst[GW_HANDOFF_KEEP];
  struct gw_handoff_exchange exchange[GW_HANDOFF_DEPTH];
  uint64_t sample_state;
};

/* Clock in ns for the handoff stamps. Preserves the caller's errno so
 * measurement never perturbs the RPC path. UINT64_MAX is the failure
 * sentinel; every caller latches it via .failed to drop the sample. */
static inline uint64_t gw_handoff_clock(clockid_t clock) {
  int saved = errno;
  struct timespec value;
  int rc = clock_gettime(clock, &value);
  errno = saved;
  return rc ? UINT64_MAX
            : (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

/* Single-writer counter increment: a relaxed load+store, deliberately not an
 * RMW, so an exporter reading the same word never contends with the writer.
 * Correct only with exactly one writer per counter. */
static inline void gw_handoff_add(_Atomic uint64_t *value, uint64_t amount) {
  atomic_store_explicit(
      value, atomic_load_explicit(value, memory_order_relaxed) + amount,
      memory_order_relaxed);
}

/* Caller side, immediately before publishing the request; the release/acquire
 * publication is what orders these stamps against the peer's reads. row is
 * the caller's own row and must be non-NULL (unlike dispatch/complete).
 * Counts every call, then advances the per-row PRNG; only a selected
 * (~1/64) request reads its first clock, and .failed latches a failure so
 * the later stages skip their clock reads. */
static inline void gw_handoff_begin(struct gw_handoff_row *row,
                                    unsigned depth) {
  gw_handoff_add(&row->calls, 1);

  uint64_t x = row->sample_state;
  if (!x) {
    x = UINT64_C(0xd1b54a32d192ed03) ^ (uintptr_t)row;
  }
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  row->sample_state = x;

  struct gw_handoff_exchange *exchange = &row->exchange[depth];
  *exchange = (struct gw_handoff_exchange){
      .sampled = ((x * UINT64_C(2685821657736338717)) >>
                  (64 - GW_HANDOFF_SAMPLE_SHIFT)) == 0};

  if (exchange->sampled) {
    exchange->start = gw_handoff_clock(CLOCK_MONOTONIC);
    exchange->failed = exchange->start == UINT64_MAX;
  }
}

/* Callee side, on accepting the request: records the second stamp pair into
 * the caller's row. row comes from gw_rpc_peer_handoff and is NULL until the
 * caller's domain published its rows; a NULL row simply leaves the stamps
 * absent, and finish then drops the sample. */
static inline void gw_handoff_dispatch(struct gw_handoff_row *row,
                                       unsigned depth) {
  if (!row) {
    return;
  }

  struct gw_handoff_exchange *exchange = &row->exchange[depth];
  if (exchange->sampled && !exchange->failed) {
    exchange->dispatch = gw_handoff_clock(CLOCK_MONOTONIC);
    exchange->cpu_start = gw_handoff_clock(CLOCK_THREAD_CPUTIME_ID);
    exchange->failed =
        exchange->dispatch == UINT64_MAX || exchange->cpu_start == UINT64_MAX;
  }
}

/* Callee side, when execution completes: records the third stamp pair (the
 * CPU pair brackets the execute interval) into the caller's row. Only a
 * request selected at begin with no clock failure so far pays for the reads. */
static inline void gw_handoff_complete(struct gw_handoff_row *row,
                                       unsigned depth) {
  if (!row) {
    return;
  }

  struct gw_handoff_exchange *exchange = &row->exchange[depth];
  if (exchange->sampled && !exchange->failed) {
    exchange->cpu_end = gw_handoff_clock(CLOCK_THREAD_CPUTIME_ID);
    exchange->complete = gw_handoff_clock(CLOCK_MONOTONIC);
    exchange->failed =
        exchange->complete == UINT64_MAX || exchange->cpu_end == UINT64_MAX;
  }
}

/* Retains the GW_HANDOFF_KEEP worst requests of the row, ranked by
 * transport_ns: the slot holding the smallest retained transport is replaced,
 * and only on a strictly greater value, so ties keep the older record.
 * Coherence for live readers comes from the version pair below. */
static inline void gw_handoff_keep(struct gw_handoff_row *row,
                                   const struct gw_handoff_record *value) {
  unsigned selected = 0;
  for (unsigned i = 1; i < GW_HANDOFF_KEEP; i++) {
    if (atomic_load_explicit(&row->worst[i].transport_ns,
                             memory_order_relaxed) <
        atomic_load_explicit(&row->worst[selected].transport_ns,
                             memory_order_relaxed)) {
      selected = i;
    }
  }

  struct gw_handoff_atomic_record *record = &row->worst[selected];
  if (value->transport_ns <=
      atomic_load_explicit(&record->transport_ns, memory_order_relaxed)) {
    return;
  }

  /* All payload members are atomic; the version pair also prevents exporting
   * a plausible-looking mix of two different requests. One writer per row. */
  atomic_fetch_add_explicit(&record->version, 1, memory_order_acq_rel);
#define GW_HANDOFF_STORE(name) \
  atomic_store_explicit(&record->name, value->name, memory_order_relaxed);
  GW_HANDOFF_RECORD(GW_HANDOFF_STORE)
#undef GW_HANDOFF_STORE
  atomic_fetch_add_explicit(&record->version, 1, memory_order_release);
}

/* Caller side, after observing the response: splits the four stamps into the
 * outbound/execute/resume components. Requires them present and ordered; a
 * clock failure or a missing peer stamp is charged to clock_errors and the
 * sample is dropped rather than recorded with a wrong split. Valid samples
 * feed the totals, the log2 histogram and the slow (>= GW_HANDOFF_SLOW_NS)
 * bucket, then gw_handoff_keep. row is the caller's own row (non-NULL). */
static inline void gw_handoff_finish(struct gw_handoff_row *row, unsigned depth,
                                     struct gw_handoff_record value) {
  struct gw_handoff_exchange *exchange = &row->exchange[depth];
  if (!exchange->sampled) {
    return;
  }
  uint64_t end = gw_handoff_clock(CLOCK_MONOTONIC);

  if (exchange->failed || end == UINT64_MAX ||
      exchange->start > exchange->dispatch ||
      exchange->dispatch > exchange->complete || exchange->complete > end ||
      exchange->cpu_end < exchange->cpu_start) {
    gw_handoff_add(&row->clock_errors, 1);
    return;
  }

  value.start_ns = exchange->start;
  value.end_ns = end;
  value.outbound_ns = exchange->dispatch - exchange->start;
  value.execute_ns = exchange->complete - exchange->dispatch;
  value.resume_ns = end - exchange->complete;
  value.transport_ns = value.outbound_ns + value.resume_ns;
  value.total_ns = end - exchange->start;
  value.execute_cpu_ns = exchange->cpu_end - exchange->cpu_start;

  gw_handoff_add(&row->samples, 1);
#define GW_HANDOFF_TIME(name) gw_handoff_add(&row->name, value.name);
  GW_HANDOFF_TIME(total_ns)
  GW_HANDOFF_TIME(outbound_ns)
  GW_HANDOFF_TIME(execute_ns)
  GW_HANDOFF_TIME(resume_ns)
  GW_HANDOFF_TIME(transport_ns)
  GW_HANDOFF_TIME(execute_cpu_ns)
#undef GW_HANDOFF_TIME

  uint64_t times[GW_HANDOFF_COMPONENTS] = {value.total_ns, value.outbound_ns,
                                           value.execute_ns, value.resume_ns,
                                           value.transport_ns};
  for (unsigned i = 0; i < GW_HANDOFF_COMPONENTS; i++) {
    unsigned bin = times[i] ? 63u - (unsigned)__builtin_clzll(times[i]) : 0;
    gw_handoff_add(&row->histogram[i][bin], 1);
  }

  if (value.total_ns >= GW_HANDOFF_SLOW_NS) {
    gw_handoff_add(&row->slow_samples, 1);
    gw_handoff_add(&row->slow_outbound_ns, value.outbound_ns);
    gw_handoff_add(&row->slow_execute_ns, value.execute_ns);
    gw_handoff_add(&row->slow_resume_ns, value.resume_ns);
    gw_handoff_add(&row->slow_execute_cpu_ns, value.execute_cpu_ns);
  }

  gw_handoff_keep(row, &value);
}

/* Export side: copies one worst record into a plain struct. Version 0 means
 * never written and an odd version means mid-update; both report the record
 * as absent. The copy is usable only if the version is even and unchanged
 * after the payload loads. */
static inline int gw_handoff_read(const struct gw_handoff_atomic_record *source,
                                  struct gw_handoff_record *value) {
  uint64_t version =
      atomic_load_explicit(&source->version, memory_order_acquire);
  if (!version || (version & 1)) {
    return 0;
  }

#define GW_HANDOFF_LOAD(name) \
  value->name = atomic_load_explicit(&source->name, memory_order_relaxed);
  GW_HANDOFF_RECORD(GW_HANDOFF_LOAD)
#undef GW_HANDOFF_LOAD

  atomic_thread_fence(memory_order_acquire);
  return version ==
         atomic_load_explicit(&source->version, memory_order_relaxed);
}

/* Live merge of one row into a summary: counts and histogram are summed with
 * relaxed loads, so the result is not a transaction across fields. The
 * retained worst records are read via gw_handoff_read and merged into a
 * transport-descending top-KEEP list; records that lose every comparison are
 * dropped. */
static inline void gw_handoff_accumulate(struct gw_handoff_summary *out,
                                         const struct gw_handoff_row *row) {
#define GW_HANDOFF_SUM(name) \
  out->name += atomic_load_explicit(&row->name, memory_order_relaxed);
  GW_HANDOFF_COUNTS(GW_HANDOFF_SUM)
#undef GW_HANDOFF_SUM

  for (unsigned i = 0; i < GW_HANDOFF_COMPONENTS; i++) {
    for (unsigned j = 0; j < GW_HANDOFF_BINS; j++) {
      out->histogram[i][j] +=
          atomic_load_explicit(&row->histogram[i][j], memory_order_relaxed);
    }
  }

  for (unsigned i = 0; i < GW_HANDOFF_KEEP; i++) {
    struct gw_handoff_record value;
    if (!gw_handoff_read(&row->worst[i], &value)) {
      continue;
    }
    for (unsigned j = 0; j < GW_HANDOFF_KEEP; j++) {
      if (value.transport_ns > out->worst[j].transport_ns) {
        for (unsigned k = GW_HANDOFF_KEEP - 1; k > j; k--) {
          out->worst[k] = out->worst[k - 1];
        }
        out->worst[j] = value;
        break;
      }
    }
  }
}

/* Implemented by the embedding, not core: returns the OTHER domain's row for
 * a context so the callee can stamp the caller's exchange. NULL until that
 * domain published its rows; only dispatch/complete tolerate a NULL row. */
struct gw_handoff_row *gw_rpc_peer_handoff(unsigned context)
    __attribute__((visibility("hidden")));
#endif
