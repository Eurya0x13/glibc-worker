#!/usr/bin/env bash
# Stage core artifacts for an existing flat-layout consumer.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
: "${GW_ABI:?Select the target ABI}"
mkdir -p "$out/include/internal" "$out/licenses"
cp "$project/build/$GW_ABI/libglibc_worker_core.a" "$out/libglibc_bootstrap.a"
cp "$project/include/"*.h "$project/src/bootstrap/loader_image.h" "$out/include/internal/"
cp "$project/third_party/runtime/COPYING.LIB" "$out/licenses/"
