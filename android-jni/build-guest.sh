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
flags=(-O1 -g -fPIC -Wall -Wextra -Werror -DGW_DOMAIN_JNI)
if [[ ${GW_NATIVE_UNWIND_INTEGRATION:-0} == 1 ]]; then
  flags+=(-DGW_DOMAIN_NONLOCAL -D_GNU_SOURCE= -fexceptions
    -include "$project_root/src/guest/kernel-syscall.h")
  unwind_sources=("$project_root/src/guest/domain_jump.c")
  unwind_sources+=(
    "$project_root/src/guest/domain_signal_workers.c"
    "$project_root/src/guest/domain_descriptor_syscall.c"
    "$GW_CORE_INCLUDE/internal/signal-actions.c"
    "$GW_CORE_INCLUDE/internal/signal-consuming.c"
    "$GW_CORE_INCLUDE/internal/guest-signalfd.c"
    "$GW_CORE_INCLUDE/internal/guest-ready.c"
    "$GW_CORE_INCLUDE/internal/guest-epoll.c"
    "$GW_CORE_INCLUDE/internal/guest-fd-lease.c"
    "$GW_CORE_INCLUDE/internal/signal-execution.c"
    "$GW_CORE_INCLUDE/internal/signal-execution-guest.c"
    "$GW_CORE_INCLUDE/internal/start.S"
    "$GW_CORE_INCLUDE/internal/signal-targets.c"
    "$GW_CORE_INCLUDE/internal/signal-inbox.c"
    "$GW_CORE_INCLUDE/internal/signal-pending.c"
    "$GW_CORE_INCLUDE/internal/signal-user-copy.c"
  )
else
  unwind_sources=()
fi
if [[ -n ${GW_ABI_FLAGS_FILE:-} ]]; then
  mapfile -t abi_flags <"$GW_ABI_FLAGS_FILE"
  flags+=("${abi_flags[@]}")
fi
"$GW_GUEST_CC" "${flags[@]}" -I"$project_root/include" \
  -I"$project_root/src/common" -I"$project_root/src/guest" \
  -I"$GW_CORE_INCLUDE" -I"$GW_CORE_INCLUDE/internal" -I"$GW_DLOPEN_INCLUDE" -I"$GW_JNI_INCLUDE" \
  -I"$(dirname "$GW_JNI_GENERATED")" -I"$GW_FFI_GUEST/include" \
  "$project_root/src/guest/domain_service.c" "$project_root/src/guest/domain_signals.c" "$project_root/src/guest/full_guest.c" \
  "$project_root/src/guest/rpc.c" "$project_root/src/guest/guest_loader.c" "$GW_FFI_GUEST/lib/libffi.a" \
  "${unwind_sources[@]}" \
  "$GW_CORE_INCLUDE/internal/signal-stack.S" \
  -ldl -pthread -o "$GW_GUEST_OUT"
