#!/usr/bin/env python3
"""Same-APK Guest Elements cache off/on comparison, ten balanced pairs."""

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

from compare_payload import ROOT, measurements
from compare_waiting import run_order

sys.path.insert(0, str(ROOT / "tests"))
from payload import payload_points, verify_payload, verify_payload_phases


def summarize(output):
    inputs = json.loads((output / "inputs.json").read_text())
    order = inputs["order"]
    profile = inputs.get("payload_profile", "default")
    sizes, operations = payload_points(profile)

    if (
        list(sizes) != inputs["sizes"]
        or list(operations) != inputs["operations"]
    ):
        raise ValueError("Payload points differ from the frozen plan")
    if order != run_order(inputs["runs_per_variant"]):
        raise ValueError("Incomplete balanced run plan")

    runs = {}
    phases = {}
    for index, label in enumerate(order):
        directory = output / label
        evidence = json.loads((directory / "summary.json").read_text())
        if (
            evidence["result"] != "PASS"
            or evidence["apk_sha256"] != inputs["apk_sha256"]
            or evidence["payload_mode"] != "guest"
            or evidence["payload_round"] != index // 2
            or evidence["jvm_mode"] != "benchmark-normal-jit"
            or evidence["array_copy_cache"] != label.startswith("B")
            or evidence.get("payload_diagnostic", False)
            or evidence.get("payload_profile", "default") != profile
        ):
            raise ValueError("Wrong configuration or incomplete run: " + label)

        rows = json.loads((directory / "payload.json").read_text())
        validated = verify_payload(
            (directory / "report.txt").read_text(),
            (directory / "payload-samples.bin").read_bytes(),
            "guest",
            index // 2,
            profile,
        )
        if rows != validated:
            raise ValueError("Missing trials")
        runs[label] = rows

        if profile == "threshold":
            phases[label] = verify_payload_phases(
                (directory / "report.txt").read_text(), sizes
            )
            (directory / "phases.json").write_text(
                json.dumps(phases[label], indent=2) + "\n"
            )

        cache = re.search(
            r"ARRAY_COPY_CACHE_FINISH hits=(\d+) misses=(\d+) fallback=(\d+) active=0 retained_bytes=0",
            (directory / "report.txt").read_text(),
        )
        if (
            not cache
            or label.startswith("B")
            and not all(int(value) for value in cache.groups())
        ):
            raise ValueError(
                "No evidence of reuse, allocation, bounded fallback and cleanup"
            )

    pairs = list(zip(order[::2], order[1::2], strict=True))
    if inputs["pairs"] != [list(pair) for pair in pairs]:
        raise ValueError("Pair plan changed")
    for left, right in pairs:
        if [(r["point"], r["trial"]) for r in runs[left]] != [
            (r["point"], r["trial"]) for r in runs[right]
        ]:
            raise ValueError("Configuration order differs within pair")

    points = []
    for operation in operations:
        for size in sizes:
            selected = {
                label: [
                    r
                    for r in rows
                    if r["operation"] == operation and r["bytes"] == size
                ]
                for label, rows in runs.items()
            }
            if any(
                len(rows) != 3 or {r["trial"] for r in rows} != {0, 1, 2}
                for rows in selected.values()
            ):
                raise ValueError("Missing point trials")

            per_run = {
                label: measurements(rows) for label, rows in selected.items()
            }
            paired = []
            for left, right in pairs:
                a, b = (left, right) if left.startswith("A") else (right, left)
                paired.append(
                    {
                        "order": [left, right],
                        **{
                            key + "_on_minus_off": per_run[b][key]
                            - per_run[a][key]
                            for key in (
                                "p50_us",
                                "p95_us",
                                "p99_us",
                                "cpu_us_per_call",
                            )
                        },
                    }
                )

            points.append(
                {
                    "operation": operation,
                    "bytes": size,
                    **{
                        name: measurements(
                            [
                                row
                                for label, rows in selected.items()
                                if label.startswith(prefix)
                                for row in rows
                            ]
                        )
                        for prefix, name in (("A", "off"), ("B", "on"))
                    },
                    "per_run": per_run,
                    "paired": paired,
                    "paired_p50_delta_median_us": statistics.median(
                        row["p50_us_on_minus_off"] for row in paired
                    ),
                }
            )

            if phases:
                points[-1]["host_minor_faults_per_call"] = {
                    **{
                        name: statistics.median(
                            values[size]["minor_faults_per_call"]
                            for label, values in phases.items()
                            if label.startswith(prefix)
                        )
                        for prefix, name in (("A", "off"), ("B", "on"))
                    },
                    "per_run": {
                        label: values[size]["minor_faults_per_call"]
                        for label, values in phases.items()
                    },
                    "scope": "Untimed Host getrusage boundaries, including warmups, Java loops and result output; not per-call counters.",
                }

    result = {
        "result": "PASS",
        "pairs": len(pairs),
        "runs": len(runs),
        "points": points,
        "scope": "Same APK and Guest C/Java workload; only Host cache configuration differs, as recorded in inputs.json. Medians of per-trial quantiles, not pooled percentiles. Whole-process CPU includes JIT. Single Java benchmark caller; separate untimed 12-lease concurrency checks precede measurement.",
    }
    (output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device")
    parser.add_argument("--apk", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs-per-variant", type=int, default=10)
    parser.add_argument(
        "--payload-profile", choices=("default", "threshold"), default="default"
    )
    parser.add_argument("--installed", action="store_true")
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()

    args.output = args.output.resolve()
    if args.summarize_only:
        summarize(args.output)
        return

    if not args.device or not args.apk:
        parser.error("--device and --apk are required")

    order = run_order(args.runs_per_variant)
    args.apk = args.apk.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    sizes, operations = payload_points(args.payload_profile)
    inputs = {
        "apk": str(args.apk),
        "apk_sha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(),
        "device": args.device,
        "order": order,
        "pairs": list(zip(order[::2], order[1::2], strict=True)),
        "runs_per_variant": args.runs_per_variant,
        "A": "Guest/cache off",
        "B": "Guest/cache automatic",
        "sizes": sizes,
        "operations": operations,
        "payload_profile": args.payload_profile,
        "warmup": 300,
        "calls_per_trial": 500,
        "trials_per_point": 3,
        "jvm": "normal JIT, CheckJNI off, RPC diagnostics off",
    }
    (args.output / "inputs.json").write_text(
        json.dumps(inputs, indent=2) + "\n"
    )

    if not args.installed:
        subprocess.run(
            [
                "android",
                "install",
                "--device=" + args.device,
                "--apks=" + str(args.apk),
                "--use-delta-install=false",
            ],
            check=True,
            timeout=180,
        )

    for index, label in enumerate(order):
        print("Array cache comparison:", label, "pair", index // 2, flush=True)

        for edge in ("before", "after"):
            if edge == "after":
                with (args.output / (label + "-driver.log")).open("w") as log:
                    subprocess.run(
                        [
                            sys.executable,
                            "jvm-jni/tests/run.py",
                            "--group",
                            "coverage",
                            "--device",
                            args.device,
                            "--reuse-apk",
                            "--installed",
                            "--apk",
                            str(args.apk),
                            "--payload-benchmark",
                            "guest",
                            "--payload-round",
                            str(index // 2),
                            "--payload-profile",
                            args.payload_profile,
                            "--benchmark-jit",
                            "--array-copy-cache",
                            "auto" if label.startswith("B") else "off",
                            "--output",
                            str(args.output / label),
                        ],
                        cwd=ROOT,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=True,
                        timeout=300,
                    )

            for name in ("battery", "thermalservice"):
                result = subprocess.run(
                    ["adb", "-s", args.device, "shell", "dumpsys", name],
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=30,
                )
                (
                    args.output / (label + "-" + edge + "-" + name + ".txt")
                ).write_text(result.stdout)

    result = summarize(args.output)

    subprocess.run(
        [
            sys.executable,
            "jvm-jni/tests/run.py",
            "--group",
            "smoke",
            "--device",
            args.device,
            "--reuse-apk",
            "--installed",
            "--apk",
            str(args.apk),
            "--array-copy-cache",
            "auto",
            "--output",
            str(args.output / "smoke-final"),
        ],
        cwd=ROOT,
        check=True,
        timeout=180,
    )

    print("Array cache comparison: PASS; runs=", result["runs"], flush=True)


if __name__ == "__main__":
    main()
