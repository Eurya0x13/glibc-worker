"""Verify timely cross-libc Guest entry without claiming a public signal ABI."""

import argparse
import hashlib
import json
import platform
import shlex
import shutil
import subprocess
from pathlib import Path

core = Path(__file__).resolve().parents[1]
source = core / "tests/guest-async"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=["x86_64", "arm64-v8a"], default="x86_64")
parser.add_argument("--runtime", required=True, type=Path)
parser.add_argument("--notification", required=True, type=int)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--serial")
parser.add_argument("--output", type=Path)
parser.add_argument(
    "--ubsan",
    action="store_true",
    help="Instrument native Host/core only; Guest remains uninstrumented",
)
args = parser.parse_args()
if bool(args.ndk) != bool(args.serial):
    parser.error("Android requires both --ndk and --serial")
if args.ndk and args.abi != "arm64-v8a":
    parser.error("The current Android fixture requires --abi arm64-v8a")
if args.ubsan and args.ndk:
    parser.error("The UBSan fixture currently supports native Host/core only")
if (
    not args.ndk
    and platform.machine()
    != {
        "x86_64": "x86_64",
        "arm64-v8a": "aarch64",
    }[args.abi]
):
    parser.error("Native execution requires a matching host architecture")
if not 34 <= args.notification <= 62 or args.notification in [
    56,
    *range(50, 54),
]:
    parser.error(
        "Choose an explicit public RT number outside the fixture lanes/reservations"
    )

runtime = args.runtime.resolve()
configuration = json.loads((core / "config/targets.json").read_text())[args.abi]
loader = runtime / configuration["loader"]
for path in (
    loader,
    runtime / "libc.so.6",
    runtime / "libm.so.6",
    runtime / "manifest.json",
):
    if not path.is_file():
        parser.error(f"Missing explicit matched runtime input: {path}")
compiler = shutil.which(configuration["guest_cc"])
if not compiler:
    parser.error(f"Missing Guest compiler: {configuration['guest_cc']}")
adb_tool = shutil.which("adb") if args.serial else None
if args.serial and not adb_tool:
    parser.error("Missing Android platform-tools adb on PATH")
out = (
    args.output
    or core
    / "build/tests"
    / (
        ("guest-async-android" if args.ndk else "guest-async")
        + ("-ubsan" if args.ubsan else "")
    )
).resolve()
out.mkdir(parents=True, exist_ok=True)
for name in (
    "summary.json",
    "failure.json",
    "manifest.json",
    "stdout.log",
    "stderr.log",
    "control-summary.json",
    "control-stdout.log",
    "control-stderr.log",
    "actions-summary.json",
    "actions-stdout.log",
    "actions-stderr.log",
    "consume-summary.json",
    "consume-stdout.log",
    "consume-stderr.log",
    "targets-summary.json",
    "targets-stdout.log",
    "targets-stderr.log",
    "anchor-summary.json",
    "anchor-stdout.log",
    "anchor-stderr.log",
    "masks-summary.json",
    "masks-stdout.log",
    "masks-stderr.log",
    "creation-summary.json",
    "creation-stdout.log",
    "creation-stderr.log",
    "creation-order-summary.json",
    "creation-order-stdout.log",
    "creation-order-stderr.log",
):
    (out / name).unlink(missing_ok=True)
commands = []


def run(command, **options):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **options)


