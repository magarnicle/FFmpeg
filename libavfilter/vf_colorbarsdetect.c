/*
 * Copyright (c) 2026 FFmpeg contributors
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
 * Color bar detection filter. Detects SMPTE, SMPTE HD, EBU 75%, and
 * EBU 100% color bar patterns in video frames.
 *
 * Detection works by sampling pixel colors along rows and matching
 * against known reference bar patterns. Different bar formats have
 * different spatial layouts (EBU uses full width with 8 equal bars,
 * SMPTE uses side panels with 7 bars in the center), so the detector
 * tries each layout independently.
 */

#include <float.h>
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

/**
 * Reference color bar YUV values [Y, Cb, Cr] in 8-bit studio range.
 * These match the values used by ffmpeg's test source filters.
 */

/* SMPTE SD bars (75% amplitude, same as rainbow[] in vsrc_testsrc.c) */
static const uint8_t smpte_bars[7][3] = {
    { 180, 128, 128 },  /* 75% white */
    { 162,  44, 142 },  /* 75% yellow */
    { 131, 156,  44 },  /* 75% cyan */
    { 112,  72,  58 },  /* 75% green */
    {  84, 184, 198 },  /* 75% magenta */
    {  65, 100, 212 },  /* 75% red */
    {  35, 212, 114 },  /* 75% blue */
};

/* SMPTE HD bars (rainbowhd[] in vsrc_testsrc.c) */
static const uint8_t smptehd_bars[7][3] = {
    { 180, 128, 128 },  /* 75% white */
    { 168,  44, 136 },  /* 75% yellow */
    { 145, 147,  44 },  /* 75% cyan */
    { 133,  63,  52 },  /* 75% green */
    {  63, 193, 204 },  /* 75% magenta */
    {  51, 109, 212 },  /* 75% red */
    {  28, 212, 120 },  /* 75% blue */
};

/* EBU/PAL 100% bars (rainbow100[] in vsrc_testsrc.c) */
static const uint8_t ebu100_bars[7][3] = {
    { 235, 128, 128 },  /* 100% white */
    { 210,  16, 146 },  /* 100% yellow */
    { 170, 166,  16 },  /* 100% cyan */
    { 145,  54,  34 },  /* 100% green */
    { 106, 202, 222 },  /* 100% magenta */
    {  81,  90, 240 },  /* 100% red */
    {  41, 240, 110 },  /* 100% blue */
};

enum BarType {
    BAR_SMPTE,
    BAR_SMPTEHD,
    BAR_EBU75,      /* uses same colors as smpte_bars */
    BAR_EBU100,
    BAR_TYPE_COUNT
};

static const char *bar_type_names[] = {
    "smpte",
    "smptehd",
    "ebu75",
    "ebu100",
};

/**
 * Bar layout descriptor. Defines where the 7 color bars sit
 * within the frame width, expressed as fractions of the total width.
 */
typedef struct BarLayout {
    double bar_start;   /**< fraction of width where first bar starts */
    double bar_end;     /**< fraction of width where last bar ends */
    int nbars;          /**< number of color bars */
    const uint8_t (*colors)[3]; /**< reference colors */
} BarLayout;

/*
 * EBU layout: 8 equal-width bars spanning full width.
 * Bars 0-6 are colors, bar 7 is black. We only check bars 0-6.
 * Each bar is 1/8 of width, so bars span 0 to 7/8.
 */
static const BarLayout ebu75_layout  = { 0.0, 7.0/8.0, 7, smpte_bars };
static const BarLayout ebu100_layout = { 0.0, 7.0/8.0, 7, ebu100_bars };

/*
 * SMPTE HD layout: gray40 panel on left (1/8 width), then 7 bars
 * occupying 3/4 of total width, then gray40 on right.
 * bar_start = 1/8 = 0.125, bar_end = 1/8 + 3/4 = 0.875
 */
static const BarLayout smptehd_layout = { 1.0/8.0, 7.0/8.0, 7, smptehd_bars };

/*
 * SMPTE SD layout: similar to HD.
 * From vsrc_testsrc.c: d_w = w/8, r_w = ((w+3)/4)*3/7 for each bar
 * 7 bars of width r_w starting at d_w.
 * Total bars width = 7 * r_w ≈ 3*w/4, starting at w/8.
 */
