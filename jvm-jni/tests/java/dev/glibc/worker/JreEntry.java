package dev.glibc.worker;

/** HotSpot-only main entered by the Pojav-style JLI launcher, never Android DEX. */
public final class JreEntry {
  private static native int run();

  public static void main(String[] args) {
    System.out.println("JRE_JLI_MAIN");
    if (args.length != 1) {
      throw new IllegalArgumentException("Expected the native test harness path");
    }
    System.load(args[0]);
    if (run() != 0) {
      throw new AssertionError("JRE bridge validation failed");
    }
  }
}
