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
    int wrap_text;        /* Word-wrap lines wider than the row instead of truncating */
    int translit;         /* Map non-G0 chars ([]->(), note->#, entities, accents) to G0-safe glyphs */
    int note_x26;         /* Emit a real G2 music note via Packet X/26 (Polistream method) vs # fallback */

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

/* Internal placeholder byte for a music note in the page buffer. It is out of
 * the 0x00-0x7F teletext code range so it can never collide with real content;
 * it is resolved just before transmission into either a space (with a real G2
 * note overlaid via Packet X/26) or the '#' fallback glyph. */
#define TELETEXT_NOTE_MARK 0xFF

/**
 * Transliterate a Unicode code point to teletext-safe ASCII.
 *
 * The teletext G0 (primary) set has no accented letters or "smart" punctuation,
 * so the common Unicode characters that appear in real subtitle files are
 * mapped to their nearest ASCII equivalent instead of being dropped: smart
 * quotes -> ' / ", en/em dashes -> -, ellipsis -> ..., accented Latin letters
 * -> their base letter. Proper accented glyphs (e.g. e-acute rendered as such)
 * would require Packet X/26 G2 diacritic composition, which is not implemented.
 * Returns a short ASCII string; unknown code points return a single space.
 */
static const char *teletext_translit(unsigned cp)
{
    switch (cp) {
    /* Punctuation */
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x00A0: case 0x2007: case 0x2009: case 0x200A: case 0x202F: return " ";
    /* Music notes (U+266A eighth note, U+266B beamed notes) frame song lyrics
     * in captions. Teletext G0 has no note glyph; a real note lives in the G2
     * supplementary set and is placed via a Packet X/26 enhancement (see
     * build_x26_row) - the Polistream on-air method. Emit a placeholder here;
     * it is resolved at transmission time into a space (with the G2 note
     * overlaid) or, when X/26 is disabled, the '#' fallback. */
    case 0x266A: case 0x266B: case 0x2669: case 0x266C: return "\xff";
    /* Accented Latin -> base letter */
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return "A";
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return "a";
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return "E";
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return "e";
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return "I";
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return "i";
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: return "O";
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: return "o";
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return "U";
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return "u";
    case 0x00D1: return "N"; case 0x00F1: return "n";
    case 0x00C7: return "C"; case 0x00E7: return "c";
    case 0x00DD: return "Y"; case 0x00FD: case 0x00FF: return "y";
    case 0x00DF: return "ss";
    case 0x00C6: return "AE"; case 0x00E6: return "ae";
    case 0x0152: return "OE"; case 0x0153: return "oe";
    default: return " ";
    }
}

/**
 * Append one Unicode code point to the output buffer as G0-safe byte(s).
 *
 * With translit enabled the code point is folded to teletext-displayable
 * glyphs: printable ASCII passes through (with the two square brackets, which
 * have no G0 glyph, mapped to parentheses); everything else goes through
 * teletext_translit(). With translit disabled only plain ASCII is emitted
 * verbatim (brackets included, i.e. as their raw 0x5B/0x5D) and any non-ASCII
 * code point is dropped to a space rather than leaking raw UTF-8 bytes.
 */
