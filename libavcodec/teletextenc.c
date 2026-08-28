/*
 * Teletext encoder for VANC output
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
 * Teletext encoder for converting subtitles to WST (World System Teletext)
 * packets suitable for VANC insertion via OP47/SMPTE RDD-8.
 */

#include "config_components.h"

#include <string.h>
#include <ctype.h>

#include "avcodec.h"
#include "codec_internal.h"
#include "teletextenc.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"

/**
 * Hamming 8/4 encoding table
 * Each 4-bit value is encoded to 8 bits with Hamming protection
 */
const uint8_t ff_teletext_ham84_encode[16] = {
    0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
    0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA
};

/**
 * Odd parity lookup table for faster encoding
 */
static const uint8_t odd_parity_table[128] = {
    0x80, 0x01, 0x02, 0x83, 0x04, 0x85, 0x86, 0x07,
    0x08, 0x89, 0x8A, 0x0B, 0x8C, 0x0D, 0x0E, 0x8F,
    0x10, 0x91, 0x92, 0x13, 0x94, 0x15, 0x16, 0x97,
    0x98, 0x19, 0x1A, 0x9B, 0x1C, 0x9D, 0x9E, 0x1F,
    0x20, 0xA1, 0xA2, 0x23, 0xA4, 0x25, 0x26, 0xA7,
    0xA8, 0x29, 0x2A, 0xAB, 0x2C, 0xAD, 0xAE, 0x2F,
    0xB0, 0x31, 0x32, 0xB3, 0x34, 0xB5, 0xB6, 0x37,
    0x38, 0xB9, 0xBA, 0x3B, 0xBC, 0x3D, 0x3E, 0xBF,
    0x40, 0xC1, 0xC2, 0x43, 0xC4, 0x45, 0x46, 0xC7,
    0xC8, 0x49, 0x4A, 0xCB, 0x4C, 0xCD, 0xCE, 0x4F,
    0xD0, 0x51, 0x52, 0xD3, 0x54, 0xD5, 0xD6, 0x57,
    0x58, 0xD9, 0xDA, 0x5B, 0xDC, 0x5D, 0x5E, 0xDF,
    0xE0, 0x61, 0x62, 0xE3, 0x64, 0xE5, 0xE6, 0x67,
    0x68, 0xE9, 0xEA, 0x6B, 0xEC, 0x6D, 0x6E, 0xEF,
    0x70, 0xF1, 0xF2, 0x73, 0xF4, 0x75, 0x76, 0xF7,
    0xF8, 0x79, 0x7A, 0xFB, 0x7C, 0xFD, 0xFE, 0x7F,
};

uint8_t ff_teletext_ham84(uint8_t nibble)
{
    return ff_teletext_ham84_encode[nibble & 0x0F];
}

uint8_t ff_teletext_odd_parity(uint8_t c)
{
    return odd_parity_table[c & 0x7F];
}

typedef struct TeletextEncContext {
    AVClass *class;
    int page;             /* Page number (100-899) */
    int magazine;         /* Magazine number (1-8) */
    int region;           /* Character set region (0-15) */
    int double_height;    /* Use double height text (OP-42 4d) */
    int erase_page;       /* C4: Erase page flag (OP-42 4h) */
    int update_indicator; /* C8: Update indicator flag (OP-42 4g) */
    int subtitle_flag;    /* C6: Subtitle indicator flag (OP-42 4f) */

    /* Internal state */
    uint16_t page_bcd;  /* Page number in BCD */
    int mag_encoded;    /* Magazine encoded (0-7) */
    uint8_t page_buffer[TELETEXT_ROWS][TELETEXT_COLS];  /* Current page content */
    uint8_t prev_page_buffer[TELETEXT_ROWS][TELETEXT_COLS];  /* Last transmitted content */
    int erase_this_frame;  /* C4 asserted this frame due to a content change */
    int sequence_num;   /* Packet sequence counter */
} TeletextEncContext;

