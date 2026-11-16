# JNI loading API and embedder integration

[English](../en/LOADER_BACKEND.md) | [简体中文](../zh/LOADER_BACKEND.md)

This layer adapts JNI lifecycle/call rules to bionic-dlopen's C backend. Complete Guest loader/libc domains load GNU libraries; never execute a GNU pointer directly in the bionic domain.

## Responsibilities

| Component | Responsibility |
| --- | --- |
| include/loader_backend.h | v1 JNI loading table, explicit descriptors, structured errors |
| host/loader_backend.c | JNI version/descriptor checks and C transport/JNI result adaptation |
| host/loader_transport.c, host/command.c | Load/unload phases, exception preservation, RPC publication |
| include/host_bridge.h | Public Host configuration/lifecycle |
| include/jni_vm_adapter.h | Optional VM extension registration/install/restore |
| host/image_policy.c, GuestImages | Match original bytes to deployed images or explicit direct paths |
| guest/ | GNU JNI proxies, loading and RPC service |

[bionic-dlopen](../../../bionic-dlopen/docs/en/README.md) owns handle generations, duplicate inodes, states, borrows and close waits. Private NativeLibraries hooks/reflection belong to [jvm-jni](../../../jvm-jni/docs/en/ADAPTER.md).

## Query and calls

```c
const struct gw_loader_api_v1 *api =
    gw_loader_query(GW_LOADER_ABI_V1, sizeof(struct gw_loader_api_v1));
```

Check NULL, ABI and size. JNIEnv must belong to the current attached thread and selected real VM, never another thread/VM.

| Operation | Contract |
| --- | --- |
| open(env, path, flags, &library, &error) | AUTO uses GNU ELF markers; explicit GUEST requires caller-confirmed GNU ABI. Success returns logical handle/JNI version |
| find(env, handle, symbol, descriptor, &entry, &error) | Complete descriptor such as (J)J; returns a bionic Host trampoline |
| close(env, handle, &error) | Reject new borrows, wait for existing calls, perform Guest OnUnload/dlclose and registration cleanup |
| owns_handle(handle) | Recognizes retired Guest tags to prevent bionic fallback |
| module_state(handle) | loading=1, live=2, closing=3, stale=-1; diagnostic only |

GW_PASS permits platform continuation; GW_OK means Guest handled it; GW_NOT_FOUND means missing symbol only; GW_ERROR stops the operation without bionic retry of the same GNU file. Preserve existing Java exceptions.

AUTO malformed/unsupported ELF returns IMAGE; classifier bounds return CAPACITY; both are ERROR. PASS requires successful parsing without GNU markers. Uncertainty is not platform-loadability.

Paths/symbols/descriptors are borrowed per call. Descriptors permit 32 arguments/256 bytes, but syntactic validity does not prove the ELF prototype; supply real method metadata. Entries are valid only while the library is open. Synchronize calls/unload and stop using cached trampolines after close.

## Startup and shutdown

1. Prepare the real VM, visible BridgeTypes/GuestImages helpers and shared protocol region.
2. If needed, register gw_jni_vm_adapter_v1 before requesting GW_HOST_VM_ADAPTER.
3. Call full_host_start on a valid target-VM JNI thread with explicit directories, image policy and flags.
4. Start Guest with matching loader/libc; wait for full_guest_start/RPC service, then call full_host_ready.
5. Admit module loads. GNU operations before ready return NOT_READY; ordinary bionic operations retain platform behavior.
6. Close every Guest module, then full_host_shutdown stops the backend, restores optional hooks and releases Host helpers/reference caches. The embedder confirms raw thread exit; Guest joins its own pthreads.

Loading/live/closing modules block stop. Native trampolines, find and registration publication hold thread-owned scopes recording all ancestors and recursion, including synchronous callbacks. Returns and exception unwinds release them. Closing a held ancestor returns BUSY before CLOSING. Innermost module metadata is not complete borrow history.

