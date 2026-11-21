/*
 * Allwinner CedarX hardware H.264 decoder for FFmpeg.
 *
 * Part of the wiliwili NextUI port for the TrimUI Brick Pro (Allwinner
 * A133P). The vendor's CedarC 1.3.0 decoder (libvdecoder.so and friends,
 * /usr/lib) is loaded at run time with dlopen(), so this codec builds
 * against stock FFmpeg and simply fails to open on a device without the
 * vendor libraries; mpv then reports the failure like any other decoder.
 *
 * This is a plain AVCodec rather than an hwaccel: it outputs regular
 * AVFrames in ordinary memory (the picture is copied out of the vendor's
 * ION buffer), so mpv's existing OpenGL path renders it unchanged. Select
 * it with `--vd=h264_cedar` (mpv) or `-c:v h264_cedar` (ffmpeg).
 *
 * The vendor ABI (CedarVideoPicture & co.) comes from the DWARF information
 * in the device's libvdecoder.so and is shared with scripts/nextui/cedar.
 *
 * Based on the CedarC wrapper of moonlight-tsp by isalin
 * (https://github.com/isalin/moonlight-tsp, GPL-3.0-or-later).
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "libavcodec/bsf.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "h2645_parse.h"
#include "internal.h"

/* ---- vendor ABI, from cedarc_abi.h (see scripts/nextui/cedar) --------- */

#define CEDAR_CODEC_H264            0x115
#define CEDAR_PIXEL_NV12            0x06
#define CEDAR_PIXEL_NV21            0x05

enum {
    CEDAR_RESULT_UNSUPPORTED       = -1,
    CEDAR_RESULT_OK                =  0,
    CEDAR_RESULT_FRAME_DECODED     =  1,
    CEDAR_RESULT_CONTINUE          =  2,
    CEDAR_RESULT_KEYFRAME_DECODED  =  3,
    CEDAR_RESULT_NO_FRAME_BUFFER   =  4,
    CEDAR_RESULT_NO_BITSTREAM      =  5,
    CEDAR_RESULT_RESOLUTION_CHANGE =  6,
};

typedef struct CedarVConfig {
    int32_t bScaleDownEn;                      /* 0x00 */
    int32_t bRotationEn;                       /* 0x04 */
    int32_t bSecOutputEn;                      /* 0x08 */
    int32_t nHorizonScaleDownRatio;            /* 0x0c */
    int32_t nVerticalScaleDownRatio;           /* 0x10 */
    int32_t nSDWidth;                          /* 0x14 */
    int32_t nSDHeight;                         /* 0x18 */
    int32_t bAnySizeSD;                        /* 0x1c */
    int32_t nSecHorizonScaleDownRatio;         /* 0x20 */
    int32_t nSecVerticalScaleDownRatio;        /* 0x24 */
    int32_t nRotateDegree;                     /* 0x28 */
    int32_t bThumbnailMode;                    /* 0x2c */
    int32_t eOutputPixelFormat;                /* 0x30 */
    int32_t eSecOutputPixelFormat;             /* 0x34 */
    int32_t bNoBFrames;                        /* 0x38 */
    int32_t bDisable3D;                        /* 0x3c */
    int32_t bSupportMaf;                       /* 0x40 */
    int32_t bDispErrorFrame;                   /* 0x44 */
    int32_t nVbvBufferSize;                    /* 0x48 */
    int32_t nFrameBufferNum;                   /* 0x4c */
    int32_t bSecureosEn;                       /* 0x50 */
    int32_t bGpuBufValid;                      /* 0x54 */
    int32_t nAlignStride;                      /* 0x58 */
    int32_t bIsSoftDecoderFlag;                /* 0x5c */
    int32_t bVirMallocSbm;                     /* 0x60 */
    int32_t bSupportPallocBufBeforeDecode;     /* 0x64 */
    int32_t nDeInterlaceHoldingFrameBufferNum; /* 0x68 */
    int32_t nDisplayHoldingFrameBufferNum;     /* 0x6c */
    int32_t nRotateHoldingFrameBufferNum;      /* 0x70 */
    int32_t nDecodeSmoothFrameBufferNum;       /* 0x74 */
    int32_t bIsTvStream;                       /* 0x78 */
    int32_t nLbcLossyComMod;                   /* 0x7c */
    uint32_t bIsLossy;                         /* 0x80 */
    uint32_t bRcEn;                            /* 0x84 */
    void *memops;                              /* 0x88 */
    int32_t eCtlAfbcMode;                      /* 0x90 */
    int32_t eCtlIptvMode;                      /* 0x94 */
    void *veOpsS;                              /* 0x98 */
    void *pVeOpsSelf;                          /* 0xa0 */
    int32_t bConvertVp910bitTo8bit;            /* 0xa8 */
    uint32_t nVeFreq;                          /* 0xac */
    int32_t bCalledByOmxFlag;                  /* 0xb0 */
    int32_t bSetProcInfoEnable;                /* 0xb4 */
    int32_t nSetProcInfoFreq;                  /* 0xb8 */
    int32_t nChannelNum;                       /* 0xbc */
    int32_t nSupportMaxWidth;                  /* 0xc0 */
    int32_t nSupportMaxHeight;                 /* 0xc4 */
    int32_t commonConfigFlag;                  /* 0xc8 */
    int32_t bATMFlag;                          /* 0xcc */
} CedarVConfig;

