"""Pin and package the Pojav Android/bionic JRE, not a desktop Linux JRE."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
from target import ABI, TARGET, find_ndk
from jre_artifacts import select, verify_home

artifact = select(ABI)

root = Path(__file__).resolve().parents[3]
archive = root / "jvm-jni/vendor" / artifact["archive"]
sha = artifact["archive_sha256"]
url = artifact["url"]
archive.parent.mkdir(exist_ok=True)
if not archive.exists():
    urllib.request.urlretrieve(url, str(archive) + ".tmp")
    Path(str(archive) + ".tmp").rename(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != sha:
    raise SystemExit("Android JRE checksum mismatch")
jre = root / "jvm-jni/vendor" / ("android-" + artifact["id"])
marker = jre / ".source-sha256"
if not marker.exists() or marker.read_text().strip() != sha:
    if jre.exists():
        shutil.rmtree(jre)
    jre.mkdir()
    with tarfile.open(archive) as t:
        t.extractall(jre, filter="data")
    marker.write_text(sha + "\n")
verify_home(jre, artifact)
assets = root / "jvm-jni/build/tests/android/assets/android-jre"
if assets.exists():
    shutil.rmtree(assets)
assets.mkdir(parents=True)
native = root / "jvm-jni/build/tests/android/jniLibs" / ABI
native.mkdir(parents=True, exist_ok=True)
# Only explicitly reviewed artifacts may use the private startup cache.
jvm = jre / artifact["libjvm"]["path"]
jvm_sha = artifact["libjvm"]["sha256"]
offset = 0
if artifact["startup"] == "saved-jvm-path":
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
    "#define JRE_USE_JLI %d\n"
    "#define JRE_LAUNCHER_JNI_BASELINE %d\n"
    "#define JRE_PATH_CACHE_OFFSET 0x%xUL\n#define JRE_PATH_CACHE_SIZE %d\n"
    '#define JRE_LIBJVM_SHA256 "%s"\n'
    '#define JRE_ARTIFACT_ID "%s"\n'
    '#define JRE_LIBJVM_PATH "%s"\n'
    '#define JRE_LIBJAVA_PATH "%s"\n'
    '#define JRE_BOOT_LIBRARY_PATH "%s"\n'
    "#define JRE_EXPECTED_MAJOR %d\n"
    '#define JRE_ARTIFACT_ABI "%s"\n'
    '#define JRE_ARCHIVE_SHA256 "%s"\n'
    '#define JRE_LIBJAVA_SHA256 "%s"\n'
    "#define JRE_PROFILE_FAMILY %d\n"
    % (
        int(artifact["startup"] == "pojav-layout"),
        int(artifact.get("launcher_jni_baseline", False)),
        offset,
        4096 if offset else 0,
        jvm_sha,
        artifact["id"],
        artifact["libjvm"]["path"],
        artifact["libjava"]["path"],
        artifact["boot_library_path"],
        artifact["major"],
        ABI,
        artifact["archive_sha256"],
        artifact["libjava"]["sha256"],
        artifact["family"],
    )
)
libjava_sha = artifact["libjava"]["sha256"]
profile = artifact["id"]
(root / "jvm-jni/build/native/jre-adapter-profile.h").write_text(
    '#define GW_JVM_PROFILE_NAME "%s"\n'
    '#define GW_JVM_LIBJAVA_SHA256 "%s"\n'
    '#define GW_JVM_LIBJVM_SHA256 "%s"\n'
    "#define GW_JVM_PROFILE_FAMILY %d\n"
    % (profile, libjava_sha, jvm_sha, artifact["family"])
)
layout = ["F\ttest-target.properties", "F\tjre-artifact.json"]
(assets / "jre-artifact.json").write_text(json.dumps(artifact, indent=2) + "\n")
(assets / "test-target.properties").write_text(
    "abi="
    + ABI
    + "\nzstd.sha256="
    + TARGET["zstd_sha256"]
    + "\nzstd.resource="
    + TARGET["zstd_resource"]
    + "\n"
)
previous = root / "jvm-jni/build/native/android-jre.json"
if previous.is_file():
    for name in json.loads(previous.read_text()).get("libraries", {}):
        (native / name).unlink(missing_ok=True)
libs = {}
for directory in ["bin", "lib", "conf", "legal"]:
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
# A JRE archive need not ship its C++ runtime. Support libraries are explicit,
# pinned test dependencies; never silently pick an arbitrary host/NDK library.
for support in artifact.get("ndk_libraries", []):
    ndk = Path(find_ndk()).resolve()
    source = (ndk / support["path"]).resolve()
    if not source.is_relative_to(ndk) or source.name != support["name"]:
        raise SystemExit("Invalid NDK support-library path")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != support["sha256"]:
        raise SystemExit(
            "NDK support-library SHA256 mismatch: " + support["name"]
        )
    if source.name in libs:
        raise SystemExit("Conflicting NDK support library: " + source.name)
    libs[source.name] = digest
    shutil.copy2(source, native / source.name)
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
    "artifact": artifact,
    "url": url,
    "sha256": sha,
    "release": (jre / "release").read_text(),
    "libraries": libs,
    "loader_adapter_profile": {
        "name": profile,
        "libjava_sha256": libjava_sha,
        "libjvm_sha256": jvm_sha,
    },
    "private_path_cache_adapter": {
        "strategy": artifact["startup"],
        "symbol": "_ZL14saved_jvm_path" if offset else None,
        "offset": hex(offset),
        "size": 4096 if offset else 0,
    },
}
(root / "jvm-jni/build/native/android-jre.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
print("Android JRE prepared:", len(libs), "libraries; archive SHA256", sha)
