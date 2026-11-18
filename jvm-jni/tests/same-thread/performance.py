"""Measure fresh-process ART/HotSpot worker-domain pairs in one installed APK."""

import argparse
import datetime
import gzip
import hashlib
import json
import math
import shlex
import statistics
import subprocess
import time
from pathlib import Path

CASES = [
    "scalar",
    "callback",
    "nested4",
    "array128",
    "array65536",
    "compute10000",
]
PACKAGE = "dev.glibc.worker.domains.hotspot"
ROOT = Path(__file__).resolve().parents[3]


def quantile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def analyze(runs):
    summary = []
    for threads in sorted({run["threads"] for run in runs}):
        for case in CASES:
            pairs = []
            for pair in sorted(
                {r["pair"] for r in runs if r["threads"] == threads}
            ):
                group = {
                    r["backend"]: r["cases"][case]
                    for r in runs
                    if r["threads"] == threads and r["pair"] == pair
                }
                if "worker" not in group or "domain" not in group:
                    continue
                worker, domain = group["worker"], group["domain"]
                pairs.append(
                    {
                        "pair": pair,
                        **{
                            key: {
                                "worker": worker[key],
                                "domain": domain[key],
                                "reduction_pct": 100
                                * (1 - domain[key] / worker[key]),
                            }
                            for key in (
                                "batch_median_ns_per_call",
                                "call_p99_ns",
                                "cpu_ns_per_call",
                                "wall_ns_per_call",
                            )
                        },
                    }
                )
            if not pairs:
                continue
            row = {"threads": threads, "case": case, "pairs": pairs}
            for key in (
                "batch_median_ns_per_call",
                "call_p99_ns",
                "cpu_ns_per_call",
                "wall_ns_per_call",
            ):
                row[key] = {
                    "worker": statistics.median(
                        p[key]["worker"] for p in pairs
                    ),
                    "domain": statistics.median(
                        p[key]["domain"] for p in pairs
                    ),
                    "paired_median_reduction_pct": statistics.median(
                        p[key]["reduction_pct"] for p in pairs
                    ),
                    "domain_lower_pairs": sum(
                        p[key]["domain"] < p[key]["worker"] for p in pairs
                    ),
                }
            direct = [
                r["cases"][case]
                for r in runs
                if r["threads"] == threads and r["backend"] == "direct"
            ]
            if direct:
                row["direct_reference"] = {
                    key: statistics.median(d[key] for d in direct)
                    for key in direct[0]
                }
            summary.append(row)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=10)
    parser.add_argument(
        "--threads", type=int, nargs="+", choices=[1, 8], default=[1, 8]
    )
    args = parser.parse_args()
    if args.pairs < 1:
        parser.error("pairs must be positive")
    manifest = json.loads((args.build / "manifest.json").read_text())
    for name, digest in manifest["sha256"].items():
        if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            parser.error(f"Changed build input: {name}")
    if not manifest.get("late_signal") or not manifest.get(
        "art_signal_profile"
    ):
        parser.error("Requires the pinned ART late-signal build")
    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.build / "performance" / stamp
    output.mkdir(parents=True)
    apk = (
        ROOT
        / "jvm-jni/build/tests/same-thread/android-app/outputs/apk/debug/app-debug.apk"
    )
    metadata = {
        "native": manifest,
        "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(
            Path(__file__).read_bytes()
        ).hexdigest(),
        "pairs": args.pairs,
        "threads": args.threads,
        "serial": args.serial,
        "scope": "ART+HotSpot same process; System.load JNI; no CheckJNI/JFR/forced GC in timed windows",
    }
    (output / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    adb = ["adb", "-s", args.serial]

    def shell(*arguments, check=True):
        return subprocess.run(
            [*adb, "shell", shlex.join(arguments)],
            capture_output=True,
            text=True,
            timeout=30,
            check=check,
        ).stdout

    installed_path = (
        shell("pm", "path", PACKAGE).strip().removeprefix("package:")
    )
    installed_hash = shell("sha256sum", installed_path).split()[0]
    if installed_hash != metadata["apk_sha256"]:
        raise SystemExit("Installed APK differs from local artifact")
    (output / "device.txt").write_text(shell("getprop"))
    shell("run-as", PACKAGE, "touch", "files/inline-vm.flag")
    runs = []
    print(output, flush=True)

    def run(backend, threads, pair):
        label = f"t{threads}-p{pair:02d}-{backend}"
        before = shell("dumpsys", "battery") + shell(
            "dumpsys", "thermalservice"
        )
        shell("am", "force-stop", PACKAGE)
        shell("run-as", PACKAGE, "rm", "-f", "files/native-report.txt")
        shell(
            "am",
            "start",
            "-n",
            PACKAGE + "/.MainActivity",
            "--ez",
            "benchmark",
            "true",
            "--es",
            "backend",
            backend,
            "--ei",
            "threads",
            str(threads),
        )
        deadline = time.monotonic() + 180
        report = ""
        while time.monotonic() < deadline:
            report = shell(
                "run-as", PACKAGE, "cat", "files/native-report.txt", check=False
            )
            if any(
                marker in report
                for marker in (
                    "APP_DOMAIN_EXIT=",
                    "APP_DOMAIN_RESULT=",
                    "JNI_DOMAIN_FAILURE",
                    "LATE_SIGNAL_REJECT",
                )
            ):
                break
            time.sleep(1)
        with gzip.open(output / (label + ".txt.gz"), "wt") as file:
            file.write(report)
        (output / (label + "-device.txt")).write_text(
            before
            + "\nAFTER\n"
            + shell("dumpsys", "battery")
            + shell("dumpsys", "thermalservice")
        )

        def rows(marker):
            return [
                json.loads(line[len(marker) + 1 :])
                for line in report.splitlines()
                if line.startswith(marker + " ")
            ]

        result = rows("SYSTEM_BENCH_RESULT")
        life = rows("SYSTEM_LOAD_LIFECYCLE")
        install = rows("LATE_SIGNAL_INSTALL")
        samples, resources = (
            rows("SYSTEM_BENCH_SAMPLES"),
            rows("SYSTEM_BENCH_RESOURCE"),
        )
        expected_loads = 0 if backend == "direct" else 1
        if (
            "APP_DOMAIN_INLINE=1" not in report
            or "APP_DOMAIN_EXIT=0" not in report
            or result
            != [
                {
                    "result": "PASS",
                    "direct": backend == "direct",
                    "loads": expected_loads,
                    "unloads": expected_loads,
                }
            ]
            or len(life) != 1
            or not life[0]["vm_destroyed"]
            or (life[0]["domains_created"] > 0) != (backend == "domain")
            or len(install) != 1
            or not install[0].get("art_chain")
            or any(
                marker in report
                for marker in (
                    "WARNING in native method",
                    "handler modified",
                    "LATE_SIGNAL_PRESSURE",
                    "Exception in thread",
                )
            )
            or len(samples) != threads * len(CASES)
            or len(resources) != len(CASES)
            or {(s["thread"], s["case"]) for s in samples}
            != {(t, c) for t in range(threads) for c in CASES}
            or {r["case"] for r in resources} != set(CASES)
        ):
            raise SystemExit(f"Invalid benchmark/lifecycle: {label}; {output}")
        cases = {}
        for case in CASES:
            selection = [s for s in samples if s["case"] == case]
            iterations = 64 if case == "array65536" else 256
            if any(
                s["iterations"] != iterations
                or s["warmup_batches"] != 8
                or len(s["batch_ns"]) != 30
                or len(s["call_ns"]) != 512
                or any(n <= 0 for n in s["batch_ns"] + s["call_ns"])
                for s in selection
            ):
                raise SystemExit("Incomplete timing samples")
            resource = next(r for r in resources if r["case"] == case)
            count = iterations * 30 * threads
            if resource["calls"] != count or resource["cpu_ns"] <= 0:
                raise SystemExit("Invalid CPU accounting")
            cases[case] = {
                "batch_median_ns_per_call": statistics.median(
                    n / iterations for s in selection for n in s["batch_ns"]
                ),
                "call_p50_ns": quantile(
                    [n for s in selection for n in s["call_ns"]], 0.50
                ),
                "call_p99_ns": quantile(
                    [n for s in selection for n in s["call_ns"]], 0.99
                ),
                "cpu_ns_per_call": resource["cpu_ns"] / count,
                "wall_ns_per_call": resource["wall_ns"] / count,
                "voluntary_per_call": resource["voluntary"] / count,
                "involuntary_per_call": resource["involuntary"] / count,
            }
        runs.append(
            {
                "backend": backend,
                "threads": threads,
                "pair": pair,
                "cases": cases,
                "lifecycle": life[0],
            }
        )
        (output / "summary.json").write_text(
            json.dumps({"runs": runs, "comparisons": analyze(runs)}, indent=2)
            + "\n"
        )
        print(label, "PASS", flush=True)
        time.sleep(2)

    for threads in args.threads:
        run("direct", threads, 0)
        for pair in range(1, args.pairs + 1):
            for backend in (
                ["worker", "domain"] if pair % 2 else ["domain", "worker"]
            ):
                run(backend, threads, pair)
            if pair == max(1, args.pairs // 2):
                run("direct", threads, pair)
        run("direct", threads, args.pairs + 1)
    shell("am", "force-stop", PACKAGE)
    print("COMPLETE", output, flush=True)


if __name__ == "__main__":
    main()
