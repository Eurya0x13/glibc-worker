"""Invoke the project's build graph for native test fixtures."""

import os
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parents[1]


def build(output, target="native", scope="host"):
    compiler = shutil.which(os.environ.get("ZIG", "zig"))
    if not compiler:
        raise SystemExit("Install Zig 0.16.0 for Host resource tests")
    output = Path(output).resolve()
    prefix = output.parent / ("stage-" + scope + "-" + target)
    command = [
        compiler,
        "build",
        "--build-file",
        str(PROJECT / "build.zig"),
        "--prefix",
        str(prefix),
        "--cache-dir",
        str(output.parent / "zig-cache"),
        "--global-cache-dir",
        str(PROJECT / "build/zig-global-cache"),
        "host-store",
        "-Dcpu=baseline",
        "-Dbindings-only=" + str(scope == "bindings").lower(),
    ]
    if target != "native":
        command.append("-Dtarget=" + target)
    subprocess.run(command, check=True)
    shutil.copyfile(
        prefix / ("binding_store.o" if scope == "bindings" else "host_store.o"),
        output,
    )
