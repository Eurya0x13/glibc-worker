package benchmark;

import dev.glibc.worker.NativeBridge;
import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CountDownLatch;

/** Same Java/C workload, selected as a bionic or GNU library in a fresh process. */
public final class PayloadModule {
  private static native long configure(ByteBuffer buffer, byte[] array, int bytes);
  private static native long address();
  private static native int probe(int operation, int bytes, int nonce, long address);
  private static native long validate(int operation, int bytes, int calls);
  private static native void finish();
  private static native void diagnostics(String path);
  private static native int arrayChecks(Object[] arrays, Throwable expected);
  private static native long arrayAcquire(byte[] array);
  private static native void arrayRelease(byte[] array, long pointer);

  private static void checkArrays() throws Exception {
    int n = 4194304;
    Object[] arrays = {new boolean[n + 17], new byte[n + 17], new char[n / 2 + 17],
        new short[n / 2 + 17], new int[n / 4 + 17], new long[n / 8 + 17], new float[n / 4 + 17],
        new double[n / 8 + 17]};
    int checks = arrayChecks(arrays, new IllegalStateException("array-copy-pending"));
    if (checks != 104) {
      throw new AssertionError("Incomplete primitive array semantics checks: " + checks);
    }

    // More concurrent leases than the eight cache slots. Release on a
    // different Java thread, after the acquiring native call has returned.
    final byte[][] concurrent = new byte[12][4194304];
    final long[] pointers = new long[concurrent.length];
    final Throwable[] failures = new Throwable[concurrent.length];
    CountDownLatch start = new CountDownLatch(1);
    Thread[] threads = new Thread[concurrent.length];

    for (int i = 0; i < threads.length; i++) {
      final int index = i;
      threads[i] = new Thread(() -> {
        try {
          start.await();
          pointers[index] = arrayAcquire(concurrent[index]);
        } catch (Throwable failure) {
          failures[index] = failure;
        }
      });
      threads[i].start();
    }
    start.countDown();
    for (Thread thread : threads) {
      thread.join();
    }

    System.gc();
    for (int i = 0; i < pointers.length; i++) {
      if (failures[i] != null || pointers[i] == 0) {
        throw new AssertionError("Concurrent acquire failed", failures[i]);
      }
      for (int j = 0; j < i; j++)
        if (pointers[i] == pointers[j]) {
          throw new AssertionError("Overlapping live buffers");
        }
      arrayRelease(concurrent[i], pointers[i]);
      if (concurrent[i][0] != 13 || concurrent[i][4194303] != 27) {
        throw new AssertionError("Cross-thread copyback failed");
      }
    }

    byte[] oversized = new byte[9 * 1048576];
    long pointer = arrayAcquire(oversized);
    if (pointer == 0) {
      throw new AssertionError("Oversized array fallback failed");
    }
    arrayRelease(oversized, pointer);
    if (oversized[0] != 13 || oversized[oversized.length - 1] != 27) {
      throw new AssertionError("Oversized array copyback failed");
    }

    System.out.println("ARRAY_COPY_TEST_SUMMARY primitive_checks=" + checks
        + " types=8 concurrent_leases=12 cross_thread_release=PASS"
        + " oversized_fallback=PASS result=PASS");
  }

  private static final int[] SIZES = {0, 64, 1024, 16384, 65536, 262144, 1048576, 4194304};
  private static final int[] THRESHOLD_SIZES = {
      0, 64, 1048576, 1572864, 2093056, 2097152, 2101248, 2621440, 3145728, 4194304};
  private static final String[] OPERATIONS = {
      "pointer", "direct_address", "memcpy", "array_region", "array_elements"};

