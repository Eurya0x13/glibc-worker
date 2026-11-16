# Experimental wake filtering

[English](../en/WAKE_FILTER.md) | [简体中文](../zh/WAKE_FILTER.md)

`GW_FUTEX_WAKE_FILTER=0` is the default protocol. Value 1 uses the low bit of the same four-byte event word as a sticky waiter hint and its upper 31 bits as an epoch. No shared field is added, but encoding changes: Host, Guest and embedded entrypoints must be rebuilt together.

Notify calls `FUTEX_WAKE_PRIVATE(INT_MAX)` only if the prior word had a hint. Epoch still advances without a hint, preserving publication. After confirming the epoch is unchanged, a waiter CAS-sets the hint and waits on the entire epoch|hint word. Wait returns never clear it; only notify atomically advances epoch and clears hint in one CAS. Stale hints cause extra WAKE calls only.

## Lost-wakeup argument and limits

Let W be a successful hint-setting CAS and N a successful epoch-advance/hint-clear CAS. Their shared atomic word establishes modification order:

- N before W: W's old-word CAS fails or the next epoch check returns. A kernel WAIT already reached sees the full word changed and returns EAGAIN.
- W before N: N observes the hint and issues WAKE. An enqueued waiter wakes; a waiter not yet enqueued fails comparison because N changed the word.
- Waiters share a sticky hint. Normal return, EINTR, reentry or departure never clears another waiter's indication. Multiple notifiers use one CAS; epoch advance and hint clearing cannot be split.

Wait CAS uses acq_rel/acquire; notify uses release/relaxed. This is not a two-atomic acquire/release argument that incorrectly ignores store buffering. Sample→pump/check→wait, predicate loops, shutdown broadcasts and reuse after join remain unchanged. The protocol does not switch thread pointers or libc TLS.

See [FUTEX_WAIT](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html) for atomic comparison/enqueue and [FUTEX_WAKE](https://man7.org/linux/man-pages/man2/FUTEX_WAKE.2const.html) for broadcast/results. The userspace argument is this project's inference from those interfaces; tests are not a complete proof.

Like the original protocol, the argument excludes ABA from a full epoch wrap during observation. The candidate reduces epoch width from 32 to 31 bits and does not solve indefinite suspension/full wrap. Raw sequence values no longer count notifications: samples clear the hint and each notification adds two. Hints/readiness/counters do not prove actual kernel blocking.

## Startup matching

Before loading activation, candidate Host writes HELLO to idle call[0] POD argument 7. Guest verifies it, writes ACK to argument 8, then publishes the existing phase=2 release. Host acquire-reads phase before ACK and retains success privately so later call[0] reuse cannot corrupt validation. Existing RPC diagnostic arguments 0..6 and operation fields stay unchanged. This runs even with profile=0.

Old Guest does not generate ACK; candidate Guest rejects old Host without HELLO. Handle mismatch in a disposable independent process: candidate shutdown notification cannot safely wake a Guest sleeping under the old protocol. Default zero keeps original startup. This is not universal configuration negotiation.

## Statistics and measurement

NOTIFY still covers every notification; wake_syscalls and ABI2 zero/positive outcomes cover actual WAKE calls only. Skipped calls are not zero returns, so outcome totals may be below NOTIFY. Real A/B uses profile=0, spin=20000, identical fixtures/Java payload, no sampling/JIT symbol export/diagnostics, balanced A/B/B/A and complete save/exit/database verification. Desktop syscall reduction does not establish game gains.

`GW_EVENT_TEST_HOOK` exists only when a test translation unit supplies it; normal builds leave it empty. Points are: 1 after registration, 2 before WAIT, 3 after WAIT return, 4 after notify CAS. Tests use the production protocol.

## Deployment status

Filtering remains off by default. Enabling it requires matched Host/Guest, full race checks and diagnostic-free real workload A/B; syscall reduction alone is insufficient. Rollback must replace both sides together.
