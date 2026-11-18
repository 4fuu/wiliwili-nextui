#!/bin/sh
# Configure and build wiliwili for NextUI on tg5040.
#
# Prerequisites: run scripts/nextui/build-deps.sh first (mpv/FFmpeg/libass into
# the toolchain sysroot). Run inside ghcr.io/loveretro/tg5040-toolchain.
#
# Environment:
#   JOBS=8          parallel build jobs (default: nproc)
#   MPV_NO_FB=1     build the MPV_NO_FB render path instead of the framebuffer
#                   (copy) path. Only needed if the device's PowerVR driver
#                   misbehaves with mpv's FBO; the default is the FBO path.

set -eu

: "${SYSROOT:?run inside the tg5040 toolchain image (SYSROOT is not set)}"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD="$ROOT/build-nextui"
JOBS=${JOBS:-$(nproc)}

EXTRA=""
if [ "${MPV_NO_FB:-0}" = "1" ]; then
    EXTRA="-DMPV_NO_FB=ON"
    echo "==> MPV_NO_FB render path enabled"
fi

cmake -B "$BUILD" -G Ninja -S "$ROOT" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-tg5040.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPLATFORM_DESKTOP=ON \
    -DUSE_SDL2=ON \
    -DUSE_SYSTEM_SDL2=ON \
    -DUSE_GLES3=ON \
    -DNEXTUI_TG5040=ON \
    $EXTRA

cmake --build "$BUILD" --target wiliwili -j "$JOBS"

echo "==> built:"
file "$BUILD/wiliwili"
