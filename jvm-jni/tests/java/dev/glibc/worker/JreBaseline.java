package dev.glibc.worker;

/** Same launcher arguments, with no native harness or bridge loaded. */
public final class JreBaseline {
  public static void main(String[] args) {
    System.out.println("JRE_LAUNCHER_BASELINE_MAIN");
  }
}
