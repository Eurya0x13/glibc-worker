package conformance;

public final class Module {
  public static void load(String path) {
    System.load(path);
  }

  public static void loadLibrary() {
    System.loadLibrary("guest_conformance");
  }

  public static native int group(int which, byte[] dynamicClass);
  public static native int bounceNative(int depth);
  public static native int tlsSet(int value);
  public static native int tlsGet();
  public static native int lockIncrement(Object lock, int[] count);
  public static native long mixed(long value, double scale, Object text);
  public static native void throwNative();
  public static native int namedOnly(int value);
  public static native int hostControl(int value);
  public static native int overloaded(int value);
  public static native long overloaded(long value);
  public static native int missingSymbol();
  public static native boolean echoZ(boolean value);
  public static native byte echoB(byte value);
  public static native char echoC(char value);
  public static native short echoS(short value);
  public static native int echoI(int value);
  public static native long echoJ(long value);
  public static native float echoF(float value);
  public static native double echoD(double value);
  public static native Object echoL(Object value);
  public native Object instanceEcho(Object value);
  public static final ThreadLocal<Integer> marker = new ThreadLocal<Integer>();

  public static int javaBounce(int depth) {
    if (marker.get() == null) {
      throw new AssertionError("Callback lost Java ThreadLocal");
    }
    return bounceNative(depth) + 1;
  }

  public static void gc() {
    System.gc();
  }
}
