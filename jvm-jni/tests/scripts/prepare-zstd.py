"""Package original Maven artifacts; never rebuild or rewrite their classes/ELF."""

import hashlib
import json
from pathlib import Path
import shutil
import sys
import urllib.request
import zipfile
from target import ABI, TARGET

ROOT = Path(__file__).resolve().parents[3]
VERSION = "1.5.7-6"
NAME = "zstd-jni-" + VERSION
BASE = (
    "https://repo.maven.apache.org/maven2/com/github/luben/zstd-jni/"
    + VERSION
    + "/"
)
SHA = "8d6feb1da335f3ab13c584c613e23c7b3c61b392e37956872057baf8f0ca1d6f"
ELF_SHA = TARGET["zstd_sha256"]
SOURCE_SHA = "0b29fece88f9931042062c824423366a5484f7b0e2204d5ead0637fb2700ab23"


def fetch(name, sha):
    path = ROOT / "jvm-jni/vendor" / name
    if not path.exists():
        path.parent.mkdir(exist_ok=True)
        urllib.request.urlretrieve(BASE + name, str(path) + ".tmp")
        Path(str(path) + ".tmp").replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        raise SystemExit("zstd artifact checksum mismatch: " + name)
    return path


jar = fetch(NAME + ".jar", SHA)
sources = fetch(NAME + "-sources.jar", SOURCE_SHA)
if "--fetch-only" in sys.argv:
    raise SystemExit(0)
assets = ROOT / "jvm-jni/build/tests/android/assets/android-jre"
native = ROOT / "jvm-jni/build/tests/android/jniLibs" / ABI
assets.mkdir(parents=True, exist_ok=True)
native.mkdir(parents=True, exist_ok=True)
resource = TARGET["zstd_resource"]
with zipfile.ZipFile(jar) as z:
    image = z.read(resource)
if hashlib.sha256(image).hexdigest() != ELF_SHA:
    raise SystemExit("Bundled Linux GNU ELF checksum mismatch")
# Deliberately different name: original loadLibrary must fail, causing the JAR's
# own extraction + System.load(tempPath). Android installs these unchanged bytes.
installed = "libinstalled_zstd.so"
(native / installed).write_bytes(image)
shutil.copyfile(jar, assets / (NAME + ".jar"))
(assets / "guest-images.tsv").write_text(
    ELF_SHA + "\t" + str(len(image)) + "\t" + installed + "\n"
)
layout = assets / "layout.tsv"
with layout.open("a") as out:
    out.write("F\t" + NAME + ".jar\nF\tguest-images.tsv\n")
manifest = dict(
    version=VERSION,
    url=BASE + jar.name,
    jar_sha256=SHA,
    abi=ABI,
    published_sha1="5ce34668f1ec629a343b9047f36c6cc4c0d3683a",
    sources_url=BASE + sources.name,
    sources_sha256=SOURCE_SHA,
    elf_resource=resource,
    elf_sha256=ELF_SHA,
    elf_bytes=len(image),
    installed_name=installed,
    jar_modified=False,
    elf_modified=False,
    native_path_override=False,
    assume_loaded=False,
)
(ROOT / "jvm-jni/build/native/zstd-jni.json").write_text(
    json.dumps(manifest, indent=2) + "\n"
)
notice = ROOT / "jvm-jni/build/tests/android/assets/runtime/zstd-jni-NOTICE.txt"
notice.parent.mkdir(parents=True, exist_ok=True)
# Preserve upstream license texts, pinned independently of the live branch.
licenses = ROOT / "jvm-jni/tests/third_party/zstd-jni"
notice.write_text(
    "Original zstd-jni "
    + VERSION
    + "\n"
    + BASE
    + jar.name
    + "\n\n"
    + "\n\n".join(p.read_text() for p in sorted(licenses.glob("*LICENSE*")))
)
print("Verified original zstd-jni", VERSION, ABI, "ELF", ELF_SHA)
