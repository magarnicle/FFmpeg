/*
 * Device-independent SD PAL teletext VBI generation.
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

#include <math.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/avutil.h"

#include "vbi_teletext.h"

/* Hamming 8/4 encode table: maps 4-bit value to protected byte (mirrors
 * ff_teletext_ham84_encode in libavcodec/teletextenc.c) */
static const uint8_t ham84_encode[16] = {
    0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
    0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA
};

/* Hamming 8/4 decode table: maps encoded byte to 4-bit value (0xFF = invalid) */
static const uint8_t ham84_decode[256] = {
    0x01, 0xFF, 0x01, 0x01, 0xFF, 0x00, 0x01, 0xFF,
    0xFF, 0x02, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x00, 0x01, 0xFF, 0x00, 0x00, 0xFF, 0x00,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0x06, 0xFF, 0xFF, 0x07, 0xFF, 0x07, 0x07, 0x07,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x0D, 0xFF,
    0x06, 0x06, 0x06, 0xFF, 0x06, 0xFF, 0xFF, 0x07,
    0xFF, 0x02, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x02, 0x02, 0xFF, 0x02, 0xFF, 0x02, 0x03, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x02, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0x03,
    0x04, 0xFF, 0xFF, 0x05, 0x04, 0x04, 0x04, 0xFF,
    0xFF, 0x02, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0xFF, 0x05, 0x05, 0x05, 0x04, 0xFF, 0xFF, 0x05,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x0A, 0xFF, 0xFF, 0x0B, 0x0A, 0x0A, 0x0A, 0xFF,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x0D, 0xFF,
    0xFF, 0x0B, 0x0B, 0x0B, 0x0A, 0xFF, 0xFF, 0x0B,
    0x0C, 0x0C, 0xFF, 0x0C, 0xFF, 0x0C, 0x0D, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x0C, 0x0D, 0xFF, 0x0D, 0xFF, 0x0D, 0x0D,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x0D, 0xFF,
    0x08, 0xFF, 0xFF, 0x09, 0xFF, 0x09, 0x09, 0x09,
    0xFF, 0x02, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x08, 0x08, 0x08, 0xFF, 0x08, 0xFF, 0xFF, 0x09,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x0F, 0xFF, 0x0F, 0x0F, 0xFF, 0x0E, 0x0F, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x0D, 0xFF,
    0xFF, 0x0E, 0x0F, 0xFF, 0x0E, 0x0E, 0xFF, 0x0E,
};

/* Clear the C4 (erase page) bit in a stored page-header row (byte 5, bit 3 of
 * the decoded nibble). Used to age out the erase flag after the header has been
 * transmitted once, so a decoder only clears the page on a genuine content
 * change rather than on every retransmission cycle. */
void ff_vbi_teletext_clear_erase_bit(uint8_t *header_row)
{
    uint8_t nibble = ham84_decode[header_row[5]];
    if (nibble != 0xFF && (nibble & 0x08))
        header_row[5] = ham84_encode[nibble & 0x07];
}

/* Decode the packet row address (0-31) from a 42-byte teletext row's MRAG.
 * Returns -1 if either Hamming 8/4 address byte is uncorrectable. */
int ff_vbi_teletext_row_address(const uint8_t *row)
{
    uint8_t a0 = ham84_decode[row[0]];
    uint8_t a1 = ham84_decode[row[1]];
    if (a0 == 0xFF || a1 == 0xFF)
        return -1;
    return (a0 | (a1 << 4)) >> 3;
}

/* Dummy header teletext packet per OP-42 Section 8 (Time Filling Headers)
 * Magazine 8 (encoded as 0), Row 0 (page header), Page FF
 *
 * Per OP-42: "time filling headers... are by convention 0,FF (Magazine 0
 * known as 8, page FF) with a subcode of 0x3F7E"
 *
 * Page header structure per ETS 300 706 Table 3:
 *   Bytes 0-1:   MRAG (magazine 8 encoded as 0, row 0)
 *   Bytes 2-3:   Page FF (units=F, tens=F)
 *   Byte 4:      S1 (subcode nibble)
 *   Byte 5:      S2 + C4 (erase page)
 *   Byte 6:      S3 (subcode nibble)
 *   Byte 7:      S4 + C5 (newsflash) + C6 (subtitle)
 *   Byte 8:      C7-C10 (all 0)
 *   Byte 9:      C11-C14 (all 0)
 *   Bytes 10-41: 32 display characters (spaces)
 *
 * Subcode 0x3F7E decomposes to S1=0xE, S2=0x7, S3=0xF, S4=0x3 with all
 * control bits 0. Hamming 8/4 values: ham84(0)=0x15, ham84(0x3)=0x5E,
 * ham84(0x7)=0x2F, ham84(0xE)=0xFD, ham84(0xF)=0xEA
 */
static const uint8_t teletext_filler_packet[42] = {
    0x15, 0x15,  /* MRAG: magazine 8 (encoded 0), row 0 */
    0xEA, 0xEA,  /* Page FF (units=F, tens=F) */
    0xFD,        /* S1 = 0xE */
    0x2F,        /* S2 = 0x7 + C4=0 */
    0xEA,        /* S3 = 0xF */
    0x5E,        /* S4 = 0x3 + C5=0 + C6=0 */
    0x15,        /* C7-C10 = 0 */
    0x15,        /* C11-C14 = 0 */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,  /* 32 spaces */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20
};

static const uint8_t *build_dummy(FFVbiTeletextContext *s);

