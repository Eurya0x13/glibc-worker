#!/usr/bin/env python3
"""Measure byte-size scaling against bionic JNI, ten balanced pairs by default."""

import argparse
import hashlib
import json
import statistics
import subprocess
import sys
from pathlib import Path

from compare_waiting import run_order

ROOT = Path(__file__).resolve().parents[3]
SIZES = (0, 64, 1024, 16384, 65536, 262144, 1048576, 4194304)
OPERATIONS = (
    "pointer",
    "direct_address",
    "memcpy",
    "array_region",
    "array_elements",
)


def measurements(rows):
    return {
        **{
            f"p{p}_us": statistics.median(row[f"p{p}_ns"] for row in rows)
            / 1000
            for p in (50, 95, 99)
        },
        "max_us": max(row["max_ns"] for row in rows) / 1000,
        "cpu_us_per_call": sum(row["cpu_ns"] for row in rows)
        / sum(row["calls"] for row in rows)
        / 1000,
        "trials": len(rows),
        "calls": sum(row["calls"] for row in rows),
        "over_1ms_calls": sum(row["over_1ms"] for row in rows),
        "over_5ms_calls": sum(row["over_5ms"] for row in rows),
        "zero_ns_calls": sum(row["zero_ns_calls"] for row in rows),
    }


def summarize(output):
    inputs = json.loads((output / "inputs.json").read_text())
    if inputs["order"] != run_order(inputs["runs_per_variant"]):
        raise ValueError("Unexpected or incomplete balanced run plan")
    if inputs["pairs"] != [
        list(pair)
        for pair in zip(
            inputs["order"][::2], inputs["order"][1::2], strict=True
        )
    ]:
        raise ValueError("Run pairing differs from the frozen plan")

    runs = {}
    for index, label in enumerate(inputs["order"]):
        directory = output / label
        summary = json.loads((directory / "summary.json").read_text())
        mode = "host" if label.startswith("A") else "guest"
        if (
            summary["result"] != "PASS"
            or summary["apk_sha256"] != inputs["apk_sha256"]
            or summary["payload_mode"] != mode
            or summary["payload_round"] != index // 2
            or summary["jvm_mode"] != "benchmark-normal-jit"
        ):
            raise ValueError("Run is incomplete or APK changed: " + label)
        runs[label] = json.loads((directory / "payload.json").read_text())
        if len(runs[label]) != 120:
            raise ValueError("Missing payload trials")

    for left, right in inputs["pairs"]:
        if [(row["point"], row["trial"]) for row in runs[left]] != [
            (row["point"], row["trial"]) for row in runs[right]
        ]:
            raise ValueError("Payload order differs inside an A/B pair")

    points = []
    for operation in OPERATIONS:
        for size in SIZES:
            per_run = {}
            for label, rows in runs.items():
                selected = [
                    row
                    for row in rows
                    if row["operation"] == operation and row["bytes"] == size
                ]
                if len(selected) != 3 or {row["trial"] for row in selected} != {
                    0,
                    1,
                    2,
                }:
                    raise ValueError("Missing configuration trials")
                per_run[label] = measurements(selected)

            values = {}
            for prefix, mode in (("A", "host"), ("B", "guest")):
                selected = [
                    row
                    for label, rows in runs.items()
                    if label.startswith(prefix)
                    for row in rows
                    if row["operation"] == operation and row["bytes"] == size
                ]
                values[mode] = measurements(selected)

            paired = []
            for left, right in inputs["pairs"]:
                a, b = (left, right) if left.startswith("A") else (right, left)
                paired.append(
                    {
                        "order": [left, right],
                        **{
                            key + "_guest_minus_host": per_run[b][key]
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
                    **values,
                    "per_run": per_run,
                    "paired": paired,
                    "paired_p50_excess_median_us": statistics.median(
                        p["p50_us_guest_minus_host"] for p in paired
                    ),
                    "elements_copy_counts": sorted(
                        {
                            row["elements_copy_calls"]
                            for rows in runs.values()
                            for row in rows
                            if row["operation"] == operation
                            and row["bytes"] == size
                        }
                    ),
                }
            )

    growth = []
    for operation in OPERATIONS:
        first = next(
            row
            for row in points
            if row["operation"] == operation and row["bytes"] == 0
        )
        last = next(
            row
            for row in points
            if row["operation"] == operation and row["bytes"] == SIZES[-1]
        )

        pairs = []
        for left, right in inputs["pairs"]:
            a, b = (left, right) if left.startswith("A") else (right, left)
            host = last["per_run"][a]["p50_us"] - first["per_run"][a]["p50_us"]
            guest = last["per_run"][b]["p50_us"] - first["per_run"][b]["p50_us"]
            pairs.append(
                {
                    "order": [left, right],
                    "host_p50_growth_us": host,
                    "guest_p50_growth_us": guest,
                    "excess_growth_us": guest - host,
                }
            )

        growth.append(
            {
                "operation": operation,
                "from_bytes": 0,
                "to_bytes": SIZES[-1],
                "pairs": pairs,
                "median_host_p50_growth_us": statistics.median(
                    p["host_p50_growth_us"] for p in pairs
                ),
                "median_guest_p50_growth_us": statistics.median(
                    p["guest_p50_growth_us"] for p in pairs
                ),
                "median_excess_growth_us": statistics.median(
                    p["excess_growth_us"] for p in pairs
                ),
            }
        )

    result = {
        "result": "PASS",
        "runs": len(runs),
        "pairs": len(inputs["pairs"]),
        "points": points,
        "growth_0_to_4mib": growth,
        "scope": "Reusable prefaulted buffers; one Java caller. Same C source built for bionic and glibc, including different libc memcpy/allocator implementations. Per-trial quantile medians, not pooled percentiles. Whole-process CPU includes JIT. Paired differences are descriptive; not automatically pure bridge cost or a linear bandwidth model.",
    }
    (output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device")
    parser.add_argument("--apk", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs-per-variant", type=int, default=10)
    parser.add_argument("--summarize-only", action="store_true")
    parser.add_argument(
        "--installed",
        action="store_true",
        help="Reuse an installation; each run still verifies its APK hash",
    )
    args = parser.parse_args()

    args.output = args.output.resolve()
    if args.summarize_only:
        summarize(args.output)
        return

    if not args.device or not args.apk:
        parser.error("--device and --apk are required for recording")
    try:
        order = run_order(args.runs_per_variant)
    except ValueError as failure:
        parser.error(str(failure))

    args.apk = args.apk.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    inputs = {
        "apk_sha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(),
        "order": order,
        "pairs": list(zip(order[::2], order[1::2], strict=True)),
        "modes": {"A": "host", "B": "guest"},
        "runs_per_variant": args.runs_per_variant,
        "sizes": SIZES,
        "operations": OPERATIONS,
        "warmup": 300,
        "calls_per_trial": 500,
        "trials_per_point": 3,
        "jvm": "normal-jit, CheckJNI off, RPC diagnostics off",
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
        mode = inputs["modes"][label[0]]
        print(
            "Payload comparison:", label, mode, "pair", index // 2, flush=True
        )

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
                            mode,
                            "--payload-round",
                            str(index // 2),
                            "--benchmark-jit",
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
            "--output",
            str(args.output / "smoke-final"),
        ],
        cwd=ROOT,
        check=True,
        timeout=180,
    )

    print("Payload comparison: PASS; runs=", result["runs"], flush=True)


if __name__ == "__main__":
    main()
