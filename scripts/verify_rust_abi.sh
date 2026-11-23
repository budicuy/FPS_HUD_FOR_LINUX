#!/usr/bin/env bash
# scripts/verify_rust_abi.sh — ELF ABI, DT_NEEDED, and GLIBC floor verification gate
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LIB_SO="${1:-${PROJECT_DIR}/target/release/libfpshud.so}"
UI_BIN="${2:-${PROJECT_DIR}/target/release/fpshud-ui}"
FFMPEG_BIN="${3:-${PROJECT_DIR}/target/release/fpshud-ffmpeg}"

for artifact in "$LIB_SO" "$UI_BIN"; do
    if [ ! -f "$artifact" ]; then
        echo "[!] ERROR: Release artifact not found: $artifact" >&2
        exit 1
    fi
done

echo "=================================================================="
echo "  FPSHUD Pure-Rust ELF ABI & Portability Gate"
echo "=================================================================="

# 0. Zero repository C/C++ source files gate
C_FILES="$(find "${PROJECT_DIR}" -path "${PROJECT_DIR}/target" -prune -o -path "${PROJECT_DIR}/.git" -prune -o \
    \( -name '*.c' -o -name '*.cc' -o -name '*.cxx' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print)"
if [ -n "$C_FILES" ]; then
    echo "[!] ERROR: Unexpected C/C++ source files remain in Pure-Rust workspace:" >&2
    printf '%s\n' "$C_FILES" >&2
    exit 1
fi
echo "[✓] Pure-Rust source gate PASSED: 0 .c/.cpp/.h/.hpp files in repository."

# 1. Exact 8 exported C-ABI symbols matching manifest/libfpshud.version
EXPECTED_EXPORTS=(
    "eglGetProcAddress"
    "eglSwapBuffers"
    "glXGetProcAddress"
    "glXGetProcAddressARB"
    "glXSwapBuffers"
    "vkGetDeviceProcAddr"
    "vkGetInstanceProcAddr"
    "vkNegotiateLoaderLayerInterfaceVersion"
)

ACTUAL_EXPORTS="$(nm -D --defined-only "$LIB_SO" | awk '$2 == "T" || $2 == "W" { print $3 }' | LC_ALL=C sort)"
EXPECTED_SORTED="$(printf '%s\n' "${EXPECTED_EXPORTS[@]}" | LC_ALL=C sort)"

if [ "$ACTUAL_EXPORTS" != "$EXPECTED_SORTED" ]; then
    echo "[!] ERROR: Exported ABI symbol mismatch on $LIB_SO!" >&2
    echo "Expected:" >&2
    printf '%s\n' "$EXPECTED_SORTED" >&2
    echo "Actual:" >&2
    printf '%s\n' "$ACTUAL_EXPORTS" >&2
    exit 1
fi
echo "[✓] ABI symbol gate PASSED: exactly 8 expected Vulkan/GLX/EGL entrypoints exported."

# 2. Zero banned DT_NEEDED graphics/X11/Wayland/font/vendor/C++ runtime libraries
BANNED_NEEDED='lib(GL|GLX|EGL|OpenGL|X11|xcb|wayland-client|fontconfig|freetype|xkbcommon|nvidia-ml|ze_loader|stdc\+\+)(\.|$)'
for artifact in "$LIB_SO" "$UI_BIN"; do
    HITS="$(LC_ALL=C readelf -d "$artifact" | grep 'NEEDED' | grep -E "$BANNED_NEEDED" || true)"
    if [ -n "$HITS" ]; then
        echo "[!] ERROR: Banned DT_NEEDED dependencies found in $(basename "$artifact"):" >&2
        printf '%s\n' "$HITS" >&2
        exit 1
    fi
done
echo "[✓] Zero banned DT_NEEDED gate PASSED on libfpshud.so & fpshud-ui."

# 3. GLIBC version floor check (target baseline: <= GLIBC_2.35)
BAD_LIB_HARD="$(objdump -T "$LIB_SO" | grep -E 'GLIBC_2\.(3[6-9]|[4-9][0-9])' | grep -v '  w  ' || true)"
if [ -n "$BAD_LIB_HARD" ]; then
    echo "[!] ERROR: libfpshud.so has non-weak GLIBC >= 2.36 symbol dependencies:" >&2
    printf '%s\n' "$BAD_LIB_HARD" >&2
    exit 1
fi

