"""Verify GNU-only signal forced unwind before using the JNI integration."""

import argparse
import hashlib
import json
import platform
import shlex
import shutil
import subprocess
from pathlib import Path

project = Path(__file__).resolve().parents[1]
root = project.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--abi", choices=["x86_64", "arm64-v8a"], default="x86_64")
parser.add_argument("--serial")
args = parser.parse_args()
if args.serial and args.abi != "arm64-v8a":
    parser.error("The device control currently supports ARM64")
if (
    not args.serial
    and platform.machine()
    != {
        "x86_64": "x86_64",
        "arm64-v8a": "aarch64",
    }[args.abi]
):
    parser.error("Native control requires the matching machine architecture")

runtime = args.runtime.resolve()
configuration = json.loads((root / "core/config/targets.json").read_text())[
    args.abi
]
compiler = configuration["guest_cc"]
source = project / "tests/runtime-sdk/guest-signal-unwind.c"
header = source.with_name("nonlocal_probe.h")
out = (
    project
    / "build/tests"
    / ("guest-signal-unwind-android" if args.serial else "guest-signal-unwind")
)
out.mkdir(parents=True, exist_ok=True)
for name in ("summary.json", "failure.json", "manifest.json"):
    (out / name).unlink(missing_ok=True)

commands = []


def run(command, **options):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **options)


run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fexceptions",
        "-fnon-call-exceptions",
        "-DGW_UNWIND_NATIVE_CONTROL",
        source,
        "-pthread",
        "-o",
        out / "control",
    ]
)
unwind = Path(
    subprocess.check_output(
        [compiler, "-print-file-name=libgcc_s.so.1"], text=True
    ).strip()
).resolve(strict=True)
shutil.copyfile(unwind, out / "libgcc_s.so.1")
shutil.copyfile(runtime / configuration["loader"], out / "control-loader")
(out / "control-loader").chmod(0o755)
inputs = [
    Path(__file__),
    source,
    header,
    root / "core/config/targets.json",
    runtime / "manifest.json",
    runtime / "libc.so.6",
    runtime / configuration["loader"],
    unwind,
    out / "control",
    out / "control-loader",
    out / "libgcc_s.so.1",
]
hashes = {
    str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs
}
command = list(
    map(
        str,
        [
            out / "control-loader",
            "--library-path",
            f"{runtime}:{out}",
            out / "control",
        ],
    )
)
if args.serial:
    remote = (
        "/data/local/tmp/gw-gnu-unwind-"
        + hashlib.sha256(
            json.dumps(hashes, sort_keys=True).encode()
        ).hexdigest()[:16]
    )
    adb = ["adb", "-s", args.serial]
    run([*adb, "shell", "mkdir", "-p", remote])
    for path in (
        out / "control",
        out / "control-loader",
        out / "libgcc_s.so.1",
        runtime / "libc.so.6",
    ):
        run([*adb, "push", path, remote + "/"])
    run([*adb, "shell", "chmod", "0755", remote + "/control-loader"])
    command = [
        *adb,
        "shell",
        shlex.join(
            [
                "/system/bin/timeout",
                "-s",
                "KILL",
                "15",
                remote + "/control-loader",
                "--library-path",
                remote,
                remote + "/control",
            ]
        ),
    ]
commands.append(command)
(out / "manifest.json").write_text(
    json.dumps({"commands": commands, "sha256": hashes}, indent=2) + "\n"
)

try:
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    stdout, stderr, returncode = result.stdout, result.stderr, result.returncode
except subprocess.TimeoutExpired as error:
    stdout = (error.stdout or b"").decode(errors="replace")
    stderr = (error.stderr or b"").decode(errors="replace")
    returncode = None
(out / "stdout.log").write_text(stdout)
(out / "stderr.log").write_text(stderr)
try:
    rows = [json.loads(line) for line in stdout.splitlines()]
except json.JSONDecodeError:
    rows = []
summary = {
    "result": "PASS"
    if returncode == 0
    and rows == [{"mode": mode, "value": 73} for mode in range(97, 101)]
    else "FAIL",
    "returncode": returncode,
    "cases": [{"result": "PASS", **row} for row in rows]
    if returncode == 0
    else rows,
    "bridge_loaded": False,
}
destination = out / (
    "summary.json" if summary["result"] == "PASS" else "failure.json"
)
destination.write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
raise SystemExit(0 if summary["result"] == "PASS" else 1)
