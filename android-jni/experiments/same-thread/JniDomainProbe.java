import java.util.Arrays;

public final class JniDomainProbe {
  static native int echo(int value);
  static native int callback(int value);
  static native int nested(int depth);
  static native Object identity(Object value);
  static native int array(byte[] value);
  static native String string(String value);
  static native void throwing();
  static native int vmCheck();
  static native void pauseGuest();
  static native int pauseStatus();
  static native int guestThreads();
  static volatile int unloads;
  static volatile long sink;

  private static Object startRecording() throws Exception {
    Class<?> type = Class.forName("jdk.jfr.Recording");
    Object recording = type.getConstructor().newInstance();
    java.util.Map<String, String> settings = new java.util.HashMap<>();
    settings.put("jdk.ExecutionSample#enabled", "true");
    settings.put("jdk.ExecutionSample#period", "1 ms");
    settings.put("jdk.NativeMethodSample#enabled", "true");
    settings.put("jdk.NativeMethodSample#period", "1 ms");
    type.getMethod("setSettings", java.util.Map.class).invoke(recording, settings);
    type.getMethod("setToDisk", boolean.class).invoke(recording, false);
    type.getMethod("start").invoke(recording);
    return recording;
  }

  static int reply(int value) {
    return value ^ 0x13579;
  }

  static int recurse(int depth) {
    return nested(depth);
  }

  static void unloaded() {
    ++unloads;
  }

  static void require(boolean value) {
    if (!value) {
      throw new AssertionError("JNI domain contract");
    }
  }

  private static int call(int kind, int value, byte[] bytes) {
    switch (kind) {
      case 0:
        return echo(value);
      case 1:
        return callback(value);
      case 2:
        return nested(4);
      default:
        return array(bytes);
    }
  }

  public static int run(int thread, boolean measure) {
    if (!measure && thread == 0) {
      require(guestThreads() == 32);
      Object recording;
      try {
        recording = startRecording();
      } catch (Exception error) {
        throw new AssertionError("JFR start", error);
      }
      Thread blocked = new Thread(JniDomainProbe::pauseGuest, "domain-blocked-native");
      blocked.start();
      long deadline = System.nanoTime() + 10000000000L;
      while (pauseStatus() == 0 && System.nanoTime() < deadline) {
        Thread.yield();
      }
      require(pauseStatus() == 1);
      int overlaps = 0;
      for (int sample = 0; sample < 20; ++sample) {
        if (pauseStatus() == 1) {
          ++overlaps;
        }
        require(!Thread.getAllStackTraces().isEmpty());
        System.gc();
      }
      try {
        blocked.join(10000);
      } catch (InterruptedException error) {
        throw new AssertionError(error);
      }
      require(!blocked.isAlive());
      require(overlaps > 0 && pauseStatus() == 2);
      // Exercise transitions between sampled Java frames and native domains.
      long until = System.nanoTime() + 500000000L;
      long sum = 0;
      while (System.nanoTime() < until) {
        for (int i = 0; i < 1000; ++i) {
          sum = (sum * 1664525 + i) ^ (sum >>> 7);
        }
        sum += callback((int) sum);
      }
      sink = sum;
      try {
        recording.getClass().getMethod("stop").invoke(recording);
        recording.getClass().getMethod("close").invoke(recording);
      } catch (Exception error) {
        throw new AssertionError("JFR stop", error);
      }
      System.out.println("JNI_DOMAIN_VM_COOP {\"samples\":20,\"overlaps\":" + overlaps + "}");
    }
    require(vmCheck() == 1);
    Object object = new Object();
    require(identity(object) == object);
    String text = "JNI-中文-\u0000-\uD83D\uDE00";
    require(text.equals(string(text)));
    for (int i = 0; i < 100; ++i) {
      require(echo(i) == reply(i) && callback(i) == reply(i));
      require(nested(8) == 8);
      try {
        throwing();
        throw new AssertionError("missing exception");
      } catch (IllegalStateException expected) {
        require("domain-probe".equals(expected.getMessage()));
      }
    }
    String[] cases = {"scalar", "callback", "nested4", "array128", "array65536"};
    for (int kind = 0; kind < cases.length; ++kind) {
      byte[] bytes = new byte[kind == 4 ? 65536 : 128];
      Arrays.fill(bytes, (byte) 7);
      require(array(bytes) == bytes.length * 7);
      if (!measure) {
        continue;
      }
      int iterations = kind == 4 ? 64 : 256;
      long checksum = 0;
      for (int i = 0; i < iterations * 8; ++i) {
        checksum += call(kind, i, bytes);
      }
      StringBuilder values = new StringBuilder();
      for (int batch = 0; batch < 30; ++batch) {
        long start = System.nanoTime();
        for (int i = 0; i < iterations; ++i) {
          checksum += call(kind, i, bytes);
        }
        long elapsed = System.nanoTime() - start;
        if (batch != 0) {
          values.append(',');
        }
        values.append(elapsed);
      }
      sink = checksum;
      System.out.println("JNI_DOMAIN_BENCH {\"thread\":" + thread + ",\"case\":\"" + cases[kind]
          + "\",\"iterations\":" + iterations + ",\"checksum\":" + checksum + ",\"batch_ns\":["
          + values + "]}");
    }
    return 1;
  }
}
