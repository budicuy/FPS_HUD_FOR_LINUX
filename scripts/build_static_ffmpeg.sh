#!/usr/bin/env bash
# scripts/build_static_ffmpeg.sh - Compiles ultra-minimal static FFmpeg (<4MB) for single-binary FPSHUD recorder
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUT_ARG="${1:-$PROJECT_DIR/build/ffmpeg_minimal}"
mkdir -p "$OUT_ARG"
OUT_DIR="$(cd "$OUT_ARG" && pwd)"
BUILD_CACHE="${FFMPEG_BUILD_CACHE:-/tmp/fpshud_ffmpeg_src_$(id -u)}"

if [ -f "$OUT_DIR/lib/libavformat.a" ] && [ -f "$OUT_DIR/lib/libavcodec.a" ] && \
   [ -f "$OUT_DIR/lib/libswscale.a" ] && [ -f "$OUT_DIR/lib/libswresample.a" ] && \
   [ -f "$OUT_DIR/lib/libavutil.a" ] && \
   (nm "$OUT_DIR/lib/libavformat.a" 2>/dev/null || true) | grep -q "ff_mov_demuxer"; then
    echo "[✓] Minimal static FFmpeg already built at: $OUT_DIR"
    exit 0
fi

for tool in curl git tar xz sha256sum make clang clang++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[!] ERROR: Utilitas build wajib '$tool' tidak ditemukan di PATH. Harap instal '$tool' terlebih dahulu." >&2
        exit 1
    fi
done

echo "=================================================================="
echo "  Building Minimal Hardware-Only Static FFmpeg (< 4 MB)"
echo "  Target Output: $OUT_DIR"
echo "=================================================================="

mkdir -p "$BUILD_CACHE"
chmod 700 "$BUILD_CACHE"
mkdir -p "$OUT_DIR"
export PKG_CONFIG_PATH="$OUT_DIR/lib/pkgconfig:$OUT_DIR/share/pkgconfig:${PKG_CONFIG_PATH:-}"

# 1. Install ffnvcodec headers if not present in system
if ! pkg-config --exists ffnvcodec 2>/dev/null; then
    echo "[*] Installing ffnvcodec (NVIDIA NVENC) headers to $OUT_DIR..."
    NV_CACHE="$BUILD_CACHE/nv_headers"
    rm -rf "$NV_CACHE"
    git clone --depth 1 https://git.videolan.org/git/ffmpeg/nv-codec-headers.git "$NV_CACHE"
    make -C "$NV_CACHE" install PREFIX="$OUT_DIR"
fi

cd "$BUILD_CACHE"

FFMPEG_VERSION="7.1"
FFMPEG_TAR="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TAR}"
FFMPEG_SHA256="40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6"

if [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
    if [ ! -f "$FFMPEG_TAR" ]; then
        echo "[*] Downloading FFmpeg ${FFMPEG_VERSION} source archive..."
        curl -fsSL "$FFMPEG_URL" -o "$FFMPEG_TAR" || {
            echo "[*] Fallback: Cloning shallow git release branch..."
            git clone --depth 1 -b "release/${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "ffmpeg-${FFMPEG_VERSION}"
        }
    fi
    if [ -f "$FFMPEG_TAR" ] && [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
        echo "[*] Verifying SHA256 checksum for ${FFMPEG_TAR}..."
        echo "${FFMPEG_SHA256}  ${FFMPEG_TAR}" | sha256sum -c - || {
            echo "[!] ERROR: SHA256 verification failed for ${FFMPEG_TAR}!" >&2
            rm -f "$FFMPEG_TAR"
            exit 1
        }
        echo "[*] Extracting ${FFMPEG_TAR}..."
        tar -xf "$FFMPEG_TAR"
    fi
fi

cd "ffmpeg-${FFMPEG_VERSION}"

FFMPEG_CC="${CC:-clang}"
FFMPEG_CXX="${CXX:-clang++}"
FFMPEG_AR_OPTS=()
if command -v llvm-ar >/dev/null 2>&1; then
    FFMPEG_AR_OPTS+=( "--ar=llvm-ar" )
fi
if command -v llvm-ranlib >/dev/null 2>&1; then
    FFMPEG_AR_OPTS+=( "--ranlib=llvm-ranlib" )
fi

echo "[*] Configuring ultra-minimal static FFmpeg (Hardware NVENC + VAAPI + MP4 only)..."
./configure \
    --prefix="$OUT_DIR" \
    --cc="$FFMPEG_CC" \
    --cxx="$FFMPEG_CXX" \
    "${FFMPEG_AR_OPTS[@]}" \
    --enable-static \
    --disable-shared \
    --disable-all \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-ffnvcodec \
    --enable-nvenc \
    --enable-vaapi \
    --enable-decoder=h264,hevc,aac \
    --enable-parser=h264,hevc,aac \
    --enable-encoder=h264_nvenc,hevc_nvenc,h264_vaapi,hevc_vaapi,aac \
    --enable-demuxer=mov,matroska \
    --enable-muxer=mp4,matroska,webm \
    --enable-protocol=file \
    --enable-hwaccel=h264_vaapi,hevc_vaapi \
    --disable-bzlib \
    --disable-lzma \
    --disable-x86asm \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --disable-xlib \
    --disable-vdpau \
    --disable-sdl2 \
    --disable-alsa \
    --disable-sndio \
    --disable-libxcb \
    --enable-small \
    --extra-cflags="-fPIC -ffunction-sections -fdata-sections" \
    --extra-cxxflags="-fPIC -ffunction-sections -fdata-sections"

echo "[*] Compiling minimal static FFmpeg with $(nproc) parallel threads..."
make -j"$(nproc)"
make install

echo "=================================================================="
echo "  Minimal Static FFmpeg Built Successfully!"
ls -lh "$OUT_DIR/lib"/*.a
echo "=================================================================="
