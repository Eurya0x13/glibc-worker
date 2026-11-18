/** Packaged separately so the VM can actually collect its owning ClassLoader. */
public final class SystemLoadFixture {
  public static void load(String path) {
    System.load(path);
  }

  public static void loadLibrary(String name) {
    System.loadLibrary(name);
  }

  public static int onLoadCallback() {
    return 73;
  }

  public static long reply(long value) {
    return value + 7;
  }

  public static int recurse(int depth) {
    return nested(depth);
  }

  public static native long callback(long value);
  public static native int nested(int depth);
  public static native int array(byte[] bytes);
  public static native long compute(long value);
  public static native int probeSignals();
  public static native int hold();
  public static native int holdState(int release);
  public static native long echo(long value);
  public static native int overload(int value);
  public static native double overload(double value);
  public static native long missing(long value);
}
