#!/usr/bin/env bash
# Prepare JNI inputs; dependency builders and their caches are owned by core.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
: "${GW_CORE_PROJECT:?Set the core project directory}"
ndk=$1
ccndk=$2
if [[ ${GW_DEPS_READY:-0} != 1 ]]; then
  "$GW_CORE_PROJECT/scripts/build-glibc.sh"
  "$GW_CORE_PROJECT/scripts/build-ffi.sh" "$ccndk"
fi
for file in "build/glibc-runtime/$GW_ABI/$GW_LOADER" \
  "build/glibc-runtime/$GW_ABI/libc.so.6" \
  "build/ffi-android/$GW_ABI/lib/libffi.a" "build/ffi-linux/$GW_ABI/lib/libffi.a"; do
  [[ -f $GW_CORE_PROJECT/$file ]] || {
    echo "Build dependency missing: $GW_CORE_PROJECT/$file" >&2
    exit 2
  }
done
mkdir -p "$project/build/jni-include"
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h" \
  "$project/build/jni-include/"
