# 64 KiB JNI array traversal on Android ARM64

Vivo V2239A, Android 13, Pojav JRE 17, one calling Java thread. Each
optimization level used a fixed APK and ten fresh-process A/B pairs in
alternating order. The bionic reference used NDK Clang 21 and the
same-thread Guest used GNU GCC 14.2.1. Every run verified the checksum,
loader selection, class-loader unload and VM shutdown. Android reported
thermal status 0 at the start and end of each run.

The [JNI fixture](../../../same-thread/system-load-fixture.c) obtains a copy
with `GetByteArrayElements`, sums the signed bytes, and releases the copy
with `JNI_ABORT`. Values below are medians of warmed batch averages in
µs/call. Differences are medians of the within-pair differences.

| End-to-end workload | bionic JNI | Same-thread Guest | Paired difference |
| --- | ---: | ---: | ---: |
| Both modules at `-O2` | 8.79 | 34.64 | +26.20 |
| Both modules at `-O3` | 8.83 | 17.65 | +8.80 |
| `-O3` modules with identical traversal machine code | 8.63 | 9.49 | +0.90 |

GCC generated a byte-at-a-time reduction at `-O2` and a NEON reduction at
`-O3`. Its `-O3` main loop handles 16 bytes and uses a single vector
accumulator. Clang's main loop handles 32 bytes with independent
accumulators. In separate `-O3` diagnostic batches the native walk took
5.35 µs with Clang and 12.74 µs with GCC. The identical-code control linked
one compiled traversal object into both modules and verified the machine
code by hash.

[Measurements](measurements.json) retains the twenty pairs of per-run
medians, the phase timings and artifact identities. The array comparison
was recorded from a diagnostic build of `cb32eff`. See the
[System.load benchmark](../../../../docs/en/PERFORMANCE.md) for the regular
fixture workflow.
