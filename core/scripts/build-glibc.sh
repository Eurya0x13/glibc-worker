#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
eval "$(python3 scripts/target.py --shell)"
command -v "$GW_GUEST_CC" >/dev/null

version=2.44
archive="vendor/glibc-$version.tar.xz"
archive_sha=37f600f2bef3c5e8300147059568b2a2e40a7ad6ccc65ce942556d49429cc667
runtime="build/glibc-runtime/$GW_ABI"
mkdir -p vendor "$runtime" work

if [[ ! -f $archive ]]; then
  curl -fLsS "https://ftp.gnu.org/gnu/libc/glibc-$version.tar.xz" -o "$archive.tmp"
  mv "$archive.tmp" "$archive"
fi
echo "$archive_sha  $archive" | sha256sum -c -

patches=(
  patches/termux/disable-clone3-glibc244.patch
  patches/termux/set-nptl-syscalls.patch
  patches/0001-use-faccessat.patch
  patches/0002-use-at-file-syscalls.patch
  patches/0003-reject-robust-mutex.patch
)
key=$(cat scripts/build-glibc.sh scripts/apply-patches.py config/targets.json \
  "${patches[@]}" | sha256sum | cut -c1-16)
root="$PWD/work/glibc-build-$GW_ABI-$key"
src="$root/src"
obj="$root/obj"
mkdir -p "$obj"

if [[ ! -f $root/patched ]]; then
  mkdir -p "$src"
  tar -xJf "$archive" --strip-components=1 -C "$src"
  python3 scripts/apply-patches.py --source "$src" "${patches[@]}"
  touch "$root/patched"
fi
python3 scripts/apply-patches.py --verify-only --source "$src" "${patches[@]}"

if [[ ! -f $obj/Makefile ]]; then
  arch_options=()
  if [[ $GW_ABI == x86_64 ]]; then
    arch_options+=(--enable-cet)
  fi
  (
    cd "$obj"
    CC="$GW_GUEST_CC" CXX="${GW_GUEST_CC%gcc}g++" \
      "$src/configure" \
      --build="$(gcc -dumpmachine)" \
      --host="$GW_GNU_TRIPLET" \
      --prefix=/data/data/com.termux/files/usr/glibc \
      --libdir=/data/data/com.termux/files/usr/glibc/lib \
      --enable-bind-now --disable-multi-arch "${arch_options[@]}" \
      --disable-nscd --disable-profile --disable-werror --disable-default-pie \
      --enable-stack-protector=strong
  )
fi

# GNU glibc is rebuilt as a matched loader/libc pair. No install into the Host.
make -C "$obj" -j"${GLIBC_BUILD_JOBS:-4}"
cp "$obj/elf/ld.so" "$runtime/$GW_LOADER"
cp "$obj/libc.so" "$runtime/libc.so.6"
for spec in nptl/libpthread:libpthread.so.0 math/libm:libm.so.6 rt/librt:librt.so.1 dlfcn/libdl:libdl.so.2; do
  cp "$obj/${spec%:*}.so" "$runtime/${spec#*:}"
done
sha256sum "$runtime/$GW_LOADER" "$runtime/libc.so.6" >"$runtime/hashes.txt"
cp "$obj/config.make" "$runtime/config.make"
printf '%s\n' "$root" >"$runtime/build-root.txt"
printf '%s\n' "$archive_sha" >"$runtime/source-sha256.txt"
sha256sum "${patches[@]}" >"$runtime/patch-hashes.txt"
"$GW_GUEST_CC" --version | head -1 >"$runtime/compiler.txt"
