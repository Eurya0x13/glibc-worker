# Paired bridge performance results

Android ARM64 Vivo V2239A / Android 13, pinned Android JRE17; ART, HotSpot, bionic and glibc in one application process.
Capture: `20261115T113637558227Z`. Same APK SHA256 `7db5cedb531bbfc965e4276c1d4637bd39cb941971ba1223b666456bb1f8524f`.
10 alternating worker/domain pairs per concurrency level, plus three ordinary bionic JNI reference runs each.
All 46 runs passed value/checksum, loader selection and unload/VM-destruction checks.
Battery temperature 26.0–28.2 °C; observed Android thermal statuses: [0].
Status 0 at boundaries does not prove fixed CPU frequencies or exclude transient scheduling noise.

## Amortized native invocation time

Medians of per-run batch medians, µs/call. Reduction is the median of paired ratios, so it need not equal the ratio of displayed medians.

| Threads | Case | Worker | Same thread | Paired reduction | Faster pairs | Bionic reference |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | scalar | 7.320 | 0.656 | 91.1% | 10/10 | 0.054 |
| 1 | callback | 5.317 | 1.147 | 78.3% | 10/10 | 0.203 |
| 1 | nested4 | 8.570 | 5.433 | 35.6% | 10/10 | 0.839 |
| 1 | array128 | 8.914 | 1.513 | 83.0% | 10/10 | 0.205 |
| 1 | array65536 | 53.100 | 32.279 | 38.9% | 10/10 | 8.197 |
| 1 | compute10000 | 14.743 | 8.338 | 44.7% | 10/10 | 7.265 |
| 8 | scalar | 15.227 | 6.178 | 58.4% | 10/10 | 0.199 |
| 8 | callback | 17.420 | 13.133 | 24.8% | 10/10 | 0.341 |
| 8 | nested4 | 78.143 | 62.844 | 18.3% | 10/10 | 1.460 |
| 8 | array128 | 25.897 | 15.900 | 38.0% | 10/10 | 0.751 |
| 8 | array65536 | 216.682 | 39.194 | 81.9% | 10/10 | 8.311 |
| 8 | compute10000 | 48.871 | 9.806 | 79.8% | 10/10 | 8.221 |

## CPU, observed long tails and scheduling

Process CPU µs/completed call includes all Host/Guest/VM/ART threads during the concurrent batch window. P99 is measured in a separate individually clocked-call phase; it includes clock overhead and is not batch-mean P99.

| Threads | Case | CPU worker → same thread | CPU reduction | P99 worker → same thread (µs) | Voluntary switches/call worker → same thread |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | scalar | 13.552 → 1.489 | 88.8% | 10.539 → 1.500 | 1.000 → 0.001 |
| 1 | callback | 9.691 → 1.231 | 87.4% | 9.635 → 2.077 | 0.491 → 0.000 |
| 1 | nested4 | 20.322 → 5.460 | 72.6% | 17.712 → 6.115 | 0.151 → 0.000 |
| 1 | array128 | 16.327 → 1.751 | 88.7% | 10.057 → 2.308 | 0.732 → 0.001 |
| 1 | array65536 | 92.340 → 44.068 | 52.8% | 196.615 → 52.577 | 1.995 → 0.062 |
| 1 | compute10000 | 25.911 → 12.606 | 49.3% | 18.500 → 8.789 | 0.858 → 0.015 |
| 8 | scalar | 12.017 → 3.549 | 70.6% | 345.365 → 69.154 | 0.518 → 0.095 |
| 8 | callback | 14.748 → 6.945 | 52.3% | 392.250 → 108.942 | 0.480 → 0.221 |
| 8 | nested4 | 69.266 → 27.970 | 59.4% | 1103.039 → 285.904 | 1.758 → 0.978 |
| 8 | array128 | 21.917 → 7.365 | 66.0% | 498.692 → 136.654 | 0.681 → 0.249 |
| 8 | array65536 | 167.199 → 51.705 | 69.0% | 771.154 → 215.116 | 3.768 → 0.016 |
| 8 | compute10000 | 40.930 → 12.247 | 70.9% | 389.096 → 77.750 | 1.453 → 0.008 |

Long-tail consistency: 1 thread(s), nested4: 9/10 lower-P99 pairs; 1 thread(s), array65536: 9/10 lower-P99 pairs; 1 thread(s), compute10000: 9/10 lower-P99 pairs. All other cases improved P99 in every pair. These exceptions are retained; their cause was not established.

## Interpretation and limits

The result applies to warmed synchronous System.load native calls and these exact artifacts. It does not measure Pojav/FPS, Guest-created asynchronous threads, startup cost, arbitrary JREs, or independent Host/Guest signal virtualization.
The bionic reference uses the same C source with a different compiler/libc. It is a reference, not an exact subtraction isolating bridge instructions. Process CPU includes background work; it is not a sampled bridge-only CPU profile or a power measurement.
No affinity, spin-budget or production-default changes were made. Fixed warmup, fresh processes, AB/BA ordering, original outliers, temperature snapshots, native/APK hashes and all raw samples are retained. The pilot is excluded from this formal result. capture.json.gz preserves unedited run output and temperature snapshots; general device properties are restricted to build/model/ABI metadata.
See [PERFORMANCE.md](../../../../docs/en/PERFORMANCE.md) for reproduction, JVM flags, timing boundaries and instrumentation limitations.
