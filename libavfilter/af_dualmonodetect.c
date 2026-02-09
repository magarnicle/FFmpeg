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
    double min_duration_time;   ///< minimum duration in seconds to report
    int64_t min_duration;       ///< minimum duration in samples

    int dual_mono_started;
    int64_t dm_start_pts;       ///< pts of start of dual mono region
    int64_t dm_end_pts;         ///< pts of end of dual mono region
    int64_t last_pts;
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
    { "d", "set minimum duration in seconds", OFFSET(min_duration_time),
      AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0, DBL_MAX, FLAGS },
    { "duration", "set minimum duration in seconds", OFFSET(min_duration_time),
      AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0, DBL_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dualmonodetect);

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DualMonoDetectContext *s = ctx->priv;

    s->sample_rate = inlink->sample_rate;
    s->time_base = inlink->time_base;
    s->min_duration = s->min_duration_time * s->sample_rate;

    if (inlink->ch_layout.nb_channels != 2) {
        av_log(ctx, AV_LOG_ERROR, "dualmonodetect requires stereo input (got %d channels)\n",
               inlink->ch_layout.nb_channels);
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_VERBOSE,
           "threshold:%f min_duration:%.3fs sample_rate:%d\n",
           s->threshold, s->min_duration_time, s->sample_rate);
    return 0;
}

static void report_dual_mono(AVFilterContext *ctx)
{
    DualMonoDetectContext *s = ctx->priv;

    if (s->dm_end_pts > s->dm_start_pts) {
        av_log(ctx, AV_LOG_INFO,
               "dual_mono_start:%s dual_mono_end:%s dual_mono_duration:%s\n",
               av_ts2timestr(s->dm_start_pts, &s->time_base),
               av_ts2timestr(s->dm_end_pts, &s->time_base),
               av_ts2timestr(s->dm_end_pts - s->dm_start_pts, &s->time_base));
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    DualMonoDetectContext *s = ctx->priv;
    int nb_samples = frame->nb_samples;
    int is_dual_mono = 0;

    if (frame->format == AV_SAMPLE_FMT_FLTP) {
        const float *left  = (const float *)frame->extended_data[0];
        const float *right = (const float *)frame->extended_data[1];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double d = left[i] - right[i];
            diff_energy += d * d;
            signal_energy += left[i] * (double)left[i] + right[i] * (double)right[i];
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        /* Also flag as dual mono if both channels are silent */
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_FLT) {
        const float *data = (const float *)frame->data[0];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            float left  = data[i * 2];
            float right = data[i * 2 + 1];
            double d = left - right;
            diff_energy += d * d;
            signal_energy += left * (double)left + right * (double)right;
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_DBLP) {
        const double *left  = (const double *)frame->extended_data[0];
        const double *right = (const double *)frame->extended_data[1];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double d = left[i] - right[i];
            diff_energy += d * d;
            signal_energy += left[i] * left[i] + right[i] * right[i];
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_DBL) {
        const double *data = (const double *)frame->data[0];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double left  = data[i * 2];
            double right = data[i * 2 + 1];
            double d = left - right;
            diff_energy += d * d;
            signal_energy += left * left + right * right;
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_S16P) {
        const int16_t *left  = (const int16_t *)frame->extended_data[0];
        const int16_t *right = (const int16_t *)frame->extended_data[1];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double d = left[i] - right[i];
            diff_energy += d * d;
            signal_energy += (double)left[i] * left[i] + (double)right[i] * right[i];
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_S16) {
        const int16_t *data = (const int16_t *)frame->data[0];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double left  = data[i * 2];
            double right = data[i * 2 + 1];
            double d = left - right;
            diff_energy += d * d;
            signal_energy += left * left + right * right;
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_S32P) {
        const int32_t *left  = (const int32_t *)frame->extended_data[0];
        const int32_t *right = (const int32_t *)frame->extended_data[1];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double d = (double)left[i] - right[i];
            diff_energy += d * d;
            signal_energy += (double)left[i] * left[i] + (double)right[i] * right[i];
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    } else if (frame->format == AV_SAMPLE_FMT_S32) {
        const int32_t *data = (const int32_t *)frame->data[0];
        double diff_energy = 0, signal_energy = 0;

        for (int i = 0; i < nb_samples; i++) {
            double left  = data[i * 2];
            double right = data[i * 2 + 1];
            double d = left - right;
            diff_energy += d * d;
            signal_energy += left * left + right * right;
        }

        double ratio = (signal_energy > 0) ? diff_energy / signal_energy : 0;
        is_dual_mono = (ratio < s->threshold);
        if (signal_energy == 0)
            is_dual_mono = 1;
    }

    if (is_dual_mono) {
        if (!s->dual_mono_started) {
            s->dual_mono_started = 1;
            s->dm_start_pts = frame->pts;
            av_dict_set(&frame->metadata, "lavfi.dual_mono", "1", 0);
            av_dict_set(&frame->metadata, "lavfi.dual_mono_start",
                        av_ts2timestr(frame->pts, &s->time_base), 0);
        }
        s->dm_end_pts = frame->pts +
            av_rescale_q(nb_samples, (AVRational){1, s->sample_rate}, s->time_base);
    } else if (s->dual_mono_started) {
        s->dual_mono_started = 0;
        report_dual_mono(ctx);
        av_dict_set(&frame->metadata, "lavfi.dual_mono", "0", 0);
        av_dict_set(&frame->metadata, "lavfi.dual_mono_end",
                    av_ts2timestr(s->dm_end_pts, &s->time_base), 0);
    }

    s->last_pts = frame->pts +
        av_rescale_q(nb_samples, (AVRational){1, s->sample_rate}, s->time_base);
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DualMonoDetectContext *s = ctx->priv;

    if (s->dual_mono_started) {
        s->dm_end_pts = s->last_pts;
        report_dual_mono(ctx);
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
