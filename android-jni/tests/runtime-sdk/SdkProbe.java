import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class SdkProbe {
  static native int probe(int value, byte[] data);
  static native long tid();
  static native int workerSignals();
  static native void prepareAdoptedSignal(long address);
  static native int finishAdoptedSignal();
  static native int jumpAdoptedSignal(long address);
  static native int hostNested();
  static native int hostMask();
  static native int hostMaskBlocked();
  static native int hostStack();
  static int reply(int value) {
    return value + 9;
  }

  static int observeAdoptedSignal(ByteBuffer shared) {
    shared.order(ByteOrder.nativeOrder()).putInt(0, 1);
    long end = System.nanoTime() + 5_000_000_000L;
    int result;
    while ((result = shared.getInt(4)) == 0 && System.nanoTime() < end) {
      Thread.yield();
    }
    return result;
  }
}
