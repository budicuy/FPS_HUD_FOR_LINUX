#!/usr/bin/env bash
# scripts/build_bundled_ffmpeg.sh — Reproducible zero-sudo minimal FFmpeg n9.1 builder for FPSHUD
# Builds a single stripped companion binary: target/release/fpshud-ffmpeg
# Zero C/C++ files are placed outside target/ffmpeg-build/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="${PROJECT_DIR}/target/ffmpeg-build"
SRC_DIR="${BUILD_ROOT}/src"
SYSROOT="${BUILD_ROOT}/sysroot"
TOOLS_BIN="${BUILD_ROOT}/tools/bin"
RELEASE_DIR="${PROJECT_DIR}/target/release"
DIST_DIR="${PROJECT_DIR}/dist"
OUT_BIN="${RELEASE_DIR}/fpshud-ffmpeg"
OUT_LICENSE="${RELEASE_DIR}/LICENSE.ffmpeg-gpl"
STAMP_FILE="${BUILD_ROOT}/.build-stamp"

# Pinned upstream versions
FFMPEG_VER="9.1"
FFMPEG_TAG="n9.1"
NV_CODEC_TAG="n12.2.72.0"
VULKAN_HEADERS_TAG="v1.4.309"
X264_COMMIT="31e19f92f00c7003fa115047ce50978bc98c3a0d"
NASM_VER="2.16.03"

BUILD_STAMP="ffmpeg=${FFMPEG_TAG}|ffnvcodec=${NV_CODEC_TAG}|vkheaders=${VULKAN_HEADERS_TAG}|x264=${X264_COMMIT}|abi=v2"

if [ "${FPSHUD_REBUILD_FFMPEG:-0}" != "1" ] \
    && [ -x "$OUT_BIN" ] \
    && [ -f "$OUT_LICENSE" ] \
    && [ -f "$STAMP_FILE" ] \
    && [ "$(cat "$STAMP_FILE")" = "$BUILD_STAMP" ]; then
    echo "[✓] Bundled fpshud-ffmpeg (${FFMPEG_TAG}) already up-to-date: ${OUT_BIN}"
    if [ "${FPSHUD_PACKAGE_FFMPEG_SOURCES:-0}" = "1" ] && [ ! -f "${DIST_DIR}/fpshud-ffmpeg-sources.tar.gz" ]; then
        mkdir -p "$DIST_DIR"
        tar -czf "${DIST_DIR}/fpshud-ffmpeg-sources.tar.gz" \
            -C "$BUILD_ROOT" src \
            -C "$PROJECT_DIR" scripts/build_bundled_ffmpeg.sh
        echo "[✓] Packaged GPLv2 corresponding sources: ${DIST_DIR}/fpshud-ffmpeg-sources.tar.gz"
    fi
    exit 0
fi

NPROC="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$SRC_DIR" "$SYSROOT/include" "$SYSROOT/lib/pkgconfig" "$TOOLS_BIN" "$RELEASE_DIR"

export PATH="${TOOLS_BIN}:${PATH}"
export PKG_CONFIG_PATH="${SYSROOT}/lib/pkgconfig:${SYSROOT}/share/pkgconfig:${PKG_CONFIG_PATH:-}"

download_file() {
    local url="$1"
    local dest="$2"
    if [ -f "$dest" ]; then
        return 0
    fi
    echo "[*] Downloading $(basename "$dest")..."
    curl -fL --retry 3 --retry-delay 2 "$url" -o "${dest}.tmp"
    mv -f "${dest}.tmp" "$dest"
}

