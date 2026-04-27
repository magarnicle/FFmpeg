/*
 * Teletext VBI Reading Filter
 * Copyright (c) 2024 FFmpeg
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

/**
 * @file
 * Filter for reading teletext data from VBI lines.
 *
 * Uses libzvbi for bit slicing to extract World System Teletext (WST)
 * data from SD video VBI lines.
 */

#include "config.h"

#if CONFIG_LIBZVBI
#include <libzvbi.h>
#endif

#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/intreadwrite.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

/* Teletext data unit size */
#define TELETEXT_DATA_UNIT_SIZE 46
#define TELETEXT_LINE_SIZE 42

/* Maximum lines to scan */
#define MAX_SCAN_LINES 32

/* Hamming 8/4 decode table */
static const uint8_t ham84_decode[256] = {
    0x01, 0xFF, 0x01, 0x01, 0xFF, 0x00, 0x01, 0xFF,
    0xFF, 0x02, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x00, 0x01, 0xFF, 0x00, 0x00, 0xFF, 0x00,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0x06, 0xFF, 0xFF, 0x07, 0xFF, 0x07, 0x07, 0x07,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x0D, 0xFF,
    0x06, 0x06, 0x06, 0xFF, 0x06, 0xFF, 0xFF, 0x07,
    0xFF, 0x02, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x02, 0x02, 0xFF, 0x02, 0xFF, 0x02, 0x03, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x02, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0x03,
    0x04, 0xFF, 0xFF, 0x05, 0x04, 0x04, 0x04, 0xFF,
    0xFF, 0x02, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0xFF, 0x05, 0x05, 0x05, 0x04, 0xFF, 0xFF, 0x05,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x0A, 0xFF, 0xFF, 0x0B, 0x0A, 0x0A, 0x0A, 0xFF,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x0D, 0xFF,
    0xFF, 0x0B, 0x0B, 0x0B, 0x0A, 0xFF, 0xFF, 0x0B,
    0x0C, 0x0C, 0xFF, 0x0C, 0xFF, 0x0C, 0x0D, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x0C, 0x0D, 0xFF, 0x0D, 0xFF, 0x0D, 0x0D,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x0D, 0xFF,
    0x08, 0xFF, 0xFF, 0x09, 0xFF, 0x09, 0x09, 0x09,
    0xFF, 0x02, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x08, 0x08, 0x08, 0xFF, 0x08, 0xFF, 0xFF, 0x09,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x0F, 0xFF, 0x0F, 0x0F, 0xFF, 0x0E, 0x0F, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x0D, 0xFF,
    0xFF, 0x0E, 0x0F, 0xFF, 0x0E, 0x0E, 0xFF, 0x0E,
};

typedef struct ReadTeletextContext {
    const AVClass *class;

    int scan_min;        /* First line to scan (1-based) */
    int scan_max;        /* Last line to scan (1-based) */
    int scan_field2;     /* Also scan field 2 */
    int system;          /* 0=auto, 1=PAL, 2=NTSC */
    float threshold;     /* Detection threshold */

    /* Statistics */
    int64_t frames_processed;
    int64_t lines_detected;

#if CONFIG_LIBZVBI
    vbi_bit_slicer slicer;
    int slicer_initialized;
#endif
} ReadTeletextContext;

#if CONFIG_LIBZVBI
/**
 * Extract luma samples from UYVY line
 */
static void extract_luma_uyvy(const uint8_t *src, uint8_t *luma, int width)
{
    for (int i = 0; i < width; i++) {
        luma[i] = src[i * 2 + 1];
    }
}

/**
 * Extract luma samples from planar YUV
 */
static void extract_luma_planar(const uint8_t *src, uint8_t *luma, int width)
{
    memcpy(luma, src, width);
}

/**
 * Initialize the bit slicer for teletext System B
 */
static void init_slicer(ReadTeletextContext *s)
{
    if (s->slicer_initialized)
        return;

    /* Parameters for teletext System B (PAL):
     * - Line width: 720 samples
     * - Sample rate: 13.5 MHz
     * - Bit rate: 6.9375 MHz
     * - Sync pattern: 0x00aaaae4 (clock run-in + framing code)
     * - Payload offset: 18 samples from sync start
     * - CRI offset: 6 samples
     * - Payload bits: 42 * 8 = 336
     */
    vbi_bit_slicer_init(&s->slicer, 720, 13500000, 6937500, 6937500,
                        0x00aaaae4, 0xffff, 18, 6, 42 * 8,
                        VBI_MODULATION_NRZ_MSB, VBI_PIXFMT_YUV420);
    s->slicer_initialized = 1;
}

/**
 * Try to decode teletext from a VBI line
 * Returns 1 if teletext was found, 0 otherwise
 */
static int decode_vbi_line(ReadTeletextContext *s, const uint8_t *luma,
                           int width, uint8_t *output)
{
    uint8_t line_buf[720];

    if (width > 720)
        width = 720;

    /* Copy luma and pad if needed */
    memcpy(line_buf, luma, width);
    if (width < 720)
        memset(line_buf + width, 16, 720 - width);  /* Pad with black */

    init_slicer(s);

    return vbi_bit_slice(&s->slicer, line_buf, output) == TRUE;
}
#endif /* CONFIG_LIBZVBI */

/**
 * Decode and log teletext content from a 42-byte teletext line
 */
