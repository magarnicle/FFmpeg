/*
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
 * null subtitle source
 */

#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "avfilter.h"
#include "filters.h"
#include "subtitle.h"

typedef struct SNullContext {
    const AVClass *class;
    int64_t duration;
    AVRational time_base;
    int64_t pts;
} SNullContext;

#define OFFSET(x) offsetof(SNullContext, x)
#define FLAGS AV_OPT_FLAG_SUBTITLE_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption snullsrc_options[] = {
    { "duration", "set the subtitle duration", OFFSET(duration), AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },
    { "d",        "set the subtitle duration", OFFSET(duration), AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },
    { "time_base", "set the time base",        OFFSET(time_base), AV_OPT_TYPE_RATIONAL, {.dbl = 0}, 0, DBL_MAX, FLAGS },
    { "tb",        "set the time base",        OFFSET(time_base), AV_OPT_TYPE_RATIONAL, {.dbl = 0}, 0, DBL_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(snullsrc);

static void subtitle_free(void *opaque, uint8_t *data)
{
    AVSubtitle *sub = (AVSubtitle *)data;
    avsubtitle_free(sub);
    av_free(sub);
}

static av_cold int config_props(AVFilterLink *outlink)
{
    SNullContext *null = outlink->src->priv;

    if (null->time_base.num <= 0 || null->time_base.den <= 0) {
        /* Default to millisecond precision like subtitles typically use */
        null->time_base = (AVRational){1, 1000};
    }

    outlink->time_base = null->time_base;

    if (null->duration >= 0)
        null->duration = av_rescale_q(null->duration, AV_TIME_BASE_Q, null->time_base);

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    SNullContext *null = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    if (null->duration >= 0 && null->pts >= null->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, null->pts);
        return 0;
    }

    if (ff_outlink_frame_wanted(outlink)) {
        AVFrame *frame;
        AVSubtitle *sub;
        AVBufferRef *buf;
        int64_t frame_duration;

        /* Allocate empty AVSubtitle - required for av_frame_clone to work */
        sub = av_mallocz(sizeof(*sub));
        if (!sub)
            return AVERROR(ENOMEM);

        buf = av_buffer_create((uint8_t *)sub, sizeof(*sub),
                               subtitle_free, NULL, 0);
        if (!buf) {
            av_free(sub);
            return AVERROR(ENOMEM);
        }

        frame = av_frame_alloc();
        if (!frame) {
            av_buffer_unref(&buf);
            return AVERROR(ENOMEM);
        }

        /* For null subtitles, generate one frame covering the entire remaining duration.
         * This avoids creating many small frames that could overwhelm the filter graph.
         */
        if (null->duration >= 0) {
            frame_duration = null->duration - null->pts;
        } else {
            /* No duration set - generate 10 second frames */
            frame_duration = null->time_base.den * 10;
        }

        frame->buf[0] = buf;
        frame->pts = null->pts;
        frame->duration = frame_duration;
        frame->time_base = null->time_base;

        /* Set AVSubtitle timing */
        sub->pts = av_rescale_q(null->pts, null->time_base, AV_TIME_BASE_Q);
        sub->start_display_time = 0;
        sub->end_display_time = av_rescale_q(frame_duration, null->time_base,
                                              (AVRational){1, 1000});
        sub->num_rects = 0;
        sub->rects = NULL;

        null->pts += frame_duration;

        return ff_filter_frame(outlink, frame);
    }

    return FFERROR_NOT_READY;
}

static const AVFilterPad snullsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_SUBTITLE,
        .config_props  = config_props,
    },
};

const FFFilter ff_ssrc_snullsrc = {
    .p.name        = "snullsrc",
    .p.description = NULL_IF_CONFIG_SMALL("Null subtitle source, return empty subtitle frames."),
    .p.priv_class  = &snullsrc_class,
    .priv_size     = sizeof(SNullContext),
    FILTER_OUTPUTS(snullsrc_outputs),
    .activate      = activate,
};
