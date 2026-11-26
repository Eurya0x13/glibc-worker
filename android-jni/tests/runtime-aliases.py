"""Run the installed Guest library helper and the packaged GNU loader."""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def build_fixtures(compiler, aliases, root, loader_name, stdcxx):
    source = Path(__file__).with_name("runtime-aliases")
    run(
        compiler,
        "-shared",
        "-fPIC",
        source / "consumer.c",
        "-Wl,-z,nodefaultlib,--no-as-needed",
        "-L" + str(aliases),
        "-l:libdl.so.2",
        "-l:libgcc_s.so.1",
        "-o",
        root / "module.so",
    )
    if stdcxx:
        run(
            compiler.removesuffix("gcc") + "g++",
            "-shared",
            "-fPIC",
            "-nodefaultlibs",
            source / "consumer.cpp",
            "-Wl,-z,defs,-z,nodefaultlib,--no-as-needed",
            "-Wl,-rpath,$ORIGIN",
            "-Wl,-rpath-link," + str(aliases),
            "-L" + str(aliases),
            "-l:libstdc++.so.6",
            "-l:libgcc_s.so.1",
            "-l:libc.so.6",
            "-l:" + loader_name,
            "-o",
            root / "module-cxx.so",
        )

    probe = root / "probe"
    run(
        compiler,
        "-std=gnu17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        source / "probe.c",
        "-Wl,-z,nodefaultlib",
        "-ldl",
        "-o",
        probe,
    )
    for link, target in (
        ("module-link.so", "module-next.so"),
        ("module-next.so", "module.so"),
        ("dangling.so", "absent.so"),
        ("cycle-a.so", "cycle-b.so"),
        ("cycle-b.so", "cycle-a.so"),
    ):
        (root / link).symlink_to(target)

    return probe


def exercise(package, helpers, runner, root):
    manifest = json.loads((package / "domain-runtime.json").read_text())
    abi = manifest["runtime"]["abi"]
    compiler = {"x86_64": "gcc", "arm64-v8a": "aarch64-linux-gnu-gcc"}[abi]
    loader_name = next(
        name
        for name, image in manifest["glibc_aliases"].items()
        if image == "libguest_loader.so"
    )
    compat = root / "compat"
    shutil.copytree(package, compat, symlinks=True)
    loader = compat / "libguest_loader.so"
    loader.chmod(0o755)
    aliases = compat / "glibc-libs"
    stdcxx = manifest["glibc_aliases"].get("libstdc++.so.6")
    probe = build_fixtures(compiler, aliases, root, loader_name, stdcxx)

    run(
        "javac",
        "--release",
        "8",
        "-cp",
        helpers,
        "-d",
        root,
        Path(__file__).with_name("runtime-aliases") / "AliasProbe.java",
    )
    staged = Path(
        run(
            "java",
            "-cp",
            str(root) + os.pathsep + str(helpers),
            "AliasProbe",
            root / "app-private",
            compat,
            stdout=subprocess.PIPE,
        ).stdout.strip()
    )
    cases = [(aliases, None), (staged, None)]
    if stdcxx:
        shadow = root / "shadow"
        shadow.mkdir()
        conflict = shutil.copyfile(loader, shadow / loader_name)
        cases.append((aliases, conflict))

    for directory, conflict in cases:
        relative = (directory / "libdl.so.2").relative_to(root)
        path = f"{conflict.parent}:{directory}" if conflict else directory
        run(
            *runner,
            loader,
            "--inhibit-cache",
            "--library-path",
            path,
            probe,
            compat / "libguest_dl.so",
            "./" + str(relative),
            root / relative,
            compat / "libguest_unwind.so",
            *([compat / stdcxx] if stdcxx else []),
            *([conflict] if conflict else []),
            cwd=root,
            env={
                k: v for k, v in os.environ.items() if not k.startswith("LD_")
            },
        )
    print(
        f"{abi}: packaged and staged Guest aliases passed (libstdc++={bool(stdcxx)})"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--helpers", type=Path, required=True)
    parser.add_argument("--runner", nargs="+", default=[])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gw-runtime-aliases-") as directory:
        exercise(
            args.package, args.helpers.resolve(), args.runner, Path(directory)
        )
