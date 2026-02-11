/*
 * Copyright (c) 2012 Stefano Sabatini (blackdetect)
 * Copyright (c) 2025 Pulsar (solidcolordetect)
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
 * Solid color frame detector. Detects frames that are a single uniform color.
 * In black mode (default), behaves like blackdetect.
 * In color mode, detects any solid color frame.
 */

#include <float.h>
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

enum SolidColorMode {
    MODE_BLACK = 0,
    MODE_COLOR = 1,
};

typedef struct SolidColorDetectContext {
    const AVClass *class;
    double  min_duration_time;
    int64_t min_duration;
    int64_t solid_start;
    int64_t solid_end;
    int64_t last_picref_pts;
    int     solid_started;

    double  picture_ratio_th;
    double  pixel_th;
    int     mode;

    unsigned int nb_matching_pixels;
    AVRational   time_base;
    int          depth;

    /* detected color (for logging) */
    unsigned int avg_y;
    unsigned int avg_u;
    unsigned int avg_v;
} SolidColorDetectContext;

#define OFFSET(x) offsetof(SolidColorDetectContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption solidcolordetect_options[] = {
    { "d",              "set minimum detected solid color duration in seconds", OFFSET(min_duration_time), AV_OPT_TYPE_DOUBLE, {.dbl=2}, 0, DBL_MAX, FLAGS },
    { "min_duration",   "set minimum detected solid color duration in seconds", OFFSET(min_duration_time), AV_OPT_TYPE_DOUBLE, {.dbl=2}, 0, DBL_MAX, FLAGS },
    { "picture_ratio_th", "set the picture solid ratio threshold", OFFSET(picture_ratio_th), AV_OPT_TYPE_DOUBLE, {.dbl=.98}, 0, 1, FLAGS },
    { "pic_th",           "set the picture solid ratio threshold", OFFSET(picture_ratio_th), AV_OPT_TYPE_DOUBLE, {.dbl=.98}, 0, 1, FLAGS },
    { "pixel_th",  "set the pixel tolerance threshold", OFFSET(pixel_th), AV_OPT_TYPE_DOUBLE, {.dbl=.10}, 0, 1, FLAGS },
    { "pix_th",    "set the pixel tolerance threshold", OFFSET(pixel_th), AV_OPT_TYPE_DOUBLE, {.dbl=.10}, 0, 1, FLAGS },
    { "mode",      "detection mode: 0=black only, 1=any color", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=MODE_BLACK}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(solidcolordetect);

#define YUVJ_FORMATS \
    AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P

static const enum AVPixelFormat yuvj_formats[] = {
    YUVJ_FORMATS, AV_PIX_FMT_NONE
};

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
    YUVJ_FORMATS,
    AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14,
    AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV440P10,
    AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_YUV444P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV420P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_NONE
};

static int query_format(const AVFilterContext *ctx,
                        AVFilterFormatsConfig **cfg_in,
                        AVFilterFormatsConfig **cfg_out)
{
    AVFilterFormats *formats = ff_make_format_list(pix_fmts);
    return ff_set_common_formats2(ctx, cfg_in, cfg_out, formats);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    SolidColorDetectContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    s->depth = desc->comp[0].depth;
    s->time_base = inlink->time_base;
    s->min_duration = s->min_duration_time / av_q2d(s->time_base);

    av_log(ctx, AV_LOG_VERBOSE,
           "min_duration:%s pixel_th:%f picture_ratio_th:%f mode:%s\n",
           av_ts2timestr(s->min_duration, &s->time_base),
           s->pixel_th, s->picture_ratio_th,
           s->mode == MODE_BLACK ? "black" : "color");
    return 0;
}

