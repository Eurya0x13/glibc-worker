package real;

import com.github.luben.zstd.*;
import com.github.luben.zstd.util.Native;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.CountDownLatch;

/** Test client only. All com.github.luben classes and native bytes are upstream. */
public final class ZstdWorkload {
  private static int checks;

  private static void check(boolean pass, String name) {
    checks++;
    System.out.println("ZSTD_WORKLOAD " + name + "=" + (pass ? "PASS" : "FAIL"));
    if (!pass) {
      throw new AssertionError(name);
    }
  }

  public static void loadImage(String path) {
    System.load(path);
  }

  public static void ensureLoaded() {
    Native.load();
  }

  public static long bound() {
    return Zstd.compressBound(1024);
  }

  private static byte[] data(int size, int seed) {
    byte[] bytes = new byte[size];
    new Random(seed).nextBytes(bytes);
    for (int i = 0; i < size; i++)
      if (i % 4 != 0) {
        bytes[i] = (byte) (i % 67);
      }
    return bytes;
  }

  private static void roundtrip(byte[] bytes) {
    byte[] compressed = Zstd.compress(bytes, 3);
    if (Zstd.getFrameContentSize(compressed) != bytes.length
        || !Arrays.equals(bytes, Zstd.decompress(compressed, bytes.length))) {
      throw new AssertionError("Array roundtrip");
    }
  }

  private static void direct(byte[] bytes) {
    ByteBuffer src = ByteBuffer.allocateDirect(bytes.length + 32);
    src.position(13);
    src.put(bytes);

    ByteBuffer compressed = ByteBuffer.allocateDirect((int) Zstd.compressBound(bytes.length) + 32);
    long n = Zstd.compressDirectByteBuffer(
        compressed, 7, compressed.capacity() - 7, src, 13, bytes.length, 3);
    if (Zstd.isError(n)) {
      throw new AssertionError(Zstd.getErrorName(n));
    }

    ByteBuffer dest = ByteBuffer.allocateDirect(bytes.length + 32);
    long size = Zstd.decompressDirectByteBuffer(dest, 11, bytes.length, compressed, 7, (int) n);
    byte[] actual = new byte[bytes.length];
    dest.position(11);
    dest.get(actual);
    check(size == bytes.length && Arrays.equals(bytes, actual) && dest.get(10) == 0
            && dest.get(11 + bytes.length) == 0,
        "direct_buffer_offsets_and_bytes");
  }

  private static void streams(byte[] bytes) throws Exception {
    ByteArrayOutputStream compressed = new ByteArrayOutputStream();
    try (ZstdOutputStream out = new ZstdOutputStream(compressed).setChecksum(true)) {
      for (int offset = 0; offset < bytes.length; offset += 4093) {
        out.write(bytes, offset, Math.min(4093, bytes.length - offset));
      }
    }

    ByteArrayOutputStream actual = new ByteArrayOutputStream();
    try (ZstdInputStream in =
             new ZstdInputStream(new ByteArrayInputStream(compressed.toByteArray()))) {
      byte[] buffer = new byte[8191];
      int n;
      while ((n = in.read(buffer)) != -1) {
        actual.write(buffer, 0, n);
      }
    }
    check(Arrays.equals(bytes, actual.toByteArray()), "stream_checksum_roundtrip");
  }

  private static Map<String, String> tasks() throws IOException {
    String[] names = new File("/proc/self/task").list();
    Map<String, String> tasks = new TreeMap<String, String>();

    for (String tid : names == null ? new String[0] : names) {
      Path path = Paths.get("/proc/self/task", tid);
      try {
        String name =
            new String(Files.readAllBytes(path.resolve("comm")), StandardCharsets.UTF_8).trim();
        String stat = new String(Files.readAllBytes(path.resolve("stat")), StandardCharsets.UTF_8);
        String[] fields = stat.substring(stat.lastIndexOf(')') + 2).split("\\s+");
        // Start time distinguishes a reused kernel TID from the original worker.
        tasks.put(tid + "@" + fields[19], name);
      } catch (NoSuchFileException exited) {
        // A concurrent VM/ART task can exit while taking the snapshot.
      }
    }

    return tasks;
  }

