#!/usr/bin/env bash
# run_sanitizers.sh - Standard CMake Workflow Preset runner for ASan/UBSan & TSan via CTest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

cmake --workflow --preset asan-ubsan
cmake --workflow --preset tsan
