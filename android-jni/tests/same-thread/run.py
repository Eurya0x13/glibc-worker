"""Validate production JNI paths, then optionally collect alternating paired runs."""

import argparse
import datetime
import hashlib
import json
import os
import shlex
import shutil
import statistics
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--jdk", type=Path, required=True)
parser.add_argument("--serial")
parser.add_argument("--pairs", type=int, default=0)
parser.add_argument("--threads", type=int, choices=[1, 8], default=1)
args = parser.parse_args()
if args.pairs < 0:
    parser.error("--pairs must be nonnegative")
build = args.build.resolve()
manifest = json.loads((build / "manifest.json").read_text())
for name, digest in manifest["sha256"].items():
    if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
        parser.error(f"Changed build input: {name}; rebuild before measuring")
android = manifest["host_libc"] == "bionic"
if android != bool(args.serial):
    parser.error("Android requires --serial; desktop must omit it")
identity = hashlib.sha256((build / "manifest.json").read_bytes()).hexdigest()[
    :16
]
stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S%fZ")
evidence = build / "runs" / stamp
evidence.mkdir(parents=True)
runtime = Path(manifest["runtime"])
environment = os.environ.copy()
environment.pop("GW_DOMAIN_MODE", None)
if android:
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/gw-production-jni-" + identity
    payload = build / "payload"
    payload.mkdir(exist_ok=True)
    home = payload / "jre"
    (home / "lib/server").mkdir(parents=True, exist_ok=True)
    assets = root / "jvm-jni/build/tests/android/assets/android-jre"
    libs = root / "jvm-jni/build/tests/android/jniLibs/arm64-v8a"
    for item in ["release", "conf", "lib/modules", "lib/tzdb.dat"]:
        if (assets / item).is_dir():
            shutil.copytree(assets / item, home / item, dirs_exist_ok=True)
        else:
            shutil.copyfile(assets / item, home / item)
    for name in ["jvm", "java", "verify", "net", "nio", "zip", "jimage"]:
        filename = "lib" + name + ".so"
        shutil.copyfile(
            libs / filename,
            home / "lib" / ("server/" if name == "jvm" else "") / filename,
        )
    for name in [
        "host",
        "guest",
        "probe.so",
        "conformance.so",
        "domain-probe.jar",
        "libgcc_s.so.1",
    ]:
        shutil.copyfile(build / name, payload / name)
    for name in [manifest["loader"], "libc.so.6", "libm.so.6"]:
        shutil.copyfile(runtime / name, payload / name)
    subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
    subprocess.run([*adb, "push", str(payload) + "/.", remote], check=True)
    env = {
        "GW_CONFORMANCE": remote + "/conformance.so",
        "GW_JAVA_HOME": remote + "/jre",
        "GW_JVM_LIB": remote + "/jre/lib/server/libjvm.so",
        "GW_JAVA_BOOT_LIB": remote + "/jre/lib",
        "GW_DOMAIN_JAR": remote + "/domain-probe.jar",
        "LD_LIBRARY_PATH": remote + "/jre/lib:" + remote + "/jre/lib/server",
    }
    native = [
        "/system/bin/linker64",
        remote + "/host",
        remote + "/" + manifest["loader"],
        remote,
        remote + "/guest",
        remote + "/probe.so",
    ]
else:
    home = args.jdk.resolve()
    env = {
        "GW_CONFORMANCE": str(build / "conformance.so"),
        "GW_JAVA_HOME": str(home),
        "GW_JVM_LIB": str(home / "lib/server/libjvm.so"),
        "GW_JAVA_BOOT_LIB": str(home / "lib"),
        "GW_DOMAIN_JAR": str(build / "domain-probe.jar"),
    }
    native = [
        str(build / "host"),
        str(runtime / manifest["loader"]),
        str(runtime) + ":" + str(build),
        str(build / "guest"),
        str(build / "probe.so"),
    ]


