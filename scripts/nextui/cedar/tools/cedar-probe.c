/*
 * cedar-probe: verify Cedar hardware decode on a TrimUI (A133P) device.
 *
 * Part of the wiliwili NextUI port spike, derived from moonlight-tsp's
 * tsp-videotest.c (Copyright (c) isalin, GPL-3.0-or-later). Only the decode
 * and verification parts are kept; the display layer is optional.
 *
 *   cedar-probe                 environment report only (no stream needed)
 *   cedar-probe -i clip.264     decode and report
 *   cedar-probe -i clip.264 -d  decode and show the pictures on the panel
 *   cedar-probe -i clip.264 -o dump.pgm   also write one decoded frame
 *
 * The exit status is 0 only if the decoder opened, decoded every access
 * unit without error and produced at least one picture.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cedar.h"
#include "cedar_sync.h"
#include "disp.h"
#include "ion.h"

#define MAX_HELD 8

struct au {
    size_t off;
    size_t len;
};

struct held_pic {
    CedarVideoPicture *pic;
    uint64_t retired_ns;
};

static volatile sig_atomic_t g_stop;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static double cpu_seconds(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 +
           (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}

static long rss_kib(void)
{
    char line[128];
    long kib = -1;
    FILE *f = fopen("/proc/self/status", "re");

    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "VmRSS: %ld", &kib) == 1)
            break;
    fclose(f);
    return kib;
}

/* Prints which of the vendor pieces this device actually has. */
static void report_environment(void)
{
    static const char *const libs[] = {
        "libvdecoder.so",    "libvideoengine.so", "libawh264.so",
        "libawh265.so",      "libVE.so",          "libMemAdapter.so",
        "libcdc_base.so",    "libsbm.so",         "libfbm.so",
    };
    static const char *const nodes[] = { "/dev/cedar_dev", "/dev/ion" };
    char buf[256];
    size_t i;

    printf("== vendor libraries (/usr/lib)\n");
    for (i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        char path[128];
        struct stat st;

        snprintf(path, sizeof(path), "/usr/lib/%s", libs[i]);
        if (stat(path, &st) == 0)
            printf("  %-18s present %lld bytes\n", libs[i],
                   (long long)st.st_size);
        else
            printf("  %-18s MISSING\n", libs[i]);
    }
    printf("== device nodes\n");
    for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
        printf("  %-14s %s\n", nodes[i], access(nodes[i], R_OK | W_OK) == 0
                                            ? "present, rw"
                                            : "missing or not rw");
    {
        FILE *f = fopen("/proc/modules", "re");

        if (f != NULL) {
            printf("== modules\n");
            while (fgets(buf, sizeof(buf), f) != NULL)
                if (strstr(buf, "cedar") != NULL || strstr(buf, "ion") != NULL)
                    printf("  %s", buf);
            fclose(f);
        }
    }
    {
        /* VE interrupts: 0 means the engine has never run since boot. */
        static const char *const irq[] = { "/proc/interrupts", NULL };
        FILE *f = fopen(irq[0], "re");

        if (f != NULL) {
            printf("== interrupts\n");
            while (fgets(buf, sizeof(buf), f) != NULL)
                if (strstr(buf, "cedar") != NULL || strstr(buf, "ve") != NULL ||
                    strstr(buf, "ion") != NULL || strstr(buf, "disp") != NULL)
                    printf("  %s", buf);
            fclose(f);
        }
    }
    /* The sizes cedarc_abi.h asserts against the vendor DWARF. */
    printf("== ABI\n  CedarVideoPicture=%zu CedarVConfig=%zu "
           "StreamInfo=%zu StreamDataInfo=%zu (expected 336 208 64 64)\n",
           sizeof(CedarVideoPicture), sizeof(CedarVConfig),
           sizeof(CedarVideoStreamInfo), sizeof(CedarVideoStreamDataInfo));
}

static int is_aud(const unsigned char *nal, int hevc)
{
    if (hevc)
        return ((nal[0] >> 1) & 0x3f) == 35;
    return (nal[0] & 0x1f) == 9;
}