static const uint8_t *build_filler(FFVbiTeletextContext *s)
{
    uint8_t *p = s->filler_buf;
    if (s->cfg.filler == FF_VBI_TT_FILLER_IDL) {
        p[0] = ham84_encode[8];            /* MRAG: mag 8 (0), row bit0=1 (row 31) */
        p[1] = ham84_encode[15];           /* row bits 1-4 = 1111 -> row 31 */
        p[2] = ham84_encode[4];            /* format/designation (matches poli 0x64) */
        p[3] = ham84_encode[6];            /* 0x38 */
        for (int i = 4; i < 10; i++)
            p[i] = ham84_encode[9];        /* 0xC7 service-address run */
        p[10] = (uint8_t)(s->idl_ci++ & 0xff);  /* continuity counter */
        for (int i = 11; i < 42; i++)
            p[i] = 0x20;                   /* fixed payload (not poli's live data) */
        return p;
    }
    return build_dummy(s);
}

/* Build the OP-42 s8 page-8FF time-filling header, whatever -teletext_filler is
 * set to. This is both the DUMMY filler and what -teletext_spare_p8ff puts in a
 * spare field. */
static const uint8_t *build_dummy(FFVbiTeletextContext *s)
{
    uint8_t *p = s->filler_buf;
    /* Page 8FF header with configurable subcode + control bits. */
    int sc = s->cfg.filler_subcode;
    int S1 = sc & 0xF, S2 = (sc >> 4) & 0x7, S3 = (sc >> 8) & 0xF, S4 = (sc >> 12) & 0x3;
    int c = s->cfg.filler_ctrl ? 1 : 0;
    p[0] = ham84_encode[0];                /* MRAG: mag 8 (0), row 0 */
    p[1] = ham84_encode[0];
    p[2] = ham84_encode[15];               /* page FF (units) */
    p[3] = ham84_encode[15];               /* page FF (tens) */
    p[4] = ham84_encode[S1];               /* S1 */
    p[5] = ham84_encode[S2];               /* S2 + C4=0 */
    p[6] = ham84_encode[S3];               /* S3 */
    p[7] = ham84_encode[(S4 | (c << 3)) & 0xF];       /* S4 + C5=0 + C6 */
    p[8] = ham84_encode[(c | (c << 1) | (c << 2)) & 0xF];  /* C7 C8 C9, C10=0 */
    p[9] = ham84_encode[0];                /* C11-C14 = 0 */
    for (int i = 10; i < 42; i++)
        p[i] = 0x20;                       /* 32 spaces */
    return p;
}

/* Build the OP-42 s7 cleardown as a real page rather than a bare header: the
 * page header with C4 (erase page) set, followed by an emptied copy of every
 * display row the current page used -- same row address, forty spaces, no Start
 * Box. Returns the number of rows built.
 *
 * Sending the header on its own, which is what this used to do, gives a strict
 * receiver an empty page reception: page opened, erased, terminated with no
 * rows. A receiver that treats an empty reception as nothing to commit keeps
 * the last good version of page 801 and puts the caption straight back up,
 * where it then sits until the next caption or the ten-second repeat of this
 * cleardown. Blanking the rows makes the reception non-empty, so the page that
 * gets committed is the blank one.
 */
static int build_cleardown(FFVbiTeletextContext *s)
{
    int n = 0;

    if (!s->have_header)
        return 0;
    memcpy(s->cleardown[n], s->last_header, 42);
    uint8_t nib = ham84_decode[s->cleardown[n][5]];
    if (nib != 0xFF)
        s->cleardown[n][5] = ham84_encode[(nib & 0x07) | 0x08];
    n++;

    for (int i = 0; i < s->row_count && n < 5; i++) {
        int row = ff_vbi_teletext_row_address(s->rows[i]);
        if (row < 1 || row > 24)
            continue;                       /* header, or not a display row */
        memcpy(s->cleardown[n], s->rows[i], 2);  /* MRAG */
        memset(s->cleardown[n] + 2, 0x20, 40);              /* spaces */
        n++;
    }
    return n;
}

/* Return the next stored teletext row to transmit and advance the carousel.
 * Applies the header C4-erase aging (assert erase once on the first header
 * after a content change, then clear it) so the decoder doesn't re-erase every
 * cycle. Called once per transmitted field (twice per frame in dual-field). */
static const uint8_t *next_row(FFVbiTeletextContext *s)
{
    int idx = s->row_index;
    /* Age the C4 erase bit on the row we are about to send, and only if it is
     * actually a page header. This used to key off idx == 0 and rewrite
     * teletext_rows[0] regardless, but the encoder does not always put the
     * header first -- captures show pages stored as [r22, header] and
     * [r20, r22] -- so it could mangle byte 5 of a text row, which is the
     * fourth display character, into a Hamming codeword. */
    if (ff_vbi_teletext_row_address(s->rows[idx]) == 0) {
        if (s->erase_pending)
            s->erase_pending = 0;   /* first header keeps C4=1 */
        else
            ff_vbi_teletext_clear_erase_bit(s->rows[idx]);
    }
    const uint8_t *d = s->rows[idx];
    s->row_index = (idx + 1) % s->row_count;
    if (s->rows_sent < INT_MAX)
        s->rows_sent++;
    return d;
}

/* Generate teletext VBI waveform for SD PAL output in V210 format
 * Per ITU-R BT.653-3 System B:
 *   - Data rate: 6.9375 Mbit/s
 *   - Sample rate: 13.5 MHz (PAL)
 *   - Samples per bit: ~1.946
 *   - Active samples: 720
 *
 * Waveform structure:
 *   - Clock run-in: 16 bits of alternating 1/0 (0xAAAA)
 *   - Framing code: 0xE4
 *   - MRAG: 2 bytes (magazine/row address)
 *   - Data: 40 bytes with odd parity
 *
 * V210 format: 6 pixels per 16 bytes (4 x 32-bit words)
 *   Word 0: Cb0[9:0], Y0[9:0], Cr0[9:0], xx
 *   Word 1: Y1[9:0], Cb1[9:0], Y2[9:0], xx
 *   Word 2: Cr1[9:0], Y3[9:0], Cb2[9:0], xx
 *   Word 3: Y4[9:0], Cr2[9:0], Y5[9:0], xx
 */
