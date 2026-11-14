#!/usr/bin/env python3
"""Exercise the Host stores with real libffi closures."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
from host_build import build

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, default=PROJECT / "build/binding-tests"
    )
    parser.add_argument("--jdk", type=Path)
    parser.add_argument(
        "--core-include", type=Path, default=ROOT / "core/include"
    )
    parser.add_argument(
        "--dlopen-include", type=Path, default=ROOT / "bionic-dlopen/include"
    )
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = output / "summary.json"
    summary.unlink(missing_ok=True)
    javac = shutil.which("javac")
    jdk = args.jdk or (Path(javac).resolve().parents[1] if javac else None)
    if not jdk or not (jdk / "include/jni.h").is_file():
        raise SystemExit("Provide --jdk with desktop JNI headers")
    if not (args.core_include / "wait_event.h").is_file():
        raise SystemExit("Provide --core-include with installed core headers")
    if not (args.dlopen_include / "module_id.h").is_file():
        raise SystemExit(
            "Provide --dlopen-include with installed loader headers"
        )
    host_obj = output / "host_store.o"
    build(host_obj)
    ffi = shlex.split(
        subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libffi"], text=True
        )
    )
    rows = []
    backend = "zig"
    executable = output / ("bindings-" + backend)
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
        *(
            "-I" + str(p)
            for p in (
                PROJECT / "src/host",
                PROJECT / "include",
                PROJECT / "src/common",
                args.dlopen_include,
                args.core_include,
                jdk / "include",
                jdk / "include/linux",
            )
        ),
        str(PROJECT / "tests/native/binding_store_test.c"),
        str(PROJECT / "src/host/bindings.c"),
        str(PROJECT / "src/host/binding_table.c"),
        str(PROJECT / "src/host/slot_allocator.c"),
        str(host_obj),
        *ffi,
        "-pthread",
        "-Wl,--wrap=ffi_closure_alloc",
        "-o",
        str(executable),
    ]
    subprocess.run(command, check=True)
    run = subprocess.run(
        [str(executable)],
        capture_output=True,
        text=True,
        timeout=120,
        env={
            **os.environ,
            "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1",
        },
    )
    (output / (backend + ".log")).write_text(run.stdout + run.stderr)
    if run.returncode:
        raise SystemExit(f"Binding test failed: {output / (backend + '.log')}")
    result = json.loads(run.stdout)
    assert result["result"] == "PASS" and result["backend"] == backend
    assert result["production_bindings"] == 4096
    assert result["live_closures"] == result["outstanding_allocations"] == 0
    rows.append(result)
    summary.write_text(
        json.dumps(
            {
                "result": "PASS",
                "zig_version": "0.16.0",
                "runs": rows,
                "sources_sha256": {
                    name: hashlib.sha256(
                        (PROJECT / name).read_bytes()
                    ).hexdigest()
                    for name in (
                        "src/host/binding_store.zig",
                        "src/host/bindings.c",
                        "tests/native/binding_store_test.c",
                    )
                },
            },
            indent=2,
        )
        + "\n"
    )
    print("Host binding checks passed: " + str(summary))


if __name__ == "__main__":
    main()
