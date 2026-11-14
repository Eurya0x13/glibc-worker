package dev.glibc.worker;

public final class NativeBridge {
  static {
    System.loadLibrary("worker_host");
  }
  public static native void restrictCpuMask(long mask);
  public static native String run(
      String nativeDir, String reportPath, String jreHome, boolean smokeOnly);
}
