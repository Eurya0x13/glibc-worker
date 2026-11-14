package compat;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.channels.ClosedChannelException;
import java.util.Arrays;
import java.util.Locale;
import java.util.Random;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.xerial.snappy.OSInfo;
import org.xerial.snappy.Snappy;
import org.xerial.snappy.SnappyError;
import org.xerial.snappy.SnappyErrorCode;
import org.xerial.snappy.SnappyFramedInputStream;
import org.xerial.snappy.SnappyFramedOutputStream;
import org.xerial.snappy.SnappyInputStream;
import org.xerial.snappy.SnappyOutputStream;

/** Standalone client of the unchanged snappy-java 1.1.10.12 JAR and GNU image. */
public final class SnappyWorkload {
  private static int checks;

  private SnappyWorkload() {}

  private static void check(boolean pass, String name) {
    checks++;
    System.out.println("NATIVE_JAR_TEST library=snappy name=snappy_" + name
        + " result=" + (pass ? "PASS" : "FAIL"));
    if (!pass) {
      throw new AssertionError(name);
    }
  }

  public static void ensureLoaded() {
    if (Snappy.maxCompressedLength(32) < 32) {
      throw new AssertionError("Snappy native bound");
    }
  }

  /** No additional global check state: safe for repeated calls by a fresh loader. */
  public static void probe() {
    try {
      roundtrip(data(257, 17));
    } catch (IOException error) {
      throw new AssertionError("Snappy probe", error);
    }
  }

  private static byte[] data(int size, int seed) {
    byte[] bytes = new byte[size];
    new Random(seed).nextBytes(bytes);
    for (int i = 0; i < bytes.length; i++)
      if ((i & 3) != 0) {
        bytes[i] = (byte) (i % 71);
      }
    return bytes;
  }

  private static void roundtrip(byte[] bytes) throws IOException {
    byte[] compressed = Snappy.compress(bytes);
    if (!Snappy.isValidCompressedBuffer(compressed)
        || Snappy.uncompressedLength(compressed) != bytes.length
        || !Arrays.equals(bytes, Snappy.uncompress(compressed))) {
      throw new AssertionError("Snappy array roundtrip");
    }
  }

  private static void nativeIdentity() throws Exception {
    String selected = OSInfo.getNativeLibFolderPathForCurrentOS();
    String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
    String expected = "aarch64".equals(arch)              ? "Linux/aarch64"
        : ("amd64".equals(arch) || "x86_64".equals(arch)) ? "Linux/x86_64"
                                                            : null;

    System.out.println("NATIVE_JAR_PLATFORM library=snappy selected=" + selected
        + " os=" + System.getProperty("os.name") + " arch=" + System.getProperty("os.arch")
        + " runtime=" + System.getProperty("java.runtime.name"));
    check(expected != null && expected.equals(selected) && !OSInfo.isAndroid(),
        "upstream_selects_linux_glibc_resource");

    String[] overrides = {"org.xerial.snappy.lib.path", "org.xerial.snappy.lib.name",
        "org.xerial.snappy.use.systemlib", "org.xerial.snappy.disable.bundled.libs",
        "org.xerial.snappy.purejava"};
    boolean unset = true;
    for (String key : overrides) {
      unset &= System.getProperty(key) == null;
    }
    check(unset, "no_native_path_or_java_fallback_override");

    ensureLoaded();
    Field field = Snappy.class.getDeclaredField("impl");
    field.setAccessible(true);
    Object implementation = field.get(null);
    Class<?> type = implementation.getClass();
    check("org.xerial.snappy.SnappyNative".equals(type.getName()),
        "original_loader_installs_native_implementation");

    Method objectCompress = type.getDeclaredMethod(
        "rawCompress", Object.class, int.class, int.class, Object.class, int.class);
    Method directCompress = type.getDeclaredMethod(
        "rawCompress", ByteBuffer.class, int.class, int.class, ByteBuffer.class, int.class);
    Method version = type.getDeclaredMethod("nativeLibraryVersion");
    check(Modifier.isNative(objectCompress.getModifiers())
            && Modifier.isNative(directCompress.getModifiers())
            && Modifier.isNative(version.getModifiers()),
        "object_and_direct_overloaded_jni_methods_present");

    // The public version helper reads VERSION; invoke the already-loaded
    // implementation's native method to exercise a real JNI String result.
    String nativeVersion = (String) version.invoke(implementation);
    String resourceVersion = Snappy.getNativeLibraryVersion();
    System.out.println("NATIVE_JAR_VERSION library=snappy artifact=1.1.10.12 native="
        + nativeVersion + " resource=" + resourceVersion);
    // Upstream SnappyNative.cpp still returns the literal 1.1.3; VERSION
    // describes Snappy 1.1.10. Neither value is this artifact's 1.1.10.12.
    check("1.1.3".equals(nativeVersion) && "1.1.10".equals(resourceVersion),
        "native_version_entrypoint_and_upstream_resource_values");
  }

