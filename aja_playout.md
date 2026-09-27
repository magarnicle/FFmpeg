# Making AJA playout as stable as DeckLink playout

The AJA output plays HD with audio and CEA-708 captions, and it has survived a
minute without dropping a frame. That is not the same as being fit for playout.
The DeckLink output carries a set of features built over a long time for one
job: playing files out to air, continuously, with one process handing over to
the next. None of them exist on the AJA side.

This document says what each of those features does, what the AJA equivalent
would be, and where the two cards differ enough that the design has to change
rather than be copied.

## Where things stand

`libavdevice/ajantv2_enc.cpp` writes a frame like this, on the muxer thread:

1. Copy the packet into a host frame buffer.
2. Insert VANC.
3. Loop on `AutoCirculateGetStatus` until `CanAcceptMoreOutputFrames()`, waiting
   on `WaitForOutputVerticalInterrupt` in between.
4. `AutoCirculateTransfer` the frame and one frame's worth of audio.

Everything is synchronous. The thread that demuxes, decodes, filters and encodes
is the same thread that waits for the card. The card paces the whole pipeline.

That is what the DeckLink output used to do, and the reason it stopped.

## Why the current model is fragile

**The card-side buffer is small and cannot grow.** AutoCirculate plays from
frames in the card's own memory, and the KonaLHe+ has 128 MB of it. How far that
goes depends on the frame geometry, which the VANC setting changes:

| geometry | slot | frames | at 25 fps |
|---|---|---|---|
| `NTV2_FG_1920x1080` (no VANC) | 8 MB | 16 | 640 ms |
| `NTV2_FG_1920x1112` (VANC TALL) | 16 MB | 8 | 320 ms |
| `NTV2_FG_720x576` | 8 MB | 16 | 640 ms |

With captions on and the audio buffer taking its share, the HD ring came down to
seven frames: 280 ms. Every hiccup anywhere in the pipeline has to fit inside
that, or the card runs out of frames and repeats the last one.

**A stall anywhere stalls everything.** Because the muxer thread blocks on the
card, a slow disk read, a segment boundary in a concat, or a filtergraph spike
stops frames reaching the card at all. On DeckLink this was not theoretical: a
CPU spike in the filtergraph at a concat boundary starved the output thread long
enough to trip the late-frame abort, and the fix was a comment and a scheduling
policy that are both still in `decklink_enc.cpp`.

**Nothing notices lateness.** The device reports `GetDroppedFrameCount()` at the
end of a run and never looks at it during one. There is no policy for a frame
that arrives after its time, no threshold, and no way to tell an operator that
playout is slipping.

**No hand-over.** The device takes the card, holds it, and gives it back at
exit. Two processes cannot arrange between themselves that one starts as the
other finishes.

## The features to port

### 1. Output buffer (`-output_buffer_size`)

**DeckLink.** A byte-bounded host-side queue per stream, `output_video_queue`
and `output_audio_queue`, with a consumer thread each. `write_packet` puts into
the queue and returns; the consumer threads do the scheduling. The put blocks
when the queue is full (`decklink_output_queue_put_blocking`), so the buffer
applies backpressure upstream instead of dropping. Sizes are checked against
available RAM at open. The default is 0, meaning the feature is off.

**AJA.** The same idea, and the AJA needs it more, because its card-side buffer
is 280 ms against DeckLink's host-side buffer of however many gigabytes you
allow. This is the single highest-value item.

It should be simpler than DeckLink's, because AutoCirculate carries audio with
the video frame in one `AutoCirculateTransfer`. There is no separate audio
schedule to race against, which is also why `-audio_schedule_retry` has no
counterpart here. So: one video packet queue, one consumer thread, and the
existing `AVFifo` for audio, which then needs a mutex because the muxer thread
fills it and the consumer thread drains it.

### 2. Output thread priority

**DeckLink.** `decklink_set_output_thread_priority()` promotes each output
thread to `SCHED_FIFO`, with audio one step above video, falling back to a
negative nice value and a loud warning when the process lacks `CAP_SYS_NICE` or
a non-zero `RLIMIT_RTPRIO`.

**AJA.** Copy it as is. It is card-independent and the reasoning behind it
applies unchanged. The production grant is a known deployment requirement.

### 3. Late-frame policy (`-late_threshold`)

**DeckLink.** Before scheduling, compare the packet's pts against
`GetScheduledStreamTime()`. Behind: warn, count, drop the frame. Further behind
than `late_threshold` seconds: abort the output with `EIO`, on the grounds that
silently playing hours of late video is worse than stopping.

**AJA.** This needs redesigning rather than porting, because AutoCirculate has no
equivalent of a scheduled stream time. DeckLink is told *when* each frame should
play; AutoCirculate is handed frames and plays them in order at the card's rate.
There is no pts to compare against a clock.

