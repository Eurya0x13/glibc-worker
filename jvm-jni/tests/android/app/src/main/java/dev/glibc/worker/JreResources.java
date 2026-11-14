package dev.glibc.worker;

import android.content.Context;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryNotEmptyException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Comparator;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import org.json.JSONObject;

final class JreResources {
  /** Remove empty managed directories too: even an empty JRE8 lib/ext rejects JRE17. */
  private static void pruneEmptyDirectories(File home) throws IOException {
    for (String name : new String[] {"bin", "lib", "conf", "legal"}) {
      Path root = new File(home, name).toPath();
      if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
        continue;
      }
      Path[] directories;
      try (Stream<Path> paths = Files.walk(root)) {
        directories = paths.filter(path -> Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS))
                          .sorted(Comparator.reverseOrder())
                          .toArray(Path[] ::new);
      }
      for (Path path : directories) {
        try {
          Files.delete(path);
        } catch (DirectoryNotEmptyException retained) {
          // Keep unmanaged files; only empty directory structure is disposable.
        }
      }
    }
  }

  private static JSONObject readArtifact(Context context) throws Exception {
    try (InputStream in = context.getAssets().open("android-jre/jre-artifact.json");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
      byte[] buffer = new byte[4096];
      int n;
      while ((n = in.read(buffer)) != -1) {
        bytes.write(buffer, 0, n);
      }
      return new JSONObject(bytes.toString("UTF-8"));
    }
  }

  private static int versionProcess(ProcessBuilder command, StringBuilder log) throws Exception {
    command.redirectErrorStream(true);
    Process process = command.start();
    if (!process.waitFor(30, TimeUnit.SECONDS)) {
      process.destroyForcibly();
      throw new IOException("JRE launcher version probe timed out");
    }
    try (BufferedReader reader = new BufferedReader(
             new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
      String line;
      while ((line = reader.readLine()) != null) {
        if (log.length() + line.length() > 65536) {
          throw new IOException("Oversized JRE version probe output");
        }
        log.append(line).append('\n');
      }
    }
    return process.exitValue();
  }

  /** Subprocess launch probe only; bridge calls still run in this app's process. */
  private static void probeLauncher(Context context, File home, JSONObject artifact)
      throws Exception {
    String executable = new File(home, "bin/java").getAbsolutePath();
    String base = new File(home, artifact.getString("boot_library_path")).getAbsolutePath();
    JSONObject result = new JSONObject();
    result.put("artifact", artifact.getString("id"));
    result.put("mode", "linker64");
    StringBuilder log = new StringBuilder();
    String search = base + "/server:" + base + "/jli:" + base + ":"
        + context.getApplicationInfo().nativeLibraryDir;
    ProcessBuilder direct = new ProcessBuilder(executable, "-version");
    direct.environment().put("JAVA_HOME", home.getAbsolutePath());
    direct.environment().put("LD_LIBRARY_PATH", search);
    if ("arm64-v8a".equals(artifact.getString("abi"))) {
      direct.environment().put(
          "LD_PRELOAD", context.getApplicationInfo().nativeLibraryDir + "/libjre_exec_compat.so");
    }
    try {
      int exit = versionProcess(direct, log);
      result.put("direct_started", true);
      result.put("direct_exit", exit);
    } catch (IOException refused) {
      result.put("direct_started", false);
      result.put("direct_exit", -1);
      log.append(refused.toString()).append('\n');
    }
    ProcessBuilder proxy = new ProcessBuilder("/system/bin/linker64", executable, "-version");
    proxy.environment().putAll(direct.environment());
    result.put("proxy_exit", versionProcess(proxy, log));
    Files.write(new File(home, "launcher-probe.log").toPath(),
        log.toString().getBytes(StandardCharsets.UTF_8));
    Files.write(new File(home, "launcher-probe.json").toPath(),
        result.toString().getBytes(StandardCharsets.UTF_8));
  }

  static String prepare(Context context) throws Exception {
    File home = new File(context.getFilesDir(), "android-jre");
    home.mkdirs();
    JSONObject packagedArtifact = readArtifact(context);
    boolean privateLayout = "private-readonly".equals(packagedArtifact.optString("deployment"));
    File priorLayout = new File(home, ".runtime-layout.tsv");
    if (priorLayout.isFile()) {
      for (String row : Files.readAllLines(priorLayout.toPath(), StandardCharsets.UTF_8)) {
        String[] parts = row.split("\\t");
        File previous = new File(home, parts[1]);
        if (!previous.toPath().normalize().startsWith(home.toPath())) {
          throw new IOException("Invalid previous runtime layout");
        }
        Files.deleteIfExists(previous.toPath());
      }
    }
    pruneEmptyDirectories(home);
    try (BufferedReader reader = new BufferedReader(new InputStreamReader(
             context.getAssets().open("android-jre/layout.tsv"), StandardCharsets.UTF_8))) {
      String line;
      while ((line = reader.readLine()) != null) {
        String[] parts = line.split("\t");
        File target = new File(home, parts[1]);
        target.getParentFile().mkdirs();
        // A prior interrupted deployment may not have published its manifest.
        // Replace each managed inode instead of truncating a read-only file or
        // following a stale symlink into another runtime.
        Files.deleteIfExists(target.toPath());
        if (parts[0].equals("L")) {
          String installed = context.getApplicationInfo().nativeLibraryDir + "/" + parts[2];
          if (privateLayout) {
            try (InputStream in = new FileInputStream(installed);
                OutputStream out = new FileOutputStream(target)) {
              // Open first, then make immutable before filling the new inode.
              Os.chmod(target.getAbsolutePath(), 0444);
              byte[] buffer = new byte[65536];
              int n;
              while ((n = in.read(buffer)) != -1) {
                out.write(buffer, 0, n);
              }
            }
          } else {
            Os.symlink(installed, target.getAbsolutePath());
          }
        } else {
          try (InputStream in = context.getAssets().open("android-jre/" + parts[1]);
              OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) != -1) {
              out.write(buffer, 0, n);
            }
          }
        }
        if (parts[1].startsWith("bin/")) {
          Os.chmod(target.getAbsolutePath(), 0500);
        }
      }
    }
    try (InputStream in = context.getAssets().open("android-jre/layout.tsv")) {
      Files.copy(in, priorLayout.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
    JSONObject artifact = new JSONObject(new String(
        Files.readAllBytes(new File(home, "jre-artifact.json").toPath()), StandardCharsets.UTF_8));
    for (String name : new String[] {"libjava", "libjvm"}) {
      JSONObject image = artifact.getJSONObject(name);
      MessageDigest sha = MessageDigest.getInstance("SHA-256");
      try (InputStream in = new FileInputStream(new File(home, image.getString("path")))) {
        byte[] buffer = new byte[65536];
        int n;
        while ((n = in.read(buffer)) != -1) {
          sha.update(buffer, 0, n);
        }
      }
      StringBuilder hex = new StringBuilder();
      for (byte b : sha.digest()) {
        hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
      }
      if (!hex.toString().equals(image.getString("sha256"))) {
        throw new IOException("Installed JRE artifact mismatch: " + name);
      }
    }
    if (privateLayout) {
      probeLauncher(context, home, artifact);
    }
    return home.getAbsolutePath();
  }
}
