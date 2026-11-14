package compat;

import dev.glibc.worker.GuestImages;
import dev.glibc.worker.NativeBridge;
import java.io.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Optional coverage extension. Each selected library runs in a disposable probe process. */
public final class NativeJarSuite {
  private static int checks;
  private static String library;

  private static int metric(int n) {
    return NativeBridge.bridgeMetric(n);
  }

  private static void check(boolean value, String name) {
    checks++;
    System.out.println("NATIVE_JAR_TEST library=" + library + " name=" + name
        + " result=" + (value ? "PASS" : "FAIL"));
    if (!value) {
      throw new AssertionError(library + ": " + name);
    }
  }

  private static Object call(
      Class<?> type, String method, Class<?>[] signature, Object... arguments) throws Exception {
    try {
      return type.getMethod(method, signature).invoke(null, arguments);
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

  private static WeakReference<ClassLoader> exercise(File home, Properties profile, int cycle)
      throws Exception {
    String prefix = library + ".";
    URLClassLoader loader = new URLClassLoader(
        new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL(),
            new File(home, profile.getProperty(prefix + "jar")).toURI().toURL()},
        null);
    String workloadName = profile.getProperty(prefix + "workload");
    Class<?> workload = loader.loadClass("compat." + workloadName);
    int loads = metric(1), opens = metric(15);

    if (cycle == 0) {
      File work = new File(home, "native-jars-work/" + library);
      Files.createDirectories(work.toPath());
      checks += (Integer) call(workload, "run", new Class<?>[] {String.class}, work.getPath());
    } else {
      call(workload, "probe", new Class<?>[] {});
      check(true, "fresh_classloader_native_reload");
    }

    check(metric(1) == loads + 1 && metric(15) == opens + 1 && metric(9) == 1,
        "one_original_guest_image_cycle_" + cycle);
    call(workload, "ensureLoaded", new Class<?>[] {});
    call(workload, "ensureLoaded", new Class<?>[] {});
    check(metric(1) == loads + 1, "original_loader_idempotent_cycle_" + cycle);

    String maps =
        new String(Files.readAllBytes(Paths.get("/proc/self/maps")), StandardCharsets.UTF_8);
    check(maps.contains("libguest_libc.so")
            && maps.contains(profile.getProperty(prefix + "installed")),
        "original_gnu_image_mapped_cycle_" + cycle);

    loader.close();
    return new WeakReference<ClassLoader>(loader);
  }

  public static int runIfEnabled(String directory) throws Exception {
    File home = new File(System.getProperty("java.home"));
    File selection = new File(home, "native-jar-selected");

    if (!selection.isFile()) {
      return 0;
    }

    library = new String(Files.readAllBytes(selection.toPath()), StandardCharsets.UTF_8).trim();
    if (!Arrays.asList("lz4", "snappy", "rocksdb").contains(library)) {
      throw new IllegalArgumentException("Unknown native JAR: " + library);
    }

    Properties profile = new Properties();
    try (InputStream in = new FileInputStream(new File(home, "native-jars.properties"))) {
      profile.load(in);
    }

    String prefix = library + ".";
    System.out.println("NATIVE_JAR_BEGIN library=" + library
        + " vm=" + System.getProperty("java.vm.name") + " os=" + System.getProperty("os.name")
        + " arch=" + System.getProperty("os.arch"));

    check(profile.getProperty(prefix + "jar.sha256")
              .equals(GuestImages.sha256(new File(home, profile.getProperty(prefix + "jar")))),
        "original_maven_jar_sha256");

    int loads = metric(1), unloads = metric(2), opens = metric(15), closes = metric(16);
    for (int cycle = 0; cycle < 2; cycle++) {
      WeakReference<ClassLoader> weak = exercise(home, profile, cycle);
      long deadline = System.nanoTime() + 8_000_000_000L;
      while ((weak.get() != null || metric(9) != 0) && System.nanoTime() < deadline) {
        System.gc();
        Thread.sleep(20);
      }
      String maps =
          new String(Files.readAllBytes(Paths.get("/proc/self/maps")), StandardCharsets.UTF_8);
      boolean retained = maps.contains(profile.getProperty(prefix + "installed"));
      System.out.println("NATIVE_JAR_LIFECYCLE library=" + library + " cycle=" + cycle
          + " loader_collected=" + (weak.get() == null) + " active=" + metric(9)
          + " globals=" + metric(7) + " weak=" + metric(8) + " closures=" + metric(5)
          + " mapping=" + (retained ? "retained" : "reclaimed"));
      check(weak.get() == null && metric(9) == 0 && metric(2) == unloads + cycle + 1,
          "classloader_logical_unload_cycle_" + cycle);
      check(metric(5) == 0 && metric(7) == 0 && metric(8) == 0,
          "bridge_references_reclaimed_cycle_" + cycle);
      // GNU UNIQUE symbols can legitimately make a DSO NODELETE. Physical
      // unmapping is separate from logical ownership and isn't promised here.
      if (library.equals("lz4")) {
        check(!retained, "lz4_physical_mapping_reclaimed_cycle_" + cycle);
      }
    }

    check(metric(1) == loads + 2 && metric(2) == unloads + 2 && metric(15) == opens + 2
            && metric(16) == closes + 2,
        "two_balanced_original_jar_load_cycles");

    System.out.println("NATIVE_JAR_SUMMARY library=" + library + " checks=" + checks
        + " failures=0 cycles=2 jar_unmodified=true elf_unmodified=true result=PASS");
    return checks;
  }
}
