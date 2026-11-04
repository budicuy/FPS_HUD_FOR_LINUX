#!/usr/bin/env bash
# run_sanitizers.sh - Diagnostic build with AddressSanitizer, LeakSanitizer & UndefinedBehaviorSanitizer
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -z "${CXX:-}" ]; then
    if command -v clang++ >/dev/null 2>&1; then
        export CXX="clang++"
        export CC="clang"
    elif command -v g++-16 >/dev/null 2>&1; then
        export CXX="g++-16"
        export CC="gcc-16"
    else
        export CXX="g++"
        export CC="gcc"
    fi
else
    export CC="${CC:-gcc}"
    export CXX="${CXX}"
fi

BUILD_DIR="${ROOT_DIR}/build_asan"

echo "=========================================================="
echo "  FPSHUD Memory Safety & Bug Detection Suite (ASan + UBSan)"
echo "  Compiler : $(which ${CXX}) ($(${CXX} --version | head -n1))"
echo "  Checks   : Memory Leaks, Buffer Overflow, UAF, UB, Int Overflow"
echo "=========================================================="

cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DENABLE_ASAN=ON \
    -DENABLE_UBSAN=ON

ninja -C "${BUILD_DIR}" fpshud_test

echo "----------------------------------------------------------"
echo "  Running Test Suite under AddressSanitizer & UBSan..."
echo "----------------------------------------------------------"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1:check_initialization_order=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

"${BUILD_DIR}/fpshud_test" 1

echo "=========================================================="
echo "  [SUCCESS] 0 Memory Leaks, 0 Buffer Overflows, 0 UB Detected!"
echo "=========================================================="
