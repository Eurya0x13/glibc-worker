package compat;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import org.rocksdb.BlockBasedTableConfig;
import org.rocksdb.BloomFilter;
import org.rocksdb.ColumnFamilyDescriptor;
import org.rocksdb.ColumnFamilyHandle;
import org.rocksdb.ColumnFamilyOptions;
import org.rocksdb.CompressionType;
import org.rocksdb.DBOptions;
import org.rocksdb.FlushOptions;
import org.rocksdb.HyperClockCache;
import org.rocksdb.Options;
import org.rocksdb.ReadOptions;
import org.rocksdb.RocksDB;
import org.rocksdb.RocksIterator;
import org.rocksdb.WriteBatch;
import org.rocksdb.WriteOptions;

/** Original RocksDB JNI client; does not replace upstream Java classes or native bytes. */
public final class RocksWorkload {
  private static final int RECORDS = 48;
  private static final int THREADS = 4;
  private static final int THREAD_RECORDS = 32;

  private RocksWorkload() {}

  private static final class Checks {
    int count;

    void check(boolean pass, String name) {
      count++;
      System.out.println(
          "NATIVE_JAR_TEST library=rocksdb name=" + name + " result=" + (pass ? "PASS" : "FAIL"));
      if (!pass) {
        throw new AssertionError(name);
      }
    }
  }

  public static void ensureLoaded() {
    RocksDB.loadLibrary();
  }

  /** Harmless native entry used after loading through a fresh class loader. */
  public static void probe() {
    ensureLoaded();
    if (RocksDB.rocksdbVersion().getMajor() <= 0) {
      throw new AssertionError("native version probe");
    }
  }

  private static byte[] bytes(String text) {
    return text.getBytes(StandardCharsets.UTF_8);
  }

  private static byte[] key(int index) {
    return ByteBuffer.allocate(Long.BYTES).putLong(index).array();
  }

  private static byte[] value(int index) {
    byte[] result = new byte[4096 + index % 31];
    for (int i = 0; i < result.length; i++) {
      result[i] = (byte) (index * 17 + i % 101);
    }
    return result;
  }

  /** Nonzero offsets exercise the original JNI direct-buffer argument handling. */
  private static ByteBuffer direct(byte[] data, int prefix) {
    ByteBuffer buffer = ByteBuffer.allocateDirect(prefix + data.length + 7);
    buffer.position(prefix);
    buffer.put(data);
    buffer.limit(prefix + data.length);
    buffer.position(prefix);
    return buffer;
  }

  private static byte[] remaining(ByteBuffer buffer) {
    byte[] result = new byte[buffer.remaining()];
    buffer.duplicate().get(result);
    return result;
  }

  private static byte atCapacity(ByteBuffer buffer, int index) {
    ByteBuffer view = buffer.duplicate();
    view.clear();
    return view.get(index);
  }

  /** Own the options/cache until after the database and all column-family handles close. */
  private static final class Database implements AutoCloseable {
    private final HyperClockCache cache = new HyperClockCache(8L << 20, 0, -1, false);
    private final BloomFilter bloom = new BloomFilter(10);
    private final ColumnFamilyOptions defaults = new ColumnFamilyOptions();
    private final ColumnFamilyOptions sections = new ColumnFamilyOptions();
    private final ColumnFamilyOptions ids = new ColumnFamilyOptions();
    private final DBOptions options = new DBOptions();
    final List<ColumnFamilyHandle> handles = new ArrayList<ColumnFamilyHandle>();
    RocksDB db;

    Database(Path directory) throws Exception {
      try {
        defaults.optimizeForSmallDb().setCompressionType(CompressionType.ZSTD_COMPRESSION);
        ids.optimizeForSmallDb().setCompressionType(CompressionType.ZSTD_COMPRESSION);
        sections.setCompressionType(CompressionType.NO_COMPRESSION)
            .setLevelCompactionDynamicLevelBytes(true)
            .setWriteBufferSize(1L << 20)
            .setTableFormatConfig(
                new BlockBasedTableConfig().setBlockCache(cache).setFilterPolicy(bloom));

        options.setIncreaseParallelism(2)
            .setMaxBackgroundJobs(2)
            .setCreateIfMissing(true)
            .setCreateMissingColumnFamilies(true);

        List<ColumnFamilyDescriptor> descriptors =
            Arrays.asList(new ColumnFamilyDescriptor(RocksDB.DEFAULT_COLUMN_FAMILY, defaults),
                new ColumnFamilyDescriptor(bytes("world_sections"), sections),
                new ColumnFamilyDescriptor(bytes("id_mappings"), ids));
        db = RocksDB.open(options, directory.toString(), descriptors, handles);
      } catch (Exception | Error failure) {
        try {
          close();
        } catch (Exception cleanup) {
          failure.addSuppressed(cleanup);
        }
        throw failure;
      }
    }

