package multi;
import dev.glibc.worker.NativeBridge;
import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.TimeUnit;
import java.util.function.IntUnaryOperator;

/** Tests the JRE load API; no Guest implementation details in the modules. */
public final class MultiSuite {
  static int checks, failures;

  public static final class OwnershipTarget {
    public static native int probe();
    public static native int other();
  }

  static int registerTarget(Loaded module, int kind) throws Exception {
    return (Integer) module.call(
        "registerTarget", new Class<?>[] {Class.class, int.class}, OwnershipTarget.class, kind);
  }

  static void requireUnbound() {
    boolean probe = false, other = false;
    try {
      OwnershipTarget.probe();
    } catch (UnsatisfiedLinkError expected) {
      probe = true;
    }
    try {
      OwnershipTarget.other();
    } catch (UnsatisfiedLinkError expected) {
      other = true;
    }
    requireScope(probe && other);
  }

  static void registrationOwnership(Loaded a, Loaded b) throws Exception {
    requireScope(registerTarget(a, 1) == 0 && OwnershipTarget.probe() == 3456);
    boolean registerDenied = false, unregisterDenied = false;
    try {
      registerTarget(b, 0);
    } catch (IllegalStateException expected) {
      registerDenied = true;
    }
    try {
      b.call("unregisterTarget", new Class<?>[] {Class.class}, OwnershipTarget.class);
    } catch (IllegalStateException expected) {
      unregisterDenied = true;
    }
    requireScope(registerDenied && unregisterDenied && OwnershipTarget.probe() == 3456);
    requireScope(
        (Integer) a.call("unregisterTarget", new Class<?>[] {Class.class}, OwnershipTarget.class)
        == 0);
    requireUnbound();
    requireScope(registerTarget(a, 3) == -1); // Detached worker: shared dependency is ambiguous.
    requireUnbound();
    requireScope(registerTarget(a, 4) == -1); // Every entry must resolve, not just the first.
    requireUnbound();
    requireScope(registerTarget(a, 2) == 0 && OwnershipTarget.probe() == 111);
    requireScope(
        (Integer) a.call("unregisterTarget", new Class<?>[] {Class.class}, OwnershipTarget.class)
        == 0);
    requireUnbound();
    requireScope(registerTarget(b, 1) == 0 && OwnershipTarget.probe() == 3456);
  }

  public static int checkCount() {
    return checks;
  }

  static void check(boolean condition, String name) {
    checks++;
    System.out.println("MULTI_TEST " + name + "=" + (condition ? "PASS" : "FAIL"));
    if (!condition) {
      failures++;
    }
  }

  static int metric(int which) {
    return NativeBridge.bridgeMetric(which);
  }

  static final class Loaded {
    URLClassLoader loader;
    Class<?> type;
    long handle;

