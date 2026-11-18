"""Package the pinned Android HotSpot/domain fixture under application UID."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

source = Path(__file__).resolve().parent
project = source.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--native", type=Path, required=True)
parser.add_argument("--jre-assets", type=Path, required=True)
parser.add_argument("--jre-libs", type=Path, required=True)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--gradle", type=Path, required=True)
parser.add_argument("--sdk", type=Path, required=True)
parser.add_argument("--serial")
parser.add_argument("--build-only", action="store_true")
parser.add_argument(
    "--jni-domain",
    action="store_true",
    help="Validate the production JNI transport fixture",
)
parser.add_argument("--system-load", action="store_true")
parser.add_argument("--inline-vm", action="store_true")
args = parser.parse_args()
if not args.build_only and not args.serial:
    parser.error("--serial is required unless --build-only is selected")
if args.system_load and args.jni_domain:
    parser.error("Choose one fixture")
native, runtime = args.native.resolve(), args.runtime.resolve()
manifest = json.loads((native / "manifest.json").read_text())
if manifest["abi"] != "arm64-v8a" or manifest["host_libc"] != "bionic":
    parser.error("This APK fixture requires a built ARM64 bionic Host")
if args.inline_vm and not (args.system_load and manifest.get("late_signal")):
    parser.error("Inline VM requires the late System.load build")
output = project / "build/experiments/same-thread"
libraries = output / "android-jniLibs/arm64-v8a"
libraries.mkdir(parents=True, exist_ok=True)
# Remove only this packager's optional payloads; avoid silently bundling a prior fixture.
for name in (
    "libdomain_conformance.so",
    "libdomain_late_bridge.so",
    "libdomain_inline_host.so",
    "libdomain_system_load.so",
    "libdomain_system_reject.so",
    "libdomain_system_ordinary.so",
):
    (libraries / name).unlink(missing_ok=True)
payload = {
    native / "host": "libdomain_host.so",
    native / "guest": "libdomain_guest.so",
    native / "probe.so": "libdomain_probe.so",
    native / "libgcc_s.so.1": "libdomain_gcc.so",
    runtime / "ld-linux-aarch64.so.1": "libdomain_loader.so",
    runtime / "libc.so.6": "libdomain_libc.so",
    runtime / "libm.so.6": "libdomain_libm.so",
}
if args.jni_domain:
    payload[native / "conformance.so"] = "libdomain_conformance.so"
if args.system_load:
    if not manifest.get("system_load"):
        parser.error("System.load native fixture required")
    for item in ("load", "reject", "ordinary"):
        payload[native / ("system-" + item + ".so")] = (
            "libdomain_system_" + item + ".so"
        )
if manifest.get("late_signal"):
    payload[native / "late-bridge.so"] = "libdomain_late_bridge.so"
    payload[native / "inline-host.so"] = "libdomain_inline_host.so"
for path, name in payload.items():
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if manifest["sha256"].get(str(path)) != digest:
        parser.error(f"Payload differs from the recorded native build: {path}")
    shutil.copyfile(path, libraries / name)
profile = manifest["jre_artifact"]
for key in ("libjvm", "libjava"):
    path = args.jre_libs / Path(profile[key]["path"]).name
    if hashlib.sha256(path.read_bytes()).hexdigest() != profile[key]["sha256"]:
        parser.error(f"JRE artifact mismatch: {path}")
for name in ("jvm", "java", "verify", "net", "nio", "zip", "jimage"):
    filename = "lib" + name + ".so"
    shutil.copyfile(args.jre_libs / filename, libraries / filename)
assets = output / "android-assets"
(assets / "jre/lib/server").mkdir(parents=True, exist_ok=True)
for item in ("release", "conf", "lib/modules", "lib/tzdb.dat"):
    src, dst = args.jre_assets / item, assets / "jre" / item
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copyfile(src, dst)
jar = native / "domain-probe.jar"
if hashlib.sha256(jar.read_bytes()).hexdigest() != manifest["sha256"][str(jar)]:
    parser.error("Java probe differs from recorded build")
shutil.copyfile(jar, assets / jar.name)
(assets / "system-fixture.jar").unlink(missing_ok=True)
if args.system_load:
    isolated = native / "system-fixture.jar"
    if (
        hashlib.sha256(isolated.read_bytes()).hexdigest()
        != manifest["sha256"][str(isolated)]
    ):
        parser.error("Isolated ClassLoader fixture changed")
    shutil.copyfile(isolated, assets / isolated.name)
environment = {**os.environ, "ANDROID_HOME": str(args.sdk.resolve())}
subprocess.run(
    [
        str(args.gradle.resolve()),
        "-p",
        str(source / "android"),
        "--no-daemon",
        "--console=plain",
        "assembleDebug",
    ],
    env=environment,
    check=True,
)
apk = output / "android-app/outputs/apk/debug/app-debug.apk"
if args.build_only:
    print(apk)
    raise SystemExit(0)
evidence = output / "android-app-evidence"
evidence.mkdir(parents=True, exist_ok=True)
(evidence / "summary.json").unlink(missing_ok=True)
(evidence / "native-manifest.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
adb = ["adb", "-s", args.serial]
package = "dev.glibc.worker.domains.hotspot"
# A previous run may have left a complete report. Remove it before launch so
# polling cannot mistake that report for the result of the new executable.
subprocess.run([*adb, "shell", "am", "force-stop", package], check=True)
installed = subprocess.run(
    [*adb, "shell", "pm", "list", "packages", package],
    capture_output=True,
    text=True,
    check=True,
)
if "package:" + package in installed.stdout.splitlines():
    subprocess.run(
        [
            *adb,
            "shell",
            "run-as",
            package,
            "rm",
            "-f",
            "files/native-report.txt",
        ],
        check=True,
    )
subprocess.run(
    ["android", "install", "--device=" + args.serial, "--apks=" + str(apk)],
    check=True,
)
subprocess.run(
    [*adb, "shell", "run-as", package, "mkdir", "-p", "files"], check=True
)
subprocess.run(
    [
        *adb,
        "shell",
        "run-as",
        package,
        "touch",
        "files/inline-vm.flag",
    ]
    if args.inline_vm
    else [*adb, "shell", "run-as", package, "rm", "-f", "files/inline-vm.flag"],
    check=True,
)
subprocess.run(
    [*adb, "shell", "am", "start", "-n", package + "/.MainActivity"], check=True
)
deadline = time.monotonic() + 180
report = ""
while time.monotonic() < deadline:
    read = subprocess.run(
        [*adb, "shell", "run-as", package, "cat", "files/native-report.txt"],
        capture_output=True,
        text=True,
        check=False,
    )
    report = read.stdout
    if any(
        marker in report
        for marker in (
            "APP_DOMAIN_EXIT=",
            "APP_DOMAIN_RESULT=",
            "JNI_DOMAIN_FAILURE",
            "LATE_SIGNAL_REJECT",
        )
    ):
        break
    time.sleep(1)
(evidence / "native-report.txt").write_text(report)
if args.system_load:
    outer_chain = (
        report.count("LATE_OUTER_CHAIN installed=1 forwards_existing_art=1")
        == 1
    )
    pressure = [
        json.loads(line.removeprefix("LATE_SIGNAL_PRESSURE "))
        for line in report.splitlines()
        if line.startswith("LATE_SIGNAL_PRESSURE ")
    ]
    signal_install = [
        json.loads(line.removeprefix("LATE_SIGNAL_INSTALL "))
        for line in report.splitlines()
        if line.startswith("LATE_SIGNAL_INSTALL ")
    ]
    signal_delivery = [
        json.loads(line.removeprefix("LATE_SIGNAL_DELIVERIES "))
        for line in report.splitlines()
        if line.startswith("LATE_SIGNAL_DELIVERIES ")
    ]
    art_reclaim = [
        json.loads(line.removeprefix("ART_SIGNAL_RECLAIM "))
        for line in report.splitlines()
        if line.startswith("ART_SIGNAL_RECLAIM ")
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
        or signal_delivery[0].get("sr_guest", 0) < 3
        or (args.inline_vm and not signal_install[0].get("art_chain"))
        or (args.inline_vm and not outer_chain)
        or (
            args.inline_vm
            and (
                len(art_reclaim) != 1
                or art_reclaim[0].get("result") != "PASS"
                or art_reclaim[0].get("reclaimed", 0) < 1
                or not art_reclaim[0].get("ordinary_mask")
                or not art_reclaim[0].get("old_callback_returned")
                or not art_reclaim[0].get("replacement_delivered")
            )
        )
    ):
        raise SystemExit(f"Late application signal coverage failed: {evidence}")
    rows = [
        json.loads(line.removeprefix("SYSTEM_LOAD_RESULT "))
        for line in report.splitlines()
        if line.startswith("SYSTEM_LOAD_RESULT ")
    ]
    life = [
        json.loads(line.removeprefix("SYSTEM_LOAD_LIFECYCLE "))
        for line in report.splitlines()
        if line.startswith("SYSTEM_LOAD_LIFECYCLE ")
    ]
    uids = [
        int(line.removeprefix("APP_DOMAIN_SUPERVISOR_UID="))
        for line in report.splitlines()
        if line.startswith("APP_DOMAIN_SUPERVISOR_UID=")
    ]
    if (
        len(rows) != 1
        or rows[0]["result"] != "PASS"
        or rows[0]["cycles"] != 3
        or not rows[0]["bionic_passthrough"]
        or rows[0]["active_modules"]
        or rows[0]["live_closures"]
        or len(life) != 1
        or not life[0]["vm_destroyed"]
        or life[0]["domains_created"] < 2
        or len(uids) != 1
        or uids[0] < 10000
        or "APP_DOMAIN_EXIT=0" not in report
        or "WARNING in native method" in report
        or "handler modified" in report
        or (args.inline_vm and "APP_DOMAIN_INLINE=1" not in report)
        or (
            manifest.get("late_signal")
            and report.count("LATE_SIGNAL_PRESSURE ") != 3
        )
    ):
        raise SystemExit(f"System.load application fixture failed: {evidence}")
    result = {
        "result": "PASS",
        "execution": "android-art-hotspot-same-process"
        if args.inline_vm
        else "android-application-exec-child",
        "late_signal": manifest.get("late_signal", False),
        "outer_forwarding_chain": outer_chain,
        "pressure": pressure,
        "signal_install": signal_install,
        "signal_delivery": signal_delivery,
        "art_reclaim": art_reclaim,
        "supervisor_uid": uids[0],
        "contract": rows[0],
        "lifecycle": life[0],
        "production_ready": False,
        "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
    }
    (evidence / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    raise SystemExit(0)
if args.jni_domain:
    coverage = [
        json.loads(line.removeprefix("JNI_DOMAIN_COVERAGE "))
        for line in report.splitlines()
        if line.startswith("JNI_DOMAIN_COVERAGE ")
    ]
    guests = [
        json.loads(line.removeprefix("JNI_DOMAIN_GUEST_THREADS "))
        for line in report.splitlines()
        if line.startswith("JNI_DOMAIN_GUEST_THREADS ")
    ]
    contracts = [
        json.loads(line.removeprefix("JNI_DOMAIN_CONFORMANCE "))
        for line in report.splitlines()
        if line.startswith("JNI_DOMAIN_CONFORMANCE ")
    ]
    coop = [
        json.loads(line.removeprefix("JNI_DOMAIN_VM_COOP "))
        for line in report.splitlines()
        if line.startswith("JNI_DOMAIN_VM_COOP ")
    ]
    rows = [
        json.loads(line.removeprefix("JNI_DOMAIN_RESULT "))
        for line in report.splitlines()
        if line.startswith("JNI_DOMAIN_RESULT ")
    ]
    uids = [
        int(line.removeprefix("APP_DOMAIN_SUPERVISOR_UID="))
        for line in report.splitlines()
        if line.startswith("APP_DOMAIN_SUPERVISOR_UID=")
    ]
    if (
        len(rows) != 1
        or rows[0]
        != {
            "result": "PASS",
            "backend": "domain",
            "threads": 8,
            "benchmark": 0,
            "errors": 0,
        }
        or len(uids) != 1
        or uids[0] < 10000
        or len(coverage) != 1
        or coverage[0]["apis"] != 234
        or coverage[0]["contexts"] < 48
        or contracts
        != [
            {
                "groups": 10,
                "java_threads": 96,
                "concurrent_threads": 32,
                "reentry_depth": 32,
                "monitor_increments": 960,
            }
        ]
        or len(coop) != 1
        or coop[0]["samples"] != 20
        or coop[0]["overlaps"] <= 0
        or coverage[0]["domains_created"] < 106
        or coverage[0]["domains_created"] != coverage[0]["domains_finished"]
        or coverage[0]["sr_deliveries"] <= 0
        or guests != [{"created": 32, "passed": 32}]
        or "APP_DOMAIN_EXIT=0" not in report
        or "WARNING in native method" in report
        or "handler modified" in report
    ):
        raise SystemExit(
            f"Production JNI application fixture failed: {evidence}"
        )
    result = {
        **rows[0],
        "execution": "android-application-exec-child",
        "supervisor_uid": uids[0],
        "coverage": coverage[0],
        "guest_threads": guests[0],
        "conformance": contracts[0],
        "vm_cooperation": coop[0],
        "production_jni_paths_validated": True,
        "production_ready": False,
        "performance_measured": False,
        "apk_sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
    }
    (evidence / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    raise SystemExit(0)
rows = [
    json.loads(line) for line in report.splitlines() if line.startswith("{")
]
vm_rows = [
    json.loads(line.removeprefix("HOTSPOT_RESULT "))
    for line in report.splitlines()
    if line.startswith("HOTSPOT_RESULT ")
]
signal_rows = [
    json.loads(line.removeprefix("SIGNAL_ROUTER "))
    for line in report.splitlines()
    if line.startswith("SIGNAL_ROUTER ")
]
if (
    len(vm_rows) != 1
    or vm_rows[0]["result"] != "PASS"
    or len(rows) != 1
    or rows[0]["result"] != "PASS"
    or rows[0]["failures"]
    or rows[0]["uid"] < 10000
    or "APP_DOMAIN_EXIT=0" not in report
    or "DomainProbe::callback" not in report
    or vm_rows[0]["forwarded_faults"] <= 0
    or len(signal_rows) != 1
    or signal_rows[0]["restore_error"] != 0
    or "handler modified" in report
):
    raise SystemExit(f"Application probe failed; retained report: {evidence}")
result = rows[0]
result.update(
    execution="android-application-exec-child",
    hotspot_fixture=vm_rows[0],
    signal_router=signal_rows[0],
    full_guest_jni_bridge_validated=False,
    performance_measured=False,
    jit_callback_compiled=True,
    art_jni_validated=False,
    production_ready=False,
    apk_sha256=hashlib.sha256(apk.read_bytes()).hexdigest(),
)
(evidence / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
