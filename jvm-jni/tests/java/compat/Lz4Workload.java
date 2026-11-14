package compat;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.Random;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.Checksum;
import net.jpountz.lz4.LZ4BlockInputStream;
import net.jpountz.lz4.LZ4BlockOutputStream;
import net.jpountz.lz4.LZ4Compressor;
import net.jpountz.lz4.LZ4Exception;
import net.jpountz.lz4.LZ4Factory;
import net.jpountz.lz4.LZ4FrameInputStream;
import net.jpountz.lz4.LZ4FrameOutputStream;
import net.jpountz.xxhash.StreamingXXHash32;
import net.jpountz.xxhash.StreamingXXHash64;
import net.jpountz.xxhash.XXHashFactory;

/** Test client for the unchanged org.lz4:lz4-java:1.8.0 artifact. */
public final class Lz4Workload {
  private static final byte CANARY = (byte) 0xa5;
  private static final int SEED32 = 0x9747b28c;
  private static final long SEED64 = 0x923456789abcdef0L;
  private static int checks;
  private static LZ4Factory nativeLz4;
  private static XXHashFactory nativeHash;

  private Lz4Workload() {}

  private static final class CheckFailure extends AssertionError {
    private static final long serialVersionUID = 1L;

    CheckFailure(String name) {
      super(name);
    }
  }

  private interface Action {
    void run() throws Exception;
  }

  private static void check(boolean pass, String name) {
    checks++;
    System.out.println(
        "NATIVE_JAR_TEST library=lz4 name=" + name + " result=" + (pass ? "PASS" : "FAIL"));
    if (!pass) {
      throw new CheckFailure(name);
    }
  }

  private static void expect(Class<? extends Throwable> type, Action action, String name)
      throws Exception {
    try {
      action.run();
    } catch (Throwable failure) {
      if (!type.isInstance(failure)) {
        check(false, name + "_wrong_exception");
        return;
      }
      check(true, name);
      return;
    }
    check(false, name + "_not_rejected");
  }

  private static boolean jni(Object instance) {
    return instance.getClass().getName().contains("JNI");
  }

  /** No fastestInstance(): native loading failures must not become Java fallbacks. */
  public static synchronized void ensureLoaded() {
    nativeLz4 = LZ4Factory.nativeInstance();
    nativeHash = XXHashFactory.nativeInstance();
    if (!jni(nativeLz4.fastCompressor()) || !jni(nativeLz4.safeDecompressor())
        || !jni(nativeHash.hash32()) || !jni(nativeHash.hash64())) {
      throw new AssertionError("LZ4 native factories selected a non-JNI implementation");
    }
  }

  /** Small repeatable call for the owner's fresh-class-loader lifecycle checks. */
  public static void probe() {
    ensureLoaded();
    byte[] bytes = "lz4-native-loader-probe-01234567890123456789".getBytes(StandardCharsets.UTF_8);
    byte[] compressed = nativeLz4.fastCompressor().compress(bytes);
    byte[] restored = new byte[bytes.length];
    int size = nativeLz4.safeDecompressor().decompress(
        compressed, 0, compressed.length, restored, 0, restored.length);
    if (size != bytes.length || !Arrays.equals(bytes, restored)
        || nativeHash.hash64().hash(bytes, 0, bytes.length, SEED64)
            != XXHashFactory.safeInstance().hash64().hash(bytes, 0, bytes.length, SEED64)) {
      throw new AssertionError("LZ4 native loader probe mismatch");
    }
  }

  private static byte[] data(int size, int seed, boolean random) {
    byte[] bytes = new byte[size];
    if (random) {
      new Random(seed).nextBytes(bytes);
    } else
      for (int i = 0; i < size; i++) {
        bytes[i] = (byte) ((i % 127) ^ ((i / 4096) & 15));
      }
    return bytes;
  }

  private static byte[] guarded(int size) {
    byte[] bytes = new byte[size];
    Arrays.fill(bytes, CANARY);
    return bytes;
  }

  private static boolean canaries(byte[] bytes, int offset, int length) {
    for (int i = 0; i < bytes.length; i++)
      if ((i < offset || i >= offset + length) && bytes[i] != CANARY) {
        return false;
      }
    return true;
  }

