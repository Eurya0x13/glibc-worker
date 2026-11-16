# C loading backend contract

[English](../en/LOADER.md) | [简体中文](../zh/LOADER.md)

`include/dlopen_backend.h` is JNI-independent. The backend owns Host logical state; caller-provided transport performs actual GNU loading/execution in the correct Guest domain.

## Initialization

Provide transport, image resolver and optional observer through `gw_dlopen_configure`, then activate when Guest can serve. Function pointers, image_context and observer_context must remain valid through stop. Per-call `void *` contexts for open/find/close are borrowed and may be NULL; this layer stores no JNIEnv. Multiple independent runtimes cannot currently be configured in one process.

Transport load/close/find/current-module does not prescribe JavaVM, JNIEnv or JNI descriptors. Upper adapters interpret metadata/binding descriptions; the resolver owns path deployment policy. This layer neither validates arbitrary function prototypes nor generates arbitrary ABI trampolines.

## Operations

Query `gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof(struct gw_dlopen_api_v1))`, checking NULL/version/size. Open returns a generation-tagged logical handle, never a system dlopen pointer. Find returns an entry satisfying the caller/transport ABI contract.

AUTO classifies GNU ELF markers; explicit GUEST needs caller-confirmed ABI. GW_PASS permits platform handling. GW_ERROR must not trigger a blind bionic retry of the GNU file. owns_handle also recognizes retired Guest tags to prevent wrong fallback.

The same device/inode cannot have duplicate active opens. Safe load-failure rollback restores state; generations reject stale handles after reuse. Capacity is fixed at 64 modules, not an unlimited runtime namespace.

## Concurrency and close

Pair `gw_dlopen_scope_enter/leave` while executing module code or servicing synchronous callbacks. Scopes belong to the current Host thread and must return there before exit. Failed enter has no matching leave; recursion returns each count. Closing any held ancestor returns GW_E_BUSY before CLOSING, without teardown/quarantine. Find automatically scopes transport lookup but does not permanently pin its returned entry.

`gw_dlopen_enter/leave` supplies transferable lifetime pins, not execution ownership. Borrow kinds are accounted separately. scope_leave on stale, unheld or another thread's scope returns zero unchanged. A pin holder closing its own target still waits; return it first or arrange another thread's return. Pins do not prevent self-wait.

Holding any execution scope while closing a target with active scopes/pins returns BUSY. Unborrowed targets may close; scope-free lifecycle threads may wait. BUSY schedules no retry. Leave the callback/scope before closing; do not spin inside it.

An allowed close blocks new borrows, waits for existing returns and calls transport unload. There is no full wait graph or automatic tracking of background work/cached entries. Caller duties cover pin-only cycles, teardown and arbitrary waits; see [execution scopes](EXECUTION_SCOPES.md).

Stop cannot succeed with loading, live or closing modules. Observers execute under the backend lock for lightweight counting only; never reenter or block. Callers own error objects; paths/symbols are borrowed only during the call.

## Unrecoverable cleanup

When transport cannot confirm rollback/unload safety, call `gw_dlopen_quarantine()`. It is irreversible, observable through `gw_dlopen_is_quarantined()`, and does not kill the process. Stop using the Guest bridge and discard its process; no same-process reconfiguration/recovery.

After quarantine, Guest open/find/close, activate/configure/stop return GW_ERROR/GW_E_QUARANTINED and enter rejects borrows. Waiting closers wake and exit without teardown. Failed transactions retain LOADING/CLOSING slots/resources without handle reuse; old borrows may leave. Ordinary platform handles may still PASS; GNU files never fall back to bionic.

Transport must retain images, entries and contexts possibly referenced by VM/Guest threads. Quarantine is not a sandbox, background-thread termination or call cancellation; synchronization duties remain.

## Validation

CTest loader_contract covers transactions/generations/busy close/stop; execution_scopes covers ancestors, recursion, 64-module borrows, ownership, pin/scope separation and cross-thread close; close_waits covers two/three-thread cycles, pin waits and unborrowed targets; quarantine_0..3 covers failed load/close, waiting pin/scope closers, terminal quarantine and retention. These do not independently establish dual libc. ELF inputs are explicit:

```sh
python3 tests/elf_route.py --libraries /path/to/test-dsos \
  --core-include /path/to/core/include --output build/elf-route.json
```

Set `GW_ABI=arm64-v8a` for ARM64 artifacts. Desktop ASan/UBSan classifies metadata only; integration hosts validate Guest execution/lifecycle.
