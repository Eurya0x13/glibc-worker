#!/usr/bin/env bash
# GNU-side JNI service. All dependency locations are supplied by the owner.
set -euo pipefail
project_root=$(cd "$(dirname "$0")" && pwd)
: "${GW_GUEST_CC:?Set the GNU compiler}"
: "${GW_GUEST_OUT:?Set the GNU service output file}"
: "${GW_CORE_INCLUDE:?Set installed core include directory}"
: "${GW_DLOPEN_INCLUDE:?Set installed loader include directory}"
: "${GW_JNI_INCLUDE:?Set VM-neutral JNI header directory}"
: "${GW_FFI_GUEST:?Set GNU libffi prefix}"
: "${GW_JNI_GENERATED:?Set JNI generated directory}"
mkdir -p "$(dirname "$GW_GUEST_OUT")"
flags=(-O1 -g -fPIC -Wall -Wextra -Werror)
if [[ -n ${GW_ABI_FLAGS_FILE:-} ]]; then
  mapfile -t abi_flags <"$GW_ABI_FLAGS_FILE"
  flags+=("${abi_flags[@]}")
fi
"$GW_GUEST_CC" "${flags[@]}" -I"$project_root/include" \
  -I"$project_root/src/common" -I"$project_root/src/guest" \
  -I"$GW_CORE_INCLUDE" -I"$GW_DLOPEN_INCLUDE" -I"$GW_JNI_INCLUDE" \
  -I"$(dirname "$GW_JNI_GENERATED")" -I"$GW_FFI_GUEST/include" \
  "$project_root/src/guest/service_main.c" "$project_root/src/guest/full_guest.c" \
  "$project_root/src/guest/rpc.c" "$project_root/src/guest/guest_loader.c" "$GW_FFI_GUEST/lib/libffi.a" \
  -ldl -pthread -o "$GW_GUEST_OUT"
