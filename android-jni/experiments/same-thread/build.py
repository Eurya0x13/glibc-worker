"""Build the isolated production-JNI transport experiment; no SDK installation."""

import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
from pathlib import Path

source = Path(__file__).resolve().parent
project = source.parents[1]
root = project.parent
core = root / "core/src/domain"
core_fixture = root / "core/experiments/same-thread"
jvm = root / "jvm-jni/experiments/same-thread"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=["x86_64", "arm64-v8a"], required=True)
parser.add_argument("--ndk", type=Path, required=True)
parser.add_argument("--jdk", type=Path, required=True)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--system-load", action="store_true")
parser.add_argument("--late-signal", action="store_true")
parser.add_argument("--art-signal-profile", type=Path)
parser.add_argument("--jvm-family", type=int, choices=range(1, 7))
args = parser.parse_args()
if args.late_signal and not args.system_load:
    parser.error("Late signal fixture requires --system-load")
android = args.abi == "arm64-v8a"
if args.art_signal_profile and not (android and args.late_signal):
    parser.error("ART signal profile requires ARM64 late loading")
out = (
    root
    / (
        "jvm-jni/build/experiments/late-system-load"
        if args.late_signal
        else "jvm-jni/build/experiments/system-load"
    )
    if args.system_load
    else project / "build/experiments/same-thread"
) / args.abi
out.mkdir(parents=True, exist_ok=True)
subprocess.run(
    [
        "python3",
        str(root / "jvm-jni/tests/scripts/gen-fixture.py"),
        "--output",
        str(out / "conformance"),
    ],
    check=True,
)
jni = out / "jni"
jni.mkdir(exist_ok=True)
shutil.copyfile(
    args.ndk
    / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h",
    jni / "jni.h",
)
subprocess.run(
    [
        "python3",
        str(project / "scripts/gen-jni.py"),
        "--jni-header",
        str(jni / "jni.h"),
        "--output",
        str(out / "generated"),
    ],
    check=True,
)
spec = importlib.util.spec_from_file_location(
    "host_build", project / "tests/host_build.py"
)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
helper.build(
    out / "host_store.o", "aarch64-linux-android" if android else "native"
)
includes = [
    jni,
    source,
    core,
    jvm,
    project / "include",
    project / "src/common",
    project / "src/host",
    project / "src/guest",
    out,
    root / "core/include",
    root / "core/src/bootstrap",
    root / "bionic-dlopen/include",
    root / "bionic-dlopen/src",
]
flags = [
    "-std=gnu17",
    "-O2",
    "-g",
    "-fPIC",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fstack-protector-strong",
    "-DGW_DOMAIN_TEST_SIGNALS",
    "-DGW_DOMAIN_JNI",
    "-DGW_DOMAIN_CHAIN",
    "-DGW_DOMAIN_SIGNALS",
    "-DGW_RPC_SPIN_NS=20000",
    "-DGW_RPC_PROFILE=0",
    *["-I" + str(p) for p in includes],
]
host_cc = (
    str(
        args.ndk
        / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang"
    )
    if android
    else "clang"
)
guest_cc = "aarch64-linux-gnu-gcc" if android else "gcc"
guest_ffi = root / "core/build/ffi-linux" / args.abi
host_ffi = (
    root / "core/build" / ("ffi-android" if android else "ffi-linux") / args.abi
)
host_sources = [
    source / "host.c",
    jvm / "vm-start.c",
    core / "switch.S",
    core / "start.S",
    core / "signals.c",
    core / "signal-entry.S",
    core / "signal-pending.c",
    core / "signal-deferred.c",
    core / "signal-stack.S",
    core / "signal-router.c",
    core / "signal-relay.c",
    core / "signal-interpose.c",
    root / "core/src/bootstrap/clone.S",
    root / "core/src/bootstrap/loader_image.c",
    root / "core/src/domain/signal-contract.c",
    root / "bionic-dlopen/src/loader.c",
    root / "bionic-dlopen/src/elf_route.c",
    *sorted((project / "src/host").glob("*.c")),
]
host_flags = ["-Wl,--export-dynamic", "-I" + str(host_ffi / "include")]
profile = None
if android:
    assets = root / "jvm-jni/build/tests/android/assets/android-jre"
    libs = root / "jvm-jni/build/tests/android/jniLibs/arm64-v8a"
    profile = json.loads((assets / "jre-artifact.json").read_text())
    header = root / "jvm-jni/build/native/jre-layout.h"
    if profile["major"] != 17:
        parser.error("Pinned JRE17 required")
    for name in ["libjvm", "libjava"]:
        digest = hashlib.sha256(
            (libs / Path(profile[name]["path"]).name).read_bytes()
        ).hexdigest()
        if (
            digest != profile[name]["sha256"]
            or digest not in header.read_text()
        ):
            parser.error("JRE profile mismatch")
    shutil.copyfile(header, out / header.name)
    harness = root / "jvm-jni/tests/native/harness"
    host_sources += [
        harness / "jre_layout_spike.c",
        harness / "jre_exec_compat.c",
    ]
    host_flags += ["-fno-emulated-tls", "-I" + str(harness)]
