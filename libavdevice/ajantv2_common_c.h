/*
 * AJA NTV2 output device: options shared between the C and C++ sides.
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

#ifndef AVDEVICE_AJANTV2_COMMON_C_H
#define AVDEVICE_AJANTV2_COMMON_C_H

#include <stdint.h>
#include "libavutil/log.h"

struct ajantv2_ctx;

struct ajantv2_cctx {
    const AVClass *cclass;
    void *ctx;                  /* struct ajantv2_ctx, C++ side */

    /* Device */
    int      channel;           /* framestore to play from, 1-based */
    char    *output;            /* SDI connectors to feed, e.g. "2" or "1,2";
                                   NULL means the one matching the framestore */
    int      ring_frames;       /* AutoCirculate frame ring depth */
    int      audio_depth;       /* bits per sample handed to the card: 16 or 32 */
    char    *format_code;       /* NTV2 video format by name, e.g. "1080i50";
                                   NULL infers it from the stream */
    int      cc;                /* embed CEA-708 in HD VANC; 0 keeps VANC out of
                                   the frame buffer entirely */
    int      playout;           /* 0 = AutoCirculate, 1 = frame flipping */

    /* Teletext. These mirror the DeckLink output's names so a playout command
     * line moves between the two by changing only -f, and they are handed
     * straight to the device-independent generator in vbi_teletext.c. */
    int      teletext_vbi_offset;
    int      teletext_vbi_offset_frac;
    int      teletext_shape;
    int      teletext_rise_ns;
    int      teletext_level;
    int      teletext_continuous;
    int      teletext_burst_frames;
    int      teletext_blank_idle;
    int      teletext_dual_field;
    int      teletext_fields;
    int      teletext_filler;
    int      teletext_filler_ctrl;
    int      teletext_filler_subcode;
    int      teletext_spare_p8ff;
    int      teletext_header_attr;
    int      teletext_defer_erase;
};

#endif /* AVDEVICE_AJANTV2_COMMON_C_H */