Three signals are available instead:

- `AUTOCIRCULATE_STATUS::GetBufferLevel()`, the frames the card still has in
  hand. Falling towards zero is the early warning.
- `GetDroppedFrameCount()`, which on output counts the times the card had no new
  frame and repeated the last one. This is the underrun counter and the device
  already reads it, once, at exit.
- Wall clock against `frames_out`. Once playback is running the card consumes
  exactly one frame per frame period, so expected frames since start is a
  reliable clock.

A workable policy: warn on any rise in the dropped count, and abort when the
deficit between expected and delivered frames exceeds `late_threshold`.

### 4. Preroll (`-preroll`)

**DeckLink.** Seconds of video buffered before playback starts, converted to a
frame count. With an async buffer configured the preroll is deliberately kept
small, two frames, because the large host buffer provides the timing safety and
a short preroll gets to air faster.

**AJA.** Already there in spirit: `aja_start_output` fills three frames before
calling `AutoCirculateStart`. It should become an option with the same name and
meaning, and the same interaction with the output buffer.

### 5. Block until available (`-block_until_available`)

**DeckLink.** `EnableVideoOutput` returns `E_ACCESSDENIED` while another process
holds the device. With this set, retry instead of failing, so an incoming
process can be started before the outgoing one has let go.

**AJA.** `AcquireStreamForApplication(fourcc, pid)` returns false when another
application holds the card. **The device currently ignores that return value**,
which means two ffmpeg processes can both believe they own the card. That is a
bug to fix regardless of this feature: check it, fail by default, and retry when
`block_until_available` is set.

### 6. Pre-render (`-pre_render`, `-pre_render_until`, `-pre_render_frames`)

**DeckLink.** The heart of the hand-over. Device initialisation is deferred: the
process starts, decodes and filters, and fills the output buffer while another
process still owns the card. A trigger thread watches for either a frame count
(`pre_render_frames`) or a wall-clock time (`pre_render_until`), and only then
opens the device, enables output and starts playback. The device can be
pre-opened earlier to shorten the critical path, and the code measures the gap
from trigger to device-ready to first-frame-on-air.

**AJA.** The structure ports directly, with the same trigger thread, the same
two triggers and the same deferred initialisation. The AJA specifics are that
"initialise the device" means the sequence in `aja_setup_video` plus
`aja_start_output`, and that acquiring the card is the step that can block.

### 7. Cross-process coordination (`-shm_*`, `-socket_*`)

**DeckLink.** Two mechanisms, both for getting frames from one process into the
process that owns the card.

`decklink_shm.c` is a POSIX shared-memory ring: a header with
`PTHREAD_PROCESS_SHARED` mutex and condition variables, a slot count
(`shm_max_frames`, default 60) and fixed-size frame slots. One instance runs as
server (`shm_server`, the one holding the card), others as clients
(`shm_client`, the ones encoding). `shm_block` decides whether a client waits or
gives up when the ring is full.

`socket_path` and `socket_listen` do the same job over a Unix socket, for frames
arriving from something that is not another ffmpeg.

**AJA.** Neither is card-specific, and both now live in
`libavdevice/outdev_feed.c`, in the same way the teletext generator was pulled
out into `vbi_teletext.c`. The DeckLink output uses that copy too, so there is
one implementation rather than two to drift apart, and the wire formats are
unchanged: anything already feeding the DeckLink output over either transport
works against the AJA one without knowing which card is on the end.

The socket server took a callback to make it card-independent. The shared memory
module needed only renaming; it never had a DeckLink type in it.

Two things are simpler on the AJA side. There is no pts rebasing when a client
reconnects mid-run, because AutoCirculate plays frames in the order they arrive
and pays no attention to timestamps. And audio goes into the device's existing
FIFO rather than a second queue, for the same reason the output buffer has only
one queue.

## Where the AJA differs enough to matter

**The retail service will take the card during a hand-over.** This is the
AJA-specific hazard with no DeckLink counterpart, and it is the one most likely
to produce something ugly on air. The device restores the previous task mode at
exit:

```c
if (NTV2_IS_VALID_TASK_MODE(ctx->saved_task_mode))
    ctx->device.SetTaskMode(ctx->saved_task_mode);
```

For a single run that is good manners. In a hand-over it means that between the
outgoing process releasing the card and the incoming one taking it, the card
goes back to `NTV2_STANDARD_TASKS` and the AJA retail service reprograms it from
the control panel's settings. That is exactly the failure that produced green
garbage for most of 25 September, and here it would land in the gap between two
programmes.

The fix is a policy rather than a line of code: the outgoing process should not
restore standard tasks when it is handing over, or the incoming process should
take OEM mode before the outgoing one lets go, or both. Whichever, it needs an
option and it needs testing with two real processes.

