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

## Direct DSO loading control

The reviewed late integration explicitly installs the VM and ART signal profile.
See the [late signal-chain guide](LATE_SIGNALS.md). The original control
observed registrations and imports after loading a DSO in an existing process.
In that setup, Pojav's premain called `System.load(libglibc_entry.so)` followed
by `NativeEntry.start`, while the early fixture exported sigaction from the
executable before VM startup.

`late-signal-probe.py` installs a Host handler in a bionic process, then loads
the signal-router DSO with `RTLD_GLOBAL`. The archive records this observation.

```json
{"result":"UNSUPPORTED","init_status":0,"existing_handler_wrapped":false,"global_sigaction_interposed":false}
```

The existing handler remains unwrapped. The symbol field compares two dynamic
lookups. Existing VM import bindings require explicit inspection and routing.
Pojav and mod results are maintained in the external validation workspace.

## Reproduction

Run from the repository root with explicit adoption glibc, SDK, NDK and JDK
inputs.

```sh
python3 android-jni/tests/same-thread/build.py --system-load \
  --abi x86_64 --jvm-family 5 --jdk "$JDK17" --ndk "$ANDROID_NDK" \
  --runtime "$DOMAIN_RUNTIME_X86_64"
python3 jvm-jni/tests/same-thread/system-load.py \
  --build jvm-jni/build/tests/system-load/x86_64 --jdk "$JDK17"

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
python3 jvm-jni/tests/same-thread/late-signal-probe.py \
  --ndk "$ANDROID_NDK" --serial "$ANDROID_SERIAL"
```

Family 5 applies to the reviewed desktop artifact in this example. Stop on a
hash or signature mismatch and review the actual artifact. The archive
contains the two build manifests, complete logs and application APK digest for
the recorded configurations. [System.load results](../../tests/results/same-thread/system-load/summary.json)
