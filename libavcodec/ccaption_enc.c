/*
 * Closed Caption Encoding
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
 * CEA-608 (EIA-608) Closed Caption Encoder
 *
 * Encodes text/ASS subtitles to CEA-608 3-byte triplets for embedding
 * in H.264 streams via A53 SEI.
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"

#define CC_SCREEN_ROWS    15
#define CC_SCREEN_COLUMNS 32

/* CEA-608 control codes (with hi=0x14 for field 1) */
#define CC_RCL  0x20  /* Resume Caption Loading (start pop-on) */
#define CC_BS   0x21  /* Backspace */
#define CC_AOF  0x22  /* Reserved (formerly Alarm Off) */
#define CC_AON  0x23  /* Reserved (formerly Alarm On) */
#define CC_DER  0x24  /* Delete to End of Row */
#define CC_RU2  0x25  /* Roll-Up 2 rows */
#define CC_RU3  0x26  /* Roll-Up 3 rows */
#define CC_RU4  0x27  /* Roll-Up 4 rows */
#define CC_FON  0x28  /* Flash On */
#define CC_RDC  0x29  /* Resume Direct Captioning (paint-on) */
#define CC_TR   0x2A  /* Text Restart */
#define CC_RTD  0x2B  /* Resume Text Display */
#define CC_EDM  0x2C  /* Erase Displayed Memory */
#define CC_CR   0x2D  /* Carriage Return */
#define CC_ENM  0x2E  /* Erase Non-Displayed Memory */
#define CC_EOC  0x2F  /* End of Caption (flip buffers) */

typedef struct CCaptionEncContext {
    AVClass *class;
    int data_field;       /* 0=field1 (0xFC), 1=field2 (0xFD) */
    int roll_up;          /* 0=pop-on, 2/3/4=roll-up rows */
    int row;              /* current row for output (1-15) */
    int start_sent;       /* whether start control has been sent for current subtitle */
} CCaptionEncContext;

/* Character set identifiers matching the decoder */
enum cc_charset {
    CCSET_BASIC_AMERICAN,
    CCSET_SPECIAL_AMERICAN,
    CCSET_EXTENDED_SPANISH_FRENCH_MISC,
    CCSET_EXTENDED_PORTUGUESE_GERMAN_DANISH,
};

/*
 * Character override list - same as decoder but we build a reverse lookup.
 * This macro is duplicated from ccaption_dec.c to avoid cross-module dependencies.
 */
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

/* Build the charset_overrides table (same format as decoder) */
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

/*
 * Row mapping for PAC codes (from decoder)
 * row_map[index] = {11, -1, 1, 2, 3, 4, 12, 13, 14, 15, 5, 6, 7, 8, 9, 10}
 * index = ((hi << 1) & 0x0e) | ((lo >> 5) & 0x01)
 *
 * Reverse lookup: row (1-15) -> index
 */
static const int8_t row_to_index[16] = {
    -1, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15, 0, 6, 7, 8, 9
};

/**
 * Add odd parity bit to byte (CEA-608 uses odd parity)
 */
static inline uint8_t add_odd_parity(uint8_t byte)
{
    /* Clear bit 7, count set bits, set bit 7 if even parity needed */
    byte &= 0x7F;
    if (!av_parity(byte))
        byte |= 0x80;
    return byte;
}

/**
 * Emit a single 3-byte CC triplet
 */
static inline void emit_cc_data(uint8_t *buf, int *pos, uint8_t hi, uint8_t lo, int field)
{
    buf[(*pos)++] = field ? 0xFD : 0xFC;  /* cc_valid=1, cc_type=0 or 1 */
    buf[(*pos)++] = add_odd_parity(hi);
    buf[(*pos)++] = add_odd_parity(lo);
}

/**
 * Emit a control code (must be sent twice per CEA-608 spec)
 */
static inline void emit_control(uint8_t *buf, int *pos, uint8_t hi, uint8_t lo, int field)
{
    emit_cc_data(buf, pos, hi, lo, field);
    emit_cc_data(buf, pos, hi, lo, field);
}

/**
 * Generate PAC (Preamble Address Code) bytes for a given row
 * @param row Row number (1-15)
 * @param indent Indent in columns (0, 4, 8, 12, 16, 20, 24, 28)
 * @param hi Output: high byte
 * @param lo Output: low byte
 */
static void generate_pac(int row, int indent, uint8_t *hi, uint8_t *lo)
{
    int index;
    int style_indent;

    if (row < 1 || row > 15)
        row = 15;

    index = row_to_index[row];

    /* PAC hi byte: 0x10 + (index >> 1)
     * This gives 0x10-0x17 for indices 0-15
     * From decoder: index = ((hi << 1) & 0x0e) | ((lo >> 5) & 0x01)
     */
    *hi = 0x10 + (index >> 1);

    /* lo byte: 0x40 | ((index & 1) << 5) | style_indent
     * style_indent encodes color/underline + indent
     * For white text, no underline: use 0x40/0x60 base
     * With indent: 0x50 + (indent/4)*2 - gives 0x52, 0x54, 0x56, 0x58, 0x5a, 0x5c, 0x5e
     */
    style_indent = 0;  /* white, no underline, no indent */
    if (indent >= 4 && indent <= 28) {
        style_indent = 0x10 + ((indent / 4) * 2);
    }

    *lo = 0x40 | ((index & 1) << 5) | style_indent;
}

