#!/usr/bin/env python3
"""Format maintained code or check it; never traverse downloads/build caches."""

import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = {".c", ".h", ".java", ".py", ".sh", ".zig"}
# Hand-maintained C fragments injected into the experimental runtime. Generated
# JNI .inc files are intentionally absent: their generators own their format.
C_FRAGMENTS = {
    "core/src/runtime/adopt-nptl.inc",
    "core/src/runtime/signal-guard.inc",
    "core/src/runtime/signal-mask.inc",
    "core/src/runtime/context-mask.inc",
    "core/src/runtime/wait-mask.inc",
    "core/src/runtime/jump-hook.inc",
    "core/src/domain/signal-registry.inc",
}
TEXT_EXTENSIONS = EXTENSIONS | {".md", ".json", ".kts", ".properties"}


def executable(name):
    configured = os.environ.get(name.upper().replace("-", "_"))
    path = configured or shutil.which(name)
    if not path and (ROOT / "scripts/build/tools" / name).is_file():
        path = str(ROOT / "scripts/build/tools" / name)
    if not path:
        raise SystemExit(
            "Install " + name + " or put it on PATH (see docs/en/STYLE.md)"
        )
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    groups = {"clang-format": [], "shfmt": [], "ruff": [], "zig": []}

    directories = (
        "core",
        "bionic-dlopen",
        "android-jni",
        "jvm-jni",
        "scripts",
        "docs",
        "config",
    )
    excluded = {
        "build",
        "work",
        "vendor",
        ".gradle",
        "__pycache__",
        "third_party",
    }
    for directory in directories:
        # Prune before walking: build/runtime trees can contain millions of
        # files, and none of their generated sources belongs to this check.
        for parent, children, names in os.walk(ROOT / directory):
            children[:] = sorted(
                name for name in children if name not in excluded
            )
            for name in sorted(names):
                path = Path(parent) / name
                fragment = path.relative_to(ROOT).as_posix() in C_FRAGMENTS
                if path.suffix not in TEXT_EXTENSIONS and not fragment:
                    continue
                data = path.read_bytes()
                normalized = data.replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"
                if data != normalized:
                    if args.check:
                        raise SystemExit(
                            "Use LF and one final newline: " + str(path)
                        )
                    path.write_bytes(normalized)
                if path.suffix not in EXTENSIONS and not fragment:
                    continue
                tool = (
                    "zig"
                    if path.suffix == ".zig"
                    else "ruff"
                    if path.suffix == ".py"
                    else "shfmt"
                    if path.suffix == ".sh"
                    else "clang-format"
                )
                groups[tool].append(str(path.relative_to(ROOT)))

    tools = {name: executable(name) for name in groups}
    for name, files in groups.items():
        if name == "clang-format":
            options = ["--dry-run", "--Werror"] if args.check else ["-i"]
        elif name == "shfmt":
            options = ["-i", "2", "-ci", "-bn", "-d" if args.check else "-w"]
        elif name == "zig":
            options = ["fmt", *(["--check"] if args.check else [])]
        else:
            options = [
                "format",
                "--no-cache",
                *(["--check"] if args.check else []),
            ]
        subprocess.run([tools[name], *options, *files], cwd=ROOT, check=True)

    print(
        "Formatting "
        + ("check" if args.check else "complete")
        + ": "
        + str(sum(map(len, groups.values())))
        + " files"
    )


if __name__ == "__main__":
    main()