if args.system_load:
    flags += [
        "-DGW_EXPERIMENT_SYSTEM_LOAD",
        "-I" + str(root / "jvm-jni/include"),
        "-I" + str(root / "jvm-jni/src/native"),
    ]
    host_sources += [
        jvm / "system-load.c",
        *[
            root / "jvm-jni/src/native" / (n + ".c")
            for n in ("adapter", "profiles", "families", "calls")
        ],
    ]
    # Explicitly reviewed JRE17 family; Android may carry an upstream backport.
    if android:
        family = profile["family"]
        java_hash = profile["libjava"]["sha256"]
        vm_hash = profile["libjvm"]["sha256"]
    else:
        if args.jvm_family is None:
            parser.error(
                "Desktop System.load requires an explicitly reviewed --jvm-family"
            )
        family = args.jvm_family
        java_hash = hashlib.sha256(
            (args.jdk / "lib/libjava.so").read_bytes()
        ).hexdigest()
        vm_hash = hashlib.sha256(
            (args.jdk / "lib/server/libjvm.so").read_bytes()
        ).hexdigest()
    (out / "jre-adapter-profile.h").write_text(
        '#define GW_JVM_PROFILE_NAME "same-thread-explicit-jre17"\n'
        + f"#define GW_JVM_PROFILE_FAMILY {family}\n"
        + f'#define GW_JVM_LIBJAVA_SHA256 "{java_hash}"\n'
        + f'#define GW_JVM_LIBJVM_SHA256 "{vm_hash}"\n'
    )
commands = []


def compile(cc, target, files, extra):
    command = [
        cc,
        *flags,
        *map(str, files),
        *map(str, extra),
        "-ldl",
        "-pthread",
        "-lm",
        "-o",
        str(out / target),
    ]
    commands.append(command)
    subprocess.run(command, check=True)


