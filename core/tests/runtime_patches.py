"""Regression: an enclosing Git repository must not silently suppress patches."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
output = PROJECT / "build/test-work/runtime-patches"
output.mkdir(parents=True, exist_ok=True)
checks = []


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


with tempfile.TemporaryDirectory(dir=output) as temporary:
    base = Path(temporary)
    repository = base / "repo"
    repository.mkdir()
    subprocess.run(["git", "init", "-q", str(repository)], check=True)
    (repository / ".gitignore").write_text("cache/\n")
    nested = repository / "component"
    nested.mkdir()
    patch = base / "runtime.patch"
    patch.write_text(
        "--- a/start.c\n+++ b/start.c\n@@ -1 +1 @@\n"
        "-register_forbidden_syscall();\n+skip_forbidden_syscall();\n"
    )
    for name, source in (
        ("ignored-source-in-checkout", repository / "cache/source"),
        ("standalone-source", base / "standalone"),
    ):
        source.mkdir(parents=True)
        target = source / "start.c"
        target.write_text("register_forbidden_syscall();\n")
        command = [
            "python3",
            str(PROJECT / "scripts/apply-patches.py"),
            "--source",
            str(source),
            str(patch),
        ]
        poisoned = {
            **os.environ,
            "GIT_DIR": str(repository / ".git"),
            "GIT_WORK_TREE": str(repository),
        }
        applied = run(command, cwd=nested, env=poisoned)
        assert applied.returncode == 0, applied.stderr
        assert target.read_text() == "skip_forbidden_syscall();\n"
        checks.append(name)
        verified = run([*command, "--verify-only"], cwd=nested, env=poisoned)
        assert verified.returncode == 0, verified.stderr
        checks.append(name + "-verified")
        again = run(command, cwd=nested, env=poisoned)
        assert again.returncode != 0
        assert target.read_text() == "skip_forbidden_syscall();\n"
        checks.append(name + "-double-apply-rejected")
        target.write_text("corrupt_source();\n")
        assert run([*command, "--verify-only"], cwd=nested).returncode != 0
        checks.append(name + "-corrupt-cache-rejected")

(output / "summary.json").write_text(
    json.dumps({"result": "PASS", "checks": checks}, indent=2) + "\n"
)
print(f"Runtime patches: {len(checks)} regression checks passed")
