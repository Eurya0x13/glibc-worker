"""Verify pair identification, cache rechecks, wrong-ABI and mixed-image rejection."""

import copy
import json
from pathlib import Path
import tempfile

from jre_artifacts import PROJECT, identify, select, verify_home

checks = {}


def rejected(name, operation):
    try:
        operation()
    except (OSError, ValueError):
        checks[name] = True
    else:
        raise AssertionError("Unexpected artifact acceptance: " + name)


rejected("unknown_id", lambda: select("arm64-v8a", "unknown"))
rejected("wrong_abi", lambda: select("x86_64", "angelaura-21-arm64"))
rows = json.loads((PROJECT / "tests/config/android-jres.json").read_text())
verified = []
for row in rows:
    home = PROJECT / "vendor" / ("android-" + row["id"])
    if not home.exists() and row["id"].startswith("angelaura-"):
        home = (
            PROJECT
            / "vendor"
            / (row["id"].replace("angelaura-", "angelaura-jre"))
        )
    if home.exists():
        verify_home(home, row)
        assert identify(home, row["abi"])["id"] == row["id"]
        verified.append(row["id"])
        wrong = copy.deepcopy(row)
        wrong["libjvm"]["sha256"] = "0" * 64
        rejected("mixed_pair_" + row["id"], lambda: verify_home(home, wrong))
assert verified
with tempfile.TemporaryDirectory(
    dir=PROJECT / "build", prefix="artifact-negative-"
) as name:
    home = Path(name)
    row = select("arm64-v8a", "angelaura-21-arm64")
    # Even an apparently valid extraction marker cannot bypass file verification.
    (home / ".source-sha256").write_text(row["archive_sha256"])
    (home / "lib").mkdir()
    (home / "lib/libjava.so").write_bytes(b"corrupted")
    rejected("corrupt_extraction_cache", lambda: verify_home(home, row))
    rejected("unknown_bundle", lambda: identify(home, "arm64-v8a"))
    (home / "lib/libjava.so").unlink()
    (home / "lib/libjava.so").symlink_to(
        PROJECT / "tests/config/android-jres.json"
    )
    rejected("escaped_library", lambda: verify_home(home, row))
output = PROJECT / "build/aud10/artifact-tests.json"
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(
    json.dumps(
        {"result": "PASS", "verified_pairs": verified, "rejected": checks},
        indent=2,
    )
    + "\n"
)
print("Android JRE artifact identity: PASS")
