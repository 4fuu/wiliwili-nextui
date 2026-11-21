/*
 * cedar-decode: decode an Annex-B H.264 file with the h264_cedar decoder on
 * the device, without the app.
 *
 * Part of the wiliwili NextUI port. It links the pak's own libavcodec (the
 * one mpv loads), so it exercises the same decoder code path as playback:
 * avcodec_send_packet()/avcodec_receive_frame() in a loop, one access unit
 * per packet. Unlike the app it prints what happens per packet, so a crash or
 * a silent stall in the vendor library can be reproduced and located from a
 * shell.
 *
 *   cedar-decode test720p60.264          # 0 = every frame decoded
 *   cedar-decode -q test720p60.264       # quiet: summary only
 *   cedar-decode -n 60 file.264          # stop after 60 frames
 *
 * The h264_cedar decoder itself writes its trace to stderr when
 * H264_CEDAR_TRACE=1 is set; this tool sets it unless -q is given.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

/* One access unit per Annex-B start code sequence, AUD not required. */
struct au {
    const uint8_t *data;
    size_t size;
};

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

static int is_start_code(const uint8_t *p, const uint8_t *end, int *len)
{
    if (end - p >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        *len = 3;
        return 1;
    }
    if (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        *len = 4;
        return 1;
    }
    return 0;
}

/*
 * first_mb_in_slice is the first ue(v) of a slice header. Its value is 0
 * exactly when the first bit is 1 (a ue(v) of 0 is a single 1 bit), which is
 * how a picture's first slice is told apart from the following slices of a
 * multi-slice picture.
 */
static int starts_picture(const uint8_t *p, const uint8_t *end)
{
    return p < end && (*p & 0x80) != 0;
}

/*
 * One packet per picture. Access units are delimited by access unit
 * delimiters; in a stream without them, a picture starts at the slice whose
 * first_mb_in_slice is 0. Parameter sets and SEI stay attached to the picture
 * that follows them, because the vendor parser needs them in the same packet.
 */
static size_t split_aus(const uint8_t *data, size_t len, struct au **out)
{
    const uint8_t *end = data + len;
    const uint8_t *p, *start = data, *slice_seen = NULL;
    size_t n = 0, cap = 0;
    struct au *aus = NULL;

    for (p = data; p < end; p++) {
        int sc;

        if (!is_start_code(p, end, &sc))
            continue;
        if (p + sc >= end)
            break;
        {
            int type = p[sc] & 0x1f;
            int new_picture = 0;

            if (type == 9)
                new_picture = p > start;
            else if (slice_seen && (type == 1 || type == 5) &&
                     starts_picture(p + sc + 1, end))
                new_picture = 1;

            if (new_picture) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 1024;
                    aus = realloc(aus, cap * sizeof(*aus));
                }
                aus[n].data = start;
                aus[n].size = (size_t)(p - start);
                n++;
                start = p;
                slice_seen = NULL;
            }
            if (type == 1 || type == 5)
                slice_seen = p;
        }
        p += sc - 1;
    }
    if (start < end) {
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            aus = realloc(aus, cap * sizeof(*aus));
        }
        aus[n].data = start;
        aus[n].size = (size_t)(end - start);
        n++;
    }
    *out = aus;
    return n;
}

/*
 * AVCC to Annex-B by hand: every NAL unit is a 4-byte length, and every
 * start code is four bytes. The app path gets this from h264_mp4toannexb,
 * which needs the container's avcC extradata; a bare payload has none.
 */
static uint8_t *avcc_to_annexb(const uint8_t *data, size_t len, size_t *out_len)
{
    uint8_t *out = av_malloc(len + 1024);
    size_t off = 0, used = 0;

    if (!out)
        return NULL;
    while (off + 4 <= len) {
        size_t l = ((size_t)data[off] << 24) | ((size_t)data[off + 1] << 16) |
                   ((size_t)data[off + 2] << 8) | data[off + 3];

        if (l == 0 || off + 4 + l > len)
            break;
        out[used++] = 0;
        out[used++] = 0;
        out[used++] = 0;
        out[used++] = 1;
        memcpy(out + used, data + off + 4, l);
        used += l;
        off += 4 + l;
    }
    *out_len = used;
    return out;
}

