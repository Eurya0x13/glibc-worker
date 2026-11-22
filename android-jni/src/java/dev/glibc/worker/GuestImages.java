package dev.glibc.worker;

import java.io.*;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.*;

/** Host-side image policy; the launcher explicitly chooses catalog or direct mode. */
public final class GuestImages {
  private GuestImages() {}

  private static final Charset PATH_CHARSET = platformPathCharset();

  private static Charset platformPathCharset() {
    // HotSpot's Unix file APIs use the native path encoding. Android uses UTF-8.
    String encoding = System.getProperty("sun.jnu.encoding");
    return encoding == null ? StandardCharsets.UTF_8 : Charset.forName(encoding);
  }

  static byte[] encodePath(String path) {
    return path.getBytes(PATH_CHARSET);
  }

  static String decodePath(byte[] path) {
    return new String(path, PATH_CHARSET);
  }

  /**
   * Immutable snapshot captured by configure; published once via the volatile field and never
   * mutated afterwards.
   */
  private static final class Configuration {
    final File directory, catalog;
    final boolean direct;

    Configuration(File directory, File catalog, boolean direct) {
      this.directory = directory;
      this.catalog = catalog;
      this.direct = direct;
    }
  }

  private static volatile Configuration configuration;

  /**
   * One-shot setup invoked from native code before any image load: canonicalizes both paths and
   * requires the installed directory (and, in catalog mode, the catalog file) to exist.
   */
  public static synchronized void configure(String directory, String catalog, boolean direct)
      throws IOException {
    if (configuration != null) {
      throw new IllegalStateException("Image policy already configured");
    }
    File dir = new File(directory).getCanonicalFile(), file = new File(catalog).getCanonicalFile();
    if (!dir.isDirectory() || (!direct && !file.isFile())) {
      throw new IOException("Image policy paths are unavailable");
    }
    configuration = new Configuration(dir, file, direct);
  }

  private static void configureNative(byte[] directory, byte[] catalog, boolean direct)
      throws IOException {
    configure(decodePath(directory), decodePath(catalog), direct);
  }

  /**
   * Streams the file through SHA-256 and returns lowercase hex; the identity used for
   * content-matched catalog lookups.
   */
  public static String sha256(File file) throws IOException {
    MessageDigest digest = newDigest();
    try (InputStream in = new FileInputStream(file)) {
      byte[] buffer = new byte[65536];
      int n;
      while ((n = in.read(buffer)) != -1) {
        digest.update(buffer, 0, n);
      }
    }
    return digestHex(digest);
  }

  static MessageDigest newDigest() {
    try {
      return MessageDigest.getInstance("SHA-256");
    } catch (NoSuchAlgorithmException e) {
      throw new AssertionError(e);
    }
  }

  static String digestHex(MessageDigest digest) {
    StringBuilder out = new StringBuilder();
    for (byte b : digest.digest()) {
      out.append(String.format(Locale.ROOT, "%02x", b & 255));
    }
    return out.toString();
  }

  /**
   * Routes one native image request: a file already inside the installed directory passes
   * through, direct mode accepts any path, and catalog mode additionally requires an exact
   * content-hash match. Rejections throw UnsatisfiedLinkError, which the native loader reports
   * as a load failure.
   */
  public static String resolve(String requested) {
    try {
      Configuration options = configuration;
      if (options == null) {
        throw new IOException("Image policy is not configured");
      }

      File file = new File(requested).getCanonicalFile();
      File installedDir = options.directory;
      if (installedDir.equals(file.getParentFile())) {
        return file.getPath();
      }
      if (options.direct) {
        System.out.println("GUEST_IMAGE_MAP disabled=1 requested=" + file);
        return file.getPath();
      }

      String hash = sha256(file);
      for (String row : Files.readAllLines(options.catalog.toPath())) {
        String[] fields = row.split("\t");
        if (fields.length != 3 || !fields[0].equals(hash)
            || Long.parseLong(fields[1]) != file.length()) {
          continue;
        }
        if (!fields[2].matches("lib[A-Za-z0-9_.-]+\\.so")) {
          throw new IOException("Invalid installed image name");
        }
        File image = new File(installedDir, fields[2]).getCanonicalFile();
        if (!installedDir.equals(image.getParentFile()) || image.length() != file.length()
            || !hash.equals(sha256(image))) {
          throw new IOException("Installed image does not match catalog");
        }
        System.out.println(
            "GUEST_IMAGE_MAP sha256=" + hash + " requested=" + file + " installed=" + image);
        return image.getPath();
      }
      throw new IOException("No installed Guest image matches content SHA256 " + hash);
    } catch (Exception e) {
      throw new UnsatisfiedLinkError("Guest image routing: " + e.getMessage());
    }
  }

  private static byte[] resolveNative(byte[] requested) {
    return encodePath(resolve(decodePath(requested)));
  }
}