typedef struct CedarVideoStreamInfo {
    int32_t eCodecFormat;            /* 0x00 */
    int32_t nWidth;                  /* 0x04 */
    int32_t nHeight;                 /* 0x08 */
    int32_t nFrameRate;              /* 0x0c */
    int32_t nFrameDuration;          /* 0x10 */
    int32_t nAspectRatio;            /* 0x14 */
    int32_t bIs3DStream;             /* 0x18 */
    int32_t nCodecSpecificDataLen;   /* 0x1c */
    char *pCodecSpecificData;        /* 0x20 */
    int32_t bSecureStreamFlag;       /* 0x28 */
    int32_t bSecureStreamFlagLevel1; /* 0x2c */
    int32_t bIsFramePackage;         /* 0x30 */
    int32_t h265ReferencePictureNum; /* 0x34 */
    int32_t bReOpenEngine;           /* 0x38 */
    int32_t bIsFrameCtsTestFlag;     /* 0x3c */
} CedarVideoStreamInfo;

typedef struct CedarVideoStreamDataInfo {
    char *pData;             /* 0x00 */
    int32_t nLength;         /* 0x08 */
    int32_t pad0;
    int64_t nPts;            /* 0x10 */
    int64_t nPcr;            /* 0x18 */
    int32_t bIsFirstPart;    /* 0x20 */
    int32_t bIsLastPart;     /* 0x24 */
    int32_t nID;             /* 0x28 */
    int32_t nStreamIndex;    /* 0x2c */
    int32_t bValid;          /* 0x30 */
    uint32_t bVideoInfoFlag; /* 0x34 */
    void *pVideoInfo;        /* 0x38 */
} CedarVideoStreamDataInfo;

typedef struct CedarVideoPicture {
    int32_t nID;               /* 0x00 */
    int32_t nStreamIndex;      /* 0x04 */
    int32_t ePixelFormat;      /* 0x08 */
    int32_t nWidth;            /* 0x0c */
    int32_t nHeight;           /* 0x10 */
    int32_t nLineStride;       /* 0x14 */
    int32_t nTopOffset;        /* 0x18 */
    int32_t nLeftOffset;       /* 0x1c */
    int32_t nBottomOffset;     /* 0x20 */
    int32_t nRightOffset;      /* 0x24 */
    int32_t nFrameRate;        /* 0x28 */
    int32_t nAspectRatio;      /* 0x2c */
    int32_t bIsProgressive;    /* 0x30 */
    int32_t bTopFieldFirst;    /* 0x34 */
    int32_t bRepeatTopField;   /* 0x38 */
    int32_t pad0;
    int64_t nPts;              /* 0x40 */
    int64_t nPcr;              /* 0x48 */
    char *pData0;              /* 0x50 */
    char *pData1;              /* 0x58 */
    char *pData2;              /* 0x60 */
    char *pData3;              /* 0x68 */
    int32_t bMafValid;         /* 0x70 */
    int32_t pad1;
    char *pMafData;            /* 0x78 */
    int32_t nMafFlagStride;    /* 0x80 */
    int32_t bPreFrmValid;      /* 0x84 */
    int32_t nBufId;            /* 0x88 */
    int32_t pad2;
    uint64_t phyYBufAddr;      /* 0x90 */
    uint64_t phyCBufAddr;      /* 0x98 */
    void *pPrivate;            /* 0xa0 */
    int32_t nBufFd;            /* 0xa8 */
    int32_t nBufStatus;        /* 0xac */
    int32_t bTopFieldError;    /* 0xb0 */
    int32_t bBottomFieldError; /* 0xb4 */
    int32_t nColorPrimary;     /* 0xb8 */
    int32_t bFrameErrorFlag;   /* 0xbc */
    void *pMetaData;           /* 0xc0 */
    int32_t video_full_range_flag;    /* 0xc8 */
    int32_t transfer_characteristics; /* 0xcc */
    int32_t matrix_coeffs;     /* 0xd0 */
    uint8_t colour_primaries;  /* 0xd4 */
    uint8_t pad3[3];
    int32_t nLower2BitBufSize;   /* 0xd8 */
    int32_t nLower2BitBufOffset; /* 0xdc */
    int32_t nLower2BitBufStride; /* 0xe0 */
    int32_t b10BitPicFlag;       /* 0xe4 */
    int32_t bEnableAfbcFlag;     /* 0xe8 */
    int32_t nLbcLossyComMod;     /* 0xec */
    uint32_t bIsLossy;           /* 0xf0 */
    uint32_t bRcEn;              /* 0xf4 */
    int32_t nBufSize;            /* 0xf8 */
    int32_t nAfbcSize;           /* 0xfc */
    int32_t nLbcSize;            /* 0x100 */
    int32_t nDebugCount;         /* 0x104 */
    uint8_t nCurFrameInfo[72];   /* 0x108 */
} CedarVideoPicture;

