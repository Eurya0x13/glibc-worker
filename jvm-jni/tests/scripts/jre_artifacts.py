"""Explicit Android artifact pins; never infer a loader family from a version."""

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import struct

PROJECT = Path(__file__).resolve().parents[2]
CATALOG = PROJECT / "tests/config/android-jres.json"


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def select(abi, artifact=None):
    artifact = (
        artifact
        or os.environ.get("GW_JRE_ARTIFACT")
        or "pojav-17-ec28559-" + abi
    )
    rows = [
        row for row in json.loads(CATALOG.read_text()) if row["id"] == artifact
    ]
    if len(rows) != 1:
        raise ValueError(
            "Unknown or ambiguous Android JRE artifact: " + artifact
        )
    row = rows[0]
    if row["abi"] != abi:
        raise ValueError("Android JRE artifact ABI mismatch: " + artifact)
    if row["family"] not in range(1, 7) or row["startup"] not in (
        "saved-jvm-path",
        "pojav-layout",
    ):
        raise ValueError("Unreviewed JRE contract or startup strategy")
    for key in ("libjava", "libjvm"):
        path = PurePosixPath(row[key]["path"])
        if path.is_absolute() or ".." in path.parts:
            raise ValueError("Invalid artifact-relative library path")
    return row


def verify_home(home, row):
    """Recheck extracted caches too; a matching extraction marker is insufficient."""
    home = Path(home).resolve()
    machine = {"arm64-v8a": 183, "x86_64": 62}[row["abi"]]
    for key in ("libjava", "libjvm"):
        file = (home / row[key]["path"]).resolve()
        if not file.is_relative_to(home) or digest(file) != row[key]["sha256"]:
            raise ValueError("Android JRE artifact mismatch: " + key)
        with file.open("rb") as stream:
            header = stream.read(20)
        if (
            len(header) != 20
            or header[:6] != b"\x7fELF\x02\x01"
            or struct.unpack_from("<H", header, 18)[0] != machine
        ):
            raise ValueError("Android JRE ELF architecture mismatch: " + key)
    return row


def identify(home, abi):
    matches = []
    for row in json.loads(CATALOG.read_text()):
        if row["abi"] != abi:
            continue
        try:
            verify_home(home, row)
        except (OSError, ValueError):
            continue
        matches.append(row)
    if len(matches) != 1:
        raise ValueError("Unknown, mixed or ambiguous Android JRE artifact")
    return matches[0]


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", choices=("arm64-v8a", "x86_64"), required=True)
    parser.add_argument("--home", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(identify(args.home, args.abi), indent=2))
