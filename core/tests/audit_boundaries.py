"""Exercise stack validation and real request/reply cycles at sequence wrap."""

import json
import os
from pathlib import Path
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
output = PROJECT / "build/test-work/audit-boundaries"
output.mkdir(parents=True, exist_ok=True)
(output / "summary.json").unlink(missing_ok=True)
rows = []
for name, sources, modes in (
    (
        "bootstrap_stack",
        ["src/bootstrap/loader_image.c", "tests/native/bootstrap_stack.c"],
        ({},),
    ),
    (
        "rpc_sequence",
        ["tests/native/rpc_sequence.c"],
        (
            {"GW_RPC_READY_POLL": ready, "GW_FUTEX_WAKE_FILTER": wake}
            for ready in (0, 1)
            for wake in (0, 1)
        ),
    ),
):
    for index, mode in enumerate(modes):
        binary = output / (name + "-" + str(index))
        command = [
            os.environ.get("SANITIZER_CC", "clang"),
            "-std=gnu17",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-pthread",
            "-I" + str(PROJECT / "include"),
            "-I" + str(PROJECT / "src/bootstrap"),
            *("-D" + k + "=" + str(v) for k, v in mode.items()),
            *(str(PROJECT / p) for p in sources),
            "-o",
            str(binary),
        ]
        subprocess.run(command, check=True)
        run = subprocess.run(
            [str(binary)],
            capture_output=True,
            text=True,
            timeout=90,
            env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
        )
        (output / (binary.name + ".log")).write_text(run.stdout + run.stderr)
        if run.returncode:
            raise SystemExit("Audit boundary failed: " + str(binary))
        row = json.loads(run.stdout)
        assert row["result"] == "PASS"
        rows.append({"test": name, "mode": mode, **row})
(output / "summary.json").write_text(
    json.dumps({"result": "PASS", "runs": rows}, indent=2) + "\n"
)
print("Bootstrap and RPC boundary checks: PASS")