def run(mode, threads, measure, label, fatal=False, reject=False):
    requested = mode
    mode = "domain" if requested is None else requested
    settings = {
        **env,
        "GW_THREADS": str(threads),
        "GW_BENCH": str(int(measure)),
        "GW_FATAL": str(int(fatal)),
    }
    if requested is not None:
        settings["GW_DOMAIN_MODE"] = requested
    command = (
        [
            *adb,
            "shell",
            "cd "
            + shlex.quote(remote)
            + " && /system/bin/timeout -s KILL 180 env -u GW_DOMAIN_MODE "
            + shlex.join([k + "=" + v for k, v in settings.items()] + native),
        ]
        if android
        else native
    )
    result = subprocess.run(
        command,
        env={**environment, **settings},
        capture_output=True,
        text=True,
        check=False,
        timeout=190,
        cwd=build,
    )
    (evidence / (label + ".stdout")).write_text(result.stdout)
    (evidence / (label + ".stderr")).write_text(result.stderr)
    if reject:
        output = result.stdout + result.stderr
        if (
            result.returncode != 2
            or "JNI_DOMAIN_REJECT invalid_backend" not in output
        ):
            raise SystemExit(
                f"Invalid backend was not rejected: {label}; {evidence}"
            )
        print(label, "expected backend rejection", flush=True)
        return {"requested": requested, "rejected": True, "command": command}
    if fatal:
        output = result.stdout + result.stderr
        if (
            result.returncode not in (-6, 134)
            or "JNI_EXPECTED_FATAL stage=guest-before-call" not in output
            or "FATAL ERROR in native method: fixture requested fatal"
            not in output
        ):
            raise SystemExit(
                f"FatalError did not abort as expected: {label}; {evidence}"
            )
        print(label, "expected SIGABRT", flush=True)
        return {
            "backend": mode,
            "returncode": result.returncode,
            "expected_sigabrt": True,
        }
    rows = [
        json.loads(line.removeprefix("JNI_DOMAIN_RESULT "))
        for line in result.stdout.splitlines()
        if line.startswith("JNI_DOMAIN_RESULT ")
    ]
    selection = [
        json.loads(line.removeprefix("JNI_DOMAIN_SELECTION "))
        for line in result.stdout.splitlines()
        if line.startswith("JNI_DOMAIN_SELECTION ")
    ]
    if selection != [
        {
            "requested": requested if requested is not None else "default",
            "backend": mode,
        }
    ]:
        raise SystemExit(f"Wrong backend selection: {label}; {evidence}")
    samples = [
        json.loads(line.removeprefix("JNI_DOMAIN_BENCH "))
        for line in result.stdout.splitlines()
        if line.startswith("JNI_DOMAIN_BENCH ")
    ]
    if (
        result.returncode
        or len(rows) != 1
        or rows[0]
        != {
            "result": "PASS",
            "backend": mode,
            "threads": threads,
            "benchmark": int(measure),
            "errors": 0,
        }
        or "WARNING in native method" in result.stdout + result.stderr
        or "handler modified" in result.stdout + result.stderr
    ):
        raise SystemExit(
            f"FAILED {label}, exit={result.returncode}; evidence: {evidence}"
        )
    expected = {
        (thread, case)
        for thread in range(threads)
        for case in ["scalar", "callback", "nested4", "array128", "array65536"]
    }
    actual = {(row["thread"], row["case"]) for row in samples}
    if measure and (
        len(samples) != len(expected)
        or actual != expected
        or any(
            row["iterations"] != (64 if row["case"] == "array65536" else 256)
            or len(row["batch_ns"]) != 30
            or any(not isinstance(ns, int) or ns <= 0 for ns in row["batch_ns"])
            for row in samples
        )
    ):
        raise SystemExit(f"Incomplete or invalid benchmark: {label}")
    if not measure and samples:
        raise SystemExit(f"Unexpected benchmark in compatibility run: {label}")
    coverage = []
    if not measure:
        coverage = [
            json.loads(line.removeprefix("JNI_DOMAIN_COVERAGE "))
            for line in result.stdout.splitlines()
            if line.startswith("JNI_DOMAIN_COVERAGE ")
        ]
        contracts = [
            json.loads(line.removeprefix("JNI_DOMAIN_CONFORMANCE "))
            for line in result.stdout.splitlines()
            if line.startswith("JNI_DOMAIN_CONFORMANCE ")
        ]
        coop = [
            json.loads(line.removeprefix("JNI_DOMAIN_VM_COOP "))
            for line in result.stdout.splitlines()
            if line.startswith("JNI_DOMAIN_VM_COOP ")
        ]
        guests = [
            json.loads(line.removeprefix("JNI_DOMAIN_GUEST_THREADS "))
            for line in result.stdout.splitlines()
            if line.startswith("JNI_DOMAIN_GUEST_THREADS ")
        ]
        if (
            len(coverage) != 1
            or coverage[0]["apis"] != 234
            or coverage[0]["contexts"] < 48
            or coverage[0]["sr_deliveries"] <= 0
            or coverage[0]["domains_created"] != coverage[0]["domains_finished"]
            or (
                mode == "domain"
                and coverage[0]["domains_created"] < 98 + threads
            )
            or contracts
            != [
                {
                    "groups": 10,
                    "java_threads": 96,
                    "concurrent_threads": 32,
                    "reentry_depth": 32,
                    "monitor_increments": 960,
                }
            ]
            or len(coop) != 1
            or coop[0]["samples"] != 20
            or coop[0]["overlaps"] <= 0
            or coop[0].get("sr_deliveries", 0) <= 0
            or guests != [{"created": 32, "passed": 32}]
        ):
            observed = {
                "coverage": coverage,
                "conformance": contracts,
                "vm_cooperation": coop,
                "guest_threads": guests,
            }
            raise SystemExit(
                f"Incomplete conformance evidence: {label}; "
                f"{json.dumps(observed, sort_keys=True)}; {evidence}"
            )
    print(label, rows[0], flush=True)
    return {
        "result": rows[0],
        "samples": samples,
        "coverage": coverage,
        "command": command,
    }