  private static void arrayRoundtrip(byte[] bytes, LZ4Compressor compressor, String name) {
    byte[] source = guarded(bytes.length + 23);
    System.arraycopy(bytes, 0, source, 7, bytes.length);
    byte[] original = source.clone();
    int bound = compressor.maxCompressedLength(bytes.length);
    byte[] compressed = guarded(bound + 25);
    int count = compressor.compress(source, 7, bytes.length, compressed, 9, bound);
    check(count > 0 && count <= bound && canaries(compressed, 9, count)
            && Arrays.equals(source, original),
        name + "_compression_and_guards");

    byte[] restored = guarded(bytes.length + 29);
    int size =
        nativeLz4.safeDecompressor().decompress(compressed, 9, count, restored, 11, bytes.length);
    check(size == bytes.length && Arrays.equals(bytes, Arrays.copyOfRange(restored, 11, 11 + size))
            && canaries(restored, 11, size),
        name + "_safe_decompression");

    Arrays.fill(restored, CANARY);
    // Fast JNI requires valid compressed data and the exact original length.
    int consumed =
        nativeLz4.fastDecompressor().decompress(compressed, 9, restored, 11, bytes.length);
    check(consumed == count
            && Arrays.equals(bytes, Arrays.copyOfRange(restored, 11, 11 + bytes.length))
            && canaries(restored, 11, bytes.length),
        name + "_fast_consumed_length");

    size = LZ4Factory.safeInstance().safeDecompressor().decompress(
        compressed, 9, count, restored, 11, bytes.length);
    byte[] javaCompressed = LZ4Factory.safeInstance().fastCompressor().compress(bytes);
    byte[] nativeRestored = nativeLz4.safeDecompressor().decompress(javaCompressed, bytes.length);
    check(size == bytes.length && Arrays.equals(bytes, Arrays.copyOfRange(restored, 11, 11 + size))
            && Arrays.equals(bytes, nativeRestored),
        name + "_java_safe_interoperability");
  }

  private static ByteBuffer buffer(boolean direct, int size) {
    ByteBuffer result = direct ? ByteBuffer.allocateDirect(size) : ByteBuffer.allocate(size);
    for (int i = 0; i < size; i++) {
      result.put(i, CANARY);
    }
    return result;
  }

  private static byte[] bytes(ByteBuffer buffer, int offset, int size) {
    byte[] result = new byte[size];
    ByteBuffer copy = buffer.duplicate();
    copy.position(offset);
    copy.get(result);
    return result;
  }

  private static boolean canaries(ByteBuffer buffer, int offset, int size) {
    for (int i = 0; i < buffer.limit(); i++)
      if ((i < offset || i >= offset + size) && buffer.get(i) != CANARY) {
        return false;
      }
    return true;
  }

  private static void buffers(byte[] input) {
    int bound = nativeLz4.fastCompressor().maxCompressedLength(input.length);

    for (boolean sourceDirect : new boolean[] {false, true}) {
      for (boolean destinationDirect : new boolean[] {false, true}) {
        String name = "buffer_" + (sourceDirect ? "direct" : "heap") + "_to_"
            + (destinationDirect ? "direct" : "heap");
        ByteBuffer sourceParent = buffer(sourceDirect, input.length + 45);
        sourceParent.position(5);
        sourceParent.limit(sourceParent.capacity() - 3);
        ByteBuffer source = sourceParent.slice();
        source.position(7);
        source.put(input);
        source.position(2);
        ByteBuffer compressedParent = buffer(destinationDirect, bound + 43);
        compressedParent.position(3);
        ByteBuffer compressed = compressedParent.slice();
        compressed.position(4);
        int sourceLimit = source.limit(), compressedLimit = compressed.limit();

        int count =
            nativeLz4.fastCompressor().compress(source, 7, input.length, compressed, 9, bound);
        check(count > 0 && source.position() == 2 && source.limit() == sourceLimit
                && compressed.position() == 4 && compressed.limit() == compressedLimit,
            name + "_absolute_positions");
        check(Arrays.equals(input, bytes(source, 7, input.length))
                && canaries(source, 7, input.length) && canaries(compressed, 9, count)
                && sourceParent.get(0) == CANARY && compressedParent.get(0) == CANARY,
            name + "_slices_offsets_guards");

        ByteBuffer restored = buffer(sourceDirect, input.length + 39);
        restored.position(3);
        int restoredLimit = restored.limit();
        int size = nativeLz4.safeDecompressor().decompress(
            compressed, 9, count, restored, 11, input.length);
        check(size == input.length && Arrays.equals(input, bytes(restored, 11, size))
                && canaries(restored, 11, size) && restored.position() == 3
                && restored.limit() == restoredLimit && compressed.position() == 4,
            name + "_safe_bytes_positions");

        int consumed =
            nativeLz4.fastDecompressor().decompress(compressed, 9, restored, 11, input.length);
        check(consumed == count && Arrays.equals(input, bytes(restored, 11, input.length))
                && canaries(restored, 11, input.length),
            name + "_fast_valid_input");
      }
    }

    ByteBuffer source = buffer(true, input.length + 17);
    source.position(7);
    source.put(input).flip();
    source.position(7);
    ByteBuffer compressed = buffer(true, bound + 19);
    compressed.position(9);
    nativeLz4.fastCompressor().compress(source, compressed);
    int count = compressed.position() - 9;
    check(source.position() == source.limit() && count > 0, "buffer_relative_compress_positions");

    compressed.limit(compressed.position());
    compressed.position(9);
    ByteBuffer restored = buffer(true, input.length + 23);
    restored.limit(11 + input.length);
    restored.position(11);
    nativeLz4.fastDecompressor().decompress(compressed, restored);
    check(compressed.position() == compressed.limit() && restored.position() == restored.limit()
            && Arrays.equals(input, bytes(restored, 11, input.length)),
        "buffer_relative_fast_positions");
  }

