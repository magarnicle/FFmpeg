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

/**
 * @file
 * Teletext insertion into the vertical blanking of 625-line SD, as an
 * analogue waveform on lines 21 and 334.
 *
 * Everything here works on packets and samples and knows nothing about any
 * particular card. A caller feeds it the DVB teletext packets from the encoder,
 * asks once per output frame what to transmit on each field, and renders the
 * answer into a VBI line buffer. Which lines those are, and how they reach the
 * hardware, is the caller's business.
 *
 * The behaviour encoded here was arrived at by measuring a reference inserter
 * off air and comparing it packet by packet, so it is worth saying what depends
 * on what before changing any of it:
 *
 *  - The bit period is exactly 72/37 samples at 13.5 MHz. An approximation
 *    here shifts every bit after the clock run-in.
 *  - The page header must reach a receiver before the rows it belongs with, and
 *    in dual-field mode that means never starting a page cycle on field 2.
 *  - A page is only committed by a receiver when a header for another page in
 *    the magazine terminates it, which is what the spare-slot P8FF is for.
 *    Once per caption is enough, and more than that makes some receivers blink.
 *  - The end-of-caption erase is a blank page, not a bare header: an empty page
 *    reception leaves a strict receiver showing what it had before.
 *
 * tools/teletext_vbi_check.py checks all of that against a capture.
 */

#ifndef AVDEVICE_VBI_TELETEXT_H
#define AVDEVICE_VBI_TELETEXT_H

#include <stdint.h>

#define FF_VBI_TT_PACKET_SIZE  42   /* MRAG plus 40 data bytes */
#define FF_VBI_TT_MAX_ROWS      5   /* rows held for one page */

enum FFVbiTeletextFiller {
    FF_VBI_TT_FILLER_DUMMY = 0,     /**< page 8FF time-filling header, OP-42 s8 */
    FF_VBI_TT_FILLER_IDL   = 1,     /**< packet 8/31 Independent Data Line */
};

/**
 * How the waveform is drawn and how captions are scheduled. Set once and left
 * alone; the module never writes to it.
 */
typedef struct FFVbiTeletextConfig {
    /* Waveform */
    int vbi_offset;         /**< sample at which clock-run-in bit 0 begins */
    int vbi_offset_frac;    /**< plus this/256 of a sample; shaped paths only */
    int shape;              /**< raised-cosine -6 dB cutoff, % of bit rate; 0 = raw */
    int rise_ns;            /**< sine-squared edge 10-90% rise in ns; 0 = off,
                                 and it overrides @ref shape when set */
    int level;              /**< binary "1" level, % of peak white */

    /* Scheduling */
    int continuous;         /**< retransmit every frame instead of bursting */
    int burst_frames;       /**< frames to retransmit a caption after an update */
    int blank_idle;         /**< leave the line blank when idle, sending no filler */
    int dual_field;         /**< carry a different row on each field */
    int both_fields;        /**< both fields are inserted (not odd- or even-only) */
    int defer_erase;        /**< skip the erase at a caption's end */
    int frames_per_10s;     /**< the OP-42 s7 cleardown period, in frames */

    /* Filler and headers */
    int filler;             /**< enum FFVbiTeletextFiller */
    int filler_ctrl;        /**< dummy filler carries C6/C7/C8/C9 = 1 */
    int filler_subcode;     /**< dummy filler page subcode */
    int spare_p8ff;         /**< fill a spare dual-field slot with a P8FF header */
    int header_attr;        /**< spacing attribute for a header's first display
                                 cell; -1 leaves the packet as sent */
} FFVbiTeletextConfig;

/**
 * What to transmit on one frame. A field with its send flag clear is left
 * blank, which only happens under @ref FFVbiTeletextConfig.blank_idle.
 */
typedef struct FFVbiTeletextFrame {
    uint8_t packet[2][FF_VBI_TT_PACKET_SIZE];  /**< [0] is field 1, [1] field 2 */
    int     send[2];
} FFVbiTeletextFrame;

typedef struct FFVbiTeletextContext {
    FFVbiTeletextConfig cfg;

    uint8_t  rows[FF_VBI_TT_MAX_ROWS][FF_VBI_TT_PACKET_SIZE];
    int      row_count;
    int      row_index;      /**< next row of the page to transmit */
    int      rows_sent;      /**< row transmissions since the page was stored */
    int      has_data;
    int      erase_pending;  /**< the header still carries C4; age it after one pass */
    int      idle_frames;    /**< frames since the last caption update */
    int64_t  caption_end_pts;/**< when the caption stops being displayed */

    uint8_t  last_header[FF_VBI_TT_PACKET_SIZE];
    int      have_header;

    uint8_t  cleardown[FF_VBI_TT_MAX_ROWS][FF_VBI_TT_PACKET_SIZE];
    int      cleardown_count;
    int      cleardown_index;

    uint8_t  filler_buf[FF_VBI_TT_PACKET_SIZE];
    unsigned idl_ci;         /**< IDL continuity counter */
} FFVbiTeletextContext;

/**
 * Reset to the idle state and take a copy of @p cfg.
 */
void ff_vbi_teletext_init(FFVbiTeletextContext *s, const FFVbiTeletextConfig *cfg);

/**
 * Store one encoder packet as the page to transmit.
 *
 * @param data      a DVB teletext payload: one data identifier byte followed by
 *                  46-byte data units
 * @param size      bytes in @p data
 * @param pts       the packet's presentation time, in output frames
 * @param duration  its display duration in frames, or 0 if unknown
 * @param first     non-zero for the first packet stored for this frame; further
 *                  packets due in the same frame append to the page rather than
 *                  replacing it
 * @return the number of rows stored, or 0 if the packet carried none
 */
int ff_vbi_teletext_store(FFVbiTeletextContext *s, const uint8_t *data, int size,
                          int64_t pts, int64_t duration, int first, void *logctx);

/**
 * Decide what to transmit on this frame. Call once per output frame, after any
 * packets due have been stored.
 *
 * @param stored_new non-zero if a packet was stored for this frame
 * @param now_pts    the frame's presentation time, in output frames
 */
void ff_vbi_teletext_frame(FFVbiTeletextContext *s, int stored_new,
                           int64_t now_pts, FFVbiTeletextFrame *out, void *logctx);

/**
 * Render one 42-byte packet as a teletext line in V210.
 *
 * @param line_buf   the VBI line buffer, at least @p line_width pixels
 * @param line_width samples in the line, normally 720
 */
void ff_vbi_teletext_render_v210(uint8_t *line_buf, int line_width,
                                 const uint8_t *packet,
                                 const FFVbiTeletextConfig *cfg);

/**
 * @return the packet's row address (0-31), or -1 if either Hamming 8/4 address
 *         byte is uncorrectable.
 */
int ff_vbi_teletext_row_address(const uint8_t *packet);

/**
 * Clear C4, the erase-page bit, in a page header, so a retransmission does not
 * make a receiver clear and redraw the page it is already showing.
 */
void ff_vbi_teletext_clear_erase_bit(uint8_t *header_row);

#endif /* AVDEVICE_VBI_TELETEXT_H */
