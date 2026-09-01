/*
 * Blackmagic DeckLink common code
 * Copyright (c) 2013-2014 Ramiro Polla
 * Copyright (c) 2017 Akamai Technologies, Inc.
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

#ifndef AVDEVICE_DECKLINK_COMMON_C_H
#define AVDEVICE_DECKLINK_COMMON_C_H

#include <stdint.h>
#include <DeckLinkAPIVersion.h>

#include "libavutil/log.h"

typedef enum DecklinkPtsSource {
    PTS_SRC_AUDIO     = 1,
    PTS_SRC_VIDEO     = 2,
    PTS_SRC_REFERENCE = 3,
    PTS_SRC_WALLCLOCK = 4,
    PTS_SRC_ABS_WALLCLOCK = 5,
    PTS_SRC_NB
} DecklinkPtsSource;

typedef enum DecklinkSignalLossAction {
    SIGNAL_LOSS_NONE    = 1,
    SIGNAL_LOSS_REPEAT  = 2,
    SIGNAL_LOSS_BARS    = 3
} DecklinkSignalLossAction;

typedef enum DecklinkTeletextFields {
    TELETEXT_FIELDS_BOTH = 0,
    TELETEXT_FIELDS_ODD  = 1,  /* Field 1 only */
    TELETEXT_FIELDS_EVEN = 2,  /* Field 2 only */
} DecklinkTeletextFields;

struct decklink_cctx {
    const AVClass *cclass;

    void *ctx;

    /* Options */
    int list_devices;
    int list_formats;
    int enable_klv;
    int64_t teletext_lines;
    double preroll;
    int audio_channels;
    int audio_depth;
    int duplex_mode;
    int link;
    int sqd;
    int level_a;
    DecklinkPtsSource audio_pts_source;
    DecklinkPtsSource video_pts_source;
    int audio_input;
    int audio_output;
    int video_input;
    int tc_format;
    int draw_bars;
    char *format_code;
    int raw_format;
    int64_t queue_size;
    int64_t vanc_queue_size;
    int copyts;
    int64_t timestamp_align;
    int timing_offset;
    int wait_for_tc;
    int block_until_available;
    DecklinkSignalLossAction signal_loss_action;
    int64_t output_buffer_size;
    double late_threshold;
    double audio_schedule_retry;
    DecklinkTeletextFields teletext_fields;
    int teletext_vbi_offset;
    int teletext_shape;
    int teletext_shape_cutoff;
    int teletext_shape_taps;
    int teletext_shape_kernel;
    int teletext_p31_filler;
    int teletext_dual_field;

    /* Socket server options */
    char *socket_path;
    int socket_listen;

    /* Shared memory options */
    char *shm_name;
    int shm_server;
    int shm_client;
    int shm_max_frames;
    int shm_block;  /* Block indefinitely when buffer full */

    /* Pre-render options */
    int pre_render;           /* Enable pre-render mode */
    char *pre_render_until;   /* Wall clock time to start playback (HH:MM:SS or Unix timestamp) */
    int pre_render_frames;    /* Number of frames to buffer before starting (0 = use time trigger) */
};

#endif /* AVDEVICE_DECKLINK_COMMON_C_H */
