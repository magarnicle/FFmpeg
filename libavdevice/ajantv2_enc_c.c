/*
 * AJA NTV2 output device: options and muxer registration
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

#include "ajantv2_common_c.h"
#include "ajantv2_enc.h"

#define OFFSET(x) offsetof(struct ajantv2_cctx, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

/* The teletext options deliberately carry the same names, defaults and meanings
 * as the DeckLink output's, because the generator behind them is the same code.
 * A playout command line should move between the two cards by changing only the
 * format and the device name. */
static const AVOption options[] = {
    { "channel", "framestore to play from (1-based)", OFFSET(channel), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 8, ENC },
    { "output", "SDI connector(s) to feed, 1-based, comma-separated for more than one (e.g. \"2\" or \"1,2\"); default is the connector matching the framestore", OFFSET(output), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "ring_frames", "AutoCirculate output ring depth in frames", OFFSET(ring_frames), AV_OPT_TYPE_INT, { .i64 = 8 }, 2, 64, ENC },
    { "audio_depth", "audio bits per sample (the card embeds 32-bit; 16 is converted)", OFFSET(audio_depth), AV_OPT_TYPE_INT, { .i64 = 32 }, 16, 32, ENC },
    { "cc", "embed CEA-708 captions from A53 side data in HD VANC; off also keeps the VANC region out of the frame buffer, which is the plainest video path the card has", OFFSET(cc), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, ENC },
    { "output_buffer_size", "host-side output buffer in bytes (0 = hand each frame straight to the card, and let the card pace the pipeline)", OFFSET(output_buffer_size), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, ENC },
    { "preroll", "seconds of video buffered on the card before playback starts", OFFSET(preroll), AV_OPT_TYPE_DOUBLE, { .dbl = 0.5 }, 0, 5, ENC },
    { "late_threshold", "stop if the output falls this many seconds behind the card (0 = never stop)", OFFSET(late_threshold), AV_OPT_TYPE_DOUBLE, { .dbl = 5.0 }, 0, 3600, ENC },
    { "block_until_available", "wait for another application to release the card instead of failing", OFFSET(block_until_available), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "format_code", "NTV2 video format by name, e.g. \"1080i50\", \"1080p25\", \"625i50\"; needed when the raster and rate alone are ambiguous, as 1080i50 and 1080p25 are", OFFSET(format_code), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },

    { "teletext_fields", "teletext field insertion mode", OFFSET(teletext_fields), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, ENC, .unit = "teletext_fields" },
    { "both", "insert on both fields (default, per OP-47)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, ENC, .unit = "teletext_fields" },
    { "odd",  "insert on field 1 only",                     0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, ENC, .unit = "teletext_fields" },
    { "even", "insert on field 2 only",                     0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, ENC, .unit = "teletext_fields" },
    { "teletext_vbi_offset", "teletext VBI clock-run-in start sample (6 puts the OP-42 penultimate run-in '1' at 12.02us)", OFFSET(teletext_vbi_offset), AV_OPT_TYPE_INT, { .i64 = 6 }, 0, 200, ENC },
    { "teletext_vbi_offset_frac", "sub-sample part of teletext_vbi_offset, in 1/256ths of a sample", OFFSET(teletext_vbi_offset_frac), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 255, ENC },
    { "teletext_shape", "band-limit teletext eye: raised-cosine -6dB cutoff as %% of bit rate (0=raw square)", OFFSET(teletext_shape), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 200, ENC },
    { "teletext_rise_ns", "sine-squared teletext edge with this 10-90%% rise time in ns (0=off; overrides teletext_shape)", OFFSET(teletext_rise_ns), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 400, ENC },
    { "teletext_level", "teletext binary '1' level, %% of peak white", OFFSET(teletext_level), AV_OPT_TYPE_INT, { .i64 = 66 }, 40, 100, ENC },
    { "teletext_continuous", "retransmit each caption every frame instead of a burst-then-hold", OFFSET(teletext_continuous), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "teletext_burst_frames", "frames to retransmit a caption after each update", OFFSET(teletext_burst_frames), AV_OPT_TYPE_INT, { .i64 = 8 }, 1, 250, ENC },
    { "teletext_blank_idle", "leave the line blank when idle instead of sending filler", OFFSET(teletext_blank_idle), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "teletext_dual_field", "put a different page row on each field", OFFSET(teletext_dual_field), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "teletext_filler", "idle filler packet type", OFFSET(teletext_filler), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, ENC, .unit = "teletext_filler" },
    { "dummy", "page 8FF dummy header (OP-42 s8)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, ENC, .unit = "teletext_filler" },
    { "idl",   "packet 8/31 Independent Data Line", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, ENC, .unit = "teletext_filler" },
    { "teletext_filler_ctrl", "dummy filler header control bits C6/C7/C8/C9 = 1", OFFSET(teletext_filler_ctrl), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { "teletext_filler_subcode", "dummy filler page subcode", OFFSET(teletext_filler_subcode), AV_OPT_TYPE_INT, { .i64 = 0x3F7E }, 0, 0x3F7F, ENC },
    { "teletext_spare_p8ff", "fill a dual-field spare slot with a P8FF time-filling header", OFFSET(teletext_spare_p8ff), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, ENC },
    { "teletext_header_attr", "spacing attribute for a page header's first display character (-1 = as sent; 6 = Alpha Cyan)", OFFSET(teletext_header_attr), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 31, ENC },
    { "teletext_defer_erase", "don't send an erase at each caption's end", OFFSET(teletext_defer_erase), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, ENC },
    { NULL },
};

static const AVClass ajantv2_muxer_class = {
    .class_name = "AJA NTV2 outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_ajantv2_muxer = {
    .p.name           = "ajantv2",
    .p.long_name      = NULL_IF_CONFIG_SMALL("AJA NTV2 output"),
    .p.audio_codec    = AV_CODEC_ID_PCM_S32LE,
    .p.video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .p.subtitle_codec = AV_CODEC_ID_DVB_TELETEXT,
    .p.flags          = AVFMT_NOFILE,
    .p.priv_class     = &ajantv2_muxer_class,
    .priv_data_size   = sizeof(struct ajantv2_cctx),
    .write_header     = ff_ajantv2_write_header,
    .write_packet     = ff_ajantv2_write_packet,
    .write_trailer    = ff_ajantv2_write_trailer,
};
