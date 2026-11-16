# Waiting and RPC diagnostics

[English](../en/WAITING.md) | [简体中文](../zh/WAITING.md)

## Event contract

`include/wait_event.h` shares only a naturally aligned 32-bit atomic event word across libc domains, never pthread synchronization objects. The default notifier advances its sequence with release ordering, then broadcasts `FUTEX_WAKE_PRIVATE`; waiters sample with acquire and use `FUTEX_WAIT_PRIVATE`.

Loop in this order: sample the event, check/pump the actual predicate, then wait against the sampled value if still unsatisfied. Recheck on return. A notification between the check and WAIT makes kernel comparison return EAGAIN; EINTR and spurious wakes also return to the predicate loop. An event word is not the result, and a wake does not establish completion.

Wait/notify preserve caller errno. Futex failures other than EAGAIN/EINTR and required clock failures are runtime faults; the current implementation terminates the process. PRIVATE futexes are for a shared address space within one process, not independent processes.

Idle service threads block immediately. Only in-flight RPC uses bounded active waiting. `GW_RPC_SPIN_NS` is a monotonic-time window, default 20000ns and optionally zero, not a loop count or completion deadline. Shutdown must publish stop, broadcast and confirm relevant thread exits before context reuse.

## Build configuration

| Setting | Default | Constraint |
| --- | --- | --- |
| `GW_RPC_PROFILE` | 0 | 0/1; enables statistics and sampling clocks |
| `GW_RPC_SPIN_NS` | 20000 | 0..1000000ns, checked by workspace builds |
| `GW_FUTEX_WAKE_FILTER` | 0 | Experimental [event encoding](WAKE_FILTER.md) |
| `GW_RPC_READY_POLL` | 0 | Experimental response-generation polling; retains event checks for callbacks |

All shared-protocol participants must match. Composition records flags in each project's `build/<ABI>/generated/abi-flags.txt`; replacing only Host or Guest does not establish layout/notification compatibility.

## Optional statistics

`rpc_metrics.h` provides counters, scopes and snapshots; `rpc_handoff.h` samples paired requests. Normal profile-off builds omit statistics TLS, diagnostic lines and sampling clocks. Measure profile-on time/memory overhead separately.

Nested scopes distinguish inclusive from self time. CPU sampling measures the current thread; wall time includes preemption and waits. Live snapshots are not transactional: do not add counters from different instants or call sample mean multiplied by entry count exact total CPU.

A zero WAKE return only means no waiter was awakened by that syscall; it does not prove the notification is removable. With filtering, NOTIFY counts every notification but zero/positive outcomes count actual WAKE calls only.

## Paired handoff

About 1/64 requests record four monotonic timestamps: before publication, callee acceptance, completion and caller response observation. Reentry depths in one context use separate slots.

| Segment | Meaning |
| --- | --- |
| outbound | Before publish to acceptance: publication, notification, pumping and scheduling |
| execute | Acceptance to completion: computation, nested JNI/RPC and preemption |
| resume | Completion to response observation: publication, notification, pumping and scheduling |
| transport | outbound + resume; not pure memory transfer |
| execute_cpu | Callee thread CPU during execute |

Valid samples satisfy `total = outbound + execute + resume`. Slow-sample summaries and bounded outliers describe selected requests, not all tail frequencies. Wall minus CPU is not automatically scheduler delay; correlate scheduler traces to distinguish sleeping from runnable CPU wait.

## Tests

From core, run `ctest --test-dir build --output-on-failure`, or `python3 tests/wait_events.py`, `python3 tests/rpc_handoff.py`, `python3 tests/ready_poll.py`. Clang ASan/UBSan is required; output uses `build/test-work/`.

The wait matrix covers 32 configurations and controlled races. Handoff checks profile on/off, nesting, concurrent snapshots and clock faults. Ready polling covers on/off × profile on/off × 0/20000ns (eight configurations). `wait_events.py --ready-poll 1` repeats the full wait matrix. Desktop checks do not replace sustained ARM64 load validation.
