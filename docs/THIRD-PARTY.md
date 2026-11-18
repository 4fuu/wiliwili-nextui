# Third-party components in WiliWili.pakz

The pak is a GPLv3 work: it contains wiliwili (GPLv3) linked with libmpv
(GPLv2-or-later). Every component below is built from source by
`scripts/nextui/build-deps.sh` with the flags documented in `NEXTUI.md`; no
prebuilt community binary is redistributed.

| Component | Version | License | Bundled | Built here |
|---|---|---|---|---|
| wiliwili | 1.6.0 | GPL-3.0-or-later | `bin/wiliwili` | yes |
| mpv / libmpv | 0.36.0 | GPL-2.0-or-later | `lib/libmpv.so.2` | yes |
| FFmpeg (`libav*`) | 6.1.1 | LGPL-2.1-or-later (no `--enable-gpl`, no `--enable-nonfree`) | `lib/libav*.so.*` | yes |
| libass | 0.17.5 | ISC | `lib/libass.so.9` | yes |
| harfbuzz | 7.3.0 | MIT | `lib/libharfbuzz.so.0` | yes |
| fribidi | 1.0.16 | LGPL-2.1-or-later | `lib/libfribidi.so.0` | yes |
| OpenSSL | 1.1 (vendor SDK) | OpenSSL/SSLeay (GPL-compatible) | `lib/libssl.so.1.1`, `lib/libcrypto.so.1.1` | no, SDK |
| libstdc++ / libgcc | gcc 8.3 (toolchain) | GPL-3.0-with-GCC-exception / GPL-3.0-with-GCC-exception | `lib/` | no, toolchain |
| SDL2, GLESv2/EGL, ALSA, zlib, FreeType, libpng | vendor SDK | zlib / MIT / LGPL-2.1 / zlib / FTL / libpng | no | device-provided |

Notes:

- FFmpeg is deliberately built without `--enable-nonfree` and without
  `--enable-gpl`. It uses the native AAC decoder rather than `fdk-aac`, so the
  resulting libraries are redistributable under the LGPL.
- mpv's GPLv2-or-later is compatible with wiliwili's GPLv3 ("or later").
- libass is built without fontconfig and without any system font dependency.
- The licenses of the built components are available in their respective
  source archives (pinned in `scripts/nextui/build-deps.sh`); this file and the
  repository `LICENSE` are shipped inside the pak under `licenses/`.
