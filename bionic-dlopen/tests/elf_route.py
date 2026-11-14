"""Run the metadata classifier under ASan/UBSan with real ELF and bad bounds."""

import argparse, json, os, struct, subprocess
from pathlib import Path
import sys

ABI = os.environ.get("GW_ABI", "x86_64")

p = argparse.ArgumentParser()
p.add_argument(
    "--output",
    type=Path,
    default=Path("bionic-dlopen/build/test-results/elf-route.json"),
)
p.add_argument(
    "--libraries",
    type=Path,
    required=True,
    help="Directory of explicitly selected test ELF inputs",
)
p.add_argument("--core-include", type=Path, required=True)
a = p.parse_args()
a.output = a.output.resolve()
a.libraries = a.libraries.resolve()
a.core_include = a.core_include.resolve()
root = Path(__file__).resolve().parents[1]
os.chdir(root)
work = root / "build/test-work/elf-route"
work.mkdir(parents=True, exist_ok=True)
(work / "probe.c").write_text(
    '#include "elf_route.h"\n#include <stdio.h>\n'
    "int main(int n,char**v){for(int i=1;i<n;i++){"
    "struct gw_loader_error e={0}; int kind=gw_elf_classify(v[i],&e);"
    'printf("%d %d\\n",kind,e.code);}return 0;}\n'
)
compiler = os.environ.get("SANITIZER_CC", "clang")
subprocess.run(
    [
        compiler,
        "-D_GNU_SOURCE",
        "-std=gnu17",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-misleading-indentation",
        "-I" + str(a.core_include),
        "-I" + str(root),
        "-I" + str(root / "include"),
        *(["-DGW_CLASSIFY_AARCH64=1"] if ABI == "arm64-v8a" else []),
        str(work / "probe.c"),
        str(root / "elf_route.c"),
        "-o",
        str(work / "probe"),
    ],
    check=True,
)
libs = a.libraries
cases = [
    (libs / n, expected, 0)
    for n, expected in [
        ("libworker_host.so", 0),
        ("libhost_control.so", 0),
        ("libjava.so", 0),
        ("libjvm.so", 0),
        ("libguest_probe.so", 1),
        ("libguest_conformance.so", 1),
        ("libguest_noonload.so", 1),
        ("libmulti_a.so", 1),
        ("libmulti_b.so", 1),
        ("libmulti_shared.so", 1),
        ("libdatabase_jni.so", 1),
        ("libsqlite_guest.so", 1),
    ]
]
cases.extend(
    [(libs / "libglibc_bridge.so", 0, 0), (libs / "libinstalled_zstd.so", 1, 0)]
)
(work / "neutral.c").write_text("int neutral(void){return 0;}\n")
subprocess.run(
    [
        compiler,
        "-shared",
        "-fPIC",
        "-nostdlib",
        str(work / "neutral.c"),
        "-o",
        str(work / "neutral.so"),
    ],
    check=True,
)
neutral = bytearray((work / "neutral.so").read_bytes())
# Metadata-only fixture: the host compiler's machine differs when checking
# ARM inputs. Match the selected machine; this image is never executed.
struct.pack_into("<H", neutral, 18, 183 if ABI == "arm64-v8a" else 62)
(work / "neutral-metadata.so").write_bytes(neutral)
cases.append((work / "neutral-metadata.so", 0, 0))
base = (libs / "libguest_probe.so").read_bytes()


def case(name, data, expected=-1, error=3):
    path = work / name
    path.write_bytes(data)
    cases.append((path, expected, error))


for n in [0, 1, 4, 16, 32, 63, 64, 128, 511]:
    case("truncated-" + str(n), base[:n])
for name, offset, fmt, value in [
    ("elf32", 4, "B", 1),
    ("big-endian", 5, "B", 2),
    ("wrong-machine", 18, "H", 62 if ABI == "arm64-v8a" else 183),
    ("excess-phdr", 56, "H", 129),
    ("offset-wrap", 32, "Q", 2**64 - 1),
]:
    b = bytearray(base)
    struct.pack_into("<" + fmt, b, offset, value)
    case(name, b, error=5 if name == "excess-phdr" else 3)
# Keep GNU version requirements, remove only DT_NEEDED tags. This metadata
# fixture is never loaded; it exercises classification of GLIBC_* requirements.
b = bytearray(base)
phoff = struct.unpack_from("<Q", b, 32)[0]
phsize, phcount = struct.unpack_from("<HH", b, 54)
for i in range(phcount):
    ph = struct.unpack_from("<IIQQQQQQ", b, phoff + i * phsize)
    if ph[0] == 2:
        for off in range(ph[2], ph[2] + ph[5], 16):
            tag = struct.unpack_from("<Q", b, off)[0]
            if tag == 1:
                struct.pack_into("<Q", b, off, 21)
