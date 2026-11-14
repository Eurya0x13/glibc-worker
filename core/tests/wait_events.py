"""Exercise real futex races, interrupts, spurious wakeups and stop broadcasts."""

import argparse
import json
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ready-poll", type=int, choices=(0, 1), default=0)
args = parser.parse_args()
output = ROOT / "build/test-work/wait-events"
if args.ready_poll:
    output /= "ready-poll"
output.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("SANITIZER_CC", "clang")
results = []
for wake_filter in (0, 1):
    for spin_ns in (0, 20000):
        for profile in (0, 1):
            for name, sources in (
                ("wait-events", ["wait_events.c"]),
                (
                    "rpc-metrics",
                    [
                        "rpc_metrics.c",
                        "rpc_metrics_peer.c",
                        "rpc_metrics_faults.c",
                    ],
                ),
                ("rpc-breakdown", ["rpc_breakdown.c", "rpc_breakdown_fake.c"]),
                ("wake-filter", ["wake_filter_races.c"]),
            ):
                directory = (
                    output
                    / f"{name}-filter{wake_filter}-profile{profile}-spin{spin_ns}"
                )
                directory.mkdir(parents=True, exist_ok=True)
                binary = directory / "probe"
                command = [
                    compiler,
                    "-std=gnu17",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                    f"-DGW_RPC_PROFILE={profile}",
                    f"-DGW_FUTEX_WAKE_FILTER={wake_filter}",
                    f"-DGW_RPC_SPIN_NS={spin_ns}",
                    f"-DGW_RPC_READY_POLL={args.ready_poll}",
                    "-Iinclude",
                    *("tests/native/" + source for source in sources),
                    "-pthread",
                    "-o",
                    str(binary),
                ]
                with (directory / "build.log").open("w") as log:
                    subprocess.run(
                        command, cwd=ROOT, stdout=log, stderr=log, check=True
                    )
                result = subprocess.run(
                    [str(binary)],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=40,
                    env={
                        **os.environ,
                        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1",
                    },
                )
                (directory / "result.txt").write_text(
                    result.stdout + result.stderr
                )
                if result.returncode:
                    raise SystemExit(result.stdout + result.stderr)
                marker = {
                    "wait-events": "WAIT_EVENT_TEST",
                    "rpc-metrics": "RPC_METRICS_TEST",
                    "rpc-breakdown": "RPC_BREAKDOWN_TEST",
                    "wake-filter": "WAKE_FILTER_TEST",
                }[name]
                if not re.search(
                    rf"^{marker} .*result=PASS$", result.stdout, re.MULTILINE
                ):
                    raise SystemExit(
                        "Missing successful probe evidence: " + str(directory)
                    )
                symbols = subprocess.check_output(
                    ["nm", "--defined-only", binary], text=True
                )
                has_tls = (
                    re.search(r"\bgw_rpc_current$", symbols, re.MULTILINE)
                    is not None
                )
                has_scope_tls = (
                    re.search(r"\bgw_rpc_scope_current$", symbols, re.MULTILINE)
                    is not None
                )
                if has_tls != bool(profile) or has_scope_tls != bool(profile):
                    raise SystemExit(
                        "Diagnostic TLS presence differs from build option"
                    )
                if not profile and "WAIT_EVENT_METRICS" in result.stdout:
                    raise SystemExit("Non-diagnostic build emitted metrics")
                case = {
                    "probe": name,
                    "profile": bool(profile),
                    "wake_filter": bool(wake_filter),
                    "ready_poll": bool(args.ready_poll),
                    "spin_budget_ns": spin_ns,
                    "diagnostic_tls_present": has_tls,
                    "diagnostic_scope_tls_present": has_scope_tls,
                    "result": "PASS",
                }
                if name == "rpc-breakdown" and profile:
                    notify = re.search(
                        r"^RPC_NOTIFY_REAL zero_calls=(\d+) zero_samples=(\d+) "
                        r"positive_calls=(\d+) positive_samples=(\d+) result=PASS$",
                        result.stdout,
                        re.MULTILINE,
                    )
                    if not notify or not all(
                        int(value) > 0 for value in notify.groups()
                    ):
                        raise SystemExit(
                            "Missing both real sampled WAKE return outcomes"
                        )
                    case["notify_real"] = dict(
                        zip(
                            (
                                "zero_calls",
                                "zero_samples",
                                "positive_calls",
                                "positive_samples",
                            ),
                            (int(value) for value in notify.groups()),
                            strict=True,
                        )
                    )
                if name == "wake-filter":
                    counts = re.search(
                        r"empty_notify=(\d+) empty_wake=(\d+)", result.stdout
                    )
                    expected = 0 if wake_filter else 10000
                    if not counts or tuple(map(int, counts.groups())) != (
                        10000,
                        expected,
                    ):
                        raise SystemExit(
                            "Unexpected actual WAKE count for empty notifications"
                        )
                    case["empty_notify_calls"] = 10000
                    case["empty_wake_syscalls"] = expected
                results.append(case)
                print(result.stdout, end="")

# Probe the actual default headers without sanitizer/runtime library symbols.
# Active waits can legitimately read CLOCK_MONOTONIC in release builds; the
# scope-only object must contain neither diagnostic TLS nor timing references.
release_object = output / "breakdown-default.o"
subprocess.run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-DGW_RPC_READY_POLL={args.ready_poll}",
        "-Iinclude",
        "-c",
        "tests/native/rpc_breakdown.c",
        "-o",
        str(release_object),
    ],
    cwd=ROOT,
    check=True,
)
release_symbols = subprocess.check_output(["nm", release_object], text=True)
for forbidden in (
    "gw_rpc_current",
    "gw_rpc_scope_current",
    "clock_gettime",
    "gw_rpc_scope_enter",
    "gw_rpc_cost_clock",
    "gw_rpc_notify_classify",
):
    if re.search(rf"\b{forbidden}$", release_symbols, re.MULTILINE):
        raise SystemExit(
            "Default breakdown probe retained diagnostic symbol: " + forbidden
        )
(output / "breakdown-default-symbols.txt").write_text(release_symbols)
print("RPC_BREAKDOWN_DEFAULT diagnostic_symbols=0 timing_symbols=0 result=PASS")
(output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
