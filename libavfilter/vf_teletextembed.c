/*
 * Teletext VBI Embedding Filter
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
 * Video filter to embed teletext subtitles as VBI waveforms.
 *
 * Renders World System Teletext (WST) data into the VBI lines of SD video
 * frames for PAL (625i) and NTSC (525i) systems.
 *
 * Teletext VBI waveform parameters (ITU-R BT.653 System B):
 *   Bit rate: 6.9375 MHz
 *   Sample rate: 13.5 MHz (SD-SDI)
 *   Samples per bit: ~1.946
 *
 * Waveform structure per line:
 *   Clock run-in: 16 cycles (32 bits) of alternating 0/1
 *   Framing code: 11100100 (0xE4)
 *   Data: 42 bytes (MRAG + 40 character bytes) = 336 bits
 */

#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/teletextenc.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

/* VBI waveform parameters */
#define VBI_LUMA_BLACK   0x10   /* Black level (IRE 0) */
#define VBI_LUMA_WHITE   0xEB   /* White level (IRE 100) */
#define VBI_CRI_OFFSET   72     /* Clock run-in start sample (after H sync) */

/* Teletext data unit size from encoder */
#define TELETEXT_DATA_UNIT_SIZE 46

typedef struct TeletextEvent {
    int64_t start_pts;
    int64_t end_pts;
    uint8_t *data;       /* Raw teletext data units */
    int data_size;
    char *text;          /* Plain text for logging */
    int logged;          /* Already logged this event */
} TeletextEvent;

typedef struct TeletextEmbedContext {
    const AVClass *class;
    int line_f1;         /* VBI line number for field 1 (1-based) */
    int line_f2;         /* VBI line number for field 2 (1-based, 0=same as f1) */
    int system;          /* 0=auto, 1=PAL, 2=NTSC */

    /* Subtitle events */
    TeletextEvent *events;
    int nb_events;
    int events_capacity;

    /* State */
    int stream_input;
    int sub_eof;
    int current_event;   /* Index of currently active event, or -1 */

    /* Teletext encoding state */
    int magazine;        /* Magazine number (1-8) */
    int page;            /* Page number (100-899) */
} TeletextEmbedContext;

/**
 * Strip ASS formatting escape sequences from text.
 * Handles {...} override blocks and \N \n \h escapes.
 */
static void strip_ass_formatting(const char *input, char *out, int max_len)
{
    const char *p = input;
    int pos = 0;

    while (*p && pos < max_len - 1) {
        if (*p == '{') {
            /* Skip ASS override block */
            const char *end = strchr(p, '}');
            if (end) {
                p = end + 1;
                continue;
            }
        }
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) {
            out[pos++] = '\n';
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1] == 'h') {
            out[pos++] = ' ';
            p += 2;
            continue;
        }
        out[pos++] = *p++;
    }
    out[pos] = '\0';
}

/**
 * Extract plain text from ASS subtitle rect.
 * Skips 8 header fields (ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect)
 * and strips ASS formatting from the Text field.
 */
static char *extract_text_from_ass(const char *ass, char *out, int max_len)
{
    const char *p = ass;
    int field_count = 0;

    /* Skip 8 commas to get to the Text field */
    while (*p && field_count < 8) {
        if (*p == ',')
            field_count++;
        p++;
    }

    if (field_count < 8) {
        out[0] = '\0';
        return out;
    }

    strip_ass_formatting(p, out, max_len);
    return out;
}

/**
 * Encode a single line of text as a teletext subtitle data unit.
 * Returns the number of bytes written (46 for one data unit).
 */
