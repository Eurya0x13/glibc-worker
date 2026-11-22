import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

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

  private static void checkOrdinaryLoad(String jar, String image) throws Exception {
    int before = stats()[0];
    try (URLClassLoader owner = loader(jar, image)) {
      Class<?> fixture = owner.loadClass("SystemLoadFixture");
      call(fixture, "load", new Class<?>[] {String.class}, image);
      check((Long) call(fixture, "echo", new Class<?>[] {long.class}, 35L) == 42,
          "ordinary native call");
      check(stats()[0] == before, "ordinary image bypasses Guest loader");
    }
  }

  private static void unicodePaths(String jar, Path image) throws Exception {
    Path dir = Files.createTempDirectory(image.getParent(), "unicode-\uD83D\uDE80-");
    Path guest = dir.resolve("guest.so");
    Path host = dir.resolve("host.so");
    try {
      Files.copy(image, guest);
      Files.copy(image.resolveSibling("system-ordinary.so"), host);

      int unloads = stats()[1];
      awaitUnload(cycle(jar, guest.toString(), false), unloads + 1);
      checkOrdinaryLoad(jar, host.toString());
    } finally {
      Files.deleteIfExists(host);
      Files.deleteIfExists(guest);
      Files.delete(dir);
    }
  }

  private static Class<?> loadFixture(URLClassLoader owner, Path image) throws Exception {
    Class<?> fixture = owner.loadClass("SystemLoadFixture");
    call(fixture, "load", new Class<?>[] {String.class}, image.toString());
    return fixture;
  }

  private static WeakReference<ClassLoader> dependencyConflict(
      String jar, Path first, Path second, int value, Path shared) throws Exception {
    try (URLClassLoader owner = loader(jar, first.toString());
        URLClassLoader other = loader(jar, second.toString())) {
      Class<?> fixture = loadFixture(owner, first);
      check((Integer) call(fixture, "dependencyValue", new Class<?>[0]) == value,
          "first module uses its bundled dependency");

      int expectedConstructors = 1;
      if (shared != null) {
        try (URLClassLoader common = loader(jar, shared.toString())) {
          Class<?> sharing = loadFixture(common, shared);
          check((Integer) call(sharing, "dependencyValue", new Class<?>[0]) == value,
              "the same dependency file can be shared");
          expectedConstructors++;
        }
      }

      try {
        loadFixture(other, second);
        throw new AssertionError("conflicting dependency accepted");
      } catch (UnsatisfiedLinkError expected) {
        String message = expected.getMessage();
        check(message.contains("Guest dependency conflict"), "dependency conflict: " + message);
        System.err.println(message);
      }

      check((Integer) call(fixture, "dependencyLoads", new Class<?>[0]) == expectedConstructors,
          "rejected module did not run its constructor");
      check((Integer) call(fixture, "dependencyValue", new Class<?>[0]) == value,
          "rejected module leaves the original dependency usable");
      return new WeakReference<>(owner);
    }
  }

  private static void loaderRegressions(String jar, String image) throws Exception {
    Path guest = Paths.get(image);
    unicodePaths(jar, guest);

    Path dependencies = guest.resolveSibling("system-dependencies");
    Path a = dependencies.resolve("a/module.so");
    Path b = dependencies.resolve("b/module.so");
    Path shared = dependencies.resolve("shared/module.so");
    int unloads = stats()[1];
    awaitUnload(dependencyConflict(jar, a, b, 101, shared), unloads + 2);
    awaitUnload(dependencyConflict(jar, b, a, 202, null), unloads + 3);

    int[] result = stats();
    check(result[2] == 0 && result[3] == result[4] && result[6] == 0,
        "path and dependency tests balance image and closure lifetimes");
    System.out.println("SYSTEM_LOAD_REGRESSIONS {\"result\":\"PASS\",\"unicode_paths\":2,"
        + "\"conflict_orders\":2,\"shared_dependency\":true}");
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
      checkOrdinaryLoad(jar, ordinary);
    }
    int[] result = stats();
    check(result[1] == 3 && result[2] == 0 && result[3] == result[4], "balanced image lifecycle");
    System.out.println("SYSTEM_LOAD_RESULT {\"result\":\"PASS\",\"cycles\":3,\"checks\":" + checks
        + ",\"unloads\":" + result[1] + ",\"active_modules\":" + result[2] + ",\"live_closures\":"
        + result[6] + ",\"bionic_passthrough\":" + !ordinary.isEmpty() + "}");
    if ("1".equals(System.getenv("GW_SYSTEM_LOAD_REGRESSIONS"))) {
      loaderRegressions(jar, image);
    }
    return 1;
  }
}
