"""Sanitize shared context/frame growth, allocation failures and concurrent claims."""

import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / "android-jni/build/context-page-tests"
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
binary = out / "pages"
subprocess.run(
    [
        os.environ.get("SANITIZER_CC", "clang"),
        "-std=gnu17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-pthread",
        "-Wl,--wrap=mmap",
        *[
            "-I" + str(root / p)
            for p in (
                "android-jni/src/common",
                "android-jni/build/jni-include",
                "core/include",
                "bionic-dlopen/include",
            )
        ],
        str(root / "android-jni/tests/native/context_pages.c"),
        "-o",
        str(binary),
    ],
    check=True,
)
r = subprocess.run(
    [str(binary)],
    capture_output=True,
    text=True,
    timeout=30,
    env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
)
(out / "run.log").write_text(r.stdout + r.stderr)
if r.returncode:
    raise SystemExit("Context page checks failed: " + str(out / "run.log"))
result = json.loads(r.stdout)
assert result["result"] == "PASS"
(out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print("Shared context/frame pages: PASS")
