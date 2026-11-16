# Execution borrows and close rules

[English](../en/EXECUTION_SCOPES.md) | [简体中文](../zh/EXECUTION_SCOPES.md)

This contract prevents self-wait in synchronous reentry such as `A → Java → B → Java → close(A)`. Checking only innermost B misses active A; moving A to CLOSING and waiting for zero active calls prevents the current thread from returning A's borrow.

It also covers cross-thread borrow cycles: one thread holds A while closing B, another holds B while closing A. Rather than building a complete wait graph, close rejects waits for other borrows while the caller holds an execution scope.

## Two borrow kinds

| API | Meaning | Returning thread | Close by this thread |
| --- | --- | --- | --- |
| `gw_dlopen_scope_enter/leave` | Current Host thread executes a module or services its synchronous callback | Same thread required | BUSY if it holds the target, or holds any scope while target has active borrows |
| `gw_dlopen_enter/leave` | Keep an image alive, allowing work handoff | May be another thread | Normal close wait; no automatic self-wait detection |

Every successful enter increases module active count. Each leave consumes only its own borrow kind. Recursive scopes require one leave per enter; one leave cannot clear the chain. Scopes cannot transfer threads, force cancellation or auto-release on thread exit. Pair all normal/error exits.

## State rules

1. Only LOADING/LIVE modules in a READY backend admit borrows. Quarantine/counter exhaustion rejects without changing counts.
2. Under the lock, close checks whether this thread holds the target. If so, return `GW_ERROR/GW_E_BUSY` without changing state/resources.
3. If the target still has any scope/pin and this thread holds any scope, return BUSY unchanged. An unborrowed target may close; a thread without scopes may wait for other threads.
4. After transport current_module probing, recheck scopes and target active before LIVE → CLOSING. Retain current_module for old transports, but it cannot replace ancestor records.
5. CLOSING rejects new borrows; old borrows may return and the last wakes the closer.
6. Quarantine wakes waiters and prohibits teardown, while existing borrows may return. It does not prove Guest execution stopped.

BUSY means this close did not execute. It does not enqueue unloading/retry. Return from callbacks and release scopes, then close from a scope-free lifecycle thread under embedder synchronization; do not spin-retry while holding scopes. Even without a real cycle, an unrelated module temporarily borrowed by another thread is conservatively rejected.

## Storage and integration

Fixed module capacity 64 permits a contiguous 64-entry TLS array per Host thread, with full generation-tagged handles and recursive counts. It records held modules/counts, not order or a full call stack. It allocates no heap memory and imposes no separate depth limit. Old generations cannot match reused slots.

Only close with nonzero target active scans up to 64 thread-local entries. Scope enter/leave and each RPC gain no scan or wait-graph maintenance.

The loader holds a scope during transport.find. JNI holds scopes during native trampolines and registration publication, including synchronous Java callbacks and nested Guest calls. Return paths, including pending exceptions, release them. The innermost module field remains RPC/registration ownership metadata, not complete borrow history.

New C exports require matching bionic-dlopen artifacts. Scope APIs do not change the v1 query-table layout; JNI still requires matching shared-protocol artifacts. Updating headers alone while linking an old library is insufficient.

## Caller responsibilities

Registered scopes prevent a thread from entering loader active-borrow waits while holding any scope, rejecting cycles built from these scopes and loader.close waits without guessing pin owners.

This is not global deadlock freedom. Transferable pins have no thread owner; callers prevent pin-only cycles. transport.close/JNI_OnUnload, user mutexes, join and arbitrary RPC waits are outside these checks. Zero active borrows do not guarantee nonblocking teardown. Guest background threads, cached pointers and detached asynchronous work are not automatically registered.

The upper [JNI ownership contract](../../../android-jni/docs/en/CONTEXTS_AND_OWNERSHIP.md) owns context growth, class ownership and shared dependencies. This layer has no deadline/cancellation acknowledgement. Synchronize unload, stop/join background work and never free potentially referenced stacks/JNI frames merely after timeout; deadline/cancellation responsibilities remain with the lifecycle contract.

## Research context

[LRPC (1990)](https://graphics.stanford.edu/~tpurcell/quals/os/bers90.pdf) motivates explicit nested cross-domain execution records; this implementation does not adopt its kernel scheduling or domain termination.

[Linux SRCU rules](https://docs.kernel.org/core-api/kernel-api.html#c.srcu_read_lock) prohibit directly/indirectly waiting for one's own read-side section. This uses the same no-self-wait constraint with module locks, counts and condition variables, not the SRCU algorithm or its full grace-period machinery.

## Validation

- CTest `execution_scopes`, `close_waits`, `quarantine_0..3` exercise the production loader and C transport under ASan/UBSan, not dual libc. Barriers keep two/three threads holding scopes until all closes return; checks include LIVE state after rejection, pin waits, rechecking after probes and unborrowed-target close.
- `jvm-jni/tests/coverage/run.sh --device SERIAL` checks self-close, A→B closing A/B, A→B→A closing B and exception unwinding on Android JRE/dual libc. `AUDIT_SCOPE_TEST` requires four passing cases; existing loading/concurrency/final-zero-resource assertions remain.
- `AUDIT_CLOSE_WAIT_TEST` requires two actual JNI calls simultaneously callback into Java and receive BUSY on mutual close, followed by successful calls without premature resource release. Test barrier/join deadlines report failures, not production timeout/cancellation guarantees.
