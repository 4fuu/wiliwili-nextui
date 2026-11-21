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
#include <libavutil/imgutils.h>

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

/* Splits at access unit delimiters when present, else at SPS/PPS/slice NALs. */
static size_t split_aus(const uint8_t *data, size_t len, struct au **out)
{
    const uint8_t *end = data + len;
    const uint8_t *p = data;
    size_t n = 0, cap = 1024;
    struct au *aus = malloc(cap * sizeof(*aus));
    const uint8_t *start = NULL;
    int have_aud = 0;

    for (p = data; p < end; p++) {
        int sc;

        if (!is_start_code(p, end, &sc))
            continue;
        if (p + sc < end && (p[sc] & 0x1f) == 9)
            have_aud = 1;
        p += sc - 1;
    }
    for (p = data; p < end; p++) {
        int sc, type;

        if (!is_start_code(p, end, &sc))
            continue;
        if (p + sc >= end)
            break;
        type = p[sc] & 0x1f;
        /* A new AU starts at an AUD, or (without AUDs) at SPS/PPS/IDR/slice
         * after a slice has already been seen. */
        if (have_aud ? type == 9 : (type == 7 || type == 8 || type == 5)) {
            if (start) {
                if (n == cap) {
                    cap *= 2;
                    aus = realloc(aus, cap * sizeof(*aus));
                }
                aus[n].data = start;
                aus[n].size = (size_t)(p - start);
                n++;
            }
            start = p;
        }
        p += sc - 1;
    }
    if (start) {
        aus[n].data = start;
        aus[n].size = (size_t)(end - start);
        n++;
    }
    *out = aus;
    return n;
}

static void hex_head(const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n && i < 32; i++)
        fprintf(stderr, "%02x ", p[i]);
    fputc('\n', stderr);
}

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
    nau = split_aus(data, len, &aus);
    if (!nau) {
        fprintf(stderr, "no H.264 access units found (Annex-B expected)\n");
        return 1;
    }
    fprintf(stderr, "cedar-decode: %s, %zu bytes, %zu access units\n", argv[optind],
            len, nau);

    codec = avcodec_find_decoder_by_name("h264_cedar");
    if (!codec) {
        fprintf(stderr, "h264_cedar is not in this libavcodec\n");
        return 1;
    }
    ctx = avcodec_alloc_context3(codec);
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "avcodec_open2: %d\n", ret);
        return 1;
    }

    for (i = 0; i < nau; i++) {
        int eof = 0;

        if (i == 0) {
            fprintf(stderr, "first access unit, first 32 bytes: ");
            hex_head(aus[i].data, aus[i].size);
        }
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
            if (errors++ == 0)
                hex_head(aus[i].data, aus[i].size);
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