# 1. Ensure nasm >= 2.13 is available (zero-sudo local bootstrap if missing)
if ! command -v nasm >/dev/null 2>&1; then
    echo "[*] nasm not found in PATH; bootstrapping nasm ${NASM_VER} locally..."
    NASM_TAR="${SRC_DIR}/nasm-${NASM_VER}.tar.xz"
    download_file "https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VER}/nasm-${NASM_VER}.tar.xz" "$NASM_TAR"
    rm -rf "${BUILD_ROOT}/nasm-work"
    mkdir -p "${BUILD_ROOT}/nasm-work"
    tar -xf "$NASM_TAR" -C "${BUILD_ROOT}/nasm-work" --strip-components=1
    (
        cd "${BUILD_ROOT}/nasm-work"
        ./configure --prefix="${BUILD_ROOT}/tools" >/dev/null
        make -j"$NPROC" nasm >/dev/null
        install -m 755 nasm "${TOOLS_BIN}/nasm"
    )
    rm -rf "${BUILD_ROOT}/nasm-work"
    echo "[✓] Bootstrapped local nasm: $(nasm -v)"
fi

# 2. Header-only nv-codec-headers (dlopen NVENC, 0 DT_NEEDED)
NV_TAR="${SRC_DIR}/nv-codec-headers-${NV_CODEC_TAG}.tar.gz"
download_file "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/${NV_CODEC_TAG}.tar.gz" "$NV_TAR"
rm -rf "${BUILD_ROOT}/nv-work"
mkdir -p "${BUILD_ROOT}/nv-work"
tar -xf "$NV_TAR" -C "${BUILD_ROOT}/nv-work" --strip-components=1
make -C "${BUILD_ROOT}/nv-work" install PREFIX="$SYSROOT" >/dev/null
rm -rf "${BUILD_ROOT}/nv-work"

# 3. Header-only KhronosGroup/Vulkan-Headers (>= 1.3.277 for FFmpeg n9.1 --enable-vulkan on Ubuntu 22.04)
VK_TAR="${SRC_DIR}/Vulkan-Headers-${VULKAN_HEADERS_TAG}.tar.gz"
download_file "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_HEADERS_TAG}.tar.gz" "$VK_TAR"
rm -rf "${BUILD_ROOT}/vk-work"
mkdir -p "${BUILD_ROOT}/vk-work"
tar -xf "$VK_TAR" -C "${BUILD_ROOT}/vk-work" --strip-components=1
cp -a "${BUILD_ROOT}/vk-work/include/." "${SYSROOT}/include/"
rm -rf "${BUILD_ROOT}/vk-work"

# 4. Static 8-bit x264 (C/ASM only, 0 libstdc++, 0 libx264.so DT_NEEDED)
X264_TAR="${SRC_DIR}/x264-${X264_COMMIT}.tar.gz"
if [ ! -f "$X264_TAR" ]; then
    if ! download_file "https://code.videolan.org/videolan/x264/-/archive/${X264_COMMIT}/x264-${X264_COMMIT}.tar.gz" "$X264_TAR"; then
        download_file "https://github.com/mirror/x264/archive/${X264_COMMIT}.tar.gz" "$X264_TAR"
    fi
fi
if [ ! -f "${SYSROOT}/lib/libx264.a" ]; then
    echo "[*] Building static 8-bit libx264..."
    rm -rf "${BUILD_ROOT}/x264-work"
    mkdir -p "${BUILD_ROOT}/x264-work"
    tar -xf "$X264_TAR" -C "${BUILD_ROOT}/x264-work" --strip-components=1
    (
        cd "${BUILD_ROOT}/x264-work"
        ./configure \
            --prefix="$SYSROOT" \
            --enable-static \
            --disable-shared \
            --disable-cli \
            --bit-depth=8 \
            --chroma-format=420 \
            --enable-pic \
            --disable-opencl \
            --disable-avs \
            --disable-swscale \
            --disable-lavf \
            --disable-ffms \
            --disable-gpac \
            --disable-lsmash >/dev/null
        make -j"$NPROC" >/dev/null
        make install >/dev/null
    )
    rm -rf "${BUILD_ROOT}/x264-work"
    echo "[✓] Built static libx264: ${SYSROOT}/lib/libx264.a"
fi

# 5. Minimal FFmpeg n9.1 (static libav* + static libx264 + NVENC/VA-API/Vulkan/Pulse)
FF_TAR="${SRC_DIR}/ffmpeg-${FFMPEG_VER}.tar.xz"
download_file "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz" "$FF_TAR"

