import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.LinkedHashMap;
import java.util.Map;

public final class AdapterTest {
  static native int install(String library, String digest, int family);
  static native int restore();
  static native int[] metrics();
  static native int stop();
  static native void faults(int mask);
  static native String error();
  static final Map<String, Boolean> checks = new LinkedHashMap<>();

  static void check(String name, boolean value) {
    if (!value || checks.put(name, value) != null) {
      throw new AssertionError(name);
    }
    System.out.println("adapter check: " + name);
  }
  static URLClassLoader loader(String jar) throws Exception {
    return new URLClassLoader(
        new URL[] {new File(jar).toURI().toURL()}, AdapterTest.class.getClassLoader());
  }
  static Object call(Class<?> cls, String method, Class<?>[] types, Object... values)
      throws Exception {
    try {
      return cls.getMethod(method, types).invoke(null, values);
    } catch (InvocationTargetException e) {
      if (e.getCause() instanceof Error) {
        throw (Error) e.getCause();
      }
      if (e.getCause() instanceof Exception) {
        throw (Exception) e.getCause();
      }
      throw e;
    }
  }
  static WeakReference<ClassLoader> cycle(String jar, String image, int cycle) throws Exception {
    URLClassLoader loader = loader(jar);
    Class<?> cls = loader.loadClass("AdapterFixture");
    call(cls, "load", new Class<?>[] {String.class}, image);
    call(cls, "load", new Class<?>[] {String.class}, image);
    check("echo_" + cycle, (Long) call(cls, "echo", new Class<?>[] {long.class}, 35L) == 42);
    check("overload_i_" + cycle,
        (Integer) call(cls, "overload", new Class<?>[] {int.class}, 2) == 13);
    check("overload_d_" + cycle,
        (Double) call(cls, "overload", new Class<?>[] {double.class}, 1.0) == 1.5);
    boolean missing = false;
    try {
      call(cls, "missing", new Class<?>[] {long.class}, 1L);
    } catch (UnsatisfiedLinkError expected) {
      missing = true;
    }
    check("missing_" + cycle, missing);
    URLClassLoader other = loader(jar);
    Class<?> duplicate = other.loadClass("AdapterFixture");
    boolean rejected = false;
    try {
      call(duplicate, "load", new Class<?>[] {String.class}, image);
    } catch (UnsatisfiedLinkError expected) {
      rejected = true;
    }
    check("different_loader_rejected_" + cycle, rejected);
    other.close();
    WeakReference<ClassLoader> reference = new WeakReference<ClassLoader>(loader);
    loader.close();
    return reference;
  }
  static void awaitUnload(WeakReference<ClassLoader> reference, int count) throws Exception {
    java.lang.reflect.Field field = dev.glibc.worker.JvmAdapter.class.getDeclaredField("owners");
    field.setAccessible(true);
    Map<?, ?> owners = (Map<?, ?>)field.get(null);
    for (int i = 0; i < 200; i++) {
      System.gc();
      if (reference.get() == null && metrics()[1] >= count && owners.isEmpty()) {
        return;
      }
      Thread.sleep(25);
    }
    throw new AssertionError("ClassLoader/unload did not finish");
  }
  static void rejection(String jar, String image) throws Exception {
    URLClassLoader loader = loader(jar);
    Class<?> cls = loader.loadClass("AdapterFixture");
    boolean rejected = false;
    try {
      call(cls, "load", new Class<?>[] {String.class}, image);
    } catch (UnsatisfiedLinkError expected) {
      rejected = true;
    }
    loader.close();
    check("onload_rejection", rejected);
  }
  static boolean wrongFamily(String library, String digest, int family) {
    try {
      return install(library, digest, family) == -1;
    } catch (LinkageError expected) {
      return true;
    }
  }
  static void ordinaryLoad(String jar, String image) throws Exception {
    URLClassLoader ordinary = loader(jar);
    Class<?> cls = ordinary.loadClass("AdapterFixture");
    call(cls, "load", new Class<?>[] {String.class}, image);
    check("ordinary_library_while_installed",
        (Long) call(cls, "echo", new Class<?>[] {long.class}, 35L) == 42);
    ordinary.close();
  }
  static void writeResult(String path) throws Exception {
    try (java.io.PrintWriter out = new java.io.PrintWriter(path, "UTF-8")) {
      out.print("{\"result\":\"PASS\",\"kind\":\"real-jvm-adapter-contract\",\"dual_libc\":false,"
          + "\"checks\":{");
      boolean first = true;
      for (String name : checks.keySet()) {
        if (!first) {
          out.print(",");
        }
        first = false;
        out.print("\"" + name + "\":true");
      }
      out.println("}}");
    }
  }
  public static void main(String[] args) throws Exception {
    System.out.println("adapter main");
    System.load(args[0]);
    int family = Integer.parseInt(args[3]);
    if (args.length == 10) {
      faults((1 << 1) | (1 << 2));
      check("failed_restoration_rejected", install(args[1], args[2], family) == -1);
      check("process_disposal_required", error().contains("process disposal required"));
      check("failed_restoration_profile_frozen", install(args[1], args[2], family) == -1);
      writeResult(args[7]);
      return; // Dedicated VM process; never reuse the partially restored adapter.
    }
    check("hash_rejected",
        install(args[1], args[2].replaceFirst(".", args[2].startsWith("0") ? "1" : "0"), family)
            == -1);
    check("unknown_family_rejected", install(args[1], args[2], 999) == -1);
    check("wrong_family_rejected", wrongFamily(args[1], args[2], family == 1 ? 6 : 1));
    faults(1 << 1);
    check("partial_registration_rejected", install(args[1], args[2], family) == -1);
    check("partial_registration_restored", error().contains("originals restored"));
    check("installed", install(args[1], args[2], family) == 1);
    check("profile_frozen", install(args[1], args[2], family) == -1);
    ordinaryLoad(args[4], args[8]);
    rejection(args[4], args[6]);
    for (int i = 0; i < 2; i++) {
      WeakReference<ClassLoader> ref = cycle(args[4], args[5], i);
      awaitUnload(ref, i + 1);
      check("unload_and_gc_" + i, true);
    }
    int[] metrics = metrics();
    check("two_loads_two_unloads", metrics[0] == 2 && metrics[1] == 2);
    check("rejection_rollback", metrics[2] == 1);
    check("descriptor_lookup", metrics[3] >= 6);
    check("restored", restore() == 1);
    check("stop", stop() == 1);
    URLClassLoader ordinary = loader(args[4]);
    Class<?> cls = ordinary.loadClass("AdapterFixture");
    call(cls, "load", new Class<?>[] {String.class}, args[5]);
    check("original_loader_after_restore",
        (Long) call(cls, "echo", new Class<?>[] {long.class}, 35L) == 42);
    ordinary.close();
    writeResult(args[7]);
  }
}
