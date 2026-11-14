"""Optional original GNU JNI corpus and its explicit Guest dependency closure."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from target import ABI, TARGET

ROOT = Path(__file__).resolve().parents[3]
ENABLED = os.environ.get("GW_NATIVE_JARS", "0")
if ENABLED not in ("0", "1"):
    raise SystemExit("GW_NATIVE_JARS must be 0 or 1")
generated = ROOT / "jvm-jni/build/native/generated"
generated.mkdir(parents=True, exist_ok=True)
header = generated / "compat-preload.inc"
native = ROOT / "jvm-jni/build/tests/android/jniLibs" / ABI
if ENABLED == "0":
    header.write_text("/* Optional GNU JNI corpus disabled. */\n")
    if native.exists():
        for path in native.glob("libcompat_*.so"):
            path.unlink()
    (ROOT / "jvm-jni/build/native/native-jars.json").write_text(
        json.dumps({"enabled": False, "abi": ABI}, indent=2) + "\n"
    )
    shutil.rmtree(
        ROOT / "jvm-jni/build/tests/android/assets/runtime/native-jars",
        ignore_errors=True,
    )
    raise SystemExit(0)

pins = json.loads((ROOT / "jvm-jni/tests/config/native-jars.json").read_text())


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url, expected):
    path = ROOT / "jvm-jni/vendor" / url.rsplit("/", 1)[1]
    path.parent.mkdir(exist_ok=True)
    if not path.exists():
        temporary = path.with_name(path.name + ".tmp")
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(path)
    if sha256(path.read_bytes()) != expected:
        raise SystemExit(
            "Original JNI artifact checksum mismatch: " + path.name
        )
    return path


jars = [(pin, fetch(pin["url"], pin["jar_sha256"])) for pin in pins]
for pin in pins:
    fetch(pin["sources_url"], pin["sources_sha256"])
if "--fetch-only" in sys.argv:
    raise SystemExit(0)

assets = ROOT / "jvm-jni/build/tests/android/assets/android-jre"
native.mkdir(parents=True, exist_ok=True)
properties = []
catalog = []
layout = ["F\tnative-jars.properties"]
manifest = {"enabled": True, "abi": ABI, "jars": [], "dependencies": []}
needed_by = {}


def dynamic_info(path):
    data = path.read_bytes()
    machine = 183 if ABI == "arm64-v8a" else 62

    if (
        data[:6] != b"\x7fELF\x02\x01"
        or int.from_bytes(data[18:20], "little") != machine
    ):
        raise SystemExit(
            "Guest ELF class/byte order/machine mismatch: " + str(path)
        )

    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
    needed_by[path.name] = needed
    return re.findall(r"\(SONAME\).*\[(.*?)\]", dynamic), needed


for pin, jar in jars:
    target = pin["targets"][ABI]
    with zipfile.ZipFile(jar) as archive:
        image = archive.read(target["resource"])
    if sha256(image) != target["sha256"] or len(image) != target["bytes"]:
        raise SystemExit("GNU resource checksum mismatch: " + pin["id"])
    installed = "libcompat_" + pin["id"] + ".so"
    (native / installed).write_bytes(image)
    _, needed = dynamic_info(native / installed)
    shutil.copyfile(jar, assets / jar.name)
    catalog.append(target["sha256"] + "\t" + str(len(image)) + "\t" + installed)
    layout.append("F\t" + jar.name)
    prefix = pin["id"] + "."
    for key, value in {
        "jar": jar.name,
        "jar.sha256": pin["jar_sha256"],
        "resource": target["resource"],
        "elf.sha256": target["sha256"],
        "installed": installed,
        "workload": {"lz4": "Lz4", "snappy": "Snappy", "rocksdb": "Rocks"}[
            pin["id"]
        ]
        + "Workload",
    }.items():
        properties.append(prefix + key + "=" + value)
    manifest["jars"].append(
        {**pin, "installed": installed, "needed": needed, "modified": False}
    )

# Android installs lib*.so, but DT_NEEDED uses GNU SONAMEs. Preload absolute
# installed paths with the existing GNU loader; SONAME lookup then works normally.
runtime = ROOT / "core/build/glibc-runtime" / ABI
dependencies = [
    ("libcompat_math.so", runtime / "libm.so.6", None, "libm.so.6"),
    ("libcompat_rt.so", runtime / "librt.so.1", None, "librt.so.1"),
    ("libcompat_dl.so", runtime / "libdl.so.2", None, "libdl.so.2"),
]
runtime_pins = json.loads(
    (ROOT / "jvm-jni/tests/config/native-jars-runtime.json").read_text()
)[ABI]
for soname, installed in [
    ("libgcc_s.so.1", "libcompat_gcc.so"),
    ("libstdc++.so.6", "libcompat_stdcpp.so"),
]:
    discovered = Path(
        subprocess.check_output(
            [TARGET["guest_cc"], "-print-file-name=" + soname], text=True
        ).strip()
    ).resolve(strict=True)
    dependencies.append((installed, discovered, runtime_pins[soname], soname))
preloads = []
provided = {"libc.so.6", "libpthread.so.0", TARGET["loader"]}
for installed, source, expected, expected_soname in dependencies:
    data = source.read_bytes()
    digest = sha256(data)
    if expected and digest != expected:
        raise SystemExit(
            "Guest C++ runtime differs from reviewed pin: " + str(source)
        )
    soname, needed = dynamic_info(source)
    if soname != [expected_soname]:
        raise SystemExit("Guest dependency SONAME mismatch: " + str(source))
    provided.add(expected_soname)
    (native / installed).write_bytes(data)
    preloads.append('"' + installed + '",')
    manifest["dependencies"].append(
        {
            "installed": installed,
            "source": str(source),
            "sha256": digest,
            "soname": soname[0],
            "needed": needed,
            "modified": False,
        }
    )
for name, needed in needed_by.items():
    if set(needed) - provided:
        raise SystemExit(
            "Guest dependency closure incomplete for "
            + name
            + ": "
            + ", ".join(sorted(set(needed) - provided))
        )
manifest["dependency_closure_checked"] = True
header.write_text("\n".join(preloads) + "\n")
(assets / "native-jars.properties").write_text("\n".join(properties) + "\n")
with (assets / "guest-images.tsv").open("a") as out:
    out.write("\n".join(catalog) + "\n")
with (assets / "layout.tsv").open("a") as out:
    out.write("\n".join(layout) + "\n")
licenses = ROOT / "jvm-jni/tests/third_party/native-jars"
shutil.copytree(
    licenses,
    ROOT / "jvm-jni/build/tests/android/assets/runtime/native-jars",
    dirs_exist_ok=True,
)
(ROOT / "jvm-jni/build/native/native-jars.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
print(
    "Original GNU JNI corpus prepared:",
    ABI,
    len(jars),
    "JARs; explicit SONAME preloads",
)
