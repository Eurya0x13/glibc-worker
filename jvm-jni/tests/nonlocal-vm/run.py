"""Document out-of-scope managed-frame transfers without loading the bridge."""

import argparse
import hashlib
import json
import resource
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--jdk", required=True, type=Path)
args = parser.parse_args()
jdk = args.jdk.resolve(strict=True)
source = Path(__file__).resolve().parent
out = source.parents[1] / "build/tests/nonlocal-vm"
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
libjvm = jdk / "lib/server/libjvm.so"
assert libjvm.is_file()
commands = [
    [
        str(jdk / "bin/javac"),
        "--release",
        "8",
        "-d",
        str(out),
        str(source / "ManagedBoundary.java"),
    ],
    [
        "cc",
        "-std=gnu17",
        "-O2",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-UNDEBUG",
        "-I" + str(jdk / "include"),
        "-I" + str(jdk / "include/linux"),
        str(source / "host.c"),
        "-ldl",
        "-pthread",
        "-o",
        str(out / "host"),
    ],
]
for command in commands:
    subprocess.run(command, check=True)
rows = []
for engine in ("int", "default"):
    for mode in ("normal", "exception", "jump", "context"):
        command = [str(out / "host"), str(libjvm), str(out), mode, engine]
        commands.append(command)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run(
            command, cwd=out, capture_output=True, text=True, timeout=15
        )
        (out / f"{engine}-{mode}.log").write_text(result.stdout + result.stderr)
        result.check_returncode()
        observations = [
            json.loads(line)
            for line in result.stdout.splitlines()
            if line.startswith("{")
        ]
        assert len(observations) == 1
        row = observations[0]
        assert row["mode"] == mode and row["engine"] == engine
        assert (
            row["bridge_present"] is False
            and row["native_target_reached"] is True
        )
        assert row["waiter_attempted"] is True
        unsafe = mode in ("jump", "context")
        assert row["finally_count"] == (0 if unsafe else 1)
        assert row["monitor_acquired"] is (not unsafe)
        assert row["monitor_blocked"] is unsafe
        assert row["vm_destroyed"] is (not unsafe)
        row["result"] = (
            "OBSERVED_UNSUPPORTED_FRAME_BYPASS"
            if unsafe
            else "PASS_NATIVE_CONTROL"
        )
        rows.append(row)
        print(json.dumps(row), flush=True)
files = [
    *source.glob("*.*"),
    libjvm,
    out / "host",
    out / "ManagedBoundary.class",
]
(out / "manifest.json").write_text(
    json.dumps(
        {
            "commands": commands,
            "sha256": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in files
            },
        },
        indent=2,
    )
    + "\n"
)
(out / "summary.json").write_text(
    json.dumps(
        {
            "result": "OBSERVED_MANAGED_FRAME_BOUNDARY",
            "scope": "OUTSIDE_NATIVE_RECOVERY_CONTRACT",
            "blocks_native_recovery": False,
            "bridge_compatibility": "NOT_ESTABLISHED",
            "observations": rows,
        },
        indent=2,
    )
    + "\n"
)
