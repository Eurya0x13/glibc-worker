"""Build and run the HotSpot same-thread signal and lifecycle fixture."""

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
project = source.parents[1]
root = project.parent
core_probe = root / "core/tests/same-thread"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--jdk", type=Path, required=True)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--jre-assets", type=Path)
parser.add_argument("--jre-libs", type=Path)
parser.add_argument("--jre-profile", type=Path)
parser.add_argument("--serial")
parser.add_argument("--build-only", action="store_true")
args = parser.parse_args()
android = bool(args.ndk)
if android and not all((args.jre_assets, args.jre_libs, args.jre_profile)):
    parser.error("Android requires explicit JRE assets, libraries and profile")
if args.serial and not android:
    parser.error("--serial requires --ndk")
output = (
    project / "build/tests/same-thread" / ("android" if android else "linux")
)
output.mkdir(parents=True, exist_ok=True)
(output / "summary.json").unlink(missing_ok=True)
classes = output / "classes"
classes.mkdir(exist_ok=True)
subprocess.run(
    [
        str(args.jdk / "bin/javac"),
        "--release",
        "8",
        "-d",
        str(classes),
        str(source / "DomainProbe.java"),
    ],
    check=True,
)
jar = output / "domain-probe.jar"
subprocess.run(
    [str(args.jdk / "bin/jar"), "cf", str(jar), "-C", str(classes), "."],
    check=True,
)
command = [
    "python3",
    str(core_probe / "run.py"),
    "--adopt",
    "--signals",
    "--credential-guard",
    "--build-only",
    "--runtime",
    str(args.runtime.resolve()),
    "--output",
    str(output),
    "--host-source",
    str(source / "hotspot.c"),
    "--host-source",
    str(source / "vm-start.c"),
    "--host-source",
    str(root / "core/src/domain/nonlocal.c"),
    "--host-source",
    str(root / "core/src/domain/signal-relay.c"),
    "--host-source",
    str(root / "core/src/domain/signal-router.c"),
    "--host-source",
    str(root / "core/src/domain/signal-quiescence.c"),
    "--host-source",
    str(root / "core/src/domain/signal-stack.S"),
    "--host-source",
    str(root / "core/src/domain/signal-interpose.c"),
]
flags = [
    "-DGW_DOMAIN_HOOKS",
    "-DGW_DOMAIN_CHAIN",
    "-Wl,--export-dynamic",
    "-ldl",
    "-I" + str(root / "core/include"),
    "-I" + str(args.jdk.resolve() / "include"),
    "-I" + str(args.jdk.resolve() / "include/linux"),
]
profile = None
if android:
    profile = json.loads((args.jre_assets / "jre-artifact.json").read_text())
    header = args.jre_profile.read_text()
    if profile["major"] != 17 or profile["abi"] != "arm64-v8a":
        parser.error(
            "This startup fixture currently supports the pinned Android JRE 17 only"
        )
    for key, macro in (
        ("libjvm", "JRE_LIBJVM_SHA256"),
        ("libjava", "JRE_LIBJAVA_SHA256"),
    ):
        path = args.jre_libs / Path(profile[key]["path"]).name
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if (
            digest != profile[key]["sha256"]
            or f'#define {macro} "{digest}"' not in header
        ):
            parser.error(f"Artifact/profile mismatch: {path}")
    if not re.search(r"#define JRE_USE_JLI 0\b", header):
        parser.error(
            "This fixture requires the existing CreateJavaVM startup profile"
        )
    (output / "jre-layout.h").write_text(header)
    harness = project / "tests/native/harness"
    command += [
        "--abi",
        "arm64-v8a",
        "--ndk",
        str(args.ndk.resolve()),
        "--host-source",
        str(harness / "jre_layout_spike.c"),
        "--host-source",
        str(harness / "jre_exec_compat.c"),
    ]
    flags += ["-I" + str(output), "-I" + str(harness)]
for flag in flags:
    command += ["--host-flag=" + flag]
subprocess.run(command, check=True)
manifest = json.loads((output / "manifest.json").read_text())
for path in (
    source / "DomainProbe.java",
    source / "hotspot.c",
    root / "core/src/domain/signal-router.h",
    Path(__file__),
    jar,
):
    manifest["sha256"][str(path)] = hashlib.sha256(
        path.read_bytes()
    ).hexdigest()
if profile:
    manifest["jre_artifact"] = profile
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
if args.build_only:
    print(output)
    raise SystemExit(0)

