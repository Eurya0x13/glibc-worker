public final class ManagedBoundary {
  private static final Object LOCK = new Object();

  static native void escape();
  static native void mark(int event);

  static void enter() {
    synchronized (LOCK) {
      try {
        escape();
      } finally {
        mark(2);
      }
    }
  }

  static void contend() {
    synchronized (LOCK) {
      mark(3);
    }
  }
}