  private static void arrayOffsets() throws IOException {
    byte[] bytes = data(32769, 203);
    byte[] input = new byte[bytes.length + 27];
    System.arraycopy(bytes, 0, input, 13, bytes.length);

    byte[] compressed = new byte[Snappy.maxCompressedLength(bytes.length) + 23];
    Arrays.fill(compressed, (byte) 0x5a);
    int packed = Snappy.compress(input, 13, bytes.length, compressed, 7);

    byte[] actual = new byte[bytes.length + 29];
    Arrays.fill(actual, (byte) 0x73);
    int restored = Snappy.uncompress(compressed, 7, packed, actual, 11);
    check(restored == bytes.length
            && Arrays.equals(bytes, Arrays.copyOfRange(actual, 11, 11 + restored))
            && actual[10] == 0x73 && actual[11 + restored] == 0x73 && compressed[6] == 0x5a
            && compressed[7 + packed] == 0x5a,
        "array_offsets_and_guard_bytes");
  }

  private static void primitiveArrays() throws IOException {
    char[] chars = {'\0', 'A', '\u4e2d', '\uffff'};
    short[] shorts = {Short.MIN_VALUE, -1, 0, 7, Short.MAX_VALUE};
    int[] integers = {Integer.MIN_VALUE, -17, 0, 91, Integer.MAX_VALUE};
    long[] longs = {Long.MIN_VALUE, -1, 0, 0x123456789abcdefL, Long.MAX_VALUE};
    float[] floats = {-0.0f, 0.0f, -1.25f, Float.POSITIVE_INFINITY, Float.NaN};
    double[] doubles = {-0.0d, 0.0d, -1.25d, Double.NEGATIVE_INFINITY, Double.NaN};

    check(Arrays.equals(chars, Snappy.uncompressCharArray(Snappy.compress(chars))),
        "char_array_roundtrip");
    check(Arrays.equals(shorts, Snappy.uncompressShortArray(Snappy.compress(shorts))),
        "short_array_roundtrip");
    check(Arrays.equals(integers, Snappy.uncompressIntArray(Snappy.compress(integers))),
        "int_array_roundtrip");
    check(Arrays.equals(longs, Snappy.uncompressLongArray(Snappy.compress(longs))),
        "long_array_roundtrip");
    check(Arrays.equals(floats, Snappy.uncompressFloatArray(Snappy.compress(floats))),
        "float_array_roundtrip");
    check(Arrays.equals(doubles, Snappy.uncompressDoubleArray(Snappy.compress(doubles))),
        "double_array_roundtrip");

    String text = "Snappy JNI \u4e2d\u6587 \u0000 \ud83d\ude80";
    check(text.equals(Snappy.uncompressString(Snappy.compress(text, "UTF-8"), "UTF-8")),
        "utf8_string_roundtrip");
  }

  private static void fill(ByteBuffer buffer, byte value) {
    for (int i = 0; i < buffer.capacity(); i++) {
      buffer.put(i, value);
    }
  }

  private static byte absolute(ByteBuffer buffer, int index) {
    ByteBuffer all = buffer.duplicate();
    all.clear();
    return all.get(index);
  }