static void log_teletext_line(AVFilterContext *ctx, int line_num, int field,
                               const uint8_t *data, int64_t pts)
{
    /* Data format: 42 bytes
     * [0-1]: MRAG (Magazine/Row Address Group) with Hamming 8/4
     * [2-41]: 40 character bytes with odd parity
     */
    uint8_t mrag0 = ham84_decode[data[0]];
    uint8_t mrag1 = ham84_decode[data[1]];

    if (mrag0 == 0xFF || mrag1 == 0xFF) {
        av_log(ctx, AV_LOG_DEBUG, "Teletext line %d field %d: Hamming error\n",
               line_num, field);
        return;
    }

    int magazine = mrag0 & 0x07;
    int row = ((mrag0 >> 3) & 1) | (mrag1 << 1);

    if (magazine == 0)
        magazine = 8;

    /* Extract text content (strip parity) */
    char text[41];
    for (int i = 0; i < 40; i++) {
        uint8_t c = data[2 + i] & 0x7F;
        if (c < 0x20 || c > 0x7E)
            c = ' ';
        text[i] = c;
    }
    text[40] = '\0';

    /* Trim trailing spaces */
    int len = 40;
    while (len > 0 && text[len - 1] == ' ')
        text[--len] = '\0';

    if (len > 0 || row == 0) {
        av_log(ctx, AV_LOG_INFO, "TTX M%d/%02d line %d f%d: \"%s\"%s\n",
               magazine, row, line_num, field, text,
               row == 0 ? " [header]" : "");
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ReadTeletextContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int is_pal, field2_start;
    int found_count = 0;

#if !CONFIG_LIBZVBI
    av_log(ctx, AV_LOG_ERROR, "libzvbi is required for teletext VBI decoding\n");
    return ff_filter_frame(outlink, frame);
#else

    s->frames_processed++;

    /* Determine system */
    if (s->system == 1 || (s->system == 0 && frame->height >= 576))
        is_pal = 1;
    else
        is_pal = 0;

    field2_start = is_pal ? 313 : 263;

    /* Clamp scan range */
    int scan_min = FFMAX(1, s->scan_min);
    int scan_max = FFMIN(s->scan_max, is_pal ? 50 : 40);

    /* Scan field 1 VBI lines */
    for (int line = scan_min; line <= scan_max && line <= frame->height; line++) {
        uint8_t luma[720];
        uint8_t ttx_data[42];
        int frame_line = line - 1;  /* Convert to 0-based */

        /* Extract luma based on pixel format */
        const uint8_t *src;
        if (frame->format == AV_PIX_FMT_UYVY422) {
            src = frame->data[0] + frame_line * frame->linesize[0];
            extract_luma_uyvy(src, luma, FFMIN(frame->width, 720));
        } else if (desc->flags & AV_PIX_FMT_FLAG_PLANAR) {
            src = frame->data[0] + frame_line * frame->linesize[0];
            extract_luma_planar(src, luma, FFMIN(frame->width, 720));
        } else {
            continue;  /* Unsupported format */
        }

        if (decode_vbi_line(s, luma, frame->width, ttx_data)) {
            log_teletext_line(ctx, line, 1, ttx_data, frame->pts);
            s->lines_detected++;
            found_count++;
        }
    }

    /* Scan field 2 VBI lines if enabled */
    if (s->scan_field2) {
        for (int line = scan_min; line <= scan_max; line++) {
            int frame_line = field2_start + line - 1;
            if (frame_line >= frame->height)
                break;

            uint8_t luma[720];
            uint8_t ttx_data[42];

            const uint8_t *src;
            if (frame->format == AV_PIX_FMT_UYVY422) {
                src = frame->data[0] + frame_line * frame->linesize[0];
                extract_luma_uyvy(src, luma, FFMIN(frame->width, 720));
            } else if (desc->flags & AV_PIX_FMT_FLAG_PLANAR) {
                src = frame->data[0] + frame_line * frame->linesize[0];
                extract_luma_planar(src, luma, FFMIN(frame->width, 720));
            } else {
                continue;
            }

            if (decode_vbi_line(s, luma, frame->width, ttx_data)) {
                log_teletext_line(ctx, line, 2, ttx_data, frame->pts);
                s->lines_detected++;
                found_count++;
            }
        }
    }

    if (found_count > 0) {
        av_log(ctx, AV_LOG_DEBUG, "Frame %"PRId64": found %d teletext lines\n",
               s->frames_processed, found_count);
    }

    return ff_filter_frame(outlink, frame);
#endif /* CONFIG_LIBZVBI */
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ReadTeletextContext *s = ctx->priv;

    av_log(ctx, AV_LOG_INFO, "Processed %"PRId64" frames, detected %"PRId64" teletext lines\n",
           s->frames_processed, s->lines_detected);
}

#define OFFSET(x) offsetof(ReadTeletextContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption readteletext_options[] = {
    { "scan_min", "first line to scan (1-based)", OFFSET(scan_min), AV_OPT_TYPE_INT, {.i64 = 6}, 1, 50, FLAGS },
    { "scan_max", "last line to scan (1-based)", OFFSET(scan_max), AV_OPT_TYPE_INT, {.i64 = 22}, 1, 50, FLAGS },
    { "field2", "also scan field 2", OFFSET(scan_field2), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, FLAGS },
    { "system", "TV system (0=auto, 1=PAL, 2=NTSC)", OFFSET(system), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 2, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(readteletext);

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_NONE
};

static const AVFilterPad readteletext_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_readteletext = {
    .p.name        = "readteletext",
    .p.description = NULL_IF_CONFIG_SMALL("Read teletext data from VBI lines"),
    .p.priv_class  = &readteletext_class,
    .priv_size     = sizeof(ReadTeletextContext),
    .uninit        = uninit,
    FILTER_INPUTS(readteletext_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
};
