"""Exercise real result files, failure before ADB, and concurrent output use."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "tests"))
from evidence import RunEvidence

(PROJECT / "build").mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(
    dir=PROJECT / "build", prefix="evidence-test-"
) as temporary:
    root = Path(temporary)
    output = root / "results"
    output.mkdir()
    (output / "keep.txt").write_text("user-owned")
    with RunEvidence(output, group="coverage") as run:
        active = (output / "run-state.json").read_text()
        try:
            with RunEvidence(output):
                raise AssertionError("Concurrent output was accepted")
        except RuntimeError:
            pass
        assert (output / "run-state.json").read_text() == active
        run.finish({"result": "PASS"})
    old_id = json.loads((output / "summary.json").read_text())["run_id"]
    assert (
        json.loads((output / "run-state.json").read_text())["status"] == "PASS"
    )
    # Real runner failure before ABI discovery must invalidate that PASS.
    fakebin = root / "bin"
    fakebin.mkdir()
    adb = fakebin / "adb"
    adb.write_text("#!/bin/sh\necho 'device offline' >&2\nexit 1\n")
    adb.chmod(0o755)
    result = subprocess.run(
        [
            sys.executable,
            str(PROJECT / "tests/run.py"),
            "--group",
            "coverage",
            "--device",
            "fake-device",
            "--reuse-apk",
            "--output",
            str(output),
        ],
        capture_output=True,
        text=True,
        env={
            **os.environ,
            "PATH": str(fakebin) + os.pathsep + os.environ["PATH"],
        },
    )
    assert result.returncode and "device offline" in result.stderr
    assert not (output / "summary.json").exists()
    failed = json.loads((output / "run-state.json").read_text())
    assert failed["status"] == "FAILED"
    assert (
        json.loads(
            (Path(failed["prior_artifacts"]) / "summary.json").read_text()
        )["run_id"]
        == old_id
    )
    try:
        with RunEvidence(output) as run:
            run.finish({"result": "PASS"})
            raise ValueError("Failure after summary publication")
    except ValueError:
        pass
    assert not (output / "summary.json").exists()
    assert (output / "failed-summary.json").is_file()
    assert (
        json.loads((output / "run-state.json").read_text())["status"]
        == "FAILED"
    )
    assert (output / "keep.txt").read_text() == "user-owned"
    with RunEvidence(output) as run:
        run.finish({"result": "PASS"})
    assert (
        json.loads((output / "run-state.json").read_text())["run_id"]
        == json.loads((output / "summary.json").read_text())["run_id"]
    )
print(
    "Result lifecycle: PASS (exclusive output, archive, early failure, post-publication failure, recovery)"
)