toolchain = (
    [
        "-DCMAKE_TOOLCHAIN_FILE="
        + str(args.ndk.resolve() / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-29",
    ]
    if args.ndk
    else []
)
if args.ubsan:
    clang = shutil.which("clang")
    if not clang:
        parser.error("Native Host UBSan requires clang on PATH")
    toolchain += [
        f"-DCMAKE_C_COMPILER={clang}",
        "-DCMAKE_C_FLAGS=-O2 -g -fsanitize=undefined -fno-sanitize-recover=all",
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined",
    ]
prefix = out / "sdk/install"
run(
    [
        "cmake",
        "-S",
        core,
        "-B",
        out / "sdk",
        "-DBUILD_TESTING=OFF",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        *toolchain,
    ]
)
run(["cmake", "--build", out / "sdk", "--parallel", "2"])
run(["cmake", "--install", out / "sdk"])
run(
    [
        "cmake",
        "-S",
        source,
        "-B",
        out / "host",
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",
        f"-DGW_DOMAIN_START={core / 'src/domain/start.S'}",
        f"-DGW_GUEST_CLONE={core / 'src/bootstrap/clone.S'}",
        *toolchain,
    ]
)
run(["cmake", "--build", out / "host", "--parallel", "2"])
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(core / "src/domain"),
        "-I" + str(core / "include"),
        source / "guest.c",
        source / "send-lifetime.c",
        source / "private-threads.c",
        source / "mask-inheritance.c",
        core / "src/domain/signal-pending.c",
        core / "src/domain/signal-inbox.c",
        core / "src/domain/signal-targets.c",
        core / "src/domain/signal-execution-guest.c",
        core / "src/domain/signal-execution.c",
        core / "src/domain/start.S",
        "-ldl",
        "-pthread",
        "-lm",
        "-o",
        out / "guest",
    ]
)
run(
    [
        compiler,
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        source / "send-control.c",
        "-pthread",
        "-o",
        out / "send-control",
    ]
)
control_loader = out / "control-loader"
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(core / "src/domain"),
        "-I" + str(core / "src/runtime"),
        source / "create-control.c",
        core / "src/domain/signal-pending.c",
        core / "src/domain/signal-inbox.c",
        core / "src/domain/signal-targets.c",
        "-ldl",
        "-pthread",
        "-o",
        out / "create-control",
    ]
)
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        source / "mask-control.c",
        "-ldl",
        "-pthread",
        "-o",
        out / "mask-control",
    ]
)
shutil.copyfile(loader, control_loader)
control_loader.chmod(0o755)
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(core / "src/domain"),
        core / "tests/signal_targets.c",
        core / "src/domain/signal-pending.c",
        core / "src/domain/signal-inbox.c",
        core / "src/domain/signal-targets.c",
        "-pthread",
        "-o",
        out / "target-control",
    ]
)
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(core / "src/domain"),
        core / "tests/signal_actions.c",
        core / "src/domain/signal-actions.c",
        core / "src/domain/signal-pending.c",
        core / "src/domain/signal-inbox.c",
        core / "src/domain/signal-targets.c",
        "-pthread",
        "-o",
        out / "actions-control",
    ]
)
run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(core / "src/domain"),
        "-I" + str(core / "src/runtime"),
        source / "consume-control.c",
        core / "src/domain/signal-consuming.c",
        core / "src/domain/guest-fd-lease.c",
        core / "src/domain/guest-signalfd.c",
        core / "src/domain/guest-epoll.c",
        core / "src/domain/signal-user-copy.c",
        core / "src/domain/signal-actions.c",
        core / "src/domain/signal-pending.c",
        core / "src/domain/signal-inbox.c",
        core / "src/domain/signal-targets.c",
        "-ldl",
        "-pthread",
        "-o",
        out / "consume-control",
    ]
)
unwind = Path(
    subprocess.check_output(
        [compiler, "-print-file-name=libgcc_s.so.1"], text=True
    ).strip()
).resolve()
if not unwind.is_file():
    parser.error("Missing Guest unwind library")
