package benchmark;

import dev.glibc.worker.NativeBridge;
import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;

public final class Workload {
  private static WeakReference<ClassLoader> exercise(String directory) throws Exception {
    URLClassLoader loader = new URLClassLoader(
        new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL()},
        Workload.class.getClassLoader()) {
      @Override
      protected synchronized Class<?> loadClass(String name, boolean resolve)
          throws ClassNotFoundException {
        if (!name.equals("benchmark.ChildModule")) {
          return super.loadClass(name, resolve);
        }
        Class<?> type = findLoadedClass(name);
        if (type == null) {
          type = findClass(name);
        }
        if (resolve) {
          resolveClass(type);
        }
        return type;
      }
    };

    // Parent owns NativeBridge metrics, while only this loader owns the JNI module.
    Class<?> module = loader.loadClass("benchmark.ChildModule");
    try {
      module.getMethod("run", String.class).invoke(null, directory);
    } catch (InvocationTargetException failure) {
      throw new AssertionError(failure.getCause());
    }

    loader.close();
    return new WeakReference<ClassLoader>(loader);
  }

  public static int run(String directory) throws Exception {
    File payload = new File(System.getProperty("java.home"), "payload-benchmark-mode");
    if (payload.isFile()) {
      String[] selection = new String(java.nio.file.Files.readAllBytes(payload.toPath()),
          java.nio.charset.StandardCharsets.UTF_8)
                               .trim()
                               .split(" ");
      if (selection.length != 2) {
        throw new AssertionError("Invalid payload benchmark selection");
      }
      return PayloadWorkload.run(directory, selection[0], Integer.parseInt(selection[1]));
    }

    if (!Boolean.getBoolean("gw.benchmark.enabled")
        && !new File(System.getProperty("java.home"), "benchmark-enabled").isFile()) {
      return 0;
    }

    WeakReference<ClassLoader> owner = exercise(directory);
    long deadline = System.nanoTime() + 10_000_000_000L;
    while ((owner.get() != null || NativeBridge.bridgeMetric(9) != 0)
        && System.nanoTime() < deadline) {
      System.gc();
      Thread.sleep(20);
    }

    if (owner.get() != null || NativeBridge.bridgeMetric(9) != 0) {
      throw new AssertionError("Benchmark module failed to unload");
    }

    System.out.println("BRIDGE_BENCH_SUMMARY samples=18 module_unloaded=PASS");
    return 0;
  }
}
