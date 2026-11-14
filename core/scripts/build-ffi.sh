#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
eval "$(python3 scripts/target.py --shell)"
mkdir -p vendor work build
ccndk="$1"
root="$PWD"
archive=vendor/libffi-3.4.6.tar.gz
sha=b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e

if [[ ! -f $archive ]]; then
  curl -fLsS --retry 2 \
    https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz \
    -o "$archive"
fi
echo "$sha  $archive" | sha256sum -c -
if [[ ! -d work/libffi-3.4.6 ]]; then
  tar -xzf "$archive" -C work
fi

for target in android linux; do
  prefix="$root/build/ffi-$target/$GW_ABI"
  mkdir -p "$prefix/share/licenses/libffi"
  cp "$root/work/libffi-3.4.6/LICENSE" "$prefix/share/licenses/libffi/"
  [[ -f $prefix/lib/libffi.a ]] && continue
  obj="$root/work/ffi-$target-$GW_ABI"
  mkdir -p "$obj"
  if [[ $target == android ]]; then
    (
      cd "$obj"
      CC="$ccndk" \
        AR="$(dirname "$ccndk")/llvm-ar" \
        RANLIB="$(dirname "$ccndk")/llvm-ranlib" \
        "$root/work/libffi-3.4.6/configure" \
        --host="$GW_NDK_TRIPLET" --prefix="$prefix" --libdir="$prefix/lib" \
        --disable-shared --enable-static --with-pic --disable-docs
    )
  else
    (
      cd "$obj"
      CC="$GW_GUEST_CC" "$root/work/libffi-3.4.6/configure" \
        --host="$GW_GNU_TRIPLET" --prefix="$prefix" --libdir="$prefix/lib" \
        --disable-shared --enable-static --with-pic --disable-docs
    )
  fi
  make -C "$obj" -j2
  make -C "$obj" install
  if [[ ! -f $prefix/lib/libffi.a && -f $prefix/lib64/libffi.a ]]; then
    mkdir -p "$prefix/lib"
    cp "$prefix/lib64/libffi.a" "$prefix/lib/libffi.a"
  fi
done
