# Private HotSpot loading adapter

[English](../en/ADAPTER.md) | [简体中文](../zh/ADAPTER.md)

This optional layer redirects HotSpot's private load/find/unload boundaries to android-jni's versioned API. It is not core or the public JNI bridge.

## Artifacts and build inputs

Both configuration methods require an explicit interface family and reviewed libjava/libjvm SHA256 values:

1. Set GW_JRE_ADAPTER_HEADER to a header defining GW_JVM_PROFILE_NAME, GW_JVM_LIBJAVA_SHA256, GW_JVM_LIBJVM_SHA256 and GW_JVM_PROFILE_FAMILY.
2. Call gw_jvm_jni_set_profile before installation. It copies the v2 structure, retains no caller pointer and returns BUSY while installed.

```c
struct gw_jvm_profile_v2 profile = {
    .abi_version = GW_JVM_PROFILE_ABI_V2,
    .struct_size = sizeof profile,
    .family = GW_JVM_NATIVE_LIBRARIES_19,
    .name = "reviewed-build-name",
    .libjava_sha256 = "<64 lowercase hex digits for reviewed libjava>",
    .libjvm_sha256 = "<64 lowercase hex digits for reviewed libjvm>"};
/* Check GW_OK before starting the public bridge. */
int result = gw_jvm_jni_set_profile(&profile, &error);
```

Hashes above are placeholders, not usable configuration. Products select profiles from their reviewed artifact inventories. The API supplies no extra trust list and never treats a current file's hash as proof of compatibility. Hash matches alone do not identify parameter semantics; select the correct family.

| Enum | Exact boundary |
| --- | --- |
| GW_JVM_CLASSLOADER_8 | 8–9 shape: instance load/find/unload; success sets loaded |
| GW_JVM_CLASSLOADER_10 | 10–14 GA shape: two-argument load0 |
| GW_JVM_CLASSLOADER_10_THROW | 11u backport: load0 adds throwExceptionIfFail |
| GW_JVM_NATIVE_LIBRARIES_15 | 15–17 GA: load includes isJNI |
| GW_JVM_NATIVE_LIBRARIES_18 | 18 GA / 17u backport: load includes isJNI and throwExceptionIfFail |
| GW_JVM_NATIVE_LIBRARIES_19 | 19+ shape: find belongs to NativeLibrary; load omits isJNI |

These name contracts, not automatic major-version dispatch. See [source compatibility](JRE_COMPATIBILITY.md). JDK28 is outside the runtime matrix.

Tests generate defaults with tests/scripts/prepare-jre.py from [pinned Android artifacts](../../tests/config/android-jres.json). ABI/archive/libjvm/libjava must match together. Only GW_JVM_* macros, jvm_adapter.h and JvmAdapter remain; JRE17 C/Java forwarders and GW_HOST_LEGACY_JRE17 were removed without source/binary aliases.

See the [project guide](README.md). Java helpers compile against installed android-jni helpers rather than copying public classes. Profile v1 ABI/size is explicitly rejected, without filling missing hashes; update public structures and both hash inputs. Files must remain immutable during validation/install/use. Hash gating is not protection against file replacement races or malicious native code.

## Lifecycle

The libgw_jvm_jni.so constructor only registers the adapter; gw_jvm_jni_register is also explicit. Register before public bridge initialization. Install occurs only when the embedder requests GW_HOST_VM_ADAPTER; shutdown restores.

Install resolves helpers, validates libjava, confirms resident image/target VM ownership and its libjvm hash, resolves fields and all three original methods, then registers. Only resident libjava is accepted; do not load another libjava/JVM for probing. Enumeration entrypoint and target JavaVM GetEnv must belong to the same VM image, and the enumerated instance must match current JNIEnv. Original exports must come from the validated libjava, not identically named dependencies. Do not modify VM before all checks finish. Failed RegisterNatives attempts full restoration. If rollback also fails, retain handles/global references, block profile changes and require process disposal. Separate real JRE8/11/17/21/25 processes cover partial registration and failed recovery.

Set-profile/install/restore require no concurrent loading/unloading/calls. The mutex protects configuration transactions, not other VM threads. Close Guest modules and stop calls before restore; release bridge only afterward. Public Host shutdown follows this order.

The GNU branch delegates JNI_OnLoad/OnUnload completely to the backend. The original VM path must not repeat lifecycle calls or pass logical Guest handles to JVM_UnloadLibrary. Ordinary bionic libraries retain original handling; the VM still owns ClassLoader context/path checks/ownership.

throwExceptionIfFail governs loading-failure policy only; OnLoad rejection, version errors and existing exceptions remain failures. Builtins and legacy non-JNI loads use original functions.

## Descriptors and source integration

Trampolines require complete method descriptors. The current private adapter uses reflection and still has limits when overloaded methods share a short JNI export. Source integration should pass actual method metadata explicitly to backend find; replacing dlopen/dlsym alone cannot infer prototypes.

