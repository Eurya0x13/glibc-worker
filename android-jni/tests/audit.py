"""Compile production command/transport code under ASan/UBSan and inject failures."""

import json
import os
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent
out = PROJECT / "build/audit-tests"
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
javac = shutil.which("javac")
if not javac:
    raise SystemExit("Install a desktop JDK")
jdk = Path(javac).resolve().parents[1]
binary = out / "transport"
command = [
    os.environ.get("SANITIZER_CC", "clang"),
    "-std=gnu17",
    "-O1",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
    "-pthread",
    "-D_GNU_SOURCE",
    "-Wno-macro-redefined",
    *(
        "-I" + str(p)
        for p in (
            PROJECT / "src/host",
            PROJECT / "src/common",
            PROJECT / "include",
            ROOT / "core/include",
            ROOT / "bionic-dlopen/include",
            jdk / "include",
            jdk / "include/linux",
        )
    ),
    *(
        str(ROOT / p)
        for p in (
            "android-jni/tests/native/audit_transport_test.c",
            "android-jni/src/host/command.c",
            "android-jni/src/host/failure.c",
            "android-jni/src/host/loader_transport.c",
            "bionic-dlopen/src/loader.c",
            "bionic-dlopen/src/elf_route.c",
        )
    ),
    "-o",
    str(binary),
]
subprocess.run(command, check=True)
run = subprocess.run(
    [str(binary)],
    capture_output=True,
    text=True,
    timeout=60,
    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
)
(out / "transport.log").write_text(run.stdout + run.stderr)
if run.returncode:
    raise SystemExit("Audit transport failed: " + str(out / "transport.log"))
row = json.loads(run.stdout)
assert row["result"] == "PASS"
(out / "summary.json").write_text(json.dumps(row, indent=2) + "\n")
print("Production loader/command failure checks: PASS")