static int encode_teletext_line(uint8_t *out, const char *text, int magazine,
                                 int row, int field)
{
    uint8_t line_data[42];
    int mag_enc = (magazine == 8) ? 0 : magazine;
    int i;

    /* Encode MRAG (Magazine and Row Address Group) */
    line_data[0] = ff_teletext_ham84((mag_enc & 0x07) | ((row & 0x01) << 3));
    line_data[1] = ff_teletext_ham84((row >> 1) & 0x0F);

    /* Fill with spaces (with odd parity) initially */
    for (i = 2; i < 42; i++)
        line_data[i] = ff_teletext_odd_parity(' ');

    /* Copy text characters with odd parity, centered */
    int text_len = strlen(text);
    if (text_len > 40)
        text_len = 40;
    int start_col = (40 - text_len) / 2;  /* Center the text */

    for (i = 0; i < text_len && text[i]; i++) {
        unsigned char c = text[i];
        if (c >= 0x20 && c <= 0x7E)
            line_data[2 + start_col + i] = ff_teletext_odd_parity(c);
    }

    /* Build the data unit */
    return ff_teletext_build_data_unit(out, TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE,
                                       field == 1 ? 1 : 0, 21,  /* Line 21 is standard subtitle line */
                                       magazine, row, line_data);
}

/**
 * Encode text subtitle as teletext data units.
 * Input should be plain text (already stripped of ASS formatting).
 * Returns allocated buffer with teletext data, sets out_size.
 */
static uint8_t *encode_text_to_teletext(AVFilterContext *ctx, const char *text,
                                         int *out_size)
{
    TeletextEmbedContext *s = ctx->priv;
    char *lines[4];  /* Max 4 lines for subtitle */
    int num_lines = 0;
    uint8_t *out;
    int out_pos = 0;
    char *p, *saveptr;

    /* Split into lines */
    char *tmp = av_strdup(text);
    if (!tmp)
        return NULL;

    p = av_strtok(tmp, "\n", &saveptr);
    while (p && num_lines < 4) {
        /* Skip empty lines */
        while (*p == ' ') p++;
        if (*p)
            lines[num_lines++] = p;
        p = av_strtok(NULL, "\n", &saveptr);
    }

    if (num_lines == 0) {
        av_free(tmp);
        *out_size = 0;
        return NULL;
    }

    /* Allocate output: 2 data units per line (both fields) */
    out = av_malloc(num_lines * 2 * TELETEXT_DATA_UNIT_SIZE);
    if (!out) {
        av_free(tmp);
        return NULL;
    }

    /* Encode each line for both fields */
    /* Subtitle rows typically start at row 21, 22, 23, 24 (bottom of screen) */
    for (int i = 0; i < num_lines; i++) {
        int row = 24 - num_lines + 1 + i;  /* Bottom-align subtitles */

        /* Field 1 */
        encode_teletext_line(out + out_pos, lines[i], s->magazine, row, 1);
        out_pos += TELETEXT_DATA_UNIT_SIZE;

        /* Field 2 (same content, different field parity) */
        encode_teletext_line(out + out_pos, lines[i], s->magazine, row, 2);
        out_pos += TELETEXT_DATA_UNIT_SIZE;
    }

    av_free(tmp);
    *out_size = out_pos;
    return out;
}

/**
 * Render a single bit into the luma samples.
 * Writes approximately 2 samples per bit (13.5 MHz / 6.9375 MHz ≈ 1.946).
 */
static inline void render_bit(uint8_t *luma, int *pos, int bit, int max_pos)
{
    uint8_t level = bit ? VBI_LUMA_WHITE : VBI_LUMA_BLACK;
    if (*pos < max_pos) luma[(*pos)++] = level;
    if (*pos < max_pos) luma[(*pos)++] = level;
}

/**
 * Render teletext data into a luma buffer (720 samples).
 *
 * @param luma   Output buffer for luma samples (must be >= 720 bytes)
 * @param data   42 bytes of teletext line data (MRAG + 40 data bytes)
 * @param width  Line width in samples (720 for SD)
 */
