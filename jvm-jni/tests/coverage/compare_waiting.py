#!/usr/bin/env python3
"""Compare ten fresh runs per variant in alternating, balanced A/B blocks."""

import argparse
import hashlib
import io
import json
import statistics
import subprocess
import sys
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[3]


def payload(path):
    with ZipFile(path) as apk:
        images = [
            name
            for name in apk.namelist()
            if name.startswith("lib/") and name.endswith("/libguest_bench.so")
        ]
        if len(images) != 1:
            raise ValueError("Expected one selected-ABI benchmark image")
        native = hashlib.sha256(apk.read(images[0])).hexdigest()

        with ZipFile(
            io.BytesIO(apk.read("assets/android-jre/smoke.jar"))
        ) as jar:
            classes = {
                name: hashlib.sha256(jar.read(name)).hexdigest()
                for name in jar.namelist()
                if name.endswith(".class")
            }

        android = {
            name: hashlib.sha256(apk.read(name)).hexdigest()
            for name in apk.namelist()
            if name.endswith(".dex") or name == "AndroidManifest.xml"
        }

    return {
        "guest_bench_sha256": native,
        "classes": classes,
        "android": android,
    }


def run_order(runs_per_variant):
    if runs_per_variant < 2 or runs_per_variant % 2:
        raise ValueError("Use an even number of runs per variant, at least two")
    return [
        variant + str(block * 4 + index)
        for block in range(runs_per_variant // 2)
        for index, variant in enumerate("ABBA" if block % 2 == 0 else "BAAB")
    ]


def metrics(rows):
    return {
        "samples": len(rows),
        "p50_us": statistics.median(row["p50_ns"] for row in rows) / 1000,
        "p95_us": statistics.median(row["p95_ns"] for row in rows) / 1000,
        "p99_us": statistics.median(row["p99_ns"] for row in rows) / 1000,
        "max_us": max(row["max_ns"] for row in rows) / 1000,
        "idle_cpu_percent_one_core": statistics.median(
            100 * row["idle_cpu_ns"] / row["idle_wall_ns"] for row in rows
        ),
        "calls_per_second": statistics.median(
            row["calls"] * 1e9 / row["wall_ns"] for row in rows
        ),
        "cpu_us_per_call": sum(row["cpu_ns"] for row in rows)
        / sum(row["calls"] for row in rows)
        / 1000,
    }


def summarize(baseline, candidate, pairs):
    labels = [directory.name for directory in baseline + candidate]
    if (
        not baseline
        or len(baseline) != len(candidate)
        or len(set(labels)) != len(labels)
        or sorted(label for pair in pairs for label in pair) != sorted(labels)
    ):
        raise ValueError(
            "Every complete run must belong to exactly one balanced pair"
        )

    for directory in baseline + candidate:
        if (
            json.loads((directory / "summary.json").read_text())["result"]
            != "PASS"
        ):
            raise ValueError("Incomplete coverage run: " + str(directory))

    result = []
    for threads in (1, 4, 8):
        for callback in (False, True):
            values, per_run = {}, {}
            for name, runs in (
                ("baseline", baseline),
                ("candidate", candidate),
            ):
                rows = []
                for directory in runs:
                    trial_rows = [
                        row
                        for row in json.loads(
                            (directory / "benchmark.json").read_text()
                        )
                        if row["threads"] == threads
                        and row["callback"] == callback
                    ]
                    if (
                        len(trial_rows) != 3
                        or {row["sample"] for row in trial_rows} != {0, 1, 2}
                        or any(
                            row["calls"] != threads * 2000
                            or row["wall_ns"] <= 0
                            or row["idle_wall_ns"] <= 0
                            or row["cpu_ns"] <= 0
                            or row["idle_cpu_ns"] < 0
                            or not 0
                            < row["p50_ns"]
                            <= row["p95_ns"]
                            <= row["p99_ns"]
                            <= row["max_ns"]
                            for row in trial_rows
                        )
                    ):
                        raise ValueError(
                            "Incomplete or invalid paired samples: "
                            + str(directory)
                        )

                    per_run[directory.name] = {
                        "variant": name,
                        **metrics(trial_rows),
                    }
                    rows.extend(trial_rows)
                values[name] = {"runs": len(runs), **metrics(rows)}

            differences = []
            for left, right in pairs:
                a, b = (per_run[label] for label in (left, right))
                if a["variant"] == "candidate":
                    a, b = b, a
                if a["variant"] != "baseline" or b["variant"] != "candidate":
                    raise ValueError("Each adjacent pair must contain A and B")
                differences.append(
                    {
                        "order": [left, right],
                        "candidate_minus_baseline": {
                            key: {
                                "absolute": b[key] - a[key],
                                "percent": 100 * (b[key] / a[key] - 1),
                            }
                            for key in (
                                "p50_us",
                                "p95_us",
                                "p99_us",
                                "cpu_us_per_call",
                            )
                        },
                    }
                )

            paired_summary = {}
            for key in ("p50_us", "p95_us", "p99_us", "cpu_us_per_call"):
                changes = [
                    pair["candidate_minus_baseline"][key]["percent"]
                    for pair in differences
                ]
                paired_summary[key] = {
                    "pairs": len(changes),
                    "candidate_lower_pairs": sum(
                        value < 0 for value in changes
                    ),
                    "median_percent": statistics.median(changes),
                    "min_percent": min(changes),
                    "max_percent": max(changes),
                }

            result.append(
                {
                    "threads": threads,
                    "callback": callback,
                    **values,
                    "per_run": per_run,
                    "paired": differences,
                    "paired_summary": paired_summary,
                }
            )

    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--benchmark-jit", action="store_true")
    parser.add_argument(
        "--runs-per-variant",
        type=int,
        default=10,
        help="Even count per variant; default 10 (20 fresh processes)",
    )
    args = parser.parse_args()

    try:
        order = run_order(args.runs_per_variant)
    except ValueError as error:
        parser.error(str(error))

    args.output = args.output.resolve()
    args.baseline = args.baseline.resolve()
    args.candidate = args.candidate.resolve()
    baseline_payload = payload(args.baseline)
    if baseline_payload != payload(args.candidate):
        raise SystemExit("Benchmark native/Java payloads differ")

    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise SystemExit("Choose a fresh comparison output directory")

    paths = {
        label: args.baseline if label.startswith("A") else args.candidate
        for label in order
    }
    pairs = list(zip(order[::2], order[1::2], strict=True))
    (args.output / "inputs.json").write_text(
        json.dumps(
            {
                "apk_sha256": {
                    name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for name, path in (
                        ("baseline", args.baseline),
                        ("candidate", args.candidate),
                    )
                },
                "benchmark_payload": baseline_payload,
                "order": list(paths),
                "runs_per_variant": args.runs_per_variant,
                "pairs": pairs,
                "jvm_mode": "normal-jit"
                if args.benchmark_jit
                else "strict-interpreter-checkjni",
                "quantiles": "Median of 3 * runs_per_variant per-trial quantiles; per-run uses three trials; not pooled-call percentiles",
                "pairing": "Adjacent A/B with balanced order; percent is (candidate / baseline - 1) * 100; descriptive, not a significance test",
            },
            indent=2,
        )
        + "\n"
    )

    for label, apk in paths.items():
        for name in ("battery", "thermalservice"):
            value = subprocess.run(
                ["adb", "-s", args.device, "shell", "dumpsys", name],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            (args.output / (label + "-before-" + name + ".txt")).write_text(
                value.stdout
            )

        command = [
            sys.executable,
            "jvm-jni/tests/run.py",
            "--group",
            "coverage",
            "--device",
            args.device,
            "--reuse-apk",
            "--apk",
            str(apk),
            "--benchmark",
            "--output",
            str(args.output / label),
        ]
        if args.benchmark_jit:
            command.append("--benchmark-jit")

        with (args.output / (label + "-driver.log")).open("w") as log:
            subprocess.run(
                command,
                cwd=ROOT,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=240,
            )

        for name in ("battery", "thermalservice"):
            value = subprocess.run(
                ["adb", "-s", args.device, "shell", "dumpsys", name],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            (args.output / (label + "-after-" + name + ".txt")).write_text(
                value.stdout
            )

    result = summarize(
        [args.output / label for label in order if label.startswith("A")],
        [args.output / label for label in order if label.startswith("B")],
        pairs,
    )
    (args.output / "comparison.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )

    # Leave the isolated test app on the candidate and check its shutdown path.
    subprocess.run(
        [
            sys.executable,
            "jvm-jni/tests/run.py",
            "--group",
            "smoke",
            "--device",
            args.device,
            "--reuse-apk",
            "--apk",
            str(args.candidate),
            "--output",
            str(args.output / "smoke-final"),
        ],
        cwd=ROOT,
        check=True,
    )

    print("Balanced waiting comparison: PASS; " + str(args.output))


if __name__ == "__main__":
    main()