results = {
    "build": identity,
    "execution": "android-shell" if android else "desktop-linux",
    "compatibility": [],
    "fatal_error": [],
    "backend_selection": [],
    "pairs": [],
    "production_ready": False,
}
(evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
results["backend_selection"] = [
    run(None, 8, False, "compat-default-8"),
    run("", 1, False, "reject-empty", reject=True),
    run("invalid", 1, False, "reject-invalid", reject=True),
]
for threads in [1, 8]:
    for mode in ["worker", "domain"]:
        results["compatibility"].append(
            run(mode, threads, False, f"compat-{mode}-{threads}")
        )
(evidence / "compatibility.json").write_text(
    json.dumps(results, indent=2) + "\n"
)
for mode in ["worker", "domain"]:
    results["fatal_error"].append(
        run(mode, 1, False, f"fatal-{mode}", fatal=True)
    )
(evidence / "compatibility.json").write_text(
    json.dumps(results, indent=2) + "\n"
)
for pair in range(args.pairs):
    modes = ["worker", "domain"] if pair % 2 == 0 else ["domain", "worker"]
    row = {"pair": pair, "order": modes}
    for mode in modes:
        row[mode] = run(mode, args.threads, True, f"pair-{pair:02}-{mode}")
    a = {
        (r["thread"], r["case"]): r["checksum"]
        for r in row["worker"]["samples"]
    }
    b = {
        (r["thread"], r["case"]): r["checksum"]
        for r in row["domain"]["samples"]
    }
    if a != b:
        raise SystemExit("Backend output mismatch")
    results["pairs"].append(row)
    (evidence / "results.json").write_text(json.dumps(results, indent=2) + "\n")
summary = {}
for case in ["scalar", "callback", "nested4", "array128", "array65536"]:
    values = {mode: [] for mode in ["worker", "domain"]}
    for pair in results["pairs"]:
        for mode, medians in values.items():
            samples = [
                ns / row["iterations"]
                for row in pair[mode]["samples"]
                if row["case"] == case
                for ns in row["batch_ns"]
            ]
            medians.append(statistics.median(samples))
    if values["worker"]:
        ratios = [
            b / a
            for a, b in zip(values["worker"], values["domain"], strict=True)
        ]
        summary[case] = {
            "worker_ns_per_call": statistics.median(values["worker"]),
            "domain_ns_per_call": statistics.median(values["domain"]),
            "paired_ratio_median": statistics.median(ratios),
            "paired_ratios": ratios,
            "faster_pairs": sum(r < 1 for r in ratios),
        }
(evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
print(evidence)