While holding any scope, closing another actively borrowed target also returns BUSY, preventing mutual callback waits. Leave callback/scopes before retry; never spin inside them. Scope-free lifecycle threads may wait; unborrowed targets may close.

This is not general deadlock detection: pin-only cycles, OnUnload/transport and user waits remain caller-owned. Borrow counts do not cover arbitrary background threads; stop/join those explicitly. See [execution rules](../../../bionic-dlopen/docs/en/EXECUTION_SCOPES.md).

One configuration, Guest and target VM are supported per process. There is no general full retry after initialization failure or GNU mapping destruction/recreation API. Dispose an isolated process when recovery is unsafe. Shutdown does not call DestroyJavaVM. See [lifecycle](LIFECYCLE.md).

<a id="卸载与失败隔离"></a>
## Unload and quarantine

Close performs Guest JNI_OnUnload while mapped, Host VM native unregistration, Guest dlclose, then trampoline release/ID sweep. Rejected OnLoad first withdraws registrations, then ABORT without OnUnload; release trampolines only after confirmation. Preserve the original Java exception through cleanup.

Weak-class promotion OOM, in-flight class reservations, UnregisterNatives failure or Guest submit/cleanup failure permanently quarantine. Retain possibly reachable registrations, closures, images and contexts; do not roll back already completed safe cleanup. Return GW_E_QUARANTINED; the embedder must discard the process. No auto-kill or same-process recovery. Publication holds module borrows; CLOSING/quarantine reject new registrations, and retained trampolines reject new Guest calls.

This does not replace synchronization of cached entries/background threads or guarantee arbitrary concurrent unload. `python3 android-jni/tests/audit.py` injects production transport/command failures with VM/Guest substitutes, complementing normal device lifecycle tests. See [contexts/ownership](CONTEXTS_AND_OWNERSHIP.md) for thread binding, dynamic capacity, class handoff and dependency attribution.

## JNI capabilities

The bridge implements the JNI1.6 base table plus GetModule and advertises at most JNI10. Startup negotiates real VM GetVersion against that cap using idle slots 10..12 and Guest acknowledgement. No ready without acknowledgement; matched Host/Guest deployment is required even with profiling/filtering disabled.

Private loader protocol v6 uses idle slots 13..23 for protocol/layout sizes, initial context/frame capacity, page width and fixed module cap 64 before activation. v6 adds owner inbox fields while retaining dynamic directories/emergency RPC; v5/older or mismatched configurations cannot mix.

Guest GetVersion returns the negotiated version. GetEnv/AttachCurrentThread reject higher/unknown versions and clear outputs; JNI_OnLoad is checked before commit and rolls back normally on rejection. JRE8 advertises at most JNI1.8; newer hosts at most JNI10. This does not limit Java bytecode versions, but JNI19+ new slots are not provided.

GetModule on pre-JNI9 hosts throws UnsupportedOperationException without reading slot 233. Full ART behavior still needs validation.

Method/field descriptor bodies are at most 255 bytes. Reflection, ID caching, registration and commands validate/copy whole descriptors, rejecting truncation; reflection/commands throw IllegalArgumentException. Context/frame growth failure preserves exceptions or raises OOM; real VM stack overflow remains intact. See [failure handling](FAILURES.md).

## Configuration and limits

v2 retains historical jre_library_path only for the optional adapter. Use GW_HOST_VM_ADAPTER; old names are removed. Requesting an unregistered adapter fails early. GW_HOST_DIRECT_IMAGES permits direct images; default content matching needs predeployed identical original bytes. Android execution policy still applies.

See [array caching](ARRAY_COPY_CACHE.md). Domains share agreed POD/addresses/lengths only, not arbitrary ownership of allocators, FILE, pthread, exception objects or JNI references.

Real tests use Android HotSpot. Public JNI does not prove ART compatibility; never pass application ART JNIEnv to a HotSpot bridge. This does not globally intercept bionic dlopen; GNU-internal dlopen uses the Guest loader.
