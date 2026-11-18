import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

/** Direct native invocations after real System.load; reflection stays outside timing. */
public final class SystemLoadBenchmark {
  private static final String[] CASES = {
      "scalar", "callback", "nested4", "array128", "array65536", "compute10000"};
  private static final int WARMUP = 8, BATCHES = 30, SAMPLES = 512;
  private static volatile long sink;

  private static long call(int kind, byte[] bytes) {
    switch (kind) {
      case 0:
        return SystemLoadFixture.echo(17);
      case 1:
        return SystemLoadFixture.callback(17);
      case 2:
        return SystemLoadFixture.nested(4);
      case 5:
        return SystemLoadFixture.compute(17);
      default:
        return SystemLoadFixture.array(bytes);
    }
  }

  private static long expected(int kind, byte[] bytes) {
    if (kind < 2) {
      return 24;
    }
    if (kind == 2) {
      return 4;
    }
    if (kind != 5) {
      return bytes.length * 7;
    }
    long value = 17;
    for (int i = 0; i < 10000; ++i) {
      value = value * 1664525 + 1013904223;
    }
    return value;
  }

  private static void await(CountDownLatch latch) throws InterruptedException {
    if (!latch.await(60, TimeUnit.SECONDS)) {
      throw new AssertionError("benchmark timeout");
    }
  }

  public static void run(int threads, Supplier<long[]> counters) throws Exception {
    if (threads != 1 && threads != 8) {
      throw new IllegalArgumentException("threads");
    }
    for (int kind = 0; kind < CASES.length; ++kind) {
      final int selected = kind;
      int iterations = kind == 4 ? 64 : 256;
      CountDownLatch ready = new CountDownLatch(threads), start = new CountDownLatch(1);
      CountDownLatch done = new CountDownLatch(threads), sample = new CountDownLatch(1);
      AtomicReference<Throwable> failure = new AtomicReference<>();
      long[][] batches = new long[threads][BATCHES], calls = new long[threads][SAMPLES];
      Thread[] workers = new Thread[threads];
      for (int t = 0; t < threads; ++t) {
        final int index = t;
        workers[t] = new Thread(() -> {
          try {
            byte[] bytes = new byte[selected == 4 ? 65536 : 128];
            Arrays.fill(bytes, (byte) 7);
            long answer = expected(selected, bytes), checksum = 0;
            for (int i = 0; i < WARMUP * iterations; ++i) {
              long actual = call(selected, bytes);
              if (actual != answer) {
                throw new AssertionError("warmup value");
              }
              checksum += actual;
            }
            ready.countDown();
            await(start);
            for (int batch = 0; batch < BATCHES; ++batch) {
              long before = System.nanoTime();
              for (int i = 0; i < iterations; ++i) {
                checksum += call(selected, bytes);
              }
              batches[index][batch] = System.nanoTime() - before;
            }
            if (checksum != answer * ((WARMUP + BATCHES) * iterations)) {
              throw new AssertionError("batch checksum");
            }
            sink = checksum;
            done.countDown();
            await(sample);
            // Separate instrumented single-call observations, never mislabeled
            // as the percentile of batch means; two clock reads add overhead.
            for (int i = 0; i < SAMPLES; ++i) {
              long before = System.nanoTime();
              long actual = call(selected, bytes);
              calls[index][i] = System.nanoTime() - before;
              if (actual != answer) {
                throw new AssertionError("sample value");
              }
            }
          } catch (Throwable error) {
            failure.compareAndSet(null, error);
            ready.countDown();
            done.countDown();
          }
        }, "bridge-bench-" + t);
        workers[t].start();
      }
      await(ready);
      if (failure.get() != null) {
        throw new AssertionError(failure.get());
      }
      long[] before = counters.get();
      long begin = System.nanoTime();
      start.countDown();
      await(done);
      long elapsed = System.nanoTime() - begin;
      long[] after = counters.get();
      sample.countDown();
      for (Thread worker : workers) {
        worker.join(65000);
        if (worker.isAlive()) {
          throw new AssertionError("benchmark join");
        }
      }
      if (failure.get() != null) {
        throw new AssertionError(failure.get());
      }
      System.out.println("SYSTEM_BENCH_RESOURCE {\"case\":\"" + CASES[kind]
          + "\",\"calls\":" + (threads * iterations * BATCHES) + ",\"wall_ns\":" + elapsed
          + ",\"cpu_ns\":" + (after[0] - before[0]) + ",\"voluntary\":" + (after[1] - before[1])
          + ",\"involuntary\":" + (after[2] - before[2]) + "}");
      for (int t = 0; t < threads; ++t) {
        System.out.println("SYSTEM_BENCH_SAMPLES {\"case\":\"" + CASES[kind] + "\",\"thread\":" + t
            + ",\"iterations\":" + iterations + ",\"warmup_batches\":" + WARMUP + ",\"batch_ns\":"
            + Arrays.toString(batches[t]) + ",\"call_ns\":" + Arrays.toString(calls[t]) + "}");
      }
    }
  }
}