  private static void hashes(byte[] input) throws Exception {
    XXHashFactory javaHash = XXHashFactory.safeInstance();
    check(nativeHash.hash32().hash(new byte[0], 0, 0, 0) == 0x02cc5d05
            && nativeHash.hash64().hash(new byte[0], 0, 0, 0) == 0xef46db3751d8e999L,
        "xxhash_known_empty_vectors");
    byte[] source = guarded(input.length + 23);
    System.arraycopy(input, 0, source, 7, input.length);
    byte[] original = source.clone();

    for (int seed : new int[] {0, SEED32}) {
      int expected = javaHash.hash32().hash(input, 0, input.length, seed);
      check(nativeHash.hash32().hash(source, 7, input.length, seed) == expected,
          "xxhash32_array_seed_" + Integer.toHexString(seed));
      for (boolean direct : new boolean[] {false, true}) {
        ByteBuffer parent = buffer(direct, source.length + 5);
        parent.position(3);
        ByteBuffer view = parent.slice();
        view.position(7);
        view.put(input);
        view.position(2);
        int limit = view.limit();
        check(nativeHash.hash32().hash(view, 7, input.length, seed) == expected
                && view.position() == 2 && view.limit() == limit,
            "xxhash32_" + (direct ? "direct" : "heap") + "_slice_seed_"
                + Integer.toHexString(seed));
      }
    }

    for (long seed : new long[] {0, SEED64}) {
      long expected = javaHash.hash64().hash(input, 0, input.length, seed);
      check(nativeHash.hash64().hash(source, 7, input.length, seed) == expected,
          "xxhash64_array_seed_" + Long.toHexString(seed));
      for (boolean direct : new boolean[] {false, true}) {
        ByteBuffer view = buffer(direct, source.length);
        view.position(7);
        view.put(input);
        view.position(2);
        int limit = view.limit();
        check(nativeHash.hash64().hash(view, 7, input.length, seed) == expected
                && view.position() == 2 && view.limit() == limit,
            "xxhash64_" + (direct ? "direct" : "heap") + "_seed_" + Long.toHexString(seed));
      }
    }
    check(Arrays.equals(source, original), "xxhash_array_input_unchanged");

    StreamingXXHash32 stream32 = nativeHash.newStreamingHash32(SEED32);
    StreamingXXHash64 stream64 = nativeHash.newStreamingHash64(SEED64);
    try {
      check(jni(stream32) && jni(stream64), "streaming_hash_native_classes");
      for (int offset = 0; offset < input.length; offset += 4093) {
        int size = Math.min(4093, input.length - offset);
        stream32.update(input, offset, size);
        stream64.update(input, offset, size);
      }
      check(stream32.getValue() == javaHash.hash32().hash(input, 0, input.length, SEED32)
              && stream64.getValue() == javaHash.hash64().hash(input, 0, input.length, SEED64),
          "streaming_hash32_64_segmented");
      stream32.reset();
      stream64.reset();
      check(stream32.getValue() == javaHash.hash32().hash(input, 0, 0, SEED32)
              && stream64.getValue() == javaHash.hash64().hash(input, 0, 0, SEED64),
          "streaming_hash_reset_fresh_guest_state");
      Checksum checksum32 = stream32.asChecksum(), checksum64 = stream64.asChecksum();
      checksum32.update(input, 0, input.length);
      checksum64.update(input, 0, input.length);
      // The original 1.8.0 Checksum adapter masks to 28 bits. Compare its
      // unchanged Java counterpart; full 32-bit hashes are checked above.
      check(checksum32.getValue() == javaChecksum32(input, SEED32)
              && checksum64.getValue() == javaHash.hash64().hash(input, 0, input.length, SEED64),
          "streaming_checksum_views_match_upstream_java");
      checksum32.reset();
      checksum64.reset();
      checksum32.update(0x81);
      checksum64.update(0x81);
      byte[] single = {(byte) 0x81};
      check(checksum32.getValue() == javaChecksum32(single, SEED32)
              && checksum64.getValue() == javaHash.hash64().hash(single, 0, 1, SEED64),
          "streaming_checksum_single_byte");
    } finally {
      stream32.close();
      stream64.close();
    }
    stream32.close();
    stream64.close();
    check(true, "streaming_hash_double_close");

    expect(AssertionError.class, () -> stream32.getValue(), "closed_hash32_get_rejected");
    expect(
        AssertionError.class, () -> stream64.update(input, 0, 1), "closed_hash64_update_rejected");
    expect(AssertionError.class, () -> stream32.reset(), "closed_hash32_reset_rejected");
  }

