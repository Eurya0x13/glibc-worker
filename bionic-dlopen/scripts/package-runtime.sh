#!/usr/bin/env bash
# Stage the C loader API; JNI packaging belongs to its caller.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
out=$1
mkdir -p "$out/include"
cp "$project/include/"*.h "$out/include/"
