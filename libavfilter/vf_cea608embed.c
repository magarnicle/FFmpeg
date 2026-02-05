/*
 * CEA-608 Closed Caption Embedding Filter
 * Copyright (c) 2024
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
 * Video filter to embed CEA-608 closed captions from a subtitle file.
 *
 * Reads subtitles (SRT, ASS, etc.) and encodes them as CEA-608 triplets,
 * then injects them into video frames as AV_FRAME_DATA_A53_CC side data.
 */

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "filters.h"
#include "video.h"

/* CEA-608 control codes */
#define CC_RCL  0x20  /* Resume Caption Loading (pop-on) */
#define CC_RU2  0x25  /* Roll-Up 2 rows */
#define CC_RU3  0x26  /* Roll-Up 3 rows */
#define CC_RU4  0x27  /* Roll-Up 4 rows */
#define CC_EDM  0x2C  /* Erase Displayed Memory */
#define CC_CR   0x2D  /* Carriage Return */
#define CC_EOC  0x2F  /* End of Caption */

/* Maximum CC output per frame */
#define MAX_CC_PER_FRAME 128

typedef struct SubtitleEvent {
    int64_t start_pts;    /* Start time in AV_TIME_BASE units */
    int64_t end_pts;      /* End time in AV_TIME_BASE units */
    char *text;           /* Plain text (stripped of formatting) */
    int sent;             /* Whether start has been sent */
    int cleared;          /* Whether end/clear has been sent */
} SubtitleEvent;

typedef struct CEA608EmbedContext {
    const AVClass *class;
    char *filename;         /* Subtitle file path */
    int roll_up;            /* Roll-up rows (0=pop-on, 2-4=roll-up) */
    int data_field;         /* 0=field1, 1=field2 */
    int start_row;          /* Starting row (0=auto) */

    /* Subtitle events */
    SubtitleEvent *events;
    int nb_events;
    int events_capacity;
    int current_event;      /* Index of current event being displayed */

    /* State tracking */
    int64_t last_sub_pts;   /* PTS of last subtitle action */
    int have_active_sub;    /* Whether a subtitle is currently displayed */
} CEA608EmbedContext;

/* Character set from encoder - minimal version for filter use */
enum cc_charset {
    CCSET_BASIC_AMERICAN,
    CCSET_SPECIAL_AMERICAN,
    CCSET_EXTENDED_SPANISH_FRENCH_MISC,
    CCSET_EXTENDED_PORTUGUESE_GERMAN_DANISH,
};

