# JRE loading compatibility and interface families

[English](../en/JRE_COMPATIBILITY.md) | [简体中文](../zh/JRE_COMPATIBILITY.md)

## Recorded support boundary

Pinned Android ARM64 JRE8/17/21/25 artifacts were tested on a real device. JRE17/21/25 passed strict CheckJNI smoke/coverage. JRE8 passed strict smoke and full ordinary-JIT coverage, but its independently reproduced CheckJNI array-commit crash prevents claiming strict coverage. Production implements six loading families; real desktop JRE8/11/17/21/25 passed adapter contracts using host libc transport. Other Android artifacts/vendors and OpenJ9 need separate validation.

| Runtime validation | Family | Scope |
| --- | --- | --- |
| Temurin 8u512 | CLASSLOADER_8 | 31 loading/collection assertions and 3 failed-recovery assertions |
| Local OpenJDK 11.0.25 | CLASSLOADER_10_THROW | Same |
| Local OpenJDK 17.0.21.0.3 | NATIVE_LIBRARIES_18 | Same |
| Temurin 21.0.13 / 25.0.5 | NATIVE_LIBRARIES_19 | Same for each |
| Pinned Android JRE8 / 17 / 21 / 25 | CLASSLOADER_8 / NATIVE_LIBRARIES_15 / NATIVE_LIBRARIES_19 | ARM64 dual libc, 142 Java/JNI and 26 loader checks, SQLite/zstd, reopen and full exit; JRE8 strict limitation below |

Desktop archives are pinned in tests/config/desktop-jres.json; local VM directories/families are explicit. Results record actual version, libjava/libjvm hashes, assertions, CheckJNI logs and an adapter-free startup control. Source audits do not qualify untested families/GA baselines as runtime passes. JDK28 remains source-audit material only, without runtime implementation/validation.

Core, C loader and public JNI remain independent. This project adapts private loading boundaries; do not duplicate glibc-worker per JRE.

## Android artifact selection

`tests/config/android-jres.json` pins ID, ABI, archive hash, libjava/libjvm paths/hashes, family and test startup strategy. GW_JRE_ARTIFACT selects explicitly. No guessing by major, trusting current-file hashes automatically or trying another family after failure. Extraction revalidates both ELF hashes, 64-bit little-endian format and machine architecture; installation checks actual files again. Production accepts resident libjava only, confirms its JavaVM is current JNIEnv's VM and hashes that actual VM image.

Artifact identity and test conclusions are separate; desktop and Android are not substitutes. ARM64 evidence uses Android13 regular app UID, covering strict CheckJNI where supported, dynamic resources, threads/reentry, class handoff, three load/unload cycles, SQLite/zstd, save/reopen, Guest stop and VM destruction.

| Android ARM64 artifact | Startup fixture | Result |
| --- | --- | --- |
| Pojav17 ec28559, 17-internal | Pinned path-cache repair | Strict smoke/coverage pass |
| AngelAura8, 1.8.0_482-internal | Pojav layout + JLI | Strict smoke and ordinary-JIT coverage pass; strict coverage fails from VM JNI_COMMIT defect |
| AngelAura21, 21.0.1-internal | Pojav layout + JLI | Strict smoke/coverage pass |
| AngelAura25, 25.0.5-internal | Same plus pinned NDK libc++ | Strict smoke/coverage pass |

[Recorded evidence](../../tests/results/android-jres-arm64.json) includes artifacts, modes, APK/report hashes and counterexamples. These conclusions cover those artifacts/modes, not every same-major JRE or all JNI. JRE8 advertises at most JNI1.8 and rejects GetModule with UnsupportedOperationException; 17/21/25 advertise at most JNI10 and test GetModule success. Coverage counts include explicit rejection paths, not universal slot support.

AngelAura fixtures deploy read-only libraries in original JRE layout and use Pojav preload/JLI_Launch in the app process. Startup sources are pinned in tests/config/pojav-startup.json; no full launcher/product code is imported. Direct private-directory bin/java execution was denied; linker64 version probing succeeded. Record subprocess probes separately from in-process bridge tests. ARM64 exec children explicitly disable heap pointer tagging because manifest settings do not carry over. See the [test manual](../../tests/README.md).

Switching JRE cleans old managed/read-only files and empty directories; JRE8's leftover lib/ext can prevent JRE17 startup. Existing embedders start their own JRE and attach the adapter, not create another VM with test fixtures. JRE17 private offsets apply only to their pinned artifact.

### JRE8 CheckJNI limitation

Pinned ARM64 JRE8 CheckJNI returns isCopy=true for GetBooleanArrayElements, then Scudo-aborts on final release after JNI_COMMIT. The same VM/C fixture reproduces without glibc-worker in a separate process; ordinary mode completes copy-back. `tests/scripts/test-array-commit.py` PASS means reproducing the VM defect and records strict_jni_supported=false.

