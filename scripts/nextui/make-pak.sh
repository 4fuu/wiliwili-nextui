#!/bin/sh
# Assemble the NextUI pak: binary, resources, bundled libraries and CA bundle,
# then zip the pak contents into WiliWili.pakz (the shape the NextUI app
# installer expects: launch.sh at the archive root).
#
# Run after scripts/nextui/build-wiliwili.sh inside the tg5040 toolchain image.

set -eu

: "${SYSROOT:?run inside the tg5040 toolchain image (SYSROOT is not set)}"
: "${CROSS_TRIPLE:?run inside the tg5040 toolchain image (CROSS_TRIPLE is not set)}"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD="$ROOT/build-nextui"
DIST="$ROOT/dist"
PAK="$DIST/WiliWili.pak"
TEMPLATE="$ROOT/Tools/tg5040/WiliWili.pak"

if [ ! -x "$BUILD/wiliwili" ]; then
    echo "make-pak: $BUILD/wiliwili is missing; run scripts/nextui/build-wiliwili.sh first" >&2
    exit 1
fi

rm -rf "$PAK"
mkdir -p "$PAK/bin" "$PAK/lib" "$PAK/res"

cp "$TEMPLATE/launch.sh" "$TEMPLATE/pak.json" "$TEMPLATE/icon.png" "$PAK/"
chmod 755 "$PAK/launch.sh"

install -m 755 "$BUILD/wiliwili" "$PAK/bin/wiliwili"

# Resources are loaded relative to the working directory (BRLS_RESOURCES is
# "./resources/"), so keep them next to launch.sh.
cp -a "$BUILD/resources" "$PAK/resources"

# libstdc++/libgcc from the cross toolchain: the firmware's C++ runtime is
# older than what gcc 8.3 links against, and bundling keeps the pak
# independent of the stock Qt install.
CXX="${CROSS_TRIPLE}-g++"
cp -L "$("$CXX" -print-file-name=libstdc++.so.6)" "$PAK/lib/libstdc++.so.6"
cp -L "$("$CXX" -print-file-name=libgcc_s.so.1)" "$PAK/lib/libgcc_s.so.1"

# Everything else comes from the sysroot and its dependencies are followed
# recursively by the collector.
python3 "$ROOT/scripts/nextui/collect-libs.py" \
    --sysroot "$SYSROOT" \
    --dest "$PAK/lib" \
    --readelf "${CROSS_TRIPLE}-readelf" \
    "$PAK/bin/wiliwili" \
    "$PAK/lib/libstdc++.so.6" \
    "$PAK/lib/libgcc_s.so.1"

# The collector leaves device libraries out, but the staged libstdc++/libgcc_s
# may pull in one of the glibc parts, and the libc link script adds
# ld-linux-aarch64.so.1 as a NEEDED entry. Neither belongs in the pak.
for soname in ld-linux-aarch64.so.1 libc.so.6 libc_malloc_debug.so.0 libm.so.6 \
               libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2; do
    rm -f "$PAK/lib/$soname"
done

# On-device decode tester (bin/cedar-decode): runs the pak's own libavcodec
# against an H.264 file from a shell, so a device problem can be reproduced
# without the app. Built by scripts/nextui/cedar/build-decode.
if [ -x "$ROOT/dist/cedar-decode" ]; then
    install -m 755 "$ROOT/dist/cedar-decode" "$PAK/bin/cedar-decode"
fi

# CA bundle fallback. The launch script prefers the system bundle in
# /mnt/SDCARD/.system/res/ca-bundle.crt and only uses this copy if that file is
# missing.
CA_SRC=${CA_BUNDLE_SRC:-/etc/ssl/certs/ca-certificates.crt}
if [ ! -r "$CA_SRC" ]; then
    CA_SRC=/etc/pki/tls/certs/ca-bundle.crt
fi
if [ ! -r "$CA_SRC" ]; then
    echo "make-pak: no CA bundle found; set CA_BUNDLE_SRC" >&2
    exit 1
fi
cp "$CA_SRC" "$PAK/res/ca-bundle.crt"

strip="${CROSS_TRIPLE}-strip"
"$strip" --strip-unneeded "$PAK/bin/wiliwili"
for lib in "$PAK"/lib/*.so*; do
    "$strip" --strip-unneeded "$lib" 2>/dev/null || true
done

# License material: wiliwili is GPLv3 and the pak bundles GPL/LGPL components.
mkdir -p "$PAK/licenses"
cp "$ROOT/LICENSE" "$PAK/licenses/wiliwili-GPL-3.0.txt"
cp "$ROOT/docs/THIRD-PARTY.md" "$PAK/licenses/THIRD-PARTY.md"

# Sanity checks before packaging: right machine, executable launch script, and
# every NEEDED entry either bundled or part of the device allowlist.
if ! "${CROSS_TRIPLE}-readelf" -h "$PAK/bin/wiliwili" | grep -q 'AArch64'; then
    echo "make-pak: bin/wiliwili is not an AArch64 ELF" >&2
    exit 1
fi
test -x "$PAK/launch.sh"

rm -f "$DIST/WiliWili.pakz"
( cd "$PAK" && zip -qrX "$DIST/WiliWili.pakz" \
    launch.sh pak.json icon.png bin lib resources res licenses )

echo "==> pak contents:"
du -sh "$PAK" "$PAK/bin" "$PAK/lib" "$PAK/resources"
ls -l "$PAK/lib"
echo "==> archive:"
ls -lh "$DIST/WiliWili.pakz"