#define CHARSET_OVERRIDE_LIST(START_SET, ENTRY, END_SET) \
    START_SET(CCSET_BASIC_AMERICAN)                      \
        ENTRY(0x27, "\u2019")                            \
        ENTRY(0x2a, "\u00e1")                            \
        ENTRY(0x5c, "\u00e9")                            \
        ENTRY(0x5e, "\u00ed")                            \
        ENTRY(0x5f, "\u00f3")                            \
        ENTRY(0x60, "\u00fa")                            \
        ENTRY(0x7b, "\u00e7")                            \
        ENTRY(0x7c, "\u00f7")                            \
        ENTRY(0x7d, "\u00d1")                            \
        ENTRY(0x7e, "\u00f1")                            \
        ENTRY(0x7f, "\u2588")                            \
    END_SET                                              \
    START_SET(CCSET_SPECIAL_AMERICAN)                    \
        ENTRY(0x30, "\u00ae")                            \
        ENTRY(0x31, "\u00b0")                            \
        ENTRY(0x32, "\u00bd")                            \
        ENTRY(0x33, "\u00bf")                            \
        ENTRY(0x34, "\u2122")                            \
        ENTRY(0x35, "\u00a2")                            \
        ENTRY(0x36, "\u00a3")                            \
        ENTRY(0x37, "\u266a")                            \
        ENTRY(0x38, "\u00e0")                            \
        ENTRY(0x39, "\u00A0")                            \
        ENTRY(0x3a, "\u00e8")                            \
        ENTRY(0x3b, "\u00e2")                            \
        ENTRY(0x3c, "\u00ea")                            \
        ENTRY(0x3d, "\u00ee")                            \
        ENTRY(0x3e, "\u00f4")                            \
        ENTRY(0x3f, "\u00fb")                            \
    END_SET                                              \
    START_SET(CCSET_EXTENDED_SPANISH_FRENCH_MISC)        \
        ENTRY(0x20, "\u00c1")                            \
        ENTRY(0x21, "\u00c9")                            \
        ENTRY(0x22, "\u00d3")                            \
        ENTRY(0x23, "\u00da")                            \
        ENTRY(0x24, "\u00dc")                            \
        ENTRY(0x25, "\u00fc")                            \
        ENTRY(0x26, "\u00b4")                            \
        ENTRY(0x27, "\u00a1")                            \
        ENTRY(0x28, "*")                                 \
        ENTRY(0x29, "\u2018")                            \
        ENTRY(0x2a, "-")                                 \
        ENTRY(0x2b, "\u00a9")                            \
        ENTRY(0x2c, "\u2120")                            \
        ENTRY(0x2d, "\u00b7")                            \
        ENTRY(0x2e, "\u201c")                            \
        ENTRY(0x2f, "\u201d")                            \
        ENTRY(0x30, "\u00c0")                            \
        ENTRY(0x31, "\u00c2")                            \
        ENTRY(0x32, "\u00c7")                            \
        ENTRY(0x33, "\u00c8")                            \
        ENTRY(0x34, "\u00ca")                            \
        ENTRY(0x35, "\u00cb")                            \
        ENTRY(0x36, "\u00eb")                            \
        ENTRY(0x37, "\u00ce")                            \
        ENTRY(0x38, "\u00cf")                            \
        ENTRY(0x39, "\u00ef")                            \
        ENTRY(0x3a, "\u00d4")                            \
        ENTRY(0x3b, "\u00d9")                            \
        ENTRY(0x3c, "\u00f9")                            \
        ENTRY(0x3d, "\u00db")                            \
        ENTRY(0x3e, "\u00ab")                            \
        ENTRY(0x3f, "\u00bb")                            \
    END_SET                                              \
    START_SET(CCSET_EXTENDED_PORTUGUESE_GERMAN_DANISH)   \
        ENTRY(0x20, "\u00c3")                            \
        ENTRY(0x21, "\u00e3")                            \
        ENTRY(0x22, "\u00cd")                            \
        ENTRY(0x23, "\u00cc")                            \
        ENTRY(0x24, "\u00ec")                            \
        ENTRY(0x25, "\u00d2")                            \
        ENTRY(0x26, "\u00f2")                            \
        ENTRY(0x27, "\u00d5")                            \
        ENTRY(0x28, "\u00f5")                            \
        ENTRY(0x29, "{")                                 \
        ENTRY(0x2a, "}")                                 \
        ENTRY(0x2b, "\\")                                \
        ENTRY(0x2c, "^")                                 \
        ENTRY(0x2d, "_")                                 \
        ENTRY(0x2e, "|")                                 \
        ENTRY(0x2f, "~")                                 \
        ENTRY(0x30, "\u00c4")                            \
        ENTRY(0x31, "\u00e4")                            \
        ENTRY(0x32, "\u00d6")                            \
        ENTRY(0x33, "\u00f6")                            \
        ENTRY(0x34, "\u00df")                            \
        ENTRY(0x35, "\u00a5")                            \
        ENTRY(0x36, "\u00a4")                            \
        ENTRY(0x37, "\u00a6")                            \
        ENTRY(0x38, "\u00c5")                            \
        ENTRY(0x39, "\u00e5")                            \
        ENTRY(0x3a, "\u00d8")                            \
        ENTRY(0x3b, "\u00f8")                            \
        ENTRY(0x3c, "\u250c")                            \
        ENTRY(0x3d, "\u2510")                            \
        ENTRY(0x3e, "\u2514")                            \
        ENTRY(0x3f, "\u2518")                            \
    END_SET

static const char charset_overrides[4][128][sizeof("\u266a")] =
{
#define START_SET(IDX) \
    [IDX] = {
#define ENTRY(idx, string) \
        [idx] = string,
#define END_SET \
    },
    CHARSET_OVERRIDE_LIST(START_SET, ENTRY, END_SET)
#undef START_SET
#undef ENTRY
#undef END_SET
};

/* Row mapping for PAC codes */
static const int8_t row_to_index[16] = {
    -1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 0, 6, 7, 8, 9
};