static void check_solid_end(AVFilterContext *ctx)
{
    SolidColorDetectContext *s = ctx->priv;

    if ((s->solid_end - s->solid_start) >= s->min_duration) {
        av_log(ctx, AV_LOG_INFO,
               "solid_start:%s solid_end:%s solid_duration:%s "
               "avg_y:%u avg_u:%u avg_v:%u\n",
               av_ts2timestr(s->solid_start, &s->time_base),
               av_ts2timestr(s->solid_end,   &s->time_base),
               av_ts2timestr(s->solid_end - s->solid_start, &s->time_base),
               s->avg_y, s->avg_u, s->avg_v);
    }
}

/**
 * Compute the average value of a plane (8-bit).
 */
static unsigned compute_avg8(const uint8_t *src, ptrdiff_t stride,
                             int width, int height)
{
    uint64_t sum = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            sum += src[x];
        src += stride;
    }
    return (unsigned)(sum / ((uint64_t)width * height));
}

/**
 * Compute the average value of a plane (>8-bit).
 */
static unsigned compute_avg16(const uint8_t *src, ptrdiff_t stride,
                              int width, int height)
{
    uint64_t sum = 0;
    for (int y = 0; y < height; y++) {
        const uint16_t *src16 = (const uint16_t *)src;
        for (int x = 0; x < width; x++)
            sum += src16[x];
        src += stride;
    }
    return (unsigned)(sum / ((uint64_t)width * height));
}

/**
 * Count pixels within tolerance of a reference value (8-bit).
 */
static unsigned count_within8(const uint8_t *src, ptrdiff_t stride,
                              int width, int height,
                              unsigned ref, unsigned tol)
{
    unsigned count = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned v = src[x];
            count += (v >= (ref > tol ? ref - tol : 0)) &&
                     (v <= ref + tol);
        }
        src += stride;
    }
    return count;
}

/**
 * Count pixels within tolerance of a reference value (>8-bit).
 */
