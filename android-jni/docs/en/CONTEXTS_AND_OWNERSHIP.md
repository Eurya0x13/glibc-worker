# Contexts, reentry and registration ownership

[English](../en/CONTEXTS_AND_OWNERSHIP.md) | [简体中文](../zh/CONTEXTS_AND_OWNERSHIP.md)

This contract defines JNI thread identity, shared call frames and native-registration lifetime. See [execution scopes](../../../bionic-dlopen/docs/en/EXECUTION_SCOPES.md) for loader borrows and [failure handling](FAILURES.md) for unrecoverable errors.

## Thread binding and capacity

A Host thread's first bridge entry binds a private context and Guest worker until thread exit. Guest-created threads bind a context and Host JNI helper through AttachCurrentThread; the controller reclaims them after DetachCurrentThread. Contexts preserve JNI/TLS identity rather than transferring to unrelated threads after each call. Guest DetachCurrentThread cannot detach Host-origin threads.

`GW_CONTEXTS` is initial inline capacity, default 16 and build range 2..256, including owner context. Host callers and Guest attachments share one dynamic directory. Exhaustion grows by 16; both sides CAS-claim a FREE slot without waiting for older threads to release slots. Safe exit/detach/startup failure permits slot reuse while retaining capacity.

Ordinary frames start at 12 depths per context and grow by 16. Host commands and Guest JNI RPC use stable frame addresses; existing request/response, arguments and results never move. Twelve is not an execution limit. VM/native stacks, memory and signed-index overflow checks still bound recursion.

Growth failure preserves an exception or raises OutOfMemoryError at Host entry; Guest attach clears JNIEnv and returns JNI_ENOMEM. An independent emergency RPC can report failure, query/clear exceptions and perform required releases without allocating another frame. It does not bypass thread identity. Integer boundaries reject as exhaustion before signed overflow.

## Storage and publication

A four-level radix directory has 256 branches per level and contiguous 16-entry leaf pages. Initial entries are inline; the allocating libc uses mmap for extensions. Atomic release/acquire publishes directories/pages. Losing allocators munmap only unpublished candidates. Published pages never move/reclaim before process exit, preventing cross-domain dangling addresses.

Shared pages contain POD, atomic sequences and stable addresses only. pthread, JNIEnv, allocator and private thread records remain domain-owned and must not be dereferenced by the other side. Per-context reference indexes use Zig dynamic vectors; growing indexes never moves published reference slots.

Initial capacity is not a budget: bindings also need stacks, VM attachment and private state, subject to OS/VM limits. Reuse does not reset event epochs or discard frame directories. Shutdown/quarantine does not authorize revoking arbitrary work.

Guest owner fg_pump does not scan context capacity. After Host publishes FB_HOST_REQUEST or a worker publishes FB_DEAD, it posts the stable context index to an owner inbox and notifies. Empty pumping reads the inbox once. Creation/reaping cost follows changed contexts in a bounded batch, then owner commands resume rather than chasing continuous arrivals.

The inbox is a multi-producer/single-owner index stack with nodes embedded in stable contexts and zero as the head sentinel. It allocates no event objects and does not replace reference pools with linked lists. Each context is queued at most once; repeated posts coalesce but still use release RMW to publish latest state. Owner detaches a batch, saves next, acquire-clears the queued flag, then examines state. Pre-clear updates are acquired; post-clear updates requeue/notify. A plain-load early return cannot replace the RMW. Do not allow repost before saving next or clear inbox fields during slot reset. Producers only prepend and never read old-head nodes, avoiding removal-style ABA from index reuse.

This is not a FIFO event log: contexts have no global cross-thread order and processing observes latest lifecycle state. Per-context creation/exit/reuse remains FB_HOST_REQUEST → FB_RUNNING → FB_DEAD → FB_JOINED → FB_FREE, with FB_ATTACH_FAILED on startup failure. A producer paused between state publication and posting delays notification; no deadline/cancel guarantee is added.

Free-context allocation, the Host controller and one-time shutdown/diagnostics still scan capacity. This change did not alter those paths or the eight-thread waiting strategy. Bound Host worker calls never performed owner maintenance; their benchmark cannot measure the owner path.