shutil.copyfile(unwind, out / "libgcc_s.so.1")
sdk_inputs = [
    Path(p) for p in (out / "host/sdk-inputs.txt").read_text().splitlines()
]
host_compiler = (out / "host/host-compiler.txt").read_text().strip()
inputs = [
    Path(__file__),
    core / "CMakeLists.txt",
    core / "tests/signal_targets.c",
    core / "tests/signal_actions.c",
    core / "config/targets.json",
    *source.iterdir(),
    *core.glob("src/domain/*.[chS]"),
    *core.glob("src/domain/*.inc"),
    *core.glob("include/*.h"),
    *core.glob("src/bootstrap/*.[chS]"),
    *core.glob("cmake/*"),
    core / "src/bootstrap/clone.S",
    core / "src/domain/start.S",
    loader,
    runtime / "libc.so.6",
    runtime / "libm.so.6",
    runtime / "manifest.json",
    out / "guest",
    out / "send-control",
    out / "mask-control",
    out / "create-control",
    core / "src/runtime/thread-create.h",
    core / "src/runtime/signal-consume-hook.h",
    out / "target-control",
    out / "actions-control",
    out / "consume-control",
    control_loader,
    out / "host/guest_async_host",
    out / "host/guest_async_no_anchor",
    out / "libgcc_s.so.1",
    *sdk_inputs,
]
manifest = {
    "commands": commands,
    "abi": args.abi,
    "notification": args.notification,
    "execution": "android-adb-shell" if args.serial else "native-linux",
    "host_libc": "bionic" if args.serial else "glibc",
    "guest_libc": "glibc",
    "sanitizer": "Host/core UBSan; Guest uninstrumented"
    if args.ubsan
    else "none",
    "compilers": {
        name: subprocess.check_output(
            [name, "--version"], text=True
        ).splitlines()[0]
        for name in {compiler, host_compiler}
    },
    "sha256": {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in inputs
        if p.is_file()
    },
}
command = [
    out / "host/guest_async_host",
    loader,
    f"{runtime}:{out}",
    out / "guest",
    args.notification,
]
if args.serial:
    adb = [adb_tool, "-s", args.serial]
    digest = hashlib.sha256(
        json.dumps(manifest, sort_keys=True).encode()
    ).hexdigest()[:16]
    remote = "/data/local/tmp/gw-guest-async-" + digest
    run([*adb, "shell", "mkdir", "-p", remote])
    for path in [
        out / "host/guest_async_host",
        out / "host/guest_async_no_anchor",
        out / "guest",
        out / "send-control",
        out / "mask-control",
        out / "create-control",
        out / "target-control",
        out / "actions-control",
        out / "consume-control",
        control_loader,
        out / "libgcc_s.so.1",
        loader,
        runtime / "libc.so.6",
        runtime / "libm.so.6",
    ]:
        run([*adb, "push", path, remote + "/"])
    run([*adb, "shell", "chmod", "0755", remote + "/control-loader"])
    manifest["device"] = {
        "serial": args.serial,
        "fingerprint": run(
            [*adb, "shell", "getprop", "ro.build.fingerprint"],
            capture_output=True,
            text=True,
        ).stdout.strip(),
        "identity": run(
            [*adb, "shell", "id"], capture_output=True, text=True
        ).stdout.strip(),
    }
    invocation = [
        "/system/bin/timeout",
        "-s",
        "KILL",
        "25",
        "/system/bin/linker64",
        remote + "/guest_async_host",
        remote + "/" + loader.name,
        remote,
        remote + "/guest",
        str(args.notification),
    ]
    command = [*adb, "shell", shlex.join(invocation)]


def native_command(binary, *arguments):
    if args.serial:
        return [
            *adb,
            "shell",
            shlex.join(
                [
                    "/system/bin/timeout",
                    "-s",
                    "KILL",
                    "10",
                    remote + "/control-loader",
                    "--library-path",
                    remote,
                    remote + "/" + binary,
                    *arguments,
                ]
            ),
        ]
    return list(
        map(
            str,
            [
                control_loader,
                "--library-path",
                f"{runtime}:{out}",
                out / binary,
                *arguments,
            ],
        )
    )


command = list(map(str, command))
if args.serial:
    missing_anchor = [
        *adb,
        "shell",
        shlex.join(
            [
                part.replace("/guest_async_host", "/guest_async_no_anchor")
                for part in invocation
            ]
        ),
    ]
else:
    missing_anchor = [
        part.replace("/guest_async_host", "/guest_async_no_anchor")
        for part in command
    ]