The inspected [OpenJDK8 snapshot](https://github.com/openjdk/jdk8u/blob/b661ee6ff840d6922aa7424a4d8f24760b21cf26/hotspot/src/share/vm/prims/jniCheck.cpp) unconditionally frees GuardedMemory in check_wrapped_array_release, including JNI_COMMIT, contrary to [retaining copies on commit](https://docs.oracle.com/en/java/javase/24/docs/specs/jni/functions.html#releaseprimitivetypearrayelements-routines). Tests do not skip commit/final-release checks, hide crashes or substitute non-CheckJNI success for strict results. Ordinary mode is go; strict array checking is no-go until the VM is repaired/replaced and retested.

x86_64 archives/hashes are independent, notably a different JRE8 patch level. Manifest entries permit emulator reproduction but ARM64 results or old x86_64 artifacts do not validate them.

## OpenJDK GA families

The audit inspects 20 GA baselines from JDK8–27 plus one pinned JDK28 early-access snapshot. Five broad loading/find/unload shapes emerge; membership does not mean identical implementation/behavior.

| GA versions | Load boundary | Lookup/unload | Adaptation consequence |
| --- | --- | --- | --- |
| 8–9 | ClassLoader$NativeLibrary instance load(String,isBuiltin): void | Instance find/unload, no explicit unload handle | Old class layout and instance methods |
| 10–14 | Same class instance load0(String,isBuiltin): boolean | findEntry; static unload takes handle | Changed return, method name and unload ABI |
| 15–17 | NativeLibraries static load(impl,name,isBuiltin,isJNI) | findEntry0 takes impl; unload includes isJNI/handle | Pinned JRE17's main shape |
| 18 | Adds throwExceptionIfFail | Otherwise previous shape | Extra boolean/failure semantics |
| 19–27 | load(impl,name,isBuiltin,throwExceptionIfFail), no isJNI | NativeLibrary.findEntry0(handle,name); unload omits isJNI | Changed boolean semantics, lookup class and unload prototype |

Upstream endpoints: [8](https://github.com/openjdk/jdk8u/blob/jdk8-b132/jdk/src/share/classes/java/lang/ClassLoader.java), [10](https://github.com/openjdk/jdk/blob/jdk-10%2B46/src/java.base/share/classes/java/lang/ClassLoader.java), [15](https://github.com/openjdk/jdk/blob/jdk-15%2B36/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java), [17](https://github.com/openjdk/jdk/blob/jdk-17%2B35/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java), [18](https://github.com/openjdk/jdk/blob/jdk-18%2B36/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java), [19](https://github.com/openjdk/jdk/blob/jdk-19%2B36/src/java.base/share/native/libjava/NativeLibraries.c), [25](https://github.com/openjdk/jdk/blob/jdk-25%2B36/src/java.base/share/native/libjava/NativeLibraries.c). Intermediate files/tags/hashes are in the audit manifest.

JDK17 and JDK19 load descriptors both end in String;ZZ)Z, yet the second boolean changes from isJNI to throw-on-failure. Matching names/descriptors or parameter count alone does not prove semantics. Fields handle:J, jniVersion:I and fromClass:Class persist in inspected GA sources, but owning classes, usage, unload objects and synchronization vary.

## JDK25–28 boundaries

| Baseline | Pinned source | Load versus 25 | Public JNI table |
| --- | --- | --- | --- |
| 25 GA | jdk-25+36 | Control | Control |
| 26 GA | jdk-26-ga, 4408cd2a07a14243a58cd9d30813302bfbe81133 | Same | Same slots/parameters |
| 27 GA | jdk-27-ga, 815ff4dc327fe17f2433c7d115a5a503af10f3c4 | Same | Same slots/parameters |
| 28 early-access | ce986f721b1d1e4d1503c50b8070a52f6f4e970a | Same in this snapshot | Appends HasIdentity and JNI_VERSION_28 |

NativeLibraries.c is byte-identical across these four inputs, including load/unload/find exports, parameters and bodies; three JVM loading prototypes also match. NativeLibrary.findEntry0 and handle/jniVersion/fromClass shapes remain. No new loading family is needed for these source baselines, which does not prove Android binary/full runtime compatibility.

Java internals still change: later sources move loadedLibraryNames into a Holder for AOT initialization and add clear. These do not affect currently hooked native boundaries; dependencies on other private fields/order require separate review.

The 28 snapshot appends HasIdentity(JNIEnv*,jobject) for value-class identity without reordering existing slots. The bridge does not implement it or JNI28 negotiation. Loading ABI similarity cannot justify claiming support. The snapshot is ea with target date 2027-03-23; later commits/final GA need reinspection. Sources: [26 load](https://github.com/openjdk/jdk/blob/jdk-26-ga/src/java.base/share/native/libjava/NativeLibraries.c), [27 load](https://github.com/openjdk/jdk/blob/jdk-27-ga/src/java.base/share/native/libjava/NativeLibraries.c), [28 pinned load](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/src/java.base/share/native/libjava/NativeLibraries.c), [28 JNI](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/src/java.base/share/native/include/jni.h), [28 version](https://github.com/openjdk/jdk/blob/ce986f721b1d1e4d1503c50b8070a52f6f4e970a/make/conf/version-numbers.conf).

## Relatively stable lower mechanism

Inspected versions call platform loading through JVM_LoadLibrary/JVM_FindLibraryEntry/JVM_UnloadLibrary, while libjava handles OnLoad/OnUnload, JNI versions and failure cleanup. This mechanism changes less than upper organization.

JVM_LoadLibrary is not immutable ABI: 8–17 GA takes name; 18–27 adds throwException, retained in the 28 snapshot. Source similarity supports a stable overall flow, not a permanent private C prototype. The adapter registers JNI native methods rather than casting different JVM_LoadLibrary addresses to one type.

## Maintenance backports

Five pinned 8u/11u/17u/21u/25u commits are specific snapshots, not exhaustive patch-level coverage:

- Inspected 8u retains the 8–9 shape.
- [11u](https://github.com/openjdk/jdk11u/blob/d001c53e35a816f26718845b5d26a877878a7f5d/src/java.base/share/classes/java/lang/ClassLoader.java) adds throwExceptionIfFail to load0, unlike 11 GA.
- [17u](https://github.com/openjdk/jdk17u/blob/14a8c6288c963d6d61f12227375398d8abafc709/src/java.base/share/classes/jdk/internal/loader/NativeLibraries.java) has three booleans, like 18 GA; pinned Android JRE17 retains two.
- Inspected 21u/25u retain the 19–25 GA boundaries.

Do not dispatch by major alone. Upgrades need not break ABI, but backports can change private entrypoints within a major. A changed SHA256 proves different identity only; rejection of unreviewed hashes is a review gate, not proof of incompatibility.

## Extension rules

Separate family, artifact identity and runtime evidence. Families specify exact classes/fields/prototypes, booleans and lifecycle; v2 profiles pin reviewed libjava/libjvm pairs; only completed tests establish supported scope. Select through build profile or gw_jvm_jni_set_profile; reject unknown families, invalid hashes and mismatched fields/methods.

Resolve/check every symbol/method/field before registration. Reject unknown shapes without side-effecting probe calls or bionic fallback for GNU DSOs. Preserve transactional registration/rollback; discard the process if restoration fails. Never remove hash gating to accept arbitrary versions.

Old GW_JRE17_* inputs/names are removed. Use GW_JVM_PROFILE_FAMILY or profile v2 family, not parameter count or major. See [adapter values/lifecycle](ADAPTER.md).

## Checks beyond loading

- Public JNI covers base JNI1.6 plus GetModule, capped at JNI10. Startup negotiation bounds Guest GetVersion/GetEnv/AttachCurrentThread/OnLoad. JRE8 caps at JNI1.8; newer hosts at JNI10. JNI19/21/24/25 capability requests are rejected. Older GetModule throws UnsupportedOperationException without out-of-table access. A loader profile does not implement new JNI slots.
- JDK24/25 [Runtime](https://github.com/openjdk/jdk/blob/jdk-24%2B36/src/java.base/share/classes/java/lang/Runtime.java) adds native-access checks before load/loadLibrary; preserve the VM's module policy.
- Pinned Android test startup patches are not universal JRE startup support.
- Each supported artifact needs OnLoad acceptance/rejection/version failure, symbols/overloads, ClassLoader ownership, callbacks/reentry, concurrency/TLS, OnUnload, references, Guest stop and VM destruction. Real libraries/mods additionally need data and save/exit checks.

## Reproducible source audit

`tests/config/openjdk-loader-sources.json` pins repositories, GA tags/full commits, paths and hashes for 129 files across 26 snapshots: 20 GA, five maintenance and one JDK28 early-access. The tool downloads pinned files and extracts boundaries without running VMs:

```sh
python3 jvm-jni/tests/scripts/audit-openjdk-loader.py
python3 jvm-jni/tests/scripts/audit-openjdk-loader.py --offline
```

Cache and loader-boundaries.json use build/jre-loader-audit/. Results include Java native declarations, C exports, function-body token hashes and JVM prototypes. Token hashes identify differences for review, not ABI/correctness proof. Preserve source audits when updating profiles and run the [real-runtime tests](../../tests/README.md).
