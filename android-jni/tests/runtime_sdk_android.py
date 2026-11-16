"""Test installed JNI runtime packages on an explicit Android/JRE artifact."""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path

from sdk_test_result import execute_case

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
for name in (
    "core-prefix",
    "dlopen-prefix",
    "jni-prefix",
    "jni-include",
    "ffi",
    "runtime",
    "ordinary-runtime",
    "jdk",
    "ndk",
    "jre-assets",
    "jre-libs",
    "vm-startup-root",
    "vm-startup-profile",
):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--serial", required=True)
parser.add_argument("--jvm-prefix", type=Path)
parser.add_argument("--late-signals", action="store_true")
parser.add_argument("--expect-unconfigured-profile", action="store_true")
parser.add_argument("--nonlocal", dest="nonlocal_test", action="store_true")
parser.add_argument("--native-unwind", action="store_true")
parser.add_argument(
    "--unwind-windows",
    action="store_true",
    help="Exercise deterministic hooks in a test-enabled SDK",
)
parser.add_argument(
    "--signal-pending",
    action="store_true",
    help="Require Host-blocked signals to stay pending during Guest execution",
)
parser.add_argument(
    "--host-fault-mask",
    action="store_true",
    help="Require Guest fault recovery while Host blocks the same signal",
)
args = parser.parse_args()
if args.host_fault_mask and not (args.nonlocal_test and args.native_unwind):
    parser.error("--host-fault-mask requires --nonlocal --native-unwind")
if args.signal_pending and not (args.nonlocal_test and args.native_unwind):
    parser.error("--signal-pending requires --nonlocal --native-unwind")
if args.unwind_windows and not (args.nonlocal_test and args.native_unwind):
    parser.error("--unwind-windows requires --nonlocal --native-unwind")
for key, value in vars(args).items():
    if isinstance(value, Path):
        setattr(args, key, value.resolve())
if (
    args.late_signals or args.expect_unconfigured_profile
) and not args.jvm_prefix:
    parser.error(
        "An explicit installed JVM prefix is required for profile testing"
    )
source = project / "tests/runtime-sdk"
late = args.late_signals or args.expect_unconfigured_profile
out = (
    project
    / "build/tests"
    / (
        "runtime-sdk-android-unconfigured"
        if args.expect_unconfigured_profile
        else "runtime-sdk-android-late"
        if late
        else "runtime-sdk-android"
    )
)
if args.nonlocal_test:
    out = out.with_name(out.name + "-nonlocal")
if args.native_unwind:
    out = out.with_name(out.name + "-recovery")
if args.signal_pending:
    out = out.with_name(out.name + "-pending")
if args.host_fault_mask:
    out = out.with_name(out.name + "-host-mask")
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
(out / "failure.json").unlink(missing_ok=True)
profile = json.loads((args.jre_assets / "jre-artifact.json").read_text())
if profile["abi"] != "arm64-v8a" or profile["major"] != 17:
    parser.error(
        "This VM-startup fixture requires the explicitly reviewed ARM64 JRE17"
    )
for name in ("libjvm", "libjava"):
    path = args.jre_libs / Path(profile[name]["path"]).name
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if (
        digest != profile[name]["sha256"]
        or digest not in args.vm_startup_profile.read_text()
    ):
        parser.error("VM startup artifact/profile mismatch: " + str(path))
commands = []


def run(command, **kwargs):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **kwargs)


prefixes = [args.core_prefix, args.dlopen_prefix, args.jni_prefix]
if args.jvm_prefix:
    prefixes.append(args.jvm_prefix)
