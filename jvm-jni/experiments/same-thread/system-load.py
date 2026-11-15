"""Validate actual System.load hooks against both dual-libc transports."""

import argparse
import datetime
import hashlib
import json
import os
import shlex
import shutil
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
root = source.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--jdk", type=Path, required=True)
parser.add_argument("--serial")
args = parser.parse_args()
build = args.build.resolve()
manifest = json.loads((build / "manifest.json").read_text())
if not manifest.get("system_load"):
    parser.error("Build with --system-load first")
for name, digest in manifest["sha256"].items():
    if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
        parser.error(f"Changed build input: {name}; rebuild before measuring")
android = manifest["host_libc"] == "bionic"
if android != bool(args.serial):
    parser.error("Android requires --serial; desktop must omit it")
identity = hashlib.sha256((build / "manifest.json").read_bytes()).hexdigest()[
    :16
]
stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S%fZ")
evidence = build / "runs" / stamp
evidence.mkdir(parents=True)
(evidence / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
runtime = Path(manifest["runtime"])
environment = os.environ.copy()
if android:
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/gw-production-jni-" + identity
    payload = build / "payload"
    payload.mkdir(exist_ok=True)
    home = payload / "jre"
    (home / "lib/server").mkdir(parents=True, exist_ok=True)
    assets = root / "jvm-jni/build/tests/android/assets/android-jre"
    libs = root / "jvm-jni/build/tests/android/jniLibs/arm64-v8a"
    for item in ["release", "conf", "lib/modules", "lib/tzdb.dat"]:
        if (assets / item).is_dir():
            shutil.copytree(assets / item, home / item, dirs_exist_ok=True)
        else:
            shutil.copyfile(assets / item, home / item)
    for name in ["jvm", "java", "verify", "net", "nio", "zip", "jimage"]:
        filename = "lib" + name + ".so"
        shutil.copyfile(
            libs / filename,
            home / "lib" / ("server/" if name == "jvm" else "") / filename,
        )
    for name in (["late-bridge.so"] if manifest.get("late_signal") else []) + [
        "host",
        "guest",
        "probe.so",
        "conformance.so",
        "system-load.so",
        "system-reject.so",
        "system-ordinary.so",
        "system-fixture.jar",
        "domain-probe.jar",
        "libgcc_s.so.1",
    ]:
        shutil.copyfile(build / name, payload / name)
    for name in [manifest["loader"], "libc.so.6", "libm.so.6"]:
        shutil.copyfile(runtime / name, payload / name)
    subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
    subprocess.run([*adb, "push", str(payload) + "/.", remote], check=True)
    env = {
        "GW_CONFORMANCE": remote + "/conformance.so",
        "GW_JAVA_HOME": remote + "/jre",
        "GW_JVM_LIB": remote + "/jre/lib/server/libjvm.so",
        "GW_JAVA_BOOT_LIB": remote + "/jre/lib",
        "GW_DOMAIN_JAR": remote + "/domain-probe.jar",
        "LD_LIBRARY_PATH": remote + "/jre/lib:" + remote + "/jre/lib/server",
    }
    native = [
        "/system/bin/linker64",
        remote + "/host",
        remote + "/" + manifest["loader"],
        remote,
        remote + "/guest",
        remote + "/probe.so",
    ]
else:
    home = args.jdk.resolve()
    env = {
        "GW_CONFORMANCE": str(build / "conformance.so"),
        "GW_JAVA_HOME": str(home),
        "GW_JVM_LIB": str(home / "lib/server/libjvm.so"),
        "GW_JAVA_BOOT_LIB": str(home / "lib"),
        "GW_DOMAIN_JAR": str(build / "domain-probe.jar"),
    }
    native = [
        str(build / "host"),
        str(runtime / manifest["loader"]),
        str(runtime) + ":" + str(build),
        str(build / "guest"),
        str(build / "probe.so"),
    ]


location = remote if android else str(build)
env.update(
    GW_SYSTEM_JAR=location + "/system-fixture.jar",
    GW_SYSTEM_IMAGE=location + "/system-load.so",
    GW_SYSTEM_REJECT=location + "/system-reject.so",
    GW_SYSTEM_ORDINARY=location + "/system-ordinary.so" if android else "",
)
if manifest.get("late_signal"):
    env["GW_LATE_BRIDGE"] = location + "/late-bridge.so"
results = []
for mode in ("worker", "domain"):
    settings = {
        **env,
        "GW_DOMAIN_MODE": mode,
        "GW_THREADS": "1",
        "GW_BENCH": "0",
    }
    command = (
        [
            *adb,
            "shell",
            "cd "
            + shlex.quote(remote)
            + " && /system/bin/timeout -s KILL 120 env "
            + shlex.join([k + "=" + v for k, v in settings.items()] + native),
        ]
        if android
        else native
    )
    completed = subprocess.run(
        command,
        env={**environment, **settings},
        cwd=build,
        capture_output=True,
        text=True,
        timeout=130,
        check=False,
    )
    (evidence / (mode + ".stdout")).write_text(completed.stdout)
    (evidence / (mode + ".stderr")).write_text(completed.stderr)
    output = completed.stdout + completed.stderr
    if manifest.get("late_signal") and "LATE_SIGNAL_INSTALL " not in output:
        raise SystemExit(f"Late signal installation failed: {evidence}")
    pressure = [
        json.loads(line.removeprefix("LATE_SIGNAL_PRESSURE "))
        for line in output.splitlines()
        if line.startswith("LATE_SIGNAL_PRESSURE ")
    ]
    signal_install = [
        json.loads(line.removeprefix("LATE_SIGNAL_INSTALL "))
        for line in output.splitlines()
        if line.startswith("LATE_SIGNAL_INSTALL ")
    ]
    signal_delivery = [
        json.loads(line.removeprefix("LATE_SIGNAL_DELIVERIES "))
        for line in output.splitlines()
        if line.startswith("LATE_SIGNAL_DELIVERIES ")
    ]
    if manifest.get("late_signal") and (
        len(pressure) != 3
        or any(
            row.get("result") != "PASS" or row.get("guest_overlaps", 0) < 1
            for row in pressure
        )
        or len(signal_install) != 1
        or signal_install[0].get("result") != "PASS"
        or len(signal_delivery) != 1
        or signal_delivery[0].get("sr", 0) < 1
    ):
        raise SystemExit(f"Late signal pressure failed: {evidence}")
    rows = [
        json.loads(line.removeprefix("SYSTEM_LOAD_RESULT "))
        for line in output.splitlines()
        if line.startswith("SYSTEM_LOAD_RESULT ")
    ]
    life = [
        json.loads(line.removeprefix("SYSTEM_LOAD_LIFECYCLE "))
        for line in output.splitlines()
        if line.startswith("SYSTEM_LOAD_LIFECYCLE ")
    ]
    if (
        completed.returncode
        or len(rows) != 1
        or rows[0]["result"] != "PASS"
        or rows[0]["cycles"] != 3
        or rows[0]["active_modules"]
        or rows[0]["live_closures"]
        or rows[0]["bionic_passthrough"] != android
        or len(life) != 1
        or not life[0]["vm_destroyed"]
        or (mode == "domain" and life[0]["domains_created"] < 2)
        or "WARNING in native method" in output
        or "handler modified" in output
    ):
        raise SystemExit(
            f"System.load {mode} failed: exit={completed.returncode}; {evidence}"
        )
    results.append(
        {
            "backend": mode,
            "contract": rows[0],
            "lifecycle": life[0],
            "command": command,
            "pressure": pressure,
            "signal_install": signal_install,
            "signal_delivery": signal_delivery,
        }
    )
    print(mode, rows[0], life[0], flush=True)
rejections = []
if manifest.get("late_signal"):
    for point in range(1, 4):
        settings = {
            **env,
            "GW_DOMAIN_MODE": "worker",
            "GW_THREADS": "1",
            "GW_BENCH": "0",
            "GW_LATE_FAIL_AFTER": str(point),
        }
        command = (
            [
                *adb,
                "shell",
                "cd "
                + shlex.quote(remote)
                + " && /system/bin/timeout -s KILL 120 env "
                + shlex.join(
                    [k + "=" + v for k, v in settings.items()] + native
                ),
            ]
            if android
            else native
        )
        rejected = subprocess.run(
            command,
            env={**environment, **settings},
            cwd=build,
            capture_output=True,
            text=True,
            timeout=130,
            check=False,
        )
        (evidence / f"rollback-{point}.stdout").write_text(rejected.stdout)
        (evidence / f"rollback-{point}.stderr").write_text(rejected.stderr)
        rollback = [
            json.loads(line.removeprefix("LATE_SIGNAL_ROLLBACK "))
            for line in rejected.stdout.splitlines()
            if line.startswith("LATE_SIGNAL_ROLLBACK ")
        ]
        finished = [
            json.loads(line.removeprefix("LATE_SIGNAL_REJECTION "))
            for line in rejected.stdout.splitlines()
            if line.startswith("LATE_SIGNAL_REJECTION ")
        ]
        if (
            rejected.returncode
            or len(rollback) != 1
            or rollback[0].get("result") != "PASS"
            or len(finished) != 1
            or not finished[0].get("vm_destroyed")
            or "handler modified" in rejected.stdout + rejected.stderr
        ):
            raise SystemExit(f"Late signal rollback {point} failed: {evidence}")
        rejections.append(
            {
                "after_binding": point,
                "rollback": rollback[0],
                "command": command,
            }
        )
(evidence / "summary.json").write_text(
    json.dumps(
        {
            "result": "PASS",
            "build": identity,
            "execution": "android-shell" if android else "desktop-linux",
            "results": results,
            "rejections": rejections,
            "production_ready": False,
            "late_signal": manifest.get("late_signal", False),
        },
        indent=2,
    )
    + "\n"
)
print(evidence)