static inline uint8_t add_odd_parity(uint8_t byte)
{
    byte &= 0x7F;
    if (!av_parity(byte))
        byte |= 0x80;
    return byte;
}

static inline void emit_cc_data(uint8_t *buf, int *pos, uint8_t hi, uint8_t lo, int field)
{
    buf[(*pos)++] = field ? 0xFD : 0xFC;
    buf[(*pos)++] = add_odd_parity(hi);
    buf[(*pos)++] = add_odd_parity(lo);
}

static inline void emit_control(uint8_t *buf, int *pos, uint8_t hi, uint8_t lo, int field)
{
    emit_cc_data(buf, pos, hi, lo, field);
    emit_cc_data(buf, pos, hi, lo, field);
}

static void generate_pac(int row, int indent, uint8_t *hi, uint8_t *lo)
{
    int index;
    int style_indent;

    if (row < 1 || row > 15)
        row = 15;

    index = row_to_index[row];

    /* PAC hi byte: 0x10 + (index >> 1)
     * This gives 0x10-0x17 for indices 0-15 */
    *hi = 0x10 + (index >> 1);

    style_indent = 0;
    if (indent >= 4 && indent <= 28) {
        style_indent = 0x10 + ((indent / 4) * 2);
    }

    *lo = 0x40 | ((index & 1) << 5) | style_indent;
}

static int encode_char(const char **utf8, int *charset, uint8_t *code)
{
    const uint8_t *p = (const uint8_t *)*utf8;
    uint32_t codepoint;
    int len;
    int set, c;

    if (!*p)
        return 0;

    if ((*p & 0x80) == 0) {
        codepoint = *p;
        len = 1;
    } else if ((*p & 0xE0) == 0xC0) {
        codepoint = (*p & 0x1F) << 6;
        if ((p[1] & 0xC0) != 0x80) return 0;
        codepoint |= (p[1] & 0x3F);
        len = 2;
    } else if ((*p & 0xF0) == 0xE0) {
        codepoint = (*p & 0x0F) << 12;
        if ((p[1] & 0xC0) != 0x80) return 0;
        codepoint |= (p[1] & 0x3F) << 6;
        if ((p[2] & 0xC0) != 0x80) return 0;
        codepoint |= (p[2] & 0x3F);
        len = 3;
    } else if ((*p & 0xF8) == 0xF0) {
        codepoint = (*p & 0x07) << 18;
        if ((p[1] & 0xC0) != 0x80) return 0;
        codepoint |= (p[1] & 0x3F) << 12;
        if ((p[2] & 0xC0) != 0x80) return 0;
        codepoint |= (p[2] & 0x3F) << 6;
        if ((p[3] & 0xC0) != 0x80) return 0;
        codepoint |= (p[3] & 0x3F);
        len = 4;
    } else {
        return 0;
    }

    if (codepoint >= 0x20 && codepoint < 0x7F) {
        if (charset_overrides[CCSET_BASIC_AMERICAN][codepoint][0]) {
            goto check_extended;
        }
        *charset = CCSET_BASIC_AMERICAN;
        *code = (uint8_t)codepoint;
        *utf8 += len;
        return len;
    }

check_extended:
    for (set = 0; set < 4; set++) {
        for (c = 0x20; c < 0x80; c++) {
            if (charset_overrides[set][c][0]) {
                const char *override = charset_overrides[set][c];
                int olen = strlen(override);
                if (olen == len && memcmp(p, override, len) == 0) {
                    *charset = set;
                    *code = (uint8_t)c;
                    *utf8 += len;
                    return len;
                }
            }
        }
    }

    if (codepoint >= 0x20 && codepoint < 0x80) {
        *charset = CCSET_BASIC_AMERICAN;
        *code = (uint8_t)codepoint;
    } else {
        *charset = CCSET_BASIC_AMERICAN;
        *code = '?';
    }
    *utf8 += len;
    return len;
}

static void strip_ass_formatting(const char *input, AVBPrint *output)
{
    const char *p = input;

    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (end) {
                p = end + 1;
                continue;
            }
        }
        if (*p == '\\' && (p[1] == 'N' || p[1] == 'n')) {
            av_bprintf(output, "\n");
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1] == 'h') {
            av_bprintf(output, " ");
            p += 2;
            continue;
        }
        av_bprint_chars(output, *p, 1);
        p++;
    }
}

