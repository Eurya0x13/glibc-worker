import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;

/** Real VM load/find/unload hooks over the production dual-libc bridge. */
public final class SystemLoadProbe {
  private static native int[] stats();
  private static native long[] metrics();
  private static int checks;

  private static void check(boolean value, String name) {
    if (!value) {
      throw new AssertionError(name);
    }
    ++checks;
  }

  private static URLClassLoader loader(String jar, String image) throws Exception {
    return new URLClassLoader(
        new URL[] {new File(jar).toURI().toURL()}, SystemLoadProbe.class.getClassLoader()) {
      @Override
      protected String findLibrary(String name) {
        return name.equals("domain_fixture") ? image : null;
      }
    };
  }

  private static Object call(Class<?> cls, String name, Class<?>[] types, Object... args)
      throws Exception {
    try {
      return cls.getMethod(name, types).invoke(null, args);
    } catch (InvocationTargetException error) {
      Throwable cause = error.getCause();
      if (cause instanceof Error) {
        throw (Error) cause;
      }
      if (cause instanceof Exception) {
        throw (Exception) cause;
      }
      throw error;
    }
  }

  private static WeakReference<ClassLoader> cycle(String jar, String image, boolean byName)
      throws Exception {
    URLClassLoader owner = loader(jar, image);
    Class<?> fixture = owner.loadClass("SystemLoadFixture");
    String method = byName ? "loadLibrary" : "load";
    String argument = byName ? "domain_fixture" : image;
    int before = stats()[0];
    call(fixture, method, new Class<?>[] {String.class}, argument);
    call(fixture, method, new Class<?>[] {String.class}, argument);
    check(stats()[0] == before + 1, "duplicate load is idempotent");
    check((Long) call(fixture, "echo", new Class<?>[] {long.class}, 35L) == 42, "short name");
    check((Integer) call(fixture, "overload", new Class<?>[] {int.class}, 2) == 13, "int overload");
    check((Double) call(fixture, "overload", new Class<?>[] {double.class}, 1.0) == 1.5,
        "double overload");
    boolean missing = false;
    try {
      call(fixture, "missing", new Class<?>[] {long.class}, 1L);
    } catch (UnsatisfiedLinkError expected) {
      missing = true;
    }
    check(missing, "missing export");
    try (URLClassLoader other = loader(jar, image)) {
      Class<?> duplicate = other.loadClass("SystemLoadFixture");
      boolean rejected = false;
      try {
        call(duplicate, "load", new Class<?>[] {String.class}, image);
      } catch (UnsatisfiedLinkError expected) {
        rejected = true;
      }
      check(rejected, "other ClassLoader cannot own same image");
    }
    if (System.getenv("GW_BOOTSTRAP_SIGNAL_PRESSURE") != null) {
      SignalPressure.run(fixture);
    }
    WeakReference<ClassLoader> reference = new WeakReference<>(owner);
    owner.close();
    return reference;
  }

  private static void awaitUnload(WeakReference<ClassLoader> reference, int count)
      throws Exception {
    for (int i = 0; i < 400; ++i) {
      System.gc();
      if (reference.get() == null && stats()[1] == count && stats()[2] == 0) {
        check(stats()[6] == 0, "closures reclaimed");
        return;
      }
      Thread.sleep(25);
    }
    throw new AssertionError("automatic ClassLoader unload: " + java.util.Arrays.toString(stats()));
  }

  private static WeakReference<ClassLoader> benchmark(String jar, String image) throws Exception {
    try (URLClassLoader owner = loader(jar, image)) {
      Class<?> fixture = owner.loadClass("SystemLoadFixture");
      call(fixture, "load", new Class<?>[] {String.class}, image);
      Class<?> bench = owner.loadClass("SystemLoadBenchmark");
      java.util.function.Supplier<long[]> counters = SystemLoadProbe::metrics;
      call(bench, "run", new Class<?>[] {int.class, java.util.function.Supplier.class},
          Integer.parseInt(System.getenv("GW_THREADS")), counters);
      return new WeakReference<>(owner);
    }
  }

  public static int run(String jar, String image, String rejectedImage, String ordinary)
      throws Exception {
    if ("1".equals(System.getenv("GW_BENCH"))) {
      boolean direct = "1".equals(System.getenv("GW_BENCH_DIRECT"));
      WeakReference<ClassLoader> ref = benchmark(jar, direct ? ordinary : image);
      awaitUnload(ref, direct ? 0 : 1);
      int[] state = stats();
      check(state[0] == (direct ? 0 : 1) && state[2] == 0 && state[6] == 0,
          "benchmark used requested loader and reclaimed resources");
      System.out.println("SYSTEM_BENCH_RESULT {\"result\":\"PASS\",\"direct\":" + direct
          + ",\"loads\":" + state[0] + ",\"unloads\":" + state[1] + "}");
      return 1;
    }
    try (URLClassLoader rejected = loader(jar, rejectedImage)) {
      Class<?> fixture = rejected.loadClass("SystemLoadFixture");
      boolean failed = false;
      try {
        call(fixture, "load", new Class<?>[] {String.class}, rejectedImage);
      } catch (UnsatisfiedLinkError expected) {
        failed = true;
      }
      check(failed && stats()[2] == 0 && stats()[5] == 1, "OnLoad rejection rollback");
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
      awaitUnload(cycle(jar, image, cycle == 1), cycle + 1);
    }
    if (!ordinary.isEmpty()) {
      int before = stats()[0];
      try (URLClassLoader host = loader(jar, ordinary)) {
        Class<?> fixture = host.loadClass("SystemLoadFixture");
        call(fixture, "load", new Class<?>[] {String.class}, ordinary);
        check((Long) call(fixture, "echo", new Class<?>[] {long.class}, 35L) == 42,
            "ordinary bionic native call");
        check(stats()[0] == before, "bionic bypasses Guest loader");
      }
    }
    int[] result = stats();
    check(result[1] == 3 && result[2] == 0 && result[3] == result[4], "balanced image lifecycle");
    System.out.println("SYSTEM_LOAD_RESULT {\"result\":\"PASS\",\"cycles\":3,\"checks\":" + checks
        + ",\"unloads\":" + result[1] + ",\"active_modules\":" + result[2] + ",\"live_closures\":"
        + result[6] + ",\"bionic_passthrough\":" + !ordinary.isEmpty() + "}");
    return 1;
  }
}
