#!/usr/bin/env bash
# build_clang.sh - Fast extreme-optimization build using Clang 23 + LLD + ThinLTO
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

export CC="${CC:-clang}"
export CXX="${CXX:-clang++}"

BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "=========================================================="
echo "  FPSHUD Clang 23 Extreme Optimization Build Pipeline"
echo "  Compiler : $(which ${CXX}) ($(${CXX} --version | head -n1))"
echo "  Linker   : $(which ld.lld 2>/dev/null || echo 'LLD default')"
echo "  Profile  : ${BUILD_TYPE} (ThinLTO + LLD + -O3)"
echo "=========================================================="

cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}"

ninja -C "${BUILD_DIR}" "$@"

echo "==> Clang build completed successfully in ${BUILD_DIR}"
