#!/usr/bin/env python3
"""Validate owner lifecycle dispatch and compare retained-capacity pump costs."""

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

from compare_reference_cost import capture

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent
CAPACITIES = (16, 256, 1024, 4096)
FIXTURE = PROJECT / "tests/native/owner_work.c"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(
    output, source, compiler, jni, ffi, *, sanitizer=None, baseline=False
):
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "owner_work"
    command = [
        str(compiler),
        "-std=gnu17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-ffunction-sections",
        "-fdata-sections",
        "-Wl,--gc-sections",
        *(["-DGW_OWNER_BASELINE=1"] if baseline else []),
        *(
            []
            if baseline
            else ["-Wl,--wrap=calloc", "-Wl,--wrap=pthread_create"]
        ),
        *(
            ["-fsanitize=" + sanitizer, "-fno-omit-frame-pointer"]
            if sanitizer
            else []
        ),
        *(
            "-I" + str(p)
            for p in (
                source / "src/common",
                source / "src/guest",
                PROJECT / "include",
                ROOT / "core/include",
                ROOT / "bionic-dlopen/include",
                jni,
                output.parent,
            )
        ),
        *ffi,
        str(FIXTURE),
        str(source / "src/guest/full_guest.c"),
        str(source / "src/guest/rpc.c"),
        "-pthread",
        "-o",
        str(binary),
    ]
    subprocess.run(command, check=True)
    (output / "inputs.json").write_text(
        json.dumps(
            {
                "compile": command,
                "binary_sha256": digest(binary),
                "fixture_sha256": digest(FIXTURE),
                "sources_sha256": {
                    str(p.relative_to(source)): digest(p)
                    for p in sorted((source / "src/common").glob("*.h"))
                    + [
                        source / "src/guest/full_guest.c",
                        source / "src/guest/rpc.c",
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    return binary


def summarize(text, capacity):
    samples = [json.loads(line) for line in text.splitlines()]
    if (
        len(samples) != 11
        or {row["trial"] for row in samples} != set(range(11))
        or any(
            r["capacity"] != capacity or r["calls"] != 2000 or r["cpu_ns"] <= 0
            for r in samples
        )
    ):
        raise ValueError("Invalid owner-pump measurement")
    return statistics.median(
        r["cpu_ns"] / r["calls"] / 1000 for r in samples if r["trial"]
    )


def compare(output, binaries, device):
    adb = ["adb", "-s", device] if device else None
    remotes = {}
    runs = {}
    order = [
        v + str(block * 4 + i)
        for block in range(5)
        for i, v in enumerate("ABBA" if block % 2 == 0 else "BAAB")
    ]
    try:
        for variant, binary in binaries.items():
            if adb:
                remote = "/data/local/tmp/gw-owner-" + digest(binary)[:16]
                remotes[variant] = remote
                subprocess.run(adb + ["push", str(binary), remote], check=True)
                subprocess.run(
                    adb + ["shell", "chmod", "700", remote], check=True
                )
        for label in order:
            if adb:
                capture(adb, output, label + "-before")
            values = {}
            for capacity in CAPACITIES:
                binary = binaries[label[0]]
                command = (
                    adb + ["shell", remotes[label[0]]] if adb else [str(binary)]
                )
                run = subprocess.run(
                    command + [str(capacity)],
                    capture_output=True,
                    text=True,
                    timeout=120,
                    check=False,
                )
                (output / f"{label}-{capacity}.jsonl").write_text(run.stdout)
                (output / f"{label}-{capacity}.stderr").write_text(run.stderr)
                run.check_returncode()
                values[capacity] = summarize(run.stdout, capacity)
            runs[label] = values
            if adb:
                capture(adb, output, label + "-after")
            print("Owner pump passed " + label, flush=True)
    finally:
        if adb:
            for remote in remotes.values():
                subprocess.run(adb + ["shell", "rm", "-f", remote], check=True)
    comparisons = []
    for capacity in CAPACITIES:
        values = {label: row[capacity] for label, row in runs.items()}
        changes = []
        for left, right in zip(order[::2], order[1::2], strict=True):
            a, b = (left, right) if left[0] == "A" else (right, left)
            changes.append(100 * (values[b] / values[a] - 1))
        comparisons.append(
            {
                "capacity": capacity,
                "baseline_median_cpu_us": statistics.median(
                    v for k, v in values.items() if k[0] == "A"
                ),
                "candidate_median_cpu_us": statistics.median(
                    v for k, v in values.items() if k[0] == "B"
                ),
                "paired_median_percent": statistics.median(changes),
                "candidate_lower_pairs": sum(v < 0 for v in changes),
                "per_run_cpu_us": values,
            }
        )
    return {"pairs": 10, "order": order, "comparison": comparisons}


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-ref")
    parser.add_argument("--android-ndk", type=Path)
    parser.add_argument(
        "--jni-include", type=Path, default=PROJECT / "build/jni-include"
    )
    parser.add_argument("--ffi-include", type=Path)
    parser.add_argument("--device")
    parser.add_argument(
        "--sanitizer",
        choices=("address,undefined", "thread"),
        default="address,undefined",
    )
    args = parser.parse_args()
    if args.device and not (args.android_ndk and args.baseline_ref):
        parser.error("--device requires --android-ndk and --baseline-ref")
    if args.android_ndk and not (
        args.ffi_include and args.device and args.baseline_ref
    ):
        parser.error(
            "Android comparison requires --ffi-include, --device and --baseline-ref"
        )
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    if not (args.jni_include / "jni.h").is_file():
        parser.error("Provide --jni-include with prepared JNI headers")
    return args, output


def prepare(output, jni):
    subprocess.run(
        [
            "python3",
            str(PROJECT / "scripts/gen-jni.py"),
            "--jni-header",
            str(jni / "jni.h"),
            "--output",
            str(output / "generated"),
        ],
        check=True,
    )
    native_ffi = subprocess.check_output(
        ["pkg-config", "--cflags", "libffi"], text=True
    ).split()
    cc = shutil.which(os.environ.get("SANITIZER_CC", "clang"))
    if not cc:
        raise SystemExit("Install a sanitizer-capable C compiler")
    return cc, native_ffi


def validate(output, cc, jni, native_ffi, sanitizer):
    test = build(
        output / "sanitized",
        PROJECT,
        cc,
        jni,
        native_ffi,
        sanitizer=sanitizer,
    )
    run = subprocess.run(
        [str(test)],
        capture_output=True,
        text=True,
        timeout=90,
        check=False,
        env={
            **os.environ,
            "UBSAN_OPTIONS": "halt_on_error=1",
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        },
    )
    (output / "validation.log").write_text(run.stdout + run.stderr)
    run.check_returncode()
    result = json.loads(run.stdout)
    if result["result"] != "PASS":
        raise ValueError("Owner lifecycle validation did not pass")
    return result


def snapshot_guest(output, reference):
    revision = subprocess.check_output(
        ["git", "rev-parse", "--verify", reference + "^{commit}"],
        cwd=ROOT,
        text=True,
    ).strip()
    archive = subprocess.check_output(
        [
            "git",
            "archive",
            revision,
            "android-jni/src/common",
            "android-jni/src/guest",
        ],
        cwd=ROOT,
    )
    snapshot = output / "baseline-source"
    with tarfile.open(fileobj=io.BytesIO(archive)) as files:
        files.extractall(snapshot, filter="data")
    return revision, snapshot / "android-jni"


def compare_revision(args, output, cc, native_ffi):
    revision, source = snapshot_guest(output, args.baseline_ref)
    compiler, ffi = cc, native_ffi
    if args.android_ndk:
        compiler = (
            args.android_ndk
            / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
        )
        ffi = ["-I" + str(args.ffi_include)]
    binaries = {
        "A": build(
            output / "A",
            source,
            compiler,
            args.jni_include,
            ffi,
            baseline=True,
        ),
        "B": build(output / "B", PROJECT, compiler, args.jni_include, ffi),
    }
    result = compare(output, binaries, args.device)
    result["baseline_revision"] = revision
    result["inputs"] = {
        variant: json.loads((binary.parent / "inputs.json").read_text())
        for variant, binary in binaries.items()
    }
    result["scope"] = (
        "Production fg_pump with no-op fg_execute, no pending lifecycle work; batch mean CPU cost, not JNI/RPC latency or game performance"
    )
    result["target"] = "aarch64-linux-android.29" if args.device else "native"
    return result


def main():
    args, output = arguments()
    cc, native_ffi = prepare(output, args.jni_include)
    result = {
        "result": "PASS",
        "validation": validate(
            output, cc, args.jni_include, native_ffi, args.sanitizer
        ),
    }
    if args.baseline_ref:
        result.update(compare_revision(args, output, cc, native_ffi))
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Owner lifecycle checks: " + str(output / "summary.json"))


if __name__ == "__main__":
    main()
