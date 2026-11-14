#!/usr/bin/env python3
"""Compare retained runtime revisions using identical scalar/callback payloads.

Functional suites may evolve between revisions. Each APK must pass its own
revision's checker; only the identical measured workload is compared. This is
an artifact comparison, not attribution of every difference to one patch.
"""

import argparse
import hashlib
import importlib.util
import io
import json
import subprocess
import sys
from pathlib import Path
from zipfile import ZipFile

from compare_waiting import run_order, summarize

ROOT = Path(__file__).resolve().parents[3]
TESTS = ROOT / "jvm-jni/tests"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def workload(apk_path):
    with ZipFile(apk_path) as apk:
        names = [
            "lib/arm64-v8a/" + name
            for name in (
                "libguest_bench.so",
                "libjava.so",
                "libjvm.so",
                "libjli.so",
            )
        ] + ["assets/android-jre/lib/modules", "assets/android-jre/release"]
        result = {name: digest(apk.read(name)) for name in names}
        with ZipFile(
            io.BytesIO(apk.read("assets/android-jre/smoke.jar"))
        ) as jar:
            result.update(
                {
                    name: digest(jar.read(name))
                    for name in jar.namelist()
                    if name.startswith("benchmark/") and name.endswith(".class")
                }
            )
    if "benchmark/ChildModule.class" not in result:
        raise ValueError("Missing measured Java workload")
    return result


def capture(device, output, label):
    for service in ("battery", "thermalservice"):
        value = subprocess.check_output(
            ["adb", "-s", device, "shell", "dumpsys", service],
            timeout=30,
        )
        (output / (label + "-" + service + ".txt")).write_bytes(value)


def single():
    parser = argparse.ArgumentParser()
    parser.add_argument("--single", action="store_true")
    parser.add_argument("--checker", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    sys.path.insert(0, str(TESTS))
    import run

    spec = importlib.util.spec_from_file_location(
        "revision_checker", args.checker
    )
    checker = importlib.util.module_from_spec(spec)
    # The saved revision's checker resolves the maintained artifact catalog.
    checker.__file__ = str(TESTS / "check.py")
    spec.loader.exec_module(checker)
    run.verify = checker.verify
    sys.argv = [str(TESTS / "run.py"), *remaining]
    run.main()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--candidate-ref", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs-per-variant", type=int, default=10)
    args = parser.parse_args()
    order = run_order(args.runs_per_variant)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    apks = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    payload = workload(apks["A"])
    if payload != workload(apks["B"]):
        parser.error("Measured workload or fixed JRE differs")
    manifest = {
        "device": args.device,
        "order": order,
        "workload": payload,
        "mode": "normal JIT, CheckJNI off",
        "variants": {},
    }
    for variant, ref in (("A", args.baseline_ref), ("B", args.candidate_ref)):
        revision = subprocess.check_output(
            ["git", "rev-parse", ref + "^{commit}"],
            cwd=ROOT,
            text=True,
        ).strip()
        checker = subprocess.check_output(
            ["git", "show", revision + ":jvm-jni/tests/check.py"],
            cwd=ROOT,
        )
        (output / (variant + "-check.py")).write_bytes(checker)
        manifest["variants"][variant] = {
            "apk": str(apks[variant]),
            "apk_sha256": digest(apks[variant].read_bytes()),
            "checker_revision": revision,
            "checker_sha256": digest(checker),
        }
    manifest["limits"] = (
        "Retained APKs: checker revision does not attest their build source. "
        "Untimed suites, Android harness and GNU runtime may differ. "
        "Quantiles are medians of per-trial quantiles, not pooled percentiles. "
        "CPU includes the whole process and worker teardown."
    )
    (output / "inputs.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for label in order:
        variant = label[0]
        print("Starting " + label, flush=True)
        capture(args.device, output, label + "-before")
        command = [
            sys.executable,
            str(Path(__file__).resolve()),
            "--single",
            "--checker",
            str(output / (variant + "-check.py")),
            "--group",
            "coverage",
            "--device",
            args.device,
            "--reuse-apk",
            "--apk",
            str(apks[variant]),
            "--benchmark",
            "--benchmark-jit",
            "--output",
            str(output / label),
        ]
        with (output / (label + "-driver.log")).open("w") as log:
            subprocess.run(
                command,
                cwd=ROOT,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
                timeout=300,
            )
        capture(args.device, output, label + "-after")
        print("Passed " + label, flush=True)
    pairs = list(zip(order[::2], order[1::2], strict=True))
    result = summarize(
        [output / label for label in order if label[0] == "A"],
        [output / label for label in order if label[0] == "B"],
        pairs,
    )
    (output / "comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "--single",
            "--checker",
            str(output / "B-check.py"),
            "--group",
            "smoke",
            "--device",
            args.device,
            "--reuse-apk",
            "--apk",
            str(apks["B"]),
            "--output",
            str(output / "smoke-final"),
        ],
        cwd=ROOT,
        check=True,
        timeout=300,
    )
    print("Comparison complete: " + str(output), flush=True)


if __name__ == "__main__":
    single() if "--single" in sys.argv else main()
