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
 * Audio sustained clipping detection filter.
 *
 * Detects runs of consecutive samples at or above full-scale (clipping)
 * that exceed a configurable minimum duration. Reports each clipping
 * event with start time, duration, and channel.
 *
 * This is useful for broadcast QC where clipping lasting more than N
 * consecutive samples (e.g. 1000) should be flagged.
 */

#include <float.h>
#include <math.h>
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"

typedef struct ChannelClipState {
    int64_t clip_run;           ///< current consecutive clipped sample count
    int64_t clip_start_sample;  ///< absolute sample position where current clip run started
    int64_t total_clips;        ///< total number of clipping events reported
    int64_t total_clipped;      ///< total number of clipped samples
} ChannelClipState;

typedef struct ClipDetectContext {
    const AVClass *class;

    int64_t min_duration;       ///< minimum consecutive clipped samples to report
    double level;               ///< clipping level threshold (0-1, default 1.0)
    int nb_channels;
    int sample_rate;
    ChannelClipState *ch;
    int64_t total_samples;      ///< total samples processed per channel
    AVRational time_base;
} ClipDetectContext;

#define OFFSET(x) offsetof(ClipDetectContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM

static const AVOption clipdetect_options[] = {
    { "n", "set minimum consecutive clipped samples", OFFSET(min_duration),
      AV_OPT_TYPE_INT64, {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "min_duration", "set minimum consecutive clipped samples", OFFSET(min_duration),
      AV_OPT_TYPE_INT64, {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "level", "set clipping level threshold (0-1)", OFFSET(level),
      AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(clipdetect);

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ClipDetectContext *s = ctx->priv;

    s->nb_channels = inlink->ch_layout.nb_channels;
    s->sample_rate = inlink->sample_rate;
    s->time_base = inlink->time_base;
    s->ch = av_calloc(s->nb_channels, sizeof(*s->ch));
    if (!s->ch)
        return AVERROR(ENOMEM);

    av_log(ctx, AV_LOG_VERBOSE,
           "min_duration:%"PRId64" samples, level:%f, channels:%d\n",
           s->min_duration, s->level, s->nb_channels);
    return 0;
}

static void report_clip(AVFilterContext *ctx, int channel,
                        int64_t start_sample, int64_t duration)
{
    ClipDetectContext *s = ctx->priv;
    double start_time = (double)start_sample / s->sample_rate;
    double dur_time = (double)duration / s->sample_rate;

    av_log(ctx, AV_LOG_INFO,
           "clip_channel:%d clip_start:%.6f clip_duration:%.6f clip_samples:%"PRId64"\n",
           channel, start_time, dur_time, duration);
}

static void check_clip_end(AVFilterContext *ctx, int c)
{
    ClipDetectContext *s = ctx->priv;
    ChannelClipState *ch = &s->ch[c];

    if (ch->clip_run >= s->min_duration) {
        report_clip(ctx, c, ch->clip_start_sample, ch->clip_run);
        ch->total_clips++;
    }
    ch->total_clipped += ch->clip_run;
    ch->clip_run = 0;
}

static inline int is_clipped_float(float sample, double level)
{
    return fabsf(sample) >= (float)level;
}

static inline int is_clipped_double(double sample, double level)
{
    return fabs(sample) >= level;
}

static inline int is_clipped_s16(int16_t sample, double level)
{
    return abs(sample) >= (int)(level * INT16_MAX);
}

static inline int is_clipped_s32(int32_t sample, double level)
{
    return llabs(sample) >= (int64_t)(level * INT32_MAX);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ClipDetectContext *s = ctx->priv;
    int nb_samples = frame->nb_samples;

    for (int c = 0; c < s->nb_channels; c++) {
        ChannelClipState *ch = &s->ch[c];

        for (int i = 0; i < nb_samples; i++) {
            int clipped = 0;

            switch (frame->format) {
            case AV_SAMPLE_FMT_FLTP:
                clipped = is_clipped_float(((const float *)frame->extended_data[c])[i], s->level);
                break;
            case AV_SAMPLE_FMT_FLT: {
                const float *data = (const float *)frame->data[0];
                clipped = is_clipped_float(data[i * s->nb_channels + c], s->level);
                break;
            }
            case AV_SAMPLE_FMT_DBLP:
                clipped = is_clipped_double(((const double *)frame->extended_data[c])[i], s->level);
                break;
            case AV_SAMPLE_FMT_DBL: {
                const double *data = (const double *)frame->data[0];
                clipped = is_clipped_double(data[i * s->nb_channels + c], s->level);
                break;
            }
            case AV_SAMPLE_FMT_S16P:
                clipped = is_clipped_s16(((const int16_t *)frame->extended_data[c])[i], s->level);
                break;
            case AV_SAMPLE_FMT_S16: {
                const int16_t *data = (const int16_t *)frame->data[0];
                clipped = is_clipped_s16(data[i * s->nb_channels + c], s->level);
                break;
            }
            case AV_SAMPLE_FMT_S32P:
                clipped = is_clipped_s32(((const int32_t *)frame->extended_data[c])[i], s->level);
                break;
            case AV_SAMPLE_FMT_S32: {
                const int32_t *data = (const int32_t *)frame->data[0];
                clipped = is_clipped_s32(data[i * s->nb_channels + c], s->level);
                break;
            }
            }

            if (clipped) {
                if (ch->clip_run == 0)
                    ch->clip_start_sample = s->total_samples + i;
                ch->clip_run++;
            } else if (ch->clip_run > 0) {
                check_clip_end(ctx, c);
            }
        }
    }

    s->total_samples += nb_samples;
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ClipDetectContext *s = ctx->priv;

    if (s->ch) {
        for (int c = 0; c < s->nb_channels; c++) {
            ChannelClipState *ch = &s->ch[c];

            /* Flush any in-progress clip run */
            if (ch->clip_run > 0)
                check_clip_end(ctx, c);

            if (ch->total_clips > 0 || ch->total_clipped > 0) {
                av_log(ctx, AV_LOG_INFO,
                       "channel:%d total_clip_events:%"PRId64" total_clipped_samples:%"PRId64"\n",
                       c, ch->total_clips, ch->total_clipped);
            }
        }
        av_freep(&s->ch);
    }
}

static const AVFilterPad clipdetect_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_af_clipdetect = {
    .p.name        = "clipdetect",
    .p.description = NULL_IF_CONFIG_SMALL("Detect sustained audio clipping."),
    .p.priv_class  = &clipdetect_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .priv_size     = sizeof(ClipDetectContext),
    .uninit        = uninit,
    FILTER_INPUTS(clipdetect_inputs),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
                      AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
                      AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P,
                      AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P),
};
