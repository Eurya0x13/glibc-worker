#!/usr/bin/env bash
# Build and stage the GNU JNI service alongside the matching core runtime.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
: "${GW_CORE_PROJECT:?Set the core project directory}"
: "${GW_CORE_PREFIX:?Set the installed core prefix}"
: "${GW_DLOPEN_PREFIX:?Set the installed C loader prefix}"
: "${GW_GUEST_CC:?Set the matched GNU compiler}"

unwind=$("$GW_GUEST_CC" -print-file-name=libgcc_s.so.1)
[[ -f $unwind ]] || {
  echo "Matched GNU unwind library missing: $unwind" >&2
  exit 2
}
runtime_args=(--unwind "$unwind")
case "${GW_PACKAGE_LIBSTDCXX:-0}" in
  0) ;;
  1)
    stdcxx=$("$GW_GUEST_CC" -print-file-name=libstdc++.so.6)
    [[ -f $stdcxx ]] || {
      echo "Matched GNU C++ library missing: $stdcxx" >&2
      exit 2
    }
    runtime_args+=(--libstdcxx "$stdcxx")
    ;;
  *)
    echo 'GW_PACKAGE_LIBSTDCXX must be 0 or 1' >&2
    exit 2
    ;;
esac

runtime=${GW_DOMAIN_RUNTIME:-}
if [[ -z $runtime ]]; then
  runtime=$(python3 "$GW_CORE_PROJECT/scripts/build-domain-runtime.py" \
    --abi "$GW_ABI" --jobs "${GW_BUILD_JOBS:-4}")
fi

# Check the runtime contract before replacing any staged images.
python3 - "$runtime/manifest.json" "$GW_ABI" <<'PYTHON'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text())
if manifest.get("domain_abi") != 1 or manifest.get("abi") != sys.argv[2]:
    raise SystemExit("Matching domain runtime manifest required")
PYTHON

service_stage=$(mktemp -d "$out/.guest-service.XXXXXX")
trap 'rm -rf -- "$service_stage"' EXIT
service="$service_stage/libguest_service.so"
GW_GUEST_OUT="$service" \
  GW_CORE_INCLUDE="$GW_CORE_PREFIX/include/glibc-worker/core" \
  GW_DLOPEN_INCLUDE="$GW_DLOPEN_PREFIX/include/glibc-worker/bionic-dlopen" \
  GW_JNI_INCLUDE="$project/build/jni-include" \
  GW_JNI_GENERATED="$project/build/$GW_ABI/generated" \
  GW_FFI_GUEST="$GW_CORE_PROJECT/build/ffi-linux/$GW_ABI" \
  GW_ABI_FLAGS_FILE="$out/generated/abi-flags.txt" "$project/build-guest.sh"

python3 "$project/scripts/package-runtime.py" "$runtime" "$out" "$GW_ABI" "$GW_LOADER" \
  --service "$service" "${runtime_args[@]}"

mkdir -p "$out/include/internal" "$out/licenses"
cp "$project/include/"*.h "$out/include/"
cp "$project/src/common/"*.h "$out/include/internal/"
cp "$GW_CORE_PROJECT/build/ffi-android/$GW_ABI/share/licenses/libffi/LICENSE" \
  "$out/licenses/libffi-LICENSE.txt"
cp "$project/third_party/zig/LICENSE" "$out/licenses/zig-LICENSE.txt"
cp "$project/../LICENSE" "$out/licenses/gcc-COPYING3"
cp "$project/third_party/gcc/COPYING.RUNTIME" "$out/licenses/gcc-COPYING.RUNTIME"
