#!/usr/bin/env bash
# run_sanitizers.sh - Diagnostic build with AddressSanitizer, LeakSanitizer & UndefinedBehaviorSanitizer
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ -z "${CXX:-}" ]; then
    if command -v g++-16 >/dev/null 2>&1; then
        export CXX="g++-16"
        export CC="gcc-16"
    elif command -v clang++-19 >/dev/null 2>&1; then
        export CXX="clang++-19"
        export CC="clang-19"
    elif command -v g++-15 >/dev/null 2>&1; then
        export CXX="g++-15"
        export CC="gcc-15"
    elif command -v clang++ >/dev/null 2>&1; then
        export CXX="clang++"
        export CC="clang"
    else
        export CXX="g++"
        export CC="gcc"
    fi
else
    if [ -z "${CC:-}" ]; then
        if [[ "$CXX" == *clang* ]]; then
            export CC="clang"
        elif [[ "$CXX" == *g++* ]]; then
            export CC="${CXX/g++/gcc}"
        else
            export CC="gcc"
        fi
    fi
    export CXX="${CXX}"
fi

BUILD_DIR="${ROOT_DIR}/build_asan"
TSAN_BUILD_DIR="${ROOT_DIR}/build_tsan"

echo "=========================================================="
echo "  FPSHUD Memory & Concurrency Safety Suite (Rust-Grade)"
echo "  Compiler : $(which ${CXX}) ($(${CXX} --version | head -n1))"
echo "  Stage 1  : ASan + LSan + UBSan + _GLIBCXX_ASSERTIONS + Stack Pattern Init"
echo "  Stage 2  : ThreadSanitizer (Data-Race & Deadlock Detection)"
echo "=========================================================="

cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DENABLE_ASAN=ON \
    -DENABLE_UBSAN=ON

ninja -C "${BUILD_DIR}"

echo "----------------------------------------------------------"
echo "  [Stage 1/2] Running Test Suite under Strict ASan + LSan + UBSan..."
echo "----------------------------------------------------------"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:strict_string_checks=1:detect_invalid_pointer_pairs=2:protect_shadow_gap=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export LSAN_OPTIONS="suppressions=${ROOT_DIR}/tests/lsan_suppressions.txt"

"${BUILD_DIR}/fpshud_test" 1

echo "----------------------------------------------------------"
echo "  [Stage 2/2] Building & Running Test Suite under ThreadSanitizer (TSan)..."
echo "----------------------------------------------------------"

cmake -B "${TSAN_BUILD_DIR}" -S "${ROOT_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DENABLE_TSAN=ON

ninja -C "${TSAN_BUILD_DIR}"

export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:history_size=7"

"${TSAN_BUILD_DIR}/fpshud_test" 1

echo "=========================================================="
echo "  [SUCCESS] 0 Leaks, 0 Overflows/UAF, 0 UB, 0 Data Races!"
echo "=========================================================="
