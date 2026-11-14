#!/usr/bin/env python3
"""Reproduce JRE8 CheckJNI JNI_COMMIT failure without loading the bridge.

Requires the test app to be installed and the pinned JRE cache prepared.
Each run deploys only its own temporary directory under the test app's UID.
A PASS means the known VM failure was reproduced, not strict JNI support.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evidence import RunEvidence
from jre_artifacts import PROJECT, select, verify_home
from target import PROFILES, find_ndk


def build_payload(abi, home, output):
    classes = output / "classes"
    classes.mkdir(exist_ok=True)
    fixture = PROJECT / "tests/adapter/array-control"
    subprocess.run(
        [
            "javac",
            "--release",
            "8",
            "-d",
            str(classes),
            str(fixture / "ArrayCommitControl.java"),
        ],
        check=True,
    )
    subprocess.run(
        ["jar", "cf", str(output / "control.jar"), "-C", str(classes), "."],
        check=True,
    )
    cc = (
        Path(find_ndk())
        / "toolchains/llvm/prebuilt/linux-x86_64/bin"
        / (PROFILES[abi]["ndk_triplet"] + "29-clang")
    )
    for source, library in (
        (fixture / "control.c", "libarray_control.so"),
        (
            PROJECT / "tests/native/harness/jre_exec_compat.c",
            "libjre_exec_compat.so",
        ),
    ):
        subprocess.run(
            [
                str(cc),
                "-shared",
                "-fPIC",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(output / library),
            ],
            check=True,
        )
    archive = output / "runtime.tar"
    with tarfile.open(archive, "w") as bundle:
        for directory in ("bin", "lib"):
            bundle.add(home / directory, arcname=directory)
        for name in (
            "control.jar",
            "libarray_control.so",
            "libjre_exec_compat.so",
        ):
            bundle.add(output / name, arcname=name)
    return archive


def run_modes(run_as, row, remote, output):
    base = remote + "/" + row["boot_library_path"]
    command = run_as + [
        "env",
        "JAVA_HOME=" + remote,
        "LD_LIBRARY_PATH=" + base + "/server:" + base + "/jli:" + base,
        "LD_PRELOAD=" + remote + "/libjre_exec_compat.so",
        "/system/bin/linker64",
        remote + "/bin/java",
        "-Xms16m",
        "-Xmx128m",
        "-Xint",
        "-XX:+UseSerialGC",
        "-XX:-UsePerfData",
        "-XX:ActiveProcessorCount=2",
        "-Djava.home=" + remote,
        "-Dsun.boot.library.path=" + base,
        "-cp",
        remote + "/control.jar",
    ]
    results = {}
    for mode in ("normal", "checkjni"):
        run = subprocess.run(
            command
            + (["-Xcheck:jni"] if mode == "checkjni" else [])
            + ["ArrayCommitControl", remote + "/libarray_control.so"],
            capture_output=True,
            text=True,
            timeout=45,
        )
        text = run.stdout + run.stderr
        (output / (mode + ".log")).write_text(text)
        results[mode] = {
            "exit": run.returncode,
            "committed_copy": "ARRAY_COMMIT_CONTROL stage=commit copy=1"
            in text,
            "round_trip": "ARRAY_COMMIT_CONTROL result=PASS" in text,
        }
    normal, strict = results["normal"], results["checkjni"]
    if (
        normal["exit"] != 0
        or not normal["round_trip"]
        or not strict["committed_copy"]
        or strict["exit"] != 134
        or strict["round_trip"]
    ):
        raise RuntimeError(
            "Expected normal success and independent CheckJNI abort not reproduced: "
            + json.dumps(results)
        )
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--artifact", default="angelaura-8-arm64")
    parser.add_argument(
        "--output", type=Path, default=PROJECT / "build/array-commit-control"
    )
    args = parser.parse_args()
    adb = ["adb", "-s", args.device]
    abi = subprocess.check_output(
        adb + ["shell", "getprop", "ro.product.cpu.abi"], text=True
    ).strip()
    row = select(abi, args.artifact)
    if row["major"] != 8:
        parser.error(
            "This regression control requires a reviewed JRE8 artifact"
        )
    home = PROJECT / "vendor" / ("android-" + row["id"])
    verify_home(home, row)
    output = args.output.resolve()
    with RunEvidence(
        output, kind="jre8-array-commit-control", artifact=row
    ) as attempt:
        archive = build_payload(abi, home, output)
        relative = "files/array-commit-" + attempt.state["run_id"]
        remote = "/data/user/0/dev.glibc.worker/" + relative
        remote_archive = (
            "/data/local/tmp/gw-array-commit-"
            + attempt.state["run_id"]
            + ".tar"
        )
        run_as = adb + ["shell", "run-as", "dev.glibc.worker"]
        subprocess.run(run_as + ["mkdir", "-p", relative], check=True)
        try:
            subprocess.run(
                adb + ["push", str(archive), remote_archive], check=True
            )
            subprocess.run(
                run_as + ["tar", "-xf", remote_archive, "-C", relative],
                check=True,
            )
            listing = subprocess.check_output(
                run_as + ["ls", "-la", relative], text=True
            )
            (output / "deployment.log").write_text(listing)
            for name in ("libjava", "libjvm"):
                actual = subprocess.check_output(
                    run_as + ["sha256sum", remote + "/" + row[name]["path"]],
                    text=True,
                ).split()[0]
                if actual != row[name]["sha256"]:
                    raise RuntimeError(
                        "Deployed control VM identity mismatch: " + name
                    )
            results = run_modes(run_as, row, remote, output)
            attempt.finish(
                {
                    "result": "PASS",
                    "meaning": "reproduced VM failure without glibc-worker",
                    "strict_jni_supported": False,
                    "modes": results,
                }
            )
        finally:
            subprocess.run(run_as + ["rm", "-rf", relative], check=True)
            subprocess.run(
                adb + ["shell", "rm", "-f", remote_archive], check=True
            )
    print(
        "Independent JRE8 JNI_COMMIT control: expected CheckJNI failure reproduced"
    )


if __name__ == "__main__":
    main()