#define CEDAR_ASSERT_OFF(type, field, off)                                    \
    _Static_assert(offsetof(type, field) == (off), #type "." #field)

CEDAR_ASSERT_OFF(CedarVConfig, eOutputPixelFormat, 0x30);
CEDAR_ASSERT_OFF(CedarVConfig, bNoBFrames, 0x38);
CEDAR_ASSERT_OFF(CedarVConfig, nVbvBufferSize, 0x48);
CEDAR_ASSERT_OFF(CedarVConfig, nAlignStride, 0x58);
CEDAR_ASSERT_OFF(CedarVConfig, nDisplayHoldingFrameBufferNum, 0x6c);
CEDAR_ASSERT_OFF(CedarVConfig, bATMFlag, 0xcc);
_Static_assert(sizeof(CedarVConfig) == 0xd0, "CedarVConfig size");

CEDAR_ASSERT_OFF(CedarVideoStreamInfo, pCodecSpecificData, 0x20);
CEDAR_ASSERT_OFF(CedarVideoStreamInfo, bIsFramePackage, 0x30);
_Static_assert(sizeof(CedarVideoStreamInfo) == 0x40, "VideoStreamInfo size");

CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, nPts, 0x10);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, bIsFirstPart, 0x20);
CEDAR_ASSERT_OFF(CedarVideoStreamDataInfo, bValid, 0x30);
_Static_assert(sizeof(CedarVideoStreamDataInfo) == 0x40,
               "VideoStreamDataInfo size");

CEDAR_ASSERT_OFF(CedarVideoPicture, nLineStride, 0x14);
CEDAR_ASSERT_OFF(CedarVideoPicture, nPts, 0x40);
CEDAR_ASSERT_OFF(CedarVideoPicture, pData0, 0x50);
CEDAR_ASSERT_OFF(CedarVideoPicture, phyYBufAddr, 0x90);
CEDAR_ASSERT_OFF(CedarVideoPicture, bFrameErrorFlag, 0xbc);
CEDAR_ASSERT_OFF(CedarVideoPicture, video_full_range_flag, 0xc8);
CEDAR_ASSERT_OFF(CedarVideoPicture, nCurFrameInfo, 0x108);
_Static_assert(sizeof(CedarVideoPicture) == 0x150, "VideoPicture size");

typedef void CedarDecoder;

typedef CedarDecoder *(*cedar_create_fn)(void);
typedef void (*cedar_destroy_fn)(CedarDecoder *);
typedef int (*cedar_initialize_fn)(CedarDecoder *, CedarVideoStreamInfo *,
                                   CedarVConfig *);
typedef void (*cedar_reset_fn)(CedarDecoder *);
typedef int (*cedar_decode_fn)(CedarDecoder *, int end_of_stream,
                               int keyframe_only, int drop_b_if_late,
                               int64_t now_us);
typedef int (*cedar_request_stream_fn)(CedarDecoder *, int size, char **buf,
                                       int *buf_size, char **ring_buf,
                                       int *ring_size, int stream_index);
typedef int (*cedar_submit_stream_fn)(CedarDecoder *,
                                      CedarVideoStreamDataInfo *,
                                      int stream_index);
typedef CedarVideoPicture *(*cedar_request_picture_fn)(CedarDecoder *,
                                                       int stream_index);
typedef int (*cedar_return_picture_fn)(CedarDecoder *, CedarVideoPicture *);
typedef int (*cedar_count_fn)(CedarDecoder *, int stream_index);
typedef void (*cedar_add_plugin_fn)(char *path);

/* ---- diagnostics ------------------------------------------------------- */

/*
 * mpv leaves libavcodec at its default log level, so av_log() messages never
 * reach the app log. Hardware-decode diagnostics go to stderr instead, and
 * only when H264_CEDAR_TRACE is set in the environment: by default the log
 * carries the one-time banner and real errors, nothing per call.
 */
static int cedar_verbose(void)
{
    const char *env = getenv("H264_CEDAR_TRACE");
    static int cached = -1;

    if (cached < 0)
        cached = env && *env && strcmp(env, "0") != 0;
    return cached;
}

