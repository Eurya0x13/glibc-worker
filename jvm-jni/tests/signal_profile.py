"""Check installed Android HotSpot/Bionic signal rollback and reclamation."""

import argparse
import errno
import hashlib
import json
import shlex
import shutil
import subprocess
from pathlib import Path

project = Path(__file__).resolve().parents[1]
root = project.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--payload", type=Path, required=True)
parser.add_argument("--vm-startup-profile", type=Path, required=True)
parser.add_argument("--ndk", type=Path, required=True)
parser.add_argument("--serial", required=True)
parser.add_argument(
    "--rejection-prefix",
    type=Path,
    action="append",
    default=[],
    help="Explicit installed SDK with an intentionally invalid signal profile",
)
args = parser.parse_args()
payload = args.payload.resolve()
profile = args.vm_startup_profile.resolve()
ndk = args.ndk.resolve()
for name in (
    "libgw_jvm_jni.so",
    "libgw_android_jni.so",
    "libgw_dlopen.so",
    "helpers.jar",
    "jvm-helpers.jar",
    "jre/lib/server/libjvm.so",
):
    if not (payload / name).is_file():
        parser.error("Missing explicitly prepared SDK input: " + name)
if not profile.is_file():
    parser.error("Missing explicit VM startup profile")
vm_hash = hashlib.sha256(
    (payload / "jre/lib/server/libjvm.so").read_bytes()
).hexdigest()
if vm_hash not in profile.read_text():
    parser.error("VM startup profile does not match the payload libjvm")

out = project / "build/tests/signal-profile-android"
out.mkdir(parents=True, exist_ok=True)
for name in ("summary.json", "failure.json", "manifest.json"):
    (out / name).unlink(missing_ok=True)
shutil.copyfile(profile, out / "jre-layout.h")
compiler = (
    ndk
    / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
)
if not compiler.is_file():
    parser.error("The selected NDK has no Linux ARM64 API29 compiler")
source = project / "experiments/same-thread"
harness = project / "tests/native/harness"
files = [
    source / "late-host.c",
    source / "vm-start.c",
    harness / "jre_layout_spike.c",
    harness / "jre_exec_compat.c",
]
rejections = []
for prefix in args.rejection_prefix:
    prefix = prefix.resolve()
    library = prefix / "lib/libgw_jvm_jni.so"
    header = prefix / "share/glibc-worker/jvm-jni/signal-profile.h"
    if not library.is_file() or not header.is_file():
        parser.error("Missing rejection SDK library/profile: " + str(prefix))
    rejections.append((library, header))

commands = []


def run(command, **options):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **options)


binary = out / "signal-profile-host"
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(source),
        "-I" + str(harness),
        "-I" + str(out),
        "-Wl,--export-dynamic",
        *files,
        "-ldl",
        "-lm",
        "-o",
        binary,
    ]
)
inputs = [
    Path(__file__),
    profile,
    binary,
    source / "vm-start.h",
    harness / "jre_layout_spike.h",
    project / "src/native/signal_profile.c",
    *files,
    *(p for pair in rejections for p in pair),
    *sorted(p for p in payload.rglob("*") if p.is_file()),
]
hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
digest = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
remote = "/data/local/tmp/gw-signal-profile-" + digest[:16]
adb = ["adb", "-s", args.serial]
run([*adb, "shell", "mkdir", "-p", remote])
run([*adb, "push", str(payload) + "/.", remote])
run([*adb, "push", binary, remote + "/"])
for index, (library, _) in enumerate(rejections):
    run([*adb, "push", library, remote + f"/rejected-{index}.so"])
environment = {
    "GW_JAVA_HOME": remote + "/jre",
    "GW_JVM_LIB": remote + "/jre/lib/server/libjvm.so",
    "GW_JAVA_BOOT_LIB": remote + "/jre/lib",
    "GW_DOMAIN_JAR": remote + "/helpers.jar:" + remote + "/jvm-helpers.jar",
    "GW_LATE_BRIDGE": remote + "/libgw_jvm_jni.so",
    "LD_LIBRARY_PATH": ":".join(
        [remote, remote + "/jre/lib", remote + "/jre/lib/server"]
    ),
}
manifest = {
    "execution": "Android ARM64 shell, explicitly selected JRE17 startup",
    "serial": args.serial,
    "sha256": hashes,
    "commands": commands,
}
rows = []
try:
    cases = [
        (f"rollback-{point}", {"GW_LATE_FAIL_AFTER": str(point)}, point)
        for point in range(1, 6)
    ]
    cases += [
        (
            f"rejected-{index}",
            {
                "GW_LATE_BRIDGE": remote + f"/rejected-{index}.so",
                "GW_LATE_EXPECT_STATUS": str(errno.ENOTSUP),
            },
            None,
        )
        for index in range(len(rejections))
    ]
    cases.append(("reclaim", {"GW_LATE_RECLAIM_TEST": "1"}, None))
    for label, overrides, point in cases:
        settings = {**environment, **overrides}
        command = [
            *adb,
            "shell",
            "cd "
            + shlex.quote(remote)
            + " && "
            + shlex.join(
                [
                    "env",
                    *(k + "=" + v for k, v in settings.items()),
                    "/system/bin/timeout",
                    "-s",
                    "KILL",
                    "30",
                    "/system/bin/linker64",
                    remote + "/signal-profile-host",
                ]
            ),
        ]
        commands.append(command)
        try:
            result = subprocess.run(
                command, text=True, capture_output=True, timeout=40
            )
        except subprocess.TimeoutExpired as error:
            for stream in ("stdout", "stderr"):
                value = getattr(error, stream) or b""
                if isinstance(value, bytes):
                    value = value.decode(errors="replace")
                (out / f"{label}.{stream}").write_text(value)
            raise

        (out / f"{label}.stdout").write_text(result.stdout)
        (out / f"{label}.stderr").write_text(result.stderr)
        result.check_returncode()
        rollback, finished = [], []
        for line in result.stdout.splitlines():
            if line.startswith("LATE_SIGNAL_ROLLBACK "):
                rollback.append(json.loads(line.split(" ", 1)[1]))
            elif line.startswith("LATE_SIGNAL_REJECTION "):
                finished.append(json.loads(line.split(" ", 1)[1]))
            elif line.startswith("LATE_SIGNAL_RECLAIM "):
                finished.append(json.loads(line.split(" ", 1)[1]))
        expected = (
            [{"result": "PASS", "status": errno.ENOMEM, "bindings_restored": 5}]
            if point is not None
            else []
        )
        assert rollback == expected, rollback
        outcome = {"result": "PASS", "vm_destroyed": True}
        if label == "reclaim":
            outcome["reclaimed"] = 1
        assert finished == [outcome], finished
        assert "handler modified" not in result.stdout + result.stderr
        rows.append({"case": label, **finished[0]})
except Exception as error:
    (out / "failure.json").write_text(
        json.dumps({"result": "FAIL", "error": str(error)}, indent=2) + "\n"
    )
    raise
finally:
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

summary = {"result": "PASS", "cases": rows}
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
