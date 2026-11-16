public final class SdkProbe {
  static native int probe(int value, byte[] data);
  static native long tid();
  static native int hostNested();
  static native int hostMask();
  static native int hostMaskBlocked();
  static native int hostStack();
  static int reply(int value) {
    return value + 9;
  }
}