static const BarLayout smpte_layout = { 1.0/8.0, 7.0/8.0, 7, smpte_bars };

typedef struct ColorBarsDetectContext {
    const AVClass *class;

    double threshold;           ///< YUV distance threshold for matching (0-1 normalized)
    double min_ratio;           ///< minimum fraction of bars that must match per row
    double min_row_ratio;       ///< minimum fraction of sampled rows that must match
    double min_duration_time;   ///< minimum duration to report, in seconds
    int64_t min_duration;       ///< minimum duration in timebase units

    int bars_started;
    int64_t bars_start;         ///< pts of first bars frame
    int64_t bars_end;           ///< pts of last bars frame
    int64_t last_pts;
    int detected_type;          ///< which bar pattern was detected
    AVRational time_base;
} ColorBarsDetectContext;

#define OFFSET(x) offsetof(ColorBarsDetectContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption colorbarsdetect_options[] = {
    { "threshold", "set color distance threshold (0-1)", OFFSET(threshold),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.15}, 0, 1, FLAGS },
    { "th", "set color distance threshold (0-1)", OFFSET(threshold),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.15}, 0, 1, FLAGS },
    { "min_ratio", "set minimum bar match ratio per row", OFFSET(min_ratio),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.7}, 0, 1, FLAGS },
    { "min_row_ratio", "set minimum fraction of rows matching bars", OFFSET(min_row_ratio),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0, 1, FLAGS },
    { "d", "set minimum detected duration in seconds", OFFSET(min_duration_time),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0, DBL_MAX, FLAGS },
    { "min_duration", "set minimum detected duration in seconds", OFFSET(min_duration_time),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.5}, 0, DBL_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(colorbarsdetect);

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_NONE
};

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ColorBarsDetectContext *s = ctx->priv;

    s->time_base = inlink->time_base;
    s->min_duration = s->min_duration_time / av_q2d(s->time_base);

    av_log(ctx, AV_LOG_VERBOSE,
           "threshold:%f min_ratio:%f min_row_ratio:%f min_duration:%s\n",
           s->threshold, s->min_ratio, s->min_row_ratio,
           av_ts2timestr(s->min_duration, &s->time_base));
    return 0;
}

/**
 * Check how many bars in a given layout match at a specific row.
 * Samples the center pixel of each expected bar region and compares
 * against the reference color.
 *
 * @param[out] total_dist  if non-NULL, accumulated distance for matched bars
 * @return number of matched bars
 */
static int check_row_bars(const AVFrame *frame,
                          int row, int chroma_hsub, int chroma_vsub,
                          const BarLayout *layout, double threshold,
                          double *total_dist)
{
    int width = frame->width;
    int crow = row >> chroma_vsub;
    int matched = 0;
    double th_sq = threshold * threshold;
    double dist_sum = 0;
    int bar_x_start = (int)(layout->bar_start * width);
    int bar_x_span  = (int)((layout->bar_end - layout->bar_start) * width);

    for (int i = 0; i < layout->nbars; i++) {
        /* Sample the center of each bar region */
        int x = bar_x_start + (int)((i + 0.5) * bar_x_span / layout->nbars);
        if (x >= width)
            x = width - 1;

        int y_val = frame->data[0][row * frame->linesize[0] + x];
        int cx = x >> chroma_hsub;
        int u_val = frame->data[1][crow * frame->linesize[1] + cx];
        int v_val = frame->data[2][crow * frame->linesize[2] + cx];

        double dy = (y_val - layout->colors[i][0]) / 255.0;
        double du = (u_val - layout->colors[i][1]) / 255.0;
        double dv = (v_val - layout->colors[i][2]) / 255.0;
        double dist = (dy * dy + du * du + dv * dv) / 3.0;

        if (dist < th_sq) {
            matched++;
            dist_sum += dist;
        }
    }

    if (total_dist)
        *total_dist += dist_sum;

    return matched;
}

/*
 * Order matters: SMPTE HD must be checked before SMPTE SD because they
 * share the same layout but have different color values. EBU 100% must
 * be checked before EBU 75% because 75% uses the same colors as SMPTE SD.
 */
