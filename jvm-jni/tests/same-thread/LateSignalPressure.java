import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

/** Compatibility pressure only; these timings are not a performance result. */
public final class LateSignalPressure {
  static void run(Class<?> fixture) throws Exception {
    String mode = System.getenv("GW_DOMAIN_MODE");
    boolean adopted = mode == null || "domain".equals(mode);
    if (adopted && (Integer) fixture.getMethod("probeSignals").invoke(null) != 3) {
      throw new AssertionError("Guest TLS/errno signal round trip");
    }
    Class<?> recordingType = Class.forName("jdk.jfr.Recording");
    Object recording = recordingType.getConstructor().newInstance();
    Map<String, String> settings = new HashMap<>();
    settings.put("jdk.ExecutionSample#enabled", "true");
    settings.put("jdk.ExecutionSample#period", "1 ms");
    settings.put("jdk.NativeMethodSample#enabled", "true");
    settings.put("jdk.NativeMethodSample#period", "1 ms");
    recordingType.getMethod("setSettings", Map.class).invoke(recording, settings);
    recordingType.getMethod("setToDisk", boolean.class).invoke(recording, false);
    recordingType.getMethod("start").invoke(recording);
    Method hold = fixture.getMethod("hold");
    Method state = fixture.getMethod("holdState", int.class);
    Method echo = fixture.getMethod("echo", long.class);
    AtomicReference<Throwable> error = new AtomicReference<>();
    Thread holder = new Thread(() -> {
      try {
        if ((Integer) hold.invoke(null) != 73) {
          throw new AssertionError("Guest hold did not finish");
        }
      } catch (Throwable failure) {
        error.compareAndSet(null, failure);
      }
    }, "late-signal-guest-hold");
    holder.start();
    int overlaps = 0;
    Thread[] workers = new Thread[8];
    try {
      long deadline = System.nanoTime() + 10_000_000_000L;
      while ((Integer) state.invoke(null, 0) != 1 && System.nanoTime() < deadline) {
        Thread.yield();
      }
      if ((Integer) state.invoke(null, 0) != 1) {
        throw new AssertionError("Guest hold did not start");
      }
      for (int i = 0; i < workers.length; ++i) {
        workers[i] = new Thread(() -> {
          try {
            for (long value = 0; value < 1000; ++value) {
              if ((Long) echo.invoke(null, value) != value + 7) {
                throw new AssertionError("concurrent native result");
              }
            }
          } catch (Throwable failure) {
            error.compareAndSet(null, failure);
          }
        }, "late-signal-caller-" + i);
        workers[i].start();
      }
      for (int sample = 0; sample < 20; ++sample) {
        if ((Integer) state.invoke(null, 0) == 1) {
          ++overlaps;
        }
        if (Thread.getAllStackTraces().isEmpty()) {
          throw new AssertionError("empty stack walk");
        }
        System.gc();
      }
    } finally {
      state.invoke(null, 1);
      holder.join(20000);
      for (Thread worker : workers) {
        if (worker != null) {
          worker.join(20000);
          if (worker.isAlive()) {
            throw new AssertionError("native caller stuck");
          }
        }
      }
      recordingType.getMethod("stop").invoke(recording);
      recordingType.getMethod("close").invoke(recording);
    }
    if (holder.isAlive() || overlaps == 0 || error.get() != null) {
      throw new AssertionError("late signal pressure", error.get());
    }
    System.out.println("LATE_SIGNAL_PRESSURE {\"result\":\"PASS\",\"threads\":8,"
        + "\"calls\":8000,\"samples\":20,\"guest_overlaps\":" + overlaps
        + ",\"explicit_guest_signals\":" + (adopted ? 3 : 0) + "}");
  }
}
