/*
 * Blackmagic DeckLink output
 * Copyright (c) 2013-2014 Ramiro Polla
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

#include "libavformat/avformat.h"
#include "libavformat/mux.h"
#include "libavutil/opt.h"

#include "decklink_common_c.h"
#include "decklink_enc.h"

#define OFFSET(x) offsetof(struct decklink_cctx, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "list_devices", "use ffmpeg -sinks decklink instead", OFFSET(list_devices), AV_OPT_TYPE_BOOL, { .i64 = 0   }, 0, 1, ENC | AV_OPT_FLAG_DEPRECATED},
    { "list_formats", "list supported formats"  , OFFSET(list_formats), AV_OPT_TYPE_INT   , { .i64 = 0   }, 0, 1, ENC },
    { "preroll"     , "video preroll in seconds", OFFSET(preroll     ), AV_OPT_TYPE_DOUBLE, { .dbl = 0.5 }, 0, 5, ENC },
    { "block_until_available",     "wait for device to become available instead of raising error", OFFSET(block_until_available), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "vanc_queue_size", "VANC queue buffer size", OFFSET(vanc_queue_size), AV_OPT_TYPE_INT64, { .i64 = (1024 * 1024)}, 0, INT64_MAX, ENC },
    { "output_buffer_size", "Async output buffer size in bytes (0 = disabled)", OFFSET(output_buffer_size), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, ENC },
    { "late_threshold", "Error if frames are more than this many seconds late (0 = never error)", OFFSET(late_threshold), AV_OPT_TYPE_DOUBLE, { .dbl = 5.0 }, 0, 3600, ENC },
    { "audio_schedule_retry", "Max seconds to retry audio scheduling on transient denial during the preroll->playback transition (0 = no retry)", OFFSET(audio_schedule_retry), AV_OPT_TYPE_DOUBLE, { .dbl = 1.0 }, 0, 60, ENC },
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0b000000
    { "duplex_mode" , "duplex mode"             , OFFSET(duplex_mode ), AV_OPT_TYPE_INT   , { .i64 = 0   }, 0, 5, ENC, .unit = "duplex_mode"},
#else
    { "duplex_mode" , "duplex mode"             , OFFSET(duplex_mode ), AV_OPT_TYPE_INT   , { .i64 = 0   }, 0, 2, ENC, .unit = "duplex_mode"},
#endif
    { "unset"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 0   }, 0, 0, ENC, .unit = "duplex_mode"},
    { "half"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 1   }, 0, 0, ENC, .unit = "duplex_mode"},
    { "full"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 2   }, 0, 0, ENC, .unit = "duplex_mode"},
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0b000000
    { "one_sub_device_full",      NULL           ,0                   , AV_OPT_TYPE_CONST , { .i64 = 2   }, 0, 0, ENC, .unit = "duplex_mode"},
    { "one_sub_device_half",      NULL           ,0                   , AV_OPT_TYPE_CONST , { .i64 = 3   }, 0, 0, ENC, .unit = "duplex_mode"},
    { "two_sub_device_full",      NULL           ,0                   , AV_OPT_TYPE_CONST , { .i64 = 4   }, 0, 0, ENC, .unit = "duplex_mode"},
    { "four_sub_device_half",     NULL           ,0                   , AV_OPT_TYPE_CONST , { .i64 = 5   }, 0, 0, ENC, .unit = "duplex_mode"},
#endif
    { "link" ,         "single/dual/quad SDI link configuration", OFFSET(link), AV_OPT_TYPE_INT, { .i64 = 0   }, 0, 3, ENC, .unit = "link"},
    { "unset"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 0   }, 0, 0, ENC, .unit = "link"},
    { "single"      ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 1   }, 0, 0, ENC, .unit = "link"},
    { "dual"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 2   }, 0, 0, ENC, .unit = "link"},
    { "quad"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 3   }, 0, 0, ENC, .unit = "link"},
    { "audio_output",  "audio output connection", OFFSET(audio_output), AV_OPT_TYPE_INT,   { .i64 = 0   }, 0, 2, ENC, .unit = "audio_output"},
    { "unset"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 0   }, 0, 0, ENC, .unit = "audio_output"},
    { "aes_ebu"     ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 1   }, 0, 0, ENC, .unit = "audio_output"},
    { "analog"      ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 2   }, 0, 0, ENC, .unit = "audio_output"},
    { "sqd"         , "set Square Division"     , OFFSET(sqd)         , AV_OPT_TYPE_INT,    { .i64 = -1  }, -1,1, ENC, .unit = "sqd"},
    { "unset"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = -1  }, 0, 0, ENC, .unit = "sqd"},
    { "false"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 0   }, 0, 0, ENC, .unit = "sqd"},
    { "true"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 1   }, 0, 0, ENC, .unit = "sqd"},
    { "level_a"     , "set SMPTE LevelA"        , OFFSET(level_a)     , AV_OPT_TYPE_INT,    { .i64 = -1  }, -1,1, ENC, .unit = "level_a"},
    { "unset"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = -1  }, 0, 0, ENC, .unit = "level_a"},
    { "false"       ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 0   }, 0, 0, ENC, .unit = "level_a"},
    { "true"        ,  NULL                     , 0                   , AV_OPT_TYPE_CONST , { .i64 = 1   }, 0, 0, ENC, .unit = "level_a"},
    { "timing_offset", "genlock timing pixel offset", OFFSET(timing_offset), AV_OPT_TYPE_INT,   { .i64 = INT_MIN }, INT_MIN, INT_MAX, ENC, .unit = "timing_offset"},
    { "unset"       ,  NULL                     , 0                        , AV_OPT_TYPE_CONST, { .i64 = INT_MIN },       0,       0, ENC, .unit = "timing_offset"},
    { "teletext_fields", "teletext field insertion mode", OFFSET(teletext_fields), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, ENC, .unit = "teletext_fields"},
    { "both"        , "insert on both fields (default, per OP-47)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, ENC, .unit = "teletext_fields"},
    { "odd"         , "insert on odd field (field 1) only",         0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, ENC, .unit = "teletext_fields"},
    { "even"        , "insert on even field (field 2) only",        0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, ENC, .unit = "teletext_fields"},
    { "teletext_vbi_offset", "teletext VBI waveform start sample offset", OFFSET(teletext_vbi_offset), AV_OPT_TYPE_INT, { .i64 = 18 }, 0, 200, ENC },
    { "socket_path" , "Unix socket path for external frame input", OFFSET(socket_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "socket_listen", "Listen on socket for external frame input", OFFSET(socket_listen), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "shm_name", "Shared memory name for cross-process frame buffer", OFFSET(shm_name), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "shm_server", "Run as shared memory server (playout instance)", OFFSET(shm_server), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "shm_client", "Run as shared memory client (encoder instance)", OFFSET(shm_client), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "shm_max_frames", "Maximum frames in shared memory buffer", OFFSET(shm_max_frames), AV_OPT_TYPE_INT, { .i64 = 60 }, 8, 240, ENC },
    { "shm_block", "Block indefinitely when shared memory buffer is full", OFFSET(shm_block), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, ENC },
    { "pre_render", "Buffer frames before starting DeckLink playback", OFFSET(pre_render), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "pre_render_until", "UTC time to start playback (ISO 8601: YYYY-MM-DDTHH:MM:SS[.ffffff]Z)", OFFSET(pre_render_until), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "pre_render_frames", "Number of frames to buffer before starting (0 = use time trigger)", OFFSET(pre_render_frames), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 10000, ENC },
    { NULL },
};

static const AVClass decklink_muxer_class = {
    .class_name = "Blackmagic DeckLink outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_decklink_muxer = {
    .p.name           = "decklink",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Blackmagic DeckLink output"),
    .p.audio_codec    = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .p.subtitle_codec = AV_CODEC_ID_EIA_608,
    .p.flags          = AVFMT_NOFILE,
    .p.priv_class     = &decklink_muxer_class,
    .get_device_list = ff_decklink_list_output_devices,
    .priv_data_size = sizeof(struct decklink_cctx),
    .write_header   = ff_decklink_write_header,
    .write_packet   = ff_decklink_write_packet,
    .write_trailer  = ff_decklink_write_trailer,
};
