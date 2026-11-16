#!/usr/bin/env bash
# Build and stage the GNU JNI service alongside the matching core runtime.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
: "${GW_CORE_PROJECT:?Set the core project directory}"
: "${GW_CORE_PREFIX:?Set the installed core prefix}"
: "${GW_DLOPEN_PREFIX:?Set the installed C loader prefix}"
runtime=${GW_DOMAIN_RUNTIME:-}
if [[ -z $runtime ]]; then
  runtime=$(python3 "$GW_CORE_PROJECT/scripts/build-domain-runtime.py" \
    --abi "$GW_ABI" --jobs "${GW_BUILD_JOBS:-4}")
fi
# Refuse an ordinary libc payload; its NPTL cannot allocate borrowed carriers.
python3 - "$runtime/manifest.json" "$GW_ABI" <<'PYTHON'
import json
import sys
from pathlib import Path
m = json.loads(Path(sys.argv[1]).read_text())
if m.get("domain_abi") != 1 or m.get("abi") != sys.argv[2]:
    raise SystemExit("Matching domain runtime manifest required")
PYTHON
GW_GUEST_OUT="$out/libguest_service.so" \
  GW_CORE_INCLUDE="$GW_CORE_PREFIX/include/glibc-worker/core" \
  GW_DLOPEN_INCLUDE="$GW_DLOPEN_PREFIX/include/glibc-worker/bionic-dlopen" \
  GW_JNI_INCLUDE="$project/build/jni-include" \
  GW_JNI_GENERATED="$project/build/$GW_ABI/generated" \
  GW_FFI_GUEST="$GW_CORE_PROJECT/build/ffi-linux/$GW_ABI" \
  GW_ABI_FLAGS_FILE="$out/generated/abi-flags.txt" "$project/build-guest.sh"
cp "$runtime/$GW_LOADER" "$out/libguest_loader.so"
cp "$runtime/libc.so.6" "$out/libguest_libc.so"
cp "$runtime/libpthread.so.0" "$out/libguest_pthread.so"
cp "$runtime/libm.so.6" "$out/libguest_math.so"
cp "$runtime/libdl.so.2" "$out/libguest_dl.so"
cp "$runtime/librt.so.1" "$out/libguest_rt.so"
: "${GW_GUEST_CC:?Set the matched GNU compiler}"
unwind=$("$GW_GUEST_CC" -print-file-name=libgcc_s.so.1)
[[ -f $unwind ]] || {
  echo "Matched GNU unwind library missing: $unwind" >&2
  exit 2
}
cp -L "$unwind" "$out/libguest_unwind.so"
mkdir -p "$out/include/internal" "$out/licenses"
cp "$project/include/"*.h "$out/include/"
cp "$project/src/common/"*.h "$out/include/internal/"
cp "$GW_CORE_PROJECT/build/ffi-android/$GW_ABI/share/licenses/libffi/LICENSE" \
  "$out/licenses/libffi-LICENSE.txt"
cp "$project/third_party/zig/LICENSE" "$out/licenses/zig-LICENSE.txt"

python3 - "$runtime" "$out" <<'PYTHON'
import hashlib
import json
import sys
from pathlib import Path
runtime, out = map(Path, sys.argv[1:])
names = ["libguest_loader.so", "libguest_libc.so", "libguest_pthread.so",
         "libguest_math.so", "libguest_dl.so", "libguest_rt.so",
         "libguest_unwind.so", "libguest_service.so"]
manifest = {
    "version": 1,
    "domain_abi": 1,
    "default_backend": "same-thread",
    "runtime": json.loads((runtime / "manifest.json").read_text()),
    "sha256": {n: hashlib.sha256((out / n).read_bytes()).hexdigest()
               for n in names},
}
(out / "domain-runtime.json").write_text(json.dumps(manifest, indent=2) + "\n")
PYTHON