void ff_vbi_teletext_render_v210(uint8_t *line_buf, int line_width,
                                 const uint8_t *packet,
                                 const FFVbiTeletextConfig *cfg)
{
    const uint8_t *teletext_data  = packet;
    const int      data_len       = FF_VBI_TT_PACKET_SIZE;
    const int      vbi_offset     = cfg->vbi_offset;
    const int      vbi_offset_frac= cfg->vbi_offset_frac;
    const int      teletext_shape = cfg->shape;
    const int      teletext_rise_ns = cfg->rise_ns;
    const int      teletext_level = cfg->level;

    /* Teletext timing parameters for PAL/625:
     * Sample rate: 13.5 MHz
     * Data rate: 6.9375 Mbps
     * Samples per bit: 13.5 / 6.9375 = 72/37 = 1.9459459...
     *
     * Keep this as the exact rational 72/37. The 8-bit fixed point it replaces
     * (498/256 = 1.9453125) was 0.0326% short, which ran the packet clock at
     * 6939.8 kbit/s instead of 6937.5 and accumulated 0.24 samples (18 ns) of
     * slip by bit 359 -- measured off air, against a Polistream capture that
     * sits on nominal. A strict slicer locks on the clock run-in and then
     * free-runs, so that slip came straight off the eye at the end of the line.
     * Integer maths is safe here: the largest product is 360 * 72.
     */
    const int SPB_NUM = 72;
    const int SPB_DEN = 37;

    /* Binary "1" level, tunable via -teletext_level (percent of peak white).
     * 66% (default) matches ETS 300 706 / Polistream and decodes on our STBs;
     * OP-42 Fig 1 specifies 70% +/-3%. 10-bit: black=64, peak white=940. */
    int lvl = teletext_level;
    if (lvl < 40) lvl = 40;
    if (lvl > 100) lvl = 100;
    const uint16_t LUMA_HIGH = (uint16_t)(64 + lvl * (940 - 64) / 100);
    const uint16_t LUMA_LOW  = 64;    /* Black level (0 IRE) */
    const uint16_t LUMA_BLACK = 64;   /* Black level */
    const uint16_t CHROMA_NEUTRAL = 512;  /* Neutral chroma */

    /* Width of the VBI line buffer. This used to be hardcoded to 720; use the
     * caller-supplied line_width instead so we match whatever the DeckLink VBI
     * line buffer actually is on this device (V210 rows are padded to 48-pixel /
     * 128-byte blocks, so the real width can exceed 720). Clamp to the scratch
     * buffer below. Logged once so we can see the real value. */
    static int width_logged = 0;
    if (!width_logged) {
        width_logged = 1;
        av_log(NULL, AV_LOG_INFO,
               "Teletext VBI waveform: line_width=%d (previously hardcoded 720)\n",
               line_width);
    }

    /* Create array of 10-bit luma values for the line */
    uint16_t luma[2048];
    int width = line_width;
    if (width < 0)
        width = 0;
    if (width > (int)(sizeof(luma) / sizeof(luma[0])))
        width = (int)(sizeof(luma) / sizeof(luma[0]));
    for (int i = 0; i < width; i++)
        luma[i] = LUMA_BLACK;

    /* Start position for teletext data in the VBI buffer
     *
     * The DeckLink VBI buffer is 720 samples (same width as PAL active video).
     * The teletext waveform requires approximately 700 samples:
     *   - 16 bits clock run-in × 1.9459 samples/bit = 31 samples
     *   - 8 bits framing code × 1.9459 = 16 samples
     *   - 336 bits data × 1.9459 = 654 samples
     *   - Total: ~700 samples
     *
     * The vbi_offset parameter (configurable via -teletext_vbi_offset) is the
     * sample at which clock-run-in bit 0 begins.
     *
     * H-TIMING (OP-42 Fig 2/3): the penultimate of the eight clock-run-in "1"s
     * (bit 12 of the 16-bit run-in) must sit 12.0 +/- 0.288 us after the
     * line-sync datum, i.e. in 11.712 .. 12.288 us.
     *
     * Measured on three Polistream captures -- poli.txt, poli_21.txt and
     * poli_22.txt, all AJA dumps with VANCMode TALLER, a 720-sample active line
     * and data on lines 21/334, so all on this same grid -- Polistream's run-in
     * bit 0 starts at sample 5.62 (bit-0 centre 6.594) and its penultimate "1"
     * centres at sample 29.95. ITU-R BT.656 puts the start of the 625-line
     * digital active line 132 samples after 0H, so that edge lands at
     * (132 + 29.95) / 13.5 = 11.996 us: the datum, near enough exactly.
     *
     * Our run-in starts at `vbi_offset` (1:1), so:
     *   offset  6 -> penultimate "1" at 12.024 us   (inside the window)
     *   offset 26 -> penultimate "1" at 13.506 us   (1.2 us outside it)
     * Offset 6 is what production runs, and what a service provider set by eye
     * against a reference feed it trusted. The default here is still 26.
     *
     * An earlier revision of this comment said poli.txt showed Polistream's
     * run-in bit 0 at sample 26 and its penultimate "1" at ~sample 50, and set
     * the default to 26 on that basis. Re-measured, that same file gives 5.62
     * and 29.95. Wherever the sample-26 figure came from, it was not that file
     * on this grid. Left standing but unexplained: at offset 6 a Bridge VB440
     * is reported to have called the line "Other data in VBI" rather than
     * locking it as WST, which is the only argument for 26 and does not fit
     * these measurements.
     *
     * The full 45-byte packet is ~701 samples, so it nominally ends ~sample 707
     * at offset 6 and ~sample 727 at offset 26, overhanging the 720-sample line
     * in the latter case. Polistream has the same shape and is conformant: its
     * real data ends by ~705 and only trailing padding would clip (OP-42 Fig 3:
     * "provided the last bit of the last character on the line is not blanked").
     */
    /* Start of clock-run-in bit 0, in output samples. -teletext_vbi_offset_frac
     * adds a sub-sample part in 1/256ths: the shaped paths place edges between
     * samples, so they can honour it, while the raw square grid can only round.
     * Polistream sits at 5 + 159/256 = 5.62 (see H-TIMING above). */
    double start_s = (double)vbi_offset + vbi_offset_frac / 256.0;
    int pixel_pos = (int)(start_s + 0.5);

    /* Build the on-air bit sequence into bitlvl[]: 16-bit clock run-in (1010...,
     * starting high), 8-bit framing code, then the 42 data bytes LSB-first. Both
     * the raw output grid and the high-res shaped waveform render from the SAME
     * bits, so keep the sequence in one place.
     *
     * Framing 0x27 = on-air 11100100 (first bit first, per ETS 300 706 / ITU-R
     * BT.653), emitted LSB-first. 0xE4 is the MSB-first spelling and frame-locks on
     * nothing; verified against a Polistream capture. */
    uint16_t bitlvl[24 + 42 * 8];
    int nbits = 0;
    for (int bit = 0; bit < 16; bit++)
        bitlvl[nbits++] = (bit & 1) ? LUMA_LOW : LUMA_HIGH;
    uint8_t framing = 0x27;
    for (int bit = 0; bit < 8; bit++)
        bitlvl[nbits++] = (framing & (1 << bit)) ? LUMA_HIGH : LUMA_LOW;
    for (int byte_idx = 0; byte_idx < 42 && byte_idx < data_len; byte_idx++) {
        uint8_t byte = teletext_data[byte_idx];
        for (int bit = 0; bit < 8; bit++)
            bitlvl[nbits++] = (byte & (1 << bit)) ? LUMA_HIGH : LUMA_LOW;
    }

    /* Render the raw output-grid square wave (used when shaping is off). Bit b
     * covers output samples [offset + floor(b*SPB), offset + floor((b+1)*SPB)),
     * with SPB = SPB_NUM/SPB_DEN. */
    for (int b = 0; b < nbits; b++) {
        int start_pixel = pixel_pos + (b * SPB_NUM) / SPB_DEN;
        int end_pixel = pixel_pos + ((b + 1) * SPB_NUM) / SPB_DEN;
        for (int p = start_pixel; p < end_pixel && p < width; p++)
            if (p >= 0)
                luma[p] = bitlvl[b];
    }

    /* Optional band-limiting of the teletext eye (teletext_shape > 0, off by
     * default). The raw square wave has instantaneous 64<->640 edges whose
     * out-of-band energy rings and overshoots in any downstream analog / band-
     * limited path -- a strict WST slicer then reads peaks well above the nominal
     * level and rejects the eye as non-conformant ("Other data in VBI").
     *
     * We build the waveform at HIGH internal resolution directly from the bit
     * sequence (bitlvl[]) at each bit's TRUE fractional position, apply a raised-
     * cosine (Nyquist, beta=1) low-pass, then decimate to the output grid. Building
     * the hi-res grid from the bits -- rather than upsampling the already-quantised
     * output-grid square -- is what keeps the eye open. Quantising the bit edges to
     * whole output samples first jitters the bit timing by up to +/-0.5 sample
     * (~0.26 bit); at ~1.95 samples/bit that alone nearly shuts the eye regardless
     * of cutoff (which is why lower shape values used to drop characters). Placing
     * the edges at OS-times finer resolution keeps the eye wide open, so the cutoff
     * can go lower (smoother edges, more sample levels, closer to a hardware
     * inserter) without corrupting data.
     *
     * teletext_shape is the -6 dB cutoff as a PERCENT of the 6.9375 MHz bit rate
     * (e.g. 90 = 6.24 MHz). Lower = softer edges / less overshoot / more sample
     * levels but a tighter eye; sweep it on the target slicer. */
    const uint16_t *out_luma = luma;
    uint16_t filtered[2048];
    if (teletext_rise_ns > 0) {
        /* Monotonic sine-squared (raised-cosine) EDGE, evaluated analytically on
         * the output grid. Each bit boundary k contributes a step
         *   d * S((t - tk)/Tfull + 0.5),  S(x) = 0.5 - 0.5*cos(pi*x) on [0,1]
         * and the steps superpose. S is monotonic, so the result can never leave
         * [LUMA_LOW, LUMA_HIGH] -- which is the point of having this path at all.
         * -teletext_shape band-limits with a sinc-derived Nyquist kernel, whose
         * sidelobes ring about 1 LSB past both rails; measured off air at
         * shape 45 the level reaches 161 and 14 against nominal 160 and 16,
         * where Polistream never leaves 16...160.
         *
         * S rises from 10% to 90% over 0.590334 of its full width, so a wanted
         * 10-90% rise of R samples needs a full transition width of R/0.590334.
         * A width over one bit period is normal and is what Polistream does: its
         * clock run-in does not reach the rails, settling only on longer runs.
         *
         * -teletext_rise_ns 86 reproduces Polistream. Beware when checking that
         * against a capture: interpolating a 13.5 MHz dump between samples reads
         * a smooth edge about 30 ns wide of the truth, and averaging transitions
         * does not cancel it, so poli reads 115 ns by that method and 86 ns when
         * the shape is fitted. tools/teletext_vbi_check.py reports both.
         *
         * This path ignores -teletext_shape and needs no oversampling, since the
         * edge is computed in closed form rather than filtered. */
        const double PI = 3.14159265358979323846;
        double rise_samples = teletext_rise_ns * 13.5 / 1000.0;   /* ns -> samples */
        double tfull = rise_samples / 0.590334;
        double acc[2048];
        if (tfull < 1e-3)
            tfull = 1e-3;
        for (int i = 0; i < width; i++)
            acc[i] = 0.0;
        for (int k = 0; k <= nbits; k++) {
            double prev = (k == 0)     ? (double)LUMA_BLACK : (double)bitlvl[k - 1];
            double next = (k == nbits) ? (double)LUMA_BLACK : (double)bitlvl[k];
            double d = next - prev;
            if (d == 0.0)
                continue;
            double tk = start_s + (double)k * SPB_NUM / SPB_DEN;
            int j0 = (int)(tk - tfull / 2.0);
            int j1 = (int)(tk + tfull / 2.0) + 1;
            if (j0 < 0) j0 = 0;
            if (j1 > width) j1 = width;
            for (int i = j0; i < j1; i++) {
                double x = ((double)i - tk) / tfull + 0.5;
                double sx = (x <= 0.0) ? 0.0
                          : (x >= 1.0) ? 1.0
                                       : 0.5 - 0.5 * cos(PI * x);
                acc[i] += d * sx;
            }
            for (int i = j1; i < width; i++)
                acc[i] += d;
        }
        for (int i = 0; i < width; i++) {
            double v = (double)LUMA_BLACK + acc[i];
            if (v < 0.0) v = 0.0;
            if (v > 1023.0) v = 1023.0;
            filtered[i] = (uint16_t)(v + 0.5);
        }
        out_luma = filtered;
    } else if (teletext_shape > 0) {
        enum { OS = 8 };                                  /* internal oversample */
        static double hires[2048 * OS];
        static double filt[2048 * OS];
        static double kern[1024];
        const double PI = 3.14159265358979323846;
        int hw = width * OS;
        if (hw > (int)(sizeof(hires) / sizeof(hires[0])))
            hw = (int)(sizeof(hires) / sizeof(hires[0]));
        /* Build the hi-res waveform from the bit sequence at fractional positions.
         * hi-res sample j is at output position j/OS; the covering bit is
         * floor((j/OS - offset) / SPB), SPB = SPB_NUM/SPB_DEN. In hi-res
         * units: bit = (j - offset*OS) * SPB_DEN / (SPB_NUM * OS). */
        for (int j = 0; j < hw; j++) {
            long long rel = (long long)j - (long long)(start_s * OS + 0.5);
            if (rel < 0) { hires[j] = LUMA_BLACK; continue; }
            long long bit = rel * SPB_DEN / ((long long)SPB_NUM * OS);
            hires[j] = (bit < nbits) ? bitlvl[bit] : LUMA_BLACK;
        }
        /* raised-cosine (Nyquist) low-pass kernel at the hi-res rate */
        const double beta = 1.0;
        double fs  = 13.5e6 * OS;
        double f6  = (teletext_shape / 100.0) * 6.9375e6; /* -6 dB cutoff (Hz) */
        double fcn = f6 / fs;                             /* cycles per hi-res sample */
        int r = (int)(6.0 * (13.5 / 6.9375) * OS);        /* ~6 bit periods each side */
        if (r < 1) r = 1;
        if (r > (int)(sizeof(kern) / sizeof(kern[0])) / 2 - 1)
            r = (int)(sizeof(kern) / sizeof(kern[0])) / 2 - 1;
        double ksum = 0.0;
        for (int n = -r; n <= r; n++) {
            double sinc = (n == 0) ? 2.0 * fcn
                                   : sin(2.0 * PI * fcn * n) / (PI * n);
            double d = 1.0 - (4.0 * beta * fcn * n) * (4.0 * beta * fcn * n);
            double ct = (fabs(d) < 1e-6) ? PI / 4.0
                                         : cos(2.0 * PI * beta * fcn * n) / d;
            double w = 0.5 - 0.5 * cos(2.0 * PI * (n + r) / (2.0 * r)); /* Hann */
            kern[n + r] = sinc * ct * w;
            ksum += kern[n + r];
        }
        for (int n = 0; n <= 2 * r; n++)
            kern[n] /= ksum;                              /* unity DC gain */
        for (int i = 0; i < hw; i++) {
            double acc = 0.0;
            for (int n = -r; n <= r; n++) {
                int j = i + n;
                if (j < 0) j = 0; else if (j >= hw) j = hw - 1;
                acc += kern[n + r] * hires[j];
            }
            filt[i] = acc;
        }
        /* decimate back to the output grid, clamp to valid 10-bit range */
        for (int i = 0; i < width; i++) {
            int j = i * OS;
            double v = filt[j < hw ? j : hw - 1];
            if (v < 0.0) v = 0.0;
            if (v > 1023.0) v = 1023.0;
            filtered[i] = (uint16_t)(v + 0.5);
        }
        out_luma = filtered;
    }

    /* Convert to V210 format: 6 pixels per 16 bytes */
    uint32_t *v210 = (uint32_t *)line_buf;
    for (int i = 0; i + 6 <= width; i += 6) {
        uint16_t y0 = out_luma[i];
        uint16_t y1 = out_luma[i + 1];
        uint16_t y2 = out_luma[i + 2];
        uint16_t y3 = out_luma[i + 3];
        uint16_t y4 = out_luma[i + 4];
        uint16_t y5 = out_luma[i + 5];

        /* V210 packing (little-endian 32-bit words):
         * Word 0: Cb0 | Y0 | Cr0
         * Word 1: Y1 | Cb1 | Y2
         * Word 2: Cr1 | Y3 | Cb2
         * Word 3: Y4 | Cr2 | Y5
         */
        *v210++ = (CHROMA_NEUTRAL) | (y0 << 10) | (CHROMA_NEUTRAL << 20);
        *v210++ = (y1) | (CHROMA_NEUTRAL << 10) | (y2 << 20);
        *v210++ = (CHROMA_NEUTRAL) | (y3 << 10) | (CHROMA_NEUTRAL << 20);
        *v210++ = (y4) | (CHROMA_NEUTRAL << 10) | (y5 << 20);
    }
}