if args.late_signal:
    vm_file = (
        libs / "libjvm.so" if android else args.jdk / "lib/server/libjvm.so"
    )
    relocations = subprocess.check_output(
        ["readelf", "-rW", str(vm_file)], text=True
    )
    entries = []
    for row in relocations.splitlines():
        columns = row.split()
        if len(columns) >= 5 and columns[4].split("@")[0] in {
            "sigaction",
            "sigaction64",
            "signal",
            "bsd_signal",
        }:
            entries.append(columns)
    if (
        len(entries) != 1
        or entries[0][4].split("@")[0] != "sigaction"
        or entries[0][2]
        != ("R_AARCH64_JUMP_SLOT" if android else "R_X86_64_JUMP_SLOT")
    ):
        parser.error("Review this VM's signal imports before adding a profile")
    offset = int(entries[0][0], 16)
    symbol_table = subprocess.run(
        ["nm", "-a", str(vm_file)], capture_output=True, text=True, check=False
    ).stdout
    cache = [
        line.split()[0]
        for line in symbol_table.splitlines()
        if line.endswith(" _ZZL20check_signal_handleriE12os_sigaction")
    ]
    lookup = [
        line.split()[0]
        for line in relocations.splitlines()
        if len(line.split()) >= 5 and line.split()[4].split("@")[0] == "dlsym"
    ]
    if len(cache) != 1 or len(lookup) != 1:
        parser.error(
            "Late signal profile requires reviewed HotSpot query cache and dlsym import"
        )
    (out / "late-signal-profile.h").write_text(
        f"#define GW_LATE_SIGACTION_OFFSET 0x{offset:x}UL\n"
        f"#define GW_LATE_DLSYM_OFFSET 0x{int(lookup[0], 16):x}UL\n"
        f"#define GW_LATE_QUERY_CACHE_OFFSET 0x{int(cache[0], 16):x}UL\n"
        f'#define GW_LATE_JVM_SHA256 "{vm_hash}"\n'
    )
    if args.art_signal_profile:
        art_profile = json.loads(args.art_signal_profile.read_text())
        if art_profile["abi"] != "arm64-v8a" or art_profile["schema"] != 1:
            parser.error("Unsupported ART signal profile")
        with (out / "late-signal-profile.h").open("a") as header:
            header.write(
                f'#define GW_ART_SIGCHAIN_SHA256 "{art_profile["sha256"]}"\n'
            )
            for name in ("sigaction", "sigaction64"):
                offset = art_profile["bottom_callbacks"][name]
                header.write(
                    f"#define GW_ART_{name.upper()}_OFFSET {offset}UL\n"
                )
    # The launcher has no router exports. All bridge code is dlopened only
    # after JNI_CreateJavaVM has completed, not linked into the launcher.
    launcher = [jvm / "late-host.c", jvm / "vm-start.c"]
    if android:
        launcher += [
            harness / "jre_layout_spike.c",
            harness / "jre_exec_compat.c",
        ]
    compile(host_cc, "host", launcher, host_flags)
    if android:
        compile(
            host_cc,
            "inline-host.so",
            launcher,
            [*host_flags, "-shared", "-DGW_INLINE_VM"],
        )

    host_sources = [p for p in host_sources if p not in launcher]
    host_sources += [root / "jvm-jni/src/native/signal_profile.c"]
    compile(
        host_cc,
        "late-bridge.so",
        host_sources,
        [
            *host_flags,
            "-shared",
            "-Wl,-Bsymbolic-functions",
            "-DGW_EXPERIMENT_LATE_SIGNAL",
            out / "host_store.o",
            host_ffi / "lib/libffi.a",
        ],
    )
else:
    compile(
        host_cc,
        "host",
        host_sources,
        [*host_flags, out / "host_store.o", host_ffi / "lib/libffi.a"],
    )

guest_sources = [
    project / "src/guest/domain_service.c",
    project / "src/guest/full_guest.c",
    project / "src/guest/rpc.c",
    project / "src/guest/guest_loader.c",
]
compile(
    guest_cc,
    "guest",
    guest_sources,
    ["-I" + str(guest_ffi / "include"), guest_ffi / "lib/libffi.a"],
)
compile(guest_cc, "probe.so", [source / "module.c"], ["-shared"])
compile(
    guest_cc,
    "conformance.so",
    [project / "tests/native/guest_conformance.c"],
    ["-shared", "-I" + str(out / "conformance")],
)
if args.system_load:
    compile(
        guest_cc,
        "system-load.so",
        [jvm / "system-load-fixture.c"],
        ["-shared", "-Wl,--no-as-needed", "-lc"],
    )
    compile(
        guest_cc,
        "system-reject.so",
        [jvm / "system-load-fixture.c"],
        ["-shared", "-DREJECT_VERSION=JNI_ERR", "-Wl,--no-as-needed", "-lc"],
    )
    compile(
        host_cc,
        "system-ordinary.so",
        [jvm / "system-load-fixture.c"],
        ["-shared", "-Wl,--no-as-needed", "-lc"],
    )
