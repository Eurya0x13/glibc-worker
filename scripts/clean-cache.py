#!/usr/bin/env python3
"""Preview or remove obsolete GNU source/object caches inside this checkout."""

import argparse
import fcntl
import json
import os
import re
import shutil
import subprocess
import uuid
from pathlib import Path

ABIS = ("x86_64", "arm64-v8a")
GENERATED = re.compile(
    r"(?:same-thread|glibc-build)-(?:x86_64|arm64-v8a)-[0-9a-f]{16}"
)


def inside(path, parent):
    return path == parent or parent in path.parents


def read_build_root(path, work):
    root = Path(path.read_text().strip())
    if not root.is_absolute() or root.is_symlink():
        raise ValueError("Invalid build-root pointer: " + str(path))
    root = root.resolve()
    if root.parent != work or not GENERATED.fullmatch(root.name):
        raise ValueError("Build-root escapes generated cache: " + str(path))
    return root


def plan(project, keep_count=1, extra_keep=()):
    work = project / "core/work"
    if work.is_symlink() or work.resolve() != work:
        raise ValueError("core/work must be a real directory in this checkout")
    keep = {}
    for abi in ABIS:
        base = project / "core/build/glibc-runtime" / abi / "build-root.txt"
        if base.is_file():
            keep[read_build_root(base, work)] = (
                "current ordinary runtime source"
            )
        runtime = project / "core/build/domain-runtime" / abi
        manifests = sorted(
            runtime.glob("*/manifest.json"),
            key=lambda path: path.stat().st_mtime_ns,
            reverse=True,
        )
        for manifest in manifests[:keep_count]:
            pointer = manifest.parent / "build-root.txt"
            if not pointer.is_file():
                raise ValueError(
                    "Published runtime has no build-root: " + str(manifest)
                )
            keep[read_build_root(pointer, work)] = "retained domain generation"
    for path in extra_keep:
        path = Path(path).resolve()
        if path.parent != work or not GENERATED.fullmatch(path.name):
            raise ValueError("--keep must select a generated core/work root")
        keep[path] = "explicit --keep"
    candidates = []
    for path in sorted(work.iterdir()):
        if not GENERATED.fullmatch(path.name) or path in keep:
            continue
        if path.is_symlink() or not path.is_dir():
            continue
        if not all(
            marker.is_file()
            for marker in (
                path / "patched",
                path / "src/Makefile",
                path / "obj/Makefile",
            )
        ):
            keep[path] = "incomplete or unrecognized build; inspect manually"
            continue
        candidates.append(path)
    return candidates, keep


def active_users(path):
    users = []
    prefix = str(path)
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            if process.stat().st_uid != os.geteuid():
                continue
            cwd = (process / "cwd").resolve(strict=True)
            command = (process / "cmdline").read_bytes().replace(b"\0", b" ")
            if inside(cwd, path) or prefix.encode() in command:
                users.append(int(process.name))
                continue
            if prefix in (process / "maps").read_text():
                users.append(int(process.name))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            # Same-user processes may still deny inspection (e.g. ptrace
            # restrictions). Missing evidence must not authorize deletion.
            raise ValueError(
                "Cannot inspect same-user process " + process.name
            ) from error
    return users


def validate_candidate(project, path):
    work = project / "core/work"
    if path.parent != work or not GENERATED.fullmatch(path.name):
        raise ValueError("Refusing non-cache path: " + str(path))
    if path.is_symlink() or path.resolve() != path or not path.is_dir():
        raise ValueError("Cache path changed or became a symlink: " + str(path))
    relative = path.relative_to(project)
    tracked = subprocess.check_output(
        ["git", "-C", str(project), "ls-files", "-z", "--", str(relative)]
    )
    if tracked:
        raise ValueError("Cache contains tracked files: " + str(path))
    ignored = subprocess.run(
        ["git", "-C", str(project), "check-ignore", "-q", "--", str(relative)],
        check=False,
    )
    if ignored.returncode != 0:
        raise ValueError("Cache is not Git-ignored: " + str(path))
    users = active_users(path)
    if users:
        raise ValueError(
            "Active processes use cache " + str(path) + ": " + str(users)
        )


def allocated_bytes(paths):
    if not paths:
        return {}
    result = subprocess.check_output(
        ["du", "-s", "-B1", "--", *map(str, paths)], text=True
    )
    return {
        Path(path): int(size)
        for size, path in (line.split("\t", 1) for line in result.splitlines())
    }


def remove(project, path):
    validate_candidate(project, path)
    # Rename first so a concurrent second cleaner cannot traverse this tree.
    # rmtree's fd-based traversal removes internal symlinks without following
    # them. The destination remains under the checked cache parent.
    if not shutil.rmtree.avoids_symlink_attacks:
        raise ValueError("This Python lacks symlink-safe rmtree")
    quarantine = path.with_name(".cleanup-" + uuid.uuid4().hex)
    path.rename(quarantine)
    try:
        shutil.rmtree(quarantine)
    except BaseException:
        if not path.exists():
            quarantine.rename(path)
        raise


def write_report(path, report):
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex)
    try:
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--apply", action="store_true", help="Delete the previewed stale caches"
    )
    parser.add_argument(
        "--keep-domain-builds", type=int, default=1, metavar="N"
    )
    parser.add_argument("--keep", action="append", default=[], type=Path)
    parser.add_argument(
        "--json", type=Path, help="Also write a machine-readable plan/result"
    )
    args = parser.parse_args()
    if args.keep_domain_builds < 1:
        parser.error("Keep at least one domain build per ABI")
    project = Path(__file__).resolve().parents[1]
    if (
        not (project / "core/CMakeLists.txt").is_file()
        or not (project / ".git").exists()
    ):
        parser.error("Run the script from its original Git checkout")
    report = None
    try:
        candidates, keep = plan(project, args.keep_domain_builds, args.keep)
        safe = []
        for path in candidates:
            try:
                validate_candidate(project, path)
                safe.append(path)
            except ValueError as error:
                keep[path] = str(error)
        sizes = allocated_bytes(safe)
        if args.json and any(inside(args.json.resolve(), p) for p in safe):
            raise ValueError("Report must be outside the caches to be removed")
        report = {
            "mode": "apply" if args.apply else "preview",
            "project": str(project),
            "reclaimable_bytes": sum(sizes.values()),
            "remove": [{"path": str(p), "bytes": sizes[p]} for p in safe],
            "keep": [
                {"path": str(p), "reason": why}
                for p, why in sorted(keep.items())
            ],
            "removed": [],
            "complete": False,
        }
        write_report(args.json, report)
        for path, reason in sorted(keep.items()):
            print("KEEP", path.relative_to(project), "-", reason, flush=True)
        for path in safe:
            print(
                "REMOVE",
                path.relative_to(project),
                f"{sizes[path] / 2**20:.1f} MiB",
                flush=True,
            )
        print(
            f"{report['mode']}: {len(safe)} roots, {sum(sizes.values()) / 2**30:.2f} GiB",
            flush=True,
        )
        if args.apply:
            lock = project / "core/build/cleanup.lock"
            lock.parent.mkdir(parents=True, exist_ok=True)
            with lock.open("w") as stream:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                for path in safe:
                    remove(project, path)
                    report["removed"].append(str(path))
                    write_report(args.json, report)
                    print("DELETED", path.relative_to(project), flush=True)
        else:
            print(
                "Nothing deleted. Repeat with --apply to remove these caches."
            )
        report["complete"] = True
        write_report(args.json, report)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        if report is not None:
            report["error"] = str(error)
            write_report(args.json, report)
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
