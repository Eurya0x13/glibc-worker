package benchmark;

import dev.glibc.worker.NativeBridge;
import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;

public final class PayloadWorkload {
  private static int samples;

  private static WeakReference<ClassLoader> exercise(String directory, String mode, int round)
      throws Exception {
    URLClassLoader loader = new URLClassLoader(
        new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL()},
        PayloadWorkload.class.getClassLoader()) {
      @Override
      protected synchronized Class<?> loadClass(String name, boolean resolve)
          throws ClassNotFoundException {
        if (!name.equals("benchmark.PayloadModule")) {
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

    try {
      samples = (Integer) loader.loadClass("benchmark.PayloadModule")
                    .getMethod("run", String.class, String.class, int.class)
                    .invoke(null, directory, mode, round);
    } catch (InvocationTargetException failure) {
      throw new AssertionError(failure.getCause());
    } finally {
      loader.close();
    }

    return new WeakReference<ClassLoader>(loader);
  }

  public static int run(String directory, String mode, int round) throws Exception {
    if (!mode.equals("host") && !mode.equals("guest")) {
      throw new AssertionError("Unknown payload mode");
    }

    WeakReference<ClassLoader> owner = exercise(directory, mode, round);
    long deadline = System.nanoTime() + 10_000_000_000L;
    while ((owner.get() != null || NativeBridge.bridgeMetric(9) != 0)
        && System.nanoTime() < deadline) {
      System.gc();
      Thread.sleep(20);
    }

    if (owner.get() != null || NativeBridge.bridgeMetric(9) != 0) {
      throw new AssertionError("Payload module failed to unload");
    }
    System.out.println("PAYLOAD_BENCH_SUMMARY mode=" + mode + " round=" + round
        + " samples=" + samples + " module_unloaded=PASS");
    return 0;
  }
}
