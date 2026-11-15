"""Build/run the disposable same-thread bootstrap probe, never install a runtime."""

import argparse
import hashlib
import json
import platform
import shlex
import shutil
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
core = source.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=["x86_64", "arm64-v8a"], default="x86_64")
parser.add_argument("--runtime", type=Path)
parser.add_argument("--output", type=Path)
parser.add_argument(
    "--ndk", type=Path, help="Build Android Host; do not run locally"
)
parser.add_argument(
    "--serial", help="With --ndk, run in an isolated ADB shell process"
)
parser.add_argument(
    "--qemu", action="store_true", help="Run ARM64 Linux under QEMU"
)
parser.add_argument(
    "--sysroot", type=Path, help="QEMU Host dynamic-loader sysroot"
)
args = parser.parse_args()
if args.ndk and args.qemu:
    parser.error(
        "Android deployment and Linux QEMU execution are separate stages"
    )
if args.serial and not args.ndk:
    parser.error("--serial requires --ndk")
if args.qemu and (args.abi != "arm64-v8a" or not args.sysroot):
    parser.error("--qemu requires --abi arm64-v8a and an explicit --sysroot")
runtime = (args.runtime or core / "build/glibc-runtime" / args.abi).resolve()
kind = "android" if args.ndk else "linux"
output = (
    args.output or core / "build/experiments/same-thread" / f"{args.abi}-{kind}"
).resolve()
output.mkdir(parents=True, exist_ok=True)
(output / "summary.json").unlink(missing_ok=True)
target = json.loads((core / "config/targets.json").read_text())[args.abi]
loader = runtime / target["loader"]
for path in (loader, runtime / "libc.so.6", runtime / "libm.so.6"):
    if not path.is_file():
        parser.error(f"Missing matched runtime file: {path}")


def executable(name):
    path = shutil.which(str(name))
    if not path:
        parser.error(f"Missing tool: {name}")
    return path


guest_cc = executable(target["guest_cc"])
if args.ndk:
    host_cc = executable(
        args.ndk.resolve()
        / "toolchains/llvm/prebuilt/linux-x86_64/bin"
        / (target["ndk_triplet"] + "29-clang")
    )
else:
    host_cc = guest_cc

commands = []


def build(cc, name, sources, flags=()):
    command = [
        cc,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fstack-protector-strong",
        "-I" + str(core / "src/bootstrap"),
        "-I" + str(core / "include"),
        *map(str, sources),
        *flags,
        "-lm",
        "-o",
        str(output / name),
    ]
    commands.append(command)
    subprocess.run(command, check=True)


build(
    host_cc,
    "host",
    [
        source / "host.c",
        source / "switch.S",
        core / "src/bootstrap/loader_image.c",
    ],
    ["-fno-emulated-tls"] if args.ndk else [],
)
build(guest_cc, "guest", [source / "guest.c"], ["-ldl"])
build(guest_cc, "probe.so", [source / "probe.c"], ["-shared", "-fPIC"])
files = [
    *source.glob("*.c"),
    source / "switch.S",
    source / "domain.h",
    Path(__file__).resolve(),
    core / "src/bootstrap/loader_image.c",
    core / "src/bootstrap/loader_image.h",
    core / "src/bootstrap/raw_syscall.h",
    core / "include/arch.h",
    core / "config/targets.json",
    loader,
    runtime / "libc.so.6",
    runtime / "libm.so.6",
    output / "host",
    output / "guest",
    output / "probe.so",
]
manifest = {
    "abi": args.abi,
    "host_libc": "bionic" if args.ndk else "glibc",
    "guest_libc": "glibc",
    "build_commands": commands,
    "compilers": {
        cc: subprocess.check_output([cc, "--version"], text=True).splitlines()[
            0
        ]
        for cc in {host_cc, guest_cc}
    },
    "sha256": {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files
    },
}
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
if args.ndk and not args.serial:
    print(f"Android binaries built, NOT executed: {output}")
    raise SystemExit(0)
if (
    not args.qemu
    and not args.serial
    and platform.machine()
    != ("aarch64" if args.abi == "arm64-v8a" else "x86_64")
):
    parser.error("Foreign architecture execution needs explicit --qemu")
command = [
    str(output / "host"),
    str(loader),
    str(runtime),
    str(output / "guest"),
    str(output / "probe.so"),
]
if args.qemu:
    command = [
        executable("qemu-aarch64"),
        "-L",
        str(args.sysroot.resolve()),
        *command,
    ]
if args.serial:
    adb = [executable("adb"), "-s", args.serial]
    digest = hashlib.sha256(
        (output / "manifest.json").read_bytes()
    ).hexdigest()[:16]
    remote = "/data/local/tmp/gw-same-thread-" + digest
    subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
    for path in [
        output / "host",
        output / "guest",
        output / "probe.so",
        loader,
        runtime / "libc.so.6",
        runtime / "libm.so.6",
    ]:
        subprocess.run([*adb, "push", str(path), remote + "/"], check=True)
    invocation = [
        "/system/bin/timeout",
        "-s",
        "KILL",
        "45",
        "/system/bin/linker64",
        remote + "/host",
        remote + "/" + loader.name,
        remote,
        remote + "/guest",
        remote + "/probe.so",
    ]
    command = [*adb, "shell", shlex.join(invocation)]
    manifest["device"] = {
        "serial": args.serial,
        "fingerprint": subprocess.check_output(
            [*adb, "shell", "getprop", "ro.build.fingerprint"], text=True
        ).strip(),
        "identity": subprocess.check_output(
            [*adb, "shell", "id"], text=True
        ).strip(),
        "remote_directory": remote,
    }
manifest["run_command"] = command
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
try:
    run = subprocess.run(
        command, check=False, capture_output=True, text=True, timeout=60
    )
except subprocess.TimeoutExpired as error:
    for name, data in (("stdout", error.stdout), ("stderr", error.stderr)):
        (output / (name + ".log")).write_bytes(
            data.encode() if isinstance(data, str) else data or b""
        )
    raise SystemExit(f"Probe timed out; logs: {output}") from error
(output / "stdout.log").write_text(run.stdout)
(output / "stderr.log").write_text(run.stderr)
if run.returncode:
    raise SystemExit(f"Probe exited {run.returncode}; logs: {output}")
result = json.loads(run.stdout)
if (
    result["result"] != "PASS"
    or result["failures"]
    or not result["same_tid"]
    or not result["distinct_tls"]
    or not result["host_signal_saw_guest_tls"]
    or result["calls"] != 80000
    or result["callbacks"] != 80000
    or result["nested"] != 70000
):
    raise SystemExit(f"Probe assertions failed: {output}")
result.update(
    execution=(
        "android-adb-shell"
        if args.serial
        else "qemu-linux"
        if args.qemu
        else "native-linux"
    ),
    host_libc="bionic" if args.ndk else "glibc",
    guest_libc="glibc",
    android_validated=bool(args.serial),
    android_app_validated=False,
    pthread_adoption_validated=False,
    production_ready=False,
    command=command,
)
(output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
