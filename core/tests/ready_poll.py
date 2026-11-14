"""Exercise a paused publisher and response generations under ASan/UBSan."""

import json
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
output = root / "build/test-work/ready-poll"
output.mkdir(parents=True, exist_ok=True)
results = []
for ready in (0, 1):
    for profile in (0, 1):
        for spin in (0, 20000):
            label = f"ready{ready}-profile{profile}-spin{spin}"
            binary = output / label
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
                    "-DGW_RPC_READY_POLL=" + str(ready),
                    "-DGW_RPC_PROFILE=" + str(profile),
                    "-DGW_RPC_SPIN_NS=" + str(spin),
                    "-I" + str(root / "include"),
                    str(root / "tests/native/ready_poll.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            run = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            (output / (label + ".txt")).write_text(run.stdout + run.stderr)
            if "result=PASS" not in run.stdout:
                raise SystemExit("Ready-poll check did not complete")
            print(run.stdout.strip())
            results.append(
                {
                    "ready": ready,
                    "profile": profile,
                    "spin": spin,
                    "result": "PASS",
                }
            )
(output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
