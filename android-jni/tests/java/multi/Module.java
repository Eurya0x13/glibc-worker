package multi;
import java.util.function.IntUnaryOperator;

public final class Module {
  public static IntUnaryOperator peer;

  public static void load(String path) {
    System.load(path);
  }

  public static native int identity();
  public static native int named();
  public static native int cached(int value);
  public static native int add(int value);
  public static native int value();
  public static native long cookie();
  public static native int ownTls(int value);
  public static native int depTls(int value);
  public static native int route(int depth);
  public static native int registerTarget(Class<?> target, int kind);
  public static native int unregisterTarget(Class<?> target);

  public static int javaEcho(int value) {
    return value + 10000;
  }

  public static int javaRoute(int depth) {
    return peer.applyAsInt(depth);
  }
}