unwind = Path(
    subprocess.check_output(
        [guest_cc, "-print-file-name=libgcc_s.so.1"], text=True
    ).strip()
)
shutil.copyfile(unwind, out / "libgcc_s.so.1")
classes = out / "classes"
classes.mkdir(exist_ok=True)
java = [
    source / "JniDomainProbe.java",
    source / "DomainConformance.java",
    project / "tests/java/conformance/Module.java",
    *sorted((out / "conformance/java").rglob("*.java")),
    *sorted((project / "src/java").rglob("*.java")),
]
if args.system_load:
    java += [
        jvm / "SystemLoadProbe.java",
        jvm / "LateSignalPressure.java",
        root / "jvm-jni/src/java/dev/glibc/worker/JvmAdapter.java",
    ]
    isolated = out / "isolated-classes"
    isolated.mkdir(exist_ok=True)
    subprocess.run(
        [
            str(args.jdk / "bin/javac"),
            "--release",
            "8",
            "-d",
            str(isolated),
            str(jvm / "SystemLoadFixture.java"),
            str(jvm / "SystemLoadBenchmark.java"),
        ],
        check=True,
    )
    subprocess.run(
        [
            str(args.jdk / "bin/jar"),
            "cf",
            str(out / "system-fixture.jar"),
            "-C",
            str(isolated),
            ".",
        ],
        check=True,
    )
subprocess.run(
    [
        str(args.jdk / "bin/javac"),
        "--release",
        "8",
        "-d",
        str(classes),
        *map(str, java),
    ],
    check=True,
)
subprocess.run(
    [
        str(args.jdk / "bin/jar"),
        "cf",
        str(out / "domain-probe.jar"),
        "-C",
        str(classes),
        ".",
    ],
    check=True,
)
runtime = args.runtime.resolve()
loader = "ld-linux-aarch64.so.1" if android else "ld-linux-x86-64.so.2"
files = [
    *host_sources,
    *guest_sources,
    source / "module.c",
    project / "src/common/domain_transport.h",
    core / "signal-registry.inc",
    core / "signals.h",
    core / "domain.h",
    core_fixture / "adopt.h",
    jvm / "vm-start.h",
    project / "tests/native/guest_conformance.c",
    out / "conformance/fixture_generated.inc",
    *java,
    *sorted((project / "src/common").glob("*.h")),
    *sorted((out / "generated").glob("*")),
    *sorted((root / "core/include").glob("*.h")),
    runtime / loader,
    runtime / "libc.so.6",
    runtime / "libm.so.6",
    *[
        out / p
        for p in [
            "host",
            "guest",
            "probe.so",
            "conformance.so",
            "libgcc_s.so.1",
            "domain-probe.jar",
            "host_store.o",
        ]
    ],
    Path(__file__),
]
if args.system_load:
    files += [
        jvm / "system-load-fixture.c",
        jvm / "SystemLoadFixture.java",
        jvm / "SystemLoadBenchmark.java",
        *[
            out / name
            for name in (
                "system-load.so",
                "system-reject.so",
                "system-ordinary.so",
                "system-fixture.jar",
                "jre-adapter-profile.h",
            )
        ],
    ]
if args.late_signal:
    files += [
        *launcher,
        root / "jvm-jni/include/signal_profile.h",
        out / "late-signal-profile.h",
        out / "late-bridge.so",
        vm_file,
    ]
if args.late_signal and android:
    files += [out / "inline-host.so"]
if args.art_signal_profile:
    files += [args.art_signal_profile.resolve()]
files += [core / "signal-router.h", project / "src/host/domain_runtime.h"]
manifest = {
    "late_signal": args.late_signal,
    "art_signal_profile": art_profile if args.art_signal_profile else None,
    "system_load": args.system_load,
    "abi": args.abi,
    "host_libc": "bionic" if android else "glibc",
    "default_backend": "domain",
    "runtime": str(runtime),
    "loader": loader,
    "commands": commands,
    "jre_artifact": profile,
    "sha256": {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files
    },
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(out)
