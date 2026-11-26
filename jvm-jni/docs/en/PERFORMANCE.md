# Same-process JNI performance

[English](../en/PERFORMANCE.md) | [简体中文](../zh/PERFORMANCE.md)

The fixture measures synchronous native calls through the JVM's
`System.load` adapter. HotSpot, bionic and the GNU Guest share a native
child under the application's UID. The executable installs the
[Host signal bootstrap](../../../core/docs/en/SIGNAL_BOOTSTRAP.md) before
starting the JVM.

## Reproduction

Build and install the [System.load application fixture](SYSTEM_LOAD.md).
After the CheckJNI and signal regression passes, run the benchmark.

```sh
python3 jvm-jni/tests/same-thread/performance.py \
  --serial "$ANDROID_SERIAL" \
  --build jvm-jni/build/tests/system-load/arm64-v8a \
  --pairs 10 --threads 1 8
```

The runner verifies native inputs and the installed APK digest, uses a fresh
application process for every run, alternates worker/domain order, and retains
three ordinary bionic JNI reference runs for each concurrency level. It does
not change affinity, scheduler policy, spin budgets, or production defaults.
Only the test application is stopped and relaunched. Keep the device otherwise
idle; temperature and Android thermal status are saved before/after each run.

## Workloads and accounting

Both transports run identical Guest functions: scalar return, one Java callback,
four nested Java/native callbacks, summing 128-byte and 64-KiB arrays obtained
with GetByteArrayElements, and a 10,000-step native integer recurrence. The
ordinary reference builds the same fixture source with the Android compiler and
bypasses the Guest loader. Different compilers/libcs make it a reference floor,
not an exact algebraic subtraction of bridge instructions.

The System.load JNI fixture uses `-O3` for both the bionic and GNU Guest builds.
The [64 KiB array comparison](../../tests/results/same-thread/array64k/REPORT.md)
shows how the optimization level affects the generated traversal on ARM64.

Each case creates one or eight calling Java threads, warms each with eight
batches, and takes thirty batches of 256 calls (64 for the large array).
The process CPU/switch window starts after all callers finish warmup and ends
when all finish their batches. It includes all process threads, including
Guest workers, VM/ART background work, latch coordination and scheduling;
it is not CPU attributed exclusively to bridge functions. Aggregate wall time
per completed call is an inverse throughput metric at eight threads, not
individual request latency. Initialization and ClassLoader cleanup are outside
these windows.

A separate phase takes 512 individually clocked calls per caller. Their P99
includes clock instrumentation and mobile scheduling variability. This differs
from the median of amortized batch timings; it is not a P99 computed from batch
averages. Concurrent callers can finish at different times, so concurrency is
not held constant at the very end of either phase.

Return values/checksums are verified; the runner also requires the requested
Guest/direct loader, successful ClassLoader unload, zero live modules/closures,
VM destruction and the ART signal-chain route. CheckJNI, JFR, forced signal
probes and forced GC are excluded from timed work. Forced GC is used only
after measurement to verify unload. The VM retains the fixture's explicit
Xbatch/non-tiered compilation and low compilation threshold; this is a controlled
bridge benchmark, not a replication of arbitrary launcher JVM flags.

## Evidence

Every run directory under `jvm-jni/build/tests/system-load/arm64-v8a/performance`
contains native identities, APK hash, device state, compressed unedited output,
and incremental structured summaries. Comparisons use paired ratios and report
how many pairs improve and retain raw outliers. Historical ten-pair measurements from the former ART-inline fixture,
raw timing samples and artifact identities are in the
[performance archive](../../tests/results/same-thread/performance/REPORT.md).

To generate a report from a completed capture:

```sh
python3 jvm-jni/tests/same-thread/report-performance.py "$CAPTURE" \
  --output jvm-jni/build/tests/system-load/arm64-v8a/performance-report
```
