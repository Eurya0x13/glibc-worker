"""Pin and package the Pojav Android/bionic JRE, not a desktop Linux JRE."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
from target import ABI, TARGET

root = Path(__file__).resolve().parents[3]
archive = root / "jvm-jni/vendor" / TARGET["jre_archive"]
sha = TARGET["jre_sha256"]
url = (
    "https://github.com/PojavLauncherTeam/android-openjdk-build-multiarch/releases/download/jre17-ec28559/"
    + archive.name
)
archive.parent.mkdir(exist_ok=True)
if not archive.exists():
    urllib.request.urlretrieve(url, str(archive) + ".tmp")
    Path(str(archive) + ".tmp").rename(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != sha:
    raise SystemExit("Android JRE checksum mismatch")
jre = root / "jvm-jni/vendor" / ("android-jre17-" + ABI)
marker = jre / ".source-sha256"
if not marker.exists() or marker.read_text().strip() != sha:
    if jre.exists():
        shutil.rmtree(jre)
    jre.mkdir()
    with tarfile.open(archive) as t:
        t.extractall(jre, filter="data")
    marker.write_text(sha + "\n")
assets = root / "jvm-jni/build/tests/android/assets/android-jre"
if assets.exists():
    shutil.rmtree(assets)
assets.mkdir(parents=True)
native = root / "jvm-jni/build/tests/android/jniLibs" / ABI
native.mkdir(parents=True, exist_ok=True)
# Version-bound startup-layout adapter. The original bytes stay unchanged.
# This private cache is writable .bss, not a stable JVM embedding API.
jvm = jre / "lib/server/libjvm.so"
jvm_sha = hashlib.sha256(jvm.read_bytes()).hexdigest()
if jvm_sha != TARGET["jvm_sha256"]:
    raise SystemExit("Unsupported libjvm for private path-cache adapter")
symbols = subprocess.check_output(["readelf", "-Ws", str(jvm)], text=True)
matches = [
    line.split()
    for line in symbols.splitlines()
    if line.endswith(" _ZL14saved_jvm_path")
]
if (
    len(matches) != 1
    or int(matches[0][2]) != 4096
    or matches[0][3:5] != ["OBJECT", "LOCAL"]
):
    raise SystemExit("Private JVM path cache symbol changed")
offset = int(matches[0][1], 16)
(root / "jvm-jni/build/native/jre-layout.h").write_text(
    "#define JRE_PATH_CACHE_OFFSET 0x%xUL\n#define JRE_PATH_CACHE_SIZE 4096\n"
    '#define JRE_LIBJVM_SHA256 "%s"\n' % (offset, jvm_sha)
)
libjava_sha = hashlib.sha256((jre / "lib/libjava.so").read_bytes()).hexdigest()
if libjava_sha != TARGET["libjava_sha256"]:
    raise SystemExit("Unsupported libjava for fixed JRE adapter")
profile = "pojav-jre17-ec28559-" + ABI
(root / "jvm-jni/build/native/jre-adapter-profile.h").write_text(
    '#define GW_JVM_PROFILE_NAME "%s"\n'
    '#define GW_JVM_LIBJAVA_SHA256 "%s"\n'
    "#define GW_JVM_PROFILE_FAMILY GW_JVM_NATIVE_LIBRARIES_15\n"
    % (profile, libjava_sha)
)
layout = ["F\ttest-target.properties"]
(assets / "test-target.properties").write_text(
    "abi="
    + ABI
    + "\nzstd.sha256="
    + TARGET["zstd_sha256"]
    + "\nzstd.resource="
    + TARGET["zstd_resource"]
    + "\n"
)
libs = {}
for directory in ["lib", "conf", "legal"]:
    for p in sorted((jre / directory).rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(jre)
        if p.name.endswith(".so"):
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            if p.name in libs and libs[p.name] != digest:
                raise SystemExit(
                    "Different libraries share filename: " + p.name
                )
            libs[p.name] = digest
            shutil.copy2(p, native / p.name)
            layout.append("L\t" + str(rel) + "\t" + p.name)
        else:
            dest = assets / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dest)
            layout.append("F\t" + str(rel))
shutil.copy2(jre / "release", assets / "release")
layout.append("F\trelease")
classes = root / "jvm-jni/build/jre-smoke-classes"
if classes.exists():
    shutil.rmtree(classes)
classes.mkdir(parents=True, exist_ok=True)
native_jars = os.environ.get("GW_NATIVE_JARS", "0") == "1"
sources = sorted(
    str(p)
    for base in [
        root / "android-jni/src/java",
        root / "jvm-jni/src/java",
        root / "android-jni/tests/java",
        root / "jvm-jni/tests/java",
        root / "jvm-jni/build/native/generated/java",
    ]
    for p in base.rglob("*.java")
    if native_jars
    or not (p.parent.name == "compat" and p.name.endswith("Workload.java"))
)
classpath = [str(root / "jvm-jni/vendor/zstd-jni-1.5.7-6.jar")]
if native_jars:
    for pin in json.loads(
        (root / "jvm-jni/tests/config/native-jars.json").read_text()
    ):
        classpath.append(
            str(root / "jvm-jni/vendor" / pin["url"].rsplit("/", 1)[1])
        )
subprocess.run(
    [
        "javac",
        "--release",
        "8",
        "-cp",
        os.pathsep.join(classpath),
        "-d",
        str(classes),
        *sources,
    ],
    check=True,
)
subprocess.run(
    ["jar", "cf", str(assets / "smoke.jar"), "-C", str(classes), "."],
    check=True,
)
layout.append("F\tsmoke.jar")
(assets / "layout.tsv").write_text("\n".join(layout) + "\n")
manifest = {
    "abi": ABI,
    "url": url,
    "sha256": sha,
    "release": (jre / "release").read_text(),
    "libraries": libs,
    "loader_adapter_profile": {"name": profile, "libjava_sha256": libjava_sha},
    "private_path_cache_adapter": {
        "symbol": "_ZL14saved_jvm_path",
        "offset": hex(offset),
        "size": 4096,
    },
}
(root / "jvm-jni/build/native/android-jre.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
print("Android JRE prepared:", len(libs), "libraries; archive SHA256", sha)
