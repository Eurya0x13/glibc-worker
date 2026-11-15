#!/usr/bin/env python3
"""Capture one retained scalar/callback workload with scheduling or CPU evidence.

Profiling runs are diagnostic and are never merged into unprofiled A/B timings.
Each APK is still validated by its matching revision's functional checker.
"""

import argparse
import hashlib
import json
import subprocess
import sys
import time
import uuid
from pathlib import Path

from review_changes import capture, workload

ROOT = Path(__file__).resolve().parents[3]
PACKAGE = "dev.glibc.worker"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def shell(adb, *args, check=True):
    return subprocess.run(
        adb + ["shell", *args],
        capture_output=True,
        text=True,
        check=check,
        timeout=30,
    )


def prepare(args, output, adb):
    revision = subprocess.check_output(
        ["git", "rev-parse", args.checker_ref + "^{commit}"],
        cwd=ROOT,
        text=True,
    ).strip()
    checker = output / "check.py"
    checker.write_bytes(
        subprocess.check_output(
            ["git", "show", revision + ":jvm-jni/tests/check.py"], cwd=ROOT
        )
    )
    manifest = {
        "kind": args.kind,
        "apk_sha256": digest(args.apk),
        "checker_revision": revision,
        "checker_sha256": digest(checker),
        "workload": workload(args.apk),
        "mode": "JIT, CheckJNI off, profiled diagnostic",
        "scope": "Scheduling covers the whole run; CPU recording triggers after the final four-thread callback trial and covers the eight-thread stage plus teardown. Not an unprofiled latency comparison.",
    }
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run(
        [
            "android",
            "install",
            "--device=" + args.device,
            "--apks=" + str(args.apk.resolve()),
            "--use-delta-install=false",
        ],
        check=True,
        timeout=180,
    )
    shell(adb, "am", "force-stop", PACKAGE)
    shell(adb, "run-as", PACKAGE, "rm", "-f", "files/native-report.txt")
    return checker, manifest


def launch(args, output, checker, log):
    command = [
        sys.executable,
        str(Path(__file__).with_name("review_changes.py")),
        "--single",
        "--checker",
        str(checker),
        "--group",
        "coverage",
        "--device",
        args.device,
        "--reuse-apk",
        "--installed",
        "--apk",
        str(args.apk.resolve()),
        "--benchmark",
        "--benchmark-jit",
        "--output",
        str(output / "run"),
    ]
    (output / "driver-command.json").write_text(
        json.dumps(command, indent=2) + "\n"
    )
    return subprocess.Popen(
        command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT
    )


def start_scheduler(adb, output, tag):
    config = output / "scheduler.cfg"
    config.write_text("""duration_ms: 90000
buffers { size_kb: 65536 fill_policy: RING_BUFFER }
flush_period_ms: 1000
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      ftrace_events: "sched/sched_process_exit"
      ftrace_events: "task/task_newtask"
      ftrace_events: "task/task_rename"
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      buffer_size_kb: 4096
      drain_period_ms: 100
    }
  }
}
data_sources {
  config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true proc_stats_poll_ms: 100 } }
}
""")
    remote_trace = "/data/misc/perfetto-traces/gw-sched-" + tag + ".pftrace"
    command = [
        "perfetto",
        "--background-wait",
        "--txt",
        "-c",
        "-",
        "-o",
        remote_trace,
    ]
    # Android's perfetto domain may not read shell_data_file configs. Send
    # the exact saved config on stdin instead of changing device policy.
    started = subprocess.run(
        adb + ["shell", *command],
        input=config.read_text(),
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    (output / "recorder-start.txt").write_text(started.stdout + started.stderr)
    started.check_returncode()
    pids = [
        line.strip()
        for line in started.stdout.splitlines()
        if line.strip().isdigit()
    ]
    if len(pids) != 1:
        raise ValueError("Missing unique Perfetto recorder PID")
    return pids[0], remote_trace


def finish_scheduler(adb, output, recorder):
    pid, remote = recorder
    shell(adb, "kill", "-TERM", pid, check=False)
    previous, stable = None, 0
    for _ in range(15):
        size = shell(
            adb, "stat", "-c", "%s", remote, check=False
        ).stdout.strip()
        stable = (
            stable + 1
            if size == previous and size.isdigit() and int(size) > 0
            else 0
        )
        if stable >= 2:
            break
        previous = size
        time.sleep(0.5)
    else:
        raise ValueError("Perfetto output did not stabilize")
    trace = output / "scheduler.pftrace"
    subprocess.run(adb + ["pull", remote, str(trace)], check=True)
    shell(adb, "rm", "-f", remote)
    return {"file": trace.name, "sha256": digest(trace)}


def cpu_profile(adb, output, tag, driver):
    deadline = time.monotonic() + 180
    marker = "BRIDGE_BENCH threads=4 callback=true sample=2 "
    while time.monotonic() < deadline:
        report = shell(
            adb,
            "run-as",
            PACKAGE,
            "cat",
            "files/native-report.txt",
            check=False,
        ).stdout
        if marker in report:
            (output / "trigger-report.txt").write_text(report)
            break
        if driver.poll() is not None:
            raise ValueError("Workload ended before the eight-thread trigger")
        time.sleep(0.25)
    else:
        raise TimeoutError("No eight-thread stage trigger")
    remote = "/data/local/tmp/gw-cpu-" + tag + ".data"
    command = [
        "simpleperf",
        "record",
        "--app",
        PACKAGE,
        "-e",
        "cpu-clock:u",
        "-f",
        "1000",
        "-g",
        "--duration",
        "10",
        "-o",
        remote,
    ]
    run = shell(adb, *command, check=False)
    (output / "recorder.txt").write_text(run.stdout + run.stderr)
    run.check_returncode()
    local = output / "perf.data"
    subprocess.run(adb + ["pull", remote, str(local)], check=True)
    shell(adb, "rm", "-f", remote)
    return {"file": local.name, "sha256": digest(local), "command": command}


def run_capture(args, output, adb, checker):
    tag = uuid.uuid4().hex[:12]
    recorder = (
        start_scheduler(adb, output, tag) if args.kind == "scheduler" else None
    )
    with (output / "driver.log").open("w") as log:
        driver = launch(args, output, checker, log)
        try:
            data = (
                cpu_profile(adb, output, tag, driver)
                if args.kind == "cpu"
                else None
            )
            status = driver.wait(timeout=240)
            if status:
                raise subprocess.CalledProcessError(status, driver.args)
        finally:
            if driver.poll() is None:
                driver.terminate()
                driver.wait(timeout=15)
            if recorder:
                data = finish_scheduler(adb, output, recorder)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--checker-ref", required=True)
    parser.add_argument("--kind", choices=("scheduler", "cpu"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    adb = ["adb", "-s", args.device]
    checker, manifest = prepare(args, output, adb)
    capture(args.device, output, "before")
    data = run_capture(args, output, adb, checker)
    capture(args.device, output, "after")
    summary = json.loads((output / "run/summary.json").read_text())
    if summary["result"] != "PASS":
        raise ValueError("Profiled workload failed functional validation")
    result = {
        "result": "PASS",
        "inputs": manifest,
        "capture": data,
        "run_id": summary["run_id"],
    }
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Profile captured: " + str(output), flush=True)


if __name__ == "__main__":
    main()
