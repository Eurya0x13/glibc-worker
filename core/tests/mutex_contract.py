"""Check native robust recovery and an explicitly selected GNU runtime policy."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--output", type=Path, default=project / "build/test-work/mutex-contract"
)
parser.add_argument("--runtime", type=Path)
parser.add_argument("--guest-cc")
parser.add_argument("--loader", help="Loader basename in the selected runtime")
parser.add_argument(
    "--emulator",
    help="Optional explicit emulator executable, such as qemu-aarch64",
)
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / "summary.json").unlink(missing_ok=True)
source = project / "tests/native/mutex_contract.c"


def compiler(name):
    path = shutil.which(name)
    if not path:
        raise SystemExit("Missing compiler or emulator: " + name)
    return path


def compile_probe(cc, binary, sanitize=False):
    command = [
        compiler(cc),
        "-std=gnu17",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DGW_MUTEX_STANDALONE",
        str(source),
        "-pthread",
        "-o",
        str(binary),
    ]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)


def probe(name, command, policy):
    run = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=30,
        env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"},
    )
    (out / (name + ".log")).write_text(run.stdout + run.stderr)
    matches = re.findall(
        r"^MUTEX_CONTRACT (\{[^\n]+\})$", run.stdout, re.MULTILINE
    )
    if run.returncode or len(matches) != 1:
        raise SystemExit("Mutex contract failed: " + str(out / (name + ".log")))
    row = json.loads(matches[0])
    assert (
        row["result"] == "PASS"
        and row["policy"] == policy
        and row["failures"] == 0
    )
    if policy == "reject":
        assert row["robust_rejected"] == 12 and row["robust_accepted"] == 0
    else:
        assert row["robust_accepted"] == 12 and row["owner_dead"] == 2
    assert row["owner_timed_out"] == 0
    return row


reference = out / "native-reference"
compile_probe(os.environ.get("SANITIZER_CC", "clang"), reference, sanitize=True)
result = {
    "native_reference": probe("native-reference", [str(reference)], "recover")
}
if args.runtime:
    if (
        not args.guest_cc
        or not args.loader
        or Path(args.loader).name != args.loader
    ):
        parser.error(
            "--runtime requires --guest-cc and a loader basename via --loader"
        )
    runtime = args.runtime.resolve()
    loader = runtime / args.loader
    if not loader.is_file() or not (runtime / "libc.so.6").is_file():
        parser.error("Runtime must contain its matching loader and libc.so.6")
    binary = out / "runtime-probe"
    compile_probe(args.guest_cc, binary)
    command = [
        str(loader),
        "--library-path",
        str(runtime),
        str(binary),
        "--reject",
    ]
    if args.emulator:
        command.insert(0, compiler(args.emulator))
    result["runtime"] = probe("runtime", command, "reject")
    result["runtime"]["execution"] = (
        "emulator" if args.emulator else "native process"
    )
    result["runtime"]["dual_libc"] = False
    result["runtime"]["sha256"] = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (loader, runtime / "libc.so.6")
    }
    result["runtime"]["command"] = shlex.join(command)
result["result"] = "PASS"
result["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
(out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
print("Mutex contract: PASS (Android dual-libc validation runs separately)")