static unsigned count_within16(const uint8_t *src, ptrdiff_t stride,
                               int width, int height,
                               unsigned ref, unsigned tol)
{
    unsigned count = 0;
    for (int y = 0; y < height; y++) {
        const uint16_t *src16 = (const uint16_t *)src;
        for (int x = 0; x < width; x++) {
            unsigned v = src16[x];
            count += (v >= (ref > tol ? ref - tol : 0)) &&
                     (v <= ref + tol);
        }
        src += stride;
    }
    return count;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    FilterLink *inl = ff_filter_link(inlink);
    AVFilterContext *ctx = inlink->dst;
    SolidColorDetectContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(picref->format);
    const int max = (1 << s->depth) - 1;
    const int factor = (1 << (s->depth - 8));
    const int full = picref->color_range == AVCOL_RANGE_JPEG ||
                     ff_fmt_is_in(picref->format, yuvj_formats);
    const int w = picref->width;
    const int h = picref->height;
    double picture_ratio = 0;
    unsigned pixel_tol_i;
    unsigned ref_y, nb_matching;

    /* Compute pixel tolerance in integer units */
    pixel_tol_i = full ? s->pixel_th * max :
        16 * factor + s->pixel_th * (235 - 16) * factor;

    if (s->mode == MODE_BLACK) {
        /* Black mode: count pixels below threshold (same as blackdetect) */
        ref_y = 0;
        if (s->depth <= 8)
            nb_matching = count_within8(picref->data[0], picref->linesize[0],
                                        w, h, 0, pixel_tol_i);
        else
            nb_matching = count_within16(picref->data[0], picref->linesize[0],
                                         w, h, 0, pixel_tol_i);
    } else {
        /* Color mode: compute average luma, then count pixels near that average */
        if (s->depth <= 8) {
            ref_y = compute_avg8(picref->data[0], picref->linesize[0], w, h);
            nb_matching = count_within8(picref->data[0], picref->linesize[0],
                                        w, h, ref_y, pixel_tol_i);
        } else {
            ref_y = compute_avg16(picref->data[0], picref->linesize[0], w, h);
            nb_matching = count_within16(picref->data[0], picref->linesize[0],
                                         w, h, ref_y, pixel_tol_i);
        }
    }

    picture_ratio = (double)nb_matching / (w * h);

    /* In color mode, also check chroma planes for uniformity */
    if (s->mode == MODE_COLOR && picture_ratio >= s->picture_ratio_th &&
        desc->nb_components >= 3) {
        int chroma_w = AV_CEIL_RSHIFT(w, desc->log2_chroma_w);
        int chroma_h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
        unsigned ref_u, ref_v, match_u, match_v;
        double ratio_u, ratio_v;

        if (s->depth <= 8) {
            ref_u = compute_avg8(picref->data[1], picref->linesize[1], chroma_w, chroma_h);
            ref_v = compute_avg8(picref->data[2], picref->linesize[2], chroma_w, chroma_h);
            match_u = count_within8(picref->data[1], picref->linesize[1],
                                    chroma_w, chroma_h, ref_u, pixel_tol_i);
            match_v = count_within8(picref->data[2], picref->linesize[2],
                                    chroma_w, chroma_h, ref_v, pixel_tol_i);
        } else {
            ref_u = compute_avg16(picref->data[1], picref->linesize[1], chroma_w, chroma_h);
            ref_v = compute_avg16(picref->data[2], picref->linesize[2], chroma_w, chroma_h);
            match_u = count_within16(picref->data[1], picref->linesize[1],
                                     chroma_w, chroma_h, ref_u, pixel_tol_i);
            match_v = count_within16(picref->data[2], picref->linesize[2],
                                     chroma_w, chroma_h, ref_v, pixel_tol_i);
        }

        ratio_u = (double)match_u / (chroma_w * chroma_h);
        ratio_v = (double)match_v / (chroma_w * chroma_h);

        /* If chroma is not uniform, this is not a solid color frame */
        if (ratio_u < s->picture_ratio_th || ratio_v < s->picture_ratio_th)
            picture_ratio = 0;

        s->avg_y = ref_y;
        s->avg_u = ref_u;
        s->avg_v = ref_v;
    } else if (s->mode == MODE_BLACK) {
        s->avg_y = 0;
        s->avg_u = 128 * factor;
        s->avg_v = 128 * factor;
    }

    av_log(ctx, AV_LOG_DEBUG,
           "frame:%"PRId64" picture_ratio:%f pts:%s t:%s type:%c\n",
           inl->frame_count_out, picture_ratio,
           av_ts2str(picref->pts), av_ts2timestr(picref->pts, &s->time_base),
           av_get_picture_type_char(picref->pict_type));

    if (picture_ratio >= s->picture_ratio_th) {
        if (!s->solid_started) {
            s->solid_started = 1;
            s->solid_start = picref->pts;
            av_dict_set(&picref->metadata, "lavfi.solid_start",
                av_ts2timestr(s->solid_start, &s->time_base), 0);
        }
    } else if (s->solid_started) {
        s->solid_started = 0;
        s->solid_end = picref->pts;
        check_solid_end(ctx);
        av_dict_set(&picref->metadata, "lavfi.solid_end",
            av_ts2timestr(s->solid_end, &s->time_base), 0);
    }

    s->last_picref_pts = picref->pts;
    return ff_filter_frame(inlink->dst->outputs[0], picref);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SolidColorDetectContext *s = ctx->priv;

    if (s->solid_started) {
        s->solid_end = s->last_picref_pts;
        check_solid_end(ctx);
    }
}

static const AVFilterPad solidcolordetect_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input,
        .filter_frame  = filter_frame,
    },
};

const FFFilter ff_vf_solidcolordetect = {
    .p.name        = "solidcolordetect",
    .p.description = NULL_IF_CONFIG_SMALL("Detect video intervals that are a solid color."),
    .p.priv_class  = &solidcolordetect_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .priv_size     = sizeof(SolidColorDetectContext),
    FILTER_INPUTS(solidcolordetect_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_format),
    .uninit        = uninit,
};
