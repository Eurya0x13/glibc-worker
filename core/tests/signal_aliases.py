"""Compare routed registrations with the carrier's actual native libc."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--serial")
args = parser.parse_args()
if bool(args.ndk) != bool(args.serial):
    parser.error("--ndk and --serial must be supplied together")
core = Path(__file__).resolve().parents[1]
out = (
    core
    / "build/tests"
    / ("signal-aliases-android" if args.ndk else "signal-aliases")
)
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
commands = []


def run(command):
    command = list(map(str, command))
    commands.append(command)
    subprocess.run(command, check=True)


configure = [
    "cmake",
    "-S",
    core,
    "-B",
    out / "sdk",
    "-DBUILD_TESTING=ON",
]
if args.ndk:
    configure += [
        "-DCMAKE_TOOLCHAIN_FILE="
        + str(args.ndk.resolve() / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-29",
    ]
run(configure)
run(
    [
        "cmake",
        "--build",
        out / "sdk",
        "--target",
        "signal_aliases_test",
        "--parallel",
        "4",
    ]
)
binary = out / "sdk/signal_aliases_test"
command = [str(binary)]
if args.serial:
    remote = (
        "/data/local/tmp/gw-signal-aliases-"
        + hashlib.sha256(binary.read_bytes()).hexdigest()[:16]
    )
    run(["adb", "-s", args.serial, "push", binary, remote])
    command = [
        "adb",
        "-s",
        args.serial,
        "shell",
        "/system/bin/linker64",
        remote,
    ]
commands.append(command)
inputs = [
    Path(__file__),
    core / "CMakeLists.txt",
    core / "tests/signal_aliases.c",
    binary,
    *[
        p
        for p in (core / "src/domain").iterdir()
        if p.suffix in (".h", ".c", ".S", ".inc")
    ],
]
(out / "manifest.json").write_text(
    json.dumps(
        {
            "commands": commands,
            "sha256": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in inputs
            },
        },
        indent=2,
    )
    + "\n"
)
result = subprocess.run(command, capture_output=True, text=True, timeout=20)
(out / "run.log").write_text(result.stdout + result.stderr)
result.check_returncode()
data = json.loads(result.stdout)
assert data["result"] == "PASS"
assert data["registrations"] == (5 if args.ndk else 9)
assert data["native_comparisons"] == (9 if args.ndk else 17)
assert data["routed_deliveries"] == (1 if args.ndk else 5)
assert data["native_ignored_fault"] is True
(out / "summary.json").write_text(json.dumps(data, indent=2) + "\n")
print(result.stdout, end="")
