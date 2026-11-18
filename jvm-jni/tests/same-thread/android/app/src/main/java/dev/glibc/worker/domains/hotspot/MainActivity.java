package dev.glibc.worker.domains.hotspot;

import android.app.Activity;
import android.os.Bundle;
import android.system.Os;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;

/** App-UID supervisor; HotSpot and both libc domains live in one native child. */
public final class MainActivity extends Activity {
  private static native void runInline(String[] arguments, String report);

  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    TextView view = new TextView(this);
    view.setText("Preparing HotSpot domain compatibility test…");
    ScrollView scroll = new ScrollView(this);
    scroll.addView(view);
    setContentView(scroll);
    new Thread(() -> runProbe(view), "hotspot-domain-supervisor").start();
  }

  private void extract(String path) throws Exception {
    String[] children = getAssets().list(path);
    File destination = new File(getFilesDir(), path);
    if (children != null && children.length > 0) {
      if (!destination.isDirectory() && !destination.mkdirs()) {
        throw new IllegalStateException("Cannot create " + destination);
      }
      for (String child : children) {
        extract(path + "/" + child);
      }
    } else {
      try (InputStream in = getAssets().open(path);
          FileOutputStream out = new FileOutputStream(destination)) {
        byte[] buffer = new byte[65536];
        int count;
        while ((count = in.read(buffer)) != -1) {
          out.write(buffer, 0, count);
        }
      }
    }
  }

  private void runProbe(TextView view) {
    File report = new File(getFilesDir(), "native-report.txt");
    String result;
    try {
      extract("jre");
      extract("domain-probe.jar");
      String nativePath = getApplicationInfo().nativeLibraryDir;
      String files = getFilesDir().getAbsolutePath();
      boolean systemLoad = new File(nativePath, "libdomain_system_load.so").isFile();
      if (systemLoad) {
        extract("system-fixture.jar");
      }
      File server = new File(getFilesDir(), "jre/lib/server");
      if (!server.isDirectory() && !server.mkdirs()) {
        throw new IllegalStateException("Cannot create JRE server directory");
      }
      // Preserve the JRE's expected layout while executable files stay in
      // Android's extracted native-library directory.
      for (String name : new String[] {"jvm", "java", "verify", "net", "nio", "zip", "jimage"}) {
        String fileName = "lib" + name + ".so";
        File link =
            new File(getFilesDir(), "jre/lib/" + (name.equals("jvm") ? "server/" : "") + fileName);
        Files.deleteIfExists(link.toPath());
        Os.symlink(nativePath + "/" + fileName, link.getAbsolutePath());
      }
      File guestRuntime = new File(getFilesDir(), "guest-runtime");
      if (!guestRuntime.isDirectory() && !guestRuntime.mkdirs()) {
        throw new IllegalStateException("Cannot create Guest runtime directory");
      }
      // Standard SONAME paths avoid preloading DSOs before Guest ownership
      // setup. The executable mappings still refer to installed native files.
      for (String[] library : new String[][] {{"libc.so.6", "libdomain_libc.so"},
               {"libm.so.6", "libdomain_libm.so"}, {"libgcc_s.so.1", "libdomain_gcc.so"}}) {
        File link = new File(guestRuntime, library[0]);
        Files.deleteIfExists(link.toPath());
        Os.symlink(nativePath + "/" + library[1], link.getAbsolutePath());
      }
      ProcessBuilder builder =
          new ProcessBuilder("/system/bin/linker64", nativePath + "/libdomain_host.so",
              nativePath + "/libdomain_loader.so", guestRuntime.getAbsolutePath(),
              nativePath + "/libdomain_guest.so", nativePath + "/libdomain_probe.so");
      builder.directory(getFilesDir());
      builder.environment().remove("GW_DOMAIN_PRELOAD");
      // Used only by the production-JNI experiment executable. The original
      // domain fixture ignores these settings.
      builder.environment().put("GW_CONFORMANCE", nativePath + "/libdomain_conformance.so");
      // Exercise the native entrypoint's default, rather than force it here.
      // Timed A/B runs below continue to select their backend explicitly.
      builder.environment().remove("GW_DOMAIN_MODE");
      builder.environment().put("GW_THREADS", "8");
      builder.environment().put("GW_BENCH", "0");
      // Explicit experiment-only launch options; one fresh process per sample.
      if (getIntent().getBooleanExtra("benchmark", false)) {
        String mode = getIntent().getStringExtra("backend");
        int threads = getIntent().getIntExtra("threads", 1);
        if ((!"worker".equals(mode) && !"domain".equals(mode) && !"direct".equals(mode))
            || (threads != 1 && threads != 8)) {
          throw new IllegalArgumentException("Invalid benchmark configuration");
        }
        builder.environment().put("GW_BENCH", "1");
        builder.environment().put("GW_THREADS", Integer.toString(threads));
        builder.environment().put("GW_DOMAIN_MODE", "domain".equals(mode) ? "domain" : "worker");
        builder.environment().put("GW_BENCH_DIRECT", "direct".equals(mode) ? "1" : "0");
      }
      builder.environment().put("GW_JAVA_HOME", files + "/jre");
      builder.environment().put("GW_JAVA_BOOT_LIB", nativePath);
      builder.environment().put("GW_JVM_LIB", nativePath + "/libjvm.so");
      builder.environment().put("GW_DOMAIN_JAR", files + "/domain-probe.jar");
      builder.environment().put("LD_LIBRARY_PATH", nativePath);
      if (systemLoad) {
        builder.environment().put("GW_SYSTEM_JAR", files + "/system-fixture.jar");
        builder.environment().put("GW_SYSTEM_IMAGE", nativePath + "/libdomain_system_load.so");
        builder.environment().put("GW_SYSTEM_REJECT", nativePath + "/libdomain_system_reject.so");
        builder.environment().put(
            "GW_SYSTEM_ORDINARY", nativePath + "/libdomain_system_ordinary.so");
      }
      if (new File(nativePath, "libdomain_late_bridge.so").isFile()) {
        builder.environment().put("GW_LATE_BRIDGE", nativePath + "/libdomain_late_bridge.so");
        if (new File(getFilesDir(), "inline-vm.flag").isFile()) {
          for (java.util.Map.Entry<String, String> setting : builder.environment().entrySet()) {
            if (setting.getKey().startsWith("GW_") || setting.getKey().equals("LD_LIBRARY_PATH")) {
              Os.setenv(setting.getKey(), setting.getValue(), true);
            }
          }
          if (!builder.environment().containsKey("GW_DOMAIN_MODE")) {
            Os.unsetenv("GW_DOMAIN_MODE");
          }
          System.loadLibrary("domain_inline_host");
          runInline(builder.command().subList(1, builder.command().size()).toArray(new String[0]),
              report.getAbsolutePath());
          throw new IllegalStateException("Inline VM unexpectedly returned");
        }
      }
      builder.redirectErrorStream(true);
      builder.redirectOutput(report);
      Process process = builder.start();
      if (!process.waitFor(120, TimeUnit.SECONDS)) {
        process.destroyForcibly();
        process.waitFor();
        result = "APP_DOMAIN_RESULT=TIMEOUT\n";
      } else {
        result = "APP_DOMAIN_EXIT=" + process.exitValue() + "\n";
      }
      result += "APP_DOMAIN_SUPERVISOR_UID=" + Os.getuid() + "\n";
      try (FileOutputStream out = new FileOutputStream(report, true)) {
        out.write(result.getBytes(StandardCharsets.UTF_8));
      }
      result = new String(Files.readAllBytes(report.toPath()), StandardCharsets.UTF_8);
      if (result.contains("APP_DOMAIN_EXIT=0")) {
        StringBuilder summary =
            new StringBuilder("Compatibility fixture only; performance not measured.\n\n");
        for (String line : result.split("\n")) {
          if (line.startsWith("HOTSPOT_") || line.startsWith("JNI_DOMAIN_")
              || line.startsWith("SYSTEM_LOAD_") || line.startsWith("{")
              || line.startsWith("Warning:") || line.startsWith("APP_DOMAIN_")) {
            summary.append(line).append("\n\n");
          }
        }
        result = summary.toString();
      }
    } catch (Exception error) {
      result = "APP_DOMAIN_RESULT=FAIL " + error + "\n";
      try (FileOutputStream out = new FileOutputStream(report, true)) {
        out.write(result.getBytes(StandardCharsets.UTF_8));
      } catch (Exception ignored) {
        // Still expose the error in the UI if report storage failed.
      }
    }
    final String displayed = result;
    runOnUiThread(() -> view.setText(displayed));
  }
}