static int append_codepoint(uint8_t *output, int pos, int max_len,
                            unsigned cp, int translit)
{
    if (cp >= 0x20 && cp <= 0x7E) {
        uint8_t b = cp;
        if (translit) {
            /* 0x5B/0x5D are National Option Character Subset positions with no
             * bracket glyph (the English subset shows them as left/right
             * arrows). Parentheses live at 0x28/0x29, which are fixed in every
             * subset, so "[soft music]" reads as "(soft music)". */
            if (b == '[')      b = '(';
            else if (b == ']') b = ')';
        }
        if (pos < max_len - 1)
            output[pos++] = b;
    } else if (translit) {
        const char *rep = teletext_translit(cp);
        while (*rep && pos < max_len - 1)
            output[pos++] = *rep++;
    } else if (pos < max_len - 1) {
        output[pos++] = ' ';
    }
    return pos;
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
            } else if (p[1] == '#') {
                /* Numeric character reference: &#DDDD; (decimal) or
                 * &#xHHHH; (hex). Common in captions for the music note
                 * (&#9834;) and similar; decode to a code point and fold it
                 * to a G0-safe glyph rather than leaking "&#9834;" as text. */
                const char *q = p + 2;
                unsigned cp = 0;
                int base = 10;
                if (*q == 'x' || *q == 'X') { base = 16; q++; }
                const char *digits = q;
                while (*q && *q != ';') q++;
                if (*q == ';' && q > digits) {
                    cp = (unsigned)strtoul(digits, NULL, base);
                    pos = append_codepoint(output, pos, max_len, cp, ctx->translit);
                    p = q + 1;
                    continue;
                }
            }
        }

        /* Copy character, converting to teletext-safe ASCII */
        unsigned char c = *p;
        if (c >= 0x20 && c <= 0x7E) {
            pos = append_codepoint(output, pos, max_len, c, ctx->translit);
        } else if (c == '\n' || c == '\r') {
            output[pos++] = '\n';
        } else if (c >= 0x80) {
            /* Decode the UTF-8 code point and (when translit is on) fold it to
             * teletext-safe ASCII (smart quotes, dashes, ellipsis, accented
             * Latin, music notes), rather than dropping every non-ASCII
             * character to a space or leaking raw UTF-8 bytes. */
            unsigned cp;
            int nb;
            if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; nb = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; nb = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; nb = 4; }
            else                         { cp = 0;        nb = 1; }
            for (int k = 1; k < nb; k++) {
                if ((p[k] & 0xC0) != 0x80) { nb = k; break; }  /* truncated */
                cp = (cp << 6) | (p[k] & 0x3F);
            }
            pos = append_codepoint(output, pos, max_len, cp, ctx->translit);
            p += (nb - 1);  /* the trailing p++ consumes the final byte */
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

    /* Start Box, transmitted twice.
     *
     * Per ETS 300 706 s12.2, when the Subtitle (C6) or Newsflash bit is set a
     * decoder displays ONLY the characters between a Start Box and an End Box,
     * overlaid on the video. Without these codes the page is received but no
     * text is rendered (the symptom we saw: "WST Page 801" detected, nothing
     * shown). The codes are set-after spacing attributes and are sent in pairs
     * for error resilience (OP-42 "StartBox Characters"). */
    if (col < TELETEXT_COLS)
        ctx->page_buffer[row][col++] = TELETEXT_START_BOX;
    if (col < TELETEXT_COLS)
        ctx->page_buffer[row][col++] = TELETEXT_START_BOX;

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

    /* End Box, transmitted twice, to close the subtitle box after the text. */
    if (col < TELETEXT_COLS)
        ctx->page_buffer[row][col++] = TELETEXT_END_BOX;
    if (col < TELETEXT_COLS)
        ctx->page_buffer[row][col++] = TELETEXT_END_BOX;
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
 *   C7 = Suppress Header (1 for subtitles: hide the page header row)
 *   C8 = Update Indicator (1 for closed captions)
 *   C9 = Interrupted Sequence (1 for subtitles: overlay as a live subtitle)
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

    /* Byte 8: C7 suppress header (bit 0) + C8 update (bit 1) + C9 interrupted (bit 2) + C10 inhibit (bit 3).
     * For subtitles the receiver must be told to overlay the page as a live
     * subtitle rather than treat it as a normal navigable page: that needs
     * C9 (interrupted sequence) and C7 (suppress header). Setting only C8
     * (byte8=0x49) left an STB in caption mode ignoring the page entirely.
     * A Polistream reference capture sets C7=C8=C9=1 (byte8=0x2f); match it. */
    {
        int c8bits = ctx->update_indicator ? 0x02 : 0;   /* C8 update */
        if (ctx->subtitle_flag)
            c8bits |= 0x01 | 0x04;                        /* C7 suppress header + C9 interrupted sequence */
        line[8] = ff_teletext_ham84(c8bits);
    }

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

/**
 * Hamming 24/18 encode one teletext triplet (ETS 300 706 s8.3).
 *
 * Packs an 18-bit value - address (6 bits), mode (5 bits), data (7 bits) - into
 * a 3-byte triplet with five Hamming protection bits plus an overall parity bit,
 * all using ODD parity. Validated byte-exact against Polistream's on-air X/26
 * packets (e.g. addr=20,mode=0x0F,data=0x55 -> 28 bd d5).
 */
static void teletext_ham24(int addr, int mode, int data, uint8_t out[3])
{
    /* Data-bit transmit positions (1-indexed): P at 1,2,4,8,16,24; data elsewhere. */
    static const int dpos[18] = { 3,5,6,7,9,10,11,12,13,14,15,17,18,19,20,21,22,23 };
    unsigned D = (addr & 0x3F) | ((mode & 0x1F) << 6) | ((data & 0x7F) << 11);
    uint8_t bit[25] = { 0 };  /* 1-indexed transmitted bits */

    for (int k = 0; k < 18; k++)
        bit[dpos[k]] = (D >> k) & 1;

    /* Hamming parity bits at 1,2,4,8,16: odd parity over the data bits they cover. */
    static const int pmask[5] = { 1, 2, 4, 8, 16 };
    for (int pi = 0; pi < 5; pi++) {
        int s = 0;
        for (int j = 1; j <= 24; j++) {
            if (j == 1 || j == 2 || j == 4 || j == 8 || j == 16 || j == 24)
                continue;  /* skip the parity positions themselves */
            if (j & pmask[pi])
                s ^= bit[j];
        }
        bit[pmask[pi]] = s ^ 1;
    }
    /* Overall parity at position 24 makes the whole 24-bit word odd parity. */
    {
        int s = 0;
        for (int j = 1; j <= 23; j++)
            s ^= bit[j];
        bit[24] = s ^ 1;
    }

    out[0] = out[1] = out[2] = 0;
    for (int p = 1; p <= 24; p++)
        if (bit[p])
            out[(p - 1) / 8] |= 1 << ((p - 1) % 8);
}

