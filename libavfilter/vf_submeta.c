/*
 * Put the active subtitle's text into video frame metadata
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
 * Takes a video stream and a subtitle stream, changes no pixels, and sets one
 * metadata key on each video frame to whatever caption is on screen at that
 * frame's time. Empty when none is.
 *
 * It exists so drawtext can burn in a live subtitle stream. drawtext expands
 * %{metadata:key} per frame and has no other per-frame source of text, so this
 * is the missing half; nothing is drawn here and nothing about drawtext changes.
 * Anything else that reads frame metadata gets it for the same price.
 *
 *     [v][s]submeta[vm]; [vm]drawtext=text='%{metadata\\:lavfi.sub.text}':...
 *
 * What it does not do is worth saying. A caption stream carries position,
 * italics and colour; this reduces it to a string, because a string is all
 * drawtext can take. Captions that move to avoid covering a face will not move.
 * For burn-in that keeps all of it, the filter to reach for is one that renders
 * ASS onto video, not this and drawtext.
 */

#include "libavcodec/avcodec.h"

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

typedef struct SubMetaEvent {
    int64_t  start;             /* AV_TIME_BASE_Q */
    int64_t  end;
    char    *text;              /* "" clears */
} SubMetaEvent;

typedef struct SubMetaContext {
    const AVClass *class;

    char    *key;
    int      strip;

    SubMetaEvent *events;
    int      nb_events;
    int      max_events;
    int      sub_eof;
    char    *last_logged;
} SubMetaContext;

#define OFFSET(x) offsetof(SubMetaContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption submeta_options[] = {
    { "key",   "metadata key to set on each video frame", OFFSET(key),
      AV_OPT_TYPE_STRING, { .str = "lavfi.sub.text" }, 0, 0, FLAGS },
    { "strip", "strip ASS override blocks and turn \\N into a line break",
      OFFSET(strip), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(submeta);

/* ffmpeg's internal ASS is a header of eight commas and then the text, with
 * override blocks in braces and \N for a line break. cc_dec emits all of it, so
 * without this the overrides land on screen as literal text -- the same fault
 * the teletext encoder had. */
static void strip_ass(const char *p, AVBPrint *out)
{
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (end) {
                p = end + 1;
                continue;
            }
        }
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) {
            av_bprintf(out, "\n");
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1] == 'h') {
            av_bprintf(out, " ");
            p += 2;
            continue;
        }
        av_bprint_chars(out, *p, 1);
        p++;
    }
}

static char *rect_text(const SubMetaContext *s, const AVSubtitleRect *rect)
{
    AVBPrint bp;
    char *out = NULL;
    unsigned i;
    int blank = 1;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (rect->type == SUBTITLE_ASS && rect->ass) {
        const char *p = rect->ass;
        int commas = 0;

        while (*p && commas < 8) {
            if (*p == ',')
                commas++;
            p++;
        }
        if (commas >= 8) {
            if (s->strip)
                strip_ass(p, &bp);
            else
                av_bprintf(&bp, "%s", p);
        }
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
        av_bprintf(&bp, "%s", rect->text);
    }

    if (!av_bprint_is_complete(&bp)) {
        av_bprint_finalize(&bp, NULL);
        return NULL;
    }

    /* A cue of nothing but spaces is not a caption. cea608embed sends exactly
     * that when keepalive is on, so that the 608 track never goes empty and a
     * player does not drop it; decoded back it is a caption of spaces, and
     * anything drawing a box around it draws an empty box. Treat it as blank. */
    for (i = 0; i < bp.len; i++) {
        if (bp.str[i] != ' ' && bp.str[i] != '\t' && bp.str[i] != '\n' &&
            bp.str[i] != '\r') {
            blank = 0;
            break;
        }
    }
    if (blank)
        av_bprint_clear(&bp);

    av_bprint_finalize(&bp, &out);
    return out;
}

static int add_event(SubMetaContext *s, int64_t start, int64_t end, char *text)
{
    if (s->nb_events == s->max_events) {
        const int n = s->max_events ? s->max_events * 2 : 32;
        SubMetaEvent *e = av_realloc_array(s->events, n, sizeof(*e));
        if (!e)
            return AVERROR(ENOMEM);
        s->events = e;
        s->max_events = n;
    }
    s->events[s->nb_events].start = start;
    s->events[s->nb_events].end   = end;
    s->events[s->nb_events].text  = text;
    s->nb_events++;
    return 0;
}

/* Events that ended a second ago cannot become current again, and a long run
 * would otherwise keep every caption it ever saw. */
static void prune(SubMetaContext *s, int64_t now)
{
    int keep = 0;

    for (int i = 0; i < s->nb_events; i++) {
        if (s->events[i].end > now - AV_TIME_BASE)
            s->events[keep++] = s->events[i];
        else
            av_freep(&s->events[i].text);
    }
    s->nb_events = keep;
}