static void cedar_note(const char *fmt, ...)
{
    va_list ap;

    if (!cedar_verbose())
        return;
    va_start(ap, fmt);
    fputs("h264_cedar: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* Real problems: always printed, banner or error, never the per-call trace. */
static void cedar_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("h264_cedar: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---- vendor library loader (process wide, loaded once) ---------------- */

typedef struct CedarAPI {
    void *vdecoder;
    void *videoengine;
    int h264_plugin;
    cedar_create_fn create;
    cedar_destroy_fn destroy;
    cedar_initialize_fn initialize;
    cedar_reset_fn reset;
    cedar_decode_fn decode;
    cedar_request_stream_fn request_stream;
    cedar_submit_stream_fn submit_stream;
    cedar_request_picture_fn request_picture;
    cedar_return_picture_fn return_picture;
    cedar_count_fn valid_pictures;
    cedar_count_fn stream_frames;
    cedar_add_plugin_fn add_plugin;
} CedarAPI;

static CedarAPI *cedar_api;
static int cedar_busy;   /* an encoder context holds the vendor engine */
static int cedar_failed; /* a decode error happened; stop using it */

static void *cedar_open_lib(const char *name)
{
    char path[128];
    void *h;

    snprintf(path, sizeof(path), "/usr/lib/%s", name);
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    return h;
}

#define LOAD_SYM(handle, field, name)                                         \
    do {                                                                      \
        *(void **)&api->field = dlsym(handle, name);                          \
        if (!api->field) {                                                    \
            cedar_warn("missing symbol %s",    \
                   name);                                                     \
            goto fail;                                                        \
        }                                                                     \
    } while (0)

static int cedar_load(AVCodecContext *avctx)
{
    CedarAPI *api;

    if (cedar_api)
        return cedar_api->h264_plugin ? 0 : AVERROR_DECODER_NOT_FOUND;

    api = av_mallocz(sizeof(*api));
    if (!api)
        return AVERROR(ENOMEM);

    /*
     * RTLD_GLOBAL makes libvdecoder and its dependencies (libcdc_base,
     * libvideoengine, libVE, libMemAdapter) visible to the codec plugins,
     * which have no DT_NEEDED entries of their own.
     */
    api->vdecoder = cedar_open_lib("libvdecoder.so");
    if (!api->vdecoder) {
        cedar_warn("dlopen libvdecoder.so: %s",
           dlerror());
        goto fail;
    }
    api->videoengine = cedar_open_lib("libvideoengine.so");
    if (!api->videoengine) {
        cedar_warn("dlopen libvideoengine.so: %s", dlerror());
        goto fail;
    }

    LOAD_SYM(api->vdecoder, create, "CreateVideoDecoder");
    LOAD_SYM(api->vdecoder, destroy, "DestroyVideoDecoder");
    LOAD_SYM(api->vdecoder, initialize, "InitializeVideoDecoder");
    LOAD_SYM(api->vdecoder, reset, "ResetVideoDecoder");
    LOAD_SYM(api->vdecoder, decode, "DecodeVideoStream");
    LOAD_SYM(api->vdecoder, request_stream, "RequestVideoStreamBuffer");
    LOAD_SYM(api->vdecoder, submit_stream, "SubmitVideoStreamData");
    LOAD_SYM(api->vdecoder, request_picture, "RequestPicture");
    LOAD_SYM(api->vdecoder, return_picture, "ReturnPicture");
    LOAD_SYM(api->vdecoder, valid_pictures, "ValidPictureNum");
    LOAD_SYM(api->vdecoder, stream_frames, "VideoStreamFrameNum");
    LOAD_SYM(api->videoengine, add_plugin, "AddVDPluginSingle");

    /*
     * AddVDPlugin() would dlopen every lib*aw*.so in /usr/lib, including
     * audio decoders. Register only the H.264 plugin; not every CedarC
     * image ships libawh265.so, so its absence is not an error.
     */
    if (access("/usr/lib/libawh264.so", R_OK) == 0) {
        cedar_note("loading /usr/lib/libawh264.so");
        api->add_plugin((char *)"/usr/lib/libawh264.so");
        api->h264_plugin = 1;
    }
    cedar_api = api;
    cedar_note("vendor decoder %s",
           api->h264_plugin ? "loaded" : "unavailable");
    if (!api->h264_plugin) {
        cedar_warn("/usr/lib/libawh264.so is not installed");
        return AVERROR_DECODER_NOT_FOUND;
    }
    return 0;

fail:
    if (api->vdecoder)
        dlclose(api->vdecoder);
    if (api->videoengine)
        dlclose(api->videoengine);
    av_free(api);
    return AVERROR_DECODER_NOT_FOUND;
}

/* ---- per-stream decoder ----------------------------------------------- */

#define CEDAR_AU_MARKER  0x7f7f7f7f /* pts of the "flush" access unit */

#define CEDAR_PARSE_WAIT_NS 10000000ull /* how long to wait for the parser */
#define CEDAR_VBV_BYTES  (4 << 20)

typedef struct H264CedarContext {
    AVBSFContext *annexb; /* MP4-style input -> Annex-B for the vendor parser */
    CedarDecoder *handle;
    int width, height;
    int logged_picture;  /* first picture already described in the log */
    int submitted;       /* access units accepted by the vendor parser */
    int pending_frames;  /* access units submitted but not decoded yet */
    int decoded;         /* at least one picture came out */
    int64_t parse_deadline; /* when to stop waiting for the parser */
    int flushed;         /* trailing access unit submitted after EOF */
    int eof_returned;
} H264CedarContext;

static const char *cedar_result_name(int result)
{
    static const char *const names[] = {
        "ok", "frame", "continue", "keyframe", "no-frame-buffer",
        "no-bitstream", "resolution-change",
    };

    if (result >= 0 && result < FF_ARRAY_ELEMS(names))
        return names[result];
    return "error";
}

static int cedar_open_decoder(AVCodecContext *avctx, int width, int height)
{
    H264CedarContext *ctx = avctx->priv_data;
    CedarVideoStreamInfo info;
    CedarVConfig cfg;
    int ret;

    memset(&info, 0, sizeof(info));
    info.eCodecFormat     = CEDAR_CODEC_H264;
    info.nWidth           = width;
    info.nHeight          = height;
    /* The same frame timing the Cedar wrapper uses on this device. */
    info.nFrameRate       = 60 * 1000;
    info.nFrameDuration   = 1000000 / 60;
    info.bIsFramePackage  = 1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.eOutputPixelFormat = CEDAR_PIXEL_NV21;
    cfg.bNoBFrames         = 1;
    cfg.bDisable3D         = 1;
    cfg.nVbvBufferSize     = CEDAR_VBV_BYTES;
    cfg.nAlignStride       = 16;
    /*
     * The holding-buffer numbers of the engine path proven on this device
     * (moonlight-nextui cedar_sdl.c): one picture for the display, none for
     * smoothing. Deeper queues only add latency here.
     */
    cfg.nDeInterlaceHoldingFrameBufferNum = 0;
    cfg.nDisplayHoldingFrameBufferNum     = 1;
    cfg.nDecodeSmoothFrameBufferNum       = 0;

    ctx->handle = cedar_api->create();
    if (!ctx->handle) {
        cedar_warn("CreateVideoDecoder failed");
        return AVERROR_EXTERNAL;
    }
    cedar_note("decoder %p, InitializeVideoDecoder(%dx%d, %d fps, fmt %d, "
               "vbv %d, hold %d, smooth %d, align %d)",
               (void *)ctx->handle, width, height, info.nFrameRate / 1000,
               cfg.eOutputPixelFormat, cfg.nVbvBufferSize,
               cfg.nDisplayHoldingFrameBufferNum,
               cfg.nDecodeSmoothFrameBufferNum, cfg.nAlignStride);
    ret = cedar_api->initialize(ctx->handle, &info, &cfg);
    cedar_note("InitializeVideoDecoder -> %d",
           ret);
    if (ret) {
        /*
         * No DestroyVideoDecoder() on this path. On the Brick Pro a decoder
         * torn down right after a failed Initialize aborts the process in
         * CdcIonFree (errno 22); libvdecoder keeps its own reference until
         * the app exits either way. mpv falls back to software decoding.
         */
        ctx->handle = NULL;
        cedar_warn("InitializeVideoDecoder failed (%d)", ret);
        return AVERROR_EXTERNAL;
    }
    ctx->width   = width;
    ctx->height  = height;
    ctx->flushed = 0;
    /*
     * mpv hands this decoder MP4-style packets: extradata with the SPS/PPS
     * and length-prefixed NAL units. The Cedar parser only understands
     * Annex-B start codes, so everything goes through h264_mp4toannexb,
     * extradata included, before it reaches the vendor library. Annex-B
     * input passes through unchanged.
     */
    if (!ctx->annexb) {
        const AVBitStreamFilter *bsf =
            av_bsf_get_by_name("h264_mp4toannexb");

        if (!bsf || av_bsf_alloc(bsf, &ctx->annexb) < 0) {
            cedar_warn("h264_mp4toannexb is not available");
            ctx->annexb = NULL;
            return AVERROR_DECODER_NOT_FOUND;
        }
        avcodec_parameters_from_context(ctx->annexb->par_in, avctx);
        ctx->annexb->time_base_in = avctx->pkt_timebase;
        if (av_bsf_init(ctx->annexb) < 0) {
            cedar_warn("h264_mp4toannexb: init failed");
            av_bsf_free(&ctx->annexb);
            return AVERROR_EXTERNAL;
        }
    }
    cedar_note("decoder opened (%dx%d)", width,
           height);
    return 0;
}

static void cedar_close_decoder(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;

    av_bsf_free(&ctx->annexb);
    if (!ctx->handle)
        return;
    /*
     * The teardown the device test exercised is Initialize -> Submit ->
     * Destroy (tsp-videotest). A decoder that was initialized but never fed
     * goes the same way as a failed Initialize, since that is where the
     * Brick Pro aborted in CdcIonFree: leave it to the vendor library.
     */
    if (ctx->submitted) {
        cedar_note("DestroyVideoDecoder after %d access units",
                   ctx->submitted);
        cedar_api->destroy(ctx->handle);
    } else {
        cedar_note("decoder never received input; leaving it to the "
                   "vendor library instead of destroying it");
    }
    ctx->handle = NULL;
}

/*
 * The vendor parser aborts on AV_NOPTS_VALUE (INT64_MIN): libawh264 asserts on
 * it instead of ignoring it. Every submitted access unit gets a real pts, a
 * monotonic microsecond counter (as tsp-videotest uses) when the packet has
 * none.
 */
static int64_t cedar_pts(H264CedarContext *ctx, int64_t pts)
{
    if (pts != AV_NOPTS_VALUE && pts > INT64_MIN / 2)
        return pts;
    return (int64_t)ctx->submitted * 1000000 / 60;
}

static int cedar_submit(AVCodecContext *avctx, const uint8_t *data, int size,
                        int64_t pts)
{
    H264CedarContext *ctx = avctx->priv_data;
    CedarVideoStreamDataInfo di;
    char *buf, *ring;
    int buf_size, ring_size;
    int ret;

    ret = cedar_api->request_stream(ctx->handle, size, &buf, &buf_size, &ring,
                                    &ring_size, 0);
    if (ret || !buf || (int64_t)buf_size + ring_size < size) {
        cedar_note("no room for a %d byte access unit (sbm full?)",
           size);
        return AVERROR(ENOMEM);
    }
    if (size <= buf_size) {
        memcpy(buf, data, size);
    } else {
        memcpy(buf, data, buf_size);
        memcpy(ring, data + buf_size, size - buf_size);
    }

    pts = cedar_pts(ctx, pts);
    if (!ctx->submitted)
        cedar_note("first SubmitVideoStreamData: %d bytes, pts %lld", size,
                   (long long)pts);
    memset(&di, 0, sizeof(di));
    di.pData       = buf;
    di.nLength     = size;
    di.nPts        = pts;
    di.nPcr        = -1;
    di.bIsFirstPart = 1;
    di.bIsLastPart  = 1;
    di.bValid       = 1;
    if (cedar_api->submit_stream(ctx->handle, &di, 0)) {
        cedar_warn("SubmitVideoStreamData failed");
        return AVERROR_EXTERNAL;
    }
    if (ctx->submitted == 0)
        cedar_note("SubmitVideoStreamData -> 0");
    ctx->submitted++;
    ctx->pending_frames = FFMIN(ctx->pending_frames + 1, 2);
    ctx->parse_deadline = av_gettime_relative() + CEDAR_PARSE_WAIT_NS;
    return 0;
}

static int cedar_copy_picture(AVCodecContext *avctx, AVFrame *frame,
                              const CedarVideoPicture *pic)
{
    H264CedarContext *ctx = avctx->priv_data;
    int chroma_w, rows, y, ret;
    const uint8_t *src;

    if (pic->ePixelFormat != CEDAR_PIXEL_NV12 &&
        pic->ePixelFormat != CEDAR_PIXEL_NV21) {
        cedar_warn("unsupported picture format %d",
           pic->ePixelFormat);
        return AVERROR_PATCHWELCOME;
    }
    if (!pic->pData0 || !pic->pData1 || pic->nLineStride <= 0) {
        cedar_warn("picture has no CPU mapping (pData0=%p pData1=%p)",
           pic->pData0, pic->pData1);
        return AVERROR_EXTERNAL;
    }
    /*
     * The vendor may rotate the chroma plane by 180 degrees (SUNXI_UV_MERGE
     * builds). The Brick Pro's pictures have not been checked yet, so trust
     * the format the picture reports instead of guessing.
     */
    frame->format = pic->ePixelFormat == CEDAR_PIXEL_NV12 ? AV_PIX_FMT_NV12
                                                          : AV_PIX_FMT_NV21;
    frame->width  = pic->nWidth;
    frame->height = pic->nHeight;
    ret = ff_decode_frame_props(avctx, frame);
    if (ret < 0)
        return ret;
    /*
     * mpv takes the picture format from the AVFrame (mp_image_from_av_frame),
     * and the output pipeline copies planes by that format's pixel size.
     * NVIDIA (the libavutil enum): 0x18 = 24 = NV12, 0x1A = 26 = NV21.
     */
    if (!ctx->logged_picture) {
        ctx->logged_picture = 1;
        cedar_note("first picture: fmt %d %dx%d stride %d crop l%d t%d r%d b%d",
                   pic->ePixelFormat, pic->nWidth, pic->nHeight,
                   pic->nLineStride, pic->nLeftOffset, pic->nTopOffset,
                   pic->nRightOffset, pic->nBottomOffset);
        cedar_note("first picture: mapped pData0 %p pData1 %p",
                   (void *)pic->pData0, (void *)pic->pData1);
    }
    /*
     * The context drives the default frame pool (format, dimensions), and
     * libavcodec has not seen our pictures before the first one arrives.
     */
    if (avctx->pix_fmt != frame->format ||
        avctx->width != pic->nWidth || avctx->height != pic->nHeight) {
        cedar_note("output %s %dx%d, stride %d, crop %d,%d %dx%d",
           av_get_pix_fmt_name(frame->format), pic->nWidth, pic->nHeight,
           pic->nLineStride, pic->nLeftOffset, pic->nTopOffset,
           pic->nRightOffset - pic->nLeftOffset,
           pic->nBottomOffset - pic->nTopOffset);
        avctx->pix_fmt = frame->format;
        ret = ff_set_dimensions(avctx, pic->nWidth, pic->nHeight);
        if (ret < 0)
            return ret;
    }
    /* NV12/NV21 have two planes; libavcodec requires the rest cleared. */
    frame->data[2] = frame->data[3] = NULL;
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;
    frame->data[2] = frame->data[3] = NULL;

    /* Copy only the visible window; the picture may carry padding. */
    chroma_w = pic->nWidth / 2;
    for (y = 0; y < pic->nHeight; y++) {
        src = (const uint8_t *)pic->pData0 +
              (size_t)(pic->nTopOffset + y) * pic->nLineStride +
              pic->nLeftOffset;
        memcpy(frame->data[0] + (size_t)y * frame->linesize[0], src,
               pic->nWidth);
    }
    rows = (pic->nHeight + 1) / 2;
    for (y = 0; y < rows; y++) {
        src = (const uint8_t *)pic->pData1 +
              (size_t)(pic->nTopOffset / 2 + y) * pic->nLineStride +
              pic->nLeftOffset;
        memcpy(frame->data[1] + (size_t)y * frame->linesize[1], src,
               2 * chroma_w);
    }

    if (pic->video_full_range_flag)
        frame->color_range = AVCOL_RANGE_JPEG;
    if (pic->matrix_coeffs > 0 && pic->matrix_coeffs <= 11)
        frame->colorspace = (enum AVColorSpace)pic->matrix_coeffs;
    /*
     * libavcodec asserts that a successful decode filled frame->buf[0]
     * (av_assert0 in decode_simple_internal): fail instead of aborting.
     */
    if (!frame->buf[0]) {
        cedar_warn("frame buffer allocation returned no buffer");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/*
 * The decode loop follows the Cedar wrapper that runs 600+ frames on this
 * device (moonlight-nextui's cedar_sdl.c): submit one access unit, then call
 * DecodeVideoStream until the frame it closes has come out. NO_BITSTREAM and
 * CONTINUE mean "the parser needs more input" and are retried; a picture is
 * taken when the engine reports one, and no frame is ever held back, so the
 * vendor's holding buffers only delay it by a frame or two.
 *
 * Every exit is a normal return: libavcodec turns a negative value into a
 * decode error, and mpv counts those. Nothing here aborts.
 */
static void cedar_take_picture(AVCodecContext *avctx, AVFrame *frame,
                               int *got_picture)
{
    H264CedarContext *ctx = avctx->priv_data;
    CedarVideoPicture *pic, *latest = NULL;

    while ((pic = cedar_api->request_picture(ctx->handle, 0)) != NULL) {
        if (latest)
            cedar_api->return_picture(ctx->handle, latest);
        latest = pic;
    }
    if (!latest)
        return;

    if (latest->bFrameErrorFlag) {
        cedar_api->return_picture(ctx->handle, latest);
        return;
    }
    if (cedar_copy_picture(avctx, frame, latest) == 0)
        *got_picture = 1;
    cedar_api->return_picture(ctx->handle, latest);
}

/*
 * One decode step, as in the proven engine path: after an access unit has
 * been submitted, DecodeVideoStream reports whether a frame came out. A
 * "needs more input" answer is not retried here, it goes back to libavcodec
 * as EAGAIN so the next packet is fed, exactly like cedar_sdl.c, which calls
 * decode once per received frame.
 */
static int cedar_step(AVCodecContext *avctx, AVFrame *frame, int *got_picture)
{
    H264CedarContext *ctx = avctx->priv_data;
    int r = cedar_api->decode(ctx->handle, 0, 0, 0, 0);

    *got_picture = 0;
    if (ctx->submitted <= 2)
        cedar_note("DecodeVideoStream -> %d (%s)", r, cedar_result_name(r));

    switch (r) {
    case CEDAR_RESULT_FRAME_DECODED:
    case CEDAR_RESULT_KEYFRAME_DECODED:
        if (ctx->pending_frames > 0)
            ctx->pending_frames--;
        cedar_take_picture(avctx, frame, got_picture);
        ctx->decoded = 1;
        return 0;
    case CEDAR_RESULT_NO_BITSTREAM:
    case CEDAR_RESULT_CONTINUE:
    case CEDAR_RESULT_OK:
    case CEDAR_RESULT_NO_FRAME_BUFFER:
        /*
         * The parser has not closed a frame yet. Not an error: the caller
         * consumes the packet and libavcodec feeds the next one.
         */
        return 0;
    case CEDAR_RESULT_RESOLUTION_CHANGE:
        /* Not recoverable in place: mpv counts this and falls back. */
        cedar_warn("resolution change");
        return AVERROR_INVALIDDATA;
    default:
        cedar_warn("decode error (%s)", cedar_result_name(r));
        return AVERROR_INVALIDDATA;
    }
}

/*
 * One packet through h264_mp4toannexb, then into the vendor parser. Returns
 * the submit result, or AVERROR(EAGAIN) when the filter keeps the packet
 * (a key frame that carries its parameter sets: the next call completes it).
 */
static int cedar_submit_packet(AVCodecContext *avctx, AVPacket *in)
{
    H264CedarContext *ctx = avctx->priv_data;
    AVPacket *out = av_packet_alloc();
    int ret;

    if (!out)
        return AVERROR(ENOMEM);
    ret = av_bsf_send_packet(ctx->annexb, in);
    if (ret < 0)
        goto done;
    while ((ret = av_bsf_receive_packet(ctx->annexb, out)) >= 0) {
        ret = cedar_submit(avctx, out->data, out->size, out->pts);
        av_packet_unref(out);
        if (ret < 0)
            break;
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        ret = 0;
done:
    av_packet_free(&out);
    return ret;
}

static int h264_cedar_decode(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    H264CedarContext *ctx = avctx->priv_data;
    int ret;

    *got_frame = 0;

    if (cedar_failed) {
        /* One error was reported; mpv counts them and falls back. */
        return AVERROR_INVALIDDATA;
    }

    if (avpkt->size == 0) {
        /*
         * End of stream. The parser closes the last frame when the next
         * access unit starts, so give it an access unit delimiter (through
         * the same filter, so it stays Annex-B) and take whatever came out.
         * This must happen before the packet is considered consumed.
         */
        AVPacket aud = { 0 };

        if (!ctx->handle || ctx->eof_returned)
            return AVERROR_EOF;
        if (!ctx->flushed) {
            static const uint8_t aud_data[] = { 0, 0, 0, 1, 0x09, 0xf0 };

            ctx->flushed = 1;
            if (av_new_packet(&aud, sizeof(aud_data)) >= 0) {
                memcpy(aud.data, aud_data, sizeof(aud_data));
                aud.pts = CEDAR_AU_MARKER;
                cedar_submit_packet(avctx, &aud);
                av_packet_unref(&aud);
            }
        }
        if (cedar_step(avctx, frame, got_frame) < 0) {
            cedar_failed = 1;
            return AVERROR_INVALIDDATA;
        }
        if (!*got_frame) {
            ctx->eof_returned = 1;
            return AVERROR_EOF;
        }
        return 0;
    }

    if (!ctx->handle) {
        /*
         * The vendor engine sizes its buffers at InitializeVideoDecoder and
         * cannot change them later, so the picture size must be the real
         * one. ff_set_dimensions() was fed the parsed SPS (avctx->width and
         * height); coded_width/height may still be 0 and must not become a
         * guess such as 1920x1080.
         */
        int width  = avctx->width  > 0 ? avctx->width  : avctx->coded_width;
        int height = avctx->height > 0 ? avctx->height : avctx->coded_height;

        if (width <= 0 || height <= 0) {
            cedar_warn("no picture size yet (%dx%d), deferring", width, height);
            return avpkt->size;
        }
        ret = cedar_open_decoder(avctx, width, height);
        if (ret < 0) {
            cedar_failed = 1;
            return ret;
        }
    }

    if (!ctx->submitted && cedar_verbose()) {
        char hex[32 * 3 + 1];
        int n = FFMIN(avpkt->size, 32), i;

        for (i = 0; i < n; i++)
            snprintf(hex + i * 3, 4, "%02x ", avpkt->data[i]);
        hex[n * 3] = 0;
        cedar_note("first packet, first %d bytes: %s", n, hex);
    }

    ret = cedar_submit_packet(avctx, avpkt);
    if (ret == AVERROR(ENOMEM)) {
        /* Stream buffer full: drop this one, the engine recovers at the
         * next key frame. */
        cedar_warn("dropped a %d byte access unit", avpkt->size);
        return avpkt->size;
    }
    if (ret < 0) {
        cedar_failed = 1;
        return ret;
    }
    /*
     * This is an FF_CODEC_DECODE_CB decoder: the return value is the number
     * of consumed bytes or a negative error, never AVERROR(EAGAIN)
     * (decode_simple_internal asserts on that). "Needs more input" is
     * therefore a consumed packet with no frame, and libavcodec feeds the
     * next one by itself.
     */
    if (ctx->submitted) {
        ret = cedar_step(avctx, frame, got_frame);
        if (ret < 0) {
            cedar_failed = 1;
            return ret;
        }
    }
    return avpkt->size;
}

static av_cold int h264_cedar_init(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;
    int ret;

    /* priv_data is not zeroed by this FFmpeg (no .flush at init). */
    memset(ctx, 0, sizeof(*ctx));
    /*
     * One hardware decoder per process: the vendor engine is a single device
     * (VE + SBM), and the problems seen on this unit come from opening or
     * tearing down more than one. A second file in the same session gets
     * software decoding.
     */
    if (cedar_busy) {
        cedar_note("the hardware decoder is already in use in this process");
        return AVERROR_DECODER_NOT_FOUND;
    }
    ret = cedar_load(avctx);
    if (ret < 0)
        return ret;
    cedar_busy = 1;
    cedar_warn("hardware decoder enabled (h264_cedar)");
    return 0;
}

static av_cold int h264_cedar_close(AVCodecContext *avctx)
{
    cedar_close_decoder(avctx);
    cedar_busy = 0;
    return 0;
}

static void h264_cedar_flush(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;

    /*
     * Seeks and discards must start clean. ResetVideoDecoder() is part of
     * the vendor API but its teardown has been where this unit aborted, so
     * only a decoder that has decoded and has no picture outstanding is
     * reset; otherwise the stream restarts with the next key frame and the
     * pictures are simply discarded by the parser.
     */
    ctx->flushed = 0;
    ctx->eof_returned = 0;
    if (ctx->handle && ctx->decoded && !ctx->pending_frames)
        cedar_api->reset(ctx->handle);
}

const FFCodec ff_h264_cedar_decoder = {
    .p.name         = "h264_cedar",
    CODEC_LONG_NAME("Allwinner CedarX H.264 (A133P vendor decoder)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    /* It is a real decoder (no hwaccel framework); AVOID_PROBING keeps
     * avcodec_find_decoder() picking FFmpeg's own H.264 decoder. */
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
                      AV_CODEC_CAP_AVOID_PROBING,
    .priv_data_size = sizeof(H264CedarContext),
    .init           = h264_cedar_init,
    .close          = h264_cedar_close,
    .flush          = h264_cedar_flush,
    FF_CODEC_DECODE_CB(h264_cedar_decode),
};