19+ lookup receives a handle rather than library object. On load commit, helpers associate the handle with a weak calling-Class reference and resolve signatures through its ClassLoader. Unload/rollback removes it. Strong references would prevent collection/automatic unload; tests cover two complete collect/unload/reload cycles.

OpenJDK8–25 has multiple interface families and maintenance backports; never apply a pinned old hook to arbitrary JREs. No universal JRE source patch or ART private adapter is claimed.

<a id="signal-registration-integration"></a>

## Signal registration integration

Late signal integration has its own reviewed `GW_SIGNAL_PROFILE_HEADER`; the
native-load family/profile does not supply it. In addition to the VM/ART sites,
Android requires `GW_HOST_LIBC_SHA256`, `GW_HOST_SIGACTION_OFFSET` and
`GW_HOST_SIGACTION64_OFFSET`. These identify the exact owning Bionic artifact
and its two data imports. Missing or mismatched inputs return `ENOTSUP` before
changing imports or admitting Guest recipients. Native desktop integration
does not use Bionic inputs.

The maintainer reviews the actual artifact's private registration call paths.
The runtime verifies its file hash, owning mapping, slot alignment, original
pointer and data-page protection. Plain HotSpot routes the imports to core;
ART retains its reviewed public front and bottom routing. Both imports join
the existing installation/rollback/restore transaction, with original RELRO
permissions restored. No libc instructions are rewritten. Raw registrations
bypassing these reviewed imports remain outside this adapter.

The experiment builder accepts `--host-libc-signal-profile` with explicit
`schema: 1`, `abi: "arm64-v8a"`, `sha256` and an `imports` object containing
reviewed integer offsets for `sigaction` and `sigaction64`. It does not infer
JRE families or generate new compatibility approvals. Stop application
registration/JNI entry during installation and retain the integration through
recipient and VM shutdown.

Participating native consumers must route their own registrations through
`signal_router_action`/`signal_router_action64`, or supply separately reviewed
imports. Patching the VM and Bionic's internal PLT sites does not interpose
every other DSO's call to the public libc symbol. The installed late JNI test
uses this explicit consumer boundary; the Bionic run-on-all-threads test uses
Bionic's actual internal registration path.

After installing the selected profile, use
`domain_late_signal_reclaim_enable` to reserve an unused public realtime
signal before starting Guest or relay helpers. Call
`domain_late_signal_reclaim` from a normal Host control thread. The proof
checks actual Linux recipients, including native VM threads. The result is
the number of retired registrations released. A missing acknowledgement
returns `EBUSY`. Ordinary generations published under complete routing are
eligible for this proof. A saved native context that names an unbound entry
requires the embedding's full barrier. See the
[core contract](../../../core/docs/en/DOMAINS.md#reclaiming-host-registrations).

`tests/signal_profile.py` reuses an explicitly prepared installed Android late
SDK payload and the matching VM startup header. Pass `--payload`,
`--vm-startup-profile`, `--ndk` and `--serial`. It injects failure after each of
the five plain-HotSpot/Bionic import writes and requires all five originals
restored, the router disabled and normal `DestroyJavaVM` completion. Optional
repeated `--rejection-prefix` arguments select separately built SDKs with
intentionally missing Host fields, a wrong Bionic digest or an invalid import
offset; each must reject with `ENOTSUP` before enabling the router and still
destroy the VM. The reclaim case installs the reviewed profile, releases a
replaced ordinary registration with JRE17 running, then destroys the VM and
restores the router. The script retains input hashes and per-case output under
`build/tests/signal-profile-android`.

`tests/signal_reclaim.py` exercises live HotSpot and Android ART recipients
using a process-owned test signal. Pass `--core-prefix` and `--jdk`. The
ARM64 variant also takes `--serial`, `--ndk` and `--d8`. It verifies a
blocked recipient, mapping release while the old callback is parked,
successful return and subsequent delivery. The shell ART run uses
`dalvikvm64` and keeps ART's installed chain in place. The
[inline application fixture](../../experiments/same-thread/LATE_SIGNALS.md)
additionally installs the reviewed ART and Bionic imports after VM startup.
It replaces an ordinary registration through ART's public entrypoints and
releases the old mapping while the application's main ART thread runs its
old callback. The callback returns and the same thread receives the new
generation.

## Runtime composition

Use four installed packages modularly or scripts/package-runtime.sh's single libglibc_bridge.so/merged helpers. Choose one per process to avoid duplicate singletons. Compatibility packaging includes public bridge/adapter, not test Activity, JNI fixtures or JRE startup path-cache patches.

Android test hosts select Pojav JLI startup or pinned JRE17 path-cache repair by artifact. Directory deployment, linker64 subprocess probes and pointer-tagging compatibility libraries belong only to the test app, not SDK. Existing JVM embedders attach the adapter without creating a second VM. See the [test manual](../../tests/README.md).
