package dev.glibc.worker;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.LinkedHashMap;
import java.util.Map;

/** Prepares GNU loader search entries for the Guest libraries installed with the app. */
public final class GuestLibraries {
  private GuestLibraries() {}

  /**
   * Verifies bundled Guest libraries against the matching glibc-libs.tsv asset and closes the
   * stream. The return value is a directory under privateStorage for use in the Guest library path.
   * nativeDirectory is the installed nativeLibraryDir (or the native files in a desktop package).
   */
  public static File prepare(File privateStorage, File nativeDirectory, InputStream catalog)
      throws IOException {
    Map<String, Path> aliases = new LinkedHashMap<>();
    MessageDigest key = GuestImages.newDigest();
    try (BufferedReader rows =
             new BufferedReader(new InputStreamReader(catalog, StandardCharsets.US_ASCII))) {
      Path installed = nativeDirectory.toPath().toRealPath();
      key.update(installed.toString().getBytes(StandardCharsets.UTF_8));
      key.update((byte) 0);

      String row;
      while ((row = rows.readLine()) != null) {
        parseAlias(row, installed, aliases);
        key.update(row.getBytes(StandardCharsets.US_ASCII));
        key.update((byte) '\n');
      }
    }
    if (aliases.isEmpty()) {
      throw new IOException("Empty Guest library catalog");
    }

    Path storage = Files.createDirectories(privateStorage.toPath().toAbsolutePath());
    Path directory = storage.resolve("glibc-libs-" + GuestImages.digestHex(key));
    prepareLinks(directory, aliases);
    return directory.toFile();
  }

  private static void parseAlias(String row, Path installed, Map<String, Path> aliases)
      throws IOException {
    String[] fields = row.split("\t", -1);
    if (fields.length != 3 || !fields[0].matches("(lib|ld-linux)[A-Za-z0-9_.+-]+\\.so(\\.[0-9]+)*")
        || !fields[1].matches("libguest_[a-z0-9_]+\\.so") || !fields[2].matches("[0-9a-f]{64}")) {
      throw new IOException("Invalid Guest library catalog entry: " + row);
    }
    if (aliases.containsKey(fields[0])) {
      throw new IOException("Conflicting Guest library alias: " + fields[0]);
    }

    Path image = installed.resolve(fields[1]);
    if (!Files.isRegularFile(image, LinkOption.NOFOLLOW_LINKS)
        || !fields[2].equals(GuestImages.sha256(image.toFile()))) {
      throw new IOException("Installed Guest library does not match catalog: " + image);
    }
    aliases.put(fields[0], image);
  }

  private static void prepareLinks(Path directory, Map<String, Path> aliases) throws IOException {
    try {
      Files.createDirectory(directory);
    } catch (FileAlreadyExistsException existing) {
      if (!Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)) {
        throw existing;
      }
    }

    // Concurrent startups publish the same links. A later call can finish interrupted staging.
    for (Map.Entry<String, Path> alias : aliases.entrySet()) {
      Path link = directory.resolve(alias.getKey());
      Path target = alias.getValue();
      try {
        Files.createSymbolicLink(link, target);
      } catch (FileAlreadyExistsException existing) {
        if (!Files.isSymbolicLink(link) || !target.equals(Files.readSymbolicLink(link))) {
          throw new IOException("Conflicting Guest library alias: " + link, existing);
        }
      }
    }

    try (DirectoryStream<Path> contents = Files.newDirectoryStream(directory)) {
      for (Path entry : contents) {
        if (!aliases.containsKey(entry.getFileName().toString())) {
          throw new IOException("Unexpected Guest library alias entry: " + entry);
        }
      }
    }
  }
}