    @Override
    public void close() throws Exception {
      try {
        // closeE owns these handles too; explicit close also verifies idempotent close.
        for (ColumnFamilyHandle handle : handles) {
          handle.close();
        }
        handles.clear();

        if (db != null) {
          db.closeE();
          db = null;
        }
      } finally {
        options.close();
        defaults.close();
        sections.close();
        ids.close();
        bloom.close();
        cache.close();
      }
    }
  }

  private static void writeAndRead(Database store, Checks checks) throws Exception {
    RocksDB db = store.db;
    ColumnFamilyHandle sections = store.handles.get(1);
    ColumnFamilyHandle ids = store.handles.get(2);
    db.put(store.handles.get(0), bytes("format"), bytes("voxy-style-compat-v1"));
    db.put(ids, bytes("id/stone"), bytes("minecraft:stone"));
    checks.check(Arrays.equals(bytes("minecraft:stone"), db.get(ids, bytes("id/stone"))),
        "array_column_family_roundtrip");
    checks.check(db.get(ids, bytes("absent")) == null, "array_missing_key");

    try (WriteOptions write = new WriteOptions(); ReadOptions read = new ReadOptions()) {
      boolean consumed = true;
      boolean valid = true;
      for (int i = 0; i < RECORDS; i++) {
        ByteBuffer inputKey = direct(key(i), 3);
        ByteBuffer inputValue = direct(value(i), 11);
        db.put(sections, write, inputKey, inputValue);
        consumed &= !inputKey.hasRemaining() && !inputValue.hasRemaining();
        ByteBuffer output = ByteBuffer.allocateDirect(value(i).length + 17);
        output.position(5);
        int length = db.get(sections, read, direct(key(i), 7), output);
        valid &= length == value(i).length && Arrays.equals(value(i), remaining(output))
            && output.get(4) == 0 && atCapacity(output, 5 + length) == 0;
      }
      checks.check(consumed, "direct_put_consumes_selected_ranges");
      checks.check(valid, "direct_buffer_offsets_and_bytes");

      ByteBuffer tiny = ByteBuffer.allocateDirect(23);
      tiny.position(3);
      int actual = db.get(sections, read, direct(key(0), 1), tiny);
      checks.check(
          actual == value(0).length && Arrays.equals(Arrays.copyOf(value(0), 20), remaining(tiny)),
          "direct_partial_get_reports_full_size");
      ByteBuffer missing = ByteBuffer.allocateDirect(16);
      missing.position(4);
      int missingSize = db.get(sections, read, direct(key(-1), 1), missing);
      checks.check(
          missingSize == RocksDB.NOT_FOUND && missing.position() == 4 && missing.limit() == 16,
          "direct_missing_key_preserves_value_range");
    }

    try (WriteBatch batch = new WriteBatch(); WriteOptions write = new WriteOptions()) {
      batch.put(ids, bytes("id/dirt"), bytes("minecraft:dirt"));
      batch.put(ids, bytes("temporary"), bytes("remove"));
      batch.delete(ids, bytes("temporary"));
      db.write(write, batch);
    }
    checks.check(Arrays.equals(bytes("minecraft:dirt"), db.get(ids, bytes("id/dirt")))
            && db.get(ids, bytes("temporary")) == null,
        "write_batch_put_and_delete");

    try (RocksIterator iterator = db.newIterator(sections)) {
      iterator.seek(direct(key(0), 5));
      boolean valid = true;
      int count = 0;
      for (; iterator.isValid() && count <= RECORDS; iterator.next()) {
        ByteBuffer actualKey = ByteBuffer.allocateDirect(16);
        actualKey.position(2);
        ByteBuffer actualValue = ByteBuffer.allocateDirect(4200);
        actualValue.position(3);
        valid &= iterator.key(actualKey) == Long.BYTES
            && iterator.value(actualValue) == value(count).length
            && Arrays.equals(key(count), remaining(actualKey))
            && Arrays.equals(value(count), remaining(actualValue));
        count++;
      }
      iterator.status();
      checks.check(valid && count == RECORDS, "iterator_direct_seek_key_value_order");
    }
  }

