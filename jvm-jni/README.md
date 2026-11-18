# glibc-worker jvm-jni

[English](docs/en/README.md) | [简体中文](docs/zh/README.md)

Optional HotSpot/JRE integration. Reviewed loading-interface families cover JRE8–25, with private loading hooks, method descriptor handling and libjava artifact checks. Integration uses android-jni's adapter registry; lower layers never depend back on this project.

`src/native/` and `src/java/` contain adapters and helpers; `include/` contains public adapter headers.

JRE8/11/17/21/25 passed adapter-contract tests on real desktop VMs. Pinned Android ARM64 JRE17/21/25 artifacts passed strict smoke/coverage; pinned JRE8 passed strict smoke and ordinary JIT coverage, with its CheckJNI array-commit defect reproduced independently. Loading adaptation does not validate every new JNI slot or every Android JRE artifact. See [compatibility](docs/en/JRE_COMPATIBILITY.md). JDK28 is outside runtime support.

## Standalone build

Run from this project:

```sh
./build.sh -DCMAKE_PREFIX_PATH="/path/to/android-jni/install;/path/to/loader/install;/path/to/core/install" \
  -DGW_JNI_INCLUDE_DIR=/path/to/neutral-jni-header \
  -DGW_JRE_ADAPTER_HEADER=/path/to/reviewed-jre-profile.h \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --install build
```

Outputs are `build/libgw_jvm_jni.so/.a`, `jvm-jni-helpers.jar` and `GlibcWorkerJvmJni`. The JAR compiles only `JvmAdapter`, referencing public classes through the installed android-jni helper JAR. Without a build profile, configuration is unconfigured; supply a reviewed profile through `gw_jvm_jni_set_profile` before activation. Major version alone never authorizes an artifact. Old JRE17 headers, C/Java forwarding entrypoints, macros and enum aliases were removed; use a new SDK installation prefix when upgrading.

Loading this DSO registers the optional adapter but does not immediately modify the VM. The embedder explicitly requests `GW_HOST_VM_ADAPTER` during initialization; stop restores hooks. Use JNIEnv from the same running VM.

See [adapter integration](docs/en/ADAPTER.md) for the profile API, six families, configuration and stopping conditions.

## Integration tests

This project's `tests/` owns the Android/JRE test application, artifact configuration, VM hosts and integration runners. It explicitly references android-jni JNI fixtures and core POD fixtures.

From the workspace root:

```sh
GW_ABI=arm64-v8a jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
jvm-jni/tests/smoke/run.sh --device SERIAL
jvm-jni/tests/coverage/run.sh --device SERIAL
```

All outputs remain in this project's `build/`. Smoke and coverage are two suites of the same upper-layer integration; core events and C loader tests belong to their own projects.

`tests/scripts/test-adapters.py` needs no Android device. It replaces production loading hooks in real desktop VMs and tests lifecycle, ClassLoader collection and rollback using host libc transport. See the [test manual](tests/README.md) for pinned artifacts and commands.

Workspace composition also produces legacy `libglibc_bridge.so`, GNU service, helpers and the old flat header layout under `build/<ABI>/compat/`. Choose this single DSO or the modular DSOs; never load both in one process.

Native JNI JAR compatibility and latency/statistics documentation belong to [this project](docs/en). Pojav integration remains external. Long-running GC, complete runtime destruction/recreation and arbitrary C++/signal coexistence require separate validation.

## Same-thread signal integration

Maintained signal integration is `src/native/signal_profile.c`, with public `signal_profile.h`. `GW_SIGNAL_PROFILE_HEADER` selects a reviewed, pinned VM/ART artifact profile; workspace builds pass through the environment variable. Without configuration, installation explicitly returns `ENOTSUP`. There is no arbitrary JRE detection or offset inference, and android-jni does not depend on these private interfaces. See [late signal-chain integration](docs/en/LATE_SIGNALS.md) and [core signal rules](../core/docs/en/DOMAINS.md).
