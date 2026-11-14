"""Check the actual public-JNI artifacts for accidental private-JRE coupling."""

import argparse
import json
from pathlib import Path
import subprocess
from zipfile import ZipFile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--library", type=Path, required=True)
parser.add_argument("--helpers", type=Path, required=True)
parser.add_argument(
    "--output",
    type=Path,
    default=Path(__file__).resolve().parents[1]
    / "build/test-results/boundary.json",
)
args = parser.parse_args()
symbols = subprocess.check_output(
    ["readelf", "-Ws", str(args.library)], text=True
)
dynamic = subprocess.check_output(
    ["readelf", "-d", str(args.library)], text=True
)
assert "gw_jni_register_vm_adapter" in symbols
assert "gw_loader_query" in symbols
assert "gw_jre17_adapter" not in symbols
assert "libgw_jvm_jni" not in dynamic
assert b"jdk/internal/loader/NativeLibraries" not in args.library.read_bytes()
with ZipFile(args.helpers) as jar:
    classes = sorted(n for n in jar.namelist() if n.endswith(".class"))
    assert classes == [
        "dev/glibc/worker/BridgeTypes.class",
        "dev/glibc/worker/GuestImages$Configuration.class",
        "dev/glibc/worker/GuestImages.class",
    ], classes
args.output.parent.mkdir(parents=True, exist_ok=True)
result = {
    "result": "PASS",
    "private_jre_symbols": False,
    "private_jre_dependency": False,
    "helper_classes": classes,
}
args.output.write_text(json.dumps(result, indent=2) + "\n")
print("ANDROID_JNI_BOUNDARY result=PASS private_jre_dependency=0")
