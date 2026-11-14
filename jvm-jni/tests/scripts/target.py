"""Test artifact pins layered over the independent runtime toolchain profile."""

import json
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[3]
runtime = runpy.run_path(str(ROOT / "scripts/target.py"))
ABI = runtime["ABI"]
pins = json.loads((ROOT / "jvm-jni/tests/config/targets.json").read_text())
PROFILES = {
    abi: {**profile, **pins[abi]}
    for abi, profile in runtime["PROFILES"].items()
}
TARGET = PROFILES[ABI]
find_ndk = runtime["find_ndk"]
