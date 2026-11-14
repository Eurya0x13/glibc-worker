#!/usr/bin/env bash
# Compose project builds; project-specific staging lives with each project.
set -euo pipefail
cd "$(dirname "$0")/.."
eval "$(python3 scripts/target.py --shell)"
ndk=$(python3 scripts/target.py --ndk)
ccndk="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/${GW_NDK_TRIPLET}29-clang"
contexts=${GW_CONTEXTS:-16}
binding_backend=${GW_BINDING_BACKEND:-zig}
[[ $binding_backend == zig ]] || {
  echo 'The C resource backend was removed; GW_BINDING_BACKEND must be zig' >&2
  exit 2
}

[[ $contexts =~ ^[1-9][0-9]*$ && $contexts -ge 2 && $contexts -le 256 ]] || {
  echo 'GW_CONTEXTS must be between 2 and 256' >&2
  exit 2
}

rpc_profile=${GW_RPC_PROFILE:-0}
rpc_spin_ns=${GW_RPC_SPIN_NS:-20000}
wake_filter=${GW_FUTEX_WAKE_FILTER:-0}
ready_poll=${GW_RPC_READY_POLL:-0}
[[ $ready_poll == 0 || $ready_poll == 1 ]] || {
  echo 'GW_RPC_READY_POLL must be 0 or 1' >&2
  exit 2
}
[[ $wake_filter == 0 || $wake_filter == 1 ]] || {
  echo 'GW_FUTEX_WAKE_FILTER must be 0 or 1' >&2
  exit 2
}
[[ $rpc_profile == 0 || $rpc_profile == 1 ]] || {
  echo 'GW_RPC_PROFILE must be 0 or 1' >&2
  exit 2
}
[[ $rpc_spin_ns =~ ^(0|[1-9][0-9]*)$ && ${#rpc_spin_ns} -le 7 && $rpc_spin_ns -le 1000000 ]] || {
  echo 'GW_RPC_SPIN_NS must be between 0 and 1000000 nanoseconds' >&2
  exit 2
}

module=all
if [[ $# -gt 0 ]]; then
  if [[ $# != 2 || $1 != --module ]]; then
    echo 'Usage: build-runtime.sh [--module core|bionic-dlopen|android-jni|jvm-jni|all]' >&2
    exit 2
  fi
  module=$2
fi
case "$module" in
  core) level=0 ;;
  bionic-dlopen) level=1 ;;
  android-jni) level=2 ;;
  jvm-jni | all) level=3 ;;
  *)
    echo "Unknown project: $module" >&2
    exit 2
    ;;
esac

owner=$module
[[ $owner == all ]] && owner=jvm-jni
out=${GW_RUNTIME_OUT:-"$PWD/$owner/build/$GW_ABI/compat"}
mkdir -p "$out/generated" "$out/licenses"
out=$(cd "$out" && pwd)
core_build="$PWD/core/build/$GW_ABI"
loader_build="$PWD/bionic-dlopen/build/$GW_ABI"
prefix_path=""
abi_flags=("-DFB_CONTEXTS=$contexts" "-DGW_RPC_PROFILE=$rpc_profile"
  "-DGW_RPC_SPIN_NS=$rpc_spin_ns" "-DGW_FUTEX_WAKE_FILTER=$wake_filter"
  "-DGW_RPC_READY_POLL=$ready_poll")
printf '%s\n' "${abi_flags[@]}" >"$out/generated/abi-flags.txt"
cmake_args=(-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI="$GW_ABI" -DANDROID_PLATFORM=android-29
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
  -DCMAKE_C_FLAGS="-O1 -g ${abi_flags[*]}" -DBUILD_TESTING=OFF
  "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-undefined")

build_project() {
  local project=$1
  shift
  local project_build="$PWD/$project/build/$GW_ABI"
  mkdir -p "$project_build/generated"
  cp "$out/generated/abi-flags.txt" "$project_build/generated/abi-flags.txt"
  GW_PROJECT_BUILD="$project_build" "$project/build.sh" "${cmake_args[@]}" \
    -DCMAKE_INSTALL_PREFIX="$project_build/install" \
    -DCMAKE_PREFIX_PATH="$prefix_path" "$@"
  cmake --install "$project_build"
  prefix_path="$project_build/install${prefix_path:+;$prefix_path}"
}

# Lower projects do not discover Java, include JNI or build libffi.
build_project core
export GW_CORE_PROJECT="$PWD/core" GW_DLOPEN_PROJECT="$PWD/bionic-dlopen"
export GW_ANDROID_JNI_PROJECT="$PWD/android-jni"
export GW_CORE_PREFIX="$core_build/install" GW_DLOPEN_PREFIX="$loader_build/install"
core/scripts/package-runtime.sh "$out"
if ((level >= 1)); then
  build_project bionic-dlopen
  bionic-dlopen/scripts/package-runtime.sh "$out"
fi
if ((level >= 2)); then
  android-jni/scripts/prepare-build.sh "$ndk" "$ccndk"
  build_project android-jni \
    -DGW_BINDING_BACKEND="$binding_backend" \
    -DGW_JNI_INCLUDE_DIR="$PWD/android-jni/build/jni-include" \
    -DGW_FFI_INCLUDE_DIR="$PWD/core/build/ffi-android/$GW_ABI/include" \
    -DGW_FFI_LIBRARY="$PWD/core/build/ffi-android/$GW_ABI/lib/libffi.a" \
    -DGW_FFI_LICENSE_FILE="$PWD/core/build/ffi-android/$GW_ABI/share/licenses/libffi/LICENSE"
  android-jni/scripts/package-runtime.sh "$out"
fi
if ((level >= 3)); then
  adapter_args=(-DGW_JNI_INCLUDE_DIR="$PWD/android-jni/build/jni-include"
    -DGW_JRE_ADAPTER_HEADER="${GW_JRE_ADAPTER_HEADER:-}")
  build_project jvm-jni "${adapter_args[@]}"
  jvm-jni/scripts/package-runtime.sh "$out" "$ccndk"
fi

printf '%s\n' "abi=$GW_ABI" "projects=$module" "contexts=$contexts" \
  "binding_backend=$binding_backend" \
  "rpc_profile=$rpc_profile" "rpc_spin_ns=$rpc_spin_ns" \
  "futex_wake_filter=$wake_filter" "rpc_ready_poll=$ready_poll" >"$out/build.properties"
(
  cd "$out"
  find . -maxdepth 2 -type f \( -name '*.so' -o -name '*.a' -o -name '*.jar' \) \
    -print0 | sort -z | xargs -0 sha256sum >SHA256SUMS
)
echo "Projects built: $module -> $out"
