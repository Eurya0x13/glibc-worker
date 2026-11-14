#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "$0")" && pwd)
build_dir=${GW_PROJECT_BUILD:-"$project_root/build"}
cmake -S "$project_root" -B "$build_dir" "$@"
cmake --build "$build_dir" --parallel
