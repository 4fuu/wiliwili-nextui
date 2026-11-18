# NextUI port (TrimUI Brick Pro / tg5040)

wiliwili v1.6.0 built from source for NextUI on the TrimUI Brick Pro
(Allwinner A133P, PowerVR GE8300, 1024x768, TinaLinux 4.9, glibc 2.33).

This is a self-built port: no community prebuilt binary is used or repackaged.
The only community material consulted was the build *approach*; every binary in
`WiliWili.pakz` is produced by this repository's scripts.

- Upstream: <https://github.com/xfangfang/wiliwili> v1.6.0, GPLv3.
- This port: <https://github.com/4fuu/wiliwili-nextui>, GPLv3.

## What works

| Area | Status |
|---|---|
| UI | GLES3 via SDL2 + nanovg, 1024x768 native layout |
| Video | libmpv render API (`vo=gpu`) with FFmpeg **software** decode |
| Audio | ALSA (mpv `ao=alsa`, SDL audio as fallback) |
| Input | SDL GameController; the device's `js0` reports "Xbox 360 Controller" |
| Login | QR code login (Bilibili app scan) |
| HTTPS | system CA bundle at `/mnt/SDCARD/.system/res/ca-bundle.crt`, bundled fallback |
| Exit | returns to the NextUI desktop; HOME is owned by NextUI's system overlay |
| Sleep | none needed: the desktop's idle-sleep loop does not run while an app is foregrounded |
| Config | `$SHARED_USERDATA_PATH/wiliwili/wiliwili_config.json` |
| Logs | `$LOGS_PATH/WiliWili.txt` |

## Known limits

- **No hardware video decode.** The BSP exposes no FFmpeg-usable decoder, so the
  port is software-only. 480p is solid, 720p30 is usable, 1080p is not. The
  build caps landscape quality at 720P (64) and portrait at 480P (32); the
  default is 720P. `PLAYER_LOW_QUALITY` is on by default in wiliwili and skips
  the loop filter, which matters a lot on this SoC.
- **No on-screen keyboard.** QR login works; text search, comments, password
  login and other text entry do not. This is a platform limitation, not a bug in
  the port.
- **No overlay integration.** The pak does not declare the `overlay`
  capability, so NextUI's system HOME panel handles exit/resume. A future
  version can adopt `overlay_host` for an in-app panel.
- Danmaku is rendered by wiliwili's own nanovg renderer and is cheap; libass is
  only a libmpv build dependency.
- 1080p and above are not exposed because the quality cap is compiled in; a
  user config cannot exceed it.

## Install

1. Download `WiliWili.pakz` from the CI artifact (or a release).
2. Install it with NextUI's app installer, or unzip it into
   `/Tools/tg5040/WiliWili.pak/` on the SD card so that
   `/Tools/tg5040/WiliWili.pak/launch.sh` exists.
3. Launch **WiliWili** from the desktop's Tools list.

First launch creates `$SHARED_USERDATA_PATH/wiliwili/` and writes the config on
exit.

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | move |
| A | confirm |
| B | back |
| X / Y | page actions (search, menu) as shown in the on-screen hint bar |
| L / R | previous / next tab |
| Start | context menu where available |

If the physical A/B labels feel inverted, enable **A-B swap** in
Settings → Playback/Interface (`APP_SWAP_ABXY`).

## Build

The aarch64 build runs inside the NextUI toolchain image
(`ghcr.io/loveretro/tg5040-toolchain`). CI does exactly this on every push; a
Docker host can run the same three commands:

```sh
docker run --rm -v "$PWD:/root/workspace" -w /root/workspace \
  ghcr.io/loveretro/tg5040-toolchain:latest bash -lc '
    apt-get update && apt-get install -y --no-install-recommends meson zip file
    ./scripts/nextui/build-deps.sh
    ./scripts/nextui/build-wiliwili.sh
    ./scripts/nextui/make-pak.sh
  '
```

Outputs:

- `build-nextui/wiliwili` — the aarch64 executable
- `dist/WiliWili.pak/` — complete pak directory
- `dist/WiliWili.pakz` — archive whose **contents** are the pak
  (`launch.sh` at the archive root), the shape NextUI's app installer expects

### Build inputs

| Piece | Version | Notes |
|---|---|---|
| wiliwili | v1.6.0 | this tree, `PLATFORM_DESKTOP` |
| SDL2 | 2.26.1 (vendor SDK) | device-provided, not bundled |
| GLESv2 / EGL | vendor SDK | device-provided |
| mpv | 0.36.0 | shared `libmpv.so.2`, `cplayer` off |
| FFmpeg | 6.1.1 | shared, software decode |
| libass | 0.17.5 | no fontconfig (device has no system fonts) |
| harfbuzz | 7.3.0 | no FreeType/GLib/ICU |
| fribidi | 1.0.16 | |
| OpenSSL | 1.1 (vendor SDK) | HTTPS for libcurl and FFmpeg |