static int take_subtitle(AVFilterContext *avctx, AVFrame *frame)
{
    SubMetaContext *s = avctx->priv;
    AVRational tb = frame->time_base;
    const AVSubtitle *sub;
    int64_t start, end;
    int ret;

    if (!frame->buf[0])
        return 0;
    sub = (const AVSubtitle *)frame->buf[0]->data;
    if (!sub)
        return 0;

    if (tb.num <= 0 || tb.den <= 0)
        tb = AV_TIME_BASE_Q;
    start = av_rescale_q(frame->pts, tb, AV_TIME_BASE_Q);
    end   = start + (int64_t)sub->end_display_time * 1000;

    /* A cue with nothing in it is how a decoder says "clear". Recording it as
     * an empty event rather than ignoring it is what makes the text go away
     * again: a later event always supersedes an earlier one. */
    if (!sub->num_rects) {
        char *empty = av_strdup("");
        if (!empty)
            return AVERROR(ENOMEM);
        return add_event(s, start, end > start ? end : INT64_MAX, empty);
    }

    for (unsigned i = 0; i < sub->num_rects; i++) {
        char *text = rect_text(s, sub->rects[i]);
        if (!text)
            continue;
        ret = add_event(s, start, end > start ? end : INT64_MAX, text);
        if (ret < 0) {
            av_free(text);
            return ret;
        }
    }
    return 0;
}

/* The caption on screen at this instant: the latest one that has started and
 * not finished. Latest, so a roll-up caption that supersedes the line above it
 * wins over the line it replaces. */
static const char *active_text(const SubMetaContext *s, int64_t now)
{
    const char *text = NULL;
    int64_t best = INT64_MIN;

    for (int i = 0; i < s->nb_events; i++) {
        if (s->events[i].start <= now && now < s->events[i].end &&
            s->events[i].start >= best) {
            best = s->events[i].start;
            text = s->events[i].text;
        }
    }
    return text;
}

static int take_video(AVFilterContext *avctx, AVFrame *frame)
{
    SubMetaContext *s = avctx->priv;
    AVFilterLink *inlink = avctx->inputs[0];
    AVRational tb = frame->time_base;
    const char *text;
    int64_t now;

    if (tb.num <= 0 || tb.den <= 0)
        tb = inlink->time_base;
    now = frame->pts == AV_NOPTS_VALUE ? INT64_MIN
                                       : av_rescale_q(frame->pts, tb, AV_TIME_BASE_Q);

    prune(s, now);
    text = active_text(s, now);

    /* Always set it, empty when nothing is up, so %{metadata:key} needs no
     * default and never renders the caption before last. */
    if (av_log_get_level() >= AV_LOG_VERBOSE && text && *text &&
        (!s->last_logged || strcmp(s->last_logged, text))) {
        av_log(avctx, AV_LOG_VERBOSE, "%s = \"%s\"\n", s->key, text);
        av_freep(&s->last_logged);
        s->last_logged = av_strdup(text);
    }

    av_dict_set(&frame->metadata, s->key, text ? text : "", 0);

    return ff_filter_frame(avctx->outputs[0], frame);
}

static int activate(AVFilterContext *avctx)
{
    SubMetaContext *s = avctx->priv;
    AVFilterLink *vin = avctx->inputs[0];
    AVFilterLink *sin = avctx->inputs[1];
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *frame;
    int64_t pts;
    int ret, status;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, avctx);

    /* Subtitles first and all of them: a cue has to be known before the video
     * frame it belongs to is handed on, and they arrive in their own time. */
    if (!s->sub_eof) {
        while ((ret = ff_inlink_consume_frame(sin, &frame)) > 0) {
            ret = take_subtitle(avctx, frame);
            av_frame_free(&frame);
            if (ret < 0)
                return ret;
        }
        if (ret < 0)
            return ret;
        if (ff_inlink_acknowledge_status(sin, &status, &pts))
            s->sub_eof = 1;
    }

    ret = ff_inlink_consume_frame(vin, &frame);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return take_video(avctx, frame);

    if (ff_inlink_acknowledge_status(vin, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    if (ff_outlink_frame_wanted(outlink)) {
        ff_inlink_request_frame(vin);
        if (!s->sub_eof)
            ff_inlink_request_frame(sin);
    }
    return FFERROR_NOT_READY;
}

static av_cold int init(AVFilterContext *avctx)
{
    AVFilterPad pad;
    int ret;

    memset(&pad, 0, sizeof(pad));
    pad.name = "default";
    pad.type = AVMEDIA_TYPE_VIDEO;
    if ((ret = ff_append_inpad(avctx, &pad)) < 0)
        return ret;

    memset(&pad, 0, sizeof(pad));
    pad.name = "subtitle";
    pad.type = AVMEDIA_TYPE_SUBTITLE;
    return ff_append_inpad(avctx, &pad);
}

static av_cold void uninit(AVFilterContext *avctx)
{
    SubMetaContext *s = avctx->priv;

    for (int i = 0; i < s->nb_events; i++)
        av_freep(&s->events[i].text);
    av_freep(&s->events);
    av_freep(&s->last_logged);
    s->nb_events = s->max_events = 0;
}

/* The output carries the video input's properties unchanged. With a subtitle
 * link on input 1 there is no sensible default for libavfilter to infer, so say
 * it outright, as cea608embed does. */
static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink = avctx->inputs[0];
    FilterLink *il = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);

    outlink->w                   = inlink->w;
    outlink->h                   = inlink->h;
    outlink->time_base           = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    ol->frame_rate               = il->frame_rate;

    return 0;
}

static const AVFilterPad submeta_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const FFFilter ff_vf_submeta = {
    .p.name        = "submeta",
    .p.description = NULL_IF_CONFIG_SMALL("Put the active subtitle's text into video frame metadata."),
    .p.priv_class  = &submeta_class,
    .p.inputs      = NULL,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .priv_size     = sizeof(SubMetaContext),
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_OUTPUTS(submeta_outputs),
};
