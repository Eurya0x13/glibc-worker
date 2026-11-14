package dev.glibc.worker;

/** Compiled to a class/JAR for HotSpot only; never put in Android DEX. */
public final class NativeBridge {
  public static native int bridgeMetric(int which);
  public static native long bridgeLastHandle();
  public static native int bridgeHandleState(long handle);
  public static native long processCpuNanos();
  public static native void payloadPhase(int stage, int operation, int bytes);
  private static Thread owner;
  private static final ThreadLocal<Integer> marker = new ThreadLocal<Integer>();

  public static int verifyVm() {
    owner = Thread.currentThread();
    marker.set(913);
    System.out.println("JRE_IDENTITY java.vm.name=" + System.getProperty("java.vm.name"));
    System.out.println("JRE_IDENTITY java.version=" + System.getProperty("java.version"));
    System.out.println("JRE_IDENTITY java.home=" + System.getProperty("java.home"));
    System.out.println("JRE_IDENTITY classloader=" + NativeBridge.class.getClassLoader());
    return System.getProperty("java.vm.name", "").contains("OpenJDK") ? 17 : -1;
  }

  private static void checkThread() {
    if (Thread.currentThread() != owner || marker.get() != 913) {
      throw new AssertionError("Wrong HotSpot callback thread or ThreadLocal");
    }
  }

  public static int callback(int value) {
    checkThread();
    System.out.println("JRE_POD_CALLBACK value=" + value);
    return value * 2;
  }

  public static int jniCallback(int value) {
    checkThread();
    System.out.println("JRE_GUEST_JNI value=" + value);
    return value * 3;
  }

  public static int jniThrowing(int value) {
    checkThread();
    System.out.println("JRE_GUEST_JNI_THROW value=" + value);
    throw new IllegalStateException("Expected HotSpot Guest JNI exception " + value);
  }

  public static int afterGuest() {
    checkThread();
    System.gc();
    System.out.println("JRE_JAVA_RESUMED thread=" + Thread.currentThread().getName());
    return 17;
  }
}