### CMake configuration

```sh
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-tg5040.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DPLATFORM_DESKTOP=ON \
  -DUSE_SDL2=ON -DUSE_SYSTEM_SDL2=ON \
  -DUSE_GLES3=ON \
  -DNEXTUI_TG5040=ON
```

`NEXTUI_TG5040=ON` selects the device defaults in
`wiliwili/source/utils/config_helper.cpp`: 1024x768 design resolution, 720P
quality default and cap, 480P portrait cap. `-DMPV_NO_FB=ON` switches to the
direct-framebuffer render path if the PowerVR driver misbehaves with mpv's FBO
(the default is the FBO copy path).

### Dependency build (license posture)

`scripts/nextui/build-deps.sh` builds everything from source with pinned
SHA-256 checks. The important configure flags:

**FFmpeg 6.1.1** — no `--enable-nonfree`, no `--enable-gpl`, no external
nonfree codec:

```
--enable-cross-compile --enable-shared --disable-static
--disable-programs --disable-avdevice --disable-postproc
--disable-encoders --disable-muxers --disable-hwaccels
--enable-zlib --enable-openssl
--enable-protocol=file,http,https,tcp,udp,hls,tls,httpproxy,data,concat,crypto
--enable-filter=format,scale,fps,crop,pad,transpose,hflip,vflip,aresample,anull,atrim,asetpts,setpts
```

Audio is decoded by FFmpeg's native AAC decoder; `fdk-aac` is not involved.
The result is an LGPL-2.1-or-later FFmpeg with no nonfree components, which is
redistributable.

**mpv 0.36.0** — `libmpv=true`, `cplayer=false`, no Lua/JS/archive/libplacebo,
no X11/Wayland/DRM/Vulkan/VAAPI, ALSA + SDL2 audio, `gl` + `plain-gl` only
(what libmpv's OpenGL render API needs).

**libass 0.17.5** — `-Dfontconfig=disabled -Drequire-system-font-provider=false`.
The brick has no fontconfig database or system fonts; wiliwili draws danmaku
itself, so this only affects `sub-add` subtitles, which wiliwili does not use.

### Bundled vs device-provided libraries

`scripts/nextui/collect-libs.py` walks the ELF `NEEDED` graph of the binary and
copies everything that is not on the device allowlist (glibc, SDL2,
GLESv2/EGL, ALSA, zlib, FreeType, libpng). The pak currently bundles libmpv,
FFmpeg's `libav*`, libass, harfbuzz, fribidi, OpenSSL, libstdc++ and libgcc.
Resolution failures are fatal, so a pak cannot be published with a missing
library.

## Layout

```
Tools/tg5040/WiliWili.pak/
  launch.sh          # env, logging, stay_awake, cleanup
  pak.json           # NextUI manifest (TOOL, tg5040)
  icon.png
  bin/wiliwili       # aarch64, GLES3
  lib/*.so*          # bundled closure
  resources/         # wiliwili resources (fonts, xml, i18n, ...)
  res/ca-bundle.crt  # fallback CA bundle
  licenses/          # GPLv3 + third-party notices
```

`launch.sh` sets `LD_LIBRARY_PATH`, `XDG_CONFIG_HOME`, `HOME`,
`SDL_AUDIODRIVER=alsa` and `CURL_CA_BUNDLE`/`SSL_CERT_FILE`, then runs the
binary and returns its exit status. NextUI needs no sleep integration for
foreground apps: the desktop process exits when a pak launches, so the idle
sleep loop that lives in it is not running. The SDL video driver is left on
auto-selection: the vendor driver is named `mali`, and forcing a name that a
future image renames would abort startup.

## Troubleshooting

The log is at `$LOGS_PATH/WiliWili.txt`
(`/mnt/SDCARD/.userdata/tg5040/logs/WiliWili.txt`). Useful facts in it:

- `sdl: GL Vendor/Renderer/Version` — should report PowerVR GE8300 / OpenGL ES 3.x
- `MPV Version` / `FFMPEG Version`
- `ca bundle: ...` — which bundle was selected

If the UI renders but video is black, try a build with `MPV_NO_FB=1` before
chasing anything else; that isolates the FBO path from the GL context.

If the app exits immediately, run `launch.sh` over ADB and check whether
`libmpv.so.2` or a bundled dependency fails to load (`LD_DEBUG=libs`).