Private protocol v6 adds owner inbox fields. Startup verifies shared/context/RPC/call sizes, initial context/frame capacities, leaf width and module limit. Deploy rebuilt Host/Guest together; v5 or older cannot mix. Handoff detail samples only the first 12 depths, while deeper calls execute and contribute to other metrics; that window is not an execution cap.

## Class registration and handoff

One logical module owns a Java class's bridged native registration at a time. Weak class references do not extend ClassLoader lifetime.

1. RegisterNatives reserves the class and holds module scope before publishing. Concurrent updates to a class with registration/unregistration in flight explicitly fail.
2. Preparation failure before VM entry withdraws new unpublished records/trampolines. VM RegisterNatives may partially publish even on failure; retain ownership/trampolines until safe unregister/unload.
3. Explicit UnregisterNatives of an owned class requires the same current module; without a current module, borrow its recorded owner. Reserve even unowned-class unregister to prevent racing new registration.
4. Successful VM unregister without pending exception removes class ownership, allowing handoff. Release only the class record; old module trampolines stay until module unload to protect other classes/in-flight calls.
5. Old unload unregisters only classes it still owns, never those transferred to a new owner.
6. Uncertain unregister of an owned class permanently quarantines, retaining records/trampolines/images for embedder process disposal; no automatic kill/recovery.

Reservations occur under the lock, VM RegisterNatives/explicit UnregisterNatives outside it. Callers still synchronize method execution with unload. Successful unregister does not prove cached entries held elsewhere are invalidated.

These rules follow class-level [JNI registration/relinking](https://docs.oracle.com/en/java/javase/26/docs/specs/jni/functions.html#registering-native-methods), not arbitrary per-method multiple ownership.

## Dependency function ownership

An active loading/native-call context owns registration, even when functions come from DT_NEEDED dependencies. Image address and lifecycle owner are distinct; a shared dependency address cannot identify one mod owner.

A Guest attached thread without current-module context uses strict reverse resolution: every function address must resolve to the same managed top-level link_map before borrowing/publishing. Unknown images, shared dependencies or mixed top-level modules reject the entire batch before VM publication. Do not inspect only the first address or choose by load order.

Context-free shared-dependency registration is unsupported; perform it through a known module call path. The bridge cannot prove arbitrary pointer/dependency lifetime. Guest-internal dlopen/dlclose remains module-owned.

## Validation entrypoints

- `python3 android-jni/tests/context_pages.py`: ASan/UBSan concurrent claims, stable addresses, 96-depth frames, page allocation failure/recovery.
- `python3 android-jni/tests/owner_work.py --output PATH`: production fg_pump pthread creation/reaping, failure recovery/reuse, 32 producers, coalescing/waits, and PROT_NONE extended pages at capacity 4096 proving empty pumping does not touch them. Default ASan/UBSan; `--sanitizer thread` selects TSan. Native execution is a stub, not JNI behavior proof.
- Add `--baseline-ref COMMIT`: same fixture/C -O1, capacities 16/256/1024/4096, one warmup and ten balanced paired measurements per capacity with fresh processes. `--android-ndk PATH --ffi-include PATH --device SERIAL` runs standalone bionic under shell UID and records thermal state. These are batch-average owner-pump CPU costs, not full TLS/JNI/RPC latency or game gains.
- Retained evidence: [owner-inbox-arm64.json](../../tests/results/owner-inbox-arm64.json).
- failures.py covers production RPC allocation/emergency/private/TLS/worker failures with substitutes; bindings.py/resources.py cover real libffi, class handoff/reentry rejection, quarantine, Zig owner growth/OOM.
- Android coverage runs three rounds of 32 Host/32 Guest threads, 32-level reentry, shared-dependency registration, background top-level reverse attribution, ambiguous/mixed rejection and post-unload ownership, alongside scope/mutex/JNI/rollback/final-zero-resource checks.

These do not establish full ART/JNI, arbitrary background work, user-lock/pin-cycle safety, deadlines or safe cancellation; see the lifecycle contract.