BAD_ANY="$(objdump -T "$LIB_SO" "$UI_BIN" | grep -E 'GLIBC_2\.(3[6-9]|[4-9][0-9])' || true)"
BAD_UI_HARD="$(objdump -T "$UI_BIN" | grep -E 'GLIBC_2\.(3[6-9]|[4-9][0-9])' | grep -v '  w  ' || true)"

HOST_GLIBC="$(ldd --version 2>/dev/null | head -n 1 | awk '{print $NF}' || true)"
[ -n "$HOST_GLIBC" ] || HOST_GLIBC="0.0"

STRICT_GLIBC="${FPSHUD_STRICT_GLIBC:-0}"
if [ -n "$HOST_GLIBC" ] && printf '%s\n2.35\n' "$HOST_GLIBC" | sort -VC 2>/dev/null; then
    STRICT_GLIBC=1
fi

if [ -n "$BAD_ANY" ]; then
    if [ "$STRICT_GLIBC" = "1" ] || [ -n "${CI:-}" ]; then
        echo "[!] ERROR: GLIBC >= 2.36 symbol versions found under strict GLIBC_2.35 gate:" >&2
        printf '%s\n' "$BAD_ANY" >&2
        exit 1
    fi
    echo "[i] Host glibc=$HOST_GLIBC linked newer libm/weak symbols in fpshud-ui (host non-strict mode):"
    if [ -n "$BAD_UI_HARD" ]; then
        printf '%s\n' "$BAD_UI_HARD" | sed 's/^/    /'
    fi
    echo "[i] Note: libfpshud.so has 0 hard GLIBC >= 2.36 symbols; CI Ubuntu 22.04 / cargo-zigbuild enforces FPSHUD_STRICT_GLIBC=1."
else
    echo "[✓] GLIBC <= 2.35 portability gate PASSED (0 symbols >= GLIBC_2.36)."
fi

# 4. Bundled fpshud-ffmpeg portability gate (when present or required)
if [ -f "$FFMPEG_BIN" ] || [ "${FPSHUD_REQUIRE_FFMPEG:-0}" = "1" ]; then
    if [ ! -x "$FFMPEG_BIN" ]; then
        echo "[!] ERROR: Required bundled fpshud-ffmpeg missing or not executable: $FFMPEG_BIN" >&2
        exit 1
    fi
    FF_NEEDED="$(LC_ALL=C readelf -d "$FFMPEG_BIN" | grep 'NEEDED' | sed -E 's/.*\[([^]]+)\].*/\1/')"
    FF_BANNED="$(printf '%s\n' "$FF_NEEDED" | grep -E 'libstdc\+\+|libx264|libx265|libavcodec|libavformat|libavutil|libavfilter|libswscale|libswresample' || true)"
    if [ -n "$FF_BANNED" ]; then
        echo "[!] ERROR: Banned DT_NEEDED dependencies found in fpshud-ffmpeg:" >&2
        printf '%s\n' "$FF_BANNED" >&2
        exit 1
    fi
    FF_ALLOWED_RE='^(libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|libgcc_s\.so\.1|ld-linux-x86-64\.so\.2|libva\.so\.2|libva-drm\.so\.2|libdrm\.so\.2|libpulse\.so\.0|libpulse-simple\.so\.0)$'
    FF_UNEXPECTED="$(printf '%s\n' "$FF_NEEDED" | grep -Ev "$FF_ALLOWED_RE" || true)"
    if [ -n "$FF_UNEXPECTED" ]; then
        echo "[!] ERROR: Unexpected DT_NEEDED dependencies in fpshud-ffmpeg (outside allowlist):" >&2
        printf '%s\n' "$FF_UNEXPECTED" >&2
        exit 1
    fi
    echo "[✓] Bundled fpshud-ffmpeg DT_NEEDED allowlist gate PASSED (0 libstdc++, 0 libx264.so, 0 libav*.so)."

    BAD_FF="$(objdump -T "$FFMPEG_BIN" | grep -E 'GLIBC_2\.(3[6-9]|[4-9][0-9])' || true)"
    if [ -n "$BAD_FF" ]; then
        if [ "$STRICT_GLIBC" = "1" ] || [ -n "${CI:-}" ]; then
            echo "[!] ERROR: fpshud-ffmpeg has GLIBC >= 2.36 symbol versions under strict gate:" >&2
            printf '%s\n' "$BAD_FF" >&2
            exit 1
        fi
        echo "[i] Host glibc=$HOST_GLIBC linked newer symbols in fpshud-ffmpeg (host non-strict mode)."
    else
        echo "[✓] Bundled fpshud-ffmpeg GLIBC <= 2.35 portability gate PASSED."
    fi
fi
