#!/usr/bin/env python3
"""Run one of the two retained Android test groups on an explicit device."""

import argparse
import hashlib
import json
import os
import re
import subprocess
import time
import zipfile
from pathlib import Path

from evidence import RunEvidence
from check import verify
from payload import payload_points, verify_payload

ROOT = Path(__file__).resolve().parents[2]


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", choices=("smoke", "coverage"), required=True)
    parser.add_argument("--device", required=True)
    parser.add_argument(
        "--reuse-apk",
        action="store_true",
        help="Reuse this workspace build; still install it",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--apk", type=Path, help="Explicit retained baseline/candidate APK"
    )
    parser.add_argument(
        "--benchmark",
        action="store_true",
        help="Add measured workload to coverage",
    )
    parser.add_argument(
        "--benchmark-jit",
        action="store_true",
        help="Benchmark with normal JIT and CheckJNI off",
    )
    parser.add_argument("--payload-benchmark", choices=("host", "guest"))
    parser.add_argument("--payload-round", type=int, default=0)
    parser.add_argument(
        "--payload-profile", choices=("default", "threshold"), default="default"
    )
    parser.add_argument("--payload-diagnostic", action="store_true")
    parser.add_argument("--array-copy-cache", choices=("auto", "on", "off"))
    parser.add_argument(
        "--native-jar",
        choices=("lz4", "snappy", "rocksdb"),
        help="Extend coverage with one original GNU JNI JAR",
    )
    parser.add_argument(
        "--cpu-mask",
        help="Restrict benchmark Host/Guest threads to a hex CPU mask",
    )
    parser.add_argument(
        "--installed",
        action="store_true",
        help="Skip installation after verifying installed APK hash",
    )
    parser.add_argument("--timeout", type=int, default=600)
    return parser.parse_args()


