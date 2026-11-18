"""Validate a JNI runtime consumer against separately installed SDK packages."""

import argparse
import hashlib
import json
import os
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
):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument(
    "--nonlocal",
    dest="nonlocal_test",
    action="store_true",
    help="Require transparent cross-domain jump and teardown",
)
parser.add_argument(
    "--native-unwind",
    action="store_true",
    help="Link opt-in native unwind capture/restore integration",
)
parser.add_argument("--worker-signals", action="store_true")
parser.add_argument("--adopted-signals", action="store_true")
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
if args.worker_signals and (not args.native_unwind or args.nonlocal_test):
    parser.error("--worker-signals requires --native-unwind without --nonlocal")
if args.adopted_signals and (
    not args.native_unwind or args.nonlocal_test or args.worker_signals
):
    parser.error("--adopted-signals requires a standalone native-unwind run")
if args.host_fault_mask and not (args.nonlocal_test and args.native_unwind):
    parser.error("--host-fault-mask requires --nonlocal --native-unwind")
if args.signal_pending and not (args.nonlocal_test and args.native_unwind):
    parser.error("--signal-pending requires --nonlocal --native-unwind")
if args.unwind_windows and not (args.nonlocal_test and args.native_unwind):
    parser.error("--unwind-windows requires --nonlocal --native-unwind")
for key, value in vars(args).items():
    if isinstance(value, Path):
        setattr(args, key, value.resolve())
source = project / "tests/runtime-sdk"
name = "runtime-sdk-nonlocal" if args.nonlocal_test else "runtime-sdk"
if args.native_unwind:
    name += "-recovery"
if args.signal_pending:
    name += "-pending"
if args.host_fault_mask:
    name += "-host-mask"
if args.worker_signals:
    name += "-worker-signals"
if args.adopted_signals:
    name += "-adopted-signals"
out = project / "build/tests" / name
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
(out / "failure.json").unlink(missing_ok=True)
commands = []


def run(command, **kwargs):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **kwargs)


prefixes = [args.core_prefix, args.dlopen_prefix, args.jni_prefix]
run(
    [
        "cmake",
        "-S",
        source,
        "-B",
        out,
        "-DCMAKE_PREFIX_PATH=" + ";".join(str(p.resolve()) for p in prefixes),
        "-DGW_JNI_INCLUDE_DIR=" + str(args.jni_include.resolve()),
        "-DGW_TEST_NATIVE_UNWIND=" + ("ON" if args.native_unwind else "OFF"),
    ]
)
run(["cmake", "--build", out, "--parallel", "4"])
run(
    [
        args.jdk / "bin/javac",
        "--release",
        "8",
        "-d",
        out,
        source / "SdkProbe.java",
    ]
)
service_env = {
    **os.environ,
    "GW_GUEST_CC": "gcc",
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
run([project / "build-guest.sh"], env=service_env)
run(
    [
        "gcc",
        "-std=gnu17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-shared",
        "-fPIC",
        "-fexceptions",
        "-fnon-call-exceptions",
        "-I" + str(args.jni_include),
        source / "guest.c",
        source / "guest-checked.c",
        source / "guest-faults.c",
        source / "guest-stacks.c",
        source / "guest-waits.c",
        source / "guest-registrations.c",
        source / "guest-signal-unwind.c",
        source / "guest-worker-signals.c",
        source / "guest-adopted-signals.c",
        source / "guest-signalfd.c",
        "-o",
        out / "module.so",
        "-lm",
    ]
)
support = out / "gnu-support"
support.mkdir(exist_ok=True)
unwind = Path(
    subprocess.check_output(
        ["gcc", "-print-file-name=libgcc_s.so.1"], text=True
    ).strip()
).resolve(strict=True)
shutil.copyfile(unwind, support / "libgcc_s.so.1")
files = [
    *source.glob("*"),
    Path(__file__),
    Path(__file__).with_name("sdk_test_result.py"),
    out / "runtime_sdk",
    out / "service",
    out / "module.so",
    support / "libgcc_s.so.1",
    unwind,
    args.runtime / "manifest.json",
]
files += [Path(p) for p in (out / "sdk-inputs.txt").read_text().splitlines()]
files.append(args.jdk / "lib/server/libjvm.so")
files.append(project / "build-guest.sh")
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
for runtime in (args.runtime, args.ordinary_runtime):
    files += sorted(runtime.glob("*.so*"))


def write_manifest():
    (out / "manifest.json").write_text(
        json.dumps(
            {
                "commands": commands,
                "prefixes": list(map(str, prefixes)),
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
cases = (
    ("default", args.runtime, False),
    ("worker", args.runtime, False),
    ("borrowed-relay", args.runtime, False),
    ("worker-borrowed-relay", args.runtime, False),
    ("reject-ordinary-libc", args.ordinary_runtime, True),
)
if args.nonlocal_test:
    cases = (("nonlocal", args.runtime, False),)
    if args.native_unwind:
        cases += tuple(
            ("guest-default-" + str(mode), args.runtime, False)
            for mode in (33, 34, 35, 82, 83)
        )
if args.host_fault_mask:
    cases += (("router-quarantine", args.runtime, False),)
if args.worker_signals:
    cases = (("worker-signals", args.runtime, False),)
if args.adopted_signals:
    cases = (("adopted-signals", args.runtime, False),)
for mode, runtime, reject in cases:
    fatal = (
        int(mode.rsplit("-", 1)[1])
        if mode.startswith("guest-default-")
        else None
    )
    command = [
        out / "runtime_sdk",
        args.jdk / "lib/server/libjvm.so",
        str(out)
        + ":"
        + str(
            args.jni_prefix
            / "share/glibc-worker/android-jni/android-jni-helpers.jar"
        ),
        runtime / "ld-linux-x86-64.so.2",
        str(runtime) + ":" + str(support),
        out / "service",
        out / "module.so",
        out,
        mode,
    ]
    command = list(map(str, command))
    commands.append(command)
    write_manifest()
    rows.append(
        execute_case(
            command,
            out,
            mode,
            60,
            require_pending=args.signal_pending,
            require_host_fault_mask=args.host_fault_mask
            and mode != "router-quarantine",
            require_guest_target=args.native_unwind and args.unwind_windows,
            fatal=fatal,
            env={
                **os.environ,
                **(
                    {"GW_TEST_UNWIND_WINDOWS": "1"}
                    if args.unwind_windows
                    else {}
                ),
                **(
                    {"GW_TEST_SIGNAL_PENDING": "1"}
                    if args.signal_pending
                    else {}
                ),
                **({"GW_EXPECT_RUNTIME_REJECTION": "1"} if reject else {}),
                **(
                    {"GW_TEST_HOST_FAULT_MASK": "1"}
                    if args.host_fault_mask
                    else {}
                ),
                **({"GW_TEST_FATAL_MODE": str(fatal)} if fatal else {}),
            },
        )
    )
    print(rows[-1])
(out / "summary.json").write_text(
    json.dumps({"result": "PASS", "runs": rows}, indent=2) + "\n"
)
