"""Validate native unwind boundaries without claiming dual-libc recovery."""

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
out = core / "build/tests" / ("nonlocal-android" if args.ndk else "nonlocal")
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
domain = core / "src/domain"
compiler = (
    str(
        args.ndk
        / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    )
    if args.ndk
    else "cc"
)
command = [
    compiler,
    "-std=gnu17",
    "-O2",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-UNDEBUG",
    *(["-fno-emulated-tls"] if args.ndk else []),
    "-I" + str(domain),
    str(core / "tests/nonlocal/host.c"),
    str(core / "tests/nonlocal/fortified.c"),
    *[
        str(domain / name)
        for name in ("nonlocal.c", "nonlocal-entry.S", "nonlocal-interpose.c")
    ],
    "-ldl",
    "-o",
    str(out / "host"),
]
subprocess.run(command, check=True)
run = [str(out / "host")]
if args.serial:
    adb = ["adb", "-s", args.serial]
    remote = (
        "/data/local/tmp/gw-nonlocal-"
        + hashlib.sha256((out / "host").read_bytes()).hexdigest()[:16]
    )
    subprocess.run([*adb, "push", str(out / "host"), remote], check=True)
    run = [*adb, "shell", "/system/bin/linker64", remote]
(out / "manifest.json").write_text(
    json.dumps(
        {
            "compile": command,
            "run": run,
            "sha256": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in [
                    Path(__file__),
                    *sorted((core / "tests/nonlocal").glob("*.c")),
                    *sorted(domain.glob("nonlocal*")),
                    out / "host",
                ]
            },
        },
        indent=2,
    )
    + "\n"
)
result = subprocess.run(run, capture_output=True, text=True, timeout=20)
(out / "run.log").write_text(result.stdout + result.stderr)
result.check_returncode()
data = json.loads(result.stdout)
assert data["result"] == "PASS"
assert data["unobserved_rejected"] is True
assert data["fortified_call_tested"] is True
assert data["directed_scopes"] == 800
assert data["glibc_checked_entry"] is (not args.ndk)
(out / "summary.json").write_text(json.dumps(data, indent=2) + "\n")
print(result.stdout, end="")