  private static void directRoundtrip(byte[] bytes) throws IOException {
    ByteBuffer backing = ByteBuffer.allocateDirect(bytes.length + 64);
    backing.position(17);
    ByteBuffer input = backing.slice();
    input.position(3);
    input.put(bytes);
    input.limit(3 + bytes.length);
    input.position(3);

    ByteBuffer compressed =
        ByteBuffer.allocateDirect(Snappy.maxCompressedLength(bytes.length) + 31);
    fill(compressed, (byte) 0x5a);
    compressed.position(7);
    int packed = Snappy.compress(input, compressed);
    if (input.position() != 3 || input.limit() != 3 + bytes.length || compressed.position() != 7
        || compressed.limit() != 7 + packed || !Snappy.isValidCompressedBuffer(compressed)
        || Snappy.uncompressedLength(compressed) != bytes.length) {
      throw new AssertionError("Snappy direct buffer ranges");
    }

    ByteBuffer output = ByteBuffer.allocateDirect(bytes.length + 37);
    fill(output, (byte) 0x73);
    output.position(11);
    int restored = Snappy.uncompress(compressed, output);
    byte[] actual = new byte[bytes.length];
    output.get(actual);
    if (restored != bytes.length || !Arrays.equals(bytes, actual) || absolute(output, 10) != 0x73
        || absolute(output, 11 + restored) != 0x73 || absolute(compressed, 6) != 0x5a
        || absolute(compressed, 7 + packed) != 0x5a) {
      throw new AssertionError("Snappy direct offsets and guards");
    }
  }

  private static final class TrackedOutput extends ByteArrayOutputStream {
    int closes;

    @Override
    public void close() throws IOException {
      closes++;
      super.close();
    }
  }

  private static final class TrackedInput extends ByteArrayInputStream {
    boolean closed;

    TrackedInput(byte[] bytes) {
      super(bytes);
    }

    @Override
    public void close() throws IOException {
      closed = true;
      super.close();
    }
  }

