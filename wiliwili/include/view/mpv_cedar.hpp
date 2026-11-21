//
// NextUI tg5040 (TrimUI Brick Pro): Allwinner CedarX hardware video decode.
//
// The bundled FFmpeg carries an extra decoder, h264_cedar (see
// scripts/nextui/ffmpeg/), that drives the device's vendor decoder
// (/usr/lib/libvdecoder.so) through dlopen() and outputs ordinary NV21
// frames. The "hardware decode" setting decides whether it is used: on, mpv
// gets --vd=h264_cedar for every file; off, mpv keeps its software decoder.
// There is no availability probe and no silent fallback, so a stream the
// vendor decoder cannot handle shows up as a decode error instead of a
// different picture path.
//

#pragma once

#ifdef NEXTUI_TG5040

#include <string>
#include <borealis/core/logger.hpp>

#include "view/mpv_core.hpp"

class MpvCedarDecoder {
public:
    // --vd is reset on the next file, so this runs before every loadfile.
    // An empty name puts the software decoder back in charge.
    static void selectDecoder(bool hardware) {
        MPVCore::instance().command_async("set", "vd", hardware ? "h264_cedar" : "");
        brls::Logger::info("MPV video decoder: {}", hardware ? "h264_cedar" : "software");
    }
};

#endif
