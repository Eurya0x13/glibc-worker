#!/usr/bin/env python3
"""Export each project with its own source, build entry, tests and documentation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = ("core", "bionic-dlopen", "android-jni", "jvm-jni")
EXCLUDED = {
    "build",
    "work",
    "vendor",
    ".git",
    ".gradle",
    ".cxx",
    "__pycache__",
    ".ruff_cache",
}
ROOT_FILES = (
    "README.md",
    ".clang-format",
    ".editorconfig",
    ".gitattributes",
    ".gitignore",
    ".ignore",
    "pyproject.toml",
)


def files_below(directory):
    for current, directories, files in os.walk(directory, followlinks=False):
        directories[:] = sorted(
            d
            for d in directories
            if d not in EXCLUDED and not (Path(current) / d).is_symlink()
        )
        for name in sorted(files):
            path = Path(current) / name
            if (
                path.is_symlink()
                or name == "local.properties"
                or path.suffix == ".pyc"
            ):
                continue
            yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit("Choose an empty output directory")

    packages = {name: list(files_below(ROOT / name)) for name in PROJECTS}
    packages["workspace-tools"] = [ROOT / name for name in ROOT_FILES]
    for name in ("scripts", "docs"):
        packages["workspace-tools"].extend(files_below(ROOT / name))

    manifest = {
        "dependencies": {
            "core": [],
            "bionic-dlopen": ["core"],
            "android-jni": ["bionic-dlopen"],
            "jvm-jni": ["android-jni"],
        },
        "archives": {},
    }

    for name, paths in packages.items():
        archive_path = output / (name + ".tar.gz")
        with tarfile.open(archive_path, "w:gz") as archive:
            for path in sorted(paths):
                archive.add(
                    path,
                    arcname=path.relative_to(ROOT).as_posix(),
                    recursive=False,
                )
        manifest["archives"][archive_path.name] = {
            "files": len(paths),
            "sha256": hashlib.sha256(archive_path.read_bytes()).hexdigest(),
        }

    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "README.md").write_text("""# 源码包

四个项目归档分别包含自己的源码、CMake 安装包、构建入口、测试和开发者文档。
基础组件按 core → bionic-dlopen → android-jni → jvm-jni 的顺序构建；每项通过已安装的 CMake 包消费依赖。
不需要上层实现时，只解压所需项目及其依赖即可。独立构建方法见对应项目 README。

workspace-tools 包提供工作区导航、组合构建、格式检查和源码导出工具。
需要组合构建或 Android/JRE 集成测试时，将所需归档解压到同一工作区。
各项目构建、下载与测试结果仅写入各自目录；工具缓存写入 scripts/build。

归档不含下载缓存、构建产物、历史运行结果、设备资料、local.properties 或 Pojav 接入。
""")
    print("Exported four projects and workspace tools: " + str(output))


if __name__ == "__main__":
    main()