**What the card shows between processes.** DeckLink holds the last scheduled
frame when scheduled playback stops. What the AJA framestore shows after
`AutoCirculateStop` has not been checked. It probably keeps displaying its
current output frame, which would be a freeze rather than a black, but that is a
guess and the hand-over design depends on it.

**Frames, not time.** Worth restating because it shapes several of the items
above: DeckLink is a scheduling API and AutoCirculate is a queue. Anything in
the DeckLink code that reasons about stream time has to be rebuilt around frame
counts and buffer levels.

**Genlock.** The device sets `NTV2_REFERENCE_FREERUN`, which is right for a test
and wrong for a playout rig. Free-running, the card's frame rate and the file's
nominal frame rate differ by the crystal error, so over hours the output buffer
drifts in one direction until something drops or repeats. The LHe+ has a
reference input; a real installation should genlock to house reference, and the
reference source should be an option.

## Two questions nobody has answered yet

Both of these are decisions rather than work, and neither is settled. They are
recorded here so they are not lost, and so that whoever picks them up knows they
were left open on purpose rather than overlooked.

**Should HD go back to `NTV2_VANCMODE_TALLER`?** On this card TALLER costs an
8 MB frame slot and TALL costs 16 MB, so TALLER gives sixteen frames where TALL
gives eight. The move to TALL was a guess made while chasing a fault that turned
out to be routing, and it has never been justified on its own terms. The counter
-argument is that TALL is the ordinary HD VANC geometry and TALLER is unusual.
Deciding needs a run of each with the output buffer in place, since a host-side
buffer may make the ring depth stop mattering.

**Should the rig genlock, or free-run?** The device sets
`NTV2_REFERENCE_FREERUN`, which is right for a test and probably wrong for
playout. Free-running, the card's clock and the file's nominal frame rate differ
by the crystal error, so the buffer drifts one way until something drops or
repeats. The LHe+ has a reference input. Answering this needs a long run to see
which way and how fast it drifts, and a decision about what the rest of the
facility is referenced to.

## Suggested order

1. ~~Check the return of `AcquireStreamForApplication`.~~ **Done.** Fails with
   `EBUSY` now instead of pretending to own the card.
2. ~~Output buffer and consumer thread, with the thread priority promotion.~~
   **Done.** `-output_buffer_size`, default 0, which keeps the old synchronous
   behaviour.
3. ~~Late-frame policy built on buffer level and dropped count.~~ **Done.**
   `-late_threshold`, default 5 seconds.
4. ~~`preroll` as an option, tied to the buffer as DeckLink ties it.~~ **Done.**
   `-preroll`, default 0.5 seconds, capped at one frame short of the ring.
5. ~~`block_until_available`.~~ **Done.**
6. ~~Pre-render, including the task-mode hand-over policy.~~ **Done.**
   `-pre_render`, `-pre_render_until`, `-pre_render_frames` and
   `-restore_task_mode`. What the framestore shows between processes is still
   unknown and still needs a card to answer.
7. ~~Shared memory and socket input, after pulling DeckLink's implementation
   out into something both devices use.~~ **Done.** `libavdevice/outdev_feed.c`
   now holds both, and the DeckLink output uses it rather than its own copy.

Items 1 to 7 are written but have not been near a card. Nothing about them has
been tested beyond compiling, and the concurrency in item 2 is the kind that
looks right and is not. The first run with `-output_buffer_size` should be a
short one with something disposable on the output.

Items 1 to 4 make single-process playout dependable. Items 5 to 7 are what makes
two processes able to hand over.

Pre-render deliberately refuses two combinations. Without `-output_buffer_size`
there is nowhere for frames to go while the card belongs to someone else, so it
is required. And teletext is refused outright, because the generator is
configured from the video format, which is not known until the card is claimed,
while subtitle packets arrive before that; the SD teletext path runs on the
DeckLink anyway.

One consequence worth knowing: if the trigger never fires, the buffer fills and
the muxer thread blocks in the put, so ffmpeg waits rather than exits. That is
the intended behaviour for a scheduled start, and it means a `-t`-limited test
run with a trigger time in the future will appear to hang.

## What still needs measuring

- A long run. Sixty seconds with no dropped frames says nothing about an hour.
  The drift question in particular cannot be answered in a minute.
- Whether seven frames of card-side ring is enough once a host-side buffer is
  feeding it. See the geometry question above.
- What the framestore outputs between `AutoCirculateStop` and the next process
  starting. This is the one that decides whether a hand-over looks like a freeze
  or like a fault, and no amount of reading answers it.
- Whether `-restore_task_mode 0` is enough on its own, or whether the incoming
  process also has to take OEM mode before the outgoing one lets go.