echo "[*] Configuring & building minimal FFmpeg ${FFMPEG_TAG}..."
rm -rf "${BUILD_ROOT}/ffmpeg-work"
mkdir -p "${BUILD_ROOT}/ffmpeg-work"
tar -xf "$FF_TAR" -C "${BUILD_ROOT}/ffmpeg-work" --strip-components=1

(
    cd "${BUILD_ROOT}/ffmpeg-work"
    ./configure \
        --prefix="$SYSROOT" \
        --pkg-config-flags="--static" \
        --extra-cflags="-I${SYSROOT}/include -O3" \
        --extra-ldflags="-L${SYSROOT}/lib -Wl,--as-needed" \
        --disable-everything \
        --disable-all \
        --disable-doc \
        --disable-debug \
        --disable-autodetect \
        --disable-network \
        --disable-ffplay \
        --disable-ffprobe \
        --enable-gpl \
        --enable-ffmpeg \
        --enable-avcodec \
        --enable-avformat \
        --enable-avutil \
        --enable-swscale \
        --enable-swresample \
        --enable-avfilter \
        --enable-avdevice \
        --enable-pthreads \
        --enable-nvenc \
        --enable-nvdec \
        --enable-hwaccel=h264_nvdec,hevc_nvdec,h264_vaapi,hevc_vaapi,h264_vulkan,hevc_vulkan \
        --enable-ffnvcodec \
        --enable-vaapi \
        --enable-vulkan \
        --enable-libpulse \
        --enable-libx264 \
        --enable-encoder=libx264,h264_nvenc,hevc_nvenc,h264_vaapi,hevc_vaapi,h264_vulkan,hevc_vulkan,aac,rawvideo,pcm_s16le \
        --enable-decoder=h264,hevc,vp8,vp9,rawvideo,aac,opus,mp3,vorbis,flac,pcm_s16le,pcm_s24le,pcm_f32le,wrapped_avframe \
        --enable-parser=h264,hevc,vp8,vp9,aac,opus,mpegaudio,vorbis,flac \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
        --enable-muxer=mp4,rawvideo,null,pulse \
        --enable-demuxer=rawvideo,mov,matroska,pulse,lavfi \
        --enable-protocol=file,pipe \
        --enable-indev=pulse,lavfi \
        --enable-outdev=pulse \
        --enable-filter=scale,format,vflip,setparams,amix,alimiter,aresample,hwupload,hwdownload,crop,scale_vaapi,fps,null,anull,color,aformat \
        >/dev/null
    make -j"$NPROC" ffmpeg >/dev/null
    cp -f ffmpeg "$OUT_BIN"
    cp -f COPYING.GPLv2 "$OUT_LICENSE"
)

STRIP_BIN="$(command -v llvm-strip || command -v strip || true)"
if [ -n "$STRIP_BIN" ]; then
    "$STRIP_BIN" --strip-unneeded "$OUT_BIN"
fi
chmod 755 "$OUT_BIN"
echo "$BUILD_STAMP" > "$STAMP_FILE"
rm -rf "${BUILD_ROOT}/ffmpeg-work"

echo "[✓] Built minimal bundled fpshud-ffmpeg (${FFMPEG_TAG}): ${OUT_BIN} ($(du -h "$OUT_BIN" | awk '{print $1}'))"

if [ "${FPSHUD_PACKAGE_FFMPEG_SOURCES:-0}" = "1" ]; then
    mkdir -p "$DIST_DIR"
    tar -czf "${DIST_DIR}/fpshud-ffmpeg-sources.tar.gz" \
        -C "$BUILD_ROOT" src \
        -C "$PROJECT_DIR" scripts/build_bundled_ffmpeg.sh
    echo "[✓] Packaged GPLv2 corresponding sources: ${DIST_DIR}/fpshud-ffmpeg-sources.tar.gz"
fi
