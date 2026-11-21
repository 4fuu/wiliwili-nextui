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
 * reach the app log. Every step of a hardware decode that can hang or abort
 * inside the vendor library is written to stderr and flushed at once, so the
 * log shows the call that preceded a crash even when the process dies.
 */
static void cedar_note(const char *fmt, ...)
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
static int cedar_used;   /* the vendor decoder was initialized once already */
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
            cedar_note("missing symbol %s",    \
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
        cedar_note("dlopen libvdecoder.so: %s",
           dlerror());
        goto fail;
    }
    api->videoengine = cedar_open_lib("libvideoengine.so");
    if (!api->videoengine) {
        cedar_note("dlopen libvideoengine.so: %s", dlerror());
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
        cedar_note("/usr/lib/libawh264.so is not installed");
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

#define CEDAR_DRAIN_MS   100        /* how long to wait for a stalled frame */
#define CEDAR_VBV_BYTES  (4 << 20)

typedef struct H264CedarContext {
    CedarDecoder *handle;
    int width, height;
    int logged_picture;  /* first picture already described in the log */
    int submitted;       /* access units accepted by the vendor parser */
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
     * The holding-buffer numbers the Cedar wrapper uses on this device; its
     * video path runs 600+ frames with them.
     */
    cfg.nDeInterlaceHoldingFrameBufferNum = 0;
    cfg.nDisplayHoldingFrameBufferNum     = 3;
    cfg.nDecodeSmoothFrameBufferNum       = 1;

    ctx->handle = cedar_api->create();
    if (!ctx->handle) {
        cedar_note("CreateVideoDecoder failed");
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
        cedar_note("InitializeVideoDecoder failed (%d)", ret);
        return AVERROR_EXTERNAL;
    }
    ctx->width   = width;
    ctx->height  = height;
    ctx->flushed = 0;
    cedar_note("decoder opened (%dx%d)", width,
           height);
    return 0;
}

static void cedar_close_decoder(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;

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
        cedar_note("SubmitVideoStreamData failed");
        return AVERROR_EXTERNAL;
    }
    if (ctx->submitted == 0)
        cedar_note("SubmitVideoStreamData -> 0");
    ctx->submitted++;
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
        cedar_note("unsupported picture format %d",
           pic->ePixelFormat);
        return AVERROR_PATCHWELCOME;
    }
    if (!pic->pData0 || !pic->pData1 || pic->nLineStride <= 0) {
        cedar_note("picture has no CPU mapping (pData0=%p pData1=%p)",
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
        cedar_note("frame buffer allocation returned no buffer");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/*
 * Runs the engine until one picture comes out or the input is exhausted.
 * *got_picture is set when the caller must release a returned picture.
 */
static int cedar_run(AVCodecContext *avctx, AVFrame *frame, int *got_picture)
{
    H264CedarContext *ctx = avctx->priv_data;
    int ret = AVERROR(EAGAIN);
    int64_t deadline = 0;

    *got_picture = 0;
    for (;;) {
        int r;
        int64_t now;

        if (ctx->submitted <= 2)
            cedar_note("about to DecodeVideoStream (submitted %d)",
                       ctx->submitted);
        r = cedar_api->decode(ctx->handle, 0, 0, 0, 0);
        if (ctx->submitted <= 2)
            cedar_note("DecodeVideoStream -> %d (%s)", r,
                       cedar_result_name(r));

        switch (r) {
        case CEDAR_RESULT_FRAME_DECODED:
        case CEDAR_RESULT_KEYFRAME_DECODED: {
            CedarVideoPicture *pic = cedar_api->request_picture(ctx->handle, 0);

            if (!pic) {
                ret = AVERROR(EAGAIN);
                break;
            }
            ret = pic->bFrameErrorFlag ? AVERROR_INVALIDDATA
                                       : cedar_copy_picture(avctx, frame, pic);
            /*
             * The vendor tags each picture with the pts of the access unit
             * it belongs to, which is the only mapping that survives
             * reordering. ff_decode_frame_props() kept that value.
             */
            if (!ret && pic->nPts != AV_NOPTS_VALUE &&
                pic->nPts != CEDAR_AU_MARKER && pic->nPts > INT64_MIN / 2)
                frame->pts = pic->nPts;
            if (ret < 0)
                cedar_note("picture %d could not be copied", pic->nID);
            cedar_api->return_picture(ctx->handle, pic);
            if (ret < 0)
                break;
            *got_picture = 1;
            break;
        }
        case CEDAR_RESULT_RESOLUTION_CHANGE:
            cedar_note("resolution change");
            ret = AVERROR(EAGAIN);
            break;
        case CEDAR_RESULT_NO_BITSTREAM:
        case CEDAR_RESULT_CONTINUE:
        case CEDAR_RESULT_OK:
            ret = AVERROR(EAGAIN);
            break;
        case CEDAR_RESULT_NO_FRAME_BUFFER:
            /* The display is holding every picture; it frees them itself. */
            ret = AVERROR(EAGAIN);
            break;
        default:
            cedar_note("decode result %s (%d)",
           cedar_result_name(r), r);
            ret = AVERROR_INVALIDDATA;
            break;
        }
        if (*got_picture || ret < 0)
            break;
        /* EAGAIN from NO_FRAME_BUFFER is the only one worth waiting for. */
        if (r != CEDAR_RESULT_NO_FRAME_BUFFER)
            break;
        now = av_gettime_relative();
        if (!deadline)
            deadline = now + CEDAR_DRAIN_MS * 1000;
        if (now > deadline)
            break;
        av_usleep(1000);
    }
    return ret;
}

static int h264_cedar_decode(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    H264CedarContext *ctx = avctx->priv_data;
    int ret;

    if (avpkt->size == 0) {
        if (!ctx->handle || ctx->eof_returned || cedar_failed) {
            *got_frame = 0;
            return AVERROR_EOF;
        }
        if (!ctx->flushed) {
            /*
             * One more access unit makes the parser close the last frame it
             * is still holding (B-frames, or a final frame without the next
             * access unit delimiter).
             */
            static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xf0 };

            ctx->flushed = 1;
            ret = cedar_submit(avctx, aud, sizeof(aud), CEDAR_AU_MARKER);
            if (ret < 0) {
                *got_frame = 0;
                return ret;
            }
        }
        ret = cedar_run(avctx, frame, got_frame);
        if (ret < 0 && !*got_frame)
            ctx->eof_returned = 1;
        *got_frame = *got_frame ? 1 : 0;
        return *got_frame ? 0 : AVERROR_EOF;
    }

    if (cedar_failed) {
        /* Already reported once; from here mpv counts errors and switches
         * to software decoding. */
        *got_frame = 0;
        return AVERROR_INVALIDDATA;
    }

    if (!ctx->handle) {
        int width  = avctx->coded_width  > 0 ? avctx->coded_width  : 1920;
        int height = avctx->coded_height > 0 ? avctx->coded_height : 1080;

        ret = cedar_open_decoder(avctx, width, height);
        if (ret < 0) {
            cedar_failed = 1;
            return ret;
        }
        cedar_used = 1;
    }

    ret = cedar_submit(avctx, avpkt->data, avpkt->size, avpkt->pts);
    if (ret == AVERROR(ENOMEM)) {
        /* The stream buffer has no room for this access unit: drop it, the
         * decoder recovers at the next key frame instead of dying. */
        cedar_note("dropped a %d byte access unit", avpkt->size);
        *got_frame = 0;
        return avpkt->size;
    }
    if (ret < 0) {
        cedar_failed = 1;
        return ret;
    }

    /*
     * The Cedar parser thread splits access units asynchronously. Until it
     * has one, DecodeVideoStream reports NO_BITSTREAM; return EAGAIN so
     * libavcodec does not count that as an error and the packet is tried
     * again once more input has arrived.
     */
    ret = cedar_run(avctx, frame, got_frame);
    *got_frame = *got_frame ? 1 : 0;
    if (*got_frame)
        return avpkt->size;
    if (ret == AVERROR_INVALIDDATA) {
        /*
         * The engine is stuck (typically a corrupt stream). Stop using it:
         * mpv counts decode errors and switches to its software decoder,
         * while avcodec_open2() keeps succeeding so mpv does not treat the
         * hardware decoder as one that failed to open.
         */
        cedar_note("giving up on the hardware decoder after a "
               "decode error");
        cedar_failed = 1;
    }
    return ret;
}

static av_cold int h264_cedar_init(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;
    int ret;

    /* priv_data is not zeroed by this FFmpeg (no .flush at init). */
    memset(ctx, 0, sizeof(*ctx));
    ret = cedar_load(avctx);
    if (ret < 0)
        return ret;
    /*
     * This is a one-shot hardware decoder. A file that cannot be decoded
     * with it (InitializeVideoDecoder failure, an unsupported stream) leaves
     * the vendor engine in a state that must not be torn down or reused, so
     * the instance is not opened again and mpv falls back to software.
     */
    if (cedar_used) {
        cedar_note("already used once in this process, not opening "
               "again");
        return AVERROR_DECODER_NOT_FOUND;
    }
    /*
     * A decode failure is final for this decoder (see h264_cedar_decode).
     * Failing avcodec_open2() instead would make mpv open it again for every
     * packet it sends, and a successful open is what mpv checks for when it
     * decides that no hwdec fallback is needed.
     */
    if (cedar_failed) {
        cedar_note("the vendor decoder failed earlier in this "
               "process, not opening it again");
        return AVERROR_DECODER_NOT_FOUND;
    }
    return 0;
}

static av_cold int h264_cedar_close(AVCodecContext *avctx)
{
    cedar_close_decoder(avctx);
    return 0;
}

static void h264_cedar_flush(AVCodecContext *avctx)
{
    H264CedarContext *ctx = avctx->priv_data;

    ctx->flushed = 0;
    ctx->eof_returned = 0;
    if (ctx->handle)
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