/**
 * Build a Packet X/26 enhancement row (42 bytes) that overlays real music-note
 * glyphs from the G2 supplementary set onto the base display rows.
 *
 * This is how Polistream puts an actual note on air (decoded from a broadcast
 * capture): a Level-1.5 enhancement packet whose triplets set the active row
 * then place G2 character 0x55 (the eighth note) at the note's column. The base
 * row carries a space at that column; the decoder composites the note over it.
 *
 * note_rc[i] = {row, col} for each note; entries must be grouped/sorted by row.
 * Packets carry 13 triplets: per row a "set active position" triplet
 * (address=40+row, mode=0x04) followed by one "G2 character" triplet per note
 * (address=column, mode=0x0F, data=0x55), padded with terminators.
 */
static void build_x26_row(TeletextEncContext *ctx, uint8_t *line,
                          const uint8_t (*note_rc)[2], int num_notes)
{
    /* Bytes 0-1: MRAG for packet 26 in this magazine */
    encode_mrag(ctx->mag_encoded, 26, &line[0], &line[1]);
    /* Byte 2: designation code 0 (first enhancement packet for the page) */
    line[2] = ff_teletext_ham84(0);

    /* Bytes 3-41: 13 Hamming 24/18 triplets */
    uint8_t *tp = &line[3];
    int t = 0;              /* triplet index 0..12 */
    int cur_row = -1;
    for (int i = 0; i < num_notes && t < 13; i++) {
        int row = note_rc[i][0];
        int col = note_rc[i][1];
        if (row != cur_row && t < 13) {
            teletext_ham24(40 + row, 0x04, 0, &tp[t * 3]);  /* set active position to row */
            t++;
            cur_row = row;
        }
        if (t < 13) {
            teletext_ham24(col, 0x0F, 0x55, &tp[t * 3]);    /* G2 note (0x55) at column */
            t++;
        }
    }
    for (; t < 13; t++)
        teletext_ham24(63, 0x1F, 0, &tp[t * 3]);            /* termination triplet */
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

    /* Collect all display lines across rectangles first, then bottom-anchor
     * them: the last line sits on R23 and further lines grow upward. This
     * matches the OP-42 subtitle placement used by broadcast inserters
     * (e.g. Polistream), which anchor captions to the bottom of the page. */
    uint8_t lines[TELETEXT_ROWS][TELETEXT_COLS];
    int line_lens[TELETEXT_ROWS];
    int line_colors[TELETEXT_ROWS];
    int num_lines = 0;

    for (unsigned i = 0; i < sub->num_rects && num_lines < TELETEXT_ROWS; i++) {
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

        /* Usable text width once box/colour/double-height overhead is removed
         * (write_to_page prepends 2 Start Box + optional colour and appends 2
         * End Box; double height consumes col 0). */
        int overhead = 4 + (color != TELETEXT_ALPHA_WHITE ? 1 : 0);
        int max_w = TELETEXT_COLS - (ctx->double_height ? 1 : 0) - overhead;
        if (max_w < 1)
            max_w = 1;

        /* Split text by newlines and stash each non-empty display line */
        const uint8_t *line_start = clean_text;
        const uint8_t *p = clean_text;

        while (*p && num_lines < TELETEXT_ROWS) {
            if (*p == '\n' || *(p + 1) == '\0') {
                int line_len = (*p == '\n') ? (p - line_start) : (p - line_start + 1);
                if (line_len > 0) {
                    if (ctx->wrap_text) {
                        /* Word-wrap: break at spaces so text wider than the row
                         * isn't lost (Polistream "Wrap text if >ttxt width"). */
                        int s = 0;
                        while (s < line_len && num_lines < TELETEXT_ROWS) {
                            int remaining = line_len - s;
                            int take, skip;
                            if (remaining <= max_w) {
                                take = remaining;
                                skip = 0;
                            } else {
                                int brk = -1;
                                for (int i = max_w; i > 0; i--) {
                                    if (line_start[s + i] == ' ') { brk = i; break; }
                                }
                                take = (brk > 0) ? brk : max_w;   /* space, else hard break */
                                skip = (brk > 0) ? 1 : 0;         /* drop the break space */
                            }
                            memcpy(lines[num_lines], line_start + s, take);
                            line_lens[num_lines] = take;
                            line_colors[num_lines] = color;
                            num_lines++;
                            s += take + skip;
                            while (s < line_len && line_start[s] == ' ')
                                s++;                              /* trim leading spaces */
                        }
                    } else {
                        if (line_len > TELETEXT_COLS)
                            line_len = TELETEXT_COLS;
                        memcpy(lines[num_lines], line_start, line_len);
                        line_lens[num_lines] = line_len;
                        line_colors[num_lines] = color;
                        num_lines++;
                    }
                }
                line_start = p + 1;
            }
            p++;
        }
    }

    /* Bottom-anchor the subtitle rows.
     *
     * A double-height row is drawn two display lines tall, so with double
     * height the lines are spaced two rows apart with the last (bottom) line
     * on R22 (its lower half covers R23) — matching Polistream's OP-42
     * placement: one line on R22, two lines on R20 and R22, etc. The
     * intervening rows (R21/R23) are left blank and not transmitted. In
     * single-height mode the lines are adjacent with the last on R23. */
    int step = ctx->double_height ? 2 : 1;
    int last_row = ctx->double_height ? 22 : 23;
    int start_row = last_row - step * (num_lines - 1);
    if (start_row < 1)
        start_row = 1;

    for (int l = 0; l < num_lines; l++) {
        int row = start_row + step * l;
        if (row >= TELETEXT_ROWS)
            break;

        /* Double height governs the whole row, so emit it at column 0 (it is a
         * set-after attribute, taking effect on every following cell), matching
         * Polistream. The boxed text is then centred in the remaining cells. */
        int box_start = 0;
        if (ctx->double_height) {
            ctx->page_buffer[row][0] = TELETEXT_DOUBLE_HEIGHT;
            box_start = 1;
        }

        /* Centre the boxed block. write_to_page prepends two Start Box codes and
         * a colour code (opt) and appends two End Box codes, so account for that
         * overhead when centring. */
        int overhead = 4 + (line_colors[l] != TELETEXT_ALPHA_WHITE ? 1 : 0);
        int col = box_start + (TELETEXT_COLS - box_start - (line_lens[l] + overhead)) / 2;
        if (col < box_start)
            col = box_start;
        av_log(avctx, AV_LOG_DEBUG, "Teletext: writing line %d to row %d col %d\n",
               l, row, col);
        write_to_page(ctx, row, col, lines[l], line_lens[l], line_colors[l]);
    }

    /* Resolve music-note placeholders (see teletext_translit). Scan the laid-out
     * page for the note mark, record each (row, col), and replace the cell: with
     * X/26 enabled the base cell becomes a space and a real G2 note is overlaid
     * there via an enhancement packet (Polistream's method); otherwise it falls
     * back to '#', which renders as a hash in the default English G0 subset. */
    uint8_t note_rc[TELETEXT_ROWS * TELETEXT_COLS][2];
    int num_notes = 0;
    for (int row = 0; row < TELETEXT_ROWS; row++) {
        for (int col = 0; col < TELETEXT_COLS; col++) {
            if (ctx->page_buffer[row][col] != TELETEXT_NOTE_MARK)
                continue;
            if (ctx->note_x26) {
                ctx->page_buffer[row][col] = ' ';
                note_rc[num_notes][0] = row;
                note_rc[num_notes][1] = col;
                num_notes++;
            } else {
                ctx->page_buffer[row][col] = 0x5F;  /* '#' in the English G0 subset */
            }
        }
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
     * empty rows every cycle would only waste VBI bandwidth. Scan from the
     * anchored start row down to the bottom display row (R23). */
    for (int row = start_row; row < TELETEXT_ROWS && row <= 23; row++) {
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

    /* Enhancement row: overlay real G2 music notes via Packet X/26 (Polistream
     * method). Sent after the base content rows it decorates. */
    if (ctx->note_x26 && num_notes > 0) {
        build_x26_row(ctx, line_data, note_rc, num_notes);
        if (total_size + TELETEXT_DATA_UNIT_SIZE > buf_size)
            return AVERROR_BUFFER_TOO_SMALL;

        ff_teletext_build_data_unit(buf + total_size, TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE,
                                    0, 7, ctx->mag_encoded, 26, line_data);
        total_size += TELETEXT_DATA_UNIT_SIZE;
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
    { "wrap_text", "Word-wrap subtitle lines wider than the teletext row instead of truncating (Polistream 'Wrap text if >ttxt width')", OFFSET(wrap_text), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "translit", "Fold characters with no G0 glyph to teletext-safe equivalents ([]->() , numeric entities, smart quotes, accents; music note via note_x26). Disable to pass raw ASCII bytes through unchanged", OFFSET(translit), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
    { "note_x26", "Render a music note as a real G2 glyph via a Packet X/26 enhancement (Polistream method, needs a Level-1.5 decoder); disable to fall back to '#'", OFFSET(note_x26), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },
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