case("gnu-version-without-direct-libc", b, 1, 0)
case("no-libc-or-gnu-marker", bytes(b).replace(b"GLIBC_", b"OTHER_"), 0, 0)


def metadata(needed=b"libother.so", version=None):
    image = bytearray(4096)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        image,
        0,
        ident,
        3,
        183 if ABI == "arm64-v8a" else 62,
        1,
        0,
        64,
        0,
        0,
        64,
        56,
        2,
        0,
        0,
        0,
    )
    strings = b"\0" + needed + b"\0"
    entries = [(5, 0x400), (10, len(strings)), (1, 1)]
    if version:
        index = len(strings)
        strings += version + b"\0"
        entries[1] = (10, len(strings))
        entries += [(0x6FFFFFFE, 0x800), (0x6FFFFFFF, 1)]
        struct.pack_into("<HHIII", image, 0x800, 1, 1, 1, 16, 0)
        struct.pack_into("<IHHII", image, 0x810, 0, 0, 2, index, 0)
    entries.append((0, 0))
    image[0x400 : 0x400 + len(strings)] = strings
    for index, (tag, value) in enumerate(entries):
        struct.pack_into("<QQ", image, 0x200 + index * 16, tag, value)
    struct.pack_into(
        "<IIQQQQQQ", image, 64, 1, 4, 0, 0, 0, len(image), len(image), 4096
    )
    struct.pack_into(
        "<IIQQQQQQ",
        image,
        120,
        2,
        4,
        0x200,
        0x200,
        0,
        len(entries) * 16,
        len(entries) * 16,
        8,
    )
    return image


case("synthetic-glibc-needed", metadata(b"libc.so.6"), 1, 0)
case("synthetic-glibc-version", metadata(version=b"GLIBC_2.17"), 1, 0)
case("synthetic-unmarked", metadata(), 0, 0)
case("name-at-limit", metadata(b"x" * 255), 0, 0)
case("name-over-limit", metadata(b"x" * 256), error=5)
for name, offset, fmt, value, error in [
    ("dynamic-over-limit", 120 + 32, "Q", 513 * 16, 5),
    ("dynamic-misaligned", 120 + 32, "Q", 63, 3),
    ("bad-string-index", 0x200 + 2 * 16 + 8, "Q", 4095, 3),
    ("missing-string-table", 0x200 + 8, "Q", 0, 3),
    ("no-dynamic-terminator", 0x200 + 3 * 16, "Q", 21, 3),
    ("version-table-over-limit", 0x200 + 4 * 16 + 8, "Q", 65, 5),
    ("version-group-over-limit", 0x800 + 2, "H", 257, 5),
    ("version-bad-offset", 0x800 + 8, "I", 2**32 - 1, 3),
    ("version-short-chain", 0x800 + 2, "H", 2, 3),
    ("version-extra-chain", 0x810 + 12, "I", 16, 3),
    ("version-missing-count", 0x200 + 4 * 16 + 8, "Q", 0, 3),
]:
    # Put a GNU marker before the malformed field: incomplete parsing must
    # still be an error, not a positive guess or an unmarked fallback.
    image = metadata(b"libc.so.6", b"GLIBC_2.17")
    if name == "no-dynamic-terminator":
        image = metadata()
    struct.pack_into("<" + fmt, image, offset, value)
    case(name, image, error=error)
image = metadata()
image[0x400 : 0x400 + len(b"\0libother.so\0")] = b"x" * len(b"\0libother.so\0")
case("unterminated-name", image)
cases.append((work / "does-not-exist.so", -1, 3))
r = subprocess.run(
    [str(work / "probe"), *[str(path) for path, _, _ in cases]],
    capture_output=True,
    text=True,
    env={
        **os.environ,
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
        "UBSAN_OPTIONS": "halt_on_error=1",
    },
)
if r.returncode:
    raise SystemExit(r.stderr)
values = [tuple(map(int, x.split())) for x in r.stdout.splitlines()]
assert len(values) == len(cases)
rows = []
for (path, expected, error), (actual, actual_error) in zip(cases, values):
    rows.append(
        dict(
            path=str(path),
            expected=expected,
            actual=actual,
            expected_error=error,
            actual_error=actual_error,
        )
    )
    assert (actual, actual_error) == (expected, error), rows[-1]
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(
    json.dumps(
        dict(
            cases=len(rows),
            sanitizers=["ASan", "UBSan"],
            diagnostics=r.stderr,
            results=rows,
        ),
        indent=2,
    )
    + "\n"
)
print("ELF route:", len(rows), "cases passed under ASan/UBSan")
