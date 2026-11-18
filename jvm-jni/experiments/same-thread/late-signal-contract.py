"""Test explicit late signal adoption without relying on global interposition."""

import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[2]
core = root / "core/src/domain"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--serial")
args = parser.parse_args()
if bool(args.ndk) != bool(args.serial):
    parser.error("Supply both --ndk and --serial for Android")
out = (
    root
    / "jvm-jni/build/experiments/late-signal-contract"
    / ("android" if args.ndk else "linux")
)
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
cc = (
    str(
        args.ndk
        / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    )
    if args.ndk
    else "clang"
)
flags = [
    cc,
    "-std=gnu17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I" + str(core),
    "-I" + str(core.parents[1] / "include"),
]
files = [
    source / "late-signal-contract.c",
    core / "signal-router.c",
    core / "signal-stack.S",
    core / "signal-quiescence.c",
    core / "signal-router.h",
    Path(__file__),
]
commands = [
    [
        *flags,
        "-shared",
        "-fPIC",
        "-Wl,-Bsymbolic-functions",
        "-Wl,-z,defs",
        "-DROUTER_STUB",
        str(files[0]),
        str(files[1]),
        str(files[2]),
        str(files[3]),
        "-ldl",
        "-o",
        str(out / "router.so"),
    ],
    [*flags, str(files[0]), "-ldl", "-o", str(out / "contract")],
]
for command in commands:
    subprocess.run(command, check=True)
run = [str(out / "contract"), str(out / "router.so")]
if args.serial:
    remote = (
        "/data/local/tmp/gw-late-contract-"
        + hashlib.sha256((out / "router.so").read_bytes()).hexdigest()[:16]
    )
    adb = ["adb", "-s", args.serial]
    subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
    subprocess.run(
        [*adb, "push", str(out / "contract"), str(out / "router.so"), remote],
        check=True,
    )
    run = [
        *adb,
        "shell",
        shlex.join(
            [
                "/system/bin/timeout",
                "10",
                "/system/bin/linker64",
                remote + "/contract",
                remote + "/router.so",
            ]
        ),
    ]
result = subprocess.run(
    run, text=True, capture_output=True, timeout=20, check=False
)
(out / "stdout.log").write_text(result.stdout)
(out / "stderr.log").write_text(result.stderr)
print(result.stdout + result.stderr, end="")
if result.returncode:
    raise SystemExit("Late signal contract failed: " + str(out))
summary = json.loads(result.stdout)
if summary["result"] != "PASS":
    raise SystemExit("Late signal assertions failed")
summary.update(
    build=commands,
    run=run,
    sha256={
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [*files, out / "contract", out / "router.so"]
    },
)
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
