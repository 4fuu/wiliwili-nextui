//
// NextUI tg5040 (TrimUI Brick Pro): Allwinner CedarX hardware video decode.
//
// The bundled FFmpeg carries an extra decoder, h264_cedar (see
// scripts/nextui/ffmpeg/), that drives the device's vendor decoder
// (/usr/lib/libvdecoder.so) through dlopen() and outputs ordinary NV21
// frames. mpv's --vd option has no per-decoder fallback, so we ask libmpv
// itself whether h264_cedar opens on this device: a throwaway mpv handle
// decodes a tiny H.264 clip with it, and only then is it forced for
// playback. Anything else (no vendor libraries, decode failure, timeout)
// keeps the software decoder, so the default stays safe on every unit.
//

#pragma once

#ifdef NEXTUI_TG5040

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <borealis/core/logger.hpp>

#include "view/mpv_core.hpp"

class MpvCedarDecoder {
public:
    // Returns true if the device's CedarX decoder works; the result is cached.
    static bool available() {
        static int cached = -1;
        if (cached < 0) cached = probe() ? 1 : 0;
        return cached == 1;
    }

    // Forces h264_cedar for the next file when available, software otherwise.
    // Call before every loadfile: --vd is reset on the next file. mpv falls
    // back to the software decoder by itself when h264_cedar cannot open, so
    // this only decides what is tried first.
    static void selectDecoder() {
        std::string decoder = available() ? "h264_cedar" : "";
        MPVCore::instance().command_async("set", "vd", decoder);
        brls::Logger::info("MPV video decoder: {}", decoder.empty() ? "software" : decoder);
    }

private:
    // One 16x16 baseline IDR picture, sent twice: the second copy completes
    // the first access unit so the decoder hands that picture out, and the
    // file ends there (no trailing access unit delimiters, which some
    // decoders report as an error at end of file). x264 with cabac=0,
    // ref=1, bframes=0, deblock=0 and the SEI stripped: 64 bytes.
    static constexpr uint8_t CLIP[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0a, 0xdd, 0xec, 0x04, 0x40,
        0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x03, 0x02, 0x83, 0xc4, 0x89,
        0xe0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x03, 0x92, 0xc8, 0x00, 0x00,
        0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xbc, 0x98, 0xa0, 0x00, 0x23, 0x3f,
        0x80, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xbc, 0x98, 0xa0,
        0x00, 0x23, 0x3f, 0x80
    };

    static bool probe() {
        std::string dir  = "/tmp/wiliwili-cedar";
        std::string file = dir + "/probe.264";
        FILE *fp         = nullptr;
        if (mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST) fp = fopen(file.c_str(), "wb");
        if (fp == nullptr) {
            brls::Logger::warning("Cedar probe: cannot write {}", file);
            return false;
        }
        fwrite(CLIP, 1, sizeof(CLIP), fp);
        fclose(fp);

        // A second mpv instance with no output devices only decodes; it is
        // destroyed again before the first video is loaded.
        mpv_handle *mpv = mpvCreate();
        if (!mpv) return false;
        mpvSetOptionString(mpv, "config", "no");
        mpvSetOptionString(mpv, "terminal", "no");
        mpvSetOptionString(mpv, "vo", "null");
        mpvSetOptionString(mpv, "ao", "null");
        mpvSetOptionString(mpv, "vd", "h264_cedar");
        mpvSetOptionString(mpv, "force-window", "no");
        bool ok = mpv_initialize(mpv) >= 0;
        if (ok) {
            const char *cmd[] = {"loadfile", file.c_str(), "replace", nullptr};
            ok                = mpvCommand(mpv, cmd) >= 0;
        }

        // mpv falls back to its software decoder when h264_cedar cannot open,
        // so the load succeeding proves nothing. The decoder's output format
        // does: h264_cedar produces NV21, the software decoder yuv420p. A
        // vendor that hangs instead of failing is caught by the timeout.
        for (int i = 0; ok && i < 60; i++) {
            mpv_event *event = mpvWaitEvent(mpv, 0.05);

            if (event->event_id == MPV_EVENT_FILE_LOADED ||
                event->event_id == MPV_EVENT_END_FILE)
                break;
        }
        // The properties are filled once the first frame is decoded, which
        // is a moment after the file loads.
        ok = false;
        for (int i = 0; !ok && i < 40; i++) {
            char *format = nullptr;

            if (mpv_get_property(mpv, "video-params/pixelformat",
                                 MPV_FORMAT_STRING, &format) >= 0 &&
                format) {
                ok = strcmp(format, "nv21") == 0;
                mpv_free(format);
                break;
            }
            mpvWaitEvent(mpv, 0.05);
        }
        mpvTerminateDestroy(mpv);
        unlink(file.c_str());
        brls::Logger::info("Cedar hardware decode: {}", ok ? "available" : "unavailable");
        return ok;
    }
};

#endif