static void render_teletext_line(uint8_t *luma, const uint8_t *data, int width)
{
    int pos = VBI_CRI_OFFSET;

    /* Initialize line to black */
    memset(luma, VBI_LUMA_BLACK, width);

    /* Clock run-in: 16 cycles of alternating 1/0 (32 bits)
     * This creates the 10101010... pattern for receiver sync */
    for (int i = 0; i < 16; i++) {
        render_bit(luma, &pos, 1, width);
        render_bit(luma, &pos, 0, width);
    }

    /* Framing code: 11100100 binary = 0xE4 (transmitted LSB first) */
    uint8_t framing = 0xE4;
    for (int i = 0; i < 8; i++) {
        render_bit(luma, &pos, (framing >> i) & 1, width);
    }

    /* Data: 42 bytes transmitted LSB first with odd parity */
    for (int byte = 0; byte < 42; byte++) {
        uint8_t b = data[byte];
        for (int bit = 0; bit < 8; bit++) {
            render_bit(luma, &pos, (b >> bit) & 1, width);
        }
    }
}

/**
 * Write luma samples into UYVY frame data at specified line.
 * UYVY format: U Y0 V Y1 (4 bytes per 2 horizontal pixels)
 */
static void write_vbi_line_uyvy(uint8_t *frame_data, int linesize,
                                 int line, const uint8_t *luma, int width)
{
    uint8_t *row = frame_data + line * linesize;

    for (int i = 0; i < width; i++) {
        /* Y samples are at byte offsets 1, 3, 5, 7, ... */
        row[i * 2 + 1] = luma[i];
        /* Set chroma to neutral gray (128) */
        row[i * 2] = 128;
    }
}

/**
 * Calculate absolute line number in frame for interlaced video.
 *
 * @param line_in_field  Line number within field (1-based, e.g., 21)
 * @param field          Field number (1 = odd/first, 2 = even/second)
 * @param is_pal         1 for PAL (625 lines), 0 for NTSC (525 lines)
 * @return               0-based line index in frame
 */
static int field_line_to_frame(int line_in_field, int field, int is_pal)
{
    if (is_pal) {
        /* PAL 625i: field 1 has lines 1-312, field 2 has lines 313-625
         * In frame buffer, lines are interleaved or sequential depending on format.
         * For progressive frame storage of interlaced content:
         * Field 1 line N -> frame line (N-1)*2
         * Field 2 line N -> frame line (N-1)*2 + 1
         * But for VBI, we typically use frame line numbers directly:
         * Field 1 line 21 -> frame line 20 (0-based)
         * Field 2 line 21 -> frame line 333 (= 313 + 21 - 1, 0-based)
         */
        if (field == 1)
            return line_in_field - 1;
        else
            return 312 + line_in_field - 1;  /* 313 + line - 1 - 1 for 0-based */
    } else {
        /* NTSC 525i: field 1 has lines 1-262, field 2 has lines 263-525 */
        if (field == 1)
            return line_in_field - 1;
        else
            return 262 + line_in_field - 1;
    }
}

static int add_teletext_event(TeletextEmbedContext *ctx, int64_t start_pts,
                               int64_t end_pts, const uint8_t *data, int size,
                               const char *text)
{
    if (ctx->nb_events >= ctx->events_capacity) {
        int new_cap = ctx->events_capacity ? ctx->events_capacity * 2 : 64;
        TeletextEvent *new_events = av_realloc_array(ctx->events, new_cap,
                                                      sizeof(TeletextEvent));
        if (!new_events)
            return AVERROR(ENOMEM);
        ctx->events = new_events;
        ctx->events_capacity = new_cap;
    }

    TeletextEvent *ev = &ctx->events[ctx->nb_events];
    ev->start_pts = start_pts;
    ev->end_pts = end_pts;
    ev->data = av_memdup(data, size);
    ev->data_size = size;
    ev->text = text ? av_strdup(text) : NULL;
    ev->logged = 0;

    if (!ev->data)
        return AVERROR(ENOMEM);

    ctx->nb_events++;
    return 0;
}

