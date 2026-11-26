"""Stage the Guest payload and its exact GNU library names."""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

UNWIND_IMAGE = "libguest_unwind.so"
STDCXX_IMAGE = "libguest_stdcpp.so"
STDCXX_SONAME = "libstdc++.so.6"


def read_dependencies(image, machine, soname):
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
    if names != [soname]:
        raise ValueError(f"Unexpected DT_SONAME in {image}: {names}")
    return set(re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic))


def prepare_aliases(directory, aliases):
    directory.mkdir(exist_ok=True)
    if directory.is_symlink():
        raise ValueError(f"Expected a real alias directory: {directory}")

    links = {name: Path("..") / image for name, image in aliases.items()}
    stale_stdcxx = None
    for entry in directory.iterdir():
        if entry.is_symlink():
            target = entry.readlink()
            if target == links.get(entry.name):
                continue
            if (
                entry.name == STDCXX_SONAME
                and STDCXX_SONAME not in links
                and target == Path("..") / STDCXX_IMAGE
            ):
                stale_stdcxx = entry
                continue
        raise ValueError(f"Conflicting Guest library alias: {entry}")

    for name, target in links.items():
        link = directory / name
        if not link.is_symlink():
            link.symlink_to(target)

    if stale_stdcxx:
        stale_stdcxx.unlink()


def package(runtime, output, abi, loader, unwind, stdcxx, service):
    base = json.loads((runtime / "manifest.json").read_text())
    if base.get("domain_abi") != 1 or base.get("abi") != abi:
        raise ValueError("Matching domain runtime manifest required")

    machine = {"x86_64": 62, "arm64-v8a": 183}[abi]
    images = {
        "libguest_loader.so": (runtime / loader, loader),
        "libguest_libc.so": (runtime / "libc.so.6", "libc.so.6"),
        "libguest_pthread.so": (runtime / "libpthread.so.0", "libpthread.so.0"),
        "libguest_math.so": (runtime / "libm.so.6", "libm.so.6"),
        "libguest_dl.so": (runtime / "libdl.so.2", "libdl.so.2"),
        "libguest_rt.so": (runtime / "librt.so.1", "librt.so.1"),
        UNWIND_IMAGE: (unwind, "libgcc_s.so.1"),
    }
    if stdcxx:
        images[STDCXX_IMAGE] = (stdcxx, STDCXX_SONAME)

    aliases = {}
    dependencies = {}
    for image, (source, soname) in images.items():
        dependencies[image] = read_dependencies(source, machine, soname)
        if soname in aliases:
            raise ValueError(f"Conflicting Guest SONAME: {soname}")
        aliases[soname] = image

    for image in (UNWIND_IMAGE, STDCXX_IMAGE):
        missing = dependencies.get(image, set()) - aliases.keys()
        if missing:
            raise ValueError(
                f"Missing Guest dependencies for {image}: {sorted(missing)}"
            )

    stale_image = output / STDCXX_IMAGE
    if not stdcxx and stale_image.is_dir() and not stale_image.is_symlink():
        raise ValueError(f"Conflicting Guest library image: {stale_image}")

    # Prepare every image and hash before changing an existing package.
    with tempfile.TemporaryDirectory(
        prefix=".guest-stage-", dir=output
    ) as directory:
        stage = Path(directory)
        files = {image: source for image, (source, _) in images.items()}
        files["libguest_service.so"] = service
        sha256 = {}
        for image, source in files.items():
            shutil.copy2(source, stage / image)
            sha256[image] = hashlib.sha256(
                (stage / image).read_bytes()
            ).hexdigest()

        manifest = {
            "version": 1,
            "domain_abi": 1,
            "default_backend": "same-thread",
            "runtime": base,
            "sha256": sha256,
            "glibc_aliases": aliases,
        }
        (stage / "domain-runtime.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
        (stage / "glibc-libs.tsv").write_text(
            "".join(
                f"{name}\t{image}\t{sha256[image]}\n"
                for name, image in sorted(aliases.items())
            ),
            encoding="ascii",
        )

        prepare_aliases(output / "glibc-libs", aliases)
        for image in files:
            (stage / image).replace(output / image)
        if not stdcxx:
            stale_image.unlink(missing_ok=True)

        for name in ("glibc-libs.tsv", "domain-runtime.json"):
            (stage / name).replace(output / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("abi", choices=("x86_64", "arm64-v8a"))
    parser.add_argument("loader")
    parser.add_argument("--unwind", type=Path)
    parser.add_argument("--libstdcxx", type=Path)
    parser.add_argument("--service", type=Path)
    args = parser.parse_args()
    try:
        package(
            args.runtime,
            args.output,
            args.abi,
            args.loader,
            args.unwind or args.output / UNWIND_IMAGE,
            args.libstdcxx,
            args.service or args.output / "libguest_service.so",
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error


if __name__ == "__main__":
    main()