/**
 * Encode a single UTF-8 character to CEA-608
 * @param utf8 Input UTF-8 string (will be advanced past consumed bytes)
 * @param charset Output: charset identifier
 * @param code Output: character code
 * @return Number of bytes consumed, or 0 if character cannot be encoded
 */
static int encode_char(const char **utf8, int *charset, uint8_t *code)
{
    const uint8_t *p = (const uint8_t *)*utf8;
    uint32_t codepoint;
    int len;
    int set, c;

    if (!*p)
        return 0;

    /* Decode UTF-8 codepoint */
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

    /* Check if it's a basic ASCII character (0x20-0x7F, excluding overrides) */
    if (codepoint >= 0x20 && codepoint < 0x7F) {
        /* Check if this ASCII code has an override in basic charset */
        if (charset_overrides[CCSET_BASIC_AMERICAN][codepoint][0]) {
            /* This code is overridden - need to find it in extended sets */
            goto check_extended;
        }
        *charset = CCSET_BASIC_AMERICAN;
        *code = (uint8_t)codepoint;
        *utf8 += len;
        return len;
    }

check_extended:
    /* Search charset overrides for the UTF-8 sequence */
    for (set = 0; set < 4; set++) {
        for (c = 0x20; c < 0x80; c++) {
            if (charset_overrides[set][c][0]) {
                /* Compare UTF-8 strings */
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

    /* Character not found - try replacing with '?' or space */
    if (codepoint >= 0x20 && codepoint < 0x80) {
        *charset = CCSET_BASIC_AMERICAN;
        *code = (uint8_t)codepoint;
    } else {
        *charset = CCSET_BASIC_AMERICAN;
        *code = '?';  /* Replacement character */
    }
    *utf8 += len;
    return len;
}

/**
 * Strip ASS formatting tags from text
 */
static void strip_ass_tags(const char *input, AVBPrint *output)
{
    const char *p = input;

    while (*p) {
        if (*p == '{') {
            /* Skip to closing brace */
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

/**
 * Extract text content from ASS dialogue line
 */
static const char *extract_ass_text(const char *ass_line, AVBPrint *output)
{
    const char *p = ass_line;
    int field_count = 0;

    /* FFmpeg's internal ASS format: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
     * We need to skip 8 commas to get to the text field */
    while (*p && field_count < 8) {
        if (*p == ',')
            field_count++;
        p++;
    }

    if (field_count < 8)
        return NULL;

    av_bprint_init(output, 0, AV_BPRINT_SIZE_UNLIMITED);
    strip_ass_tags(p, output);

    return output->str;
}

/**
 * Encode a subtitle to CEA-608 triplets
 */
static int ccaption_encode_frame(AVCodecContext *avctx,
                                  unsigned char *buf, int bufsize,
                                  const AVSubtitle *sub)
{
    CCaptionEncContext *ctx = avctx->priv_data;
    int pos = 0;
    int i;
    uint8_t hi, lo;
    AVBPrint text;
    int line_num;
    int last_row;

    if (sub->num_rects == 0)
        return 0;

    av_bprint_init(&text, 0, AV_BPRINT_SIZE_UNLIMITED);

    /* Collect all text from rectangles */
    for (i = 0; i < sub->num_rects; i++) {
        const AVSubtitleRect *rect = sub->rects[i];
        if (rect->type == SUBTITLE_ASS && rect->ass) {
            AVBPrint stripped;
            if (extract_ass_text(rect->ass, &stripped)) {
                if (text.len > 0)
                    av_bprintf(&text, "\n");
                av_bprintf(&text, "%s", stripped.str);
                av_bprint_finalize(&stripped, NULL);
            }
        } else if (rect->type == SUBTITLE_TEXT && rect->text) {
            if (text.len > 0)
                av_bprintf(&text, "\n");
            av_bprintf(&text, "%s", rect->text);
        }
    }

    if (!av_bprint_is_complete(&text) || text.len == 0) {
        av_bprint_finalize(&text, NULL);
        return 0;
    }

    /* Emit mode control (RCL for pop-on, RU2/3/4 for roll-up) */
    if (ctx->roll_up >= 2 && ctx->roll_up <= 4) {
        /* Roll-up mode */
        emit_control(buf, &pos, 0x14, CC_RU2 + (ctx->roll_up - 2), ctx->data_field);
    } else {
        /* Pop-on mode - emit RCL (Resume Caption Loading) */
        emit_control(buf, &pos, 0x14, CC_RCL, ctx->data_field);
    }

    /* Process text line by line */
    const char *line_start = text.str;
    line_num = 0;

    /* Count lines first to position starting row */
    int num_lines = 1;
    for (const char *p = text.str; *p; p++) {
        if (*p == '\n') num_lines++;
    }

    /* Start from appropriate row (captions typically at bottom) */
    if (ctx->row > 0) {
        last_row = ctx->row;
    } else {
        last_row = 15 - num_lines + 1;
        if (last_row < 1) last_row = 1;
    }

    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len;
        const char *p;
        int current_row;
        int char_count = 0;

        if (!line_end)
            line_end = line_start + strlen(line_start);

        line_len = line_end - line_start;
        current_row = last_row + line_num;
        if (current_row > 15) current_row = 15;

        /* Emit PAC for this row */
        generate_pac(current_row, 0, &hi, &lo);
        emit_control(buf, &pos, hi, lo, ctx->data_field);

        /* Encode characters */
        p = line_start;
        while (p < line_end && char_count < CC_SCREEN_COLUMNS) {
            int charset;
            uint8_t code;
            int consumed;

            consumed = encode_char(&p, &charset, &code);
            if (consumed == 0) {
                p++;
                continue;
            }

            /* Check buffer space */
            if (pos + 6 > bufsize) {
                av_log(avctx, AV_LOG_WARNING, "Output buffer too small\n");
                break;
            }

            switch (charset) {
            case CCSET_BASIC_AMERICAN:
                /* Basic characters are sent directly */
                /* Try to pair with next character if also basic */
                if (p < line_end && char_count + 1 < CC_SCREEN_COLUMNS) {
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
                /* Single character */
                emit_cc_data(buf, &pos, code, 0x00, ctx->data_field);
                char_count++;
                break;

            case CCSET_SPECIAL_AMERICAN:
                /* Prefix 0x11, codes 0x30-0x3F */
                emit_cc_data(buf, &pos, 0x11, code, ctx->data_field);
                char_count++;
                break;

            case CCSET_EXTENDED_SPANISH_FRENCH_MISC:
                /* Prefix 0x12, codes 0x20-0x3F - replaces previous character */
                emit_cc_data(buf, &pos, 0x12, code, ctx->data_field);
                /* Extended chars replace previous char, so don't increment count */
                break;

            case CCSET_EXTENDED_PORTUGUESE_GERMAN_DANISH:
                /* Prefix 0x13, codes 0x20-0x3F - replaces previous character */
                emit_cc_data(buf, &pos, 0x13, code, ctx->data_field);
                /* Extended chars replace previous char, so don't increment count */
                break;
            }
        }

        /* Move to next line */
        if (ctx->roll_up >= 2) {
            /* In roll-up mode, emit CR between lines */
            if (*line_end == '\n' && *(line_end + 1)) {
                emit_control(buf, &pos, 0x14, CC_CR, ctx->data_field);
            }
        }

        line_num++;
        line_start = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    /* Emit EOC (End of Caption) for pop-on mode */
    if (ctx->roll_up == 0) {
        emit_control(buf, &pos, 0x14, CC_EOC, ctx->data_field);
    }

    av_bprint_finalize(&text, NULL);
    return pos;
}

static av_cold int ccaption_encode_init(AVCodecContext *avctx)
{
    CCaptionEncContext *ctx = avctx->priv_data;

    /* Validate options */
    if (ctx->roll_up != 0 && (ctx->roll_up < 2 || ctx->roll_up > 4)) {
        av_log(avctx, AV_LOG_ERROR, "roll_up must be 0 (pop-on) or 2-4\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

#define OFFSET(x) offsetof(CCaptionEncContext, x)
#define SE AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "data_field", "select data field", OFFSET(data_field), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, SE, .unit = "data_field" },
    {   "first",  "use first field (CC1/CC2)",  0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, SE, .unit = "data_field" },
    {   "second", "use second field (CC3/CC4)", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, SE, .unit = "data_field" },
    { "roll_up", "roll-up mode (0=pop-on, 2-4=roll-up rows)", OFFSET(roll_up), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 4, SE },
    { "row", "starting row (1-15, 0=auto)", OFFSET(row), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 15, SE },
    { NULL },
};

static const AVClass ccaption_enc_class = {
    .class_name = "Closed Captions (EIA-608) Encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_ccaption_encoder = {
    .p.name         = "cc_enc",
    CODEC_LONG_NAME("Closed Captions (EIA-608)"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_EIA_608,
    .p.priv_class   = &ccaption_enc_class,
    .priv_data_size = sizeof(CCaptionEncContext),
    .init           = ccaption_encode_init,
    FF_CODEC_ENCODE_SUB_CB(ccaption_encode_frame),
};