/**
 * Map color name to teletext color code
 */
static int parse_color_name(const char *name)
{
    if (!name)
        return TELETEXT_ALPHA_WHITE;

    if (!av_strcasecmp(name, "red"))
        return TELETEXT_ALPHA_RED;
    if (!av_strcasecmp(name, "green"))
        return TELETEXT_ALPHA_GREEN;
    if (!av_strcasecmp(name, "yellow"))
        return TELETEXT_ALPHA_YELLOW;
    if (!av_strcasecmp(name, "blue"))
        return TELETEXT_ALPHA_BLUE;
    if (!av_strcasecmp(name, "magenta"))
        return TELETEXT_ALPHA_MAGENTA;
    if (!av_strcasecmp(name, "cyan"))
        return TELETEXT_ALPHA_CYAN;
    if (!av_strcasecmp(name, "white"))
        return TELETEXT_ALPHA_WHITE;
    if (!av_strcasecmp(name, "black"))
        return TELETEXT_ALPHA_BLACK;

    return TELETEXT_ALPHA_WHITE;
}

/**
 * Map hex color to nearest teletext color
 */
static int hex_to_teletext_color(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;

    /* Simple threshold-based mapping */
    int r_bit = r > 127 ? 1 : 0;
    int g_bit = g > 127 ? 1 : 0;
    int b_bit = b > 127 ? 1 : 0;

    /* Map to teletext color (RGB bits map to color codes) */
    return (r_bit << 0) | (g_bit << 1) | (b_bit << 2);
}

/**
 * Strip HTML/SRT tags from text and extract formatting
 */
static int strip_tags_and_format(TeletextEncContext *ctx, const char *input,
                                  uint8_t *output, int max_len,
                                  int *out_color)
{
    const char *p = input;
    int pos = 0;
    int current_color = TELETEXT_ALPHA_WHITE;
    int in_tag = 0;
    char tag_buf[64];
    int tag_pos = 0;

    *out_color = current_color;

    while (*p && pos < max_len - 1) {
        if (*p == '<') {
            in_tag = 1;
            tag_pos = 0;
            p++;
            continue;
        }

        if (in_tag) {
            if (*p == '>') {
                tag_buf[tag_pos] = '\0';
                in_tag = 0;

                /* Parse font color tag */
                if (av_strstart(tag_buf, "font color=\"#", NULL) ||
                    av_strstart(tag_buf, "font color='#", NULL)) {
                    const char *hex = strchr(tag_buf, '#');
                    if (hex) {
                        uint32_t color = strtoul(hex + 1, NULL, 16);
                        current_color = hex_to_teletext_color(color);
                        if (pos == 0)
                            *out_color = current_color;
                    }
                } else if (av_strstart(tag_buf, "font color=\"", NULL) ||
                           av_strstart(tag_buf, "font color='", NULL)) {
                    const char *start = strchr(tag_buf, '"');
                    if (!start)
                        start = strchr(tag_buf, '\'');
                    if (start) {
                        start++;
                        char color_name[32];
                        int i = 0;
                        while (*start && *start != '"' && *start != '\'' && i < 31)
                            color_name[i++] = *start++;
                        color_name[i] = '\0';
                        current_color = parse_color_name(color_name);
                        if (pos == 0)
                            *out_color = current_color;
                    }
                }
                /* Ignore other tags like <b>, <i>, </font>, etc. */

                p++;
                continue;
            }

            if (tag_pos < sizeof(tag_buf) - 1)
                tag_buf[tag_pos++] = *p;
            p++;
            continue;
        }

        /* Handle ASS newline escape sequences: \N and \n */
        if (*p == '\\' && (*(p + 1) == 'N' || *(p + 1) == 'n')) {
            output[pos++] = '\n';
            p += 2;
            continue;
        }

        /* Handle HTML entities */
        if (*p == '&') {
            if (av_strstart(p, "&lt;", NULL)) {
                output[pos++] = '<';
                p += 4;
                continue;
            } else if (av_strstart(p, "&gt;", NULL)) {
                output[pos++] = '>';
                p += 4;
                continue;
            } else if (av_strstart(p, "&amp;", NULL)) {
                output[pos++] = '&';
                p += 5;
                continue;
            } else if (av_strstart(p, "&nbsp;", NULL)) {
                output[pos++] = ' ';
                p += 6;
                continue;
            }
        }

        /* Copy character, converting to teletext-safe ASCII */
        unsigned char c = *p;
        if (c >= 0x20 && c <= 0x7E) {
            output[pos++] = c;
        } else if (c == '\n' || c == '\r') {
            output[pos++] = '\n';
        } else if (c >= 0x80) {
            /* Skip multi-byte UTF-8 sequences, replace with space */
            if ((c & 0xE0) == 0xC0) {
                p++;  /* 2-byte sequence */
                output[pos++] = ' ';
            } else if ((c & 0xF0) == 0xE0) {
                p += 2;  /* 3-byte sequence */
                output[pos++] = ' ';
            } else if ((c & 0xF8) == 0xF0) {
                p += 3;  /* 4-byte sequence */
                output[pos++] = ' ';
            } else {
                output[pos++] = ' ';
            }
        } else {
            output[pos++] = ' ';
        }
        p++;
    }

    output[pos] = '\0';
    return pos;
}

