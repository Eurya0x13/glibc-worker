package dev.glibc.worker;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.ResultReceiver;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.nio.file.Files;

public final class MainActivity extends Activity {
  private TextView status, details;
  private Button run, smoke;
  private boolean running;
  private int generation;
  private ServiceConnection connection;
  private final Handler handler = new Handler(Looper.getMainLooper());

  @Override
  public void onCreate(Bundle saved) {
    super.onCreate(saved);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    root.setPadding(40, 70, 40, 40);
    root.setBackgroundColor(Color.rgb(246, 248, 251));

    TextView title = new TextView(this);
    title.setText("glibc Worker");
    title.setTextSize(30);
    title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
    root.addView(title);

    TextView subtitle = new TextView(this);
    subtitle.setText("普通应用环境 · " + android.os.Build.SUPPORTED_ABIS[0]
        + "\nAndroid JRE / HotSpot 桥接验证");
    subtitle.setTextSize(16);
    subtitle.setPadding(0, 20, 0, 24);
    root.addView(subtitle);

    status = new TextView(this);
    status.setText("准备就绪");
    status.setTextSize(22);
    root.addView(status);

    run = new Button(this);
    run.setText("运行覆盖测试");
    root.addView(run);
    smoke = new Button(this);
    smoke.setText("运行冒烟测试");
    root.addView(smoke);

    TextView scope = new TextView(this);
    scope.setText(
        "Guest JNI 目标为打包的 Android JRE / HotSpot。ART 仅作应用入口；JNI 仍为实验子集。");
    scope.setPadding(0, 16, 0, 16);
    root.addView(scope);

    ScrollView scroll = new ScrollView(this);
    details = new TextView(this);
    details.setTextIsSelectable(true);
    details.setTextSize(12);
    details.setTypeface(Typeface.MONOSPACE);
    details.setText("应用 UID: " + Process.myUid()
        + "\n点击运行后显示原始结果。\n每次实验使用新的应用测试进程。");
    scroll.addView(details);
    root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

    setContentView(root);
    run.setOnClickListener(v -> startProbe(false));
    smoke.setOnClickListener(v -> startProbe(true));
  }

  private void startProbe(boolean smokeOnly) {
    if (running) {
      return;
    }

    int token = ++generation;
    running = true;
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    run.setEnabled(false);
    smoke.setEnabled(false);
    status.setText(smokeOnly ? "冒烟测试中…" : "覆盖测试中…");
    status.setTextColor(Color.DKGRAY);

    File nativeReport = new File(getFilesDir(), "native-report.txt");
    if (nativeReport.exists()) {
      nativeReport.delete();
    }

    ResultReceiver receiver = new ResultReceiver(handler) {
      @Override
      protected void onReceiveResult(int code, Bundle data) {
        if (!running) {
          return;
        }
        String report = data.getString("report", "APP_RESULT=FAIL\nNo report");
        if (generation != token) {
          return;
        }
        finishProbe(code == 0, report);
      }
    };

    Intent service =
        new Intent(this, ProbeService.class)
            .putExtra("receiver", receiver)
            .putExtra("smoke-only", smokeOnly)
            .putExtra("cpu-mask", getIntent().getStringExtra("cpu-mask"))
            .putExtra("native-jar", getIntent().getStringExtra("native-jar"))
            .putExtra("benchmark", getIntent().getBooleanExtra("benchmark", false))
            .putExtra("payload-benchmark", getIntent().getStringExtra("payload-benchmark"))
            .putExtra("payload-round", getIntent().getIntExtra("payload-round", 0))
            .putExtra("benchmark-jit", getIntent().getBooleanExtra("benchmark-jit", false));

    // Keep the disposable process in the visible Activity's scheduling group.
    // A started-only service is background work, unlike a foreground game JVM.
    connection = new ServiceConnection() {
      @Override
      public void onServiceConnected(ComponentName name, IBinder binder) {
        if (generation == token && running) {
          startService(service);
        }
      }
      @Override
      public void onServiceDisconnected(ComponentName name) {
        // Allow an already sent ResultReceiver to arrive before classifying death.
        handler.postDelayed(() -> {
          if (running && generation == token) {
            finishProbe(false,
                "APP_RESULT=FAIL\nTest process exited before returning its result.\n"
                    + readNativeReport(nativeReport));
          }
        }, 1000);
      }
    };

    if (!bindService(
            service, connection, BIND_AUTO_CREATE | BIND_IMPORTANT | BIND_ADJUST_WITH_ACTIVITY)) {
      connection = null;
      finishProbe(false, "APP_RESULT=FAIL\nCannot bind test process\n");
      return;
    }
    long deadline = SystemClock.uptimeMillis() + 600000;
    handler.postDelayed(new Runnable() {
      @Override
      public void run() {
        if (!running || generation != token) {
          return;
        }
        String partial = readNativeReport(nativeReport);
        if (partial.contains("APP_RESULT=BLOCKED")) {
          finishProbe(false, partial);
        } else if (partial.contains("JNI_EXPECTED_FATAL stage=host-forward")
            && partial.contains("FATAL ERROR in native method: fixture requested fatal")) {
          finishProbe(false, "APP_RESULT=EXPECTED_FATAL\n" + partial);
        } else if (partial.contains("Error occurred during initialization of VM")) {
          finishProbe(false, "APP_RESULT=FAIL\nJRE 初始化退出，未运行 Guest。\n" + partial);
        } else if (SystemClock.uptimeMillis() >= deadline) {
          finishProbe(false, "APP_RESULT=FAIL\n测试进程未返回；查看崩溃日志。\n" + partial);
        } else {
          handler.postDelayed(this, 250);
        }
      }
    }, 500);
  }

  private static String readNativeReport(File report) {
    try {
      return new String(
          Files.readAllBytes(report.toPath()), java.nio.charset.StandardCharsets.UTF_8);
    } catch (Exception unavailable) {
      return "";
    }
  }

  private void finishProbe(boolean pass, String report) {
    running = false;
    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    releaseProbe();
    handler.postDelayed(() -> {
      run.setEnabled(true);
      smoke.setEnabled(true);
    }, 500);
    boolean blocked = report.contains("APP_RESULT=BLOCKED");
    String result = pass ? "PASS" : blocked ? "BLOCKED" : "FAIL";
    status.setText(pass ? "通过 · PASS" : blocked ? "系统策略阻止 · BLOCKED" : "未通过 · FAIL");
    status.setTextColor(pass ? Color.rgb(24, 112, 65) : Color.rgb(170, 40, 40));
    details.setText(report);
    try {
      Files.write(new File(getFilesDir(), "last-result.txt").toPath(),
          report.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    } catch (Exception e) {
      Log.e("GlibcWorker", "Save result", e);
    }
    Log.i("GlibcWorker", "UI_RESULT=" + result);
  }

  private void releaseProbe() {
    if (connection != null) {
      unbindService(connection);
      connection = null;
    }
  }

  @Override
  public void onDestroy() {
    generation++;
    running = false;
    releaseProbe();
    super.onDestroy();
  }
}
