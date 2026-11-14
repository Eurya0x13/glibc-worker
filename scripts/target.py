"""Workspace toolchain discovery; the target profile belongs to core."""

import os
from pathlib import Path
import runpy

workspace = Path(__file__).resolve().parents[1]
local = workspace / "local.properties"
if (
    not (os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME"))
    and local.exists()
):
    for line in local.read_text().splitlines():
        if line.startswith("sdk.dir="):
            os.environ["ANDROID_SDK_ROOT"] = line.split("=", 1)[1]
            break
profile = runpy.run_path(
    str(workspace / "core/scripts/target.py"),
    run_name="__main__" if __name__ == "__main__" else "core_target",
)
if __name__ != "__main__":
    globals().update(profile)