/**
 * Clear the page buffer
 */
static void clear_page_buffer(TeletextEncContext *ctx)
{
    memset(ctx->page_buffer, ' ', sizeof(ctx->page_buffer));
}

/**
 * Write text to page buffer at specified row
 * Optionally applies double height formatting per OP-42 4d
 */
static void write_to_page(TeletextEncContext *ctx, int row, int col,
                          const uint8_t *text, int len, int color)
{
    if (row < 0 || row >= TELETEXT_ROWS)
        return;
    if (col < 0)
        col = 0;

    /* Insert double height code if enabled (OP-42 4d) */
    if (ctx->double_height && col < TELETEXT_COLS) {
        ctx->page_buffer[row][col++] = TELETEXT_DOUBLE_HEIGHT;
    }

    /* Insert color code at start if not white */
    if (color != TELETEXT_ALPHA_WHITE && col < TELETEXT_COLS) {
        ctx->page_buffer[row][col++] = color;
    }

    /* Copy text */
    for (int i = 0; i < len && col < TELETEXT_COLS; i++) {
        if (text[i] == '\n')
            break;
        ctx->page_buffer[row][col++] = text[i];
    }
}

/**
 * Encode MRAG (Magazine and Row Address Group) per ETS 300 706 s7.1.2
 *
 * The address is two Hamming 8/4 code words carrying 8 message bits:
 * magazine (3 bits) and packet/row address (5 bits). The first word holds
 * the magazine in its low three data bits and row bit 0 as its top data bit;
 * the second word holds row bits 1-4:
 *   Byte1 nibble = M0 M1 M2 R0        Byte2 nibble = R1 R2 R3 R4
 *
 * This matches the decoder in decklink_enc.cpp (log_teletext_packet), which
 * reads magazine from byte4 bits 0-2, row bit 0 from byte4 bit 3, and row
 * bits 1-4 from byte5.
 */
static void encode_mrag(int mag, int row, uint8_t *byte1, uint8_t *byte2)
{
    int m0 = (mag >> 0) & 1;
    int m1 = (mag >> 1) & 1;
    int m2 = (mag >> 2) & 1;
    int r0 = (row >> 0) & 1;
    int r1 = (row >> 1) & 1;
    int r2 = (row >> 2) & 1;
    int r3 = (row >> 3) & 1;
    int r4 = (row >> 4) & 1;

    *byte1 = ff_teletext_ham84(m0 | (m1 << 1) | (m2 << 2) | (r0 << 3));
    *byte2 = ff_teletext_ham84(r1 | (r2 << 1) | (r3 << 2) | (r4 << 3));
}

