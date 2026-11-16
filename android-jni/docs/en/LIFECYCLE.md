# Runtime lifecycle and process disposal

[English](../en/LIFECYCLE.md) | [简体中文](../zh/LIFECYCLE.md)

## Supported process model

One glibc-worker JNI runtime per process binds one explicitly selected JavaVM. Modules, ClassLoaders and JNI threads share it. Android entrypoint ART and embedded HotSpot are different VMs; after selecting HotSpot, never pass ART JNIEnv. Each JNIEnv remains owned by its attached thread.

Singleton operation is the supported configuration, not proof multiple GNU loaders/libcs can never coexist. Module unload does not destroy runtime. Concurrent runtimes, restart after shutdown and recovery from quarantine are unsupported. Core neither depends on nor selects JavaVM.

## States and entrypoints

`full_host_state()` is an atomic diagnostic snapshot readable by any thread, not a resource hold or guarantee of later call success.

| State | Meaning and allowed operations |
| --- | --- |
| NEW | Register optional adapter, then owner calls start |
| STARTING | One-time startup claimed; no parallel second startup |
| STARTED | Host initialized; owner starts matching Guest, waits for RPC readiness, then calls ready |
| READY | Loading/JNI allowed; owner coordinates work cessation before shutdown |
| STOPPING | Stop loader, restore hooks, wake/join Host helpers, release resources proven safe |
| STOPPED | Terminal; shared mappings/singleton remain, start/ready/shutdown rejected |
| QUARANTINED | Terminal; reject new Guest operations/shutdown, retain possibly used resources; embedder disposes process |

Configuration checks/missing adapter before claim leave NEW. Any initialization failure after claim, handshake failure, hook restore failure or shutdown join failure quarantines; do not send partially initialized state through ordinary shutdown. Lifecycle state changes do not replace pending exceptions.

Embedder serializes adapter registration/lifecycle calls. ready/shutdown must use the successful start thread; wrong thread, repetition or wrong state returns JNI_ERR without borrowing another thread's JNIEnv. Atomic startup claim selects one initializer; it does not legalize concurrent unrelated operations during startup.

## Orderly shutdown

1. Close business entrypoints; prohibit new loads, native calls and attaches; stop producing work.
2. Let in-flight calls and reentrant callbacks complete while servicing requests. Stop/join module background threads. Host bindings become idle with no further requests guaranteed by the embedder; Guest attachments detach and finish reclamation. Host threads may exit; idle internal JVM threads need not exit first.
3. Close all modules outside execution scopes. Synchronize cached JNI/native entries; borrow counts do not prove background work stopped.
4. Startup owner calls full_host_shutdown outside reentry. FREE or coordinated idle Host RUNNING contexts with no requests are allowed; Guest attachment/handoff reclamation must finish. Every ordinary/emergency request needs a response; RPC active must be zero. Existing modules/load/close transactions reject shutdown. JOINED alone does not prove Host TLS destruction finished.
5. After success, the embedder requests and confirms GNU service main-thread exit, then follows its own DestroyJavaVM lifecycle. Bridge shutdown does not destroy VM/libc/loader mappings.

Shutdown is synchronous without a deadline. BUSY/nonquiescent rejection preserves READY for retry after coordination. It does not auto-unload, cancel native calls or bound pthread join/third-party destructors. Stopped producers are a prerequisite; conservative context checks are not a concurrent admission barrier.

## Stalls, cancellation and deadlines

There is no universal cancellation protocol for arbitrary native/JNI calls. Published arguments may reference caller stacks, JNI frames, shared buffers or Guest addresses. A watchdog deadline only establishes noncompletion, not cessation of address access.

| Stage | Embedder action |
| --- | --- |
| Work not yet admitted | Cancel in the business queue; no bridge resource borrowed |
| Published call without completion confirmation | Retain stack, references, buffers, scopes and images; never forge response or force timeout return |
| Module offers cooperative cancellation | Request it; reclaim only after confirmation that borrowed resources will no longer be accessed, or actual original completion |
| Deadline exceeded without safety confirmation | Independently mark gw_dlopen_quarantine, stop new entry and require execution-process disposal |

Quarantine does not acknowledge cancellation or release arbitrary futex/user-lock/I/O/JNI/join waits. Calls may wait forever or unwind after a genuine late response; quarantine remains irreversible in either case. Completed safe cleanup is not rolled back; currently executing close code is not promised to stop.

Do not fake safe reclamation through pthread_cancel, killing one Guest thread, longjmp, response-sequence edits or premature VM destruction. Moving shared frames onto the heap does not extend indirectly referenced stacks/JNI object lifetimes.

## Supervision and fault boundaries

The embedder chooses deadlines, diagnostics and process disposal. The project sets no universal mod timeout and never automatically kills the app. Supervision cannot depend on a blocked call returning; use another process if disposal must remain possible after a process-wide stall.

An optional deployment is launcher/supervisor → game execution process, the latter hosting bionic, JRE and glibc-worker. JNI still bridges in-process; the control channel handles startup/status/exit only. This is not a new Android service or JNI RPC implementation.

Runtime and modules provide no memory/crash isolation. Corruption, fatal signals, exit and process-wide resource conflicts can affect the entire process.

## Validation

`python3 android-jni/tests/lifecycle.py` runs nine independent production-Host/VM-substitute cases under ASan/UBSan: startup race, normal stop, mapping/JNI-version/adapter-install/controller-create/join/adapter-restore/handshake failures. It covers wrong thread, repeated shutdown, reentry/in-flight/unreclaimed handoff rejection, idle Host acceptance and restart rejection, without GNU Guest execution.

`tests/audit.py` drives production fh_command: quarantine wakes but retains borrowed stacks/frames; only a genuine late response permits return. A test parent kills a disposable child for permanent loss. Substitute VM/Guest and parent deadlines are not production guarantees. bionic-dlopen close_waits/loader_quarantine cover borrow waits and retained CLOSING state.

Android smoke/coverage emits RUNTIME_LIFECYCLE_TEST on real JRE/dual libc, checking second start, wrong-thread shutdown, terminal states, repetition and restart rejection, followed by Guest main-thread exit and VM destruction. Destructive stall cases stay in disposable desktop children. Normal device success does not imply arbitrary cancellation or full ART support.
