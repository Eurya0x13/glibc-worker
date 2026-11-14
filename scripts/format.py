#!/usr/bin/env python3
"""Format maintained code or check it; never traverse downloads/build caches."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = {".c", ".h", ".java", ".py", ".sh"}
TEXT_EXTENSIONS = EXTENSIONS | {".md", ".json", ".kts", ".properties"}


def executable(name):
    configured = os.environ.get(name.upper().replace("-", "_"))
    path = configured or shutil.which(name)
    if not path and (ROOT / "scripts/build/tools" / name).is_file():
        path = str(ROOT / "scripts/build/tools" / name)
    if not path:
        raise SystemExit(
            "Install " + name + " or put it on PATH (see docs/STYLE.md)"
        )
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    groups = {"clang-format": [], "shfmt": [], "ruff": []}

    for directory in (
        "core",
        "bionic-dlopen",
        "android-jni",
        "jvm-jni",
        "scripts",
        "docs",
        "config",
    ):
        for path in sorted((ROOT / directory).rglob("*")):
            if not path.is_file() or path.suffix not in TEXT_EXTENSIONS:
                continue
            if any(
                part
                in (
                    "build",
                    "work",
                    "vendor",
                    ".gradle",
                    "__pycache__",
                    "third_party",
                )
                for part in path.relative_to(ROOT).parts
            ):
                continue
            data = path.read_bytes()
            normalized = data.replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"
            if data != normalized:
                if args.check:
                    raise SystemExit(
                        "Use LF and one final newline: " + str(path)
                    )
                path.write_bytes(normalized)
            if path.suffix not in EXTENSIONS:
                continue
            if any(
                part in (".gradle", "build", "__pycache__", "third_party")
                for part in path.relative_to(ROOT).parts
            ):
                continue
            tool = (
                "ruff"
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
