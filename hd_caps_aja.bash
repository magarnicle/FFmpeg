#!/bin/bash
# HD playout to an AJA card at 1080p25, with CEA-608 captions embedded in VANC
# for a downstream machine to turn into SD teletext.
#
# Differences from hd_caps.bash, which plays the same thing out of a DeckLink:
#
#   -f ajantv2 and a device index instead of a DeckLink device name. The AJA
#   device takes the index as the output argument; "0" is the first card.
#   -channel picks the framestore. No -output, so the picture goes to every SDI
#   connector the card has. Naming one is only worth it when something else
#   needs the other connector: the monitor on this machine is on SDI Out 2, and
#   feeding SDI 1 alone looks exactly like a dead card.
#
#   -format_code 1080p25 states the video format. It has to be said rather than
#   inferred: 1080i50 and 1080p25 are both 1920x1080 at 25 frames, and which one
#   goes down the wire is a different SDI standard, so the raster and the rate do
#   not identify it. Without this the device guesses and warns.
#
#   Progressive, so fieldorder=tff and setfield=tff are gone. Those made the
#   DeckLink version interlaced; leaving them in would contradict 1080p25.
#
#   No -output_buffer_size and no -preroll. Those are DeckLink options and the
#   AJA device does not have them yet -- it has no asynchronous output buffer,
#   preroll or late-frame policy, so expect it to drift on a long run.
#
#   No -map [caption]. The DeckLink output takes captions two ways: embedded in
#   the video frames by cea608embed, and as a separate EIA-608 subtitle stream.
#   The AJA device reads only the first, from the A53 side data cea608embed
#   attaches, so the subtitle goes into the filter and nowhere else, and ssplit
#   is not needed.
#
#   Captions are on, which is the point of this script. They come from the A53
#   side data cea608embed attaches, go out as a CEA-708 CDP on VANC line 11, and
#   need the VANC region present in the frame buffer -- so the frame geometry is
#   1920x1112 here and 1920x1080 without them. If the picture is good with
#   -cc 0 and wrong without it, that geometry is where to look.

/home/matt/ffmpeg_sources/FFmpeg/ffmpeg -loglevel debug -nostdin -hide_banner -noautorotate -fflags +discardcorrupt \
    -i $HOME/Videos/AU111999-M-Preview.mov -an \
    -i $HOME/Pictures/good.mov \
    -i /Volumes/sydneyplayout/captions/1/1/1/9/9/9/AU111999-M.srt -filter_complex \
    "
[0:a]volume=volume=-0dB[audio0];
    [0:v]scale='trunc(ih*dar):ih', scale=1920:1080:force_original_aspect_ratio=decrease, pad=1920:1080:-1:-1:color=black, setdar=16/9, setpts=PTS-STARTPTS, fps=fps=25, format=yuv444p10le [video0];
    [1:v]tpad=stop_mode=clone:stop=-1:stop_duration=50.0, trim=duration=50.0, setpts=PTS-STARTPTS+0.0/TB, fps=fps=25[graphic0.0.time];
    [video0][graphic0.0.time] overlay=x=1400: y=20: alpha=straight:format=yuv422p10: repeatlast=0[videobug];
    [videobug][2:s]cea608embed=continuous=1:keepalive=1[videoout]" \
    -map [videoout] -map [audio0] \
    -metadata name='2026-09-25 10:52:52.040000+10:00 AU112301-M.mov' \
    -c:v v210 -f ajantv2 -format_code 1080p25 -channel 1 0
