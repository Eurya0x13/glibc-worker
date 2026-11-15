#!/usr/bin/env python3
"""Compare retained loader bookkeeping, not JNI latency, under mutex contention."""

import argparse
import hashlib
import json
import re
import statistics
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def record(adb, output, label):
    for name in ("battery", "thermalservice"):
        (output / (label + "-" + name + ".txt")).write_bytes(
            subprocess.check_output(
                adb + ["shell", "dumpsys", name], timeout=30
            )
        )


def pair_summary(runs, order, threads):
    result = {"threads": threads, "pairs": 10}
    for field in ("cpu_ns", "wall_ns"):
        values = {
            k: rows[threads][field] / rows[threads]["calls"]
            for k, rows in runs.items()
        }
        changes = []
        for left, right in zip(order[::2], order[1::2], strict=True):
            a, b = (left, right) if left[0] == "A" else (right, left)
            changes.append(100 * (values[b] / values[a] - 1))
        result[field + "_per_pair"] = {
            "baseline": statistics.median(
                v for k, v in values.items() if k[0] == "A"
            ),
            "candidate": statistics.median(
                v for k, v in values.items() if k[0] == "B"
            ),
            "paired_median_percent": statistics.median(changes),
            "candidate_lower_pairs": sum(v < 0 for v in changes),
            "per_run": values,
        }
    return result


def run_case(adb, command, output, label, threads, mode):
    run = subprocess.run(
        adb + ["shell", *command, mode, str(threads), "100000"],
        capture_output=True,
        text=True,
        timeout=90,
        check=False,
    )
    (output / f"{label}-{threads}.txt").write_text(run.stdout + run.stderr)
    run.check_returncode()
    matches = re.findall(r"^SCOPE_COST (.+)$", run.stdout, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError("Missing unique scope-cost result")
    row = json.loads(matches[0])
    if (
        row["result"] != "PASS"
        or row["calls"] != threads * 100000
        or row["threads"] != threads
        or row["mode"] != mode
        or min(row["cpu_ns"], row["wall_ns"]) <= 0
    ):
        raise ValueError("Invalid scope-cost result")
    return row


def compare(adb, binary, variants, output):
    output.mkdir()
    order = [
        v + str(block * 4 + i)
        for block in range(5)
        for i, v in enumerate("ABBA" if block % 2 == 0 else "BAAB")
    ]
    runs = {}
    for label in order:
        record(adb, output, label + "-before")
        library, mode = variants[label[0]]
        runs[label] = {
            threads: run_case(
                adb, [binary, library], output, label, threads, mode
            )
            for threads in (1, 8)
        }
        record(adb, output, label + "-after")
        print(output.name + " passed " + label, flush=True)
    result = {
        "order": order,
        "variants": variants,
        "comparison": [pair_summary(runs, order, t) for t in (1, 8)],
        "raw_runs": runs,
    }
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def build(args, output):
    compiler = (
        args.android_ndk
        / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    )
    binary = output / "scope_cost"
    source = PROJECT / "tests/scope_cost.c"
    command = [
        str(compiler),
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fno-emulated-tls",
        "-I" + str(PROJECT / "include"),
        str(source),
        "-pthread",
        "-ldl",
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True)
    paths = {
        "binary": binary,
        "old": args.baseline_library.resolve(),
        "new": args.candidate_library.resolve(),
    }
    manifest = {
        "compile": command,
        "compiler": subprocess.check_output(
            [str(compiler), "--version"], text=True
        ),
        "source_sha256": digest(source),
        "inputs_sha256": {k: digest(p) for k, p in paths.items()},
        "scope": "Standalone bionic process, real exported backend functions and shared mutex, fake transport, all threads borrow the same module; no JNI/Guest/RPC",
        "method": "10 balanced pairs per comparison; 2000 warmup pairs per thread then 100000 measured pairs; averages per enter+leave, not call latency quantiles",
    }
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return paths, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--android-ndk", type=Path, required=True)
    parser.add_argument("--baseline-library", type=Path, required=True)
    parser.add_argument("--candidate-library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    paths, manifest = build(args, output)
    adb = ["adb", "-s", args.device]
    remotes = {
        k: "/data/local/tmp/gw-scope-" + digest(p)[:16]
        for k, p in paths.items()
    }
    try:
        for key, path in paths.items():
            subprocess.run(adb + ["push", str(path), remotes[key]], check=True)
            subprocess.run(
                adb + ["shell", "chmod", "700", remotes[key]], check=True
            )
        comparisons = {
            "old-pin-vs-new-scope": {
                "A": (remotes["old"], "pin"),
                "B": (remotes["new"], "scope"),
            },
            "new-pin-vs-new-scope": {
                "A": (remotes["new"], "pin"),
                "B": (remotes["new"], "scope"),
            },
        }
        results = {
            name: compare(adb, remotes["binary"], variants, output / name)
            for name, variants in comparisons.items()
        }
    finally:
        for remote in set(remotes.values()):
            subprocess.run(adb + ["shell", "rm", "-f", remote], check=True)
    (output / "summary.json").write_text(
        json.dumps(
            {"result": "PASS", "inputs": manifest, "comparisons": results},
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
