#ifndef GW_RPC_METRICS_H
#define GW_RPC_METRICS_H
#include <stddef.h>
#include <stdint.h>
#include "rpc_handoff.h"

#ifndef GW_RPC_PROFILE
#define GW_RPC_PROFILE 0
#endif
#ifndef GW_RPC_SPIN_NS
#define GW_RPC_SPIN_NS 20000
#endif
#if GW_RPC_PROFILE != 0 && GW_RPC_PROFILE != 1
#error GW_RPC_PROFILE must be 0 or 1
#endif
#if GW_RPC_SPIN_NS < 0
#error GW_RPC_SPIN_NS must be nonnegative
#endif

#define GW_RPC_METRICS_ABI 1u
#define GW_RPC_HISTOGRAM_BINS 64u

/* Cumulative counters, not durations with Guest/JNI work subtracted. Host RPC
 * latency is FB_NATIVE dispatch through response; Guest latency is a JNI RPC
 * through response. Recursive calls are individually inclusive and must not be
 * added together as disjoint CPU time. Wait counters cover that libc domain's
 * workers and controller, including idle/control traffic while a metrics row
 * is owned. The final Guest detach/failed-attach control wake occurs after
 * retiring its row, before slot reuse, and is deliberately not counted. */
struct gw_rpc_counters {
  uint64_t started, completed, inflight;
  uint64_t elapsed_ns, max_ns;
  /* floor(log2(ns)); bucket zero includes 0 and 1 ns. */
  uint64_t latency_ns[GW_RPC_HISTOGRAM_BINS];
  uint64_t active_waits, ready_before_spin, spin_hits, spin_exhausted;
  uint64_t spin_ns;
  uint64_t wait_syscalls, wait_success, wait_eagain, wait_eintr;
  uint64_t wake_syscalls, wake_threads;
  uint64_t syscall_errors, clock_errors;
};

struct gw_rpc_snapshot {
  uint32_t abi_version, struct_size, enabled, contexts;
  uint64_t monotonic_ns, spin_budget_ns;
  struct gw_rpc_counters host, guest;
};

#define GW_RPC_BREAKDOWN_ABI 2u
#define GW_RPC_API_SLOTS 235u
#define GW_RPC_SAMPLE_SHIFT 6u

enum gw_rpc_stage {
  GW_RPC_HOST_TRAMPOLINE_PRE,
  GW_RPC_HOST_TRAMPOLINE_POST,
  GW_RPC_HOST_COMMAND_PACK,
  GW_RPC_HOST_COMMAND_FINISH,
  GW_RPC_HOST_JNI_GLUE,
  GW_RPC_HOST_JNI_EXECUTE,
  GW_RPC_GUEST_NATIVE_PREPARE,
  GW_RPC_GUEST_NATIVE_EXECUTE,
  GW_RPC_GUEST_RPC_PACK,
  GW_RPC_SPIN,
  GW_RPC_FUTEX_WAIT,
  GW_RPC_NOTIFY,
  GW_RPC_STAGE_COUNT
};

enum gw_rpc_notify_outcome {
  GW_RPC_NOTIFY_ZERO_RETURN,
  GW_RPC_NOTIFY_POSITIVE_RETURN,
  GW_RPC_NOTIFY_OUTCOME_COUNT
};

struct gw_rpc_stage_cost {
  uint64_t calls, samples, wall_ns, cpu_ns, clock_errors;
};

struct gw_rpc_api_cost {
  /* Existing uint32_t protocol hits, widened without wrap correction. These
   * include local VM APIs; served_calls counts Host-dispatched requests,
   * executed_calls counts entries into the primary JNI call/helper. */
  uint64_t existing_hits, served_calls, executed_calls, samples, wall_ns,
      cpu_ns, clock_errors;
};

struct gw_rpc_breakdown {
  uint32_t abi_version, struct_size, enabled, contexts;
  uint32_t sample_shift, stage_count, api_slots, reserved;
  uint64_t monotonic_ns, spin_budget_ns;
  struct gw_rpc_stage_cost host[GW_RPC_STAGE_COUNT], guest[GW_RPC_STAGE_COUNT];
  struct gw_rpc_api_cost api[GW_RPC_API_SLOTS];
  /* Per-domain minimum empty measured interval from 32 startup trials.
   * Raw samples retain clock overhead; these floors are not subtracted. */
  struct gw_rpc_stage_cost host_clock_floor, guest_clock_floor;
  /* ABI 2: the same NOTIFY samples classified by WAKE's returned count.
   * Zero means no thread was woken, not proof that notification was useless.
   * Both include epoch update, userspace accounting and the entire syscall.
   * These duplicate NOTIFY totals and must not be added to that category. */
  struct gw_rpc_stage_cost host_notify[GW_RPC_NOTIFY_OUTCOME_COUNT];
  struct gw_rpc_stage_cost guest_notify[GW_RPC_NOTIFY_OUTCOME_COUNT];
};

/* Breakdown samples are exclusive only among these nested scopes on the same
 * thread. Even an unsampled child pauses its sampled parent. JNI_EXECUTE is
 * the requested primary JNI call (RegisterNatives includes its bridge helper);
 * supporting JNI calls used by glue remain in JNI_GLUE. Guest native execution
 * includes uninstrumented module work, excluding nested RPC scopes. CPU uses
 * CLOCK_THREAD_CPUTIME_ID; wall minus CPU is not a scheduler measurement.
 * calls are exact diagnostic entries, samples count completed valid samples.
 * A PRNG selects approximately 1/64 entries independently of call position.
 * Do not add inclusive RPC latency to exclusive breakdown stage durations.
 * Live snapshots are nontransactional, and sampled totals are not measured
 * full-population totals. Profile overhead includes nested pause/resume clocks.
 * Coverage is deliberately incomplete: cold context acquisition, generated
 * Guest wrapper argument construction, pump/predicate orchestration and native
 * response bookkeeping have no dedicated category. Uninstrumented wrapper
 * work inside a Guest native call remains in GUEST_NATIVE_EXECUTE. Notify and
 * wait scopes include their userspace preparation/accounting and syscall;
 * they do not measure kernel CPU separately. A category sum is not total bridge
 * CPU. Host trampoline pre starts after context acquisition; Guest native
 * prepare starts after module lookup/validation. No timing is done for loader
 * cold paths except any nested wait/notify scopes. Retiring a row discards an
 * unfinished sampled scope rather than writing a slot that may be reused.
 */

/* Diagnostic builds only. The caller owns the POD output. Returns 0, or -1
 * with errno (EINVAL for size/output, ENODEV before a matching Guest is ready).
 * A live snapshot is approximate across independent fields/contexts; the
 * exporter never requests an RPC or resets counters. */
#if GW_RPC_PROFILE
int gw_rpc_diagnostics_snapshot(struct gw_rpc_snapshot *, size_t);
int gw_rpc_breakdown_snapshot(struct gw_rpc_breakdown *, size_t);
#endif
#endif
