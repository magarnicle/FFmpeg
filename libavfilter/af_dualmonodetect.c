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
 * Dual mono detection filter. Detects when a stereo stream carries
 * identical content on both channels (mono duplicated to stereo).
 *
 * Works by computing the RMS energy of the difference between the
 * left and right channels. If the difference energy is below a
 * threshold relative to the signal energy, the audio is flagged
 * as dual mono.
 *
 * Options:
 *   threshold (th): max diff/signal energy ratio to consider dual mono (default 0.001)
 *   duration (d):   minimum duration in seconds for a dual mono region to be reported (default 2)
 *   ratio (r):      if set >0, report overall dual mono percentage at end and warn if
 *                   the percentage exceeds this value (0-100, default 0 = disabled)
 */

#include <float.h>
#include <math.h>
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"

typedef struct DualMonoDetectContext {
    const AVClass *class;

    double threshold;           ///< max diff/signal ratio to consider dual mono
    int64_t duration;           ///< minimum duration of dual mono to report (microseconds)
    double ratio;               ///< percentage threshold for overall dual mono warning (0=disabled)

    int dual_mono_started;
    int64_t dm_start_pts;       ///< pts of start of dual mono region
    int64_t dm_end_pts;         ///< pts of end of dual mono region
    int64_t last_pts;
    int64_t total_dm_samples;   ///< total samples in dual mono regions
    int64_t total_samples;      ///< total samples processed
    int sample_rate;
    AVRational time_base;
} DualMonoDetectContext;

#define OFFSET(x) offsetof(DualMonoDetectContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption dualmonodetect_options[] = {
    { "threshold", "set difference threshold ratio", OFFSET(threshold),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.001}, 0, 1, FLAGS },
    { "th", "set difference threshold ratio", OFFSET(threshold),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0.001}, 0, 1, FLAGS },
    { "d", "set minimum duration in seconds", OFFSET(duration),
      AV_OPT_TYPE_DURATION, {.i64 = 2000000}, 0, INT64_MAX, FLAGS },
    { "duration", "set minimum duration in seconds", OFFSET(duration),
      AV_OPT_TYPE_DURATION, {.i64 = 2000000}, 0, INT64_MAX, FLAGS },
    { "ratio", "set percentage threshold for overall dual mono warning (0=disabled)", OFFSET(ratio),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, 100, FLAGS },
    { "r", "set percentage threshold for overall dual mono warning (0=disabled)", OFFSET(ratio),
      AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, 100, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dualmonodetect);

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DualMonoDetectContext *s = ctx->priv;

    s->sample_rate = inlink->sample_rate;
    s->time_base = inlink->time_base;

    if (inlink->ch_layout.nb_channels != 2) {
        av_log(ctx, AV_LOG_ERROR, "dualmonodetect requires stereo input (got %d channels)\n",
               inlink->ch_layout.nb_channels);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "threshold:%f duration:%s sample_rate:%d ratio:%.1f%%\n",
           s->threshold,
           av_ts2timestr(s->duration, &AV_TIME_BASE_Q),
           s->sample_rate, s->ratio);
    return 0;
}

static void report_dual_mono(AVFilterContext *ctx)
{
    DualMonoDetectContext *s = ctx->priv;
    int64_t dm_duration = av_rescale_q(s->dm_end_pts - s->dm_start_pts,
                                       s->time_base, AV_TIME_BASE_Q);

    if (dm_duration >= s->duration) {
        av_log(ctx, AV_LOG_INFO,
               "dual_mono_start:%s dual_mono_end:%s dual_mono_duration:%s\n",
               av_ts2timestr(s->dm_start_pts, &s->time_base),
               av_ts2timestr(s->dm_end_pts, &s->time_base),
               av_ts2timestr(s->dm_end_pts - s->dm_start_pts, &s->time_base));
    }
}

/**
 * Compute diff/signal energy ratio for a frame. Returns 1 if dual mono.
 * Handles all supported sample formats via a macro to avoid duplication.
 */
