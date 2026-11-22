package dev.glibc.worker;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.concurrent.ConcurrentHashMap;

/** Shared helpers for reviewed JVM loader profiles. */
public final class JvmAdapter {
  private JvmAdapter() {}

  /**
   * Maps a backend handle to the Class that loaded the Guest library. Values are weak so an
   * unloadable ClassLoader can still be collected.
   */
  private static final ConcurrentHashMap<Long, WeakReference<Class<?>>> owners =
      new ConcurrentHashMap<>();

  /**
   * Records the owner of a freshly committed handle; called from native publish. A null owner
   * is rejected: later signature lookups resolve classes through the owner's loader.
   */
  public static void remember(long handle, Class<?> owner) {
    if (owner == null) {
      throw new IllegalArgumentException("Guest library has no caller class");
    }
    owners.put(handle, new WeakReference<Class<?>>(owner));
  }

  /** Drops the owner entry on unload or load rollback; later signature lookups then fail. */
  public static void forget(long handle) {
    owners.remove(handle);
  }

  /**
   * Native entry point: maps a handle back to its owner Class and reflects the JNI descriptor
   * for a symbol. Throws when the owner is unknown or already collected, so a lookup never
   * proceeds without a ClassLoader.
   */
  public static String nativeSignature(String symbol, long handle) throws Exception {
    WeakReference<Class<?>> reference = owners.get(handle);
    Class<?> owner = reference == null ? null : reference.get();
    if (owner == null) {
      throw new IllegalStateException("Guest library owner is unavailable");
    }
    return nativeSignature(symbol, owner);
  }

  /**
   * Length of the JNI escape sequence starting at i, or 0 for a plain character. Mangling and
   * unmangling share this grammar: _1/_2/_3 are two characters, _0xxxx is six.
   */
  private static int escapeLength(String s, int i) {
    if (i + 1 >= s.length() || s.charAt(i) != '_') {
      return 0;
    }
    char next = s.charAt(i + 1);
    if (next == '1' || next == '2' || next == '3') {
      return 2;
    }
    return next == '0' && i + 5 < s.length() ? 6 : 0;
  }

  /**
   * True only when the file's SHA256 equals the reviewed digest pinned by the profile; a
   * mismatch blocks the native install.
   */
  public static boolean verifyLibrary(String path, String expected) throws Exception {
    return expected.equals(GuestImages.sha256(new File(path)));
  }

  private static boolean verifyLibrary(byte[] path, String expected) throws Exception {
    return verifyLibrary(GuestImages.decodePath(path), expected);
  }

  private static byte[] encodePath(String path) {
    return GuestImages.encodePath(path);
  }

  private static UnsatisfiedLinkError loadError(byte[] message) {
    return new UnsatisfiedLinkError(GuestImages.decodePath(message));
  }

  /**
   * Decodes JNI symbol mangling: _1 is '_', _2 is ';', _3 is '[', _0xxxx is a UTF-16 unit and
   * a bare '_' is '/'. Inverse of the JVM's JNI name mangling.
   */
  private static String unmangle(String s) {
    StringBuilder b = new StringBuilder();

    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c != '_') {
        b.append(c);
        continue;
      }
      if (i + 1 < s.length()) {
        char n = s.charAt(i + 1);
        if (n == '1') {
          b.append('_');
          i++;
          continue;
        }
        if (n == '2') {
          b.append(';');
          i++;
          continue;
        }
        if (n == '3') {
          b.append('[');
          i++;
          continue;
        }
        if (n == '0' && i + 5 < s.length()) {
          b.append((char) Integer.parseInt(s.substring(i + 2, i + 6), 16));
          i += 5;
          continue;
        }
      }
      b.append('/');
    }

    return b.toString();
  }

  /**
   * Derives the full JNI descriptor of a Java_-prefixed export: loads the owner class in from's
   * loader and scans its native methods. Returns null when the symbol is no JNI export, no
   * native matches or the match is ambiguous; a missing owner class throws.
   */
  public static String nativeSignature(String symbol, Class<?> from) throws Exception {
    if (!symbol.startsWith("Java_")) {
      return null;
    }

    String body = symbol.substring(5), suffix = null;
    int overload = -1;

    for (int i = 0; i < body.length(); i++) {
      int escaped = escapeLength(body, i);
      if (escaped != 0) {
        i += escaped - 1;
        continue;
      }
      // Class separator + an escaped leading '_' is "__1", not an overload.
      if (body.charAt(i) == '_' && i + 1 < body.length() && body.charAt(i + 1) == '_'
          && escapeLength(body, i + 1) == 0) {
        overload = i;
        break;
      }
    }

    if (overload >= 0) {
      suffix = unmangle(body.substring(overload + 2));
      body = body.substring(0, overload);
    }

    int split = -1;
    for (int i = 0; i < body.length(); i++)
      if (body.charAt(i) == '_') {
        int escaped = escapeLength(body, i);
        if (escaped != 0) {
          i += escaped - 1;
        } else {
          split = i;
        }
      }
    if (split < 0) {
      return null;
    }

    Class<?> owner = Class.forName(
        unmangle(body.substring(0, split)).replace('/', '.'), false, from.getClassLoader());
    String name = unmangle(body.substring(split + 1));

    String found = null;
    for (java.lang.reflect.Method m : owner.getDeclaredMethods())
      if (m.getName().equals(name) && java.lang.reflect.Modifier.isNative(m.getModifiers())) {
        String sig = BridgeTypes.describeMember(m);
        if (suffix != null && !sig.substring(1, sig.indexOf(')')).equals(suffix)) {
          continue;
        }
        if (found != null) {
          return null;
        }
        found = sig;
      }

    return found;
  }
}
