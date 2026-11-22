# System.load integration with same-thread domains

[English](../en/SYSTEM_LOAD.md) | [简体中文](../zh/SYSTEM_LOAD.md)

The tests use the production JVM adapter with `System.load` and
`System.loadLibrary` to verify Guest module loading and unloading. The fixture
defaults to same-thread. Set `GW_DOMAIN_MODE=worker` for a worker run. The runner
checks the default, worker and domain selections and verifies that the default
creates an adopted domain. Embedders supply an explicit JRE profile and choose
the backend through the [runtime startup API](../../../android-jni/docs/en/RUNTIME.md).

## Implementation

`android-jni/tests/same-thread/build.py --system-load` adds the production JVM
adapter sources to the JNI transport fixture. It writes
`jvm-jni/build/tests/system-load/<ABI>/`. The Java helpers and loading tests
belong to jvm-jni.

The fixture registers the adapter before `full_host_start` with
`GW_HOST_VM_ADAPTER` and the path of the resident libjava. The adapter verifies
libjava and libjvm hashes, the selected VM and private method signatures before
installing load, find and unload hooks. The module exposes ordinary `Java_*`
symbols. Native lookup exercises a short JNI name and two overloaded long names.

Three isolated URLClassLoader cycles cover load, call, reference release and GC.
The middle cycle uses `System.loadLibrary` with the absolute fixture path
returned by `findLibrary`. A second ClassLoader must reject an already opened
image. A module whose `JNI_OnLoad` returns `JNI_ERR` must roll back. Missing JNI
exports must throw `UnsatisfiedLinkError`. Android also loads an ordinary bionic
library and checks that it takes the native VM path and leaves Guest counts unchanged.

The adapter exchanges filesystem paths with native code as bytes in the host's
path encoding. The shell runner checks a Guest library and an ordinary Host
library under a directory whose name contains a supplementary Unicode character.
It also loads two modules with different bundled files for the same SONAME in
both orders, checks that the second load fails before its constructor runs,
and verifies that both modules can share a dependency when their search paths
resolve to the same file.

## Results recorded on 2026-11-15

| Environment | Worker | Same-thread | Assertions |
| --- | --- | --- | --- |
| Linux x86_64 / OpenJDK 17.0.21.0.3 | PASS | PASS | 23 checks and three automatic unloads |
| V2239A / Android 13 / pinned Android JRE17 / shell | PASS | PASS | 25 checks including bionic passthrough |
| Same device / application UID 10310 / linker64 child | — | PASS | 25 checks, three automatic unloads and VM destruction |

Each run used CheckJNI, ended with zero active modules and closures, and
balanced image opens and closes. The reviewed desktop artifact uses family 5.
The Android artifact uses family 4. Both are JRE17.

The fixture stops the Host bridge and restores VM hooks before joining the Guest
worker. During same-thread shutdown, the calling thread's domain finishes and
the VM cleaning thread's idle binding stays mapped until process exit. The
module and JNI closures unload according to the
[runtime lifecycle contract](../../../android-jni/docs/en/LIFECYCLE.md).

## Native Host startup

The application fixture starts a fresh app-UID native process and calls the
[Host signal bootstrap](../../../core/docs/en/SIGNAL_BOOTSTRAP.md) before
loading HotSpot. The test verifies the route before VM creation, after VM
creation and after Guest JNI. Bionic's actual private registration API checks
Host TLS while the VM is running. Each ClassLoader cycle runs eight concurrent
native callers with JFR, Java stack walks, GC and explicit Guest signal sends.

The launcher maintains the JRE startup layout and native loading profile.
The Android fixture accepts the pinned Pojav JRE17 and AngelAura JRE21 artifacts.
JRE17 uses the test harness's reviewed path-cache adapter. AngelAura uses the
public VM invocation API and its original read-only library layout.

## Reproduction

Run from the repository root with explicit adoption glibc, SDK, NDK and JDK
inputs.

```sh
python3 android-jni/tests/same-thread/build.py --system-load \
  --abi x86_64 --jvm-family 5 --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_X86_64"
python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/system-load/x86_64 --jdk "$JDK17"

# Select the pinned Android artifact explicitly.
export JAVA_HOME="$JDK17"
export PATH="$JAVA_HOME/bin:$PATH"
GW_ABI=arm64-v8a python3 jvm-jni/tests/scripts/prepare-zstd.py --fetch-only
python3 android-jni/tests/gen-binding-capacity.py --output jvm-jni/build/native/generated
GW_ABI=arm64-v8a GW_JRE_ARTIFACT=angelaura-21-arm64 \
  python3 jvm-jni/tests/scripts/prepare-jre.py

python3 android-jni/tests/same-thread/build.py --system-load \
  --abi arm64-v8a --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_ARM64"
python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/system-load/arm64-v8a --jdk "$JDK17" \
  --serial "$ANDROID_SERIAL"
python3 jvm-jni/tests/same-thread/android.py --system-load \
  --native jvm-jni/build/tests/system-load/arm64-v8a \
  --runtime "$DOMAIN_RUNTIME_ARM64" \
  --jre-assets jvm-jni/build/tests/android/assets/android-jre \
  --jre-libs jvm-jni/build/tests/android/jniLibs/arm64-v8a \
  --gradle "$GRADLE" --sdk "$ANDROID_HOME" --serial "$ANDROID_SERIAL"
```

Family 5 applies to the reviewed desktop artifact in this example. Stop on a
hash or signature mismatch and review the actual artifact. The archive
contains the two build manifests, complete logs and application APK digest for
the recorded configurations. [System.load results](../../tests/results/same-thread/system-load/summary.json)
