"""Check paired timestamps, publication, nesting and coherent live export."""

import json
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
output = root / "build/test-work/rpc-handoff"
output.mkdir(parents=True, exist_ok=True)
results = []
for profile in (0, 1):
    binary = output / ("profile-" + str(profile))
    subprocess.run(
        [
            os.environ.get("SANITIZER_CC", "clang"),
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-pthread",
            "-DGW_RPC_PROFILE=" + str(profile),
            "-I" + str(root / "include"),
            str(root / "tests/native/rpc_handoff.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=90, check=True
    )
    (output / ("profile-" + str(profile) + ".txt")).write_text(
        result.stdout + result.stderr
    )
    if "result=PASS" not in result.stdout:
        raise SystemExit("Handoff check did not complete")
    print(result.stdout.strip())
    results.append({"profile": profile, "result": "PASS"})
(output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