run(
    [
        "cmake",
        "-S",
        source,
        "-B",
        out,
        "-DCMAKE_TOOLCHAIN_FILE="
        + str(args.ndk / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-29",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",
        "-DCMAKE_PREFIX_PATH=" + ";".join(str(p.resolve()) for p in prefixes),
        "-DGW_JNI_INCLUDE_DIR=" + str(args.jni_include.resolve()),
        "-DGW_TEST_NATIVE_UNWIND=" + ("ON" if args.native_unwind else "OFF"),
        "-DGW_TEST_LATE_SIGNALS=" + ("ON" if late else "OFF"),
        "-DGW_VM_STARTUP_ROOT=" + str(args.vm_startup_root.resolve()),
        "-DGW_VM_STARTUP_PROFILE=" + str(args.vm_startup_profile.resolve()),
    ]
)
run(["cmake", "--build", out, "--parallel", "4"])
run(
    [
        args.jdk / "bin/javac",
        "--release",
        "8",
        "-d",
        out / "classes",
        source / "SdkProbe.java",
    ]
)
run([args.jdk / "bin/jar", "cf", out / "probe.jar", "-C", out / "classes", "."])
run(
    [
        "python3",
        project / "scripts/gen-jni.py",
        "--jni-header",
        args.jni_include / "jni.h",
        "--output",
        out / "generated",
    ]
)
service_env = {
    **os.environ,
    "GW_GUEST_CC": "aarch64-linux-gnu-gcc",
    "GW_GUEST_OUT": str(out / "service"),
    "GW_CORE_INCLUDE": str(args.core_prefix / "include/glibc-worker/core"),
    "GW_DLOPEN_INCLUDE": str(
        args.dlopen_prefix / "include/glibc-worker/bionic-dlopen"
    ),
    "GW_JNI_INCLUDE": str(args.jni_include),
    "GW_FFI_GUEST": str(args.ffi),
    "GW_JNI_GENERATED": str(out / "generated"),
    "GW_NATIVE_UNWIND_INTEGRATION": "1" if args.native_unwind else "0",
}
run([project / "build-guest.sh"], env=service_env)
run(
    [
        "aarch64-linux-gnu-gcc",
        "-std=gnu17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-shared",
        "-fPIC",
        "-I" + str(args.jni_include),
        source / "guest.c",
        source / "guest-checked.c",
        source / "guest-faults.c",
        source / "guest-stacks.c",
        source / "guest-waits.c",
        source / "guest-registrations.c",
        "-o",
        out / "module.so",
        "-lm",
    ]
)
payload = out / "payload"
payload.mkdir(exist_ok=True)
files = []


def copy(origin, destination):
    files.append(origin)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(origin, destination)


for name in ("runtime_sdk", "service", "module.so", "probe.jar"):
    copy(out / name, payload / name)
for prefix, name in (
    (args.jni_prefix, "libgw_android_jni.so"),
    (args.dlopen_prefix, "libgw_dlopen.so"),
):
    copy(prefix / "lib" / name, payload / name)
copy(
    args.jni_prefix / "share/glibc-worker/android-jni/android-jni-helpers.jar",
    payload / "helpers.jar",
)
if args.jvm_prefix:
    copy(args.jvm_prefix / "lib/libgw_jvm_jni.so", payload / "libgw_jvm_jni.so")
    copy(
        args.jvm_prefix / "share/glibc-worker/jvm-jni/jvm-jni-helpers.jar",
        payload / "jvm-helpers.jar",
    )
for directory, runtime in (
    (payload, args.runtime),
    (payload / "ordinary", args.ordinary_runtime),
):
    for name in ("ld-linux-aarch64.so.1", "libc.so.6", "libm.so.6"):
        copy(runtime / name, directory / name)
unwind = Path(
    subprocess.check_output(
        ["aarch64-linux-gnu-gcc", "-print-file-name=libgcc_s.so.1"], text=True
    ).strip()
)
copy(unwind, payload / "libgcc_s.so.1")
home = payload / "jre"
for name in ("release", "lib/modules", "lib/tzdb.dat"):
    copy(args.jre_assets / name, home / name)
if (args.jre_assets / "conf").is_dir():
    shutil.copytree(args.jre_assets / "conf", home / "conf", dirs_exist_ok=True)
for name in ("jvm", "java", "verify", "net", "nio", "zip", "jimage"):
    lib = "lib" + name + ".so"
    copy(
        args.jre_libs / lib,
        home / "lib" / ("server/" if name == "jvm" else "") / lib,
    )
digest = hashlib.sha256()
for path in files:
    digest.update(path.read_bytes())
identity = digest.hexdigest()[:16]
remote = "/data/local/tmp/gw-runtime-sdk-" + identity
adb = ["adb", "-s", args.serial]
run([*adb, "shell", "mkdir", "-p", remote])
run([*adb, "push", str(payload) + "/.", remote])
files += [
    *source.glob("*"),
    Path(__file__),
    Path(__file__).with_name("sdk_test_result.py"),
    args.vm_startup_profile,
]
if args.jvm_prefix:
    profile_file = (
        args.jvm_prefix / "share/glibc-worker/jvm-jni/signal-profile.h"
    )
    if profile_file.is_file():
        files.append(profile_file)
files.append(project / "build-guest.sh")
files += [Path(p) for p in (out / "sdk-inputs.txt").read_text().splitlines()]
for directory in (
    project / "src/guest",
    project / "src/common",
    args.core_prefix / "include/glibc-worker/core",
    args.jni_include,
    args.ffi / "include",
):
    files += sorted(
        p
        for p in directory.rglob("*")
        if p.is_file() and p.suffix in (".c", ".h", ".S")
    )
files.append(args.ffi / "lib/libffi.a")


def write_manifest():
    (out / "manifest.json").write_text(
        json.dumps(
            {
                "commands": commands,
                "jre_artifact": profile,
                "sha256": {
                    str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in files
                },
            },
            indent=2,
        )
        + "\n"
    )


rows = []
cases = [("default", False), ("worker", False), ("reject-ordinary-libc", True)]
if args.expect_unconfigured_profile:
    cases = [("unconfigured-profile", False)]
if args.nonlocal_test:
    cases = [("nonlocal", False)]
    if args.native_unwind:
        cases += [
            ("guest-default-" + str(mode), False)
            for mode in (33, 34, 35, 82, 83)
        ]
for mode, reject in cases:
    fatal = (
        int(mode.rsplit("-", 1)[1])
        if mode.startswith("guest-default-")
        else None
    )
    runtime = remote + ("/ordinary" if reject else "")
    classpath = remote + "/probe.jar:" + remote + "/helpers.jar"
    if args.jvm_prefix:
        classpath += ":" + remote + "/jvm-helpers.jar"
    env = {
        **({"GW_TEST_FATAL_MODE": str(fatal)} if fatal else {}),
        "GW_JAVA_HOME": remote + "/jre",
        **({"GW_TEST_UNWIND_WINDOWS": "1"} if args.unwind_windows else {}),
        **({"GW_TEST_SIGNAL_PENDING": "1"} if args.signal_pending else {}),
        **({"GW_TEST_HOST_FAULT_MASK": "1"} if args.host_fault_mask else {}),
        "GW_JVM_LIB": remote + "/jre/lib/server/libjvm.so",
        "GW_JAVA_BOOT_LIB": remote + "/jre/lib",
        "GW_DOMAIN_JAR": classpath,
        "LD_LIBRARY_PATH": remote
        + ":"
        + remote
        + "/jre/lib:"
        + remote
        + "/jre/lib/server",
        **({"GW_EXPECT_RUNTIME_REJECTION": "1"} if reject else {}),
        **(
            {"GW_EXPECT_SIGNAL_PROFILE_REJECTION": "1"}
            if args.expect_unconfigured_profile
            else {}
        ),
    }
    native = [
        "/system/bin/timeout",
        "60",
        "/system/bin/linker64",
        remote + "/runtime_sdk",
        remote + "/jre/lib/server/libjvm.so",
        remote + "/probe.jar:" + remote + "/helpers.jar",
        runtime + "/ld-linux-aarch64.so.1",
        runtime,
        remote + "/service",
        remote + "/module.so",
        remote,
        mode,
    ]
    command = [
        *adb,
        "shell",
        "cd "
        + shlex.quote(remote)
        + " && "
        + shlex.join(["env", *(k + "=" + v for k, v in env.items()), *native]),
    ]
    commands.append(command)
    write_manifest()
    rows.append(
        execute_case(
            command,
            out,
            mode,
            90,
            require_pending=args.signal_pending,
            require_host_fault_mask=args.host_fault_mask,
            require_guest_target=args.native_unwind and args.unwind_windows,
            fpe_optional=True,
            fatal=fatal,
        )
    )
    print(rows[-1])
(out / "summary.json").write_text(
    json.dumps({"result": "PASS", "runs": rows}, indent=2) + "\n"
)
