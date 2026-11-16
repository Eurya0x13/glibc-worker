# JNI payload-size and latency methodology

[English](../en/PAYLOAD_LATENCY.md) | [简体中文](../zh/PAYLOAD_LATENCY.md)

## Comparison scope

Within one Android/JRE test app, A is ordinary bionic JNI and B is GNU worker, compiled from the same C fixture. Cases cover pointer+length, DirectBuffer address lookup, memcpy, GetByteArrayRegion and Get/ReleaseByteArrayElements. Pointer controls do not touch all declared bytes; copy cases do.

B−A includes libc/compiler/allocator/memory-location differences, VM work and scheduling; not all of it is bridge cost. Elements allocation/copy is not pure memcpy bandwidth. Keep [array-cache](../../../android-jni/docs/en/ARRAY_COPY_CACHE.md) configuration matched.

## Execution

Build the selected ABI's normal APK, then compare ten pairs within the same APK:

```sh
GW_ABI=arm64-v8a GW_RPC_PROFILE=0 GW_RPC_SPIN_NS=20000 \
  GW_FUTEX_WAKE_FILTER=0 GW_RPC_READY_POLL=0 \
  jvm-jni/tests/android/gradlew -p jvm-jni/tests/android :app:assembleDebug
python3 jvm-jni/tests/coverage/compare_payload.py --device SERIAL \
  --apk jvm-jni/build/tests/android/app/outputs/apk/debug/app-debug.apk \
  --output jvm-jni/build/test-results/payload-comparison
```

compare_array_cache.py compares off/automatic within the same Guest path; --payload-profile threshold adds sizes around thresholds. Formal timing uses ordinary JIT without CheckJNI; run strict semantics separately and never merge modes.

## Samples and validation

Default is ten fresh-process rounds per side in balanced order, sharing configuration shuffle seed within each pair. Forty configurations have three trials each, with 300 warmup and 500 timed iterations. Allocation, pretouch, filling and full data verification are outside timing; allocation/copy intrinsic to the measured API stays inside.

Retain raw samples, percentiles, ≥1ms/≥5ms counts, mode, APK hash and scheduling state per round. tests/payload.py verifies counts/summary consistency. Incomplete rounds cannot represent a complete ten-pair comparison. Analyze paired differences and state aggregation order; the median of trial quantiles is not pooled p99.

## Diagnostics and interpretation

--payload-diagnostic adds native-stage clocks and Host getrusage. analyze_payload.py analyzes samples; capture_payload.py records simultaneous scheduling traces. Diagnostic overhead means these runs are separate from formal A/B.

Vary sizes to separate fixed control and content-copy costs. Use minor faults, thread CPU and traces to distinguish allocation/page-touch from scheduling. Low CPU share does not rule out tails; wall minus CPU is not all scheduler wait.

The fixture has one Java caller, reused pretouched buffers and references created during setup. It does not cover cold caches, newly allocated arrays per call, all concurrency or game FPS. Validate trigger frequency, end-to-end latency, retained capacity and save/exit in real mods before adopting an optimization.
