#!/usr/bin/env python3
"""Build explicit Android JREs, then validate them sequentially on one target.

With --emulator, build and emulator phases never overlap. Any failed artifact
fails the matrix; VM startup failures are distinct from bridge validation.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evidence import RunEvidence, atomic_json
from jre_artifacts import PROJECT, select

GRADLE = PROJECT / "tests/android/gradlew"
ANDROID_PROJECT = PROJECT / "tests/android"


def command(argv, log, **kwargs):
    with log.open("w") as stream:
        return subprocess.run(
            list(map(str, argv)),
            stdout=stream,
            stderr=subprocess.STDOUT,
            **kwargs,
        ).returncode


def resource_processes(kind):
    lines = subprocess.check_output(
        ["ps", "-eo", "args"], text=True
    ).splitlines()
    return [
        line
        for line in lines
        if line.split()
        and (
            (kind == "emulator" and "qemu-system-" in line.split()[0])
            or (
                kind == "gradle"
                and line.split()[0].endswith("/java")
                and "org.gradle.launcher.daemon.bootstrap.GradleDaemon"
                in line.split()
            )
        )
    ]


def await_closed(kind):
    deadline = time.monotonic() + 30
    while resource_processes(kind):
        if time.monotonic() >= deadline:
            raise RuntimeError(
                kind + " is still running; phases cannot overlap"
            )
        time.sleep(0.25)


def stop_emulator(name, device):
    if any(
        "@" + name in line.split() for line in resource_processes("emulator")
    ):
        subprocess.run(["android", "emulator", "stop", name], check=True)
    if any(
        "@" + name in line.split() for line in resource_processes("emulator")
    ):
        actual = subprocess.check_output(
            ["adb", "-s", device, "emu", "avd", "name"], text=True
        ).splitlines()
        if not actual or actual[0].strip() != name:
            raise RuntimeError("Emulator console identity mismatch")
        subprocess.run(["adb", "-s", device, "emu", "kill"], check=True)
    await_closed("emulator")


def build_artifact(row, target, abi):
    env = dict(os.environ, GW_ABI=abi, GW_JRE_ARTIFACT=row["id"])
    item = {
        "artifact": row,
        "status": "BUILD_FAILED",
        "phases": {},
        "directory": str(target),
    }
    rc = command(
        [GRADLE, "-p", ANDROID_PROJECT, ":app:assembleDebug", "--no-daemon"],
        target / "build.log",
        env=env,
    )
    if rc:
        return item
    apk = target / "app.apk"
    shutil.copyfile(
        PROJECT / "build/tests/android/app/outputs/apk/debug/app-debug.apk", apk
    )
    shutil.copyfile(
        PROJECT / "build/native/android-jre.json",
        target / "build-manifest.json",
    )
    item.update(
        status="BUILT", apk_sha256=hashlib.sha256(apk.read_bytes()).hexdigest()
    )
    return item


def failure_stage(report):
    if not report.is_file():
        return "RUNNER_FAILED"
    text = report.read_text()
    startup = any(
        stage in text
        for stage in (
            "JRE_STAGE=JNI_CreateJavaVM",
            "JRE_STAGE=JLI_Launch",
            "JRE_DLOPEN_ERROR=",
        )
    )
    if (
        "APP_RESULT=FAIL" in text
        and startup
        and "JRE_CREATE_RESULT=0" not in text
    ):
        return "JVM_STARTUP_BLOCKED"
    return "TEST_FAILED"


def test_artifact(item, device):
    target = Path(item["directory"])
    row = item["artifact"]
    env = dict(os.environ, GW_ABI=row["abi"], GW_JRE_ARTIFACT=row["id"])
    for group in ("smoke", "coverage"):
        rc = command(
            [
                PROJECT / ("tests/" + group + "/run.sh"),
                "--device",
                device,
                "--reuse-apk",
                "--apk",
                target / "app.apk",
                "--output",
                target / group,
            ],
            target / (group + ".log"),
            env=env,
        )
        item["phases"][group] = "PASS" if rc == 0 else "FAILED"
        item["status"] = (
            "PASS" if rc == 0 else failure_stage(target / group / "report.txt")
        )
        if rc:
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument(
        "--emulator",
        help="AVD to stop before builds and start after Gradle exits",
    )
    parser.add_argument(
        "--abi", default="arm64-v8a", choices=("arm64-v8a", "x86_64")
    )
    parser.add_argument("--artifact", action="append", required=True)
    parser.add_argument(
        "--output", type=Path, default=PROJECT / "build/android-jre-matrix"
    )
    args = parser.parse_args()
    rows = [select(args.abi, item) for item in args.artifact]
    if len({row["id"] for row in rows}) != len(rows):
        parser.error("Duplicate artifact selection")
    output = args.output.resolve()
    with RunEvidence(
        output, kind="android-jre-matrix", selected=[r["id"] for r in rows]
    ) as attempt:
        if args.emulator:
            stop_emulator(args.emulator, args.device)
        results = []
        for row in rows:
            target = output / attempt.state["run_id"] / row["id"]
            target.mkdir(parents=True)
            results.append(build_artifact(row, target, args.abi))
            attempt.update(runtimes=results)
        try:
            if args.emulator:
                subprocess.run(
                    [str(GRADLE), "-p", str(ANDROID_PROJECT), "--stop"],
                    check=True,
                )
                await_closed("gradle")
                rc = command(
                    [
                        "android",
                        "emulator",
                        "start",
                        args.emulator,
                        "--headless",
                        "--cold",
                    ],
                    output / "emulator-start.log",
                )
                if rc:
                    raise RuntimeError("Emulator startup failed")
            for item in results:
                if item["status"] == "BUILT":
                    test_artifact(item, args.device)
                    attempt.update(runtimes=results)
                print(
                    item["artifact"]["id"] + ": " + item["status"], flush=True
                )
        finally:
            if args.emulator:
                stop_emulator(args.emulator, args.device)
        passed = all(item["status"] == "PASS" for item in results)
        summary = {"result": "PASS" if passed else "FAIL", "runtimes": results}
        if not passed:
            atomic_json(
                output / "failed-summary.json",
                {**summary, "run_id": attempt.state["run_id"]},
            )
            raise RuntimeError(
                "Android JRE matrix failed; inspect per-artifact evidence"
            )
        attempt.finish(summary)


if __name__ == "__main__":
    main()
