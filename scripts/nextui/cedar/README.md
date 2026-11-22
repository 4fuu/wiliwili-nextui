# CedarX hardware H.264 decode for the TrimUI Brick Pro (tg5040)

The Brick Pro (Allwinner A133P, kernel 4.9) ships a CedarC 1.3.0 video
decoder: `/usr/lib/libvdecoder.so`, its dependencies (`libvideoengine`,
`libVE`, `libMemAdapter`, `libcdc_base`, `libsbm`, `libfbm`), the codec
plugins (`libawh264.so`, and `libawh265.so` on some images), and the
`/dev/cedar_dev` and `/dev/ion` nodes. None of it has `DT_NEEDED` entries or
headers, so everything here loads it with `dlopen()` and talks to it through
the structures recovered from the vendor library's DWARF debug info.

Provenance: the ABI declarations (`include/cedarc_abi.h`,
`sunxi_disp_abi.h`, `sunxi_g2d_abi.h`) and the wrapper (`src/cedar*.c`,
`disp.c`, `g2d_rot.c`, `ion.c`) come from
[moonlight-tsp](https://github.com/isalin/moonlight-tsp) by isalin,
GPL-3.0-or-later (`LICENSE.moonlight-tsp`), which proved this decoder on the
same chip (TrimUI Smart Pro). `tools/cedar-probe.c` is a cut-down
`tsp-videotest.c` that reports the environment and checks that a stream
decodes, without needing a display layer.

## Pieces

| Path | What it is |
|---|---|
| `tools/cedar-probe.c` | Standalone spike: environment report, decode check, optional display-layer show |
| `build-probe` | Cross-builds `cedar-probe` (aarch64) and cuts a 720p30 test clip |
| `toolchain.sh` | Full-build toolchain: vendor SDK sysroot + Bootlin glibc 2.31 C library/ld |
| `src/`, `include/` | Vendor ABI + wrapper from moonlight-tsp (shared with the FFmpeg decoder) |
| `../ffmpeg/h264_cedardec.c` | The FFmpeg `AVCodec` built into the pak (`scripts/nextui/build-deps.sh`) |

## Spike

```sh
CROSS_COMPILE=/opt/.../bin/aarch64-buildroot-linux-gnu- ./scripts/nextui/cedar/build-probe
```

writes `dist/cedar-spike/` with `cedar-probe`, `clip-720p30.264` and
`README.txt`. On the device, with the app closed:

```sh
./cedar-probe                            # vendor libs, nodes, modules, ABI sizes
./cedar-probe -i clip-720p30.264         # decode; "result: PASS" means OK
./cedar-probe -i clip-720p30.264 -d      # also show it on the panel
```

The `picture:` line reports the output format (5 = NV21, 6 = NV12), stride,
crop and whether the vendor hands out CPU pointers (`pData0`). `-d` owns the
display layer ch0/lyr0 at z=32 for the run and disables it on exit.

Result on the Brick Pro (via moonlight-tsp's `tsp-videotest`, 2026-11): H.264
720p60 decode call 3.74 ms mean, ~6% of one core, 600 frames without error;
pictures are NV21, stride = width, no crop, contiguous ION memory.

## The FFmpeg decoder

`h264_cedardec.c` is a plain `AVCodec` named `h264_cedar` (not an hwaccel):
it converts incoming packets to Annex-B, feeds one access unit at a time to
the vendor parser, copies each NV21 picture out of its ION buffer into a
normal `AVFrame`, and passes the packet's pts through to that frame. It is
built by the default `.github/workflows/nextui-tg5040.yml` build; software
decode remains the runtime default. On a device without the vendor libraries
it fails to open like any other decoder.

wiliwili selects it from `view/mpv_cedar.hpp` when the "hardware decode"
setting is on: `vd=h264_cedar` is forced for every file, and the switch off
keeps mpv's software decoder. There is no availability probe. "Needs more
input" is handled by consuming the packet and letting libavcodec feed the
next one; an old-style decode callback must never return `EAGAIN`. Every
failure returns an FFmpeg error instead of aborting. Diagnostics are off by
default; `H264_CEDAR_TRACE=1` in the environment turns on per-call tracing.

Known vendor behaviour: the parser drops the first picture and may leave up
to three pictures in the decoder at the end of a stream. Roughly one frame at
the start and 0.1 s at the end are missing; playback and A/V sync are not
affected, and software decode does not have this behaviour.

## Toolchain

The vendor SDK's Linaro 7.5 toolchain cannot build this stack: its sysroot has
glibc 2.25 (no `sem_clockwait`, which libavcodec needs) and its ld has no
`--export-dynamic-symbol` (libsbm must bind to our `SemTimedWait`). The tg5040
build keeps the vendor sysroot for SDL2/GLES/ALSA/OpenSSL and
puts Bootlin's `aarch64--glibc--stable-2020.08-1` (glibc 2.31, below the
device's 2.33) in front for the C library and linker. `toolchain.sh` writes
the `libc.so`/`libpthread.so` link scripts and the matching flags.
