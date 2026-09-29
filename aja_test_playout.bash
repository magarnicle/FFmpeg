#!/bin/bash
# Known-good signal out of the AJA, so a playout fault is obvious rather than
# something to listen for.
#
# Bars carry motion and fine detail, so tearing, a wrong field order and a
# dropped frame all show. The tone is the point of it though: one frequency at
# one level, so any interruption is a gap in a flat waveform that a machine can
# find and count. Programme audio hides that; a sine does not.
#
# -ac 2 because lavfi's sine is mono and the card embeds pairs. The upmix just
# puts the same tone on both channels, which is what a gap test wants.
#
# The card options match hd_caps_aja.bash, captions included, because the VANC
# region changes the frame geometry and with it the ring depth -- seven frames
# against sixteen -- and a playout test on the wrong geometry is not a test of
# playout. There is no caption source here, so the VANC line simply stays blank.
#
# Pair with aja_test_capture.bash on the machine with the DeckLink.

FFMPEG=${FFMPEG:-/home/matt/ffmpeg_sources/FFmpeg/ffmpeg}
buffer_size=$(( 200 * 1024 * 1024 ))

$FFMPEG -nostdin -hide_banner \
    -f lavfi -i "testsrc2=s=1920x1080:r=25" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -map 0:v -map 1:a \
    -c:v v210 -c:a pcm_s32le -ac 2 \
    -f ajantv2 -format_code 1080p25 -channel 1 \
    -output_buffer_size ${buffer_size} 0
