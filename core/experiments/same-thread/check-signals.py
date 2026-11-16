"""Build and run the Host signal contract independently of the domain runtime."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
domain = source.parents[1] / "src/domain"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--serial")
args = parser.parse_args()
if bool(args.ndk) != bool(args.serial):
    parser.error("Android requires both --ndk and --serial")
output = (
    source.parents[1]
    / "build/experiments/same-thread/signal-contract-results"
    / ("android" if args.ndk else "linux")
)
output.mkdir(parents=True, exist_ok=True)
(output / "summary.json").unlink(missing_ok=True)
compiler = (
    str(
        args.ndk
        / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
    )
    if args.ndk
    else "gcc"
)
files = [
    source / "signal-contract.c",
    domain / "signal-router.c",
    domain / "signal-interpose.c",
    domain / "signal-stack.S",
]
command = [
    compiler,
    "-std=gnu17",
    "-I" + str(domain),
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wl,--export-dynamic",
    *map(str, files),
    "-ldl",
    "-pthread",
    "-o",
    str(output / "contract"),
]
subprocess.run(command, check=True)
run = [str(output / "contract")]
if args.serial:
    remote = (
        "/data/local/tmp/gw-signal-contract-"
        + hashlib.sha256((output / "contract").read_bytes()).hexdigest()[:16]
    )
    adb = ["adb", "-s", args.serial]
    subprocess.run([*adb, "push", str(output / "contract"), remote], check=True)
    run = [
        *adb,
        "shell",
        "/system/bin/timeout",
        "10",
        "/system/bin/linker64",
        remote,
    ]
result = subprocess.run(
    run, check=False, capture_output=True, text=True, timeout=20
)
(output / "stdout.log").write_text(result.stdout)
(output / "stderr.log").write_text(result.stderr)
(output / "manifest.json").write_text(
    json.dumps(
        {
            "build": command,
            "run": run,
            "sha256": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in [
                    *files,
                    domain / "signal-router.h",
                    domain / "signal-stack.h",
                    domain / "nonlocal-mask.h",
                    Path(__file__),
                    output / "contract",
                ]
            },
        },
        indent=2,
    )
    + "\n"
)
print(result.stdout, end="")
print(result.stderr, end="")
rows = [
    json.loads(line)
    for line in result.stdout.splitlines()
    if line.startswith("{")
]
if result.returncode or len(rows) != 1 or rows[0]["result"] != "PASS":
    raise SystemExit(f"Signal contract failed: {output}")
(output / "summary.json").write_text(json.dumps(rows[0], indent=2) + "\n")