/**
 * Build a teletext header row (row 0) per ETS 300 706
 *
 * Page header structure (42 bytes total) per ETS 300 706 Table 3. The
 * subcode is 13 bits split S1(4) S2(3) S3(4) S4(2), with the control bits
 * C4-C14 packed alongside S2/S4 and in the two following code words:
 *   Bytes 0-1:   MRAG (Hamming 8/4)
 *   Bytes 2-3:   Page units, page tens (Hamming 8/4)
 *   Byte 4:      S1 (bits 0-3) (Hamming 8/4)
 *   Byte 5:      S2 (bits 0-2) + C4 erase page (bit 3) (Hamming 8/4)
 *   Byte 6:      S3 (bits 0-3) (Hamming 8/4)
 *   Byte 7:      S4 (bits 0-1) + C5 newsflash (bit 2) + C6 subtitle (bit 3)
 *   Byte 8:      C7 suppress header (bit 0) + C8 update (bit 1) + C9 (bit 2) + C10 (bit 3)
 *   Byte 9:      C11 magazine serial (bit 0) + C12-C14 national charset (bits 1-3)
 *   Bytes 10-41: 32 characters page header display (odd parity)
 *
 * Control bits per OP-42:
 *   C4 = Erase Page (set to 1 between caption transmissions)
 *   C5 = Newsflash (0)
 *   C6 = Subtitle indicator (1 for closed captions)
 *   C7 = Suppress Header (0)
 *   C8 = Update Indicator (1 for closed captions)
 *   C9 = Interrupted Sequence (0)
 *   C10 = Inhibit Display (0)
 *   C11 = Magazine Serial (0 for parallel mode per OP-42 4e)
 *   C12-C14 = National Option Character Subset (region)
 */
static void build_header_row(TeletextEncContext *ctx, uint8_t *line)
{
    /* Bytes 0-1: Magazine and packet address (row 0 = page header) */
    encode_mrag(ctx->mag_encoded, 0, &line[0], &line[1]);

    /* Bytes 2-3: Page number units and tens */
    line[2] = ff_teletext_ham84(ctx->page_bcd & 0x0F);
    line[3] = ff_teletext_ham84((ctx->page_bcd >> 4) & 0x0F);

    /* Byte 4: S1 (subcode bits 0-3) */
    line[4] = ff_teletext_ham84(0);

    /* Byte 5: S2 (bits 0-2) + C4 erase page (bit 3).
     * C4 is asserted either by the erase_page option or automatically on the
     * header of a frame where the caption content changed (OP-42 4h) so the
     * decoder clears the previous page before painting the new rows. */
    line[5] = ff_teletext_ham84((ctx->erase_page || ctx->erase_this_frame) ? 0x08 : 0);

    /* Byte 6: S3 (subcode bits 0-3) */
    line[6] = ff_teletext_ham84(0);

    /* Byte 7: S4 (bits 0-1) + C5 newsflash (bit 2) + C6 subtitle (bit 3) per
     * EN 300 706 s9.3.1. S4 is a 2-bit subcode field, so C6 is the top data bit. */
    line[7] = ff_teletext_ham84(ctx->subtitle_flag ? 0x08 : 0);  /* C6 at bit 3 */

    /* Byte 8: C7 suppress header (bit 0) + C8 update (bit 1) + C9 interrupted (bit 2) + C10 inhibit (bit 3) */
    line[8] = ff_teletext_ham84(ctx->update_indicator ? 0x02 : 0);  /* C8 at bit 1 */

    /* Byte 9: C11 magazine serial (bit 0) + C12-C14 national option charset (bits 1-3)
     * C11 = 0 for parallel mode per OP-42 4e */
    line[9] = ff_teletext_ham84((ctx->region & 0x07) << 1);

    /* Bytes 10-41: 32 characters page header display (odd parity) */
    for (int i = 0; i < 32; i++) {
        line[10 + i] = ff_teletext_odd_parity(' ');
    }
}

