package conformance;
import dev.glibc.worker.NativeBridge;
import java.io.*;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;

public final class Runner {
  static int failures, checks, unsupported;

  static synchronized void check(boolean b, String name) {
    checks++;
    System.out.println("JNI_TEST " + name + "=" + (b ? "PASS" : "FAIL"));
    if (!b) {
      failures++;
    }
  }

  static Object invoke(Class<?> c, String name, Class<?>[] types, Object... args) throws Exception {
    try {
      return c.getMethod(name, types).invoke(null, args);
    } catch (InvocationTargetException e) {
      Throwable t = e.getCause();
      if (t instanceof Error) {
        throw (Error) t;
      }
      if (t instanceof Exception) {
        throw (Exception) t;
      }
      throw e;
    }
  }

  static URLClassLoader loader() throws Exception {
    return new URLClassLoader(
        new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL()}, null);
  }

  static byte[] dynamic() throws Exception {
    try (InputStream in = Runner.class.getResourceAsStream("/conformance/Dynamic.class");
        ByteArrayOutputStream out = new ByteArrayOutputStream()) {
      byte[] b = new byte[4096];
      int n;
      while ((n = in.read(b)) != -1) {
        out.write(b, 0, n);
      }
      return out.toByteArray();
    }
  }

  @SuppressWarnings("unchecked")
  static WeakReference<ClassLoader> cycle(String path, int cycle) throws Exception {
    URLClassLoader l = loader();
    Class<?> c = l.loadClass("conformance.Module");
    int before = NativeBridge.bridgeMetric(1);

    if (cycle == 1) {
      invoke(c, "loadLibrary", new Class<?>[] {});
    } else {
      invoke(c, "load", new Class<?>[] {String.class}, path);
    }
    invoke(c, "load", new Class<?>[] {String.class}, path);
    check(NativeBridge.bridgeMetric(1) == before + 1, "onload_once_cycle_" + cycle);

    Class<?> capacity = l.loadClass("conformance.BindingCapacity");
    if (capacity.getField("enabled").getBoolean(null)) {
      int methods = (Integer) invoke(capacity, "verify", new Class<?>[] {});
      int live = NativeBridge.bridgeMetric(5);
      if (methods != 1024 || live < 1024) {
        throw new AssertionError("Binding capacity or invocation failure");
      }
      Class<?> resources = l.loadClass("conformance.ResourceCapacity");
      int classes = (Integer) invoke(resources, "verify", new Class<?>[] {});
      int references = resources.getField("references").getInt(null);
      if (classes != 320 || references != 20000) {
        throw new AssertionError("Resource capacity or invocation failure");
      }
      System.out.println("RESOURCE_CAPACITY_TEST {\"cycle\":" + cycle + ",\"classes\":" + classes
          + ",\"references\":" + references + "}");
      System.out.println("BINDING_CAPACITY_TEST {\"cycle\":" + cycle + ",\"methods\":" + methods
          + ",\"live_closures\":" + live + "}");
    }

    if (cycle == 0) {
      invoke(c, "load", new Class<?>[] {String.class},
          new File(path).getParent() + "/libhost_control.so");
      check((Integer) invoke(c, "hostControl", new Class<?>[] {int.class}, 5) == 105,
          "bionic_load_lookup_fallback");

      byte[] bytes = dynamic();
      for (int group = 0; group < 10; group++) {
        try {
          int rc =
              (Integer) invoke(c, "group", new Class<?>[] {int.class, byte[].class}, group, bytes);
          check(rc == 0, "group_" + group + "_rc_" + rc);
        } catch (Throwable t) {
          System.out.println("JNI_TEST_EXCEPTION group=" + group + " " + t);
          check(false, "group_" + group);
        }

        try {
          URLClassLoader other = loader();
          Class<?> second = other.loadClass("conformance.Module");
          boolean rejected = false;
          try {
            invoke(second, "load", new Class<?>[] {String.class}, path);
          } catch (UnsatisfiedLinkError expected) {
            rejected = true;
          }
          check(rejected, "different_classloader_duplicate_rejected");
          other.close();
        } catch (Throwable t) {
          check(false, "duplicate_loader_test");
        }

        ((ThreadLocal<Integer>) c.getField("marker").get(null)).set(913);
      }
      check((Integer) invoke(c, "bounceNative", new Class<?>[] {int.class}, 4) == 8,
          "nested_reentry_depth_4");
      check((Long) invoke(c, "mixed", new Class<?>[] {long.class, double.class, Object.class},
                0x123456789L, 2.5, "abc")
              == 0x123456789L + 253,
          "mixed_native_abi");
      boolean thrown = false;
      try {
        invoke(c, "throwNative", new Class<?>[] {});
      } catch (IllegalStateException expected) {
        thrown = expected.getMessage().contains("guest native throw");
      }
      check(thrown, "native_exception_to_java");
      check((Integer) invoke(c, "namedOnly", new Class<?>[] {int.class}, 5) == 6,
          "automatic_Java_symbol_binding");
      check((Integer) invoke(c, "overloaded", new Class<?>[] {int.class}, 5) == 7
              && (Long) invoke(c, "overloaded", new Class<?>[] {long.class}, 5L) == 8L,
          "long_name_overloads");
      boolean missing = false;
      try {
        invoke(c, "missingSymbol", new Class<?>[] {});
      } catch (UnsatisfiedLinkError expected) {
        missing = true;
      }
      check(missing, "missing_symbol_rejected");

      Class<?>[] echoTypes = {boolean.class, byte.class, char.class, short.class, int.class,
          long.class, float.class, double.class, Object.class};
      String[] echoNames = {"Z", "B", "C", "S", "I", "J", "F", "D", "L"};
      Object[] echoValues = {
          true, (byte) -7, '中', (short) -2000, -123456, 0x123456789abcL, 1.25f, 2.5, new Object()};
      for (int i = 0; i < echoTypes.length; i++) {
        check(echoValues[i].equals(
                  invoke(c, "echo" + echoNames[i], new Class<?>[] {echoTypes[i]}, echoValues[i])),
            "native_return_" + echoNames[i]);
      }
      Object receiver = c.getConstructor().newInstance(), object = new Object();
      check(c.getMethod("instanceEcho", Object.class).invoke(receiver, object) == object,
          "instance_native_object_return");

      final Class<?> fixture = c;
      final List<Throwable> errors = Collections.synchronizedList(new ArrayList<Throwable>());
      final Object lock = new Object();
      final int[] count = {0};
      Thread[] threads = new Thread[4];

      for (int i = 0; i < threads.length; i++) {
        final int v = 100 + i;
        threads[i] = new Thread(() -> {
          try {
            check((Integer) invoke(fixture, "tlsSet", new Class<?>[] {int.class}, v) == 17,
                "fresh_guest_tls_" + v);
            for (int k = 0; k < 20; k++) {
              if ((Integer) invoke(fixture, "tlsGet", new Class<?>[] {}) != v) {
                throw new AssertionError("TLS mixed");
              }
              invoke(fixture, "lockIncrement", new Class<?>[] {Object.class, int[].class}, lock,
                  count);
            }
          } catch (Throwable t) {
            errors.add(t);
          }
        }, "java-worker-" + i);
        threads[i].start();
      }
      for (Thread t : threads) {
        t.join();
      }
      check(
          errors.isEmpty() && count[0] == 80, "concurrent_tls_monitors_" + count[0] + "_" + errors);

      for (int i = 0; i < 20; i++) {
        final int v = i;
        Thread t = new Thread(() -> {
          try {
            if ((Integer) invoke(fixture, "tlsSet", new Class<?>[] {int.class}, v) != 17) {
              throw new AssertionError("recycled TLS");
            }
          } catch (Throwable e) {
            errors.add(e);
          }
        });
        t.start();
        t.join();
      }
      check(errors.isEmpty(), "thread_exit_and_slot_reuse");
    }

    l.close();
    return new WeakReference<ClassLoader>(l);
  }

  public static int run(String path) throws Exception {
    String dir = new File(path).getParent();
    URLClassLoader control = loader();
    Class<?> cc = control.loadClass("conformance.Module");

    for (String name : new String[] {"reject", "badversion"}) {
      int before = NativeBridge.bridgeMetric(1), dtors = NativeBridge.bridgeMetric(3);
      boolean rejected = false;
      try {
        invoke(cc, "load", new Class<?>[] {String.class}, dir + "/libguest_" + name + ".so");
      } catch (UnsatisfiedLinkError expected) {
        rejected = true;
      }
      check(rejected && NativeBridge.bridgeMetric(1) == before
              && NativeBridge.bridgeMetric(3) == dtors + 1,
          "load_" + name + "_rollback");
    }

    int nohookBefore = NativeBridge.bridgeMetric(1), unloadBefore = NativeBridge.bridgeMetric(2);
    invoke(cc, "load", new Class<?>[] {String.class}, dir + "/libguest_noonload.so");
    check(NativeBridge.bridgeMetric(1) == nohookBefore + 1, "absent_onload_defaults_1_1");

    WeakReference<ClassLoader> controlWeak = new WeakReference<ClassLoader>(control);
    control.close();
    cc = null;
    control = null;

    long controlDeadline = System.nanoTime() + 8_000_000_000L;
    while ((controlWeak.get() != null || NativeBridge.bridgeMetric(2) != unloadBefore + 1)
        && System.nanoTime() < controlDeadline) {
      System.gc();
      Thread.sleep(20);
    }
    check(controlWeak.get() == null && NativeBridge.bridgeMetric(2) == unloadBefore + 1,
        "absent_onunload_dlclose");

    int baseline = NativeBridge.bridgeMetric(2);
    int destructorBaseline = NativeBridge.bridgeMetric(3);
    for (int i = 0; i < 3; i++) {
      WeakReference<ClassLoader> w = cycle(path, i);
      long deadline = System.nanoTime() + 8_000_000_000L;
      while ((w.get() != null || NativeBridge.bridgeMetric(2) != baseline + i + 1)
          && System.nanoTime() < deadline) {
        System.gc();
        Thread.sleep(20);
      }
      check(w.get() == null && NativeBridge.bridgeMetric(2) == baseline + i + 1,
          "classloader_gc_onunload_" + i);
      check(NativeBridge.bridgeMetric(3) == destructorBaseline + i + 1, "dso_destructor_" + i);
      check(NativeBridge.bridgeMetric(5) == 0, "closure_reclaimed_" + i);
    }

    check(NativeBridge.bridgeMetric(7) == 0 && NativeBridge.bridgeMetric(8) == 0,
        "global_and_weak_cleanup");
    failures += multi.MultiSuite.run(dir);
    checks += multi.MultiSuite.checkCount();
    checks += real.ZstdSuite.run(dir);

    System.out.println("JNI_TEST_SUMMARY checks=" + checks + " failures=" + failures
        + " unsupported=" + unsupported);
    compat.NativeJarSuite.runIfEnabled(dir);
    return failures;
  }
}