static char *extract_subtitle_text(const AVSubtitleRect *rect)
{
    AVBPrint bp;
    char *result = NULL;

    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (rect->type == SUBTITLE_ASS && rect->ass) {
        const char *p = rect->ass;
        int field_count = 0;

        /* FFmpeg's internal ASS format: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
         * Skip 8 commas to get to the Text field */
        while (*p && field_count < 8) {
            if (*p == ',')
                field_count++;
            p++;
        }
        if (field_count >= 8) {
            strip_ass_formatting(p, &bp);
        }
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
        av_bprintf(&bp, "%s", rect->text);
    }

    if (av_bprint_is_complete(&bp) && bp.len > 0) {
        av_bprint_finalize(&bp, &result);
    } else {
        av_bprint_finalize(&bp, NULL);
    }

    return result;
}

static int add_subtitle_event(CEA608EmbedContext *ctx, int64_t start_pts,
                              int64_t end_pts, const char *text)
{
    if (ctx->nb_events >= ctx->events_capacity) {
        int new_capacity = ctx->events_capacity ? ctx->events_capacity * 2 : 64;
        SubtitleEvent *new_events = av_realloc_array(ctx->events, new_capacity,
                                                      sizeof(SubtitleEvent));
        if (!new_events)
            return AVERROR(ENOMEM);
        ctx->events = new_events;
        ctx->events_capacity = new_capacity;
    }

    ctx->events[ctx->nb_events].start_pts = start_pts;
    ctx->events[ctx->nb_events].end_pts = end_pts;
    ctx->events[ctx->nb_events].text = av_strdup(text);
    ctx->events[ctx->nb_events].sent = 0;
    ctx->events[ctx->nb_events].cleared = 0;
    if (!ctx->events[ctx->nb_events].text)
        return AVERROR(ENOMEM);

    ctx->nb_events++;
    return 0;
}

static int load_subtitles(AVFilterContext *avctx)
{
    CEA608EmbedContext *ctx = avctx->priv;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *dec;
    AVPacket *pkt = NULL;
    int ret, stream_idx;

    ret = avformat_open_input(&fmt_ctx, ctx->filename, NULL, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unable to open subtitle file '%s': %s\n",
               ctx->filename, av_err2str(ret));
        return ret;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to find stream info: %s\n",
               av_err2str(ret));
        goto end;
    }

    stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (stream_idx < 0) {
        av_log(avctx, AV_LOG_ERROR, "No subtitle stream found in '%s'\n",
               ctx->filename);
        ret = AVERROR(EINVAL);
        goto end;
    }

    dec = avcodec_find_decoder(fmt_ctx->streams[stream_idx]->codecpar->codec_id);
    if (!dec) {
        av_log(avctx, AV_LOG_ERROR, "Failed to find subtitle decoder\n");
        ret = AVERROR_DECODER_NOT_FOUND;
        goto end;
    }

    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[stream_idx]->codecpar);
    if (ret < 0)
        goto end;

    dec_ctx->pkt_timebase = fmt_ctx->streams[stream_idx]->time_base;

    ret = avcodec_open2(dec_ctx, dec, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open subtitle decoder: %s\n",
               av_err2str(ret));
        goto end;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            AVSubtitle sub = {0};
            int got_subtitle = 0;

            ret = avcodec_decode_subtitle2(dec_ctx, &sub, &got_subtitle, pkt);
            if (ret >= 0 && got_subtitle) {
                int64_t start_pts = sub.pts;
                int64_t duration = (int64_t)sub.end_display_time * 1000;
                int64_t end_pts = start_pts + duration;

                for (int i = 0; i < sub.num_rects; i++) {
                    char *text = extract_subtitle_text(sub.rects[i]);
                    if (text) {
                        ret = add_subtitle_event(ctx, start_pts, end_pts, text);
                        av_free(text);
                        if (ret < 0) {
                            avsubtitle_free(&sub);
                            goto end;
                        }
                    }
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
    }

    av_log(avctx, AV_LOG_INFO, "Loaded %d subtitle events from '%s'\n",
           ctx->nb_events, ctx->filename);
    ret = 0;

end:
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    return ret;
}