/* -teletext_header_attr: write a spacing attribute as the first of a page
 * header's 32 display characters. A reference inserter sends 0x06, Alpha Cyan,
 * on every header it transmits, P801 and P8FF alike, where we send a space. The
 * row is not rendered by a conformant receiver anyway -- C7, suppress header, is
 * set in both -- so this only closes a difference against the reference.
 * Applied to the outgoing copies, so the stored rows are left alone. */
static void apply_header_attr(FFVbiTeletextContext *s, FFVbiTeletextFrame *out)
{
    int i, bit, ones;
    uint8_t attr;

    if (s->cfg.header_attr < 0)
        return;
    attr = s->cfg.header_attr & 0x1F;
    for (ones = 0, bit = 0; bit < 8; bit++)
        ones += (attr >> bit) & 1;
    for (i = 0; i < 2; i++)
        if (out->send[i] && ff_vbi_teletext_row_address(out->packet[i]) == 0)
            out->packet[i][10] = (ones & 1) ? attr : (uint8_t)(attr | 0x80);
}


void ff_vbi_teletext_init(FFVbiTeletextContext *s, const FFVbiTeletextConfig *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->caption_end_pts = AV_NOPTS_VALUE;
    if (s->cfg.frames_per_10s < 50)
        s->cfg.frames_per_10s = 250;
}