/*
 * AVCC: every NAL unit is a 4-byte big-endian length. A file that does not
 * start with a start code and whose first length lands on a plausible NAL
 * unit type is AVCC.
 */
static int is_avcc(const uint8_t *data, size_t len)
{
    size_t l;
    int type;

    if (len < 5 || data[0] != 0 || data[1] != 0 || data[2] != 0)
        return 0;
    l = ((size_t)data[0] << 24) | ((size_t)data[1] << 16) |
        ((size_t)data[2] << 8) | data[3];
    if (l == 0 || l == 1 || l > len - 4)
        return 0;
    type = data[4] & 0x1f;
    return type >= 1 && type <= 12;
}

/* H.264 NAL types this decoder handles; HEVC has no AUD of type 9. */
static int is_h264(const uint8_t *au, size_t size)
{
    size_t i;

    for (i = 0; i + 4 < size; i++) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            int type = au[i + 3] & 0x1f;

            return type == 1 || type == 5 || type == 7 || type == 8 ||
                   type == 9;
        }
    }
    return 0;
}

static int g_width, g_height;

int main(int argc, char **argv)
{
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    struct au *aus = NULL;
    uint8_t *data;
    size_t len, nau, i, sent = 0, frames = 0, errors = 0;
    int quiet = 0, limit = 0, opt, ret;

    while ((opt = getopt(argc, argv, "qn:")) != -1) {
        switch (opt) {
        case 'q': quiet = 1; break;
        case 'n': limit = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-q] [-n frames] file.264\n", argv[0]);
            return 2;
        }
    }
    if (optind != argc - 1) {
        fprintf(stderr, "usage: %s [-q] [-n frames] file.264\n", argv[0]);
        return 2;
    }
    if (!quiet && !getenv("H264_CEDAR_TRACE"))
        setenv("H264_CEDAR_TRACE", "1", 0);

    data = read_file(argv[optind], &len);
    if (!data) {
        perror(argv[optind]);
        return 1;
    }
    if (!quiet) {
        size_t k;

        fprintf(stderr, "cedar-decode: first bytes of the file: ");
        for (k = 0; k < len && k < 32; k++)
            fprintf(stderr, "%02x ", data[k]);
        fputc('\n', stderr);
    }
    /*
     * A raw stream carries no container. AVCC input is converted to Annex-B
     * first (the app gets this from h264_mp4toannexb, which needs the
     * container's avcC extradata that a bare payload does not have), then
     * split into access units like any other Annex-B stream.
     */
    if (is_avcc(data, len)) {
        size_t conv_len = 0;
        uint8_t *conv = avcc_to_annexb(data, len, &conv_len);

        if (!conv || conv_len == 0) {
            fprintf(stderr, "AVCC to Annex-B conversion failed\n");
            return 1;
        }
        fprintf(stderr, "cedar-decode: AVCC input, converted to Annex-B "
                        "(%zu bytes)\n", conv_len);
        free(data);
        data = conv;
        len = conv_len;
    } else {
        fprintf(stderr, "cedar-decode: Annex-B input\n");
    }

    nau = split_aus(data, len, &aus);
    if (!nau) {
        fprintf(stderr, "no H.264 access units found\n");
        free(data);
        return 1;
    }
    fprintf(stderr, "cedar-decode: %s, %zu bytes, %zu access units\n",
            argv[optind], len, nau);
    if (!quiet) {
        size_t k;

        fprintf(stderr, "cedar-decode: first bytes fed to the decoder: ");
        for (k = 0; k < len && k < 32; k++)
            fprintf(stderr, "%02x ", data[k]);
        fputc('\n', stderr);
    }
    /* h264_cedar only decodes H.264; say so instead of feeding HEVC to it. */
    if (!is_h264(aus[0].data, aus[0].size)) {
        fprintf(stderr, "cedar-decode: not an H.264 stream (HEVC?); h264_cedar "
                        "does not decode it\n");
        free(aus);
        free(data);
        return 3;
    }

    {
        /*
         * Size from the first access unit only: one picture is enough for
         * the SPS to be parsed, and anything the software decoder prints on
         * the way is suppressed (the app gets the size from mpv instead).
         */
        const AVCodec *sw = avcodec_find_decoder(AV_CODEC_ID_H264);
        AVCodecContext *sctx = sw ? avcodec_alloc_context3(sw) : NULL;
        AVPacket *spkt = av_packet_alloc();
        AVFrame *sframe = av_frame_alloc();

        if (sctx && spkt && sframe) {
            int saved = av_log_get_level();

            av_log_set_level(AV_LOG_QUIET);
            avcodec_open2(sctx, sw, NULL);
            spkt->data = (uint8_t *)aus[0].data;
            spkt->size = (int)aus[0].size;
            if (avcodec_send_packet(sctx, spkt) >= 0)
                avcodec_receive_frame(sctx, sframe);
            av_log_set_level(saved);
            g_width  = sctx->width  > 0 ? sctx->width  : sframe->width;
            g_height = sctx->height > 0 ? sctx->height : sframe->height;
        }
        av_frame_free(&sframe);
        av_packet_free(&spkt);
        avcodec_free_context(&sctx);
    }

    codec = avcodec_find_decoder_by_name("h264_cedar");
    if (!codec) {
        fprintf(stderr, "h264_cedar is not in this libavcodec\n");
        free(aus);
        free(data);
        return 1;
    }
    ctx = avcodec_alloc_context3(codec);
    /*
     * The app's libavcodec hands the decoder the container's SPS/PPS as
     * extradata; without one the decoder learns the size from the stream.
     * The packet time base must match the pts the packets carry.
     */
    ctx->pkt_timebase = (AVRational){ 1, 1000 };
    if (g_width > 0 && g_height > 0) {
        ctx->width  = g_width;
        ctx->height = g_height;
        fprintf(stderr, "cedar-decode: SPS says %dx%d\n", g_width, g_height);
    }
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "avcodec_open2: %d\n", ret);
        return 1;
    }

    for (i = 0; i < nau; i++) {
        int eof = 0;

        if (getenv("CEDAR_DUMP_AU") && i == 0) {
            const char *path = getenv("CEDAR_DUMP_AU");
            FILE *f = fopen(path, "wb");

            if (f) {
                fwrite(aus[i].data, 1, aus[i].size, f);
                fclose(f);
                fprintf(stderr, "first access unit written to %s\n", path);
            }
        }
        pkt->data = (uint8_t *)aus[i].data;
        pkt->size = (int)aus[i].size;
        pkt->pts = pkt->dts = (int64_t)i;
        ret = avcodec_send_packet(ctx, pkt);
        sent++;
        if (ret < 0) {
            fprintf(stderr, "au %zu (%zu bytes): send_packet %d\n", i,
                    aus[i].size, ret);
            if (errors++ == 0) {
                size_t k;

                fprintf(stderr, "  first bytes: ");
                for (k = 0; k < aus[i].size && k < 32; k++)
                    fprintf(stderr, "%02x ", aus[i].data[k]);
                fputc('\n', stderr);
            }
        }
        while (!eof) {
            ret = avcodec_receive_frame(ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                fprintf(stderr, "au %zu: receive_frame %d\n", i, ret);
                errors++;
                eof = 1;
                break;
            }
            frames++;
            if (!quiet)
                fprintf(stderr, "frame %zu: %s %dx%d pts %lld (au %zu)\n", frames,
                        av_get_pix_fmt_name(frame->format), frame->width,
                        frame->height, (long long)frame->pts, i);
            av_frame_unref(frame);
            if (limit && (int)frames >= limit) {
                i = nau;
                break;
            }
        }
    }
    /* Flush whatever the decoder still holds. */
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) == 0) {
        frames++;
        av_frame_unref(frame);
    }

    avcodec_free_context(&ctx);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    free(aus);
    free(data);
    fprintf(stderr, "summary: sent %zu/%zu, frames %zu, errors %zu\n", sent, nau,
            frames, errors);
    return errors == 0 && frames > 0 ? 0 : 1;
}