  public static int run(String directory, String mode, int round) throws Exception {
    System.load(directory + (mode.equals("host") ? "/libpayload_host.so" : "/libguest_payload.so"));
    checkArrays();

    boolean diagnostic =
        new File(System.getProperty("java.home"), "payload-diagnostic-enabled").isFile();
    diagnostics(diagnostic
            ? new File(System.getProperty("java.home"), "payload-stages.bin").getPath()
            : null);
    boolean threshold =
        new File(System.getProperty("java.home"), "payload-threshold-enabled").isFile();
    int[] sizes = threshold ? THRESHOLD_SIZES : SIZES;

    ByteBuffer buffer = ByteBuffer.allocateDirect(sizes[sizes.length - 1]);
    List<Integer> order = new ArrayList<Integer>();
    for (int i = 0; i < sizes.length * OPERATIONS.length; i++)
      if (!threshold || i / sizes.length == 4) {
        order.add(i);
      }
    Collections.shuffle(order, new Random(0x67777061796c6f61L + round));
    int rows = 0;

    try (DataOutputStream raw = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(
             new File(System.getProperty("java.home"), "payload-samples.bin"))))) {
      raw.writeInt(0x47575031);
      for (int point : order) {
        int operation = point / sizes.length, bytes = sizes[point % sizes.length];
        byte[] array = new byte[bytes];
        for (int i = 0; i < bytes; i++) {
          array[i] = (byte) (i * 31 + 7);
        }
        buffer.clear();
        buffer.put(array);
        long identity = configure(buffer, array, bytes);
        long address = address();
        if ((identity >>> 32) != (mode.equals("host") ? 1 : 2)) {
          throw new AssertionError("Wrong native payload domain");
        }
        int fingerprint =
            operation >= 2 && bytes > 0 ? (array[0] & 255) ^ (array[bytes - 1] & 255) : 0;
        final int warmup = 300, count = 500;
        int accumulatedCalls = 0;
        if (diagnostic || threshold) {
          NativeBridge.payloadPhase(0, operation, bytes);
        }

        for (int trial = 0; trial < 3; trial++) {
          for (int i = 0; i < warmup; i++)
            if (probe(operation, bytes, i, address) != (i ^ bytes ^ fingerprint)) {
              throw new AssertionError("Payload warmup mismatch");
            }
          long[] samples = new long[count];
          long cpu = NativeBridge.processCpuNanos(), wall = System.nanoTime();
          for (int i = 0; i < count; i++) {
            long before = System.nanoTime();
            int result = probe(operation, bytes, i, address);
            samples[i] = System.nanoTime() - before;
            if (result != (i ^ bytes ^ fingerprint)) {
              throw new AssertionError("Payload result mismatch");
            }
          }
          wall = System.nanoTime() - wall;
          cpu = NativeBridge.processCpuNanos() - cpu;
          accumulatedCalls += warmup + count;
          long copyCalls = validate(operation, bytes, accumulatedCalls);
          raw.writeInt(operation);
          raw.writeInt(bytes);
          raw.writeInt(trial);
          raw.writeInt(count);
          int over1ms = 0, over5ms = 0;
          for (long value : samples) {
            raw.writeLong(value);
            if (value >= 1_000_000L) {
              over1ms++;
            }
            if (value >= 5_000_000L) {
              over5ms++;
            }
          }
          Arrays.sort(samples);
          System.out.println("PAYLOAD_BENCH mode=" + mode + " round=" + round + " point=" + point
              + " operation=" + OPERATIONS[operation] + " bytes=" + bytes + " trial=" + trial
              + " calls=" + count + " warmup=" + warmup + " p50_ns=" + samples[count / 2]
              + " p95_ns=" + samples[count * 95 / 100] + " p99_ns=" + samples[count * 99 / 100]
              + " max_ns=" + samples[count - 1] + " wall_ns=" + wall + " cpu_ns=" + cpu
              + " over_1ms=" + over1ms + " over_5ms=" + over5ms
              + " elements_copy_calls=" + copyCalls + " validated_calls=" + accumulatedCalls
              + " native_tid=" + (identity & 0xffffffffL));
          rows++;
        }

        if (diagnostic || threshold) {
          NativeBridge.payloadPhase(1, operation, bytes);
        }
      }
    } finally {
      finish();
    }

    System.out.println("PAYLOAD_BENCH_CHECK rows=" + rows + " buffers=PASS domain=PASS");
    return rows;
  }
}