/**
 * Build a teletext content row
 */
static void build_content_row(TeletextEncContext *ctx, uint8_t *line, int row)
{
    /* Bytes 0-1: Magazine and row address (interleaved per ITU-R BT.653-3) */
    encode_mrag(ctx->mag_encoded, row, &line[0], &line[1]);

    /* Bytes 2-41: 40 characters with odd parity */
    for (int i = 0; i < TELETEXT_COLS; i++) {
        uint8_t c = ctx->page_buffer[row][i];
        if (c < 0x20)
            line[2 + i] = ff_teletext_odd_parity(c);  /* Control code */
        else
            line[2 + i] = ff_teletext_odd_parity(c);
    }
}

int ff_teletext_build_data_unit(uint8_t *out, int data_unit_id,
                                int field_parity, int line_offset,
                                int magazine, int packet_number,
                                const uint8_t *data)
{
    /* Data unit header */
    out[0] = data_unit_id;
    out[1] = 0x2C;  /* data_unit_length = 44 */

    /* Field parity and line offset */
    out[2] = ((field_parity & 1) << 5) | (line_offset & 0x1F);

    /* Framing code */
    out[3] = 0xE4;

    /* Copy 42 bytes of teletext data */
    memcpy(out + 4, data, TELETEXT_LINE_SIZE);

    /* Log the full output data unit for debugging */
    av_log(NULL, AV_LOG_INFO,
           "Teletext encoder output (mag=%d row=%d):\n", magazine, packet_number);
    av_log(NULL, AV_LOG_INFO,
           "  header: %02x %02x %02x %02x\n",
           out[0], out[1], out[2], out[3]);
    av_log(NULL, AV_LOG_INFO,
           "  MRAG + data bytes 0-19: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           out[4], out[5], out[6], out[7], out[8], out[9], out[10], out[11],
           out[12], out[13], out[14], out[15], out[16], out[17], out[18], out[19],
           out[20], out[21], out[22], out[23]);
    av_log(NULL, AV_LOG_INFO,
           "  data bytes 20-41:       %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
           out[24], out[25], out[26], out[27], out[28], out[29], out[30], out[31],
           out[32], out[33], out[34], out[35], out[36], out[37], out[38], out[39],
           out[40], out[41], out[42], out[43], out[44], out[45]);

    return TELETEXT_DATA_UNIT_SIZE;
}

static av_cold int teletext_encode_init(AVCodecContext *avctx)
{
    TeletextEncContext *ctx = avctx->priv_data;

    /* Validate page number */
    if (ctx->page < 100 || ctx->page > 899) {
        av_log(avctx, AV_LOG_ERROR, "Invalid teletext page number %d (must be 100-899)\n", ctx->page);
        return AVERROR(EINVAL);
    }

    /* Validate magazine */
    if (ctx->magazine < 1 || ctx->magazine > 8) {
        av_log(avctx, AV_LOG_ERROR, "Invalid magazine number %d (must be 1-8)\n", ctx->magazine);
        return AVERROR(EINVAL);
    }

    /* Convert page to BCD (just the last two digits) */
    int page_units = ctx->page % 10;
    int page_tens = (ctx->page / 10) % 10;
    ctx->page_bcd = (page_tens << 4) | page_units;

    /* Magazine 8 is encoded as 0 */
    ctx->mag_encoded = (ctx->magazine == 8) ? 0 : ctx->magazine;

    ctx->sequence_num = 0;
    clear_page_buffer(ctx);
    /* Seed the "previously transmitted" page as blank so a content change is
     * detected the first time a real caption is encoded. */
    memset(ctx->prev_page_buffer, ' ', sizeof(ctx->prev_page_buffer));
    ctx->erase_this_frame = 0;

    av_log(avctx, AV_LOG_INFO, "Teletext encoder initialized: page %d, magazine %d, region %d\n",
           ctx->page, ctx->magazine, ctx->region);

    return 0;
}