  private static void concurrent(final Database store, Checks checks) throws Exception {
    ExecutorService executor = Executors.newFixedThreadPool(THREADS);
    List<Future<Boolean>> futures = new ArrayList<Future<Boolean>>();

    try {
      for (int t = 0; t < THREADS; t++) {
        final int worker = t;
        futures.add(executor.submit(new Callable<Boolean>() {
          @Override
          public Boolean call() throws Exception {
            try (WriteOptions write = new WriteOptions(); ReadOptions read = new ReadOptions()) {
              for (int i = 0; i < THREAD_RECORDS; i++) {
                if (Thread.currentThread().isInterrupted()) {
                  throw new InterruptedException("RocksDB worker cancelled");
                }
                byte[] k = key(1000 + worker * THREAD_RECORDS + i);
                byte[] v = value(worker * THREAD_RECORDS + i);
                store.db.put(store.handles.get(1), write, k, v);
                if (!Arrays.equals(v, store.db.get(store.handles.get(1), read, k))) {
                  return false;
                }
              }
              return true;
            }
          }
        }));
      }

      executor.shutdown();
      boolean completed = executor.awaitTermination(30, TimeUnit.SECONDS);
      checks.check(completed, "bounded_concurrent_threads_finish");

      boolean valid = true;
      for (Future<Boolean> future : futures) {
        valid &= future.get().booleanValue();
      }
      checks.check(valid, "concurrent_array_column_family_roundtrip");
    } finally {
      executor.shutdownNow();
      if (!executor.awaitTermination(5, TimeUnit.SECONDS)) {
        throw new IllegalStateException("RocksDB worker still running; terminate test process");
      }
    }
  }

  private static void verifyPersisted(Database store, Checks checks) throws Exception {
    checks.check(Arrays.equals(bytes("voxy-style-compat-v1"),
                     store.db.get(store.handles.get(0), bytes("format"))),
        "reopen_default_column_family");
    checks.check(Arrays.equals(bytes("minecraft:stone"),
                     store.db.get(store.handles.get(2), bytes("id/stone")))
            && Arrays.equals(
                bytes("minecraft:dirt"), store.db.get(store.handles.get(2), bytes("id/dirt")))
            && store.db.get(store.handles.get(2), bytes("temporary")) == null,
        "reopen_id_mappings_and_delete");

    boolean valid = true;
    for (int i = 0; i < RECORDS; i++) {
      valid &= Arrays.equals(value(i), store.db.get(store.handles.get(1), key(i)));
    }
    checks.check(valid, "reopen_world_section_bytes");

    valid = true;
    for (int t = 0; t < THREADS; t++) {
      for (int i = 0; i < THREAD_RECORDS; i++) {
        valid &= Arrays.equals(value(t * THREAD_RECORDS + i),
            store.db.get(store.handles.get(1), key(1000 + t * THREAD_RECORDS + i)));
      }
    }
    checks.check(valid, "reopen_concurrent_record_bytes");
  }

  public static int run(String workDirectory) throws Exception {
    ensureLoaded();
    Checks checks = new Checks();
    checks.check("10.2.1".equals(RocksDB.rocksdbVersion().toString()), "upstream_native_version");

    Path root = Paths.get(workDirectory);
    Files.createDirectories(root);
    // Retain the DB as evidence; the runner owns cleanup of its work directory.
    Path directory = Files.createTempDirectory(root, "rocksdb-");

    try (Database store = new Database(directory)) {
      checks.check(store.handles.size() == 3
              && Arrays.equals(bytes("world_sections"), store.handles.get(1).getName())
              && Arrays.equals(bytes("id_mappings"), store.handles.get(2).getName()),
          "open_three_voxy_style_column_families");

      writeAndRead(store, checks);
      concurrent(store, checks);
      store.db.flushWal(true);
      checks.check(true, "flush_wal_sync_returns");
      try (FlushOptions flush = new FlushOptions().setWaitForFlush(true)) {
        store.db.flush(flush, store.handles);
      }
      checks.check(true, "flush_all_column_families_returns");
      store.db.compactRange(store.handles.get(1));
      checks.check(true, "compact_world_sections_returns");
    }
    checks.check(true, "close_database_resources");

    try (Database store = new Database(directory)) {
      verifyPersisted(store, checks);
    }
    checks.check(true, "close_reopened_database_resources");

    try (Options options = new Options()) {
      List<byte[]> names = RocksDB.listColumnFamilies(options, directory.toString());
      checks.check(names.size() == 3, "list_persisted_column_families");
    }
    return checks.count;
  }
}
