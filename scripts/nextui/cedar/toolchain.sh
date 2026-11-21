#!/bin/sh
# Sets up the tg5040 cross toolchain for a full wiliwili build (see
# nextui-tg5040-cedar.yml):
#
#   eval "$(./scripts/nextui/cedar/toolchain.sh)"
#
# Why not the vendor SDK alone: its Linaro 7.5 sysroot has glibc 2.25, which
# has no sem_clockwait() that libavcodec (via libmpv) needs, and ld 2.34 has
# no --export-dynamic-symbol that libsbm.so needs to bind to our SemTimedWait.
# The vendor sysroot stays the base (SDL2, GLES, ALSA, OpenSSL); Bootlin's
# aarch64 glibc 2.31 toolchain provides the C library and the linker.
set -eu

: "${SYSROOT:?toolchain.sh: SYSROOT is not set (source this inside the tg5040 toolchain image)}"

CACHE=${CEDAR_TOOLCHAIN_DIR:-/opt/cedar-toolchain}
BOOTLIN=aarch64--glibc--stable-2020.08-1
BOOTLIN_SHA=8ab7a2f17cb96621b048ab0a872650dd62faa54cd74c961b9902b8c04bff7dd1
URL="https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$BOOTLIN.tar.bz2"

mkdir -p "$CACHE"
if [ ! -x "$CACHE/$BOOTLIN/bin/aarch64-buildroot-linux-gnu-gcc" ]; then
    echo "==> download $BOOTLIN" >&2
    curl -sfL -o "$CACHE/$BOOTLIN.tar.bz2" "$URL"
    echo "$BOOTLIN_SHA  $CACHE/$BOOTLIN.tar.bz2" | sha256sum -c -
    tar xjf "$CACHE/$BOOTLIN.tar.bz2" -C "$CACHE"
fi

BOOT="$CACHE/$BOOTLIN"
BOOT_SYSROOT="$BOOT/aarch64-buildroot-linux-gnu/sysroot"
VENDOR_BIN=$(dirname "$(command -v "$CROSS_TRIPLE-gcc")")

# Bootlin's ld (2.33) cannot --export-dynamic-symbol, so libsbm binds to
# SemTimedWait through -Wl,-E on wiliwili (CMakeLists.txt, NEXTUI_TG5040).
#
# Every link goes through these: Bootlin's glibc 2.31 (the one the compiler
# was built against) together with the C library's startup objects from the
# vendor sysroot, which also produced the program's crt objects. Both sides
# define a few symbols, hence --allow-multiple-definition. libm is Bootlin's
# outright: the vendor's 2.25 libm lacks pow@GLIBC_2.29 that libmpv uses.
mkdir -p "$CACHE/lib"
printf 'GROUP ( %s )\n' \
    "$BOOT_SYSROOT/lib/libc.so.6 $BOOT_SYSROOT/lib/ld-linux-aarch64.so.1" \
    "AS_NEEDED ( $BOOT_SYSROOT/usr/lib/libc_nonshared.a )" >"$CACHE/lib/libc.so"
# The shared libs under $SYSROOT/usr/lib (OpenSSL, SDL2) NEED libpthread.so.0,
# so the script must carry that SONAME; otherwise ld records the path.
printf 'GROUP ( %s )\n' \
    "$BOOT_SYSROOT/lib/libpthread.so.0 $BOOT_SYSROOT/lib/libc.so.6" \
    >"$CACHE/lib/libpthread.so"
printf 'GROUP ( %s )\n' "$BOOT_SYSROOT/lib/libm.so.6" >"$CACHE/lib/libm.so"

# The vendor sysroot's libc.so/libpthread.so scripts would send us back to its
# glibc 2.25, and its -L directories are searched before ours. Replace both
# with the scripts above, and point the libc.so.6/libpthread.so.0 symlinks at
# the libraries the scripts name so both resolve to the same files.
for lib in libc libpthread libm; do
    for dir in "$SYSROOT/lib" "$SYSROOT/usr/lib"; do
        if [ -e "$dir/$lib.so" ] && [ ! -e "$dir/$lib.so.vendor" ]; then
            mv "$dir/$lib.so" "$dir/$lib.so.vendor"
        fi
        ln -sf "$CACHE/lib/$lib.so" "$dir/$lib.so"
    done
done
for so in libc.so.6 libpthread.so.0 libm.so.6; do
    for dir in "$SYSROOT/lib" "$SYSROOT/usr/lib"; do
        if [ -e "$dir/$so" ] && [ ! -e "$dir/$so.vendor" ]; then
            mv "$dir/$so" "$dir/$so.vendor"
        fi
        ln -sf "$BOOT_SYSROOT/lib/$so" "$dir/$so"
    done
done

# Keep CROSS_TRIPLE and CROSS_ROOT on the vendor toolchain: build-deps.sh and
# CMake find the sysroot and compiler through them, and make-pak.sh stages
# libstdc++/libgcc_s from $CROSS_TRIPLE-g++ out of that sysroot. Only the C
# library and ld come from Bootlin (prepended to PATH).
echo "export PATH=$BOOT/bin:$VENDOR_BIN:\$PATH"
echo "export CFLAGS=\"-L$CACHE/lib\""
echo "export CXXFLAGS=\"-L$CACHE/lib\""
echo "export LDFLAGS=\"-Wl,--allow-multiple-definition -L$CACHE/lib\""
