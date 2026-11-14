package dev.glibc.worker;

/** Compatibility entry points for existing JRE17 helper clients. */
public final class Jre17Adapter {
  private Jre17Adapter() {}

  public static boolean verifyLibrary(String path, String expected) throws Exception {
    return JvmAdapter.verifyLibrary(path, expected);
  }

  public static String nativeSignature(String symbol, Class<?> owner) throws Exception {
    return JvmAdapter.nativeSignature(symbol, owner);
  }
}
