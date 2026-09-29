#!/bin/bash
# Capture what the AJA is sending and count the faults in it.
#
# Run aja_test_playout.bash on the playout machine first. This records the tone
# from the SDI and then asks where it stopped. Every gap is printed with the
# time it happened, so the spacing says what kind of fault it is: a regular
# interval points at something structural in the playout path, an irregular one
# at scheduling or load.
#
# The threshold is -50dB over 2ms. The source is a full-level sine, so anything
# quieter than that for longer than two milliseconds is a dropout and not
# programme material.

FFMPEG=${FFMPEG:-./ffmpeg}
DEV=${DEV:-DeckLink SDI 4K}
OUT=${1:-/tmp/aja_test}
SECS=${2:-30}

echo "Recording ${SECS}s from '$DEV' to $OUT.wav"
$FFMPEG -nostdin -hide_banner -y \
    -f decklink -format_code Hp25 -raw_format yuv422p10 -i "$DEV" \
    -t "$SECS" -map 0:a -c:a pcm_s24le "$OUT.wav" || exit 1

echo
echo "Gaps in the tone:"
$FFMPEG -nostdin -hide_banner -i "$OUT.wav" \
    -af silencedetect=n=-50dB:d=0.002 -f null - 2>&1 |
  awk '/silence_start/ {s=$NF; print "  gap at " s "s"
                        if (p != "") printf "    %.3fs since the last one\n", s-p
                        p=s}
       END {if (p=="") print "  none"}'
