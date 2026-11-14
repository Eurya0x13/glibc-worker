package benchmark;

import dev.glibc.worker.NativeBridge;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Loaded by a disposable classloader, so benchmark natives must also unload. */
public final class ChildModule {
  public static native int echo(int value);
  public static native int roundtrip(int value);
  public static native int nested(int depth);
  public static native void hold(int milliseconds);

  public static int reply(int value) {
    return value + 7;
  }

  public static int bounce(int depth) {
    return depth == 0 ? 0 : nested(depth - 1) + 1;
  }

  private static void check(boolean value) {
    if (!value) {
      throw new AssertionError("Benchmark output mismatch");
    }
  }

  private static void trial(final int threads, final boolean callback, final int sample)
      throws Exception {
    final int warmup = 500, count = 2000;
    final long[][] values = new long[threads][count];
    final AtomicReference<Throwable> failure = new AtomicReference<Throwable>();
    final CountDownLatch ready = new CountDownLatch(threads);
    final CountDownLatch start = new CountDownLatch(1);
    final CountDownLatch done = new CountDownLatch(threads);
    Thread[] workers = new Thread[threads];

    for (int t = 0; t < threads; t++) {
      final int index = t;
      workers[t] = new Thread(() -> {
        try {
          for (int i = 0; i < warmup; i++) {
            check((callback ? roundtrip(i) : echo(i)) == (callback ? i + 7 : i ^ 0x13579));
          }
          ready.countDown();
          start.await();
          for (int i = 0; i < count; i++) {
            long before = System.nanoTime();
            int result = callback ? roundtrip(i) : echo(i);
            values[index][i] = System.nanoTime() - before;
            check(result == (callback ? i + 7 : i ^ 0x13579));
          }
        } catch (Throwable error) {
          failure.compareAndSet(null, error);
          ready.countDown();
        } finally {
          done.countDown();
        }
      }, "bridge-bench-" + t);
      workers[t].start();
    }

    ready.await();
    long idleWall = System.nanoTime(), idleCpu = NativeBridge.processCpuNanos();
    Thread.sleep(750);
    idleCpu = NativeBridge.processCpuNanos() - idleCpu;
    idleWall = System.nanoTime() - idleWall;

    long wall = System.nanoTime(), cpu = NativeBridge.processCpuNanos();
    start.countDown();
    done.await();
    cpu = NativeBridge.processCpuNanos() - cpu;
    wall = System.nanoTime() - wall;

    for (Thread worker : workers) {
      worker.join();
    }
    if (failure.get() != null) {
      throw new AssertionError(failure.get());
    }

    long[] sorted = new long[threads * count];
    for (int i = 0; i < threads; i++) {
      System.arraycopy(values[i], 0, sorted, i * count, count);
    }
    Arrays.sort(sorted);

    System.out.println("BRIDGE_BENCH threads=" + threads + " callback=" + callback + " sample="
        + sample + " calls=" + sorted.length + " p50_ns=" + sorted[sorted.length / 2] + " p95_ns="
        + sorted[(sorted.length * 95) / 100] + " p99_ns=" + sorted[(sorted.length * 99) / 100]
        + " max_ns=" + sorted[sorted.length - 1] + " wall_ns=" + wall + " cpu_ns=" + cpu
        + " idle_wall_ns=" + idleWall + " idle_cpu_ns=" + idleCpu);
  }

  public static void run(String directory) throws Exception {
    System.load(directory + "/libguest_bench.so");
    for (int threads : new int[] {1, 4, 8}) {
      for (boolean callback : new boolean[] {false, true}) {
        for (int sample = 0; sample < 3; sample++) {
          trial(threads, callback, sample);
        }
      }
    }
    for (int i = 0; i < 100; i++) {
      check(nested(6) == 6);
    }
    Thread slow = new Thread(() -> hold(300), "bridge-slow-guest");
    slow.start();
    for (int i = 0; i < 100; i++) {
      check(roundtrip(i) == i + 7);
    }
    slow.join();
    System.out.println("BRIDGE_BENCH_CHECK reentry_and_parallel_callbacks=PASS");
  }
}
