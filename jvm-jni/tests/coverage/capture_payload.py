#!/usr/bin/env python3
"""Capture scheduler evidence overlapping the diagnostic 4MiB Elements case."""

import argparse
import hashlib
import json
import re
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=False)
    adb = ["adb", "-s", args.device]

    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        result = subprocess.run(
            adb
            + [
                "shell",
                "run-as",
                "dev.glibc.worker",
                "cat",
                "files/native-report.txt",
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )

        starts = re.findall(
            r"PAYLOAD_PHASE stage=0 operation=4 bytes=4194304 .*?tid=(\d+)",
            result.stdout,
        )
        ends = re.findall(
            r"PAYLOAD_PHASE stage=1 operation=4 bytes=4194304", result.stdout
        )

        if starts and not ends:
            host_tid = int(starts[-1])
            break
        if ends:
            raise SystemExit(
                "Diagnostic phase already ended; no contemporaneous trace"
            )
        time.sleep(0.1)
    else:
        raise SystemExit("Diagnostic phase not reached")

    pid = subprocess.check_output(
        adb + ["shell", "pidof", "dev.glibc.worker:probe"], text=True
    ).strip()
    if not pid.isdigit():
        raise SystemExit("Expected one probe process")

    config = """buffers { size_kb: 65536 fill_policy: DISCARD }
duration_ms: 8000
data_sources { config { name: "linux.ftrace" ftrace_config {
 compact_sched { enabled: true }
 ftrace_events: "sched/sched_switch"
 ftrace_events: "sched/sched_waking"
 ftrace_events: "task/task_newtask"
 ftrace_events: "task/task_rename"
 ftrace_events: "power/cpu_frequency"
 buffer_size_kb: 4096
 drain_period_ms: 100
} } }
data_sources { config { name: "linux.process_stats" } }
"""
    (args.output / "config.pbtxt").write_text(config)

    remote = "/data/misc/perfetto-traces/glibc-worker-payload.pftrace"
    result = subprocess.run(
        adb + ["shell", "perfetto", "--txt", "-c", "-", "-o", remote],
        input=config,
        capture_output=True,
        text=True,
        check=True,
        timeout=30,
    )
    (args.output / "capture.log").write_text(result.stdout + result.stderr)

    trace = args.output / "scheduling-private.pftrace"
    subprocess.run(
        adb + ["pull", remote, str(trace)],
        check=True,
        capture_output=True,
        timeout=30,
    )
    subprocess.run(adb + ["shell", "rm", remote], check=True)

    metadata = {
        "pid": int(pid),
        "host_tid": host_tid,
        "trace_sha256": hashlib.sha256(trace.read_bytes()).hexdigest(),
        "scope": "Diagnostic timing; intervals must be individually intersected with trace bounds",
    }
    (args.output / "capture.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )

    print("Payload scheduling captured", flush=True)


if __name__ == "__main__":
    main()
