#!/usr/bin/env python3
"""Check array-cache bounds/ownership with ASan and UBSan on the build host."""

import argparse
import json
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "android-jni/build/array-cache-tests",
    )
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").unlink(missing_ok=True)
    if not (root / "android-jni/build/jni-include/jni.h").is_file():
        parser.error(
            "Prepare Android JNI headers with the integration build first"
        )
    command = [
        args.cc,
        "-std=gnu17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-pthread",
    ]
    for include in (
        "android-jni/build/jni-include",
        "android-jni/include",
        "android-jni/src/common",
        "android-jni/src/host",
        "core/include",
        "bionic-dlopen/include",
    ):
        command += ["-I", str(root / include)]
    command += [
        str(root / "android-jni/tests/native/array_cache_limits.c"),
        "-o",
        str(out / "array-cache-limits"),
    ]
    subprocess.run(command, check=True)
    result = subprocess.run(
        [str(out / "array-cache-limits")],
        check=True,
        text=True,
        capture_output=True,
    )
    (out / "run.txt").write_text(result.stdout + result.stderr)
    (out / "summary.json").write_text(
        json.dumps(
            {
                "result": "PASS",
                "sanitizers": ["address", "undefined"],
                "compile": command,
            },
            indent=2,
        )
        + "\n"
    )
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
