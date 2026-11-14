package dev.glibc.worker;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.ResultReceiver;
import android.util.Log;
import java.io.File;

public final class ProbeService extends Service {
  private boolean started;

  @Override
  public IBinder onBind(Intent intent) {
    return new Binder();
  }

  @Override
  public int onStartCommand(Intent intent, int flags, int startId) {
    if (started || intent == null) {
      return START_NOT_STICKY;
    }
    started = true;
    ResultReceiver receiver = intent.getParcelableExtra("receiver");
    boolean smokeOnly = intent.getBooleanExtra("smoke-only", false);
    new Thread(() -> {
      String report;
      try {
        String cpuMask = intent.getStringExtra("cpu-mask");
        if (cpuMask != null) {
          NativeBridge.restrictCpuMask(Long.parseUnsignedLong(cpuMask, 16));
        }
        String jreHome = JreResources.prepare(this);
        File benchmark = new File(jreHome, "benchmark-enabled");
        if (intent.getBooleanExtra("benchmark", false)) {
          benchmark.createNewFile();
        } else {
          benchmark.delete();
        }
        File benchmarkJit = new File(jreHome, "benchmark-jit-enabled");
        String payloadMode = intent.getStringExtra("payload-benchmark");
        int payloadRound = intent.getIntExtra("payload-round", 0);
        File payload = new File(jreHome, "payload-benchmark-mode");
        if (payloadMode != null) {
          if (smokeOnly || intent.getBooleanExtra("benchmark", false)
              || !java.util.Arrays.asList("host", "guest").contains(payloadMode)
              || payloadRound < 0) {
            throw new IllegalArgumentException("Invalid payload benchmark mode");
          }
          java.nio.file.Files.write(payload.toPath(),
              (payloadMode + " " + payloadRound).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } else {
          java.nio.file.Files.deleteIfExists(payload.toPath());
        }
        if (intent.getBooleanExtra("benchmark-jit", false)) {
          if ((!intent.getBooleanExtra("benchmark", false) && payloadMode == null) || smokeOnly) {
            throw new IllegalArgumentException("Normal JIT requires the coverage benchmark");
          }
          benchmarkJit.createNewFile();
        } else {
          java.nio.file.Files.deleteIfExists(benchmarkJit.toPath());
        }
        File selection = new File(jreHome, "native-jar-selected");
        String nativeJar = intent.getStringExtra("native-jar");
        if (nativeJar != null && !smokeOnly) {
          if (!java.util.Arrays.asList("lz4", "snappy", "rocksdb").contains(nativeJar)) {
            throw new IllegalArgumentException("Unknown native JAR: " + nativeJar);
          }
          java.nio.file.Files.write(
              selection.toPath(), nativeJar.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } else {
          java.nio.file.Files.deleteIfExists(selection.toPath());
        }
        report = NativeBridge.run(getApplicationInfo().nativeLibraryDir,
            new File(getFilesDir(), "native-report.txt").getAbsolutePath(), jreHome, smokeOnly);
        report += "\nJAVA_ART_RESUMED pid=" + Process.myPid() + " uid=" + Process.myUid()
            + " tid=" + Process.myTid() + "\n";
        Log.i("GlibcWorker", "JAVA_ART_RESUMED pid=" + Process.myPid());
      } catch (Throwable t) {
        Log.e("GlibcWorker", "Probe Java failure", t);
        report = "APP_RESULT=FAIL\n" + t;
      }
      boolean pass = report.contains("APP_RESULT=PASS") && report.contains("JAVA_ART_RESUMED");
      Bundle result = new Bundle();
      result.putString("report", report);
      if (receiver != null) {
        receiver.send(pass ? 0 : 1, result);
      }
      try {
        Thread.sleep(300);
      } catch (InterruptedException ignored) {
      }
      // One Guest bootstrap per process. Do not pretend to support unloading.
      Process.killProcess(Process.myPid());
    }, "probe-host").start();
    return START_NOT_STICKY;
  }
}