static int teletext_encode_frame(AVCodecContext *avctx, uint8_t *buf,
                                  int buf_size, const AVSubtitle *sub)
{
    TeletextEncContext *ctx = avctx->priv_data;
    int total_size = 0;
    uint8_t line_data[TELETEXT_LINE_SIZE];

    /* Clear page for new subtitle */
    clear_page_buffer(ctx);

    if (!sub || sub->num_rects == 0) {
        /* Empty subtitle - generate clear page. Assert C4 if this clears
         * content that was previously on screen. */
        ctx->erase_this_frame = memcmp(ctx->page_buffer, ctx->prev_page_buffer,
                                       sizeof(ctx->page_buffer)) != 0;
        memcpy(ctx->prev_page_buffer, ctx->page_buffer, sizeof(ctx->page_buffer));

        /* Build header row */
        build_header_row(ctx, line_data);

        if (buf_size < TELETEXT_DATA_UNIT_SIZE)
            return AVERROR_BUFFER_TOO_SMALL;

        ff_teletext_build_data_unit(buf, TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE,
                                    0, 7, ctx->mag_encoded, 0, line_data);
        return TELETEXT_DATA_UNIT_SIZE;
    }

    /* Process subtitle rectangles */
    int current_row = 20;  /* Start near bottom of teletext page */

    for (unsigned i = 0; i < sub->num_rects && current_row < TELETEXT_ROWS; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        const char *text = NULL;

        if (rect->type == SUBTITLE_TEXT) {
            text = rect->text;
        } else if (rect->type == SUBTITLE_ASS) {
            text = rect->ass;
            /* Skip ASS header if present.
             * FFmpeg's SRT decoder outputs ASS format:
             * ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
             * That's 8 commas before the actual text content.
             */
            const char *p = text;
            int commas = 0;
            while (*p && commas < 8) {
                if (*p == ',')
                    commas++;
                p++;
            }
            if (commas == 8)
                text = p;
        }

        if (!text || !*text)
            continue;

        av_log(avctx, AV_LOG_DEBUG, "Teletext: raw input text: \"%s\"\n", text);

        /* Parse text and strip tags */
        uint8_t clean_text[256];
        int color;
        int len = strip_tags_and_format(ctx, text, clean_text, sizeof(clean_text), &color);

        av_log(avctx, AV_LOG_DEBUG, "Teletext: cleaned text (%d chars): \"%s\"\n", len, clean_text);

        /* Split text by newlines and write each line */
        const uint8_t *line_start = clean_text;
        const uint8_t *p = clean_text;
        int line_count = 0;

        while (*p && current_row < TELETEXT_ROWS) {
            if (*p == '\n' || *(p + 1) == '\0') {
                int line_len = (*p == '\n') ? (p - line_start) : (p - line_start + 1);
                if (line_len > 0) {
                    /* Center the text */
                    int col = (TELETEXT_COLS - line_len) / 2;
                    if (col < 1) col = 1;

                    /* Log the line being written */
                    char debug_line[41];
                    int copy_len = line_len < 40 ? line_len : 40;
                    memcpy(debug_line, line_start, copy_len);
                    debug_line[copy_len] = '\0';
                    av_log(avctx, AV_LOG_DEBUG, "Teletext: writing line %d to row %d col %d: \"%s\"\n",
                           line_count, current_row, col, debug_line);

                    write_to_page(ctx, current_row, col, line_start, line_len, color);
                    current_row++;
                    line_count++;
                }
                line_start = p + 1;
            }
            p++;
        }

        av_log(avctx, AV_LOG_DEBUG, "Teletext: wrote %d lines total\n", line_count);
    }

    /* Assert C4 (erase page) on this header if the visible content differs
     * from what we last transmitted, so the decoder clears the old caption
     * before painting the new rows (OP-42 4h). */
    ctx->erase_this_frame = memcmp(ctx->page_buffer, ctx->prev_page_buffer,
                                   sizeof(ctx->page_buffer)) != 0;
    memcpy(ctx->prev_page_buffer, ctx->page_buffer, sizeof(ctx->page_buffer));

    /* Generate teletext packets */
    /* Header row (row 0) */
    build_header_row(ctx, line_data);
    if (total_size + TELETEXT_DATA_UNIT_SIZE > buf_size)
        return AVERROR_BUFFER_TOO_SMALL;

    ff_teletext_build_data_unit(buf + total_size, TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE,
                                0, 7, ctx->mag_encoded, 0, line_data);
    total_size += TELETEXT_DATA_UNIT_SIZE;

    /* Content rows - only emit rows that actually carry text. Blank rows are
     * not transmitted: the C4 erase-page bit clears stale content, so sending
     * empty R22/R23 every cycle would only waste VBI bandwidth. */
    for (int row = 19; row < TELETEXT_ROWS && row <= 23; row++) {
        /* Check if row has content */
        int has_content = 0;
        for (int col = 0; col < TELETEXT_COLS; col++) {
            if (ctx->page_buffer[row][col] != ' ') {
                has_content = 1;
                break;
            }
        }

        if (has_content) {
            build_content_row(ctx, line_data, row);
            if (total_size + TELETEXT_DATA_UNIT_SIZE > buf_size)
                return AVERROR_BUFFER_TOO_SMALL;

            ff_teletext_build_data_unit(buf + total_size, TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE,
                                        0, 7 + row, ctx->mag_encoded, row, line_data);
            total_size += TELETEXT_DATA_UNIT_SIZE;
        }
    }

    ctx->sequence_num++;
    return total_size;
}

