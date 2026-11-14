"""One target profile for build, packaging and verification scripts.

GW_ABI selects Android ABI, defaulting to the original x86_64 experiment.
Shell consumers source the safely quoted output of ``target.py --shell``.
"""

import json
import os
from pathlib import Path
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]
ABI = os.environ.get("GW_ABI", "x86_64")
PROFILES = json.loads((ROOT / "config/targets.json").read_text())
if ABI not in PROFILES:
    raise SystemExit("Unsupported GW_ABI: " + ABI)
TARGET = PROFILES[ABI]


def find_ndk():
    """Discover a matching installed NDK; never invent a toolchain path."""
    configured = os.environ.get("ANDROID_NDK_HOME")
    if configured:
        candidates = [Path(configured)]
    else:
        sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get(
            "ANDROID_HOME"
        )

        local = ROOT / "local.properties"
        if not sdk and local.exists():
            sdk = next(
                (
                    line.split("=", 1)[1]
                    for line in local.read_text().splitlines()
                    if line.startswith("sdk.dir=")
                ),
                None,
            )
        if not sdk:
            raise SystemExit(
                "Set ANDROID_NDK_HOME, Android SDK environment or sdk.dir"
            )

        candidates = sorted(
            (Path(sdk) / "ndk").glob("*"),
            reverse=True,
            key=lambda p: tuple(
                int(x) for x in p.name.split(".") if x.isdigit()
            ),
        )

    for path in candidates:
        compiler = (
            path
            / "toolchains/llvm/prebuilt/linux-x86_64/bin"
            / (TARGET["ndk_triplet"] + "29-clang")
        )
        if compiler.is_file() and os.access(compiler, os.X_OK):
            return path

    raise SystemExit("No matching installed NDK compiler for " + ABI)


if __name__ == "__main__":
    if sys.argv[1:] == ["--shell"]:
        values = {
            "GW_ABI": ABI,
            **{"GW_" + k.upper(): v for k, v in TARGET.items()},
        }
        for name, value in values.items():
            print("export " + name + "=" + shlex.quote(value))
    elif sys.argv[1:] == ["--ndk"]:
        print(find_ndk())
    else:
        print(json.dumps({"abi": ABI, **TARGET}, indent=2))