runtime = args.runtime.resolve()
if android:
    if not args.serial:
        parser.error("Android execution requires --serial or --build-only")
    adb = ["adb", "-s", args.serial]
    identity = hashlib.sha256(
        (output / "manifest.json").read_bytes()
    ).hexdigest()[:16]
    remote = "/data/local/tmp/gw-hotspot-domain-" + identity
    payload = output / "payload"
    payload.mkdir(exist_ok=True)
    home = payload / "jre"
    (home / "lib/server").mkdir(parents=True, exist_ok=True)
    for item in ("release", "conf", "lib/modules", "lib/tzdb.dat"):
        src, dst = args.jre_assets / item, home / item
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copyfile(src, dst)
    for name in ("jvm", "java", "verify", "net", "nio", "zip", "jimage"):
        filename = "lib" + name + ".so"
        shutil.copyfile(
            args.jre_libs / filename,
            home / "lib" / ("server/" if name == "jvm" else "") / filename,
        )
    for name in (
        "host",
        "guest",
        "probe.so",
        "libgcc_s.so.1",
        "domain-probe.jar",
    ):
        shutil.copyfile(output / name, payload / name)
    for name in ("ld-linux-aarch64.so.1", "libc.so.6", "libm.so.6"):
        shutil.copyfile(runtime / name, payload / name)
    subprocess.run([*adb, "shell", "mkdir", "-p", remote], check=True)
    subprocess.run([*adb, "push", str(payload) + "/.", remote], check=True)
    environment = {
        "GW_JAVA_HOME": remote + "/jre",
        "GW_JVM_LIB": remote + "/jre/lib/server/libjvm.so",
        "GW_JAVA_BOOT_LIB": remote + "/jre/lib",
        "GW_DOMAIN_JAR": remote + "/domain-probe.jar",
        "LD_LIBRARY_PATH": remote + "/jre/lib:" + remote + "/jre/lib/server",
    }
    native = [
        "/system/bin/linker64",
        remote + "/host",
        remote + "/ld-linux-aarch64.so.1",
        remote,
        remote + "/guest",
        remote + "/probe.so",
    ]
    command = [
        *adb,
        "shell",
        "cd "
        + shlex.quote(remote)
        + " && /system/bin/timeout -s KILL 120 env "
        + shlex.join([k + "=" + v for k, v in environment.items()] + native),
    ]
    environment = os.environ.copy()
    manifest["device"] = subprocess.check_output(
        [*adb, "shell", "getprop", "ro.build.fingerprint"], text=True
    ).strip()
else:
    home = args.jdk.resolve()
    environment = {
        **os.environ,
        "GW_JAVA_HOME": str(home),
        "GW_JVM_LIB": str(home / "lib/server/libjvm.so"),
        "GW_JAVA_BOOT_LIB": str(home / "lib"),
        "GW_DOMAIN_JAR": str(jar),
    }
    command = [
        str(output / "host"),
        str(runtime / "ld-linux-x86-64.so.2"),
        str(runtime) + ":" + str(output),
        str(output / "guest"),
        str(output / "probe.so"),
    ]
manifest["run_command"] = command
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
try:
    result = subprocess.run(
        command,
        check=False,
        env=environment,
        capture_output=True,
        text=True,
        timeout=135,
        cwd=output,
    )
except subprocess.TimeoutExpired as error:
    (output / "stdout.log").write_bytes(error.stdout or b"")
    (output / "stderr.log").write_bytes(error.stderr or b"")
    raise SystemExit(f"HotSpot fixture timed out: {output}") from error
(output / "stdout.log").write_text(result.stdout)
(output / "stderr.log").write_text(result.stderr)
for line in result.stdout.splitlines():
    if line.startswith(("HOTSPOT_", "{")):
        print(line)
print(result.stderr)
rows = [
    json.loads(line)
    for line in result.stdout.splitlines()
    if line.startswith("{")
]
vm_rows = [
    json.loads(line.removeprefix("HOTSPOT_RESULT "))
    for line in result.stdout.splitlines()
    if line.startswith("HOTSPOT_RESULT ")
]
signal_rows = [
    json.loads(line.removeprefix("SIGNAL_ROUTER "))
    for line in result.stdout.splitlines()
    if line.startswith("SIGNAL_ROUTER ")
]
if (
    result.returncode
    or len(rows) != 1
    or len(vm_rows) != 1
    or rows[0]["result"] != "PASS"
    or vm_rows[0]["result"] != "PASS"
    or "DomainProbe::callback" not in result.stdout
    or vm_rows[0]["forwarded_faults"] <= 0
    or len(signal_rows) != 1
    or signal_rows[0]["restore_error"] != 0
    or "handler modified" in result.stdout + result.stderr
):
    raise SystemExit(
        f"HotSpot same-thread fixture failed ({result.returncode}); logs: {output}"
    )
summary = {
    "native": rows[0],
    "hotspot": vm_rows[0],
    "signal_router": signal_rows[0],
    "execution": "android-shell" if android else "desktop-linux",
    "full_guest_jni_bridge_validated": False,
    "production_ready": False,
    "performance_measured": False,
    "jit_callback_compiled": True,
}
(output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