static av_cold int teletext_encode_close(AVCodecContext *avctx)
{
    return 0;
}

#define OFFSET(x) offsetof(TeletextEncContext, x)
#define VE AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption teletext_options[] = {
    { "teletext_page", "Teletext page number (100-899)", OFFSET(page), AV_OPT_TYPE_INT, { .i64 = 888 }, 100, 899, VE },
    { "page", "Teletext page number (100-899)", OFFSET(page), AV_OPT_TYPE_INT, { .i64 = 888 }, 100, 899, VE },
    { "magazine", "Magazine number (1-8)", OFFSET(magazine), AV_OPT_TYPE_INT, { .i64 = 8 }, 1, 8, VE },
    { "region", "Character set region (0-15)", OFFSET(region), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 15, VE },
    { "double_height", "Use double height text (OP-42 4d)", OFFSET(double_height), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "erase_page", "C4: Erase page between transmissions (OP-42 4h)", OFFSET(erase_page), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },
    { "update_indicator", "C8: Update indicator flag (OP-42 4g)", OFFSET(update_indicator), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "subtitle_flag", "C6: Subtitle indicator flag (OP-42 4f)", OFFSET(subtitle_flag), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { NULL }
};

static const AVClass teletext_enc_class = {
    .class_name = "teletext encoder",
    .item_name  = av_default_item_name,
    .option     = teletext_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

#if CONFIG_TELETEXT_ENCODER
const FFCodec ff_teletext_encoder = {
    .p.name         = "teletext",
    CODEC_LONG_NAME("Teletext subtitle encoder"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_DVB_TELETEXT,
    .priv_data_size = sizeof(TeletextEncContext),
    .p.priv_class   = &teletext_enc_class,
    .init           = teletext_encode_init,
    FF_CODEC_ENCODE_SUB_CB(teletext_encode_frame),
    .close          = teletext_encode_close,
};
#endif
