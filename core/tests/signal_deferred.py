"""Verify kernel notifications and recipient retirement on Linux or Android."""

import argparse
import hashlib
import json
import os
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
    / ("signal-deferred-android" if args.ndk else "signal-deferred")
)
out.mkdir(parents=True, exist_ok=True)
for name in ("summary.json", "failure.json"):
    (out / name).unlink(missing_ok=True)
commands = []


def run(command, **options):
    command = list(map(str, command))
    commands.append(command)
    return subprocess.run(command, check=True, **options)


configure = ["cmake", "-S", core, "-B", out / "sdk", "-DBUILD_TESTING=ON"]
if args.ndk:
    configure += [
        "-DCMAKE_TOOLCHAIN_FILE="
        + str(args.ndk.resolve() / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-29",
    ]
run(configure)
targets = [
    "signal_inbox_test",
    "signal_deferred_test",
    "signal_ordering_test",
    "signal_handler_order_test",
    "signal_restore_test",
    "carrier_lifecycle_test",
    "signal_relay_test",
    "signal_stdio_test",
    "signal_waiting_test",
    "signal_sleep_control",
    "signal_sleep_test",
    "signal_sleep_lifecycle_test",
    "signal_fd_test",
    "signal_fd_lifecycle_test",
    "signal_delivery_test",
    "signal_restart_test",
    "signal_consume_test",
]
run(["cmake", "--build", out / "sdk", "--target", *targets, "--parallel", "2"])
inputs = [
    Path(__file__),
    core / "CMakeLists.txt",
    core / "tests/signal_inbox.c",
    core / "tests/signal_deferred.c",
    core / "tests/signal_ordering.c",
    core / "tests/signal_handler_order.c",
    core / "tests/signal_restore.c",
    core / "tests/carrier_lifecycle.c",
    core / "tests/signal_relay.c",
    core / "tests/signal_stdio.c",
    core / "tests/signal_waiting.c",
    core / "tests/signal_sleep_control.c",
    core / "tests/signal_sleep_lifecycle.c",
    core / "tests/signal_fd.c",
    core / "tests/signal_fd_lifecycle.c",
    core / "tests/signal_delivery.c",
    core / "tests/signal_restart.c",
    core / "tests/signal_close_backend.c",
    core / "tests/signal_consume.c",
    *sorted((core / "include").glob("*.h")),
    *[
        p
        for p in (core / "src/domain").iterdir()
        if p.suffix in (".c", ".h", ".S", ".inc")
    ],
    *[out / "sdk" / target for target in targets],
    out / "sdk/libsignal_close_backend.so",
]
manifest = {
    "commands": commands,
    "sha256": {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs
    },
}
cases, controls = [], []
try:
    for target, arguments in [
        *((target, []) for target in targets),
        ("signal_inbox_test", ["--notification-control"]),
        ("signal_waiting_test", ["--native"]),
        ("signal_fd_test", ["--native-close-window"]),
    ]:
        binary = out / "sdk" / target
        command = [binary]
        environment = None
        if args.serial:
            remote = (
                "/data/local/tmp/gw-deferred-"
                + manifest["sha256"][str(binary)][:16]
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
        if arguments == ["--native-close-window"]:
            backend = out / "sdk/libsignal_close_backend.so"
            if args.serial:
                remote_backend = (
                    "/data/local/tmp/gw-close-"
                    + manifest["sha256"][str(backend)][:16]
                    + ".so"
                )
                run(["adb", "-s", args.serial, "push", backend, remote_backend])
                command.insert(4, "LD_PRELOAD=" + remote_backend)
            else:
                environment = {**os.environ, "LD_PRELOAD": str(backend)}
        command = list(map(str, command)) + arguments
        commands.append(
            ["env", "LD_PRELOAD=" + str(backend), *command]
            if environment is not None
            else command
        )
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=20, env=environment
        )
        case = target + (arguments[0][1:] if arguments else "")
        (out / (case + ".log")).write_text(result.stdout + result.stderr)
        result.check_returncode()
        if target == "signal_sleep_control":
            data = json.loads(result.stdout)
            assert data == {
                "result": "CONTROL_ONLY",
                "native_only": True,
                "sa_restart": True,
                "success_ignores_remainder": True,
                "interrupted_remainder_efault": True,
                "clock_errno_preserved": True,
                "different_timeout_saturation": True,
            }, data
            controls.append({"case": case, **data})
            continue
        if target == "signal_inbox_test" and arguments:
            data = json.loads(result.stdout)
            assert data == {
                "result": "CONTROL_ONLY",
                "native_only": True,
                "sa_restart": True,
                "guest_packet": False,
                "host_ppoll_eintr": True,
            }, data
            controls.append({"case": case, **data})
            continue
        if target == "signal_sleep_lifecycle_test":
            data = json.loads(result.stdout)
            expected = {
                "result": "PASS",
                "sleep_siglongjmp": True,
                "sleep_after_jump": True,
                "sleep_actual_block_notification": True,
                "sleep_preentry_rearm": True,
            }
            expected.update(
                {
                    "sleep_setcontext": "unavailable",
                    "pthread_cancel": "unavailable",
                }
                if args.ndk
                else {
                    "sleep_setcontext": True,
                    "pthread_cancel": True,
                    "cancel_before_entry": True,
                    "cancel_mask_restored": True,
                }
            )
            assert data == expected, data
            data.pop("result")
        elif target == "signal_fd_lifecycle_test":
            observations = [
                json.loads(line) for line in result.stdout.splitlines()
            ]
            assert len(observations) == 6, observations
            data = observations.pop()
            assert data.pop("result") == "PASS"
            assert data["fd_lifecycle_cases"] == 5
            assert {row["case"] for row in observations} == {
                "raw-duplicate",
                "scm-rights",
                "close-range",
                "private-table",
                "untracked-before-enable",
            }
            skipped = 0
            for row in observations:
                assert row["stage"] == "fd-lifecycle"
                if row["result"] == "UNAVAILABLE":
                    assert row["case"] in ("close-range", "private-table")
                    assert row["native_error"] in (1, 22, 38)
                    skipped += 1
                else:
                    assert row["result"] == "PASS" and not row["native_error"]
            assert data["native_unavailable"] == skipped
            data["observations"] = observations
        else:
            data = json.loads(result.stdout)
            assert data.pop("result") == "PASS" and data
            assert all(value is True for value in data.values()), data
        cases.append({"case": case, "result": "PASS", **data})
except Exception as error:
    (out / "failure.json").write_text(
        json.dumps({"result": "FAIL", "error": str(error)}, indent=2) + "\n"
    )
    raise
finally:
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out / "summary.json").write_text(
    json.dumps(
        {"result": "PASS", "cases": cases, "native_controls": controls},
        indent=2,
    )
    + "\n"
)
print(
    json.dumps({"result": "PASS", "cases": cases, "native_controls": controls})
)
