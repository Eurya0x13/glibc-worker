#!/usr/bin/env python3
"""Copy four installed SDKs and link C/C++ consumers using only those prefixes."""

import argparse
from pathlib import Path
import shutil
import subprocess
from zipfile import ZipFile

PROJECT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=("arm64-v8a", "x86_64"), required=True)
parser.add_argument("--ndk", type=Path, required=True)
for name in ("core", "dlopen", "android-jni", "jvm-jni"):
    parser.add_argument("--" + name + "-prefix", type=Path, required=True)
parser.add_argument("--output", type=Path, default=PROJECT / "build/sdk-check")
args = parser.parse_args()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
prefixes = []
for name in ("core", "dlopen", "android_jni", "jvm_jni"):
    destination = output / "prefixes" / name
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(getattr(args, name + "_prefix").resolve(), destination)
    prefixes.append(str(destination))

jvm_prefix = Path(prefixes[3])
assert not (
    jvm_prefix / "include/glibc-worker/jvm-jni/jre17_adapter.h"
).exists()
assert not (
    jvm_prefix / "include/glibc-worker/jvm-jni/signal_profile.h"
).exists()
assert not (jvm_prefix / "share/glibc-worker/jvm-jni/signal-profile.h").exists()
header = Path(prefixes[2]) / "include/glibc-worker/android-jni/host_bridge.h"
assert "GW_HOST_LEGACY_JRE17" not in header.read_text()
symbols = subprocess.check_output(
    ["readelf", "-Ws", str(jvm_prefix / "lib/libgw_jvm_jni.so")], text=True
)
assert "gw_jre17_adapter_" not in symbols
assert "domain_late_signal_" not in symbols
with ZipFile(
    jvm_prefix / "share/glibc-worker/jvm-jni/jvm-jni-helpers.jar"
) as jar:
    assert "dev/glibc/worker/Jre17Adapter.class" not in jar.namelist()
commands = [
    [
        "cmake",
        "-S",
        str(PROJECT / "tests/sdk-consumer"),
        "-B",
        str(output / "consumer-build"),
        "-DCMAKE_TOOLCHAIN_FILE="
        + str(args.ndk.resolve() / "build/cmake/android.toolchain.cmake"),
        "-DANDROID_ABI=" + args.abi,
        "-DANDROID_PLATFORM=android-29",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH",
        "-DCMAKE_PREFIX_PATH=" + ";".join(prefixes),
    ],
    ["cmake", "--build", str(output / "consumer-build"), "--parallel"],
]
with (output / "build.log").open("w") as log:
    for command in commands:
        subprocess.run(
            command, stdout=log, stderr=subprocess.STDOUT, check=True
        )
for consumer in ("c_consumer", "cxx_consumer"):
    binary = output / "consumer-build" / consumer
    dependencies = subprocess.check_output(
        ["readelf", "-dW", binary], text=True
    )
    assert (
        "libgw_" not in dependencies
        and "libglibc_bridge.so" not in dependencies
    )
    symbols = subprocess.check_output(
        ["readelf", "--dyn-syms", "-W", binary], text=True
    )
    exported = {
        fields[7]
        for line in symbols.splitlines()
        if len(fields := line.split()) == 8 and fields[6] != "UND"
    }
    assert {"sigaction", "sigaction64", "signal"} <= exported
print("Relocated C/C++ static SDK consumers: PASS")