int ff_vbi_teletext_store(FFVbiTeletextContext *s, const uint8_t *data, int size,
                          int64_t pts, int64_t duration, int first, void *logctx)
{
        /* Parse all data units from the packet (each is 46 bytes) */
        int num_units = size / 46;
        if (num_units > 5)
            num_units = 5;  /* Limit to 5 rows max */

        if (num_units > 0) {
            /* Storing a packet used to replace the page outright, which lost any
             * row of the previous one that had not been transmitted yet. Two
             * ways that happens: several packets come due in the same frame, and
             * a caption arrives while the one before it is still going out (the
             * encoder splits a caption across packets -- captures show [r20, r22]
             * followed a frame later by [header, r18]). Either way the dropped
             * rows never reach air and that part of the caption is simply
             * missing. Rows already sent are safe, since a decoder keeps them in
             * page memory until the page is erased; only the unsent tail matters,
             * so carry it forward ahead of the new rows. */
            int base;
            if (!first) {
                base = s->row_count;      /* another packet, same frame */
            } else if (s->has_data
                       && s->rows_sent < s->row_count) {
                int keep = s->row_count - s->rows_sent;
                memmove(s->rows[0],
                        s->rows[s->rows_sent], keep * 42);
                av_log(logctx, AV_LOG_DEBUG,
                       "Teletext: carrying %d unsent row(s) into the new page\n", keep);
                base = keep;
            } else {
                base = 0;
            }
            if (base + num_units > 5)
                num_units = 5 - base;
            if (num_units <= 0) {
                av_log(logctx, AV_LOG_WARNING,
                       "Teletext: more than 5 rows due this frame, dropping the rest\n");
                return 0;
            }
            for (int i = 0; i < num_units; i++) {
                const uint8_t *du = data + (i * 46);
                /* Copy 42-byte payload (bytes 4-45 of each data unit) */
                memcpy(s->rows[base + i], du + 4, 42);
                if (ff_vbi_teletext_row_address(du + 4) == 0) {
                    memcpy(s->last_header, du + 4, 42);
                    s->have_header = 1;
                }
            }
            s->row_count = base + num_units;
            s->row_index = 0;  /* Reset to start of new content */
            s->rows_sent = 0;  /* Nothing of this page has gone out */
            s->has_data = 1;
            s->erase_pending = 1;  /* New content: header carries C4=1 */
                        /* Remember when this caption should stop being displayed, so we can
             * erase it promptly at its end instead of waiting for the OP-42 s7
             * idle cleardown (which lingers the caption up to ~10s). Falls back
             * to the idle timer when the packet carries no duration. The latest
             * end time wins when several packets make up one caption. */
            int64_t end = (duration > 0)
                ? pts + duration : AV_NOPTS_VALUE;
            if (base == 0 || end == AV_NOPTS_VALUE
                || s->caption_end_pts == AV_NOPTS_VALUE
                || end > s->caption_end_pts)
                s->caption_end_pts = end;

            av_log(logctx, AV_LOG_DEBUG,
                   "Teletext: storing %d data units at %d (pts=%"PRId64" dur=%"PRId64" end=%"PRId64")\n",
                   num_units, base, pts, duration,
                   s->caption_end_pts);
            return num_units;
        }
        return 0;
}