  private static byte[] readBounded(InputStream input, int maximum) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    byte[] buffer = new byte[8191];
    int n;
    while ((n = input.read(buffer)) != -1) {
      if (n == 0 || out.size() > maximum - n) {
        throw new IOException("Unexpected stream size or no progress");
      }
      out.write(buffer, 0, n);
    }
    return out.toByteArray();
  }

  private static byte[] streams(byte[] bytes) throws IOException {
    TrackedOutput raw = new TrackedOutput();
    SnappyOutputStream out = new SnappyOutputStream(raw);

    try {
      for (int offset = 0; offset < bytes.length; offset += 4093) {
        out.write(bytes, offset, Math.min(4093, bytes.length - offset));
      }
      out.flush();
    } finally {
      out.close();
    }
    out.close();

    boolean closed = false;
    try {
      out.write(1);
    } catch (IOException expected) {
      closed = true;
    }
    check(closed && raw.closes == 1, "legacy_stream_close_is_idempotent_and_write_rejected");

    TrackedInput source = new TrackedInput(raw.toByteArray());
    byte[] restored;
    try (SnappyInputStream input = new SnappyInputStream(source, 1024 * 1024)) {
      restored = readBounded(input, bytes.length);
    }
    check(source.closed && Arrays.equals(bytes, restored),
        "legacy_stream_chunked_roundtrip_and_input_close");

    TrackedOutput framed = new TrackedOutput();
    SnappyFramedOutputStream frameOut = new SnappyFramedOutputStream(framed);
    try {
      for (int offset = 0; offset < bytes.length; offset += 4093) {
        frameOut.write(bytes, offset, Math.min(4093, bytes.length - offset));
      }
      frameOut.flush();
    } finally {
      frameOut.close();
    }
    frameOut.close();

    closed = false;
    try {
      frameOut.write(1);
    } catch (IOException expected) {
      closed = true;
    }
    check(closed && framed.closes == 1, "framed_stream_close_is_idempotent_and_write_rejected");

    TrackedInput frameSource = new TrackedInput(framed.toByteArray());
    SnappyFramedInputStream frameIn = new SnappyFramedInputStream(frameSource, true);
    try {
      restored = readBounded(frameIn, bytes.length);
    } finally {
      frameIn.close();
    }
    frameIn.close();

    boolean readClosed = false;
    try {
      frameIn.read(new byte[1]);
    } catch (ClosedChannelException expected) {
      readClosed = true;
    }
    check(frameSource.closed && readClosed && Arrays.equals(bytes, restored),
        "framed_checksum_roundtrip_and_closed_input_rejected");

    return framed.toByteArray();
  }

  private static void errors(byte[] framed) throws IOException {
    byte[] invalid = {(byte) 0x80}; // Truncated length varint, with bounded input size.
    check(!Snappy.isValidCompressedBuffer(invalid), "invalid_array_block_detected");

    boolean rejected = false;
    try {
      Snappy.uncompress(invalid);
    } catch (IOException expected) {
      rejected = true;
    }
    check(rejected, "invalid_array_block_throws_ioexception");
    rejected = false;
    try {
      Snappy.compress(new byte[32], 0, 32, new byte[1], 0);
    } catch (IllegalArgumentException expected) {
      rejected = true;
    }
    check(rejected, "small_array_output_rejected_before_native_write");
    rejected = false;
    try {
      Snappy.compress(ByteBuffer.allocate(32), ByteBuffer.allocateDirect(128));
    } catch (SnappyError expected) {
      rejected = expected.errorCode == SnappyErrorCode.NOT_A_DIRECT_BUFFER;
    }
    check(rejected, "heap_buffer_rejected_by_direct_api");

    byte[] corrupt = framed.clone();
    if (corrupt.length <= 18) {
      throw new AssertionError("Expected a framed data block");
    }
    // 10-byte stream identifier, 4-byte chunk header, then its masked CRC.
    corrupt[14] ^= 1;
    rejected = false;
    try (SnappyFramedInputStream input =
             new SnappyFramedInputStream(new ByteArrayInputStream(corrupt), true)) {
      readBounded(input, 256 * 1024);
    } catch (IOException expected) {
      rejected = true;
    }
    check(rejected, "framed_checksum_corruption_rejected");
    // No raw address calls or malformed direct-buffer decompression: upstream
    // explicitly warns that those low-level entry points can access invalid memory.
  }

  private static void concurrent() throws Exception {
    final CountDownLatch ready = new CountDownLatch(4), start = new CountDownLatch(1);
    final ConcurrentLinkedQueue<Throwable> failures = new ConcurrentLinkedQueue<Throwable>();
    Thread[] threads = new Thread[4];

    for (int i = 0; i < threads.length; i++) {
      final int seed = i;
      threads[i] = new Thread(() -> {
        ready.countDown();
        try {
          if (!start.await(10, TimeUnit.SECONDS)) {
            throw new AssertionError("Snappy worker start timeout");
          }
          for (int iteration = 0; iteration < 8; iteration++) {
            roundtrip(data(65536, seed * 101 + iteration));
            directRoundtrip(data(8192, seed * 103 + iteration));
          }
        } catch (Throwable failure) {
          failures.add(failure);
        }
      }, "snappy-java-" + i);
      threads[i].setDaemon(true);
      threads[i].start();
    }

    boolean entered;
    try {
      entered = ready.await(10, TimeUnit.SECONDS);
    } finally {
      start.countDown();
    }

    long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
    boolean stopped = true;
    for (Thread thread : threads) {
      long remaining = deadline - System.nanoTime();
      if (remaining > 0) {
        thread.join(Math.max(1, TimeUnit.NANOSECONDS.toMillis(remaining)));
      }
      if (thread.isAlive()) {
        stopped = false;
        thread.interrupt();
      }
    }

    for (Throwable failure : failures) {
      failure.printStackTrace(System.out);
    }
    check(entered && stopped && failures.isEmpty(),
        "four_java_threads_32_array_and_32_direct_roundtrips");
  }

  public static int run(String workDirectory) throws Exception {
    checks = 0;
    System.out.println("NATIVE_JAR_WORKLOAD library=snappy artifact=1.1.10.12 loader="
        + SnappyWorkload.class.getClassLoader() + " directory=" + workDirectory);

    nativeIdentity();
    for (int size : new int[] {0, 1, 257, 65536, 1048576}) {
      roundtrip(data(size, size + 13));
      check(true, "byte_array_roundtrip_" + size);
    }

    arrayOffsets();
    primitiveArrays();
    directRoundtrip(data(65536, 407));
    check(true, "direct_slice_positions_limits_and_guard_bytes");
    byte[] framed = streams(data(256 * 1024, 503));
    errors(framed);
    concurrent();

    ensureLoaded();
    ensureLoaded();
    probe();
    check(true, "repeat_initialization_and_probe_are_usable");

    System.gc();
    probe();
    check(true, "java_gc_then_native_probe");

    return checks;
  }
}
