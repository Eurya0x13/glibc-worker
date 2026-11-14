/** Isolate the VM's JNI_COMMIT behavior without loading glibc-worker. */
public final class ArrayCommitControl {
  private static native int check();

  public static void main(String[] args) {
    System.load(args[0]);
    if (check() != 0) {
      throw new AssertionError("Array commit round trip failed");
    }
    System.out.println("ARRAY_COMMIT_CONTROL result=PASS");
  }
}
