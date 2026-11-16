# Host resource stores

[English](../en/ZIG_BINDINGS.md) | [简体中文](../zh/ZIG_BINDINGS.md)

## Scope and build

Bindings, reference/ID slots and class records exclusively use Zig stores, requiring exactly Zig 0.16.0. The fixed C backend was removed after desktop/ARM64 comparisons. GW_BINDING_BACKEND defaults to zig and rejects c. C thin layers still own public ABI, JNI calls, C atomics/locks and libffi lifecycle. This change does not migrate core, GNU loader, Guest or JVM private adapters.

[build.zig](../../build.zig) is the sole store compilation graph: ReleaseSafe, PIC, Host allocator and no extra TLS/runtime. CMake invokes it and owns C/JNI, shared/static libraries and SDK installation. Installed C/C++ consumers need no Zig.

```sh
# From android-jni.
zig build host-store --prefix build/host-store -Dcpu=baseline
zig build host-store --prefix build/host-store-arm64 \
  -Dtarget=aarch64-linux-android.29 -Dcpu=baseline

# From the workspace root.
GW_ABI=arm64-v8a scripts/build-runtime.sh --module jvm-jni
```

Python orchestrates tests/reporting/dependencies. The four projects have not fully migrated to Zig build graphs. Caches/installations stay in each project's build/.

## Capacity and growth

| Resource | Initial capacity | On exhaustion |
| --- | --- | --- |
| Top-level Guest modules | Fixed 64 | GW_E_CAPACITY; no growth |
| JNI bindings | Shared 512 | Stable 64-slot blocks |
| References and methodID/fieldID | Shared 16,384 | Stable 64-slot blocks |
| Registered-class owners | Shared 256 | Stable 64-slot blocks |

Defaults live in [resource_limits.h](../../src/host/resource_limits.h); module limit in [module_id.h](../../../bionic-dlopen/include/module_id.h). First use reserves storage, not that many closures, VM references/classes or every module index. Add a block only when slots run out; block-pointer/hash/owner vectors grow geometrically, avoiding per-object allocations.

All modules share one binding pool and have per-module hashes of Guest address/full descriptor plus contiguous release vectors. Sixty-four modules with eight entries each consume 512 slots, not 512 reserved per module. Freed bindings return to the shared pool; module exit leaves no blocks usable only by that module.

Published objects/descriptors/CIFs/type arrays never move. Hot calls carry direct binding pointers without store scans/hash lookup. Reference tokens directly index blocks; local membership uses contiguous token vectors and swap-remove with backfilled positions, not lists.

Tokens encode generation in high 32 bits and index+1 in low 32 bits; zero is null. At UINT32_MAX a slot permanently retires rather than reviving stale tokens. Reference/class pools represent at most 4,294,967,232 slots, practically limited by memory. Bindings are limited by address space, allocator and hash representation, without another business cap. Tokens are neither pointers nor persistent across store/bridge closure/processes.

Empty blocks remain until store destruction and are reused first. No automatic shrinking resets generations or moves addresses. Initial capacity is not a performance ceiling: class identity still scans registration capacity with IsSameObject. Single Guest, 64-method registration batches and 255-byte descriptor bodies remain; dynamic contexts/reentry still face stack/memory/index limits.

Token resolution and ID dedup differ. fh_ref/fh_id directly locate slots; fh_add_id queries a Zig secondary index under handles_lock by `(kind, VM ID value)`, then verifies candidate weak-class identity with IsSameObject. jclass/jweak addresses are not identities; an ID value alone cannot merge classes or method/field kinds.

The index uses hashes and contiguous vectors. A key usually has one inline token; same-key/different-class collisions grow candidate vectors. No per-ID objects/lists. Average lookup depends on matching-key candidates, not all global/local slots. A separate indexed-ID token vector supports sweeping only IDs; swap-remove rereads the current position. Reference pool capacity/history does not magnify ID sweeps.

Only indexed IDs consume hash entries. Each 64-slot block gains a 64-bit flag; deletion updates indexes only for indexed tokens. Generation/address contracts remain unchanged. Index OOM withdraws the new slot, deletes its new weak reference, preserves exceptions or raises OOM. Deletion does not allocate; destruction releases index capacity. Growth means insertion is not strictly constant time on every call.

