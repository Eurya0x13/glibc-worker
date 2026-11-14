#!/usr/bin/env bash
# Build two separate libc domains. Only headers/protocol are shared between them.
set -euo pipefail
cd "$(dirname "$0")/../../.."
eval "$(python3 scripts/target.py --shell)"
ndk=$(python3 scripts/target.py --ndk)
ccndk="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/${GW_NDK_TRIPLET}29-clang"
command -v "$GW_GUEST_CC" >/dev/null
out="jvm-jni/build/tests/android/jniLibs/$GW_ABI"
mkdir -p jvm-jni/build/native/generated jvm-jni/vendor "$out" jvm-jni/build/tests/android/assets/runtime
cp core/third_party/runtime/* jvm-jni/build/tests/android/assets/runtime/

# Tests always use the maintained, matched GNU runtime.
if [[ ${GW_DEPS_READY:-0} != 1 ]]; then
  core/scripts/build-glibc.sh
fi
runtime="core/build/glibc-runtime/$GW_ABI"

# 2. Pinned resources, VM-neutral JNI header, generated forwarding and libffi.
mkdir -p android-jni/build/jni-include
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/jni.h" android-jni/build/jni-include/
sha256sum android-jni/build/jni-include/jni.h >jvm-jni/build/native/jni-header-hash.txt
python3 jvm-jni/tests/scripts/gen-fixture.py
python3 android-jni/tests/gen-binding-capacity.py --output jvm-jni/build/native/generated
python3 jvm-jni/tests/scripts/prepare-zstd.py --fetch-only
python3 jvm-jni/tests/scripts/prepare-native-jars.py --fetch-only
python3 jvm-jni/tests/scripts/prepare-jre.py
python3 jvm-jni/tests/scripts/prepare-zstd.py
python3 jvm-jni/tests/scripts/prepare-native-jars.py
core/scripts/build-ffi.sh "$ccndk"
cp "core/build/ffi-android/$GW_ABI/share/licenses/libffi/LICENSE" \
  jvm-jni/build/tests/android/assets/runtime/libffi-LICENSE.txt
cp android-jni/third_party/zig/LICENSE \
  jvm-jni/build/tests/android/assets/runtime/zig-LICENSE.txt
python3 android-jni/scripts/gen-jni.py
python3 jvm-jni/tests/scripts/prepare-sqlite.py

includes=(-Iandroid-jni/include -Icore/include -Iandroid-jni/src/common -Ibionic-dlopen/include -Ibionic-dlopen/src -Iandroid-jni/src/host -Iandroid-jni/src/guest
  -Ijvm-jni/include -Ijvm-jni/src/native -Icore/src/bootstrap -Ijvm-jni/tests/native/harness
  -Iandroid-jni/build -Ijvm-jni/build/native -Ijvm-jni/build/native/generated)
warnings=(-Wall -Wextra -Werror)
host_flags=(-fPIC -fno-emulated-tls -O1 -g "${warnings[@]}" "${includes[@]}")
guest_flags=(-fPIC -O1 -g "${warnings[@]}" "${includes[@]}" -Iandroid-jni/build/jni-include)
guest_flags+=(-DGW_TEST_BINDING_CAPACITY=1)
lab_sources=(jvm-jni/tests/native/harness/bootstrap.c
  jvm-jni/tests/native/harness/jni.c jvm-jni/tests/native/harness/jni_dispatch.c jvm-jni/tests/native/harness/jre_host.c
  jvm-jni/tests/native/harness/pojav_jli.c jvm-jni/tests/native/harness/jre_layout_spike.c jvm-jni/tests/native/harness/loader_smoke.c
  jvm-jni/tests/native/harness/lab_suite.c
  jvm-jni/tests/native/harness/sigsys.c)

# 3. Build the same independent runtime, selecting only this harness's JRE profile.
core_out="$PWD/jvm-jni/build/tests/runtime/$GW_ABI"
GW_DEPS_READY=1 GW_RUNTIME_OUT="$core_out" \
  GW_JRE_ADAPTER_HEADER="$PWD/jvm-jni/build/native/jre-adapter-profile.h" scripts/build-runtime.sh
mapfile -t abi_flags <"$core_out/generated/abi-flags.txt"
host_flags+=("${abi_flags[@]}")
guest_flags+=("${abi_flags[@]}")
cp "$core_out/libglibc_bridge.so" "$out/"
"$ccndk" -shared "${host_flags[@]}" \
  jvm-jni/tests/native/harness/jre_exec_compat.c -o "$out/libjre_exec_compat.so"
"$ccndk" -shared "${host_flags[@]}" \
  "${lab_sources[@]}" -L"$out" -lglibc_bridge "$core_out/libglibc_bootstrap.a" \
  -llog -ldl -pthread \
  -Wl,--no-undefined -o "$out/libworker_host.so"
"$ccndk" -shared "${host_flags[@]}" \
  android-jni/tests/native/host_load_control.c -o "$out/libhost_control.so"
"$ccndk" -shared "${host_flags[@]}" -O2 -fno-builtin-memcpy -DPAYLOAD_HOST=1 \
  android-jni/tests/native/payload_bench.c android-jni/tests/native/array_copy_checks.c \
  -o "$out/libpayload_host.so"

# 4. GNU Guest program and test DSOs. Never pass the bionic sysroot to GNU cc.
"$GW_GUEST_CC" "${guest_flags[@]}" -I"core/build/ffi-linux/$GW_ABI/include" \
  jvm-jni/tests/native/harness/worker.c jvm-jni/tests/native/harness/jni_proxy.c \
  android-jni/src/guest/full_guest.c android-jni/src/guest/rpc.c android-jni/src/guest/guest_loader.c \
  "core/build/ffi-linux/$GW_ABI/lib/libffi.a" -ldl -pthread -o "$out/libguest_worker.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" core/tests/native/guest.c core/tests/native/mutex_contract.c \
  -Icore/tests/native -pthread -o "$out/libguest_probe.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" android-jni/tests/native/guest_bench.c \
  -o "$out/libguest_bench.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" -O2 -fno-builtin-memcpy \
  android-jni/tests/native/payload_bench.c android-jni/tests/native/array_copy_checks.c \
  -o "$out/libguest_payload.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" \
  android-jni/tests/native/guest_jni.c -pthread -o "$out/libguest_jni.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" \
  android-jni/tests/native/guest_conformance.c -pthread -o "$out/libguest_conformance.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" -DCONTROL_VERSION=JNI_ERR \
  android-jni/tests/native/guest_load_control.c -o "$out/libguest_reject.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" -DCONTROL_VERSION=0x7fffffff \
  android-jni/tests/native/guest_load_control.c -o "$out/libguest_badversion.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" \
  android-jni/tests/native/guest_load_control.c -o "$out/libguest_noonload.so"

# Cache the expensive original SQLite compile by script + compiler identity.
sqlite_key=$(
  python3 - <<'PY'
import hashlib, os, subprocess
from pathlib import Path
h = hashlib.sha256(
    Path('jvm-jni/tests/scripts/prepare-sqlite.py').read_bytes()
    + Path('jvm-jni/tests/scripts/build-native.sh').read_bytes()
)
for option in ['-dumpmachine', '-dumpfullversion']:
    h.update(subprocess.check_output([os.environ['GW_GUEST_CC'], option]))
print(h.hexdigest()[:16])
PY
)
sqlite_cache="jvm-jni/build/sqlite/$GW_ABI-libsqlite-$sqlite_key.so"
if [[ ! -f "$sqlite_cache" ]]; then
  "$GW_GUEST_CC" -shared -fPIC -O1 -DSQLITE_THREADSAFE=1 jvm-jni/build/sqlite/sqlite3.c \
    -Wl,-soname,libsqlite_guest.so -ldl -pthread -o "$sqlite_cache"
fi
cp "$sqlite_cache" "$out/libsqlite_guest.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" -Ijvm-jni/build/sqlite android-jni/tests/native/guest_sqlite.c \
  -L"$out" -lsqlite_guest -Wl,-rpath,'$ORIGIN' -pthread -o "$out/libdatabase_jni.so"
"$GW_GUEST_CC" -shared "${guest_flags[@]}" android-jni/tests/native/guest_shared.c \
  -Wl,-soname,libmulti_shared.so -o "$out/libmulti_shared.so"
for spec in a:111 b:222 reject:333; do
  name="${spec%:*}"
  ident="${spec#*:}"
  flags=()
  [[ "$name" != reject ]] || flags+=(-DREJECT_AFTER_REGISTER)
  "$GW_GUEST_CC" -shared "${guest_flags[@]}" "-DMODULE_ID=$ident" "${flags[@]}" \
    \
    android-jni/tests/native/guest_multi.c -L"$out" -lmulti_shared -Wl,-rpath,'$ORIGIN' \
    -o "$out/libmulti_$name.so"
done
# Link-only dependency deliberately excluded from APK (negative load test).
mkdir -p jvm-jni/build/native/missing-link
"$GW_GUEST_CC" -shared -fPIC -O1 android-jni/tests/native/missing_dependency.c \
  -Wl,-soname,libmissing_spike.so -o jvm-jni/build/native/missing-link/libmissing_spike.so
"$GW_GUEST_CC" -shared "${guest_flags[@]}" android-jni/tests/native/guest_missing.c \
  -Ljvm-jni/build/native/missing-link -lmissing_spike -o "$out/libdependency_failure.so"

# 5. Package exact GNU images and retain their hashes for audit.
cp "$runtime/$GW_LOADER" "$out/libguest_loader.so"
cp "$runtime/libc.so.6" "$out/libguest_libc.so"
cp "$runtime/libpthread.so.0" "$out/libguest_pthread.so"
sha256sum "$out/"*.so >jvm-jni/build/native/hashes.txt
file "$out/libglibc_bridge.so" "$out/libguest_worker.so" "$out/libguest_loader.so"
