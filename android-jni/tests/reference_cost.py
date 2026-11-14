#!/usr/bin/env python3
"""Measure production reference operations without RPC or actual VM work."""

import argparse
import hashlib
import io
import json
import os
import shutil
import statistics
import subprocess
import tarfile
from pathlib import Path

from host_build import build

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent


def summarize(samples):
    operations = {"reference_get", "id_hit_early", "id_hit_late", "id_miss"}
    expected = {
        (size, operation, trial)
        for size in (0, 32768, 131072, 262144)
        for operation in operations
        for trial in range(11)
    }
    seen, groups = set(), {}
    for sample in samples:
        key = (sample["high_water_refs"], sample["operation"], sample["trial"])
        calls = 256 if sample["operation"] == "id_miss" else 1000
        if (
            key not in expected
            or key in seen
            or sample["calls"] != calls
            or sample["cpu_ns"] <= 0
            or sample["allocations"] < 0
        ):
            raise ValueError("Invalid or duplicate reference-cost sample")
        seen.add(key)
        if sample["trial"]:
            groups.setdefault(key[:2], []).append(sample)
    if seen != expected:
        raise ValueError("Incomplete reference-cost trials")
    result = []
    for (size, operation), rows in groups.items():
        values = [r["cpu_ns"] / r["calls"] / 1000 for r in rows]
        result.append(
            {
                "high_water_refs": size,
                "operation": operation,
                "capacity": max(r["capacity"] for r in rows),
                "median_cpu_us": statistics.median(values),
                "min_cpu_us": min(values),
                "max_cpu_us": max(values),
                "trials": len(rows),
            }
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--android-ndk", type=Path)
    parser.add_argument("--jni-include", type=Path)
    parser.add_argument("--ffi-include", type=Path)
    parser.add_argument("--device")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument(
        "--source-ref",
        help="Build production stores/references from a Git commit",
    )
    args = parser.parse_args()
    if args.device and not args.android_ndk:
        parser.error("--device requires --android-ndk")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name in ("summary.json", "samples.jsonl"):
        (output / name).unlink(missing_ok=True)
    source = PROJECT
    revision = None
    if args.source_ref:
        revision = subprocess.check_output(
            ["git", "rev-parse", "--verify", args.source_ref + "^{commit}"],
            cwd=ROOT,
            text=True,
        ).strip()
        archive = subprocess.check_output(
            [
                "git",
                "archive",
                revision,
                "android-jni/build.zig",
                "android-jni/src/host",
            ],
            cwd=ROOT,
        )
        snapshot = output / ("source-" + revision)
        snapshot.mkdir(exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(archive)) as files:
            files.extractall(snapshot, filter="data")
        source = snapshot / "android-jni"
    includes = [
        source / "src/host",
        PROJECT / "include",
        PROJECT / "src/common",
        ROOT / "core/include",
        ROOT / "bionic-dlopen/include",
    ]
    if args.android_ndk:
        if not args.jni_include or not args.ffi_include:
            parser.error("Android requires --jni-include and --ffi-include")
        compiler = (
            args.android_ndk
            / "toolchains/llvm/prebuilt/linux-x86_64/bin"
            / "aarch64-linux-android29-clang"
        ).resolve()
        includes += [args.jni_include.resolve(), args.ffi_include.resolve()]
        target = "aarch64-linux-android.29"
    else:
        javac = shutil.which("javac")
        cc = shutil.which(os.environ.get("CC", "clang"))
        if not javac or not cc:
            parser.error("Desktop measurement requires javac and a C compiler")
        jdk = Path(javac).resolve().parents[1]
        compiler = Path(cc).resolve()
        includes += [jdk / "include", jdk / "include/linux"]
        target = "native"
    host_object = output / "host_store.o"
    build(host_object, target, project=source)
    binary = output / "reference_cost"
    command = [
        str(compiler),
        "-std=gnu17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        *("-I" + str(p) for p in includes),
        str(PROJECT / "tests/native/reference_cost.c"),
        str(source / "src/host/references.c"),
        str(host_object),
        "-pthread",
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True)
    inputs = {
        "target": target,
        "source_revision": revision,
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "compile": command,
        "clock": "CLOCK_THREAD_CPUTIME_ID",
        "zig_optimize": "ReleaseSafe",
        "warmup_trials": 1,
        "measured_trials": 10,
        "scope": "Production C/Zig reference paths, minimal VM double; no RPC, ART or HotSpot",
        "runner_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "sources_sha256": {
            "android-jni/" + str(p.relative_to(source)): hashlib.sha256(
                p.read_bytes()
            ).hexdigest()
            for p in [
                source / "src/host/references.c",
                source / "src/host/slot_store.zig",
                source / "src/host/slot_index.zig",
            ]
            if p.is_file()
        },
    }
    inputs["sources_sha256"]["android-jni/tests/native/reference_cost.c"] = (
        hashlib.sha256(
            (PROJECT / "tests/native/reference_cost.c").read_bytes()
        ).hexdigest()
    )
    (output / "inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
    if args.build_only:
        return
    execute = [str(binary)]
    if args.device:
        remote = (
            "/data/local/tmp/gw-reference-cost-"
            + hashlib.sha256(binary.read_bytes()).hexdigest()[:16]
        )
        adb = ["adb", "-s", args.device]
        subprocess.run(adb + ["push", str(binary), remote], check=True)
        subprocess.run(adb + ["shell", "chmod", "700", remote], check=True)
        execute = adb + ["shell", remote]
        (output / "device.txt").write_bytes(
            subprocess.check_output(adb + ["shell", "getprop"])
        )
    elif args.android_ndk:
        parser.error("Use --device or --build-only for an Android binary")
    samples = []
    try:
        with (output / "samples.jsonl").open("w") as log:
            for size in (0, 32768, 131072, 262144):
                run = subprocess.run(
                    execute + [str(size)],
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=180,
                )
                log.write(run.stdout)
                log.flush()
                samples.extend(
                    json.loads(line) for line in run.stdout.splitlines()
                )
    finally:
        if args.device:
            subprocess.run(adb + ["shell", "rm", "-f", remote], check=True)
    summary = summarize(samples)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("Reference cost: " + str(output))


if __name__ == "__main__":
    main()
