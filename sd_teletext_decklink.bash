#!/bin/bash
# SD PAL with teletext, from an HD feed carrying CEA-608 in VANC.
#
# The downstream half of the playout chain: hd_caps_aja.bash puts 1080p25 out
# of an AJA card with captions embedded on VANC line 11, this takes that in on
# a DeckLink and puts 625i50 with WST teletext on air. Proven end to end on
# 2026-09-25.
#
# The caption path is a transcode, not a copy:
#
#   -eia608 1 makes the captions the input already lifts out of HD VANC into a
#   stream -map can point at. Without it they exist only as A53 side data on the
#   video packets, which is what re-embedding wants and re-encoding cannot use.
#
#   -c:s dvb_teletext then runs eia_608 -> cc_dec -> teletextenc. cc_dec emits
#   ASS, so teletextenc strips the override blocks rather than putting {\an7} on
#   air. -teletext_page and -double_height are encoder options and go before
#   -f decklink; everything teletext_* after it belongs to the output device and
#   shapes the VBI waveform.
#
# One SDI 4K card captures and plays out at once. If a card refuses, try
# -duplex_mode half on the output before reaching for a second card.
#
# The teletext_* values are the working configuration from plan_21.txt, arrived
# at by measuring our VBI against a Polistream capture. Do not change one
# without reading that file.
#
# Known rough edge: the output clock runs while the input is still probing, so
# the first few frames arrive late and are dropped before it catches up. Raise
# -preroll if that matters.

DEV=${DEV:-DeckLink SDI 4K}

/home/matt/ffmpeg_sources/FFmpeg/ffmpeg -loglevel debug -nostdin -hide_banner \
    -f decklink -eia608 1 -format_code Hp25 -raw_format yuv422p10 -i "$DEV" \
    -filter_complex \
    "[0:v]scale=720:576:interl=1, setdar=16/9, format=yuv422p10le, setfield=tff[v]" \
    -map '[v]' -map 0:a -map 0:s \
    -c:v v210 \
    -c:s dvb_teletext -teletext_page 801 -double_height 1 \
    -f decklink \
        -teletext_rise_ns 86 \
        -teletext_vbi_offset 5 -teletext_vbi_offset_frac 159 \
        -teletext_dual_field 1 \
        -teletext_filler idl -teletext_spare_p8ff 1 \
        -teletext_filler_ctrl 1 -teletext_filler_subcode 0 \
        -teletext_header_attr 6 \
        -teletext_burst_frames 25 \
        "$DEV"
