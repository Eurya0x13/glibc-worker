"""Invoke the project's build graph for native test fixtures."""

import os
import shutil
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def build(output, target="native", project=PROJECT):
    compiler = shutil.which(os.environ.get("ZIG", "zig"))
    if not compiler:
        raise SystemExit("Install Zig 0.16.0 for Host resource tests")
    output = Path(output).resolve()
    prefix = output.parent / ("stage-host-" + target)
    command = [
        compiler,
        "build",
        "--build-file",
        str(Path(project) / "build.zig"),
        "--prefix",
        str(prefix),
        "--cache-dir",
        str(output.parent / "zig-cache"),
        "--global-cache-dir",
        str(PROJECT / "build/zig-global-cache"),
        "host-store",
        "-Dcpu=baseline",
    ]
    if target != "native":
        command.append("-Dtarget=" + target)
    subprocess.run(command, check=True)
    shutil.copyfile(prefix / "host_store.o", output)