#define COMPUTE_DUAL_MONO_PLANAR(type, cast)                                \
    do {                                                                    \
        const type *left  = (const type *)frame->extended_data[0];          \
        const type *right = (const type *)frame->extended_data[1];          \
        double diff_energy = 0, signal_energy = 0;                         \
        for (int i = 0; i < nb_samples; i++) {                             \
            double l = (cast)left[i], r = (cast)right[i];                  \
            double d = l - r;                                              \
            diff_energy += d * d;                                          \
            signal_energy += l * l + r * r;                                \
        }                                                                  \
        is_dual_mono = signal_energy > 0                                   \
            ? (diff_energy / signal_energy) < s->threshold                 \
            : 1;                                                           \
    } while (0)

#define COMPUTE_DUAL_MONO_INTERLEAVED(type, cast)                           \
    do {                                                                    \
        const type *data = (const type *)frame->data[0];                    \
        double diff_energy = 0, signal_energy = 0;                         \
        for (int i = 0; i < nb_samples; i++) {                             \
            double l = (cast)data[i * 2], r = (cast)data[i * 2 + 1];      \
            double d = l - r;                                              \
            diff_energy += d * d;                                          \
            signal_energy += l * l + r * r;                                \
        }                                                                  \
        is_dual_mono = signal_energy > 0                                   \
            ? (diff_energy / signal_energy) < s->threshold                 \
            : 1;                                                           \
    } while (0)

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    DualMonoDetectContext *s = ctx->priv;
    int nb_samples = frame->nb_samples;
    int is_dual_mono = 0;

    switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP:  COMPUTE_DUAL_MONO_PLANAR(float, double);       break;
    case AV_SAMPLE_FMT_FLT:   COMPUTE_DUAL_MONO_INTERLEAVED(float, double);  break;
    case AV_SAMPLE_FMT_DBLP:  COMPUTE_DUAL_MONO_PLANAR(double, double);      break;
    case AV_SAMPLE_FMT_DBL:   COMPUTE_DUAL_MONO_INTERLEAVED(double, double); break;
    case AV_SAMPLE_FMT_S16P:  COMPUTE_DUAL_MONO_PLANAR(int16_t, double);     break;
    case AV_SAMPLE_FMT_S16:   COMPUTE_DUAL_MONO_INTERLEAVED(int16_t, double);break;
    case AV_SAMPLE_FMT_S32P:  COMPUTE_DUAL_MONO_PLANAR(int32_t, double);     break;
    case AV_SAMPLE_FMT_S32:   COMPUTE_DUAL_MONO_INTERLEAVED(int32_t, double);break;
    }

    s->total_samples += nb_samples;
    if (is_dual_mono)
        s->total_dm_samples += nb_samples;

    if (is_dual_mono) {
        if (!s->dual_mono_started) {
            s->dual_mono_started = 1;
            s->dm_start_pts = frame->pts;
        }
        s->dm_end_pts = frame->pts +
            av_rescale_q(nb_samples, (AVRational){1, s->sample_rate}, s->time_base);
    } else if (s->dual_mono_started) {
        s->dual_mono_started = 0;
        report_dual_mono(ctx);
    }

    s->last_pts = frame->pts +
        av_rescale_q(nb_samples, (AVRational){1, s->sample_rate}, s->time_base);
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DualMonoDetectContext *s = ctx->priv;

    /* Flush any in-progress region */
    if (s->dual_mono_started) {
        s->dm_end_pts = s->last_pts;
        report_dual_mono(ctx);
    }

    /* Report overall dual mono percentage */
    if (s->total_samples > 0) {
        double pct = 100.0 * s->total_dm_samples / s->total_samples;
        av_log(ctx, AV_LOG_INFO,
               "dual_mono_total_samples:%"PRId64" total_samples:%"PRId64" dual_mono_percent:%.2f%%\n",
               s->total_dm_samples, s->total_samples, pct);

        if (s->ratio > 0 && pct >= s->ratio) {
            av_log(ctx, AV_LOG_WARNING,
                   "dual mono percentage %.2f%% exceeds threshold %.1f%%\n",
                   pct, s->ratio);
        }
    }
}

static const AVFilterPad dualmonodetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_af_dualmonodetect = {
    .p.name        = "dualmonodetect",
    .p.description = NULL_IF_CONFIG_SMALL("Detect dual mono (identical stereo channels)."),
    .p.priv_class  = &dualmonodetect_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .priv_size     = sizeof(DualMonoDetectContext),
    .uninit        = uninit,
    FILTER_INPUTS(dualmonodetect_inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
                      AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
                      AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P,
                      AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P),
};
