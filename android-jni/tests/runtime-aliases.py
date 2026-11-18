"""Run the installed glibc-alias helper and the packaged GNU loader."""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def exercise(package, helpers, runner, root):
    manifest = json.loads((package / "domain-runtime.json").read_text())
    abi = manifest["runtime"]["abi"]
    compiler = {"x86_64": "gcc", "arm64-v8a": "aarch64-linux-gnu-gcc"}[abi]
    source = Path(__file__).with_name("runtime-aliases")
    compat = root / "compat"
    shutil.copytree(package, compat, symlinks=True)
    loader = compat / "libguest_loader.so"
    loader.chmod(0o755)
    aliases = compat / "glibc-libs"
    run(
        compiler,
        "-shared",
        "-fPIC",
        source / "consumer.c",
        "-Wl,-z,nodefaultlib,--no-as-needed",
        "-L" + str(aliases),
        "-l:libdl.so.2",
        "-o",
        root / "module.so",
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

    run(
        "javac",
        "--release",
        "8",
        "-cp",
        helpers,
        "-d",
        root,
        source / "AliasProbe.java",
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
    for directory in (aliases, staged):
        relative = (directory / "libdl.so.2").relative_to(root)
        run(
            *runner,
            loader,
            "--inhibit-cache",
            "--library-path",
            directory,
            probe,
            compat / "libguest_dl.so",
            "./" + str(relative),
            root / relative,
            cwd=root,
            env={
                k: v for k, v in os.environ.items() if not k.startswith("LD_")
            },
        )
    print(f"{abi}: packaged and staged glibc aliases passed")


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