void ff_vbi_teletext_frame(FFVbiTeletextContext *s, int stored_new,
                           int64_t now_pts, FFVbiTeletextFrame *out, void *logctx)
{
    memset(out, 0, sizeof(*out));

    /* Track how long we've gone without a caption update, for the OP-42 s7
     * cleardown. Reset on a new packet; otherwise age once per frame. */
    if (stored_new)
        s->idle_frames = 0;
    else if (s->has_data)
        s->idle_frames++;

    const int frames_10s = s->cfg.frames_per_10s;

    /* Prompt clear at the caption's own end time: when the current caption's
     * display duration has elapsed and no newer caption has arrived, jump the
     * idle timer to the cleardown point so the OP-42 s7 erase fires now instead
     * of ~10s later (which left the caption lingering past program end).
     *
     * MUST NOT fire before the page has been transmitted. The jump sets
     * teletext_idle_frames to frames_10s, which also closes the burst gate below
     * (teletext_idle_frames < teletext_burst_frames), so firing it too early cuts
     * the caption off after as little as one frame. That happens whenever the
     * caption's end_pts is already <= last_pts on arrival, from a short display
     * duration or captions delivered at or behind the video clock.
     *
     * Two conditions, and both are needed. A minimum dwell keeps a caption on
     * air even when its end time had already passed when it arrived, which is
     * what a short display duration or delivery behind the video clock looks
     * like; without it the caption is cut to a single frame. teletext_rows_sent
     * >= teletext_row_count additionally requires that the page has actually
     * been transmitted, so a page whose rows have not all gone out is never
     * erased from under itself.
     *
     * The dwell is capped at 8 frames rather than being teletext_burst_frames
     * itself. The burst is redundancy and can be set long -- 25 frames helps a
     * receiver that misses packets -- but tying the erase to it held every
     * caption a second past its end time, which is a caption clearing late.
     * Eight frames is what the dwell was when burst_frames defaulted to 8.
     *
     * row_index == 0 holds the erase to a page-cycle boundary. The cleardown's
     * header terminates whatever page is open, so firing it halfway through a
     * cycle commits a partial page: the caption appears with a line missing for
     * one frame, then blanks. Waiting for the cycle to finish means the page
     * committed is complete, and identical to what is already on screen.
     *
     * An earlier revision dropped the burst_frames half, on the reading that
     * captions arriving closer together than burst_frames could never reach the
     * erase. On-air logs show caption bursts running their full eight frames
     * with 16 to 95 frames between them, so that case does not arise in this
     * material and dropping the guard only reinstated the single-frame bug. */
    if (!s->cfg.defer_erase
        && s->has_data && !stored_new
        && s->caption_end_pts != AV_NOPTS_VALUE
        && now_pts >= s->caption_end_pts
        && s->idle_frames >= FFMIN(s->cfg.burst_frames, 8)
        && s->rows_sent >= s->row_count
        && s->row_index == 0
        && s->idle_frames < frames_10s) {
        /* teletext_defer_erase off (default): promptly clear the caption at its end
         * time via the OP-42 s7 cleardown. On: skip this, so no standalone erase
         * header is sent per caption -- the next caption's C4 erase clears it (or the
         * 10s idle cleardown), which cuts our erase-header count toward Polistream's. */
        s->idle_frames = frames_10s;
    }

    /* Determine which data to transmit. The stored page is retransmitted one
     * row per frame (continuous carousel), the same packet on both fields.
     *
     * data_to_send == NULL means "send nothing" -- leave line 21/334 blank this
     * frame. That only happens where we would otherwise send P8FF FILLER, and only
     * when -teletext_blank_idle is set: some downstream encoders flag the continuous
     * filler as "extra data on line 21" and expect the line blank between caption
     * bursts (as the MS Now reference does). It steps outside OP-42 s4(b) (which
     * wants dummy headers when idle), so it is opt-in. The cleardown packets and the
     * caption rows are NEVER blanked -- the cleardown carries the erase command the
     * decoder needs, so it must always go out. */
    const uint8_t *data_to_send;
    const uint8_t *data_f2 = NULL;   /* field-2 override for dual-field; NULL = copy field 1 */
    int data_is_filler = 0;          /* set when data_to_send came from the filler builder */
    if (!s->has_data) {
        /* Lead-in, before any caption has arrived: filler so the line is never
         * dead and the decoder clock stays locked (or blank if blank_idle). */
        data_to_send = s->cfg.blank_idle ? NULL : build_filler(s);
        data_is_filler = data_to_send != NULL;
        av_log(logctx, AV_LOG_DEBUG, "Teletext: %s (no caption yet)\n",
               data_to_send ? "filler" : "blank");
    } else if (s->idle_frames >= frames_10s) {
        /* OP-42 s7: after 10s with no caption update, clear the page (blank
         * P801 with C4=1, no StartBox) and drop to continuous filler,
         * repeating the cleardown every 10s. */
        if (s->idle_frames % frames_10s < 5 && s->have_header) {
            /* Built once at the start of each five-frame cleardown, then cycled
             * through with the same two-slot rule as a caption so the header
             * always leads and the page is terminated after its rows. */
            if (s->idle_frames % frames_10s == 0) {
                s->cleardown_count = build_cleardown(s);
                s->cleardown_index = 0;
            }
            if (s->cleardown_count > 0) {
                int *idx = &s->cleardown_index;
                data_to_send = s->cleardown[*idx];
                *idx = (*idx + 1) % s->cleardown_count;
                if (s->cfg.both_fields
                    && s->cleardown_count > 1) {
                    if (*idx == 0) {
                        data_f2 = s->cfg.spare_p8ff
                                      ? build_dummy(s) : data_to_send;
                    } else {
                        data_f2 = s->cleardown[*idx];
                        *idx = (*idx + 1) % s->cleardown_count;
                    }
                }
            }
            av_log(logctx, AV_LOG_DEBUG,
                   "Teletext: idle cleardown (pts=%"PRId64" idle=%d rows=%d)\n",
                   now_pts, s->idle_frames,
                   s->cleardown_count);
        } else {
            data_to_send = s->cfg.blank_idle ? NULL : build_filler(s);
            data_is_filler = data_to_send != NULL;
            av_log(logctx, AV_LOG_DEBUG, "Teletext: idle %s\n",
                   data_to_send ? "filler" : "blank");
        }
    } else if (s->row_count > 0
               && (s->cfg.continuous
                   || s->idle_frames < s->cfg.burst_frames
                   || s->rows_sent < s->row_count
                   || s->row_index != 0)) {
        /* The teletext_rows_sent test is a floor under teletext_burst_frames:
         * keep transmitting until every row of the page has gone out at least
         * once, however short the burst is set. Dual-field sends two rows per
         * frame, so a three- or four-row page needs two frames, and a burst of
         * one would have left the last row -- usually the bottom line of the
         * caption -- never transmitted at all. On-air logs show 3- and 4-row
         * pages making up over half the captions.
         *
         * row_index keeps it going to the end of the page cycle, for the same
         * reason the erase waits for one: a burst that stops mid-cycle leaves a
         * partial page for the next header to commit. */
        /* Active or recently-updated caption. Two modes:
         *   Burst (default): retransmit the stored page only for the first
         *     teletext_burst_frames after each update, then stop and drop to
         *     filler while the decoder HOLDS the displayed page -- matching a real
         *     subtitle inserter's short-burst-then-pause cadence (the MS Now
         *     reference), instead of flooding a downstream caption extractor with
         *     the same caption every frame. The end-of-caption cleardown above
         *     still erases it at its end time / the 10s idle timer.
         *   Continuous (-teletext_continuous 1): legacy carousel, retransmit
         *     every frame.
         * Either way, next_row ages out the header's C4 erase bit after
         * the first transmission so the decoder doesn't clear and re-render. */
        data_to_send = next_row(s);
        /* Dual-field (ala MS Now): field 2 (line 334) carries the NEXT row rather
         * than a copy of field 1, so a multi-row page transmits in half the frames.
         * Only meaningful with >1 row and both fields active; filler/cleardown below
         * stay identical on both fields. next_row also ages the C4 erase
         * bit, so the header still erases correctly whichever field it lands on. */
        if (s->cfg.dual_field && s->row_count > 1
            && s->cfg.both_fields) {
            /* Never start a new page cycle in field 2. If the carousel has just
             * wrapped, this field has no further row of the caption to carry,
             * and taking the next entry would put the page header in field 2 --
             * after the rows it belongs with. The cycle then free-runs out of
             * phase: a three-row page goes out as (header, r20) (r22, header)
             * (r20, r22), so every third frame carries rows with no header
             * ahead of them at all. A decoder that only accepts rows while a
             * page is open discards those, and strict receivers showed no
             * captions whatsoever until this was pinned.
             *
             * Filling the spare slot instead keeps the cycle aligned as
             * (header, r20) (r22, filler), header first, every time.
             * -teletext_spare_p8ff picks what goes in the slot: an OP-42 s8
             * P8FF time-filling header, which is what Polistream sends there --
             * 7 of its 8 P8FF packets across poli_21 and poli_22 sit in exactly
             * this position -- or, with the option off, a repeat of the packet
             * field 1 just carried, which is inert. Either way the carousel
             * index is left alone so the next frame restarts at the header. */
            if (s->row_index == 0) {
                /* The P8FF terminates page 801 so a receiver commits it, which
                 * is why it cannot simply be dropped. But it also opens page
                 * 8FF, and a receiver that blanks while acquiring a new page in
                 * the magazine shows that as the caption dipping and coming
                 * back. Once per caption is enough: the page is committed on
                 * the first pass and every later pass is redundancy, which
                 * needs no further commit. Raising -teletext_burst_frames made
                 * this visible, because twelve dips are noticeable where three
                 * were not.
                 *
                 * teletext_rows_sent has just passed row_count on the first
                 * wrap, so this is true exactly once per stored page. Later
                 * wraps repeat what field 1 carried, which is inert. */
                data_f2 = (s->cfg.spare_p8ff
                           && s->rows_sent <= s->row_count)
                              ? build_dummy(s) : data_to_send;
            } else
                data_f2 = next_row(s);
            /* Send the pair in ascending row order within the frame. The carousel
             * hands rows over in stored order, which put the bottom line of a
             * two-line caption on field 1 and the top line on field 2, so a
             * decoder drew the caption bottom-first for one field (20 ms).
             * Only swap two display rows: a page header (row 0) carries the C4
             * erase and has to stay ahead of the text it erases for. */
            int r_f1 = ff_vbi_teletext_row_address(data_to_send);
            int r_f2 = ff_vbi_teletext_row_address(data_f2);   /* 0 for a header, never swapped */
            if (r_f1 > 0 && r_f2 > 0 && r_f1 > r_f2) {
                const uint8_t *swap = data_to_send;
                data_to_send = data_f2;
                data_f2 = swap;
            }
        }
        av_log(logctx, AV_LOG_DEBUG,
               "Teletext: burst row (pts=%"PRId64" idle=%d/%d rows=%d sent=%d idx=%d "
               "continuous=%d dual=%d)\n",
               now_pts, s->idle_frames, s->cfg.burst_frames,
               s->row_count, s->rows_sent,
               s->row_index, s->cfg.continuous, data_f2 != NULL);
    } else {
        /* Burst finished (or no rows): filler holds the line for OP-42 s4(b)
         * while the decoder keeps displaying the last page (or blank if
         * blank_idle -- the decoder holds the page regardless). */
        data_to_send = s->cfg.blank_idle ? NULL : build_filler(s);
        data_is_filler = data_to_send != NULL;
        av_log(logctx, AV_LOG_DEBUG,
               "Teletext: hold %s (idle=%d/%d rows=%d)\n",
               data_to_send ? "filler" : "blank", s->idle_frames,
               s->cfg.burst_frames, s->row_count);
    }

    /* Hand the answer back. A field whose send flag stays clear is left blank,
     * which only happens under blank_idle. */
    if (data_to_send) {
        int sent_f1 = 0;
        if (s->cfg.both_fields || !s->cfg.dual_field) {
            memcpy(out->packet[0], data_to_send, FF_VBI_TT_PACKET_SIZE);
            out->send[0] = 1;
            sent_f1 = 1;
        }
        if (s->cfg.both_fields) {
            const uint8_t *f2 = data_f2;
            /* Filler is a fresh packet on each field, not a copy of field 1's.
             * The IDL filler carries a per-packet continuity counter, and
             * re-sending field 1's bytes advanced it only once per frame, so a
             * receiver could not tell a lost field-1 packet from a lost field-2
             * one -- the only thing the counter is for. Rebuild only when field 1
             * actually went out, otherwise the counter would skip. */
            if (!f2 && sent_f1 && data_is_filler)
                f2 = build_filler(s);
            memcpy(out->packet[1], f2 ? f2 : data_to_send, FF_VBI_TT_PACKET_SIZE);
            out->send[1] = 1;
        }
    }
    apply_header_attr(s, out);
}
