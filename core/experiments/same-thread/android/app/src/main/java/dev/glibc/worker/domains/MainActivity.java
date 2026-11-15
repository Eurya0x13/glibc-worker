package dev.glibc.worker.domains;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;

/** Runs the disposable native fixture with application UID and inherited policy. */
public final class MainActivity extends Activity {
  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    TextView view = new TextView(this);
    view.setText("Running isolated domain compatibility test…");
    setContentView(view);
    new Thread(() -> runProbe(view), "domain-fixture-supervisor").start();
  }

  private void runProbe(TextView view) {
    File report = new File(getFilesDir(), "native-report.txt");
    String result;
    try {
      String nativePath = getApplicationInfo().nativeLibraryDir;
      ProcessBuilder builder = new ProcessBuilder("/system/bin/linker64",
          nativePath + "/libdomain_host.so", nativePath + "/libdomain_loader.so", nativePath,
          nativePath + "/libdomain_guest.so", nativePath + "/libdomain_probe.so");
      builder.environment().put("GW_DOMAIN_PRELOAD",
          nativePath + "/libdomain_libc.so:" + nativePath + "/libdomain_libm.so:" + nativePath
              + "/libdomain_gcc.so");
      builder.redirectErrorStream(true);
      builder.redirectOutput(report);
      Process process = builder.start();
      if (!process.waitFor(45, TimeUnit.SECONDS)) {
        process.destroyForcibly();
        process.waitFor();
        result = "APP_DOMAIN_RESULT=TIMEOUT\n";
      } else {
        result = "APP_DOMAIN_EXIT=" + process.exitValue() + "\n";
      }
      try (FileOutputStream out = new FileOutputStream(report, true)) {
        out.write(result.getBytes(StandardCharsets.UTF_8));
      }
      result = new String(Files.readAllBytes(report.toPath()), StandardCharsets.UTF_8);
    } catch (Exception error) {
      result = "APP_DOMAIN_RESULT=FAIL " + error + "\n";
      try (FileOutputStream out = new FileOutputStream(report, true)) {
        out.write(result.getBytes(StandardCharsets.UTF_8));
      } catch (Exception ignored) {
        // The UI still exposes the failure if report storage is unavailable.
      }
    }
    final String displayed = result;
    runOnUiThread(() -> view.setText(displayed));
  }
}
