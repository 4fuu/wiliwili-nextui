#!/usr/bin/env python3
"""Copy the runtime library closure of the built binaries into a pak lib dir.

The tg5040 device only provides the libraries the stock firmware and NextUI
ship (glibc, SDL2, GLESv2/EGL, ALSA, zlib, freetype, ...). Everything the port
adds itself (libmpv, FFmpeg, libass, harfbuzz, fribidi, OpenSSL, libstdc++)
must travel with the pak.

Usage:
  collect-libs.py --sysroot DIR --dest DIR --allowlist FILE ELF [ELF ...]

The allowlist file holds one substring per line; a NEEDED entry that contains
one of them is left to the device. Resolution failures are fatal so a pak can
never be produced with a missing library.
"""

import argparse
import os
import shutil
import subprocess
import sys

# Libraries the NextUI runtime image provides. Matching is by substring so
# versioned names (libSDL2-2.0.so.0) match their family.
DEFAULT_ALLOWLIST = """\
ld-linux-aarch64.so.1
libc.so.6
libm.so.6
libdl.so.2
libpthread.so.0
librt.so.1
libresolv.so.2
libGLESv2.so
libEGL.so
libSDL2-2.0.so
libSDL2_ttf-2.0.so
libSDL2_image-2.0.so
libasound.so.2
libz.so.1
libfreetype.so.6
libpng16.so.16
libpng12.so.0
"""


def readelf_needed(path, readelf):
    out = subprocess.run([readelf, "-d", path], check=True, capture_output=True, text=True).stdout
    needed = []
    for line in out.splitlines():
        if "(NEEDED)" not in line:
            continue
        start = line.find("[")
        end = line.find("]")
        if start != -1 and end != -1:
            needed.append(line[start + 1:end])
    return needed


def resolve(soname, search_dirs):
    for directory in search_dirs:
        candidate = os.path.join(directory, soname)
        if os.path.exists(candidate):
            return os.path.realpath(candidate)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sysroot", required=True)
    parser.add_argument("--dest", required=True)
    parser.add_argument("--allowlist", help="extra allowlist file (device-provided sonames)")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--search-dir", action="append", default=[])
    parser.add_argument("elf", nargs="+")
    args = parser.parse_args()

    allow = [line.strip() for line in DEFAULT_ALLOWLIST.splitlines() if line.strip()]
    if args.allowlist:
        with open(args.allowlist, encoding="utf-8") as handle:
            allow += [line.strip() for line in handle if line.strip() and not line.startswith("#")]

    search_dirs = list(args.search_dir) + [
        os.path.join(args.sysroot, "usr", "lib"),
        os.path.join(args.sysroot, "usr", "lib", "aarch64-linux-gnu"),
        os.path.join(args.sysroot, "lib"),
    ]

    os.makedirs(args.dest, exist_ok=True)
    queue = list(args.elf)
    seen = set()
    copied = {}

    while queue:
        current = queue.pop()
        for soname in readelf_needed(current, args.readelf):
            if soname in seen:
                continue
            seen.add(soname)
            if any(marker in soname for marker in allow):
                continue
            target = os.path.join(args.dest, soname)
            if os.path.exists(target):
                # Already staged (libstdc++/libgcc are copied before the walk).
                queue.append(target)
                continue
            source = resolve(soname, search_dirs)
            if source is None:
                sys.exit(f"collect-libs: {soname} (needed by {current}) not found in {search_dirs}")
            shutil.copy2(source, target)
            os.chmod(target, 0o755)
            copied[soname] = source
            queue.append(target)

    for soname in sorted(copied):
        print(f"{soname} <- {copied[soname]}")


if __name__ == "__main__":
    main()
