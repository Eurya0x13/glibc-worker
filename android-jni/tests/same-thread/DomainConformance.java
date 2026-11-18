import conformance.Module;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/** Reuses the production JNI method/field/array matrix without JRE-private load hooks. */
public final class DomainConformance {
  private static native void failNextContext();
  private static void require(boolean ok) {
    if (!ok) {
      throw new AssertionError("domain conformance");
    }
  }

  public static int run() throws Exception {
    AtomicReference<Throwable> injectedFailure = new AtomicReference<>();
    failNextContext();
    Thread recovery = new Thread(() -> {
      try {
        try {
          Module.tlsGet();
          throw new AssertionError("context failure was not reported");
        } catch (OutOfMemoryError expected) {
          require(Module.tlsGet() == 17);
        }
      } catch (Throwable error) {
        injectedFailure.set(error);
      }
    }, "domain-allocation-recovery");
    recovery.start();
    recovery.join(30000);
    require(!recovery.isAlive());
    if (injectedFailure.get() != null) {
      throw new AssertionError("failed context recovery", injectedFailure.get());
    }
    byte[] dynamic;
    try (InputStream input =
             DomainConformance.class.getResourceAsStream("/conformance/Dynamic.class");
        ByteArrayOutputStream output = new ByteArrayOutputStream()) {
      byte[] bytes = new byte[4096];
      int n;
      while ((n = input.read(bytes)) != -1) {
        output.write(bytes, 0, n);
      }
      dynamic = output.toByteArray();
    }
    for (int group = 0; group < 10; ++group) {
      int result = Module.group(group, dynamic);
      if (result != 0) {
        throw new AssertionError("JNI group " + group + " failed at " + result);
      }
    }
    require(Module.mixed(0x123456789L, 2.5, "abc") == 0x123456789L + 253);
    AtomicReference<Throwable> failure = new AtomicReference<>();
    Object monitor = new Object();
    int[] increments = {0};
    // Each round holds 32 live contexts before allowing their work to finish.
    // Java owns these threads: no fixture prebinding or manual slot assignment.
    for (int round = 0; round < 3; ++round) {
      CountDownLatch ready = new CountDownLatch(32), go = new CountDownLatch(1);
      Thread[] children = new Thread[32];
      for (int i = 0; i < children.length; ++i) {
        final int value = round * 32 + i + 100;
        children[i] = new Thread(() -> {
          try {
            require(Module.tlsSet(value) == 17);
            Module.marker.set(value);
            ready.countDown();
            require(go.await(30, TimeUnit.SECONDS));
            require(Module.bounceNative(32) == 64);
            for (int j = 0; j < 10; ++j) {
              require(Module.tlsGet() == value);
              Module.lockIncrement(monitor, increments);
            }
          } catch (Throwable error) {
            failure.compareAndSet(null, error);
          } finally {
            ready.countDown();
          }
        }, "domain-conformance-" + value);
        children[i].start();
      }
      boolean allReady = ready.await(30, TimeUnit.SECONDS);
      go.countDown();
      require(allReady);
      for (Thread child : children) {
        child.join(30000);
        require(!child.isAlive());
      }
      if (failure.get() != null) {
        throw new AssertionError("Java carrier lifecycle", failure.get());
      }
      System.gc();
    }
    require(increments[0] == 960);
    System.out.println("JNI_DOMAIN_CONFORMANCE {\"groups\":10,\"java_threads\":96,"
        + "\"concurrent_threads\":32,\"reentry_depth\":32,\"monitor_increments\":960}");
    return 1;
  }
}