static size_t split_access_units(const unsigned char *data, size_t len,
                                 int hevc, struct au **out)
{
    size_t cap = 1024, n = 0, i;
    struct au *aus = malloc(cap * sizeof(*aus));

    if (aus == NULL)
        return 0;
    for (i = 0; i + 3 < len; i++) {
        size_t start;

        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (!is_aud(data + i + 3, hevc)) {
            i += 2;
            continue;
        }
        start = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
        if (n == cap) {
            struct au *tmp;

            cap *= 2;
            tmp = realloc(aus, cap * sizeof(*aus));
            if (tmp == NULL) {
                free(aus);
                return 0;
            }
            aus = tmp;
        }
        if (n > 0)
            aus[n - 1].len = start - aus[n - 1].off;
        aus[n].off = start;
        aus[n].len = 0;
        n++;
        i += 2;
    }
    if (n > 0)
        aus[n - 1].len = len - aus[n - 1].off;
    *out = aus;
    return n;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    struct stat st;
    unsigned char *buf;
    size_t got = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    while (buf != NULL && got < (size_t)st.st_size) {
        ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

        if (r <= 0) {
            free(buf);
            buf = NULL;
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    *len = got;
    return buf;
}

static void dump_picture(const CedarVideoPicture *p)
{
    printf("  picture: id=%d fmt=%d %dx%d stride=%d crop l=%d t=%d r=%d b=%d "
           "pts=%lld phyY=%#llx (C-Y=%lld) fd=%d full_range=%d matrix=%d "
           "afbc=%d 10bit=%d err=%d pData0=%p\n",
           p->nID, p->ePixelFormat, p->nWidth, p->nHeight, p->nLineStride,
           p->nLeftOffset, p->nTopOffset, p->nRightOffset, p->nBottomOffset,
           (long long)p->nPts, (unsigned long long)p->phyYBufAddr,
           (long long)(p->phyCBufAddr - p->phyYBufAddr), p->nBufFd,
           p->video_full_range_flag, p->matrix_coeffs, p->bEnableAfbcFlag,
           p->b10BitPicFlag, p->bFrameErrorFlag, (void *)p->pData0);
}

/* Writes the visible luma window as a binary PGM. */
static int dump_luma(const CedarVideoPicture *p, const char *path)
{
    unsigned x0 = 0, y0 = 0, w = (unsigned)p->nWidth, h = (unsigned)p->nHeight;
    const unsigned char *plane = (const unsigned char *)p->pData0;
    unsigned stride = (unsigned)p->nLineStride;
    FILE *f;
    unsigned row;

    if (plane == NULL || stride == 0)
        return -1;
    if (p->nRightOffset > p->nLeftOffset && p->nBottomOffset > p->nTopOffset) {
        x0 = (unsigned)p->nLeftOffset;
        y0 = (unsigned)p->nTopOffset;
        w = (unsigned)(p->nRightOffset - p->nLeftOffset);
        h = (unsigned)(p->nBottomOffset - p->nTopOffset);
    }
    f = fopen(path, "we");
    if (f == NULL)
        return -1;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    for (row = 0; row < h; row++)
        fwrite(plane + (size_t)(y0 + row) * stride + x0, 1, w, f);
    return fclose(f) == 0 ? 0 : -1;
}

static int picture_to_frame(const CedarVideoPicture *p, int color_space,
                            const struct dispv *d, struct dispv_frame *f)
{
    unsigned stride = (unsigned)p->nLineStride;
    unsigned w, h;

    memset(f, 0, sizeof(*f));
    if (p->ePixelFormat == CEDAR_PIXEL_NV12)
        f->format = TSP_DISP_FORMAT_YUV420_SP_UVUV;
    else if (p->ePixelFormat == CEDAR_PIXEL_NV21)
        f->format = TSP_DISP_FORMAT_YUV420_SP_VUVU;
    else
        return -1;
    if (p->phyYBufAddr == 0 || p->phyCBufAddr == 0 || stride == 0)
        return -1;
    f->dmabuf_fd = -1;
    f->addr_y = p->phyYBufAddr;
    f->addr_c = p->phyCBufAddr;
    f->color_space = color_space;
    f->stride = stride;
    if (p->phyCBufAddr > p->phyYBufAddr)
        f->alloc_h = (unsigned)((p->phyCBufAddr - p->phyYBufAddr) / stride);
    else
        f->alloc_h = ((unsigned)p->nHeight + 15u) & ~15u;
    if (p->nRightOffset > p->nLeftOffset &&
        p->nBottomOffset > p->nTopOffset) {
        f->crop_x = (unsigned)p->nLeftOffset;
        f->crop_y = (unsigned)p->nTopOffset;
        w = (unsigned)(p->nRightOffset - p->nLeftOffset);
        h = (unsigned)(p->nBottomOffset - p->nTopOffset);
    } else {
        w = (unsigned)p->nWidth;
        h = (unsigned)p->nHeight;
    }
    f->crop_w = w;
    f->crop_h = h;
    f->win = dispv_fit(d->screen_w, d->screen_h, w, h);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-i stream.264] [-c h264|h265] [-s WxH] [-l LOOPS]\n"
            "          [-t SECONDS] [-d] [-o dump.pgm] [-N FRAME] [-v]\n"
            "  no -i: report the Cedar environment and exit\n"
            "  -d: show every picture on a display layer (resets it on exit)\n"
            "  -o: write frame -N's luma as a PGM (default frame 10)\n"
            "  -w MS: retry no-bitstream decodes for up to MS ms (default 100)\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct cedar_config cfg = {
        .codec = CEDAR_CODEC_H264,
        .width = 1280,
        .height = 720,
        .fps = 30,
        .pixel_format = CEDAR_PIXEL_NV21,
        .rotate = 0,
        .display_hold = 3,
        .smooth = 1,
        .vbv_bytes = 4 << 20,
    };
    const char *input = NULL, *dump_path = NULL;
    int display = 0, verbose = 0, loops = 1, seconds = 30, wait_ms = 100;
    unsigned long dump_at = 10;
    struct held_pic held[MAX_HELD];
    int nheld = 0;
    char err[256];
    struct cedar_decoder *dec;
    struct dispv disp = { .fd = -1 };
    unsigned char *data = NULL;
    size_t len, nau, i;
    struct au *aus = NULL;
    unsigned long results[8] = { 0 };
    unsigned long submitted = 0, pictures = 0, shown = 0, errors = 0,
                  frame_err = 0, nobuf = 0, nobits = 0, reschange = 0;
    uint64_t t_open, t_start, t_end;
    double cpu_start;
    long rss_start;
    int opt, rc = 0;

    while ((opt = getopt(argc, argv, "i:c:s:l:t:do:N:vw:")) != -1) {
        switch (opt) {
        case 'i': input = optarg; break;
        case 'c':
            cfg.codec = strcmp(optarg, "h265") == 0 ? CEDAR_CODEC_H265
                                                    : CEDAR_CODEC_H264;
            break;
        case 's':
            if (sscanf(optarg, "%dx%d", &cfg.width, &cfg.height) != 2) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'l': loops = atoi(optarg); break;
        case 't': seconds = atoi(optarg); break;
        case 'd': display = 1; break;
        case 'o': dump_path = optarg; break;
        case 'N': dump_at = strtoul(optarg, NULL, 0); break;
        case 'v': verbose = 1; break;
        case 'w': wait_ms = atoi(optarg); break;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (loops < 1)
        loops = 1;

    printf("cedar-probe: wiliwili NextUI Cedar spike\n");
    report_environment();

    if (input == NULL) {
        printf("no stream given: environment report only\n");
        return 0;
    }

    data = read_file(input, &len);
    if (data == NULL) {
        perror(input);
        return 1;
    }
    nau = split_access_units(data, len, cfg.codec == CEDAR_CODEC_H265, &aus);
    if (nau == 0) {
        fprintf(stderr, "no access unit delimiters in %s; re-mux with\n"
                        "  ffmpeg -i in.mp4 -c copy -bsf:v h264_metadata=aud=insert "
                        "-f h264 out.264\n", input);
        return 1;
    }
    printf("stream: %s codec=%s bytes=%zu access_units=%zu loops=%d\n", input,
           cfg.codec == CEDAR_CODEC_H265 ? "h265" : "h264", len, nau, loops);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    t_open = now_ns();
    if (cedar_load(err, sizeof(err)) != 0) {
        fprintf(stderr, "cedar_load: %s\n", err);
        return 1;
    }
    printf("plugins: h264=%s h265=%s\n",
           cedar_have_plugin(CEDAR_CODEC_H264) ? "registered" : "MISSING",
           cedar_have_plugin(CEDAR_CODEC_H265) ? "registered" : "MISSING");
    if (!cedar_have_plugin(cfg.codec)) {
        fprintf(stderr, "cedar_open: vendor plugin for %s is not installed\n",
                cfg.codec == CEDAR_CODEC_H265 ? "h265" : "h264");
        return 1;
    }
    dec = cedar_open(&cfg, err, sizeof(err));
    if (dec == NULL) {
        fprintf(stderr, "cedar_open: %s\n", err);
        return 1;
    }
    printf("decoder: opened in %.1f ms (fmt=%d hold=%d smooth=%d vbv=%d)\n",
           (now_ns() - t_open) / 1e6, cfg.pixel_format, cfg.display_hold,
           cfg.smooth, cfg.vbv_bytes);

    if (display) {
        if (dispv_open(&disp, 0, 0, 32) != 0) {
            perror("open /dev/disp");
            rc = 1;
            goto out;
        }
        printf("display: screen %ux%u\n", disp.screen_w, disp.screen_h);
    }

    cpu_start = cpu_seconds();
    rss_start = rss_kib();
    t_start = now_ns();

    for (int loop = 0; loop < loops && !g_stop; loop++) {
        for (i = 0; i < nau && !g_stop; i++) {
            struct cedar_segment seg = { data + aus[i].off, aus[i].len };
            CedarVideoPicture *pic, *latest = NULL;
            uint64_t t_submit;
            int ret = CEDAR_RESULT_CONTINUE, tries = 0;

            if ((now_ns() - t_start) / 1000000000ull >= (uint64_t)seconds)
                g_stop = 1;
            t_submit = now_ns();
            ret = cedar_submit(dec, &seg, 1, (int64_t)submitted * 33333);
            if (ret != 0) {
                fprintf(stderr, "submit failed (%d) at au %zu\n", ret, i);
                errors++;
                continue;
            }
            submitted++;

            for (tries = 0; tries < 2000; tries++) {
                ret = cedar_decode(dec);
                if (ret >= 0 && ret < 8)
                    results[ret]++;
                if (ret == CEDAR_RESULT_NO_FRAME_BUFFER)
                    nobuf++;
                if (ret == CEDAR_RESULT_RESOLUTION_CHANGE)
                    reschange++;
                if (ret == CEDAR_RESULT_NO_BITSTREAM) {
                    nobits++;
                    /*
                     * The parser thread splits frames asynchronously; wait
                     * for this submission instead of leaving it behind.
                     */
                    if (wait_ms > 0 &&
                        now_ns() - t_submit < (uint64_t)wait_ms * 1000000ull) {
                        usleep(200);
                        continue;
                    }
                }
                if (ret == CEDAR_RESULT_FRAME_DECODED ||
                    ret == CEDAR_RESULT_KEYFRAME_DECODED) {
                    if (cedar_valid_pictures(dec) > 0)
                        break;
                    continue;
                }
                if (ret == CEDAR_RESULT_CONTINUE || ret == CEDAR_RESULT_OK)
                    continue;
                break;
            }
            if (ret < 0 || ret == CEDAR_RESULT_UNSUPPORTED) {
                errors++;
                if (errors < 5)
                    fprintf(stderr, "decode result %d (%s) at au %zu\n", ret,
                            cedar_result_name(ret), i);
            }

            while ((pic = cedar_request_picture(dec)) != NULL) {
                pictures++;
                if (pic->bFrameErrorFlag)
                    frame_err++;
                if (pictures == 1)
                    printf("first picture after %.1f ms (submitted=%lu)\n",
                           (now_ns() - t_submit) / 1e6, submitted);
                if (pictures == 1 || verbose)
                    dump_picture(pic);
                if (dump_path != NULL && pictures == dump_at &&
                    dump_luma(pic, dump_path) == 0)
                    printf("  dump: picture %lu luma -> %s\n", pictures,
                           dump_path);
                if (latest != NULL)
                    cedar_return_picture(dec, latest);
                latest = pic;
            }
            if (latest == NULL)
                continue;

            if (display) {
                struct dispv_frame f;
                int j;

                if (picture_to_frame(latest, TSP_DISP_BT601, &disp, &f) != 0) {
                    fprintf(stderr, "picture cannot be displayed (fmt=%d "
                                    "phyY=%#llx)\n",
                            latest->ePixelFormat,
                            (unsigned long long)latest->phyYBufAddr);
                    cedar_return_picture(dec, latest);
                    g_stop = 1;
                    rc = 1;
                    break;
                }
                if (shown == 0)
                    printf("  frame: stride=%u alloc_h=%u crop=%u,%u %ux%u "
                           "win=%d,%d %ux%u\n",
                           f.stride, f.alloc_h, f.crop_x, f.crop_y, f.crop_w,
                           f.crop_h, f.win.x, f.win.y, f.win.width,
                           f.win.height);
                if (dispv_show(&disp, &f) != 0) {
                    perror("DISP_LAYER_SET_CONFIG");
                    cedar_return_picture(dec, latest);
                    g_stop = 1;
                    rc = 1;
                    break;
                }
                shown++;
                for (j = 0; j < nheld; j++)
                    if (held[j].retired_ns == 0)
                        held[j].retired_ns = now_ns();
                if (nheld < MAX_HELD) {
                    held[nheld].pic = latest;
                    held[nheld].retired_ns = 0;
                    nheld++;
                } else {
                    cedar_return_picture(dec, latest);
                }
                for (j = 0; j < nheld;) {
                    if (held[j].retired_ns != 0 &&
                        now_ns() >= held[j].retired_ns + 34000000ull) {
                        cedar_return_picture(dec, held[j].pic);
                        held[j] = held[--nheld];
                    } else {
                        j++;
                    }
                }
            } else {
                cedar_return_picture(dec, latest);
            }
        }
    }
    t_end = now_ns();

    printf("summary: wall=%.2fs submitted=%lu pictures=%lu shown=%lu "
           "fps=%.1f errors=%lu frame_errors=%lu\n",
           (t_end - t_start) / 1e9, submitted, pictures, shown,
           pictures / ((t_end - t_start) / 1e9), errors, frame_err);
    printf("decode results: ok=%lu frame=%lu continue=%lu key=%lu nobuf=%lu "
           "nobits=%lu reschange=%lu\n",
           results[0], results[1], results[2], results[3], results[4],
           results[5], results[6]);
    printf("cpu: %.3fs = %.1f%% of one core; rss %ld -> %ld KiB; "
           "stream_frames_left=%d\n",
           cpu_seconds() - cpu_start,
           100.0 * (cpu_seconds() - cpu_start) / ((t_end - t_start) / 1e9),
           rss_start, rss_kib(), cedar_stream_frames(dec));
    {
        struct cedar_sync_stats st;

        cedar_sync_stats(&st);
        printf("semwait: %s calls=%lu blocked=%lu timeouts=%lu\n",
               st.vendor ? "vendor" : st.calls ? "interposed"
                                               : "NOT interposed",
               st.calls, st.blocked, st.timeouts);
    }

    if (display) {
        while (nheld > 0)
            cedar_return_picture(dec, held[--nheld].pic);
        usleep(50000);
        dispv_hide(&disp);
        usleep(50000);
        dispv_close(&disp);
    }

out:
    if (dec != NULL)
        cedar_close(dec);
    free(aus);
    free(data);
    if (rc == 0 && (pictures == 0 || errors > 0))
        rc = 1;
    printf("result: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