  private static long javaChecksum32(byte[] input, int seed) {
    try (StreamingXXHash32 stream = XXHashFactory.safeInstance().newStreamingHash32(seed)) {
      stream.update(input, 0, input.length);
      return stream.asChecksum().getValue();
    }
  }

  private static void writeChunks(OutputStream out, byte[] input) throws IOException {
    for (int offset = 0; offset < input.length; offset += 4093) {
      out.write(input, offset, Math.min(4093, input.length - offset));
      if (offset == 4093) {
        out.flush();
      }
    }
  }

  private static byte[] readBounded(InputStream in, int expected) throws IOException {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    byte[] buffer = new byte[8191];
    int count;
    while ((count = in.read(buffer)) != -1) {
      if (count == 0 || out.size() + count > expected) {
        throw new IOException("Unexpected stream length");
      }
      out.write(buffer, 0, count);
    }
    return out.toByteArray();
  }

  private static byte[] block(byte[] input) throws IOException {
    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    try (StreamingXXHash32 checksum = nativeHash.newStreamingHash32(SEED32);
        LZ4BlockOutputStream out = new LZ4BlockOutputStream(
            bytes, 65536, nativeLz4.fastCompressor(), checksum.asChecksum(), true)) {
      writeChunks(out, input);
    }
    return bytes.toByteArray();
  }

  private static byte[] readBlock(byte[] compressed, int expected, boolean stopOnEmpty)
      throws IOException {
    try (StreamingXXHash32 checksum = nativeHash.newStreamingHash32(SEED32);
        LZ4BlockInputStream in = new LZ4BlockInputStream(new ByteArrayInputStream(compressed),
            nativeLz4.fastDecompressor(), checksum.asChecksum(), stopOnEmpty)) {
      return readBounded(in, expected);
    }
  }

  private static LZ4FrameOutputStream frameOutput(OutputStream out, int size) throws IOException {
    return new LZ4FrameOutputStream(out, LZ4FrameOutputStream.BLOCKSIZE.SIZE_64KB, size,
        nativeLz4.fastCompressor(), nativeHash.hash32(),
        LZ4FrameOutputStream.FLG.Bits.BLOCK_INDEPENDENCE,
        LZ4FrameOutputStream.FLG.Bits.CONTENT_SIZE, LZ4FrameOutputStream.FLG.Bits.BLOCK_CHECKSUM,
        LZ4FrameOutputStream.FLG.Bits.CONTENT_CHECKSUM);
  }

  private static byte[] readFrame(byte[] compressed, int expected, boolean single)
      throws IOException {
    try (LZ4FrameInputStream in = new LZ4FrameInputStream(new ByteArrayInputStream(compressed),
             nativeLz4.safeDecompressor(), nativeHash.hash32(), single)) {
      return readBounded(in, expected);
    }
  }

  private static byte[] concatenate(byte[] first, byte[] second) {
    byte[] result = Arrays.copyOf(first, first.length + second.length);
    System.arraycopy(second, 0, result, first.length, second.length);
    return result;
  }

