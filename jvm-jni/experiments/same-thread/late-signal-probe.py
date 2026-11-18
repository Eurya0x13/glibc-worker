"""Record whether the existing pre-VM signal router supports late DSO loading."""

import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ndk", type=Path, required=True)
parser.add_argument("--serial", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
source = Path(__file__).with_suffix(".c")
core = root / "core/src/domain"
out = root / "jvm-jni/build/experiments/system-load/late-signal"
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
cc = (
    args.ndk
    / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
)
flags = [
    str(cc),
    "-std=gnu17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I" + str(core),
    "-I" + str(core.parents[1] / "include"),
]
commands = [
    [
        *flags,
        "-shared",
        "-fPIC",
        "-DROUTER_STUB",
        str(source),
        str(core / "signal-router.c"),
        str(core / "signal-quiescence.c"),
        str(core / "signal-stack.S"),
        "-ldl",
        "-o",
        str(out / "router.so"),
    ],
    [*flags, str(source), "-ldl", "-o", str(out / "probe")],
]
for command in commands:
    subprocess.run(command, check=True)
adb = ["adb", "-s", args.serial]
remote = (
    "/data/local/tmp/gw-late-signal-"
    + hashlib.sha256((out / "router.so").read_bytes()).hexdigest()[:16]
)
subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
subprocess.run(
    [*adb, "push", str(out / "probe"), str(out / "router.so"), remote],
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
            remote + "/probe",
            remote + "/router.so",
        ]
    ),
]
result = subprocess.run(
    run, capture_output=True, text=True, check=False, timeout=15
)
(out / "stdout.log").write_text(result.stdout)
(out / "stderr.log").write_text(result.stderr)
print(result.stdout, end="")
if result.returncode:
    raise SystemExit("Late load probe failed to execute: " + str(out))
evidence = json.loads(result.stdout)
evidence.update(
    build=commands,
    run=run,
    scope="late DSO registration only; no VM/game execution",
    sha256={
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (
            source,
            Path(__file__),
            core / "signal-router.c",
            core / "signal-quiescence.c",
            core / "signal-stack.S",
            out / "probe",
            out / "router.so",
        )
    },
)
(out / "summary.json").write_text(json.dumps(evidence, indent=2) + "\n")
