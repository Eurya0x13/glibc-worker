# RPC waiting-strategy validation

[English](../en/RPC_LATENCY.md) | [简体中文](../zh/RPC_LATENCY.md)

See [core waiting](../../../core/docs/en/WAITING.md) for primitives and segments. This project owns real-VM integration/comparisons, not a second event implementation.

## Configuration and order

Defaults: spin=20000ns, wake_filter=0, ready_poll=0, profile=0. Host, Guest and embedder entrypoints must match. Build/hash each APK separately; do not generate A/B concurrently in one build directory.

First run core ASan/UBSan event/handoff/ready-poll tests, then device strict smoke/coverage, then timing.

```sh
python3 jvm-jni/tests/coverage/compare_waiting.py --device SERIAL \
  --baseline /path/to/control.apk --candidate /path/to/candidate.apk \
  --benchmark-jit --output jvm-jni/build/test-results/waiting-comparison
```

Default is ten paired balanced fresh-process rounds per side. Strict mode uses interpreter/CheckJNI; ordinary timing explicitly enables benchmark JIT. Do not pool these modes.

## Evidence

Scalar echo and Guest→Java callback measure full round trips, not one-way time, array bandwidth or game FPS. Retain functional checks, raw latency, CPU, idle waiting, thermal state and full lifecycle. Failed/unfinished rounds cannot enter a complete comparison.

Paired diagnostics split outbound/execute/resume. Correlate millisecond tails with the same TID/time-window scheduler trace to distinguish execution, blocking and runnable CPU wait. Bounded outliers do not represent all calls. Profiling adds clocks/memory; diagnostics do not replace ordinary-build A/B.

Change defaults only after sustained improvement on real paths without compatibility regressions. Fewer futex calls or a lower microbenchmark median alone is not product benefit.
