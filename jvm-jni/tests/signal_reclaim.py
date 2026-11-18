"""Exercise native recipient reclamation in a live HotSpot or ART process."""

import argparse
import json
import shlex
import subprocess
import uuid
from pathlib import Path

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--core-prefix", required=True, type=Path)
parser.add_argument("--jdk", required=True, type=Path)
parser.add_argument("--ndk", type=Path)
parser.add_argument("--d8", type=Path)
parser.add_argument("--serial")
args = parser.parse_args()
if any((args.serial, args.ndk, args.d8)) and not all(
    (args.serial, args.ndk, args.d8)
):
    parser.error("Android requires --serial, --ndk and --d8")

android = bool(args.serial)
prefix = args.core_prefix.resolve()
header = prefix / "include/glibc-worker/core"
library = next(prefix.glob("lib*/libglibc_worker_domains.a"), None)
if not library:
    parser.error("Install the core SDK into --core-prefix")
compiler = (
    args.ndk
    / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    if android
    else "cc"
)
out = project / "build/tests/signal-reclaim" / ("android" if android else "pc")
out.mkdir(parents=True, exist_ok=True)
binary = out / "libsignal_reclaim.so"
binary.unlink(missing_ok=True)
report = out / "summary.json"
report.unlink(missing_ok=True)


def run(command, **options):
    return subprocess.run(list(map(str, command)), check=True, **options)


run(
    [
        compiler,
        "-std=gnu17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fPIC",
        "-shared",
        "-Wl,-Bsymbolic-functions",
        "-Wl,-z,defs",
        "-I" + str(header),
        "-I" + str(header / "internal"),
        "-I" + str(args.jdk / "include"),
        "-I" + str(args.jdk / "include/linux"),
        project / "tests/native/signal_reclaim.c",
        library,
        library.with_name("libglibc_worker_core.a"),
        "-ldl",
        "-pthread",
        "-o",
        binary,
    ]
)
run(
    [
        args.jdk / "bin/javac",
        "--release",
        "8",
        "-d",
        out,
        project / "tests/java/compat/SignalReclaimSuite.java",
    ]
)
class_name = "compat.SignalReclaimSuite"
if android:
    jar = out / "signal-reclaim.jar"
    run(
        [
            args.d8,
            "--min-api",
            "26",
            "--output",
            jar,
            out / "compat/SignalReclaimSuite.class",
        ]
    )
    adb = ["adb", "-s", args.serial]
    remote = "/data/local/tmp/gw-signal-reclaim-" + uuid.uuid4().hex[:16]
    vm = "/apex/com.android.art/bin/dalvikvm64"
    run([*adb, "shell", "mkdir", remote])
    try:
        run([*adb, "push", jar, binary, remote])
        command = [
            "/system/bin/timeout",
            "25",
            vm,
            "-Xcheck:jni",
            "-cp",
            remote + "/" + jar.name,
            class_name,
            remote + "/" + binary.name,
        ]
        result = run(
            [*adb, "shell", shlex.join(command)],
            stdout=subprocess.PIPE,
            text=True,
            timeout=30,
        )
    finally:
        run([*adb, "shell", "rm", "-rf", remote])
else:
    result = run(
        [args.jdk / "bin/java", "-Xcheck:jni", "-cp", out, class_name, binary],
        stdout=subprocess.PIPE,
        text=True,
        timeout=25,
    )
summary = json.loads(result.stdout)
if summary != {"result": "PASS", "case": "live-vm-signal-reclaim"}:
    raise SystemExit("Missing VM reclamation result")
summary["vm"] = "ART" if android else "HotSpot"
report.write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary))