controls = [
    (
        "control",
        native_command("send-control"),
        {"result": "PASS", "native_handler_errno": True, "cases": 6},
    ),
    (
        "targets",
        native_command("target-control"),
        {
            "result": "PASS",
            "targets": 2048,
            "concurrent_senders": 4,
            "discard_quota": True,
            "blocked_consuming": True,
            "prepared_admission": True,
        },
    ),
    (
        "actions",
        native_command("actions-control"),
        {
            "result": "PASS",
            "snapshots": True,
            "one_shot": True,
            "ignore_flush": True,
            "quota_recovered": True,
            "blocked_ignored": True,
            "native_ignored_control": True,
            "job_control_pending": True,
            "concurrent_senders": 4,
        },
    ),
    (
        "consume",
        native_command("consume-control"),
        {
            "result": "PASS",
            "public_waits": True,
            "host_pending_separate": True,
            "timeout_and_copyout": True,
            "blocked_publication": True,
            "guest_eintr": True,
            "host_eintr_hidden": True,
            "native_cancel": True,
        },
    ),
    (
        "creation",
        native_command("create-control"),
        {
            "result": "PASS",
            "children": 130,
            "early_exits": 64,
            "prestart_send": True,
            "cancel_cleanup": True,
            "prepare_failure": True,
            "clone_failure": True,
            "setup_failure": True,
        },
    ),
    (
        "creation-order",
        native_command("create-control", "--child-only"),
        {"result": "PASS", "missing_parent_admission_detected": True},
    ),
    (
        "anchor",
        missing_anchor,
        {"result": "PASS", "missing_host_anchor_detected": True},
    ),
    (
        "masks",
        native_command("mask-control"),
        {
            "result": "PASS",
            "threads": 5,
            "logical_inheritance": True,
            "explicit_attributes": True,
            "c11_inheritance": True,
            "context_masks": True,
            "jump_masks": True,
            "temporary_masks": True,
            "errno": True,
        },
    ),
]
commands.extend(item[1] for item in controls)
commands.append(command)
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
capture_prefix = ""
try:
    for name, invocation, expected in controls:
        capture_prefix = name + "-"
        native = subprocess.run(
            invocation, capture_output=True, text=True, timeout=15
        )
        (out / (capture_prefix + "stdout.log")).write_text(native.stdout)
        (out / (capture_prefix + "stderr.log")).write_text(native.stderr)
        native.check_returncode()
        native_data = json.loads(native.stdout)
        assert native_data == expected, native_data
        (out / (capture_prefix + "summary.json")).write_text(
            json.dumps(native_data, indent=2) + "\n"
        )

    capture_prefix = ""
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (out / "stdout.log").write_text(result.stdout)
    (out / "stderr.log").write_text(result.stderr)
    result.check_returncode()
    data = json.loads(result.stdout)
    assert data.pop("result") == "PASS"
    assert data.pop("packets") == 216
    assert data.pop("self_packets") == 72
    assert data.pop("self_entries") == 72
    assert data.pop("host_entries") >= 24
    assert data.pop("guest_entries") == 12
    assert data.pop("retired_async_entries") == 36
    assert data.pop("production_ready") is False
    assert data.pop("bionic_private_routes") is bool(args.ndk)
    assert data.pop("gnu_host_private_lifecycle") is not bool(args.ndk)
    assert data and all(value is True for value in data.values()), data
except Exception as error:
    if isinstance(error, subprocess.TimeoutExpired):
        for name, partial in (
            ("stdout", error.stdout),
            ("stderr", error.stderr),
        ):
            (out / (capture_prefix + name + ".log")).write_bytes(
                partial.encode() if isinstance(partial, str) else partial or b""
            )
    (out / "failure.json").write_text(
        json.dumps({"result": "FAIL", "error": str(error)}, indent=2) + "\n"
    )
    raise
summary = json.loads(result.stdout)
summary.update(
    native_handler_errno_control=True,
    native_targets_control=True,
    native_actions_control=True,
    native_consuming_waits_control=True,
    missing_host_anchor_control=True,
    native_mask_lifecycle_control=True,
    native_creation_lifecycle_control=True,
    guest_public_signal_abi=False,
    jni_vm_validated=False,
    nonlocal_async_entry_validated=False,
)
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