static const BarLayout *bar_layouts[] = {
    [BAR_SMPTE]   = &smpte_layout,
    [BAR_SMPTEHD] = &smptehd_layout,
    [BAR_EBU75]   = &ebu75_layout,
    [BAR_EBU100]  = &ebu100_layout,
};

/**
 * Detect whether a frame contains color bars.
 * Returns the detected bar type index or -1 if no bars found.
 */
static int detect_bars(AVFilterContext *ctx, AVFrame *frame)
{
    ColorBarsDetectContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int chroma_hsub = desc->log2_chroma_w;
    int chroma_vsub = desc->log2_chroma_h;
    int height = frame->height;

    /* Sample rows in the upper 55% of the frame. This covers the color bar
     * region for all pattern types (SMPTE bars are in upper 7/12 ≈ 58%). */
    int sample_height = height * 55 / 100;
    int row_step = FFMAX(sample_height / 20, 1);
    int best_type = -1;
    double best_score = DBL_MAX;

    for (int type = 0; type < BAR_TYPE_COUNT; type++) {
        const BarLayout *layout = bar_layouts[type];
        int matching_rows = 0;
        int checked_rows = 0;
        double total_dist = 0;

        for (int row = row_step; row < sample_height; row += row_step) {
            int matched = check_row_bars(frame, row,
                                         chroma_hsub, chroma_vsub,
                                         layout, s->threshold,
                                         &total_dist);
            checked_rows++;
            if (matched >= (int)(layout->nbars * s->min_ratio))
                matching_rows++;
        }

        if (checked_rows > 0) {
            double row_ratio = (double)matching_rows / checked_rows;
            /* Pick the pattern with sufficient row matches and lowest total
             * color distance (best_score = lowest = closest match). */
            if (row_ratio >= s->min_row_ratio && total_dist < best_score) {
                best_score = total_dist;
                best_type = type;
            }
        }
    }

    if (best_type >= 0) {
        av_log(ctx, AV_LOG_DEBUG, "frame bars match: type=%s\n",
               bar_type_names[best_type]);
    }

    return best_type;
}

static void check_bars_end(AVFilterContext *ctx)
{
    ColorBarsDetectContext *s = ctx->priv;

    if ((s->bars_end - s->bars_start) >= s->min_duration) {
        av_log(ctx, AV_LOG_INFO,
               "colorbars_type:%s colorbars_start:%s colorbars_end:%s colorbars_duration:%s\n",
               bar_type_names[s->detected_type],
               av_ts2timestr(s->bars_start, &s->time_base),
               av_ts2timestr(s->bars_end, &s->time_base),
               av_ts2timestr(s->bars_end - s->bars_start, &s->time_base));
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ColorBarsDetectContext *s = ctx->priv;

    int type = detect_bars(ctx, frame);

    if (type >= 0) {
        if (!s->bars_started) {
            s->bars_started = 1;
            s->bars_start = frame->pts;
            s->detected_type = type;
            av_dict_set(&frame->metadata, "lavfi.colorbars_start",
                        av_ts2timestr(s->bars_start, &s->time_base), 0);
            av_dict_set(&frame->metadata, "lavfi.colorbars_type",
                        bar_type_names[type], 0);
        }
        s->bars_end = frame->pts;
    } else if (s->bars_started) {
        s->bars_started = 0;
        check_bars_end(ctx);
        av_dict_set(&frame->metadata, "lavfi.colorbars_end",
                    av_ts2timestr(s->bars_end, &s->time_base), 0);
    }

    s->last_pts = frame->pts;
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ColorBarsDetectContext *s = ctx->priv;

    if (s->bars_started) {
        s->bars_end = s->last_pts;
        check_bars_end(ctx);
    }
}

static const AVFilterPad colorbarsdetect_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input,
        .filter_frame  = filter_frame,
    },
};

const FFFilter ff_vf_colorbarsdetect = {
    .p.name        = "colorbarsdetect",
    .p.description = NULL_IF_CONFIG_SMALL("Detect SMPTE/EBU color bar patterns."),
    .p.priv_class  = &colorbarsdetect_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .priv_size     = sizeof(ColorBarsDetectContext),
    FILTER_INPUTS(colorbarsdetect_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .uninit        = uninit,
};