  private static void contexts(String guestName) throws Exception {
    byte[] bytes = data(4 * 1024 * 1024, 773);
    Map<String, String> before = tasks();
    CountDownLatch release = new CountDownLatch(1);
    CountDownLatch entered = new CountDownLatch(1);

    Thread unrelated = new Thread(() -> {
      entered.countDown();
      try {
        release.await();
      } catch (InterruptedException interrupted) {
        Thread.currentThread().interrupt();
      }
    }, "zstd-foreign");
    unrelated.start();

    try {
      entered.await();
      Set<String> extra;
      ZstdCompressCtx compressor = new ZstdCompressCtx();
      ZstdDecompressCtx decompressor = new ZstdDecompressCtx();

      try {
        compressor.setLevel(3).setChecksum(true).setWorkers(2);
        byte[] compressed = compressor.compress(bytes);
        check(Arrays.equals(bytes, decompressor.decompress(compressed, bytes.length)),
            "native_context_worker_roundtrip");
        Map<String, String> created = tasks();
        created.keySet().removeAll(before.keySet());
        extra = new HashSet<String>(created.keySet());
        // The pinned upstream Zstd pool inherits its GNU caller's kernel name.
        // VM/ART helpers and the deliberately live observer are not pool workers.
        extra.removeIf(tid -> !guestName.equals(created.get(tid)));
        System.out.println("ZSTD_NATIVE_WORKERS new_tids=" + extra + " created=" + created
            + " guest_parent_name=" + guestName);
        check(extra.size() >= 2, "native_pthread_workers_created");
        compressor.reset();
        decompressor.reset();
        check(
            Arrays.equals(bytes, decompressor.decompress(compressor.compress(bytes), bytes.length)),
            "native_context_reset_reuse");
      } finally {
        compressor.close();
        decompressor.close();
      }

      compressor.close();
      decompressor.close();

      boolean closed = false;
      try {
        compressor.compress(new byte[1]);
      } catch (IllegalStateException expected) {
        closed = true;
      }
      check(closed, "closed_context_rejects_use_and_double_close_safe");

      long deadline = System.nanoTime() + 3_000_000_000L;
      while (System.nanoTime() < deadline) {
        Set<String> live = new HashSet<String>(tasks().keySet());
        live.retainAll(extra);
        if (live.isEmpty()) {
          break;
        }
        Thread.sleep(10);
      }

      Set<String> live = new HashSet<String>(tasks().keySet());
      live.retainAll(extra);
      check(live.isEmpty() && unrelated.isAlive(), "native_workers_joined_on_close");
    } finally {
      release.countDown();
      unrelated.join(3000);
      if (unrelated.isAlive()) {
        throw new AssertionError("Unrelated observer did not stop");
      }
    }
  }

  public static int run(String guestName) throws Exception {
    System.out.println("ZSTD_ORIGINAL_LOADER os=" + System.getProperty("os.name")
        + " arch=" + System.getProperty("os.arch") + " loader=" + Zstd.class.getClassLoader());
    check(System.getProperty("ZstdNativePath") == null
            && System.getProperty("ZstdTempFolder") == null && !Native.isLoaded(),
        "no_loader_override_or_assume_loaded");
    check(bound() > 1024 && Native.isLoaded(), "original_native_loader_completed");

    for (int n : new int[] {0, 1, 257, 65536, 1048576}) {
      roundtrip(data(n, n + 7));
      check(true, "array_roundtrip_" + n);
    }

    byte[] bytes = data(256 * 1024, 443);
    direct(bytes);
    streams(bytes);

    boolean tooSmall = false;
    try {
      Zstd.decompress(new byte[8], Zstd.compress(bytes));
    } catch (ZstdException expected) {
      tooSmall = expected.getMessage().contains("too small");
    }
    check(tooSmall && Zstd.isError(-Zstd.errDstSizeTooSmall())
            && Zstd.getErrorName(-Zstd.errDstSizeTooSmall()).length() > 0,
        "native_error_code_and_name");

    boolean invalid = false;
    try {
      Zstd.decompress(new byte[31], 123);
    } catch (ZstdException expected) {
      invalid = true;
    }
    check(invalid, "corrupt_frame_exception");

    contexts(guestName);

    final List<Throwable> errors = Collections.synchronizedList(new ArrayList<Throwable>());
    Thread[] threads = new Thread[4];
    for (int i = 0; i < threads.length; i++) {
      final int seed = i;
      threads[i] = new Thread(() -> {
        try {
          for (int j = 0; j < 12; j++) {
            roundtrip(data(128 * 1024, seed * 100 + j));
          }
        } catch (Throwable t) {
          errors.add(t);
        }
      }, "zstd-java-" + i);
      threads[i].start();
    }
    for (Thread thread : threads) {
      thread.join();
    }
    check(errors.isEmpty(), "four_java_threads_48_roundtrips_" + errors);

    for (int i = 0; i < 16; i++) {
      try (ZstdCompressCtx c = new ZstdCompressCtx();
          ZstdDecompressCtx d = new ZstdDecompressCtx()) {
        byte[] b = data(8192, i);
        if (!Arrays.equals(b, d.decompress(c.compress(b), b.length))) {
          throw new AssertionError("Context cycle");
        }
      }
      check(true, "sixteen_native_context_create_close_cycles");
    }

    System.gc();
    roundtrip(bytes);
    check(true, "java_gc_then_native_call");

    return checks;
  }
}
