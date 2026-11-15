"""Package and run the native domain fixture under an ordinary application UID."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

source = Path(__file__).resolve().parent
core = source.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--native", type=Path, required=True)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--gradle", type=Path, required=True)
parser.add_argument("--sdk", type=Path, required=True)
parser.add_argument("--serial", required=True)
args = parser.parse_args()
native, runtime = args.native.resolve(), args.runtime.resolve()
manifest = json.loads((native / "manifest.json").read_text())
if manifest["abi"] != "arm64-v8a" or manifest["host_libc"] != "bionic":
    parser.error("This APK fixture requires a built ARM64 bionic Host")
output = core / "build/experiments/same-thread"
libraries = output / "android-jniLibs/arm64-v8a"
libraries.mkdir(parents=True, exist_ok=True)
payload = {
    native / "host": "libdomain_host.so",
    native / "guest": "libdomain_guest.so",
    native / "probe.so": "libdomain_probe.so",
    native / "libgcc_s.so.1": "libdomain_gcc.so",
    runtime / "ld-linux-aarch64.so.1": "libdomain_loader.so",
    runtime / "libc.so.6": "libdomain_libc.so",
    runtime / "libm.so.6": "libdomain_libm.so",
}
for path, name in payload.items():
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if manifest["sha256"].get(str(path)) != digest:
        parser.error(f"Payload differs from the recorded native build: {path}")
    shutil.copyfile(path, libraries / name)
environment = {**os.environ, "ANDROID_HOME": str(args.sdk.resolve())}
subprocess.run(
    [
        str(args.gradle.resolve()),
        "-p",
        str(source / "android"),
        "--no-daemon",
        "--console=plain",
        "assembleDebug",
    ],
    env=environment,
    check=True,
)
apk = output / "android-app/outputs/apk/debug/app-debug.apk"
evidence = output / "android-app-evidence"
evidence.mkdir(parents=True, exist_ok=True)
(evidence / "summary.json").unlink(missing_ok=True)
adb = ["adb", "-s", args.serial]
package = "dev.glibc.worker.domains"
# A previous run may have left a complete report. Remove it before launch so
# polling cannot mistake that report for the result of the new executable.
subprocess.run([*adb, "shell", "am", "force-stop", package], check=True)
installed = subprocess.run(
    [*adb, "shell", "pm", "list", "packages", package],
    capture_output=True,
    text=True,
    check=True,
)
if "package:" + package in installed.stdout.splitlines():
    subprocess.run(
        [
            *adb,
            "shell",
            "run-as",
            package,
            "rm",
            "-f",
            "files/native-report.txt",
        ],
        check=True,
    )
subprocess.run(
    ["android", "run", "--device=" + args.serial, "--apks=" + str(apk)],
    check=True,
)
deadline = time.monotonic() + 65
report = ""
while time.monotonic() < deadline:
    read = subprocess.run(
        [*adb, "shell", "run-as", package, "cat", "files/native-report.txt"],
        capture_output=True,
        text=True,
        check=False,
    )
    report = read.stdout
    if "APP_DOMAIN_EXIT=" in report or "APP_DOMAIN_RESULT=" in report:
        break
    time.sleep(1)
(evidence / "native-report.txt").write_text(report)
rows = [
    json.loads(line) for line in report.splitlines() if line.startswith("{")
]
if (
    len(rows) != 1
    or rows[0]["result"] != "PASS"
    or rows[0]["failures"]
    or rows[0]["uid"] < 10000
    or "APP_DOMAIN_EXIT=0" not in report
):
    raise SystemExit(f"Application probe failed; retained report: {evidence}")
result = rows[0]
result.update(
    execution="android-application-exec-child",
    hotspot_validated=False,
    art_jni_validated=False,
    production_ready=False,
    apk_sha256=hashlib.sha256(apk.read_bytes()).hexdigest(),
)
(evidence / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
