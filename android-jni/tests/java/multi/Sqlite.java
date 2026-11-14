package multi;

public final class Sqlite {
  public static void load(String path) {
    System.load(path);
  }

  public static native String version();
  public static native int exercise(String path);
}
