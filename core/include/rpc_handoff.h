#ifndef GW_RPC_HANDOFF_H
#define GW_RPC_HANDOFF_H
#include <stddef.h>
#include <stdint.h>

#define GW_HANDOFF_ABI 1u
#define GW_HANDOFF_BINS 64u
#define GW_HANDOFF_KEEP 16u
#define GW_HANDOFF_DEPTH 12u
#define GW_HANDOFF_SAMPLE_SHIFT 6u
#define GW_HANDOFF_SLOW_NS UINT64_C(1000000)

enum {
  GW_HANDOFF_TOTAL,
  GW_HANDOFF_OUTBOUND,
  GW_HANDOFF_EXECUTE,
  GW_HANDOFF_RESUME,
  GW_HANDOFF_TRANSPORT,
  GW_HANDOFF_COMPONENTS
};

// clang-format off
#define GW_HANDOFF_COUNTS(X) \
  X(calls) X(samples) X(clock_errors) \
  X(total_ns) X(outbound_ns) X(execute_ns) X(resume_ns) X(transport_ns) \
  X(execute_cpu_ns) X(slow_samples) X(slow_outbound_ns) X(slow_execute_ns) \
  X(slow_resume_ns) X(slow_execute_cpu_ns)
#define GW_HANDOFF_RECORD(X) \
  X(start_ns) X(end_ns) X(total_ns) X(outbound_ns) \
  X(execute_ns) X(resume_ns) X(transport_ns) X(execute_cpu_ns) \
  X(function) X(module) X(api) X(context) X(depth) X(host_tid) \
  X(guest_tid) X(request)
// clang-format on

struct gw_handoff_record {
#define GW_HANDOFF_VALUE(name) uint64_t name;
  GW_HANDOFF_RECORD(GW_HANDOFF_VALUE)
#undef GW_HANDOFF_VALUE
};

struct gw_handoff_summary {
#define GW_HANDOFF_VALUE(name) uint64_t name;
  GW_HANDOFF_COUNTS(GW_HANDOFF_VALUE)
#undef GW_HANDOFF_VALUE
  uint64_t histogram[GW_HANDOFF_COMPONENTS][GW_HANDOFF_BINS];
  struct gw_handoff_record worst[GW_HANDOFF_KEEP];
};

struct gw_handoff_snapshot {
  uint32_t abi_version, struct_size, contexts, sample_shift;
  uint64_t monotonic_ns;
  /* Native Host->Guest and JNI Guest->Host are separate populations. */
  struct gw_handoff_summary host, guest;
};

/* Optional diagnostic only. Each selected request has four matched monotonic
 * timestamps: publication, dispatch, completion, caller observation. Execute
 * includes nested RPC/JNI and callee preemption; it is not pure algorithm CPU.
 * Transport includes notification, pumping, scheduling and observation, not
 * just memory transfer. The sampled histograms are not full-population tails.
 * Summary fields are live atomic snapshots, not a transaction. Retained worst
 * records are individually coherent, cumulative and ranked by transport time.
 */
#if GW_RPC_PROFILE
int gw_rpc_handoff_snapshot(struct gw_handoff_snapshot *, size_t);
#endif
#endif
