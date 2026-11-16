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

## Runtime composition

Use four installed packages modularly or scripts/package-runtime.sh's single libglibc_bridge.so/merged helpers. Choose one per process to avoid duplicate singletons. Compatibility packaging includes public bridge/adapter, not test Activity, JNI fixtures or JRE startup path-cache patches.

Android test hosts select Pojav JLI startup or pinned JRE17 path-cache repair by artifact. Directory deployment, linker64 subprocess probes and pointer-tagging compatibility libraries belong only to the test app, not SDK. Existing JVM embedders attach the adapter without creating a second VM. See the [test manual](../../tests/README.md).