  private static void streams(byte[] input, Path directory) throws Exception {
    byte[] compressedBlock = block(input);
    check(Arrays.equals(input, readBlock(compressedBlock, input.length, true)),
        "block_native_checksum_flush_roundtrip");
    check(Arrays.equals(concatenate(input, input),
              readBlock(concatenate(compressedBlock, compressedBlock), input.length * 2, false)),
        "block_concatenated_streams");
    byte[] badBlockMagic = compressedBlock.clone();
    badBlockMagic[0] ^= 1;
    expect(IOException.class,
        () -> readBlock(badBlockMagic, input.length, true), "block_bad_magic_rejected");
    // 1.8.0 block header: magic(8), token(1), two lengths(8), checksum(4).
    // Change the checksum only; never feed malformed data to the fast decoder.
    byte[] badBlockChecksum = compressedBlock.clone();
    badBlockChecksum[17] ^= 1;
    expect(IOException.class,
        () -> readBlock(badBlockChecksum, input.length, true), "block_bad_checksum_rejected");

    Path path = Files.createTempFile(directory, "lz4-workload-", ".frame");
    try {
      try (LZ4FrameOutputStream out = frameOutput(Files.newOutputStream(path), input.length)) {
        writeChunks(out, input);
      }

      byte[] frame = Files.readAllBytes(path);
      check(frame.length > 15, "frame_file_created");
      try (LZ4FrameInputStream in = new LZ4FrameInputStream(Files.newInputStream(path),
               nativeLz4.safeDecompressor(), nativeHash.hash32(), true)) {
        check(in.isExpectedContentSizeDefined() && in.getExpectedContentSize() == input.length
                && Arrays.equals(input, readBounded(in, input.length)),
            "frame_file_native_hash_content_size_roundtrip");
      }
      check(Arrays.equals(concatenate(input, input),
                readFrame(concatenate(frame, frame), input.length * 2, false)),
          "frame_concatenated");
      check(Arrays.equals(input, readFrame(concatenate(frame, frame), input.length, true)),
          "frame_single_frame_mode");

      byte[] badMagic = frame.clone();
      badMagic[0] ^= 1;
      expect(IOException.class,
          () -> readFrame(badMagic, input.length, false), "frame_bad_magic_rejected");
      byte[] badChecksum = frame.clone();
      badChecksum[badChecksum.length - 1] ^= 1;
      expect(IOException.class,
          () -> readFrame(badChecksum, input.length, false), "frame_content_checksum_rejected");
      byte[] truncated = Arrays.copyOf(frame, frame.length - 2);
      expect(IOException.class,
          () -> readFrame(truncated, input.length, false), "frame_truncation_rejected");
    } finally {
      Files.deleteIfExists(path);
    }

    check(!Files.exists(path), "temporary_frame_file_removed");
  }

  private static void badData(byte[] input) throws Exception {
    byte[] compressed = nativeLz4.fastCompressor().compress(input);

    expect(LZ4Exception.class,
        ()
            -> nativeLz4.safeDecompressor().decompress(
                new byte[] {(byte) 0xf0}, 0, 1, new byte[64], 0, 64),
        "native_safe_malformed_block_rejected");
    expect(LZ4Exception.class,
        ()
            -> nativeLz4.safeDecompressor().decompress(
                compressed, 0, compressed.length, new byte[input.length - 1], 0, input.length - 1),
        "native_safe_small_destination_rejected");
    expect(LZ4Exception.class,
        ()
            -> nativeLz4.fastCompressor().compress(input, 0, input.length, new byte[16], 0, 0),
        "native_compress_small_destination_rejected");
    expect(IndexOutOfBoundsException.class,
        ()
            -> nativeLz4.safeDecompressor().decompress(
                compressed, -1, compressed.length, new byte[input.length], 0, input.length),
        "java_guard_invalid_array_offset");

    ByteBuffer source = ByteBuffer.allocateDirect(compressed.length);
    source.put(compressed);
    ByteBuffer destination = ByteBuffer.allocateDirect(input.length).asReadOnlyBuffer();
    expect(ReadOnlyBufferException.class,
        ()
            -> nativeLz4.safeDecompressor().decompress(
                source, 0, compressed.length, destination, 0, input.length),
        "java_guard_readonly_destination");
  }

