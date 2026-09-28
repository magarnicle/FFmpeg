# No -output_buffer_size here, and it is not an oversight.
#
# A host-side output buffer is a playout-from-files feature. Reading a file, the
# producer can run faster than realtime, so the buffer fills once and then
# absorbs any later stall without the output ever noticing.
#
# This is live in, live out. The producer is a capture card, so it can never run
# faster than realtime and can never refill a buffer it has drawn down. All a
# buffer can do here is add latency, and because the DeckLink output schedules
# by pts against the card's own clock, that latency is fatal: a frame that waits
# in the buffer is late by exactly the time it waited.
#
# With 200MiB it deadlocked. The buffer filled to 95 frames during startup while
# the input was probing, and from then on every single frame arrived 97 frames
# -- 3.88s -- behind the card, was dropped as late, and nothing went to air for
# the rest of the run:
#
#   Async buffer: 105835112 / 209715200 bytes (50.5%), 95 video + 98 audio
#   Dropping late video frame: pts=119 < stream=216 (3.88s behind)
#   Dropping late video frame: pts=192 < stream=289 (3.88s behind)
#
# The gap never moved, because dropping consumes frames at exactly the rate they
# arrive. It also never reached -late_threshold's 5 seconds, so nothing failed
# and nothing said so beyond the drop lines.
./ffmpeg -f decklink -eia608 1 -format_code Hp25 -raw_format yuv422p10 \
       -i 'DeckLink SDI 4K' \
  -filter_complex "[0:v]scale=720:576:interl=1,setdar=16/9,format=yuv422p10le,setfield=tff[v]" \
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
     'DeckLink SDI 4K'