/**
 * Encode text to CEA-608 triplets
 */
static int encode_text_to_cc(CEA608EmbedContext *ctx, const char *text,
                             uint8_t *buf, int bufsize)
{
    int pos = 0;
    uint8_t hi, lo;
    const char *line_start;
    int line_num = 0;
    int num_lines = 1;
    int last_row;

    /* Count lines */
    for (const char *p = text; *p; p++) {
        if (*p == '\n') num_lines++;
    }

    /* Emit mode control */
    if (ctx->roll_up >= 2 && ctx->roll_up <= 4) {
        emit_control(buf, &pos, 0x14, CC_RU2 + (ctx->roll_up - 2), ctx->data_field);
    } else {
        emit_control(buf, &pos, 0x14, CC_RCL, ctx->data_field);
    }

    /* Calculate starting row */
    if (ctx->start_row > 0) {
        last_row = ctx->start_row;
    } else {
        last_row = 15 - num_lines + 1;
        if (last_row < 1) last_row = 1;
    }

    line_start = text;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        const char *p;
        int current_row;
        int char_count = 0;

        if (!line_end)
            line_end = line_start + strlen(line_start);

        current_row = last_row + line_num;
        if (current_row > 15) current_row = 15;

        /* Emit PAC for this row */
        generate_pac(current_row, 0, &hi, &lo);
        emit_control(buf, &pos, hi, lo, ctx->data_field);

        /* Encode characters */
        p = line_start;
        while (p < line_end && char_count < 32) {
            int charset;
            uint8_t code;
            int consumed;

            consumed = encode_char(&p, &charset, &code);
            if (consumed == 0) {
                p++;
                continue;
            }

            if (pos + 6 > bufsize)
                break;

            switch (charset) {
            case CCSET_BASIC_AMERICAN:
                if (p < line_end && char_count + 1 < 32) {
                    const char *next = p;
                    int next_charset;
                    uint8_t next_code;
                    if (encode_char(&next, &next_charset, &next_code) > 0 &&
                        next_charset == CCSET_BASIC_AMERICAN) {
                        emit_cc_data(buf, &pos, code, next_code, ctx->data_field);
                        p = next;
                        char_count += 2;
                        continue;
                    }
                }
                emit_cc_data(buf, &pos, code, 0x00, ctx->data_field);
                char_count++;
                break;
            case CCSET_SPECIAL_AMERICAN:
                emit_cc_data(buf, &pos, 0x11, code, ctx->data_field);
                char_count++;
                break;
            case CCSET_EXTENDED_SPANISH_FRENCH_MISC:
                emit_cc_data(buf, &pos, 0x12, code, ctx->data_field);
                break;
            case CCSET_EXTENDED_PORTUGUESE_GERMAN_DANISH:
                emit_cc_data(buf, &pos, 0x13, code, ctx->data_field);
                break;
            }
        }

        if (ctx->roll_up >= 2) {
            if (*line_end == '\n' && *(line_end + 1)) {
                emit_control(buf, &pos, 0x14, CC_CR, ctx->data_field);
            }
        }

        line_num++;
        line_start = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    /* End of caption for pop-on mode */
    if (ctx->roll_up == 0) {
        emit_control(buf, &pos, 0x14, CC_EOC, ctx->data_field);
    }

    return pos;
}

/**
 * Encode EDM (erase) command
 */
static int encode_erase_cc(CEA608EmbedContext *ctx, uint8_t *buf, int bufsize)
{
    int pos = 0;
    if (bufsize < 6)
        return 0;
    emit_control(buf, &pos, 0x14, CC_EDM, ctx->data_field);
    return pos;
}

static av_cold int init(AVFilterContext *avctx)
{
    CEA608EmbedContext *ctx = avctx->priv;
    int ret;

    if (!ctx->filename) {
        av_log(avctx, AV_LOG_ERROR, "No subtitle file specified\n");
        return AVERROR(EINVAL);
    }

    if (ctx->roll_up != 0 && (ctx->roll_up < 2 || ctx->roll_up > 4)) {
        av_log(avctx, AV_LOG_ERROR, "roll_up must be 0 (pop-on) or 2-4\n");
        return AVERROR(EINVAL);
    }

    ret = load_subtitles(avctx);
    if (ret < 0)
        return ret;

    return 0;
}

