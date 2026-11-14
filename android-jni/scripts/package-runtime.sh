#!/usr/bin/env bash
# Build and stage the GNU JNI service alongside the matching core runtime.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
: "${GW_CORE_PROJECT:?Set the core project directory}"
: "${GW_CORE_PREFIX:?Set the installed core prefix}"
: "${GW_DLOPEN_PREFIX:?Set the installed C loader prefix}"
runtime="$GW_CORE_PROJECT/build/glibc-runtime/$GW_ABI"
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
mkdir -p "$out/include/internal" "$out/licenses"
cp "$project/include/"*.h "$out/include/"
cp "$project/src/common/"*.h "$out/include/internal/"
cp "$GW_CORE_PROJECT/build/ffi-android/$GW_ABI/share/licenses/libffi/LICENSE" \
  "$out/licenses/libffi-LICENSE.txt"
if [[ ${GW_BINDING_BACKEND:-c} == zig ]]; then
  cp "$project/third_party/zig/LICENSE" "$out/licenses/zig-LICENSE.txt"
fi
