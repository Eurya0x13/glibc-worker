#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
python3 jvm-jni/tests/coverage/jni_symbols.py
# Build the Android group before classifying its selected-ABI ELF inputs.
python3 jvm-jni/tests/run.py --group coverage "$@"
