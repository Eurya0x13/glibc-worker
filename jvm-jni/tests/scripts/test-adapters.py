#!/usr/bin/env python3
"""Exercise production JVM hooks inside real desktop VMs, without dual libc.

--pinned uses the SHA256-pinned Linux x64 JREs in config/desktop-jres.json.
--runtime FAMILY:HOME adds an explicitly reviewed local VM. Neither identifies
the loader contract from the VM's major version: a wrong family must fail closed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

PROJECT = Path(__file__).resolve().parents[2]
ROOT = PROJECT.parent


def run(command, **kwargs):
    result = subprocess.run(
        list(map(str, command)),
        check=True,
        text=True,
        capture_output=True,
        **kwargs,
    )
    return result


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def pinned_runtimes():
    families = {8: 1, 21: 6, 25: 6}
    for item in json.loads(
        (PROJECT / "tests/config/desktop-jres.json").read_text()
    ):
        archive = PROJECT / "vendor" / item["archive"]
        archive.parent.mkdir(exist_ok=True)
        if not archive.is_file() or digest(archive) != item["sha256"]:
            partial = archive.with_suffix(".partial")
            with urllib.request.urlopen(item["url"], timeout=120) as response:
                with partial.open("wb") as output:
                    shutil.copyfileobj(response, output)
            if digest(partial) != item["sha256"]:
                partial.unlink()
                raise SystemExit(
                    "Downloaded JRE SHA256 mismatch: " + item["archive"]
                )
            partial.replace(archive)
        home = PROJECT / "vendor" / ("desktop-jre-" + str(item["major"]))
        if not home.is_dir():
            home.mkdir()
            with tarfile.open(archive) as source:
                source.extractall(home, filter="data")
        yield families[item["major"]], home


def java_image(home):
    if not home.is_dir():
        raise SystemExit("JRE directory does not exist: " + str(home))
    java = home / "bin/java"
    if not java.is_file():
        candidates = list(home.glob("*/bin/java"))
        if len(candidates) != 1:
            raise SystemExit("Expected one bin/java under " + str(home))
        java = candidates[0]
    home = java.parent.parent.resolve()
    libraries = list(home.rglob("libjava.so"))
    if len(libraries) != 1:
        raise SystemExit("Expected one libjava.so under " + str(home))
    return java.resolve(), libraries[0].resolve()


def build_tests(output, jdk):
    include = jdk / "include"
    if (
        not (include / "jni.h").is_file()
        or not (include / "linux/jni_md.h").is_file()
    ):
        raise SystemExit("Desktop JDK JNI headers missing under " + str(jdk))
    (output / "jre-adapter-profile.h").write_text(
        '#define GW_JVM_PROFILE_NAME "unconfigured"\n'
        '#define GW_JVM_LIBJAVA_SHA256 ""\n'
        '#define GW_JVM_LIBJVM_SHA256 ""\n'
        "#define GW_JVM_PROFILE_FAMILY GW_JVM_NATIVE_LIBRARIES_15\n"
    )
    common = [
        os.environ.get("CC", "cc"),
        "-shared",
        "-fPIC",
        "-g",
        "-O1",
        "-std=gnu11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I" + str(include),
        "-I" + str(include / "linux"),
    ]
    sources = [
        PROJECT / "src/native" / (name + ".c")
        for name in ("adapter", "profiles", "calls", "families")
    ]
    sources += [
        ROOT / "bionic-dlopen/src/loader.c",
        ROOT / "bionic-dlopen/src/elf_route.c",
        PROJECT / "tests/adapter/native/backend.c",
    ]
    dirs = [
        PROJECT / "include",
        PROJECT / "src/native",
        ROOT / "bionic-dlopen/include",
        ROOT / "core/include",
        ROOT / "android-jni/include",
        output,
    ]
    command = [
        *common,
        *("-I" + str(path) for path in dirs),
        *sources,
        "-ldl",
        "-pthread",
        "-o",
        output / "libadapter_control.so",
    ]
    run(command)
    fixture = PROJECT / "tests/adapter/native/fixture.c"
    run([*common, fixture, "-o", output / "libadapter_fixture.so"])
    run(
        [
            *common,
            "-DREJECT_VERSION=JNI_ERR",
            fixture,
            "-o",
            output / "libadapter_fixture_reject.so",
        ]
    )
    shutil.copyfile(
        output / "libadapter_fixture.so", output / "libadapter_ordinary.so"
    )
    classes, fixtures = output / "classes", output / "fixture-classes"
    for directory in (classes, fixtures):
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir()
    java = [
        PROJECT / "tests/adapter/java" / (name + ".java")
        for name in ("AdapterTest", "AdapterBaseline")
    ]
    java += [
        PROJECT / "src/java/dev/glibc/worker" / (name + ".java")
        for name in ("JvmAdapter",)
    ]
    java += [
        ROOT / "android-jni/src/java/dev/glibc/worker" / (name + ".java")
        for name in ("BridgeTypes", "GuestImages")
    ]
    run([jdk / "bin/javac", "--release", "8", "-d", classes, *java])
    run(
        [
            jdk / "bin/javac",
            "--release",
            "8",
            "-d",
            fixtures,
            PROJECT / "tests/adapter/java/AdapterFixture.java",
        ]
    )
    run([jdk / "bin/jar", "cf", output / "fixture.jar", "-C", fixtures, "."])
    return list(map(str, command))


def execute_case(output, prefix, arguments, name):
    result = output / (name + ".json")
    result.unlink(missing_ok=True)
    arguments = list(arguments)
    arguments[7] = result
    command = [*prefix, "AdapterTest", *arguments]
    baseline_command = [*prefix, "AdapterBaseline", *arguments]
    baseline = run(baseline_command, timeout=30)
    (output / (name + "-baseline.log")).write_text(
        baseline.stdout + baseline.stderr
    )
    completed = subprocess.run(
        list(map(str, command)), text=True, capture_output=True, timeout=180
    )
    log = output / (name + ".log")
    log.write_text(completed.stdout + completed.stderr)
    if completed.returncode or not result.is_file():
        raise RuntimeError("VM adapter test failed; see " + str(log))
    evidence = json.loads(result.read_text())
    if evidence.get("result") != "PASS" or not all(evidence["checks"].values()):
        raise RuntimeError("Invalid structured adapter result: " + str(result))
    # Some JRE8 launchers warn once per command argument, before entering main.
    # Require an identical no-adapter launcher baseline; no runtime JNI warning
    # is ignored. Finalization is asynchronous (no System.runFinalization JNI).
    before, after = completed.stdout.split("adapter main\n", 1)
    if before != baseline.stdout.split("adapter baseline main\n", 1)[0]:
        raise RuntimeError(
            "Unexpected JNI warning before main; see " + str(log)
        )
    if any(
        marker in after + completed.stderr
        for marker in (
            "WARNING in native method",
            "FATAL ERROR in native method",
            "WARNING: JNI local refs",
        )
    ):
        raise RuntimeError(
            "CheckJNI reported invalid JNI usage; see " + str(log)
        )
    return {
        "evidence": evidence,
        "log": str(log),
        "checkjni_runtime_warnings": 0,
        "launcher_baseline_warnings": before.count("WARNING in native method"),
    }


def test_runtime(output, family, home, index):
    java, library = java_image(home)
    vms = list(java.parent.parent.rglob("libjvm.so"))
    if len(vms) != 1:
        raise SystemExit(
            "Expected one libjvm.so under " + str(java.parent.parent)
        )
    vm_digest = digest(vms[0])
    version = run([java, "-version"]).stderr.strip()
    modern = 'version "24' in version or 'version "25' in version
    options = ["--enable-native-access=ALL-UNNAMED"] if modern else []
    prefix = [
        java,
        *options,
        "-Xmixed",
        "-Xcheck:jni",
        "-XX:+UseSerialGC",
        "-cp",
        output / "classes",
    ]
    arguments = [
        output / "libadapter_control.so",
        library,
        digest(library),
        str(family),
        output / "fixture.jar",
        output / "libadapter_fixture.so",
        output / "libadapter_fixture_reject.so",
        "RESULT",
        output / "libadapter_ordinary.so",
        vm_digest,
    ]
    normal = execute_case(output, prefix, arguments, "runtime-" + str(index))
    poisoned = execute_case(
        output,
        prefix,
        [*arguments, "rollback-failure"],
        "runtime-" + str(index) + "-rollback-failure",
    )
    return {
        "java": str(java),
        "version": version,
        "family": family,
        "libjava_sha256": digest(library),
        "libjvm_sha256": vm_digest,
        "normal": normal,
        "poisoned": poisoned,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--runtime", action="append", default=[], metavar="FAMILY:HOME"
    )
    parser.add_argument(
        "--pinned", action="store_true", help="fetch/use pinned JRE8,21,25"
    )
    parser.add_argument(
        "--jdk",
        type=Path,
        default=Path(shutil.which("javac") or "/missing/javac")
        .resolve()
        .parents[1],
    )
    parser.add_argument(
        "--output", type=Path, default=PROJECT / "build/adapter-tests"
    )
    args = parser.parse_args()
    runtimes = [
        (int(value.split(":", 1)[0]), Path(value.split(":", 1)[1]).resolve())
        for value in args.runtime
    ]
    if args.pinned:
        runtimes = [*pinned_runtimes(), *runtimes]
    if not runtimes:
        parser.error("Specify --pinned or at least one --runtime FAMILY:HOME")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    summary = {
        "scope": "desktop JVM adapter contract; no dual libc",
        "results": [],
    }
    try:
        summary["build"] = build_tests(output, args.jdk.resolve())
        for index, (family, home) in enumerate(runtimes):
            evidence = test_runtime(output, family, home, index)
            summary["results"].append(evidence)
            print(evidence["version"].splitlines()[0] + ": PASS", flush=True)
        summary["result"] = "PASS"
    except (RuntimeError, subprocess.SubprocessError) as error:
        summary["result"] = "FAIL"
        summary["error"] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            summary["diagnostic"] = error.stdout + error.stderr
            print(summary["diagnostic"], flush=True)
        raise
    finally:
        (output / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n"
        )


if __name__ == "__main__":
    main()