def execute(args, output, attempt):

    adb = ["adb", "-s", args.device]
    abi = subprocess.check_output(
        adb + ["shell", "getprop", "ro.product.cpu.abi"], text=True
    ).strip()
    if abi not in ("arm64-v8a", "x86_64"):
        raise SystemExit("Unsupported device ABI: " + abi)

    env = {**os.environ, "GW_ABI": abi}
    if args.native_jar:
        env["GW_NATIVE_JARS"] = "1"
        if args.group != "coverage" or args.benchmark or args.payload_benchmark:
            raise SystemExit(
                "--native-jar requires coverage without --benchmark"
            )

    measured = args.benchmark or args.payload_benchmark
    if args.payload_diagnostic and not args.payload_benchmark:
        raise SystemExit("--payload-diagnostic requires --payload-benchmark")
    if args.payload_profile != "default" and not args.payload_benchmark:
        raise SystemExit("--payload-profile requires --payload-benchmark")
    if measured and args.group != "coverage":
        raise SystemExit("Benchmark is part of the coverage group")
    if args.payload_round < 0 or args.payload_benchmark and args.benchmark:
        raise SystemExit(
            "Choose one benchmark with a nonnegative payload round"
        )
    if args.benchmark_jit and not measured:
        raise SystemExit("--benchmark-jit requires a benchmark")
    if args.cpu_mask and (
        not measured
        or not re.fullmatch(r"[0-9a-fA-F]+", args.cpu_mask)
        or int(args.cpu_mask, 16) == 0
        or int(args.cpu_mask, 16).bit_length() > 64
    ):
        raise SystemExit(
            "--cpu-mask requires --benchmark and a nonzero 64-bit hex mask"
        )

    if not args.reuse_apk and not args.installed:
        subprocess.run(
            [
                "jvm-jni/tests/android/gradlew",
                "-p",
                "jvm-jni/tests/android",
                ":app:assembleDebug",
                "--console=plain",
            ],
            cwd=ROOT,
            env=env,
            check=True,
        )

    apk = (
        args.apk
        or ROOT
        / "jvm-jni/build/tests/android/app/outputs/apk/debug/app-debug.apk"
    ).resolve()
    if not apk.is_file():
        raise SystemExit("Build the test APK before --reuse-apk")
    with zipfile.ZipFile(apk) as archive:
        if "lib/" + abi + "/libworker_host.so" not in archive.namelist():
            raise SystemExit("Test APK does not match the device ABI: " + abi)
        if (
            args.native_jar
            and "assets/android-jre/native-jars.properties"
            not in archive.namelist()
        ):
            raise SystemExit("Selected APK was built without GW_NATIVE_JARS=1")

    if args.group == "coverage" and not args.apk:
        subprocess.run(
            [
                "python3",
                "bionic-dlopen/tests/elf_route.py",
                "--libraries",
                str(ROOT / "jvm-jni/build/tests/android/jniLibs" / abi),
                "--core-include",
                str(ROOT / "core/include"),
                "--output",
                str(output / "elf-route.json"),
            ],
            cwd=ROOT,
            env=env,
            check=True,
        )

    apk_hash = hashlib.sha256(apk.read_bytes()).hexdigest()
    attempt.update(abi=abi, apk_sha256=apk_hash)
    if not args.installed:
        subprocess.run(
            [
                "android",
                "run",
                "--device=" + args.device,
                "--apks=" + str(apk),
                "--activity=dev.glibc.worker.MainActivity",
                "--use-delta-install=false",
            ],
            cwd=ROOT,
            check=True,
        )

    remote = (
        subprocess.check_output(
            adb + ["shell", "pm", "path", "dev.glibc.worker"], text=True
        )
        .strip()
        .removeprefix("package:")
    )
    actual = subprocess.check_output(
        adb + ["shell", "sha256sum", remote], text=True
    ).split()[0]
    if actual != apk_hash:
        raise SystemExit("Installed APK differs from the selected test build")
    attempt.update(installed_apk_sha256=actual)

    subprocess.run(
        adb
        + [
            "shell",
            "am",
            "start",
            "-S",
            "-n",
            "dev.glibc.worker/.MainActivity",
            "--ez",
            "benchmark",
            str(args.benchmark).lower(),
        ]
        + ["--ez", "benchmark-jit", str(args.benchmark_jit).lower()]
        + (
            [
                "--es",
                "payload-benchmark",
                args.payload_benchmark,
                "--ei",
                "payload-round",
                str(args.payload_round),
            ]
            if args.payload_benchmark
            else []
        )
        + (["--es", "cpu-mask", args.cpu_mask] if args.cpu_mask else [])
        + (["--es", "native-jar", args.native_jar] if args.native_jar else []),
        check=True,
    )

    # The previous probe is stopped before deleting its result.
    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "rm",
            "-f",
            "files/last-result.txt",
        ],
        check=True,
    )

    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "mkdir",
            "-p",
            "files/android-jre",
        ],
        check=True,
    )
    label = "运行冒烟测试" if args.group == "smoke" else "运行覆盖测试"
    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "touch" if args.array_copy_cache == "off" else "rm",
            *([] if args.array_copy_cache == "off" else ["-f"]),
            "files/android-jre/array-copy-cache-disabled",
        ],
        check=True,
    )
    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "touch" if args.payload_profile == "threshold" else "rm",
            *([] if args.payload_profile == "threshold" else ["-f"]),
            "files/android-jre/payload-threshold-enabled",
        ],
        check=True,
    )
    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "touch" if args.array_copy_cache == "on" else "rm",
            *([] if args.array_copy_cache == "on" else ["-f"]),
            "files/android-jre/array-copy-cache-enabled",
        ],
        check=True,
    )
    subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "touch" if args.payload_diagnostic else "rm",
            *([] if args.payload_diagnostic else ["-f"]),
            "files/android-jre/payload-diagnostic-enabled",
        ],
        check=True,
    )

    layout = output / "ready.json"
    button = None
    for _ in range(12):
        subprocess.run(
            [
                "android",
                "layout",
                "--device=" + args.device,
                "--flat",
                "--output=" + str(layout),
            ],
            check=True,
        )

        matches = [
            node
            for window in json.loads(layout.read_text())
            for node in window.get("content", [])
            if node.get("text") == label
            and "CLICKABLE" in node.get("interactions", [])
        ]
        if len(matches) == 1:
            button = matches[0]
            break
        time.sleep(0.5)
    if button is None:
        raise SystemExit("Test action missing: " + label)

    x, y = re.findall(r"\d+", button["center"])
    subprocess.run(adb + ["shell", "input", "tap", x, y], check=True)

    deadline = time.monotonic() + args.timeout
    scheduling = None
    while time.monotonic() < deadline:
        if scheduling is None:
            probe = subprocess.run(
                adb + ["shell", "pidof", "dev.glibc.worker:probe"],
                check=False,
                capture_output=True,
                text=True,
            )
            pid = probe.stdout.strip()
            if probe.returncode == 0 and pid.isdigit():
                scheduling = {"pid": int(pid)}
                if args.cpu_mask:
                    scheduling["cpu_mask"] = args.cpu_mask
                for name in ("status", "cgroup"):
                    state = subprocess.run(
                        adb
                        + [
                            "shell",
                            "run-as",
                            "dev.glibc.worker",
                            "cat",
                            "/proc/" + pid + "/" + name,
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    scheduling[name] = (
                        state.stdout if state.returncode == 0 else None
                    )
                (output / "scheduling.json").write_text(
                    json.dumps(scheduling, indent=2) + "\n"
                )

        result = subprocess.run(
            adb
            + [
                "shell",
                "run-as",
                "dev.glibc.worker",
                "cat",
                "files/last-result.txt",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        if result.returncode == 0 and "APP_RESULT=" in result.stdout:
            report = output / "report.txt"
            report.write_text(result.stdout)
            if "JRE_EXEC_PROBE " in result.stdout:
                probe = subprocess.run(
                    adb
                    + [
                        "shell",
                        "run-as",
                        "dev.glibc.worker",
                        "cat",
                        "files/android-jre/launcher-probe.log",
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                (output / "launcher-probe.log").write_text(probe.stdout)

            evidence = verify(
                result.stdout,
                args.group,
                args.native_jar,
                args.benchmark_jit,
                args.payload_benchmark,
                payload_trials=3
                * len(payload_points(args.payload_profile)[0])
                * len(payload_points(args.payload_profile)[1]),
            )
            evidence["abi"] = abi
            evidence["apk_sha256"] = apk_hash
            evidence["installed_apk_sha256"] = actual

            cache = re.search(
                r"ARRAY_COPY_CACHE enabled=([01]) min_bytes=(\d+) max_bytes=(\d+) ",
                result.stdout,
            )
            cache_enabled = bool(cache and cache[1] == "1")
            if args.array_copy_cache is not None and (
                cache is None
                or cache_enabled != (args.array_copy_cache != "off")
            ):
                raise SystemExit(
                    "Array copy cache configuration was not applied"
                )
            if cache is not None and not re.search(
                r"ARRAY_COPY_CACHE_FINISH hits=\d+ misses=\d+ fallback=\d+ active=0 retained_bytes=0",
                result.stdout,
            ):
                raise SystemExit("Array copy cache did not shut down cleanly")
            evidence["array_copy_cache"] = cache_enabled
            if cache:
                evidence["array_copy_min_bytes"] = int(cache[2])
                evidence["array_copy_max_bytes"] = int(cache[3])

            if args.cpu_mask:
                marker = "TEST_CPU_AFFINITY mask=" + format(
                    int(args.cpu_mask, 16), "x"
                )
                if marker + " scope=host-and-descendants" not in result.stdout:
                    raise SystemExit(
                        "Requested benchmark affinity was not installed"
                    )
                evidence["cpu_mask"] = args.cpu_mask

            if args.benchmark:
                if (
                    "BRIDGE_BENCH_SUMMARY samples=18 module_unloaded=PASS"
                    not in result.stdout
                ):
                    raise SystemExit("Benchmark did not complete")
                rows = []
                for line in result.stdout.splitlines():
                    if line.startswith("BRIDGE_BENCH "):
                        fields = dict(
                            token.split("=", 1) for token in line.split()[1:]
                        )
                        rows.append(
                            {
                                k: v == "true" if k == "callback" else int(v)
                                for k, v in fields.items()
                            }
                        )
                if len(rows) != 18:
                    raise SystemExit("Incomplete benchmark samples")
                (output / "benchmark.json").write_text(
                    json.dumps(rows, indent=2) + "\n"
                )

            if args.payload_benchmark:
                if args.array_copy_cache is not None and (
                    "ARRAY_COPY_TEST_SUMMARY primitive_checks=104 types=8 concurrent_leases=12 cross_thread_release=PASS oversized_fallback=PASS result=PASS"
                    not in result.stdout
                ):
                    raise SystemExit(
                        "Array copy semantics tests did not complete"
                    )

                if args.payload_diagnostic:
                    (output / "payload-stages.bin").write_bytes(
                        subprocess.check_output(
                            adb
                            + [
                                "shell",
                                "run-as",
                                "dev.glibc.worker",
                                "cat",
                                "files/android-jre/payload-stages.bin",
                            ]
                        )
                    )
                    evidence["payload_diagnostic"] = True

                raw = subprocess.check_output(
                    adb
                    + [
                        "shell",
                        "run-as",
                        "dev.glibc.worker",
                        "cat",
                        "files/android-jre/payload-samples.bin",
                    ]
                )
                (output / "payload-samples.bin").write_bytes(raw)

                rows = verify_payload(
                    result.stdout,
                    raw,
                    args.payload_benchmark,
                    args.payload_round,
                    args.payload_profile,
                )
                evidence["payload_mode"] = args.payload_benchmark
                evidence["payload_round"] = args.payload_round
                evidence["payload_profile"] = args.payload_profile
                (output / "payload.json").write_text(
                    json.dumps(rows, indent=2) + "\n"
                )

            attempt.finish(evidence)
            print(args.group + ": PASS; evidence=" + str(output))
            return

        time.sleep(0.3)

    partial = subprocess.run(
        adb
        + [
            "shell",
            "run-as",
            "dev.glibc.worker",
            "cat",
            "files/native-report.txt",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    (output / "timeout-report.txt").write_text(partial.stdout)
    raise SystemExit("Timed out; no final test result")


def main():
    args = arguments()
    output = (
        args.output or ROOT / "jvm-jni/build/test-results" / args.group
    ).resolve()
    with RunEvidence(output, group=args.group, device=args.device) as attempt:
        execute(args, output, attempt)


if __name__ == "__main__":
    main()