  private static void concurrent() throws Exception {
    CountDownLatch start = new CountDownLatch(1), done = new CountDownLatch(4);
    AtomicReference<Throwable> failure = new AtomicReference<Throwable>();
    Thread[] workers = new Thread[4];

    for (int i = 0; i < workers.length; i++) {
      final int id = i;
      workers[i] = new Thread(() -> {
        try {
          if (!start.await(30, TimeUnit.SECONDS)) {
            throw new AssertionError("LZ4 concurrency start timeout");
          }
          for (int round = 0; round < 12; round++) {
            byte[] input = data(32771, id * 101 + round, (round & 1) != 0);
            byte[] compressed = nativeLz4.fastCompressor().compress(input);
            byte[] output = nativeLz4.safeDecompressor().decompress(compressed, input.length);
            if (!Arrays.equals(input, output)) {
              throw new AssertionError("Concurrent LZ4 roundtrip");
            }
            try (StreamingXXHash64 hash = nativeHash.newStreamingHash64(id)) {
              hash.update(input, 0, 13);
              hash.update(input, 13, input.length - 13);
              if (hash.getValue() != nativeHash.hash64().hash(input, 0, input.length, id)) {
                throw new AssertionError("Concurrent native hash state");
              }
            }
          }
        } catch (Throwable error) {
          failure.compareAndSet(null, error);
        } finally {
          done.countDown();
        }
      }, "lz4-compat-" + id);
      workers[i].setDaemon(true);
      workers[i].start();
    }

    start.countDown();
    boolean completed = done.await(120, TimeUnit.SECONDS);
    if (!completed) {
      for (Thread worker : workers) {
        worker.interrupt();
      }
    }
    check(completed, "four_threads_complete_within_deadline");
    for (Thread worker : workers) {
      worker.join(1000);
    }

    if (failure.get() != null) {
      failure.get().printStackTrace(System.err);
    }
    check(failure.get() == null, "four_threads_48_roundtrips_and_hash_states");
    if (failure.get() != null) {
      throw new AssertionError(failure.get());
    }
  }

  private static void stateCycles(byte[] input) {
    long expected = nativeHash.hash64().hash(input, 0, input.length, SEED64);

    for (int i = 0; i < 32; i++) {
      try (StreamingXXHash32 hash32 = nativeHash.newStreamingHash32(SEED32);
          StreamingXXHash64 hash64 = nativeHash.newStreamingHash64(SEED64)) {
        hash32.update(input, 0, input.length);
        hash64.update(input, 0, input.length);
        if (hash64.getValue() != expected
            || hash32.getValue() != nativeHash.hash32().hash(input, 0, input.length, SEED32)) {
          throw new AssertionError("Repeated native hash state cycle");
        }
      }
    }

    check(true, "thirty_two_guest_hash_state_create_close_cycles");
  }

  public static int run(String workDirectory) throws Exception {
    checks = 0;

    try {
      ensureLoaded();
      check(jni(nativeLz4.fastCompressor()) && jni(nativeLz4.highCompressor())
              && jni(nativeLz4.fastDecompressor()) && jni(nativeLz4.safeDecompressor())
              && jni(nativeHash.hash32()) && jni(nativeHash.hash64()),
          "all_factories_and_codecs_are_native_jni");

      for (int size : new int[] {0, 1, 15, 16, 255, 256, 65539, 1048576}) {
        for (boolean random : new boolean[] {false, true}) {
          byte[] input = data(size, size + 773, random);
          String suffix = size + "_" + (random ? "random" : "repeated");
          arrayRoundtrip(input, nativeLz4.fastCompressor(), "array_fast_" + suffix);
          arrayRoundtrip(input, nativeLz4.highCompressor(), "array_hc9_" + suffix);
        }
      }

      byte[] input = data(65539, 443, false);
      arrayRoundtrip(input, nativeLz4.highCompressor(1), "array_hc1");
      arrayRoundtrip(input, nativeLz4.highCompressor(17), "array_hc17");
      buffers(input);
      hashes(input);
      Path directory = Paths.get(workDirectory);
      Files.createDirectories(directory);
      streams(data(262147, 901, false), directory);
      badData(input);
      concurrent();
      stateCycles(input);

      System.gc();
      probe();
      check(true, "gc_then_reentrant_probe");

      return checks;
    } catch (Throwable failure) {
      if (!(failure instanceof CheckFailure)) {
        System.out.println("NATIVE_JAR_TEST library=lz4 name=unexpected_failure result=FAIL");
      }
      if (failure instanceof Exception) {
        throw (Exception) failure;
      }
      if (failure instanceof Error) {
        throw (Error) failure;
      }
      throw new AssertionError(failure);
    }
  }
}
