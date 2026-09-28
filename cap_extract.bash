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
#
# -max_interleave_delta is what keeps the picture moving, and without it the
# output stutters in a way that looks like the machine being too slow.
#
# The muxer holds packets until every interleaved stream has one
# (mux.c: nb_interleaved_streams == stream_count). Three streams go out here,
# and teletext is sparse: one packet per caption, seconds apart. So video and
# audio pile up in the muxing queue until the next caption releases them in a
# burst. Measured on 28 September: a steady 24-26fps for seven seconds, then
# nothing for 3.5s, then 33 frames in one report, then nothing for 2.5s, then 64
# frames. The average was realtime; the delivery was not.
#
# 200000 is 0.2 seconds, and the value sits between two failure modes.
#
# Too large (the 10s default) and video waits for the next caption, which is the
# stall above. Too small and it deadlocks: with 1 microsecond every packet goes
# out the moment it exists, audio no longer advances in lockstep with video, and
# it fills the card's audio preroll buffer before video has reached
# first_pts + frames_preroll. ScheduleAudioSamples blocks, video stops, and
# StartScheduledPlayback is never called. Measured on 28 September: wedged at
# frame 10 of a 12-frame preroll, 17 input buffer overruns, no "Starting
# scheduled playback" in the log at all.
#
# 0 is not the answer either: it disables the override and waits forever. The
# negative-timestamp warning in mux.c recommends 0 for an unrelated problem and
# is easy to follow by mistake.
#
# At 0.2s video is never held up for longer than that, which the card's 60-frame
# scheduling queue absorbs, and audio can never be more than that far ahead,
# which its preroll buffer holds comfortably.
#
# hd_caps_aja.bash does not need this. Its captions go into the filter chain as
# A53 side data on the video frames, so its only output streams are video and
# audio and neither is sparse.
#
# No interl=1 on the scale, and that matters. It tells swscale the picture is
# interlaced, so it splits each frame into two fields and scales them
# separately. That is right for a 1080i50 source and wrong for this one:
#
#   Stream #0:1: Video: v210, yuv422p10le(progressive), 1920x1080
#
# The AJA sends 1080p25. Scaling a progressive frame as two fields takes every
# other line, scales the halves independently and interleaves them again, so
# adjacent output lines come from differently phased samples of the same
# picture. Horizontal edges shimmer and fine detail crawls -- it looks badly
# interlaced because it has been interlaced, by the scaler, for no reason.
#
# Scaled whole and then marked tff, both fields of each PAL frame come from the
# same instant, which is what a 25p source carried as 576i25 should be. If the
# upstream ever changes to 1080i50, interl=1 comes back.
#
# -preroll is left at its 0.5s default, and raising it here does not work.
#
# A longer preroll looks like the answer to the handful of frames dropped at
# startup: playback begins at the first frame's pts once -preroll seconds of
# frames are scheduled, so the preroll is the cushion. But without an async
# output buffer, video and audio are both written from the muxer thread, and the
# card's audio preroll buffer fills long before 1.5s of video is scheduled.
# ScheduleAudioSamples then blocks, no more video arrives, the pts never reaches
# the threshold, and StartScheduledPlayback is never called. Tried on
# 28 September:
#
#   frame=103 fps=4.6 fps_inst=0x speed=0.19x     (wedged)
#   Decklink input buffer overrun!                (x20, nothing consuming)
#   Starting scheduled playback.                  (never logged)
#
# So the startup drops stay. There were eight of them on 25 September, all in
# the first second and a half, and then it ran clean. For a pass-through that is
# the cheaper of the two problems.
./ffmpeg -f decklink -eia608 1 -format_code Hp25 -raw_format yuv422p10 \
       -i 'DeckLink SDI 4K' \
  -filter_complex "[0:v]scale=720:576,setdar=16/9,format=yuv422p10le,setfield=tff[v]" \
  -map '[v]' -map 0:a -map 0:s \
  -c:v v210 \
  -c:s dvb_teletext -teletext_page 801 -double_height 1 \
  -max_interleave_delta 200000 \
  -f decklink \
     -teletext_rise_ns 86 \
     -teletext_vbi_offset 5 -teletext_vbi_offset_frac 159 \
     -teletext_dual_field 1 \
     -teletext_filler idl -teletext_spare_p8ff 1 \
     -teletext_filler_ctrl 1 -teletext_filler_subcode 0 \
     -teletext_header_attr 6 \
     -teletext_burst_frames 25 \
     -preroll 0.7 \
     'DeckLink SDI 4K'
