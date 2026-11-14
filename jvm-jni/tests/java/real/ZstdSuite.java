package real;

import dev.glibc.worker.GuestImages;
import dev.glibc.worker.NativeBridge;
import java.io.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

public final class ZstdSuite {
  private static final String VERSION = "1.5.7-6";
  private static final Properties TARGET = targetProfile();
  private static final String ELF = TARGET.getProperty("zstd.sha256");

  private static Properties targetProfile() {
    Properties p = new Properties();
    try (InputStream in = new FileInputStream(
             new File(System.getProperty("java.home"), "test-target.properties"))) {
      p.load(in);
      return p;
    } catch (IOException e) {
      throw new ExceptionInInitializerError(e);
    }
  }

  private static int checks;

  private static int metric(int n) {
    return NativeBridge.bridgeMetric(n);
  }

  private static void check(boolean value, String name) {
    checks++;
    System.out.println("ZSTD_TEST " + name + "=" + (value ? "PASS" : "FAIL"));
    if (!value) {
      throw new AssertionError(name);
    }
  }

  private static URLClassLoader loader() throws Exception {
    return new URLClassLoader(
        new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL(),
            new File(System.getProperty("java.home"), "zstd-jni-" + VERSION + ".jar")
                .toURI()
                .toURL()},
        null);
  }

  private static Object call(Class<?> type, String name, Class<?>[] signature, Object... args)
      throws Exception {
    try {
      return type.getMethod(name, signature).invoke(null, args);
    } catch (InvocationTargetException e) {
      Throwable cause = e.getCause();
      if (cause instanceof Error) {
        throw (Error) cause;
      }
      if (cause instanceof Exception) {
        throw (Exception) cause;
      }
      throw e;
    }
  }

  private static String maps() throws Exception {
    return new String(Files.readAllBytes(Paths.get("/proc/self/maps")), "UTF-8");
  }

  private static File copyImage(String name, boolean corrupt) throws Exception {
    File home = new File(System.getProperty("java.home")), copy = new File(home, name);
    try (ZipFile jar = new ZipFile(new File(home, "zstd-jni-" + VERSION + ".jar"));
        InputStream in = jar.getInputStream(jar.getEntry(TARGET.getProperty("zstd.resource")))) {
      Files.copy(in, copy.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    if (corrupt) {
      try (RandomAccessFile file = new RandomAccessFile(copy, "rw")) {
        long offset = file.length() - 1;
        file.seek(offset);
        int last = file.read();
        file.seek(offset);
        file.write(last ^ 1);
      }
    }
    return copy;
  }

  private static WeakReference<ClassLoader> exercise(String directory, int cycle) throws Exception {
    URLClassLoader loader = loader();
    Class<?> workload = loader.loadClass("real.ZstdWorkload");
    int loads = metric(1);

    if (cycle == 0) {
      String guestName =
          new String(Files.readAllBytes(Paths.get("/proc/self/task/" + metric(17) + "/comm")),
              java.nio.charset.StandardCharsets.UTF_8)
              .trim();
      int count = (Integer) call(workload, "run", new Class<?>[] {String.class}, guestName);
      checks += count;
    } else {
      check((Long) call(workload, "bound", new Class<?>[] {}) > 1024,
          "fresh_loader_can_reload_original_jar");
    }

    long handle = NativeBridge.bridgeLastHandle();
    check(metric(1) == loads + 1 && metric(9) == 1 && NativeBridge.bridgeHandleState(handle) == 2,
        "one_guest_image_cycle_" + cycle);
    call(workload, "ensureLoaded", new Class<?>[] {});
    call(workload, "ensureLoaded", new Class<?>[] {});
    check(metric(1) == loads + 1, "original_loader_repeat_is_idempotent_" + cycle);
    String mappings = maps();
    check(mappings.contains("libguest_libc.so") && mappings.contains("libinstalled_zstd.so"),
        "installed_image_and_glibc_mapped_" + cycle);

    if (cycle == 0) {
      File good = copyImage("zstd-copy.so", false), bad = copyImage("zstd-corrupt.so", true);
      try {
        check(ELF.equals(GuestImages.sha256(good)), "extracted_resource_bytes_unchanged");

        int opens = metric(15), closures = metric(5);
        boolean alias = false, corrupt = false;
        try {
          call(workload, "loadImage", new Class<?>[] {String.class}, good.getPath());
        } catch (UnsatisfiedLinkError expected) {
          alias = expected.getMessage().contains("already owned");
          System.out.println("ZSTD_EXPECTED_ALIAS_REJECTION " + expected);
        }
        check(alias && metric(15) == opens && metric(5) == closures && metric(9) == 1,
            "alternate_path_cannot_coalesce_live_image");

        try {
          call(workload, "loadImage", new Class<?>[] {String.class}, bad.getPath());
        } catch (UnsatisfiedLinkError expected) {
          corrupt = expected.getMessage().contains("No installed Guest image matches");
          System.out.println("ZSTD_EXPECTED_CONTENT_REJECTION " + expected);
        }
        check(corrupt && metric(15) == opens && metric(5) == closures,
            "content_mismatch_rejected_before_guest_dlopen");

        URLClassLoader other = loader();
        try {
          boolean rejected = false;
          try {
            call(other.loadClass("real.ZstdWorkload"), "bound", new Class<?>[] {});
          } catch (UnsatisfiedLinkError expected) {
            rejected = expected.getMessage().contains("already owned");
          }
          check(rejected && metric(15) == opens && metric(9) == 1,
              "second_loader_cannot_share_live_installed_image");
        } finally {
          other.close();
        }
        check((Long) call(workload, "bound", new Class<?>[] {}) > 1024,
            "rejections_preserve_original_library");
      } finally {
        Files.deleteIfExists(good.toPath());
        Files.deleteIfExists(bad.toPath());
      }
    }

    loader.close();
    return new WeakReference<ClassLoader>(loader);
  }

  public static int run(String directory) throws Exception {
    File jar = new File(System.getProperty("java.home"), "zstd-jni-" + VERSION + ".jar");
    check("8d6feb1da335f3ab13c584c613e23c7b3c61b392e37956872057baf8f0ca1d6f".equals(
              GuestImages.sha256(jar)),
        "original_maven_jar_sha256");
    check(!new File(directory, "libzstd-jni-" + VERSION + ".so").exists(),
        "standard_library_name_absent_for_extraction_path");

    int loads = metric(1), unloads = metric(2), opens = metric(15), closes = metric(16);
    for (int i = 0; i < 2; i++) {
      WeakReference<ClassLoader> weak = exercise(directory, i);
      long deadline = System.nanoTime() + 8_000_000_000L;
      while ((weak.get() != null || metric(9) != 0) && System.nanoTime() < deadline) {
        System.gc();
        Thread.sleep(20);
      }
      check(weak.get() == null && metric(9) == 0 && metric(2) == unloads + i + 1,
          "original_jar_loader_gc_unload_" + i);
      String remaining = maps();
      check(metric(5) == 0 && metric(7) == 0 && metric(8) == 0
              && !remaining.contains("libinstalled_zstd.so")
              && !remaining.contains("/libzstd-jni-" + VERSION),
          "closures_refs_and_mapping_reclaimed_" + i);
    }

    check(metric(1) == loads + 2 && metric(2) == unloads + 2 && metric(15) == opens + 2
            && metric(16) == closes + 2,
        "two_real_guest_dlopen_dlclose_cycles");

    System.out.println("ZSTD_TEST_SUMMARY checks=" + checks
        + " failures=0 cycles=2 jar_unmodified=true elf_unmodified=true");
    return checks;
  }
}
