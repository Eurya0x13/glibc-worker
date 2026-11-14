#!/usr/bin/env python3
"""Run ten balanced A/B pairs of identical reference workloads on Android."""

import argparse
import hashlib
import json
import statistics
import subprocess
from pathlib import Path

from reference_cost import summarize


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(adb, output, label):
    for service in ("battery", "thermalservice"):
        (output / (label + "-" + service + ".txt")).write_bytes(
            subprocess.check_output(
                adb + ["shell", "dumpsys", service], timeout=30
            )
        )


def run_one(adb, remote, output):
    samples = []
    with output.open("w") as log:
        for size in (0, 32768, 131072, 262144):
            run = subprocess.run(
                adb + ["shell", remote, str(size)],
                capture_output=True,
                text=True,
                check=True,
                timeout=180,
            )
            log.write(run.stdout)
            log.flush()
            samples.extend(json.loads(line) for line in run.stdout.splitlines())
    return summarize(samples)


def comparison(runs, order):
    pairs = list(zip(order[::2], order[1::2], strict=True))
    result = []
    for case in runs[order[0]]:
        key = (case["high_water_refs"], case["operation"])
        values = {
            label: next(
                row["median_cpu_us"]
                for row in rows
                if (row["high_water_refs"], row["operation"]) == key
            )
            for label, rows in runs.items()
        }
        changes = []
        for left, right in pairs:
            a, b = (left, right) if left[0] == "A" else (right, left)
            changes.append(100 * (values[b] / values[a] - 1))
        result.append(
            {
                "high_water_refs": key[0],
                "operation": key[1],
                "baseline_median_cpu_us": statistics.median(
                    v for k, v in values.items() if k[0] == "A"
                ),
                "candidate_median_cpu_us": statistics.median(
                    v for k, v in values.items() if k[0] == "B"
                ),
                "paired_median_percent": statistics.median(changes),
                "candidate_lower_pairs": sum(change < 0 for change in changes),
                "pairs": len(pairs),
                "per_run_cpu_us": values,
            }
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    binaries = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    inputs = {
        v: json.loads((path.parent / "inputs.json").read_text())
        for v, path in binaries.items()
    }
    fixture = "android-jni/tests/native/reference_cost.c"
    if inputs["A"]["sources_sha256"][fixture] != inputs["B"]["sources_sha256"][
        fixture
    ] or any(
        data["target"] != "aarch64-linux-android.29" for data in inputs.values()
    ):
        parser.error("Reference fixture or Android target differs")
    order = [
        v + str(block * 4 + i)
        for block in range(5)
        for i, v in enumerate("ABBA" if block % 2 == 0 else "BAAB")
    ]
    manifest = {
        "device": args.device,
        "order": order,
        "inputs": inputs,
        "binary_sha256": {v: digest(path) for v, path in binaries.items()},
        "scope": "Shell UID, minimal VM double, CPU time; not JNI/RPC wall latency",
    }
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    adb = ["adb", "-s", args.device]
    remotes, runs = {}, {}
    try:
        for variant, binary in binaries.items():
            remote = "/data/local/tmp/gw-id-cost-" + digest(binary)[:16]
            remotes[variant] = remote
            subprocess.run(adb + ["push", str(binary), remote], check=True)
            subprocess.run(adb + ["shell", "chmod", "700", remote], check=True)
        for label in order:
            print("Starting " + label, flush=True)
            capture(adb, output, label + "-before")
            runs[label] = run_one(
                adb, remotes[label[0]], output / (label + ".jsonl")
            )
            capture(adb, output, label + "-after")
            print("Passed " + label, flush=True)
    finally:
        for remote in remotes.values():
            subprocess.run(adb + ["shell", "rm", "-f", remote], check=True)
    (output / "comparison.json").write_text(
        json.dumps(comparison(runs, order), indent=2) + "\n"
    )
    print("Reference comparison: " + str(output))


if __name__ == "__main__":
    main()
