#!/usr/bin/env bash
# Compose the legacy single-DSO/JAR distribution for a JVM embedder.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
ccndk=$2
: "${GW_CORE_PROJECT:?Set the core project directory}"
: "${GW_ANDROID_JNI_PROJECT:?Set the public JNI project directory}"
: "${GW_DLOPEN_PROJECT:?Set the C loader project directory}"
jni_build="$GW_ANDROID_JNI_PROJECT/build/$GW_ABI"
loader_build="$GW_DLOPEN_PROJECT/build/$GW_ABI"
jvm_build="$project/build/$GW_ABI"
"$ccndk" -shared -Wl,--no-undefined -Wl,-soname,libglibc_bridge.so \
  -Wl,--whole-archive "$jni_build/libgw_android_jni.a" \
  "$jvm_build/libgw_jvm_jni.a" "$loader_build/libgw_dlopen.a" \
  -Wl,--no-whole-archive "$GW_CORE_PROJECT/build/$GW_ABI/libglibc_worker_domains.a" \
  "$GW_CORE_PROJECT/build/$GW_ABI/libglibc_worker_core.a" \
  "$GW_CORE_PROJECT/build/ffi-android/$GW_ABI/lib/libffi.a" \
  -ldl -pthread -o "$out/libglibc_bridge.so"
rm -rf "$out/classes"
mkdir -p "$out/classes"
cp -R "$jni_build/classes/." "$out/classes/"
cp -R "$jvm_build/classes/." "$out/classes/"
jar cf "$out/glibc-bridge-helpers.jar" -C "$out/classes" .
