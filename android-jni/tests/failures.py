"""Run production Host/Guest failure paths with VM and worker test doubles."""

import json
import os
from pathlib import Path
import shlex
import subprocess
from host_build import build

root = Path(__file__).resolve().parents[2]
out = root / "android-jni/build/failure-tests"
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
header = root / "android-jni/build/jni-include/jni.h"
if not header.is_file():
    raise SystemExit(
        "Prepare Android JNI headers with the Android integration build first"
    )
subprocess.run(
    [
        "python3",
        str(root / "android-jni/scripts/gen-jni.py"),
        "--jni-header",
        str(header),
        "--output",
        str(out / "generated"),
    ],
    check=True,
)
obj = out / "host_store.o"
build(obj)
includes = [
    "android-jni/include",
    "android-jni/src/common",
    "android-jni/src/host",
    "android-jni/src/guest",
    "android-jni/build/jni-include",
    "core/include",
    "bionic-dlopen/include",
]
sources = [
    "android-jni/tests/native/failure_paths.c",
    "android-jni/src/host/full_host.c",
    "android-jni/src/host/failure.c",
    "android-jni/src/host/references.c",
    "android-jni/src/host/bindings.c",
    "android-jni/src/host/binding_table.c",
    "android-jni/src/host/slot_allocator.c",
    "android-jni/src/host/command.c",
    "android-jni/src/host/array_copy.c",
    "android-jni/src/host/loader_backend.c",
    "android-jni/src/guest/full_guest.c",
    "android-jni/src/guest/guest_loader.c",
    "android-jni/src/guest/rpc.c",
    "bionic-dlopen/src/loader.c",
    "bionic-dlopen/src/elf_route.c",
]
ffi = shlex.split(
    subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libffi"], text=True
    )
)
cmd = [
    os.environ.get("SANITIZER_CC", "clang"),
    "-std=gnu17",
    "-O1",
    "-g",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wl,--gc-sections",
    "-Wl,--wrap=posix_memalign",
    "-Wl,--wrap=pthread_setspecific",
    "-I" + str(out),
    *("-I" + str(root / p) for p in includes),
    *(str(root / p) for p in sources),
    str(obj),
    *ffi,
    "-ldl",
    "-pthread",
    "-o",
    str(out / "failure_paths"),
]
subprocess.run(cmd, check=True)
r = subprocess.run(
    [str(out / "failure_paths")],
    capture_output=True,
    text=True,
    timeout=60,
    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
)
(out / "run.log").write_text(r.stdout + r.stderr)
if r.returncode:
    raise SystemExit("Failure checks failed: " + str(out / "run.log"))
d = json.loads(r.stdout)
assert d["result"] == "PASS"
(out / "summary.json").write_text(json.dumps(d, indent=2) + "\n")
print("Production failure paths: PASS")