    Loaded() {
      try {
        loader = new URLClassLoader(
            new URL[] {new File(System.getProperty("java.class.path")).toURI().toURL()}, null);
        type = loader.loadClass("multi.Module");
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    }

    void load(String path) throws Exception {
      call("load", new Class<?>[] {String.class}, path);
      handle = NativeBridge.bridgeLastHandle();
    }

    Object call(String name, Class<?>[] args, Object... values) throws Exception {
      try {
        return type.getMethod(name, args).invoke(null, values);
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

    int noarg(String name) throws Exception {
      return (Integer) call(name, new Class<?>[] {});
    }

    int integer(String name, int value) throws Exception {
      return (Integer) call(name, new Class<?>[] {int.class}, value);
    }

    long cookie() throws Exception {
      return (Long) call("cookie", new Class<?>[] {});
    }

    WeakReference<ClassLoader> drop() throws Exception {
      WeakReference<ClassLoader> w = new WeakReference<ClassLoader>(loader);
      loader.close();
      type = null;
      loader = null;
      return w;
    }
  }

  static void awaitUnload(Loaded module, int expectedActive, String name) throws Exception {
    long old = module.handle;
    WeakReference<ClassLoader> w = module.drop();
    long deadline = System.nanoTime() + 8_000_000_000L;

    while ((w.get() != null || NativeBridge.bridgeHandleState(old) != -1
               || metric(9) != expectedActive)
        && System.nanoTime() < deadline) {
      System.gc();
      Thread.sleep(20);
    }

    check(
        w.get() == null && NativeBridge.bridgeHandleState(old) == -1 && metric(9) == expectedActive,
        name);

    if (w.get() != null || NativeBridge.bridgeHandleState(old) != -1) {
      throw new AssertionError("Unload timed out: " + name);
    }
  }

  static void crossCalls(Loaded a, Loaded b) throws Exception {
    IntUnaryOperator toA = depth -> {
      try {
        return a.integer("route", depth);
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    };
    IntUnaryOperator toB = depth -> {
      try {
        return b.integer("route", depth);
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    };

    a.type.getField("peer").set(null, toB);
    b.type.getField("peer").set(null, toA);

    try {
      check(a.integer("route", 3) == 666, "cross_library_reentry");
    } finally {
      a.type.getField("peer").set(null, null);
      b.type.getField("peer").set(null, null);
    }
  }

  static void concurrent(Loaded a, Loaded b) throws Exception {
    List<Throwable> errors = Collections.synchronizedList(new ArrayList<Throwable>());
    Thread[] threads = new Thread[4];

    for (int i = 0; i < threads.length; i++) {
      final int number = i;
      threads[i] = new Thread(() -> {
        try {
          if (a.integer("ownTls", 100 + number) != 17 || b.integer("ownTls", 200 + number) != 17
              || a.integer("depTls", -1) != 9) {
            throw new AssertionError("Fresh TLS");
          }
          a.integer("depTls", 300 + number);
          if (b.integer("depTls", -1) != 300 + number) {
            throw new AssertionError("Shared dependency TLS");
          }
          for (int n = 0; n < 25; n++)
            if (a.integer("cached", n) != 10000 + n || b.integer("cached", n) != 10000 + n
                || a.integer("ownTls", -1) != 100 + number
                || b.integer("ownTls", -1) != 200 + number) {
              throw new AssertionError("Cross-library TLS/cache");
            }
        } catch (Throwable t) {
          errors.add(t);
        }
      }, "multi-java-" + i);
      threads[i].start();
    }

    for (Thread t : threads) {
      t.join();
    }
    check(errors.isEmpty(), "concurrent_two_libraries_" + errors);
  }

  static void requireScope(boolean good) {
    if (!good)
      throw new AssertionError("Execution scope contract");
  }

  static int closeBusy(Loaded target) {
    requireScope(NativeBridge.bridgeTryClose(target.handle) == 9);
    requireScope(NativeBridge.bridgeHandleState(target.handle) == 2);
    return 0;
  }

  static IntUnaryOperator routeTo(Loaded module) {
    return depth -> {
      try {
        return module.integer("route", depth);
      } catch (RuntimeException failure) {
        throw failure;
      } catch (Exception failure) {
        throw new RuntimeException(failure);
      }
    };
  }

  static void executionScopes(Loaded a, Loaded b) throws Exception {
    int closures = metric(5), unloads = metric(2), errors = metric(4);
    try {
      a.type.getField("peer").set(null, (IntUnaryOperator) depth -> closeBusy(a));
      requireScope(a.integer("route", 1) == 111);
      a.type.getField("peer").set(null, routeTo(b));
      b.type.getField("peer").set(null, (IntUnaryOperator) depth -> {
        closeBusy(a); // A -> Java -> B -> Java -> close(A).
        return closeBusy(b);
      });
      requireScope(a.integer("route", 2) == 333);
      a.type.getField("peer").set(null, (IntUnaryOperator) depth -> {
        if (depth == 0)
          return closeBusy(b);
        return routeTo(b).applyAsInt(depth);
      });
      b.type.getField("peer").set(null, routeTo(a));
      requireScope(a.integer("route", 3) == 444); // A -> B -> A -> close(B).
      a.type.getField("peer").set(null, routeTo(b));
      b.type.getField("peer").set(
          null, (IntUnaryOperator) depth -> { throw new IllegalStateException("scope-unwind"); });
      boolean unwound = false;
      try {
        a.integer("route", 2);
      } catch (IllegalStateException expected) {
        unwound = "scope-unwind".equals(expected.getMessage());
      }
      requireScope(unwound && a.integer("cached", 7) == 10007 && b.integer("cached", 8) == 10008);
      requireScope(metric(5) == closures && metric(2) == unloads && metric(4) == errors);
    } finally {
      a.type.getField("peer").set(null, null);
      b.type.getField("peer").set(null, null);
    }
    System.out.println("AUDIT_SCOPE_TEST {\"result\":\"PASS\",\"cases\":4,"
        + "\"busy_refusals\":4,\"exception_unwind\":true,\"resources_preserved\":true}");
  }

  static IntUnaryOperator closePeer(Loaded target, CyclicBarrier ready, CyclicBarrier returned) {
    return depth -> {
      try {
        ready.await(5, TimeUnit.SECONDS);
        int result = closeBusy(target);
        // Both native calls stay in flight until both closes have returned.
        returned.await(5, TimeUnit.SECONDS);
        return result;
      } catch (RuntimeException failure) {
        throw failure;
      } catch (Exception failure) {
        throw new AssertionError("Cross-thread close rendezvous", failure);
      }
    };
  }

  static Thread routeThread(Loaded module, int expected, List<Throwable> errors) {
    Thread thread = new Thread(() -> {
      try {
        requireScope(module.integer("route", 1) == expected);
      } catch (Throwable failure) {
        errors.add(failure);
      }
    }, "scope-close-" + expected);
    thread.setDaemon(true);
    return thread;
  }

  static void closeWaits(Loaded a, Loaded b) throws Exception {
    int closures = metric(5), unloads = metric(2), errorsBefore = metric(4);
    List<Throwable> errors = Collections.synchronizedList(new ArrayList<Throwable>());
    CyclicBarrier ready = new CyclicBarrier(2), returned = new CyclicBarrier(2);
    a.type.getField("peer").set(null, closePeer(b, ready, returned));
    b.type.getField("peer").set(null, closePeer(a, ready, returned));
    Thread first = routeThread(a, 111, errors), second = routeThread(b, 222, errors);
    try {
      first.start();
      second.start();
      first.join(10_000);
      second.join(10_000);
      requireScope(!first.isAlive() && !second.isAlive() && errors.isEmpty());
      requireScope(a.integer("cached", 9) == 10009 && b.integer("cached", 10) == 10010);
      requireScope(metric(5) == closures && metric(2) == unloads && metric(4) == errorsBefore);
    } finally {
      a.type.getField("peer").set(null, null);
      b.type.getField("peer").set(null, null);
    }
    System.out.println("AUDIT_CLOSE_WAIT_TEST {\"result\":\"PASS\",\"threads\":2,"
        + "\"busy_refusals\":2,\"resources_preserved\":true}");
  }

  static void rejectedLoad(String path, Loaded a, Loaded b) throws Exception {
    Loaded rejected = new Loaded();
    int closures = metric(5), failed = metric(12);
    boolean threw = false;

    try {
      rejected.load(path);
    } catch (UnsatisfiedLinkError expected) {
      threw = true;
    }
    check(threw && metric(9) == 2 && metric(5) == closures && metric(12) == failed + 1,
        "register_then_reject_transaction");

    boolean unbound = false;
    try {
      rejected.noarg("identity");
    } catch (UnsatisfiedLinkError expected) {
      unbound = true;
    }
    check(unbound, "failed_registration_is_unbound");
    check(a.noarg("identity") == 111 && b.integer("cached", 3) == 10003,
        "failed_load_preserves_other_libraries");

    rejected.drop();
  }

  static void sqlite(String directory, Loaded a, Loaded b) throws Exception {
    Loaded sql = new Loaded();
    sql.type = sql.loader.loadClass("multi.Sqlite");
    sql.load(directory + "/libdatabase_jni.so");
    check(metric(9) == 3 && "3.50.4".equals(sql.call("version", new Class<?>[] {})),
        "sqlite_real_glibc_dependency_loaded");

    String database =
        new File(System.getProperty("java.home"), "bridge-sqlite-spike.db").getAbsolutePath();
    check((Integer) sql.call("exercise", new Class<?>[] {String.class}, database) == 0,
        "sqlite_wal_pthreads_integrity_and_reopen");

    awaitUnload(sql, 2, "sqlite_module_gc_unload");
    check(a.noarg("identity") == 111 && b.integer("cached", 4) == 10004,
        "sqlite_unload_preserves_other_modules");
  }

  public static int run(String directory) throws Exception {
    int loads = metric(1), unloads = metric(2), dtors = metric(3), depC = metric(13),
        depD = metric(14);
    Loaded a = new Loaded(), b = new Loaded();
    a.load(directory + "/libmulti_a.so");
    long first = a.handle;
    b.load(directory + "/libmulti_b.so");

    check(a.handle != b.handle && NativeBridge.bridgeHandleState(a.handle) == 2
            && NativeBridge.bridgeHandleState(b.handle) == 2,
        "distinct_live_handles");
    check(a.noarg("identity") == 111 && b.noarg("identity") == 222 && a.noarg("named") == 112
            && b.noarg("named") == 223,
        "same_named_class_and_symbols_isolated");
    check(metric(9) == 2 && metric(13) == depC + 1 && a.cookie() == b.cookie(),
        "one_shared_dependency_mapping");
    check(a.integer("add", 3) == 3 && b.integer("add", 4) == 7, "dependency_shared_state");
    check(a.integer("ownTls", 73) == 17 && b.integer("ownTls", -1) == 17
            && a.integer("depTls", 77) == 9 && b.integer("depTls", -1) == 77,
        "root_and_dependency_tls");
    check(a.integer("cached", 1) == 10001 && b.integer("cached", 2) == 10002,
        "per_class_method_cache");

    crossCalls(a, b);
    executionScopes(a, b);
    closeWaits(a, b);
    registrationOwnership(a, b);
    concurrent(a, b);
    sqlite(directory, a, b);
    rejectedLoad(directory + "/libmulti_reject.so", a, b);

    Loaded missing = new Loaded();
    boolean missingRejected = false;
    int failed = metric(12);
    try {
      missing.load(directory + "/libdependency_failure.so");
    } catch (UnsatisfiedLinkError expected) {
      missingRejected = expected.getMessage().contains("libmissing_spike.so");
    }
    check(missingRejected && metric(9) == 2 && metric(12) == failed + 1,
        "missing_dependency_message_and_rollback");
    missing.drop();

    Loaded neutral = new Loaded();
    neutral.load(directory + "/libguest_noonload.so");
    check(metric(9) == 3, "third_guest_library_accepted");
    awaitUnload(neutral, 2, "third_library_unloads_independently");

    awaitUnload(a, 1, "first_library_gc_unload");
    requireScope(OwnershipTarget.probe() == 3456);
    check(metric(14) == depD && b.noarg("named") == 223 && b.integer("cached", 9) == 10009
            && b.noarg("value") == 7,
        "survivor_cache_closure_and_dependency");

    Loaded again = new Loaded();
    again.load(directory + "/libmulti_a.so");
    check(again.handle != first && NativeBridge.bridgeHandleState(first) == -1
            && NativeBridge.bridgeHandleState(again.handle) == 2,
        "stale_handle_generation_rejected");
    check(again.integer("ownTls", -1) == 17 && again.integer("add", 1) == 8
            && again.cookie() == b.cookie() && metric(13) == depC + 1,
        "reload_root_keeps_shared_dependency");
    awaitUnload(again, 1, "reloaded_first_library_unloads");
    awaitUnload(b, 0, "last_owner_unloads");
    requireUnbound();
    System.out.println("AUDIT_OWNERSHIP_TEST {\"result\":\"PASS\",\"handoff\":true,"
        + "\"dependency_context\":true,\"background_top\":true,"
        + "\"ambiguous_dependency_rejected\":true,\"mixed_batch_rejected\":true,"
        + "\"old_owner_unload_preserved_new\":true,\"final_owner_unload_unbound\":true}");
    check(metric(14) == depD + 1, "dependency_destroyed_after_last_owner");

    Loaded fresh = new Loaded();
    fresh.load(directory + "/libmulti_b.so");
    check(fresh.noarg("value") == 0 && fresh.integer("depTls", -1) == 9 && metric(13) == depC + 2,
        "fresh_dependency_after_full_unload");
    awaitUnload(fresh, 0, "fresh_cycle_unloads");

    check(metric(1) == loads + 6 && metric(2) == unloads + 6 && metric(3) == dtors + 7
            && metric(14) == depD + 2,
        "all_module_and_dependency_lifecycles");
    check(metric(5) == 0 && metric(7) == 0 && metric(8) == 0 && metric(9) == 0,
        "multi_resources_reclaimed");

    System.out.println(
        "MULTI_TEST_SUMMARY checks=" + checks + " failures=" + failures + " peak=" + metric(10));
    return failures;
  }
}
