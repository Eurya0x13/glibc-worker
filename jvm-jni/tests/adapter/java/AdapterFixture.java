public final class AdapterFixture {
  public static void load(String path) {
    System.load(path);
  }
  public static int onLoadCallback() {
    return 73;
  }
  public static native long echo(long value);
  public static native int overload(int value);
  public static native double overload(double value);
  public static native long missing(long value);
}