static void log_cc_data(AVFilterContext *avctx, const uint8_t *data, int size,
                        const char *text, int is_erase)
{
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(&bp, "CEA-608 %s: ", is_erase ? "ERASE" : "CAPTION");
    if (text && !is_erase) {
        /* Log the text, replacing newlines with spaces for single-line output */
        for (const char *p = text; *p; p++) {
            if (*p == '\n')
                av_bprintf(&bp, " | ");
            else
                av_bprint_chars(&bp, *p, 1);
        }
    }
    av_bprintf(&bp, " [%d bytes:", size);
    for (int i = 0; i < size && i < 30; i += 3) {
        av_bprintf(&bp, " %02X%02X%02X", data[i], data[i+1], data[i+2]);
    }
    if (size > 30)
        av_bprintf(&bp, "...");
    av_bprintf(&bp, "]");

    av_log(avctx, AV_LOG_INFO, "%s\n", bp.str);
    av_bprint_finalize(&bp, NULL);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *avctx = inlink->dst;
    CEA608EmbedContext *ctx = avctx->priv;
    AVFilterLink *outlink = avctx->outputs[0];
    int64_t frame_pts;
    uint8_t cc_buf[MAX_CC_PER_FRAME * 3];
    int cc_size = 0;
    const char *caption_text = NULL;
    int is_erase = 0;

    /* Convert frame PTS to AV_TIME_BASE */
    frame_pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

    /* Check for subtitle events that should start/end */
    for (int i = 0; i < ctx->nb_events; i++) {
        SubtitleEvent *ev = &ctx->events[i];

        /* Start event */
        if (!ev->sent && frame_pts >= ev->start_pts && frame_pts < ev->end_pts) {
            cc_size = encode_text_to_cc(ctx, ev->text, cc_buf, sizeof(cc_buf));
            ev->sent = 1;
            ctx->have_active_sub = 1;
            ctx->current_event = i;
            caption_text = ev->text;
            break;
        }

        /* End event - send erase if this subtitle just ended */
        if (ev->sent && !ev->cleared && frame_pts >= ev->end_pts) {
            cc_size = encode_erase_cc(ctx, cc_buf, sizeof(cc_buf));
            ev->cleared = 1;
            ctx->have_active_sub = 0;
            ctx->current_event = -1;
            is_erase = 1;
            break;
        }
    }

    /* Inject CC data directly as side data */
    if (cc_size > 0) {
        AVFrameSideData *sd;

        /* Log the caption at debug level */
        log_cc_data(avctx, cc_buf, cc_size, caption_text, is_erase);

        /* Add CC data as A53 side data */
        sd = av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, cc_size);
        if (sd) {
            memcpy(sd->data, cc_buf, cc_size);
        } else {
            av_log(avctx, AV_LOG_WARNING, "Failed to allocate CC side data\n");
        }
    }

    return ff_filter_frame(outlink, frame);
}

static av_cold void uninit(AVFilterContext *avctx)
{
    CEA608EmbedContext *ctx = avctx->priv;

    for (int i = 0; i < ctx->nb_events; i++) {
        av_free(ctx->events[i].text);
    }
    av_freep(&ctx->events);
    ctx->nb_events = 0;
    ctx->events_capacity = 0;
}

#define OFFSET(x) offsetof(CEA608EmbedContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption cea608embed_options[] = {
    { "filename", "subtitle file path", OFFSET(filename), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "f",        "subtitle file path", OFFSET(filename), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS },
    { "roll_up",  "roll-up mode (0=pop-on, 2-4=roll-up rows)", OFFSET(roll_up), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 4, FLAGS },
    { "data_field", "select data field (0=first, 1=second)", OFFSET(data_field), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS },
    { "start_row", "starting row (0=auto, 1-15=fixed)", OFFSET(start_row), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 15, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(cea608embed);

static const AVFilterPad cea608embed_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_cea608embed = {
    .p.name        = "cea608embed",
    .p.description = NULL_IF_CONFIG_SMALL("Embed CEA-608 closed captions from subtitle file"),
    .p.priv_class  = &cea608embed_class,
    .priv_size     = sizeof(CEA608EmbedContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(cea608embed_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
};
