#!/bin/sh
# Build the third-party runtime stack for the NextUI (tg5040) port.
#
# Run inside ghcr.io/loveretro/tg5040-toolchain:
#
#   ./scripts/nextui/build-deps.sh
#
# Output goes into the vendor SDK sysroot ($SYSROOT/usr), the same prefix the
# SDK's SDL2/GLES/ALSA/OpenSSL live in, so pkg-config and CMake need only one
# search path. Everything is built from source here; no prebuilt community
# binary is used.
#
# Licensing: every component is built without --enable-nonfree and without
# any nonfree external library. FFmpeg uses its native AAC decoder; no fdk-aac,
# no nvenc, no libfdk. See NEXTUI.md for the exact flag rationale.

set -eu

: "${SYSROOT:?run inside the tg5040 toolchain image (SYSROOT is not set)}"
: "${CROSS_TRIPLE:?run inside the tg5040 toolchain image (CROSS_TRIPLE is not set)}"
: "${CROSS_ROOT:?run inside the tg5040 toolchain image (CROSS_ROOT is not set)}"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PREFIX="$SYSROOT/usr"
WORK="$ROOT/.nextui-build"
SRC="$WORK/src"
BUILD="$WORK/deps"
JOBS=${JOBS:-$(nproc)}
CROSS="${CROSS_TRIPLE}-"

FFMPEG_VERSION=6.1.1
MPV_VERSION=0.36.0
LIBASS_VERSION=0.17.5
HARFBUZZ_VERSION=7.3.0
FRIBIDI_VERSION=1.0.16

mkdir -p "$SRC" "$BUILD"

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"

fetch() {
    # fetch <url> <file name> <sha256>
    url=$1
    name=$2
    sum=$3
    if [ ! -f "$SRC/$name" ]; then
        echo "==> download $name"
        wget -q -O "$SRC/$name.part" "$url"
        mv "$SRC/$name.part" "$SRC/$name"
    fi
    echo "$sum  $SRC/$name" | sha256sum -c -
}

unpack() {
    # unpack <file name> <destination dir>
    if [ ! -d "$SRC/$2" ]; then
        echo "==> unpack $1"
        tar xf "$SRC/$1" -C "$SRC"
    fi
}

# Meson cannot infer a cross sysroot from the compiler alone, and the image's
# pkg-config environment points at the sysroot only for shells that exported
# it. Write the cross file explicitly so harfbuzz/fribidi/libass/mpv all see
# the same prefix.
MESON_CROSS="$BUILD/tg5040-cross.txt"
cat >"$MESON_CROSS" <<EOF
[binaries]
c = '${CROSS}gcc'
cpp = '${CROSS}g++'
ar = '${CROSS}ar'
strip = '${CROSS}strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'

[properties]
sys_root = '${SYSROOT}'
pkg_config_libdir = '${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig'
needs_exe_wrapper = true

[built-in options]
c_args = ['-mcpu=cortex-a53', '-O2']
cpp_args = ['-mcpu=cortex-a53', '-O2']
EOF

meson_install() {
    meson install -C "$1" --destdir "$SYSROOT" --quiet
}

meson_setup() {
    # meson_setup <build dir> <source dir> [extra setup args...]
    dir=$1
    src=$2
    shift 2
    rm -rf "$dir"
    meson setup "$dir" "$src" --cross-file "$MESON_CROSS" --prefix /usr --buildtype release "$@"
}

build_fribidi() {
    echo "==> fribidi $FRIBIDI_VERSION"
    fetch "https://github.com/fribidi/fribidi/releases/download/v$FRIBIDI_VERSION/fribidi-$FRIBIDI_VERSION.tar.xz" \
        "fribidi-$FRIBIDI_VERSION.tar.xz" \
        1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
    unpack "fribidi-$FRIBIDI_VERSION.tar.xz" "fribidi-$FRIBIDI_VERSION"
    meson_setup "$BUILD/fribidi" "$SRC/fribidi-$FRIBIDI_VERSION" \
        -Ddocs=false -Dtests=false -Dbin=false
    meson compile -C "$BUILD/fribidi" -j "$JOBS"
    meson_install "$BUILD/fribidi"
}

build_harfbuzz() {
    echo "==> harfbuzz $HARFBUZZ_VERSION"
    fetch "https://github.com/harfbuzz/harfbuzz/releases/download/$HARFBUZZ_VERSION/harfbuzz-$HARFBUZZ_VERSION.tar.xz" \
        "harfbuzz-$HARFBUZZ_VERSION.tar.xz" \
        20770789749ac9ba846df33983dbda22db836c70d9f5d050cb9aa5347094a8fb
    unpack "harfbuzz-$HARFBUZZ_VERSION.tar.xz" "harfbuzz-$HARFBUZZ_VERSION"
    # libass only needs shaping + FriBidi; skip FreeType/GLib/ICU so the runtime
    # closure stays small and does not depend on the device's fontconfig.
    meson_setup "$BUILD/harfbuzz" "$SRC/harfbuzz-$HARFBUZZ_VERSION" \
        -Dfreetype=disabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
        -Dchafa=disabled -Dicu=disabled -Dgraphite2=disabled \
        -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dintrospection=disabled
    meson compile -C "$BUILD/harfbuzz" -j "$JOBS"
    meson_install "$BUILD/harfbuzz"
}

