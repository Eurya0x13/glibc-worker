package dev.glibc.worker;

import android.content.Context;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

final class JreResources {
  static String prepare(Context context) throws Exception {
    File home = new File(context.getFilesDir(), "android-jre17");
    home.mkdirs();
    try (BufferedReader reader = new BufferedReader(new InputStreamReader(
             context.getAssets().open("android-jre/layout.tsv"), StandardCharsets.UTF_8))) {
      String line;
      while ((line = reader.readLine()) != null) {
        String[] parts = line.split("\t");
        File target = new File(home, parts[1]);
        target.getParentFile().mkdirs();
        if (parts[0].equals("L")) {
          // Actual executable inode belongs to the installed APK.
          Files.deleteIfExists(target.toPath());
          Os.symlink(context.getApplicationInfo().nativeLibraryDir + "/" + parts[2],
              target.getAbsolutePath());
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
      }
    }
    return home.getAbsolutePath();
  }
}