static int process_subtitle_frame(AVFilterContext *avctx, AVFrame *frame)
{
    TeletextEmbedContext *ctx = avctx->priv;
    AVSubtitle *sub;
    int64_t start_pts, duration, end_pts;
    AVRational tb;

    if (!frame->buf[0]) {
        av_log(avctx, AV_LOG_DEBUG, "Subtitle frame has no buf[0]\n");
        return 0;
    }

    sub = (AVSubtitle *)frame->buf[0]->data;
    if (!sub) {
        av_log(avctx, AV_LOG_DEBUG, "Subtitle frame buf[0]->data is NULL\n");
        return 0;
    }

    tb = frame->time_base;
    if (tb.num <= 0 || tb.den <= 0)
        tb = AV_TIME_BASE_Q;

    start_pts = av_rescale_q(frame->pts, tb, AV_TIME_BASE_Q);
    duration = (int64_t)sub->end_display_time * 1000;
    end_pts = start_pts + duration;

    av_log(avctx, AV_LOG_DEBUG, "Subtitle: num_rects=%u start=%.3fs end=%.3fs\n",
           sub->num_rects, start_pts / 1e6, end_pts / 1e6);

    for (unsigned i = 0; i < sub->num_rects; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        char plain[256];
        uint8_t *ttx_data;
        int ttx_size;

        av_log(avctx, AV_LOG_DEBUG, "  rect[%u]: type=%d ass=%p text=%p\n",
               i, rect->type, rect->ass, rect->text);

        /* Extract plain text from ASS or text subtitle */
        if (rect->type == SUBTITLE_ASS && rect->ass) {
            extract_text_from_ass(rect->ass, plain, sizeof(plain));
        } else if (rect->type == SUBTITLE_TEXT && rect->text) {
            av_strlcpy(plain, rect->text, sizeof(plain));
        } else {
            /* Check if this is raw teletext data (stored as bitmap data) */
            if (rect->type == SUBTITLE_BITMAP && rect->data[0] && rect->linesize[0] > 0) {
                int ret = add_teletext_event(ctx, start_pts, end_pts,
                                              rect->data[0], rect->linesize[0], NULL);
                if (ret < 0)
                    return ret;
            }
            continue;
        }

        /* Skip empty subtitles */
        if (!plain[0])
            continue;

        /* Encode text to teletext */
        ttx_data = encode_text_to_teletext(avctx, plain, &ttx_size);
        if (ttx_data && ttx_size > 0) {
            int ret = add_teletext_event(ctx, start_pts, end_pts,
                                          ttx_data, ttx_size, plain);
            av_free(ttx_data);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int render_teletext_to_frame(AVFilterContext *avctx, AVFrame *frame,
                                     const uint8_t *ttx_data, int ttx_size)
{
    TeletextEmbedContext *ctx = avctx->priv;
    int width = frame->width;
    int height = frame->height;
    int is_pal;
    uint8_t luma[720];

    /* Determine system (PAL/NTSC) */
    if (ctx->system == 1 || (ctx->system == 0 && height >= 576))
        is_pal = 1;
    else if (ctx->system == 2 || (ctx->system == 0 && height < 576))
        is_pal = 0;
    else
        is_pal = 1;  /* Default to PAL */

    /* Process each teletext data unit (46 bytes each) */
    int num_units = ttx_size / TELETEXT_DATA_UNIT_SIZE;

    for (int i = 0; i < num_units; i++) {
        const uint8_t *du = ttx_data + i * TELETEXT_DATA_UNIT_SIZE;

        /* Data unit format (from teletext encoder):
         *   [0]: data_unit_id
         *   [1]: data_unit_length (44)
         *   [2]: field_parity (bit 5) + line_offset (bits 0-4)
         *   [3]: framing_code (0xE4) - not used, we generate our own
         *   [4-45]: 42 bytes of teletext data (MRAG + 40 bytes)
         */
        int field = ((du[2] >> 5) & 1) ? 1 : 2;  /* 1=odd/field1, 0=even/field2 */
        int line_offset = du[2] & 0x1F;
        const uint8_t *teletext_data = &du[4];  /* 42 bytes */

        /* Use configured line numbers or line from data unit */
        int vbi_line;
        if (field == 1)
            vbi_line = ctx->line_f1 ? ctx->line_f1 : line_offset;
        else
            vbi_line = ctx->line_f2 ? ctx->line_f2 : (ctx->line_f1 ? ctx->line_f1 : line_offset);

        /* Convert to frame line number */
        int frame_line = field_line_to_frame(vbi_line, field, is_pal);

        /* Validate line is within VBI region */
        if (frame_line < 0 || frame_line >= height) {
            av_log(avctx, AV_LOG_WARNING,
                   "VBI line %d (field %d, line %d) outside frame bounds\n",
                   frame_line, field, vbi_line);
            continue;
        }

        /* Render teletext waveform */
        render_teletext_line(luma, teletext_data, FFMIN(width, 720));

        /* Write to frame */
        if (frame->format == AV_PIX_FMT_UYVY422) {
            write_vbi_line_uyvy(frame->data[0], frame->linesize[0],
                                frame_line, luma, FFMIN(width, 720));
        } else {
            av_log(avctx, AV_LOG_WARNING,
                   "Unsupported pixel format %s for VBI embedding\n",
                   av_get_pix_fmt_name(frame->format));
            return 0;
        }
    }

    return 0;
}

static int process_video_frame(AVFilterContext *avctx, AVFilterLink *inlink,
                                AVFrame *frame)
{
    TeletextEmbedContext *ctx = avctx->priv;
    AVFilterLink *outlink = avctx->outputs[0];
    int64_t frame_pts;
    int ret;

    frame_pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

    /* Make frame writable since we'll modify it */
    ret = ff_inlink_make_frame_writable(inlink, &frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    /* Find active event for this frame */
    for (int i = 0; i < ctx->nb_events; i++) {
        TeletextEvent *ev = &ctx->events[i];
        if (frame_pts >= ev->start_pts && frame_pts < ev->end_pts) {
            /* Log when event first becomes active */
            if (!ev->logged) {
                if (ev->text) {
                    av_log(avctx, AV_LOG_INFO, "TTX EMBED: %s\n", ev->text);
                }
                ev->logged = 1;
            }
            ret = render_teletext_to_frame(avctx, frame, ev->data, ev->data_size);
            if (ret < 0) {
                av_frame_free(&frame);
                return ret;
            }
            break;
        }
    }

    return ff_filter_frame(outlink, frame);
}

static int activate(AVFilterContext *avctx)
{
    TeletextEmbedContext *ctx = avctx->priv;
    AVFilterLink *video_link = avctx->inputs[0];
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *frame;
    int ret, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, avctx);

    if (ctx->stream_input) {
        AVFilterLink *sub_link = avctx->inputs[1];

        /* Consume all available subtitle frames */
        if (!ctx->sub_eof) {
            while (1) {
                ret = ff_inlink_consume_frame(sub_link, &frame);
                if (ret < 0)
                    return ret;
                if (!ret)
                    break;
                ret = process_subtitle_frame(avctx, frame);
                av_frame_free(&frame);
                if (ret < 0)
                    return ret;
            }

            if (ff_inlink_acknowledge_status(sub_link, &status, &pts))
                ctx->sub_eof = 1;
        }
    }

    /* Process video frame */
    ret = ff_inlink_consume_frame(video_link, &frame);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return process_video_frame(avctx, video_link, frame);

    if (ff_inlink_acknowledge_status(video_link, &status, &pts)) {
        ff_outlink_set_status(outlink, status, pts);
        return 0;
    }

    if (ff_outlink_frame_wanted(outlink)) {
        ff_inlink_request_frame(video_link);
        if (ctx->stream_input && !ctx->sub_eof)
            ff_inlink_request_frame(avctx->inputs[1]);
    }

    return 0;
}

static av_cold int init(AVFilterContext *avctx)
{
    TeletextEmbedContext *ctx = avctx->priv;
    AVFilterPad pad;
    int ret;

    /* Video input pad */
    memset(&pad, 0, sizeof(pad));
    pad.name = "default";
    pad.type = AVMEDIA_TYPE_VIDEO;
    ret = ff_append_inpad(avctx, &pad);
    if (ret < 0)
        return ret;

    /* Subtitle input pad */
    ctx->stream_input = 1;
    memset(&pad, 0, sizeof(pad));
    pad.name = "subtitle";
    pad.type = AVMEDIA_TYPE_SUBTITLE;
    ret = ff_append_inpad(avctx, &pad);
    if (ret < 0)
        return ret;

    ctx->current_event = -1;

    /* Default to Australian standard: line 21 for both fields */
    if (ctx->line_f1 == 0)
        ctx->line_f1 = 21;

    av_log(avctx, AV_LOG_INFO,
           "Teletext VBI embed: line_f1=%d line_f2=%d system=%s\n",
           ctx->line_f1, ctx->line_f2 ? ctx->line_f2 : ctx->line_f1,
           ctx->system == 1 ? "PAL" : ctx->system == 2 ? "NTSC" : "auto");

    return 0;
}

static av_cold void uninit(AVFilterContext *avctx)
{
    TeletextEmbedContext *ctx = avctx->priv;

    for (int i = 0; i < ctx->nb_events; i++) {
        av_freep(&ctx->events[i].data);
        av_freep(&ctx->events[i].text);
    }
    av_freep(&ctx->events);
    ctx->nb_events = 0;
    ctx->events_capacity = 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink = avctx->inputs[0];
    FilterLink *il = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->time_base = inlink->time_base;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    ol->frame_rate = il->frame_rate;

    /* Validate dimensions for SD - VBI embedding only works for SD video */
    if (inlink->w != 720 || (inlink->h != 576 && inlink->h != 486 && inlink->h != 480)) {
        av_log(avctx, AV_LOG_ERROR,
               "Frame size %dx%d is not standard SD (720x576 PAL or 720x480/486 NTSC). "
               "VBI teletext embedding requires SD video. For HD, use VANC via the decklink output device.\n",
               inlink->w, inlink->h);
        return AVERROR(EINVAL);
    }

    return 0;
}

#define OFFSET(x) offsetof(TeletextEmbedContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption teletextembed_options[] = {
    { "line",     "VBI line number for field 1 (1-based)", OFFSET(line_f1), AV_OPT_TYPE_INT, {.i64 = 21}, 1, 50, FLAGS },
    { "line_f1",  "VBI line number for field 1 (1-based)", OFFSET(line_f1), AV_OPT_TYPE_INT, {.i64 = 21}, 1, 50, FLAGS },
    { "line_f2",  "VBI line number for field 2 (0=same as f1)", OFFSET(line_f2), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 50, FLAGS },
    { "system",   "TV system (0=auto, 1=PAL, 2=NTSC)", OFFSET(system), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 2, FLAGS },
    { "magazine", "Teletext magazine number (1-8)", OFFSET(magazine), AV_OPT_TYPE_INT, {.i64 = 8}, 1, 8, FLAGS },
    { "page",     "Teletext page number (100-899)", OFFSET(page), AV_OPT_TYPE_INT, {.i64 = 888}, 100, 899, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(teletextembed);

static const AVFilterPad teletextembed_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_UYVY422,
    AV_PIX_FMT_NONE
};

const FFFilter ff_vf_teletextembed = {
    .p.name        = "teletextembed",
    .p.description = NULL_IF_CONFIG_SMALL("Embed teletext as VBI waveform in SD video"),
    .p.priv_class  = &teletextembed_class,
    .p.inputs      = NULL,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .priv_size     = sizeof(TeletextEmbedContext),
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_OUTPUTS(teletextembed_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
};