build_libass() {
    echo "==> libass $LIBASS_VERSION"
    fetch "https://github.com/libass/libass/releases/download/$LIBASS_VERSION/libass-$LIBASS_VERSION.tar.xz" \
        "libass-$LIBASS_VERSION.tar.xz" \
        2dca25c0e0c837ddf00b52011b3f82cac1e4ddd3ad018227806b0c2288864acc
    unpack "libass-$LIBASS_VERSION.tar.xz" "libass-$LIBASS_VERSION"
    # The device has no fontconfig and no system fonts. wiliwili renders
    # danmaku itself, so libass is only a libmpv build dependency; embedded
    # fonts are enough and skipping fontconfig keeps the runtime closure free
    # of /etc/fonts expectations.
    meson_setup "$BUILD/libass" "$SRC/libass-$LIBASS_VERSION" \
        -Dfontconfig=disabled -Drequire-system-font-provider=false \
        -Dharfbuzz=enabled -Dfribidi=enabled -Dasm=enabled \
        -Dtest=disabled -Dcompare=disabled -Dprofile=disabled -Dfuzz=disabled -Dcheckasm=disabled
    meson compile -C "$BUILD/libass" -j "$JOBS"
    meson_install "$BUILD/libass"
}

build_ffmpeg() {
    echo "==> ffmpeg $FFMPEG_VERSION (software decode only, no nonfree)"
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" \
        "ffmpeg-$FFMPEG_VERSION.tar.xz" \
        8684f4b00f94b85461884c3719382f1261f0d9eb3d59640a1f4ac0873616f968
    unpack "ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION"
    mkdir -p "$BUILD/ffmpeg"
    cd "$BUILD/ffmpeg"
    "$SRC/ffmpeg-$FFMPEG_VERSION/configure" \
        --prefix=/usr \
        --cross-prefix="$CROSS" \
        --sysroot="$SYSROOT" \
        --arch=aarch64 --cpu=cortex-a53 --target-os=linux \
        --enable-cross-compile --enable-pic --enable-neon \
        --disable-doc --disable-debug --disable-programs \
        --disable-avdevice --disable-postproc \
        --disable-encoders --disable-muxers --disable-hwaccels \
        --enable-shared --disable-static \
        --enable-zlib --enable-openssl \
        --disable-protocols \
        --enable-protocol=file,http,https,tcp,udp,hls,tls,httpproxy,data,concat,crypto \
        --disable-filters \
        --enable-filter=format,scale,fps,crop,pad,transpose,hflip,vflip,aresample,anull,atrim,asetpts,setpts
    make -j "$JOBS"
    make DESTDIR="$SYSROOT" install
    cd "$ROOT"
}

build_mpv() {
    echo "==> mpv $MPV_VERSION (libmpv, GL render API, software decode)"
    fetch "https://github.com/mpv-player/mpv/archive/v$MPV_VERSION.tar.gz" \
        "mpv-$MPV_VERSION.tar.gz" \
        29abc44f8ebee013bb2f9fe14d80b30db19b534c679056e4851ceadf5a5e8bf6
    unpack "mpv-$MPV_VERSION.tar.gz" "mpv-$MPV_VERSION"
    # No libplacebo: mpv 0.36's vo=gpu renderer is what libmpv's OpenGL render
    # API uses, and gpu-next would only add another GLSL/Vulkan-sized surface.
    # No X11/Wayland/DRM/Vulkan/VAAPI: the device has none of them.
    meson_setup "$BUILD/mpv" "$SRC/mpv-$MPV_VERSION" \
        --default-library shared \
        -Dlibmpv=true -Dcplayer=false -Dtests=false -Dbuild-date=false \
        -Dlua=disabled -Djavascript=disabled -Dlibarchive=disabled \
        -Dlibbluray=disabled -Ddvdnav=disabled -Dcdda=disabled -Ddvbin=disabled -Dcaca=disabled \
        -Dgl=enabled -Dplain-gl=enabled -Degl=disabled -Dvulkan=disabled \
        -Dshaderc=disabled -Dspirv-cross=disabled \
        -Dlibplacebo=disabled -Dlibplacebo-next=disabled \
        -Dwayland=disabled -Dx11=disabled -Ddrm=disabled -Dgbm=disabled \
        -Dvaapi=disabled -Dvdpau=disabled -Dcuda-hwaccel=disabled -Dcuda-interop=disabled \
        -Dvideotoolbox=disabled -Dmacos-cocoa-cb=disabled -Drpi=disabled -Duwp=disabled \
        -Dalsa=enabled -Dpulse=disabled -Dpipewire=disabled -Djack=disabled -Dopenal=disabled \
        -Dsdl2=enabled -Dsdl2-audio=enabled -Dsdl2-video=disabled -Dsdl2-gamepad=disabled \
        -Duchardet=disabled -Dzimg=disabled -Drubberband=disabled -Dlcms2=disabled -Djpeg=disabled \
        -Dvapoursynth=disabled -Dsixel=disabled -Doss-audio=disabled -Dsndio=disabled
    meson compile -C "$BUILD/mpv" -j "$JOBS"
    meson_install "$BUILD/mpv"
}

build_fribidi
build_harfbuzz
build_libass
build_ffmpeg
build_mpv

echo "==> installed runtime stack:"
ls -l "$PREFIX/lib"/libmpv.so* "$PREFIX/lib"/libavcodec.so* "$PREFIX/lib"/libass.so* 2>/dev/null || true
"${CROSS}readelf" -d "$PREFIX/lib/libmpv.so.2" 2>/dev/null | grep NEEDED || true
