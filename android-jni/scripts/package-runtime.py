"""Stage a matched glibc payload and its exact GNU library names."""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path


def read_soname(image, machine):
    with image.open("rb") as stream:
        header = stream.read(20)
    # ELF64, little endian, ET_DYN and the target e_machine.
    if (
        len(header) != 20
        or header[:6] != b"\x7fELF\x02\x01"
        or struct.unpack_from("<HH", header, 16) != (3, machine)
    ):
        raise ValueError(f"Wrong ELF type or architecture: {image}")

    dynamic = subprocess.check_output(
        ["readelf", "-Wd", image], env={**os.environ, "LC_ALL": "C"}, text=True
    )
    names = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    if names != [image.name]:
        raise ValueError(f"Unexpected DT_SONAME in {image}: {names}")
    return names[0]


def prepare_aliases(directory, aliases):
    directory.mkdir(exist_ok=True)
    if directory.is_symlink():
        raise ValueError(f"Expected a real alias directory: {directory}")

    links = {name: Path("..") / image for name, image in aliases.items()}
    for entry in directory.iterdir():
        if not entry.is_symlink() or entry.readlink() != links.get(entry.name):
            raise ValueError(f"Conflicting glibc alias: {entry}")

    for name, target in links.items():
        link = directory / name
        if not link.is_symlink():
            link.symlink_to(target)


def package(runtime, output, abi, loader):
    base = json.loads((runtime / "manifest.json").read_text())
    if base.get("domain_abi") != 1 or base.get("abi") != abi:
        raise ValueError("Matching domain runtime manifest required")

    machine = {"x86_64": 62, "arm64-v8a": 183}[abi]
    images = {
        "libguest_loader.so": runtime / loader,
        "libguest_libc.so": runtime / "libc.so.6",
        "libguest_pthread.so": runtime / "libpthread.so.0",
        "libguest_math.so": runtime / "libm.so.6",
        "libguest_dl.so": runtime / "libdl.so.2",
        "libguest_rt.so": runtime / "librt.so.1",
    }
    aliases = {}
    for image, source in images.items():
        soname = read_soname(source, machine)
        if soname in aliases:
            raise ValueError(f"Conflicting glibc SONAME: {soname}")
        aliases[soname] = image

    prepare_aliases(output / "glibc-libs", aliases)
    for image, source in images.items():
        shutil.copyfile(source, output / image)

    sha256 = {
        image: hashlib.sha256((output / image).read_bytes()).hexdigest()
        for image in (*images, "libguest_unwind.so", "libguest_service.so")
    }
    manifest = {
        "version": 1,
        "domain_abi": 1,
        "default_backend": "same-thread",
        "runtime": base,
        "sha256": sha256,
        "glibc_aliases": aliases,
    }
    (output / "domain-runtime.json").write_text(
        json.dumps(manifest, indent=2) + "\n"
    )
    (output / "glibc-libs.tsv").write_text(
        "".join(
            f"{name}\t{image}\t{sha256[image]}\n"
            for name, image in sorted(aliases.items())
        ),
        encoding="ascii",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("abi", choices=("x86_64", "arm64-v8a"))
    parser.add_argument("loader")
    args = parser.parse_args()
    try:
        package(args.runtime, args.output, args.abi, args.loader)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
