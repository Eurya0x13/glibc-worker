import java.util.concurrent.atomic.AtomicLong;

/** Test-owned Java endpoints, deliberately separate from the production bridge. */
public final class DomainProbe {
  private static final ThreadLocal<Integer> depth = ThreadLocal.withInitial(() -> 0);
  private static final AtomicLong calls = new AtomicLong();
  private static final AtomicLong exceptions = new AtomicLong();
  private static final AtomicLong collections = new AtomicLong();
  private static volatile boolean running;
  private static Thread collector;
  private static volatile Object retained;

  private static final class Box {
    int value = 42;
  }

  private static int read(Box box) {
    return box.value;
  }

  private static native int reenter();

  public static void start() {
    System.out.println("HOTSPOT_JAVA_VERSION " + System.getProperty("java.version"));
    running = true;
    collector = new Thread(() -> {
      while (running) {
        retained = new byte[65536];
        System.gc();
        collections.incrementAndGet();
        try {
          Thread.sleep(1);
        } catch (InterruptedException ignored) {
          Thread.currentThread().interrupt();
          return;
        }
      }
    }, "domain-gc-probe");
    collector.start();
  }

  public static int callback(int value, int nesting) {
    if (value != 42 || depth.get() != nesting) {
      throw new AssertionError("callback value/depth mismatch");
    }
    depth.set(nesting + 1);
    try {
      Box box = new Box();
      retained = box; // Materialize a heap object that remains live across reentry/GC.
      int identity = System.identityHashCode(box);
      if (read(box) != 42) {
        throw new AssertionError("Java object corrupted");
      }
      try {
        read(null);
        throw new AssertionError("missing NullPointerException");
      } catch (NullPointerException expected) {
        exceptions.incrementAndGet();
      }
      if (nesting < 7 && reenter() != 84) {
        throw new AssertionError("nested Guest result mismatch");
      }
      if (read(box) != 42 || System.identityHashCode(box) != identity) {
        throw new AssertionError("live Java reference changed across Guest execution");
      }
      calls.incrementAndGet();
      return value * 2;
    } finally {
      depth.set(nesting);
    }
  }

  public static long finish() throws InterruptedException {
    running = false;
    collector.join();
    if (calls.get() != 19200 || exceptions.get() != calls.get() || collections.get() == 0) {
      throw new AssertionError("Java lifecycle/count mismatch");
    }
    return collections.get();
  }
}