`python3 android-jni/tests/reference_cost.py --output PATH` isolates production C/Zig paths with 0/32,768/131,072/262,144 global-token high-water marks, then tests direct resolution, early/late ID hits and misses after freeing. Each group warms once and records ten rounds. A minimal VM substitute excludes RPC/real VM/game costs. Explicit --android-ndk, --jni-include, --ffi-include and --device run standalone bionic under shell UID, not app scheduling. --build-only needs no device.

Retain old/new binaries and inputs.json, then `tests/compare_reference_cost.py --device SERIAL --baseline OLD_BINARY --candidate NEW_BINARY --output NEW_DIRECTORY` runs ten balanced pairs. Reports summarize per-operation mean CPU across ten trials, not individual-call P50 or full JNI/RPC latency.

`reference_cost.py --source-ref COMMIT --build-only` extracts old production store/reference source with the same current fixture/toolchain and current public protocol/JNI headers; it is not a complete old runtime rebuild. Inputs retain source/fixture/binary hashes. See [ARM64 comparison and strict JNI evidence](../../tests/results/id-index-arm64.json).

## Ownership, failures and release

All allocation uses Host C callbacks and returns to the same libc. Zig adds no default heap, threads, TLS, locks or process startup; internal symbols are hidden. ReleaseSafe invariant failures trap without extra panic/backtrace runtime.

Insertion reserves blocks/indexes/owner vectors before publishing objects/tokens. OOM never publishes partial state; existing objects/tokens/membership remain valid. Failed initial reservation frees allocated blocks. An operation may retain grown empty capacity without having created an object. Deletion does not allocate; destroying a store with live objects is rejected.

After store OOM, the C caller still owns the VM reference. Borrowed native local arguments must not be deleted by storage. Generated local/global/weak wrappers delete newly created VM results if token allocation fails; new ID weak-class references are similarly released. Preserve pending exceptions, otherwise report OOM through the public failure path.

RegisterNatives failure before VM entry withdraws new bindings/unpublished reservations. Pending counts serialize reservations; JNI_ERR after VM entry may mean partial publication, so retain records/closures until module cleanup. Weak classes do not pin ClassLoaders.

Releasing requires that VM cannot enter old trampolines and Guest background work has stopped. Active counts cannot protect a VM thread that has not entered the closure yet. Uncertain cleanup permanently disables the bridge while retaining reachable resources and requiring process disposal; see [loading](LOADER_BACKEND.md). Growth/ordinary lifecycle tests do not prove arbitrary concurrent unload safety.

## Validation

```sh
# Linux: Zig 0.16.0, Clang, pkg-config/libffi, desktop JDK.
python3 android-jni/tests/bindings.py
python3 android-jni/tests/resources.py

# Android: the default build uses Zig.
GW_ABI=arm64-v8a \
  jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/coverage/run.sh --device SERIAL --reuse-apk --apk APK --output OUTPUT
```

Desktop tests accept explicit installed core/dlopen headers/JDK. C/JNI is ASan/UBSan-instrumented; Zig uses ReleaseSafe, not claimed full ASan instrumentation.

Checks cover shared initial pools, growth/stable addresses, reuse, initialization/insertion OOM, retirement and owner swap-remove. Stress uses 8,192 stored bindings, 4,096 production bindings, 10,000 concurrent old-entry calls during growth, 1,024 classes and 32,768 live references, ending with balanced resources. Separate C loader tests fill 64 modules, reject the 65th, and check high slots/stale generations.

Pinned ARM64 JRE17 checks call 1,024 distinct methods, register 320 classes and create/release 20,000 globals per round. Three ClassLoader cycles require structured capacity/descriptor results and zero references/closures on exit. This is not ART/other-JRE/arbitrary-mod validation or a performance claim.

C/Zig comparison and post-removal default-build evidence stays in project build/aud01/. The C backend is no longer maintained; inspect parent commit 8975fc0 for the old implementation.
