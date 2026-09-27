/*
 * AJA NTV2 output device
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

/*
 * Video and SD PAL teletext out of an AJA card, as a counterpart to the
 * DeckLink output.
 *
 * All the teletext behaviour is in libavdevice/vbi_teletext.c and is shared
 * with the DeckLink output, so the two cards put the same waveform and the same
 * page structure on air. This file only has to put the rendered line somewhere,
 * and on AJA that is easier than on DeckLink: with the VANC mode set to TALLER
 * the vertical blanking lines are simply rows of the frame buffer, so line 21
 * is written by writing into the buffer at the right row rather than through a
 * separate ancillary interface.
 *
 * What is here: 625i50 video in v210 or UYVY, and teletext on lines 21 and 334.
 *
 * What is not here yet: audio, the asynchronous output buffer and pre-render
 * machinery the DeckLink output has, genlock selection, and HD modes with
 * OP-47 teletext in VANC. The frame path is deliberately the simple one --
 * AutoCirculate with a ring, one transfer per frame -- so that the first thing
 * proven on a card is the picture and the captions.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2devicefeatures.h"
#include "ntv2formatdescriptor.h"
#include "ntv2utils.h"
#include "ajabase/system/systemtime.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavcodec/packet_internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "libavutil/parseutils.h"
#include "libavutil/frame.h"

#include "ajantv2_common_c.h"
#include "ajantv2_enc.h"
#include "vbi_teletext.h"
}

/* After the block above, because CONFIG_LIBKLVANC comes from config.h and is
 * not defined until one of those headers has pulled it in. */
#if CONFIG_LIBKLVANC
#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"
#endif

/* SD teletext lives on line 21 of field 1 and the line 313 further on in
 * field 2, per OP-42 and Australian practice. */
#define AJA_SD_LINE_FIELD1  21
#define AJA_SD_LINE_FIELD2  334

/* SMPTE 334 puts CEA-708 in HD VANC on line 9 or 11; the DeckLink output uses
 * 11, so the two cards agree. */
#define AJA_HD_LINE_CC      11

#if CONFIG_LIBKLVANC
#define AJA_CC_ENABLED(c) ((c)->cc_enabled)
#else
#define AJA_CC_ENABLED(c) 0
#endif

/* Host-side output buffer.
 *
 * Without one, the thread that demuxes, decodes, filters and encodes is also
 * the thread that waits on the card, so the card paces the whole pipeline and
 * every stall anywhere in it reaches the output. The card's own elasticity is
 * whatever the AutoCirculate ring holds, which on a KonaLHe+ playing HD with
 * VANC is seven frames -- 280ms. A slow read or a filtergraph spike at a concat
 * boundary does not fit in that.
 *
 * So packets go into this queue and a consumer thread hands them to the card.
 * The queue is bounded by bytes and the put blocks when it is full, which
 * pushes back upstream rather than dropping. Same shape as the DeckLink
 * output's, minus the second queue: AutoCirculate carries audio with the video
 * frame in one transfer, so there is nothing to schedule separately. */
struct AjaPacketQueue {
    PacketList       pkt_list;
    int              nb_packets;
    int64_t          size;
    int64_t          max_size;
    int              stop;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
};

struct ajantv2_ctx {
    AVFormatContext    *avctx;
    AVStream           *video_st;
    CNTV2Card           device;
    NTV2Channel         channel;              /* framestore */
    std::vector<NTV2Channel> outputs;          /* SDI connectors fed from it */
    NTV2VideoFormat     video_format;
    NTV2PixelFormat     pixel_format;
    NTV2VANCMode        vanc_mode;
    NTV2FormatDescriptor fd;
    ULWord              frame_size;
    ULWord              ring_first;
    ULWord              ring_last;

    NTV2Buffer          host_buffer;
    AUTOCIRCULATE_TRANSFER xfer;

    /* Audio. The card takes one frame's worth with each video frame, so packets
     * are accumulated here and drawn down a frame at a time. */
    AVStream           *audio_st;
    NTV2AudioSystem     audio_system;
    int                 audio_channels;
    int                 audio_bytes_per_sample;   /* on the card: always 4 */
    int                 audio_frame_bytes;        /* one video frame's worth */
    AVFifo             *audio_fifo;
    NTV2Buffer          audio_buffer;
    int64_t             audio_underruns;

    NTV2TaskMode        saved_task_mode;  /* to hand the card back as found */

    int64_t             last_pts;
    int                 frames_out;
    int                 preroll_frames;   /* transfers to do before starting */
    int                 started;          /* AutoCirculate is running */
    int                 cdp_tb_num, cdp_tb_den;  /* frame duration, for the CDP */

    /* Output buffer and its consumer thread. buffered is 0 when
     * -output_buffer_size is not set, and then everything below is unused and
     * the muxer thread hands frames to the card itself. */
    int                 buffered;
    AjaPacketQueue      queue;

    /* Pre-render. The card is not touched until the trigger fires, so frames
     * pile up in the queue while another process is still on air. */
    int                 pre_render;
    int64_t             trigger_time_us;   /* wall clock, 0 = no time trigger */
    int                 trigger_frames;    /* queue depth, 0 = no count trigger */
    int64_t             fired_us;
    volatile int        device_ready;
    pthread_t           trigger_thread;
    int                 trigger_started;
    volatile int        trigger_stop;

    pthread_t           thread;
    int                 thread_started;
    int                 thread_stop;
    int                 thread_error;

    /* The audio FIFO and the teletext generator are filled by the muxer thread
     * and drained by the consumer thread. */
    pthread_mutex_t     audio_lock;
    pthread_mutex_t     tt_lock;

    /* Late detection. The card consumes exactly one frame per frame period once
     * it is running, so wall clock since playback started says how many frames
     * it has asked for, and frames_out says how many it got. */
    int64_t             playback_start_us;
    int64_t             frames_at_start;
    ULWord              last_dropped;
    int64_t             late_logged_us;
    double              late_threshold;

    /* Teletext, all of it device-independent. */
#if CONFIG_LIBKLVANC
    struct klvanc_context_s *vanc_ctx;
    uint16_t             cdp_sequence_num;
    int                  cc_enabled;
#endif

    FFVbiTeletextContext tt;
    AVStream            *teletext_st;
    int                  teletext_enabled;
    ULWord               tt_row_f1;
    ULWord               tt_row_f2;
};

/* --- output buffer -------------------------------------------------------- */

static void *aja_output_thread(void *arg);

static void aja_queue_init(AjaPacketQueue *q, int64_t max_size)
{
    memset(&q->pkt_list, 0, sizeof(q->pkt_list));
    q->nb_packets = 0;
    q->size       = 0;
    q->max_size   = max_size;
    q->stop       = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void aja_queue_flush(AjaPacketQueue *q)
{
    AVPacket pkt;

    pthread_mutex_lock(&q->mutex);
    while (avpriv_packet_list_get(&q->pkt_list, &pkt) == 0)
        av_packet_unref(&pkt);
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void aja_queue_destroy(AjaPacketQueue *q)
{
    aja_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int aja_queue_depth(AjaPacketQueue *q)
{
    int n;
    pthread_mutex_lock(&q->mutex);
    n = q->nb_packets;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

/* Wake anything waiting, so a put or a get in progress gives up. */
static void aja_queue_stop(AjaPacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->stop = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/* Blocks while the queue is full. That backpressure is the point: it paces the
 * pipeline against the card without the card pacing it directly. */
static int aja_queue_put(AjaPacketQueue *q, AVPacket *pkt)
{
    const int size = pkt->size;
    int ret;

    if (av_packet_make_refcounted(pkt) < 0) {
        av_packet_unref(pkt);
        return AVERROR(ENOMEM);
    }

    pthread_mutex_lock(&q->mutex);
    while (q->size >= q->max_size && !q->stop)
        pthread_cond_wait(&q->cond, &q->mutex);
    if (q->stop) {
        pthread_mutex_unlock(&q->mutex);
        av_packet_unref(pkt);
        return AVERROR(EIO);
    }
    ret = avpriv_packet_list_put(&q->pkt_list, pkt, NULL, 0);
    if (ret < 0) {
        av_packet_unref(pkt);
    } else {
        q->nb_packets++;
        q->size += size + (int64_t)sizeof(AVPacket);
    }
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

/* Returns 1 with a packet, or 0 when the queue is empty and stopping. */
static int aja_queue_get(AjaPacketQueue *q, AVPacket *pkt)
{
    pthread_mutex_lock(&q->mutex);
    for (;;) {
        if (avpriv_packet_list_get(&q->pkt_list, pkt) == 0) {
            q->nb_packets--;
            q->size -= pkt->size + (int64_t)sizeof(AVPacket);
            pthread_cond_broadcast(&q->cond);
            pthread_mutex_unlock(&q->mutex);
            return 1;
        }
        if (q->stop) {
            pthread_mutex_unlock(&q->mutex);
            return 0;
        }
        pthread_cond_wait(&q->cond, &q->mutex);
    }
}

/* Promote the output thread to SCHED_FIFO so the realtime playout cannot be
 * starved by the SCHED_OTHER decode and filter workers. This is
 * decklink_set_output_thread_priority() with the names changed; the reasoning
 * is in the comment there, and it came from a CPU spike at a concat boundary
 * starving the DeckLink output thread past its late threshold. Only one thread
 * here, since audio rides with the video frame.
 *
 * SCHED_FIFO needs CAP_SYS_NICE or a non-zero RLIMIT_RTPRIO. Without it,
 * fall back to a negative nice and say so loudly. Priority never fails the
 * output. */
static void aja_set_output_thread_priority(AVFormatContext *avctx)
{
    const int min_prio = sched_get_priority_min(SCHED_FIFO);
    const int max_prio = sched_get_priority_max(SCHED_FIFO);
    struct sched_param param;
    int ret;

    if (min_prio < 0 || max_prio < 0) {
        av_log(avctx, AV_LOG_WARNING,
               "Output thread: SCHED_FIFO unavailable here; default priority.\n");
        return;
    }
    memset(&param, 0, sizeof(param));
    param.sched_priority = FFMIN(min_prio + 2, max_prio);

    /* pthread_setschedparam returns an errno value; it does not set errno. */
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (ret == 0) {
        av_log(avctx, AV_LOG_INFO,
               "Output thread: SCHED_FIFO priority %d set for realtime output.\n",
               param.sched_priority);
        return;
    }
    if (ret == EPERM) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -10) == 0)
            av_log(avctx, AV_LOG_WARNING,
                   "Output thread: no permission for SCHED_FIFO (needs "
                   "CAP_SYS_NICE or a non-zero RLIMIT_RTPRIO); fell back to "
                   "nice -10. Grant realtime scheduling or expect the card to "
                   "run dry under load.\n");
        else
            av_log(avctx, AV_LOG_WARNING,
                   "Output thread: no permission for SCHED_FIFO and the nice "
                   "fallback failed (%s); default priority. Expect the card to "
                   "run dry under load.\n", strerror(errno));
        return;
    }
    av_log(avctx, AV_LOG_WARNING,
           "Output thread: could not set SCHED_FIFO (%s); default priority.\n",
           strerror(ret));
}

/* Pick the frame buffer format from the codec, the way the DeckLink output does.
 * The pixel format of an encoded stream is not reliable at write_header time --
 * a v210 stream has been seen reporting yuv422p there -- so the codec is what
 * decides, and v210 is the one that can carry teletext.
 */
static bool aja_frame_format(AVFormatContext *avctx, AVCodecParameters *c,
                            NTV2PixelFormat *out)
{
    if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (c->format != AV_PIX_FMT_UYVY422) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
                   " Only AV_PIX_FMT_UYVY422 is supported for wrapped frames.\n");
            return false;
        }
        *out = NTV2_FBF_8BIT_YCBCR;
        return true;
    }
    if (c->codec_id == AV_CODEC_ID_V210) {
        *out = NTV2_FBF_10BIT_YCBCR;
        return true;
    }
    av_log(avctx, AV_LOG_ERROR, "Unsupported codec type!"
           " Only V210 and wrapped frames with AV_PIX_FMT_UYVY422 are supported.\n");
    return false;
}

/* Find a video format by the name the SDK gives it -- "1080i50", "1080p25",
 * "625i50". Matched against NTV2VideoFormatToString over every format rather
 * than a table here, so the accepted spellings are the SDK's own and cannot
 * drift from it. */
static NTV2VideoFormat aja_format_by_name(const char *name)
{
    for (int f = NTV2_FORMAT_FIRST_HIGH_DEF_FORMAT; f < NTV2_FORMAT_END_STANDARD_DEF_FORMATS; f++) {
        const NTV2VideoFormat vf = NTV2VideoFormat(f);
        const std::string s = ::NTV2VideoFormatToString(vf);
        if (!s.empty() && !av_strcasecmp(s.c_str(), name))
            return vf;
    }
    return NTV2_FORMAT_UNKNOWN;
}

static void aja_log_formats(AVFormatContext *avctx)
{
    std::string line;
    av_log(avctx, AV_LOG_INFO, "Video formats this build knows:\n");
    for (int f = NTV2_FORMAT_FIRST_HIGH_DEF_FORMAT; f < NTV2_FORMAT_END_STANDARD_DEF_FORMATS; f++) {
        const std::string s = ::NTV2VideoFormatToString(NTV2VideoFormat(f));
        if (s.empty() || s.find('?') != std::string::npos)
            continue;
        line += "  " + s;
        if (line.size() > 64) {
            av_log(avctx, AV_LOG_INFO, "%s\n", line.c_str());
            line.clear();
        }
    }
    if (!line.empty())
        av_log(avctx, AV_LOG_INFO, "%s\n", line.c_str());
}

/* Map the stream's raster and rate onto an NTV2 video format.
 *
 * The SDK will do the matching, so this does not carry a table: the rate comes
 * from the stream's frame rate as a fraction, and interlaced-versus-progressive
 * from the stream's field order. An interlaced stream carries fields at twice
 * the frame rate in ffmpeg's reckoning, which is not what
 * GetFirstMatchingVideoFormat wants, so the rate handed to it is the frame rate.
 */
static bool aja_video_format(AVFormatContext *avctx, struct ajantv2_cctx *cctx,
                            AVStream *st, NTV2VideoFormat *out)
{
    AVCodecParameters *par = st->codecpar;
    AVRational fr = st->avg_frame_rate;
    const bool interlaced = par->field_order != AV_FIELD_PROGRESSIVE &&
                            par->field_order != AV_FIELD_UNKNOWN;
    NTV2FrameRate rate;

    /* An explicit name wins. It has to be available, because a raster and a
     * frame rate do not always identify a format: 1080i50 and 1080p25 are both
     * 1920x1080 at 25, and which one goes down the wire is a different SDI
     * standard. ffmpeg often reaches a muxer with the field order unset, so
     * there is nothing to tell them apart by. */
    if (cctx->format_code && *cctx->format_code) {
        *out = aja_format_by_name(cctx->format_code);
        if (*out == NTV2_FORMAT_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Unknown format_code '%s'\n", cctx->format_code);
            aja_log_formats(avctx);
            return false;
        }
        av_log(avctx, AV_LOG_INFO, "Video format %s (from format_code)\n",
               ::NTV2VideoFormatToString(*out).c_str());
        return true;
    }

    if (!fr.num || !fr.den)
        fr = st->r_frame_rate;
    if (!fr.num || !fr.den) {
        av_log(avctx, AV_LOG_ERROR, "Cannot determine the stream frame rate\n");
        return false;
    }

    rate = ::GetNTV2FrameRateFromNumeratorDenominator(fr.num, fr.den);
    if (rate == NTV2_FRAMERATE_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported frame rate %d/%d\n", fr.num, fr.den);
        return false;
    }

    /* Ask for what the stream says it is, then fall back. Some 1080 sources are
     * segmented-frame rather than true interlace, and those are different
     * formats to the SDK even though the raster and rate match. And a raster
     * can have only one scan: there is exactly one 720x576 format and one
     * 720x486 one, so refusing an SD stream because it calls itself
     * progressive helps nobody. Take the scan the stream claims as a
     * preference, not a requirement. */
    *out = ::GetFirstMatchingVideoFormat(rate, (UWord)par->height,
                                         (UWord)par->width, interlaced, false, false);
    if (*out == NTV2_FORMAT_UNKNOWN && interlaced)
        *out = ::GetFirstMatchingVideoFormat(rate, (UWord)par->height,
                                            (UWord)par->width, true, false, true);
    if (*out == NTV2_FORMAT_UNKNOWN) {
        *out = ::GetFirstMatchingVideoFormat(rate, (UWord)par->height,
                                            (UWord)par->width, !interlaced, false, false);
        if (*out != NTV2_FORMAT_UNKNOWN)
            av_log(avctx, AV_LOG_WARNING,
                   "The stream says %s, and there is no %s format at %dx%d and "
                   "%d/%d, so %s it is. Pass -format_code to say outright.\n",
                   interlaced ? "interlaced" : "progressive",
                   interlaced ? "interlaced" : "progressive",
                   par->width, par->height, fr.num, fr.den,
                   ::NTV2VideoFormatToString(*out).c_str());
    }
    if (*out == NTV2_FORMAT_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR,
               "No NTV2 video format for %dx%d %s at %d/%d\n",
               par->width, par->height, interlaced ? "interlaced" : "progressive",
               fr.num, fr.den);
        aja_log_formats(avctx);
        return false;
    }
    av_log(avctx, AV_LOG_INFO, "%dx%d %s %d/%d -> %s\n",
           par->width, par->height, interlaced ? "interlaced" : "progressive",
           fr.num, fr.den, ::NTV2VideoFormatToString(*out).c_str());
    if (par->field_order == AV_FIELD_UNKNOWN && par->height >= 1080)
        av_log(avctx, AV_LOG_WARNING,
               "The stream does not say whether it is interlaced, so %s was "
               "guessed. Pass -format_code to be certain: 1080i50 and 1080p25 "
               "share this raster and rate.\n",
               ::NTV2VideoFormatToString(*out).c_str());
    return true;
}

/* Bring up the audio system and work out how much of it goes with each frame.
 *
 * The card embeds 32-bit samples, so 16-bit input is widened on the way through
 * rather than refused. The per-frame sample count comes from the video format's
 * rate, and for rates that are not a whole number of samples per frame -- 29.97
 * and friends -- it is the rounded value, with the FIFO absorbing the remainder
 * over time.
 */
/* Fill a span of frame buffer with legal blanking: black luma and neutral
 * chroma. NTV2Buffer::Allocate zeroes what it hands back, and zero is not black
 * -- in 10-bit YCbCr, Y=0 Cb=0 Cr=0 is both a reserved SDI code word and, to
 * anything that renders it, bright green. That matters because the rows this
 * device does not write are the VANC rows, and the card does not clamp the VANC
 * region: it cannot, since a limiter would destroy the ancillary payload. So
 * whatever is in those rows reaches the wire as-is.
 *
 * Also used per frame on the VANC rows, so a VANC packet written for one frame
 * does not sit in the buffer and go out again on the next. */
static void aja_fill_blanking(void *buf, size_t bytes, NTV2PixelFormat pf)
{
    if (pf == NTV2_FBF_10BIT_YCBCR) {
        /* V210, packed exactly as the VBI renderer packs it: three 10-bit words
         * to a little-endian 32-bit word, Cb Y Cr / Y Cb Y repeating. Black is
         * 64 and neutral chroma is 512, which makes the pattern two words long. */
        uint32_t *p = (uint32_t *)buf;
        size_t n = bytes / 4;
        for (size_t i = 0; i < n; i++)
            p[i] = (i & 1) ? (64u | (512u << 10) | (64u << 20))
                           : (512u | (64u << 10) | (512u << 20));
    } else {
        /* 8-bit UYVY: Cb Y Cr Y, black 16 and neutral chroma 128. */
        uint8_t *p = (uint8_t *)buf;
        for (size_t i = 0; i < bytes; i++)
            p[i] = (i & 1) ? 0x10 : 0x80;
    }
}

/* Put legal blanking back in the VANC rows, ahead of writing this frame's VANC
 * into them. */
static void aja_blank_vanc(struct ajantv2_ctx *ctx)
{
    const ULWord first = ctx->fd.GetFirstActiveLine();

    if (!first)
        return;
    aja_fill_blanking(ctx->host_buffer.GetHostPointer(),
                      (size_t)first * ctx->fd.GetBytesPerRow(),
                      ctx->pixel_format);
}

static int aja_setup_audio(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                           struct ajantv2_cctx *cctx)
{
    AVCodecParameters *par = ctx->audio_st->codecpar;
    const NTV2FrameRate fr = ::GetNTV2FrameRateFromVideoFormat(ctx->video_format);
    const double fps = ::GetFramesPerSecond(fr);
    int samples_per_frame;

    ctx->audio_channels = par->ch_layout.nb_channels;
    if (ctx->audio_channels < 2 || ctx->audio_channels > 16 ||
        (ctx->audio_channels & 1)) {
        av_log(avctx, AV_LOG_ERROR,
               "%d audio channels is not something the card embeds; use an even "
               "count from 2 to 16\n", ctx->audio_channels);
        return AVERROR(EINVAL);
    }
    if (par->sample_rate != 48000) {
        av_log(avctx, AV_LOG_ERROR, "Audio must be 48kHz, not %d\n", par->sample_rate);
        return AVERROR(EINVAL);
    }
    if (fps < 1.0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot determine the output frame rate\n");
        return AVERROR(EINVAL);
    }

    ctx->audio_system = ::NTV2ChannelToAudioSystem(ctx->channel);
    /* A few older boards have one audio system but several framestores. */
    if (!ctx->device.features().CanDoFrameStore1Display())
        ctx->audio_system = NTV2_AUDIOSYSTEM_1;

    ctx->device.SetNumberAudioChannels(ctx->audio_channels, ctx->audio_system);
    ctx->device.SetAudioRate(NTV2_AUDIO_48K, ctx->audio_system);
    ctx->device.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, ctx->audio_system);
    for (size_t n = 0; n < ctx->outputs.size(); n++) {
        ctx->device.SetSDIOutputAudioSystem(ctx->outputs[n], ctx->audio_system);
        ctx->device.SetSDIOutputDS2AudioSystem(ctx->outputs[n], ctx->audio_system);
    }
    /* If the previous user left the board in E-E mode, loopback would embed the
     * SDI input's audio over ours. */
    ctx->device.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, ctx->audio_system);

    ctx->audio_bytes_per_sample = 4;
    samples_per_frame = (int)(48000.0 / fps + 0.5);
    ctx->audio_frame_bytes = samples_per_frame * ctx->audio_channels *
                             ctx->audio_bytes_per_sample;

    /* Room for a second or so, so a burst of packets does not spill. */
    ctx->audio_fifo = av_fifo_alloc2(ctx->audio_frame_bytes * 64, 1,
                                     AV_FIFO_FLAG_AUTO_GROW);
    if (!ctx->audio_fifo)
        return AVERROR(ENOMEM);
    if (!ctx->audio_buffer.Allocate(ctx->audio_frame_bytes, true))
        return AVERROR(ENOMEM);

    av_log(avctx, AV_LOG_INFO,
           "Audio: %d channels, 48kHz, %d samples and %d bytes a frame\n",
           ctx->audio_channels, samples_per_frame, ctx->audio_frame_bytes);
    return 0;
}

static int aja_setup_video(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                           struct ajantv2_cctx *cctx, AVStream *st)
{
    AVCodecParameters *par = st->codecpar;

    if (!aja_video_format(avctx, cctx, st, &ctx->video_format))
        return AVERROR(EINVAL);
    if (!aja_frame_format(avctx, par, &ctx->pixel_format))
        return AVERROR(EINVAL);

    if (!ctx->device.features().CanDoVideoFormat(ctx->video_format)) {
        av_log(avctx, AV_LOG_ERROR, "Device cannot do %s\n",
               ::NTV2VideoFormatToString(ctx->video_format).c_str());
        return AVERROR(EINVAL);
    }

    /* The VBI renderer writes V210, so teletext needs the 10-bit frame buffer
     * format. Rendering it into a UYVY row would put noise on line 21 rather
     * than fail, so refuse the combination instead. */
    if (ctx->teletext_enabled && ctx->pixel_format != NTV2_FBF_10BIT_YCBCR) {
        av_log(avctx, AV_LOG_ERROR,
               "Teletext output needs -c:v v210; uyvy422 cannot carry the VBI "
               "waveform\n");
        return AVERROR(EINVAL);
    }

    /* TALLER puts the vertical blanking lines at the top of the frame buffer,
     * which is what makes writing line 21 a matter of writing into the buffer.
     * Only asked for when teletext is actually wanted, since it changes the
     * buffer geometry. */
    if (ctx->teletext_enabled && !NTV2_IS_625_FORMAT(ctx->video_format)) {
        av_log(avctx, AV_LOG_WARNING,
               "Teletext output only applies to 625-line SD; ignoring the "
               "subtitle stream for %s\n",
               ::NTV2VideoFormatToString(ctx->video_format).c_str());
        ctx->teletext_enabled = 0;
    }
#if CONFIG_LIBKLVANC
    /* Captions arriving as A53 side data are written into HD VANC, which needs
     * the region present in the frame buffer and 10-bit pixels to hold it. */
    ctx->cc_enabled = cctx->cc && !NTV2_IS_625_FORMAT(ctx->video_format) &&
                      ctx->pixel_format == NTV2_FBF_10BIT_YCBCR;
#endif

    /* How much of the blanking the frame buffer has to hold. TALLER reaches SD
     * line 21, which is the only reason to ask for it; HD captions sit on line
     * 11, which TALL already covers (its first row is SMPTE line 10). TALL is
     * the ordinary HD VANC geometry, so prefer it: it is the one an old card is
     * likelier to have firmware for. Without either, the frame buffer is the
     * picture and nothing else, which is the plainest path the card has. */
    if (ctx->teletext_enabled)
        ctx->vanc_mode = NTV2_VANCMODE_TALLER;
    else if (AJA_CC_ENABLED(ctx))
        ctx->vanc_mode = NTV2_VANCMODE_TALL;
    else
        ctx->vanc_mode = NTV2_VANCMODE_OFF;

    /* Every framestore on a card that cannot do multi-format shares one video
     * format register, so another one left enabled -- by a capture run, or by
     * the AJA control panel -- fights this setup. We hold the whole device, so
     * shut them all down first, as the SDK's own player does. */
    if (!ctx->device.features().CanDoMultiFormat()) {
        NTV2ChannelSet enabled;
        ctx->device.GetEnabledChannels(enabled);
        if (!enabled.empty())
            ctx->device.DisableChannels(enabled);
    }

    if (!ctx->device.SetVideoFormat(ctx->video_format, false, false, ctx->channel))
        av_log(avctx, AV_LOG_WARNING, "SetVideoFormat(%s) failed\n",
               ::NTV2VideoFormatToString(ctx->video_format).c_str());
    if (!ctx->device.SetFrameBufferFormat(ctx->channel, ctx->pixel_format))
        av_log(avctx, AV_LOG_WARNING, "SetFrameBufferFormat(%s) failed\n",
               ::NTV2FrameBufferFormatToString(ctx->pixel_format).c_str());
    if (!ctx->device.SetVANCMode(ctx->vanc_mode, ctx->channel))
        av_log(avctx, AV_LOG_WARNING, "SetVANCMode(%s) failed\n",
               ::NTV2VANCModeToString(ctx->vanc_mode).c_str());
    ctx->device.SetMode(ctx->channel, NTV2_MODE_DISPLAY);
    ctx->device.EnableChannel(ctx->channel);

    /* Where the output gets its clock. Left alone it keeps whatever it was last
     * set to, and if that is an SDI input with nothing on it the output has no
     * timing reference at all. Playout has nothing to genlock to, so free-run.
     */
    ctx->device.SetReference(NTV2_REFERENCE_FREERUN);

    /* WaitForOutputVerticalInterrupt cannot block on a channel whose interrupt
     * has not been subscribed; without this the wait returns immediately and the
     * flow-control loop in aja_write_video spins on the CPU. */
    ctx->device.SubscribeOutputVerticalEvent(ctx->channel);

    /* 8-bit VANC is shifted by the hardware unless told otherwise; 10-bit is
     * not. Match what the VBI renderer writes. */
    if (NTV2_IS_VANCMODE_ON(ctx->vanc_mode))
        ctx->device.SetVANCShiftMode(ctx->channel,
            ctx->pixel_format == NTV2_FBF_8BIT_YCBCR
                ? NTV2_VANCDATA_8BITSHIFT_ENABLE : NTV2_VANCDATA_8BITSHIFT_DISABLE);

    ctx->fd = NTV2FormatDescriptor(ctx->video_format, ctx->pixel_format,
                                   ctx->vanc_mode);
    {   /* The CDP carries the frame rate. klvanc names its arguments num and
         * den but wants the frame *duration* -- 1/25, not 25/1 -- so the
         * fraction GetFramesPerSecond returns goes in inverted. The DeckLink
         * output passes its time base here for the same reason. */
        const NTV2FrameRate fr = ::GetNTV2FrameRateFromVideoFormat(ctx->video_format);
        ULWord num = 0, den = 0;
        ::GetFramesPerSecond(fr, num, den);
        ctx->cdp_tb_num = (int)den;
        ctx->cdp_tb_den = (int)num;
    }
    ctx->frame_size = ::GetVideoWriteSize(ctx->video_format, ctx->pixel_format,
                                         ctx->vanc_mode);

    if (ctx->teletext_enabled) {
        if (!ctx->fd.GetLineOffsetFromSMPTELine(AJA_SD_LINE_FIELD1, ctx->tt_row_f1) ||
            !ctx->fd.GetLineOffsetFromSMPTELine(AJA_SD_LINE_FIELD2, ctx->tt_row_f2)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot locate VBI lines %d/%d in the frame buffer\n",
                   AJA_SD_LINE_FIELD1, AJA_SD_LINE_FIELD2);
            return AVERROR(EINVAL);
        }
        av_log(avctx, AV_LOG_INFO,
               "Teletext on SMPTE lines %d and %d (buffer rows %u and %u)\n",
               AJA_SD_LINE_FIELD1, AJA_SD_LINE_FIELD2,
               ctx->tt_row_f1, ctx->tt_row_f2);
    }

    /* Route the framestore to every requested SDI connector. One framestore can
     * feed several, which is how a card with two outputs mirrors the same
     * picture, so this is a list rather than a single choice. */
    {
        const NTV2OutputCrosspointID fsOut(::GetFrameBufferOutputXptFromChannel(ctx->channel));
        const NTV2Standard std(::GetNTV2StandardFromVideoFormat(ctx->video_format));
        const UWord num_outputs(ctx->device.features().GetNumVideoOutputs());
        const bool verify(ctx->device.features().HasCrosspointConnectROM());
        std::string listed;

        /* No -output: feed every SDI connector the card has, which is what the
         * SDK's test pattern demo does and the only setup on this card that has
         * ever put a picture on a monitor. Feeding one connector and guessing
         * which is not worth the hour it costs when the guess is wrong. */
        if (ctx->outputs.empty()) {
            for (UWord n = 0; n < (num_outputs ? num_outputs : 1); n++)
                ctx->outputs.push_back((NTV2Channel)n);
        }

        /* Start from nothing. A crosspoint left over from another run -- a
         * capture, or the AJA control panel's own routing -- can still be
         * driving the connector we are about to connect, and on a card with a
         * video processor in the path the leftovers decide what comes out. The
         * SDK's player clears the routing for the same reason. */
        ctx->device.ClearRouting();

        for (size_t n = 0; n < ctx->outputs.size(); n++) {
            const NTV2Channel out(ctx->outputs[n]);
            if (num_outputs && UWord(out) >= num_outputs) {
                av_log(avctx, AV_LOG_ERROR,
                       "SDI output %d requested but this device has %u\n",
                       int(out) + 1, num_outputs);
                return AVERROR(EINVAL);
            }
            if (!ctx->device.Connect(::GetSDIOutputInputXpt(out), fsOut, verify))
                av_log(avctx, AV_LOG_WARNING,
                       "Cannot route framestore %d to SDI output %d\n",
                       int(ctx->channel) + 1, int(out) + 1);
            if (ctx->device.features().HasBiDirectionalSDI())
                ctx->device.SetSDITransmitEnable(out, true);
            ctx->device.SetSDIOutputStandard(out, std);
            ctx->device.SetSDIOutLevelAtoLevelBConversion(out, false);
            ctx->device.SetSDIOutRGBLevelAConversion(out, false);
            listed += (n ? ", " : "");
            listed += std::to_string(int(out) + 1);

            /* Read the crosspoint back. Connect returning true only means the
             * register write went through; it does not mean the connector is
             * now fed from the framestore. */
            {
                NTV2OutputCrosspointID got(NTV2_XptBlack);
                ctx->device.GetConnectedOutput(::GetSDIOutputInputXpt(out), got);
                av_log(avctx, got == fsOut ? AV_LOG_INFO : AV_LOG_ERROR,
                       "SDI output %d is fed from %s (wanted %s)\n",
                       int(out) + 1,
                       ::NTV2OutputCrosspointIDToString(got).c_str(),
                       ::NTV2OutputCrosspointIDToString(fsOut).c_str());
            }
        }
        av_log(avctx, AV_LOG_INFO, "Framestore %d -> SDI output%s %s\n",
               int(ctx->channel) + 1, ctx->outputs.size() > 1 ? "s" : "",
               listed.c_str());
    }

    if (!ctx->host_buffer.Allocate(ctx->frame_size, true)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate a %u-byte frame buffer\n",
               ctx->frame_size);
        return AVERROR(ENOMEM);
    }
    aja_fill_blanking(ctx->host_buffer.GetHostPointer(), ctx->frame_size,
                      ctx->pixel_format);

    /* Read the registers back and say what the card really has. Nearly every
     * setter here is advisory -- the driver can refuse a combination and leave
     * the previous value in place -- and a framestore whose geometry disagrees
     * with the frame buffer this device writes produces a picture that is wrong
     * without anything reporting an error. */
    {
        NTV2VideoFormat vf(NTV2_FORMAT_UNKNOWN);
        NTV2FrameBufferFormat fbf(NTV2_FBF_INVALID);
        NTV2VANCMode vm(NTV2_VANCMODE_INVALID);
        NTV2FrameGeometry fg(NTV2_FG_INVALID);
        NTV2ReferenceSource ref(NTV2_REFERENCE_INVALID);

        ctx->device.GetVideoFormat(vf, ctx->channel);
        ctx->device.GetFrameBufferFormat(ctx->channel, fbf);
        ctx->device.GetVANCMode(vm, ctx->channel);
        ctx->device.GetFrameGeometry(fg, ctx->channel);
        ctx->device.GetReference(ref);
        {
            NTV2ChannelSet on;
            std::string names;
            ctx->device.GetEnabledChannels(on);
            for (NTV2ChannelSetConstIter it(on.begin()); it != on.end(); ++it)
                names += (names.empty() ? "" : ",") + std::to_string(int(*it) + 1);
            av_log(avctx, AV_LOG_INFO, "Framestores enabled: %s\n",
                   names.empty() ? "none" : names.c_str());
        }

        av_log(avctx, AV_LOG_INFO,
               "Card reports: %s %s %s %s, reference %s\n",
               ::NTV2VideoFormatToString(vf).c_str(),
               ::NTV2FrameBufferFormatToString(fbf).c_str(),
               ::NTV2FrameGeometryToString(fg).c_str(),
               ::NTV2VANCModeToString(vm).c_str(),
               ::NTV2ReferenceSourceToString(ref).c_str());

        if (vf != ctx->video_format || fbf != ctx->pixel_format ||
            vm != ctx->vanc_mode)
            av_log(avctx, AV_LOG_ERROR,
                   "The card did not take the requested setup (asked for %s %s "
                   "%s). The picture will be wrong.\n",
                   ::NTV2VideoFormatToString(ctx->video_format).c_str(),
                   ::NTV2FrameBufferFormatToString(ctx->pixel_format).c_str(),
                   ::NTV2VANCModeToString(ctx->vanc_mode).c_str());
    }

    return 0;
}

/* Start AutoCirculate. Separate from the video setup because the audio system
 * has to be known here: it is passed to AutoCirculateInitForOutput, and the
 * transfer carries video and audio together from then on. */
static int aja_start_output(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                           struct ajantv2_cctx *cctx)
{
    ULWord ring = cctx->ring_frames > 1 ? (ULWord)cctx->ring_frames : 8;
    const NTV2AudioSystem asys = ctx->audio_st ? ctx->audio_system
                                               : NTV2_AUDIOSYSTEM_INVALID;

    AUTOCIRCULATE_STATUS st;

    ctx->device.AutoCirculateStop(ctx->channel);


    /* How many frames the card has at this geometry. It is not a constant: on
     * the KonaLHe+, 1920x1112 costs a 16MB slot and leaves 8 frames in the
     * card's 128MB, while 1920x1080 and 1920x1114 cost 8MB and leave 16. Ask
     * for more than exist and AutoCirculateInitForOutput fails outright, and
     * the audio buffer takes its share on top, so come down until it takes. */
    {
        const NTV2FrameGeometry fg(::GetNTV2FrameGeometryFromVideoFormat(ctx->video_format));
        const ULWord have = ::NTV2DeviceGetNumberFrameBuffers(ctx->device.GetDeviceID(),
                                                              ctx->vanc_mode == NTV2_VANCMODE_OFF
                                                                  ? fg : ::GetVANCFrameGeometry(fg, ctx->vanc_mode),
                                                              ctx->pixel_format);
        ULWord want = ring;
        bool ok = false;

        if (have && want > have)
            want = have;
        for (; want >= 2; want--)
            if ((ok = ctx->device.AutoCirculateInitForOutput(ctx->channel, want, asys)))
                break;
        if (!ok) {
            av_log(avctx, AV_LOG_ERROR,
                   "AutoCirculateInitForOutput failed for every ring depth from "
                   "%u down to 2; this geometry leaves the card %u frames\n",
                   ring, have);
            return AVERROR(EIO);
        }
        if (want != ring)
            av_log(avctx, AV_LOG_WARNING,
                   "Ring of %u frames, not %u: this geometry leaves the card "
                   "%u and the audio buffer takes its share\n",
                   want, ring, have);
        ring = want;
    }
    if (!ctx->device.AutoCirculateGetStatus(ctx->channel, st)) {
        av_log(avctx, AV_LOG_ERROR, "AutoCirculateGetStatus failed\n");
        return AVERROR(EIO);
    }

    /* Put black in every frame the card will play from. These are slots in the
     * card's own memory, holding whatever the last thing to use the card left
     * there, and the frames it plays before the first transfer lands come
     * straight from them. The SDK's player does the same, in its own words, to
     * "prevent garbage output frames". */
    for (UWord f = st.GetStartFrame(); f <= st.GetEndFrame(); f++)
        ctx->device.DMAWriteFrame((ULWord)f,
                                  (ULWord *)ctx->host_buffer.GetHostPointer(),
                                  ctx->frame_size);

    /* Hold off starting. AutoCirculate started on an empty ring plays frames
     * that have nothing in them yet, so fill a few first and start from
     * aja_deliver_frame once they are there, which is how the player does it.
     *
     * -preroll is in seconds, as on the DeckLink output, so a command line
     * moves between the two cards unchanged. It cannot mean the same thing on
     * both, though: DeckLink buffers in host memory and this buffers in the
     * card's, where the whole ring is a fraction of a second, so the request is
     * capped at one frame short of the ring. With an output buffer configured
     * the preroll is kept to two frames and the host-side buffer carries the
     * timing safety, which is what the DeckLink output does for the same
     * reason: a short preroll gets to air sooner. */
    if (ctx->buffered) {
        ctx->preroll_frames = 2;
    } else {
        const double fps = ctx->cdp_tb_num > 0
                               ? (double)ctx->cdp_tb_den / ctx->cdp_tb_num : 25.0;
        ctx->preroll_frames = (int)(cctx->preroll * fps + 0.5);
        if (ctx->preroll_frames < 1)
            ctx->preroll_frames = 1;
    }
    if (st.GetFrameCount() > 1 && ctx->preroll_frames > (int)st.GetFrameCount() - 1)
        ctx->preroll_frames = st.GetFrameCount() - 1;

    av_log(avctx, AV_LOG_INFO,
           "AJA output: %s %s, %u-frame ring, buffer %u bytes, audio %s\n",
           ::NTV2VideoFormatToString(ctx->video_format).c_str(),
           ::NTV2FrameBufferFormatToString(ctx->pixel_format).c_str(),
           ring, ctx->frame_size, ctx->audio_st ? "on" : "off");
    av_log(avctx, AV_LOG_INFO,
           "Card frames %u to %u (%u of them), starting after %d\n",
           st.GetStartFrame(), st.GetEndFrame(), st.GetFrameCount(),
           ctx->preroll_frames);

    return 0;
}

#if CONFIG_LIBKLVANC
/* Turn the frame's A53 caption side data into a CEA-708 CDP and write it as a
 * VANC line of the frame buffer.
 *
 * The CDP is built exactly as the DeckLink output builds it, through the same
 * klvanc calls, so a receiver cannot tell which card a feed came from. The only
 * difference is where the rendered line goes: DeckLink asks the API for a
 * blanking line, and here the line is a row of the frame buffer, found the same
 * way the teletext row is.
 */
static void aja_insert_cc(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                          AVPacket *pkt)
{
    struct klvanc_line_set_s vanc_lines = {0};
    struct klvanc_packet_eia_708b_s *cdp = NULL;
    uint16_t *cdp_words = NULL;
    uint16_t len = 0;
    uint8_t cc_count;
    size_t size;
    int ret, i;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
    if (!data || !size)
        return;

    cc_count = size / 3;
    if (klvanc_create_eia708_cdp(&cdp))
        return;
    if (klvanc_set_framerate_EIA_708B(cdp, ctx->cdp_tb_num, ctx->cdp_tb_den)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid frame rate for CEA-708: %d/%d\n",
               ctx->cdp_tb_den, ctx->cdp_tb_num);
        klvanc_destroy_eia708_cdp(cdp);
        return;
    }
    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Illegal cc_count %d\n", cc_count);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    cdp->header.ccdata_present = 1;
    cdp->header.caption_service_active = 1;
    cdp->ccdata.cc_count = cc_count;
    for (i = 0; i < cc_count; i++) {
        if (data[3 * i] & 0x04)
            cdp->ccdata.cc[i].cc_valid = 1;
        cdp->ccdata.cc[i].cc_type = data[3 * i] & 0x03;
        cdp->ccdata.cc[i].cc_data[0] = data[3 * i + 1];
        cdp->ccdata.cc[i].cc_data[1] = data[3 * i + 2];
    }

    klvanc_finalize_EIA_708B(cdp, ctx->cdp_sequence_num++);
    ret = klvanc_convert_EIA_708B_to_words(cdp, &cdp_words, &len);
    klvanc_destroy_eia708_cdp(cdp);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting the 708 packet to words\n");
        return;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, &vanc_lines, cdp_words, len,
                             AJA_HD_LINE_CC, 0);
    free(cdp_words);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed inserting the VANC line\n");
        return;
    }

    for (i = 0; i < vanc_lines.num_lines; i++) {
        struct klvanc_line_s *line = vanc_lines.lines[i];
        ULWord row;
        void *buf;

        if (!line)
            break;
        if (!ctx->fd.GetLineOffsetFromSMPTELine((ULWord)line->line_number, row)) {
            av_log(avctx, AV_LOG_WARNING,
                   "SMPTE line %d is outside the frame buffer's VANC region\n",
                   line->line_number);
            continue;
        }
        buf = ctx->fd.GetWriteableRowAddress(ctx->host_buffer.GetHostPointer(), row);
        if (buf)
            klvanc_generate_vanc_line_v210(ctx->vanc_ctx, line, (uint8_t *)buf,
                                           ctx->fd.GetRasterWidth());
    }
    for (i = 0; i < vanc_lines.num_lines; i++)
        klvanc_line_free(vanc_lines.lines[i]);
}
#endif /* CONFIG_LIBKLVANC */

/* Render whatever the teletext generator says goes out this frame into the two
 * VBI rows of the frame buffer. */
static void aja_insert_teletext(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                                int stored_new)
{
    FFVbiTeletextFrame out;
    const ULWord rows[2] = {ctx->tt_row_f1, ctx->tt_row_f2};
    int f;

    ff_vbi_teletext_frame(&ctx->tt, stored_new, ctx->last_pts, &out, avctx);

    for (f = 0; f < 2; f++) {
        void *row;
        if (!out.send[f])
            continue;
        row = ctx->fd.GetWriteableRowAddress(ctx->host_buffer.GetHostPointer(),
                                             rows[f]);
        if (!row)
            continue;
        ff_vbi_teletext_render_v210((uint8_t *)row, ctx->fd.GetRasterWidth(),
                                    out.packet[f], &ctx->tt.cfg);
    }
}

/* A cheap fingerprint of the frame buffer, for telling "the picture never got
 * into the buffer" apart from "the buffer never got to the card". Both look the
 * same on a monitor. */
static uint32_t aja_buffer_hash(const void *buf, size_t bytes)
{
    const uint32_t *p = (const uint32_t *)buf;
    size_t n = bytes / 4;
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < n; i += 97)       /* every 97th word is plenty */
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Is the card being fed fast enough?
 *
 * AutoCirculate is a queue, not a scheduling API: frames are handed over and
 * played in order at the card's rate, so unlike the DeckLink output there is no
 * per-frame presentation time to compare against a clock. What there is:
 *
 *   - the card's own dropped-frame count, which on output counts the times it
 *     wanted a frame and had none, so repeated the last one. Every increment is
 *     a frame of the programme that did not go out.
 *   - the number of frames it must have asked for by now. Once running it takes
 *     exactly one per frame period, so wall clock since playback started says
 *     how many, and frames_out says how many it got.
 *
 * A rise in the first is reported. A deficit in the second past late_threshold
 * seconds stops the output, on the same reasoning as the DeckLink late check:
 * hours of quietly wrong playout is worse than stopping.
 *
 * cdp_tb_* is the frame duration, so den/num is the frame rate. */
static int aja_check_late(AVFormatContext *avctx, struct ajantv2_ctx *ctx)
{
    AUTOCIRCULATE_STATUS st;
    int64_t now, expected, behind_frames;
    double behind_secs;

    if (!ctx->started || ctx->cdp_tb_num <= 0 || ctx->cdp_tb_den <= 0)
        return 0;
    if (!ctx->device.AutoCirculateGetStatus(ctx->channel, st))
        return 0;

    now = av_gettime_relative();

    if (st.GetDroppedFrameCount() > ctx->last_dropped) {
        /* At most one line a second: a card that is starving will do this on
         * every frame, and a log full of it is no easier to read. */
        if (now - ctx->late_logged_us > 1000000) {
            av_log(avctx, AV_LOG_WARNING,
                   "Card ran out of frames %u times (%u since the last "
                   "report); %u frames in hand\n",
                   st.GetDroppedFrameCount(),
                   st.GetDroppedFrameCount() - ctx->last_dropped,
                   st.GetBufferLevel());
            ctx->late_logged_us = now;
        }
        ctx->last_dropped = st.GetDroppedFrameCount();
    }

    expected = (now - ctx->playback_start_us) * ctx->cdp_tb_den /
               ((int64_t)ctx->cdp_tb_num * 1000000);
    behind_frames = expected - (ctx->frames_out - ctx->frames_at_start);
    if (behind_frames <= 0)
        return 0;

    behind_secs = (double)behind_frames * ctx->cdp_tb_num / ctx->cdp_tb_den;
    if (ctx->late_threshold > 0 && behind_secs > ctx->late_threshold) {
        av_log(avctx, AV_LOG_ERROR,
               "Output is %.2fs behind the card (threshold %.2fs). "
               "Stopping.\n", behind_secs, ctx->late_threshold);
        return AVERROR(EIO);
    }
    return 0;
}
/* Build one frame in the host buffer and hand it to the card. Runs on the muxer
 * thread when there is no output buffer, and on the consumer thread when there
 * is, so everything it touches that the muxer thread also touches is locked. */
static int aja_deliver_frame(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                             AVPacket *pkt, int stored_new)
{
    AVFrame *frame = NULL;
    const uint8_t *src;
    int src_stride, i;
    uint8_t *dst;

    if (pkt->size == sizeof(AVFrame *))
        frame = *(AVFrame **)pkt->data;      /* AV_CODEC_ID_WRAPPED_AVFRAME */

    /* The active picture starts after the VANC rows, so copy into the visible
     * part of the buffer and leave the blanking to the teletext renderer. */
    dst = (uint8_t *)ctx->fd.GetWriteableRowAddress(ctx->host_buffer.GetHostPointer(),
                                                    ctx->fd.GetFirstActiveLine());
    if (!dst)
        return AVERROR(EIO);

    if (frame) {
        src = frame->data[0];
        src_stride = frame->linesize[0];
    } else {
        src = pkt->data;
        src_stride = (int)ctx->fd.GetBytesPerRow();

        /* A packet that is not the size of the raster means the encoder and the
         * frame buffer disagree about the picture, and copying row by row out
         * of it would read past the end and put whatever follows on air. Say so
         * rather than sending it. */
        if (pkt->size != (int)ctx->fd.GetVisibleRasterBytes()) {
            av_log(avctx, AV_LOG_ERROR,
                   "Packet is %d bytes, but %s %s wants %u (%u rows of %u). "
                   "Refusing to send it.\n",
                   pkt->size, ::NTV2VideoFormatToString(ctx->video_format).c_str(),
                   ::NTV2FrameBufferFormatToString(ctx->pixel_format).c_str(),
                   ctx->fd.GetVisibleRasterBytes(),
                   ctx->fd.GetVisibleRasterHeight(), ctx->fd.GetBytesPerRow());
            return AVERROR(EINVAL);
        }
    }
    for (i = 0; i < (int)ctx->fd.GetVisibleRasterHeight(); i++)
        memcpy(dst + i * ctx->fd.GetBytesPerRow(),
               src + (size_t)i * src_stride, ctx->fd.GetBytesPerRow());

    ctx->last_pts = FFMAX(ctx->last_pts == AV_NOPTS_VALUE ? pkt->pts : ctx->last_pts,
                          pkt->pts);

    if (NTV2_IS_VANCMODE_ON(ctx->vanc_mode))
        aja_blank_vanc(ctx);
    if (ctx->teletext_enabled) {
        pthread_mutex_lock(&ctx->tt_lock);
        aja_insert_teletext(avctx, ctx, stored_new);
        pthread_mutex_unlock(&ctx->tt_lock);
    }
#if CONFIG_LIBKLVANC
    if (ctx->cc_enabled)
        aja_insert_cc(avctx, ctx, pkt);
#endif

    /* Wait for room in the ring, then hand the frame over. Nothing is being
     * played until the preroll is done, so there is nothing to wait for. */
    while (ctx->started) {
        AUTOCIRCULATE_STATUS st;
        if (!ctx->device.AutoCirculateGetStatus(ctx->channel, st))
            return AVERROR(EIO);
        if (st.CanAcceptMoreOutputFrames())
            break;
        ctx->device.WaitForOutputVerticalInterrupt(ctx->channel);
    }

    ctx->xfer.SetVideoBuffer((ULWord *)ctx->host_buffer.GetHostPointer(),
                             ctx->frame_size);

    if (ctx->frames_out < 5) {
        const uint32_t *w = (const uint32_t *)ctx->host_buffer.GetHostPointer();
        av_log(avctx, AV_LOG_DEBUG,
               "Frame %d: buffer %u bytes, hash %08x, first words %08x %08x, "
               "row %u words %08x %08x\n",
               ctx->frames_out, ctx->frame_size,
               aja_buffer_hash(w, ctx->frame_size), w[0], w[1],
               ctx->fd.GetVisibleRasterHeight() / 2,
               w[(ctx->fd.GetVisibleRasterHeight() / 2) *
                 (ctx->fd.GetBytesPerRow() / 4)],
               w[(ctx->fd.GetVisibleRasterHeight() / 2) *
                 (ctx->fd.GetBytesPerRow() / 4) + 1]);
    }

    /* One frame's worth of audio goes with the frame. Short of that -- the
     * stream has not caught up, or has ended -- the rest is silence, which
     * keeps the embedder fed and the two in step rather than letting audio slip
     * a frame behind for the rest of the run. */
    if (ctx->audio_fifo) {
        size_t have;
        const size_t want = (size_t)ctx->audio_frame_bytes;
        uint8_t *dst = (uint8_t *)ctx->audio_buffer.GetHostPointer();

        pthread_mutex_lock(&ctx->audio_lock);
        have = av_fifo_can_read(ctx->audio_fifo);
        if (have >= want) {
            av_fifo_read(ctx->audio_fifo, dst, want);
        } else {
            if (have)
                av_fifo_read(ctx->audio_fifo, dst, have);
            memset(dst + have, 0, want - have);
            if (ctx->frames_out > 2 && ++ctx->audio_underruns % 25 == 1)
                av_log(avctx, AV_LOG_WARNING,
                       "Audio underrun: %zu of %zu bytes, padding with silence "
                       "(%"PRId64" so far)\n", have, want, ctx->audio_underruns);
        }
        pthread_mutex_unlock(&ctx->audio_lock);
        ctx->xfer.SetAudioBuffer((ULWord *)dst, ctx->audio_frame_bytes);
    }
    if (!ctx->device.AutoCirculateTransfer(ctx->channel, ctx->xfer)) {
        av_log(avctx, AV_LOG_ERROR, "AutoCirculateTransfer failed\n");
        return AVERROR(EIO);
    }

    if (ctx->frames_out < 5) {
        ULWord shown = 0;
        NTV2Mode mode = NTV2_MODE_INVALID;
        ctx->device.GetOutputFrame(ctx->channel, shown);
        ctx->device.GetMode(ctx->channel, mode);
        av_log(avctx, AV_LOG_DEBUG,
               "Frame %d went to card frame %d; framestore is showing %u, "
               "mode %s\n", ctx->frames_out,
               (int)ctx->xfer.GetTransferFrameNumber(), shown,
               ::NTV2ModeToString(mode).c_str());
    }
    ctx->frames_out++;

    if (!ctx->started && ctx->frames_out >= ctx->preroll_frames) {
        if (!ctx->device.AutoCirculateStart(ctx->channel)) {
            av_log(avctx, AV_LOG_ERROR, "AutoCirculateStart failed\n");
            return AVERROR(EIO);
        }
        ctx->started = 1;
        ctx->playback_start_us = av_gettime_relative();
        ctx->frames_at_start = ctx->frames_out;
        {
            AUTOCIRCULATE_STATUS st;
            if (ctx->device.AutoCirculateGetStatus(ctx->channel, st))
                av_log(avctx, AV_LOG_INFO,
                       "AutoCirculate started: state %s, frames %u to %u, "
                       "active frame %d\n",
                       ::NTV2AutoCirculateStateToString(st.acState).c_str(),
                       st.GetStartFrame(), st.GetEndFrame(),
                       (int)st.GetActiveFrame());
        }
        return 0;
    }

    return aja_check_late(avctx, ctx);
}

/* Consumer thread: take frames off the queue and hand them to the card. The
 * muxer thread is then free to run ahead into the buffer, which is the whole
 * point of having one. */
static void *aja_output_thread(void *arg)
{
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    AVPacket pkt;

    aja_set_output_thread_priority(avctx);

    while (!ctx->thread_stop) {
        int ret;

        /* In pre-render the card has not been claimed yet, so there is nothing
         * to deliver to. Frames pile up in the queue meanwhile, which is the
         * point of the mode. */
        if (!ctx->device_ready) {
            av_usleep(5000);
            continue;
        }

        if (!aja_queue_get(&ctx->queue, &pkt))
            break;                      /* empty and stopping */
        ret = aja_deliver_frame(avctx, ctx, &pkt, 0);
        av_packet_unref(&pkt);
        if (ret < 0) {
            ctx->thread_error = ret;
            /* Let the muxer thread out of a blocking put so it can see it. */
            aja_queue_stop(&ctx->queue);
            break;
        }
    }
    av_log(avctx, AV_LOG_INFO, "Output thread finished after %d frames\n",
           ctx->frames_out);
    return NULL;
}
extern "C" {

static void *aja_trigger_thread(void *arg);
/* Claim the card and set it up. Split out because pre-render calls it from the
 * trigger thread, long after write_header has returned, so that the frames of
 * this programme can be decoded and buffered while the previous one is still on
 * air. Everything that talks to the card for the first time lives here. */
static int aja_claim_and_init(AVFormatContext *avctx)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)cctx->ctx;
    int ret;

    if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(avctx->url ? avctx->url : "0",
                                                        ctx->device)) {
        av_log(avctx, AV_LOG_ERROR, "No AJA device matching '%s'\n",
               avctx->url ? avctx->url : "0");
        return AVERROR(ENODEV);
    }
    if (!ctx->device.IsDeviceReady(false)) {
        av_log(avctx, AV_LOG_ERROR, "AJA device is not ready\n");
        return AVERROR(EIO);
    }
    /* Take the card away from the AJA retail service before touching a single
     * register. Left in standard task mode, that service keeps reprogramming
     * the card from the control panel's settings while we run: our writes land,
     * and it puts them back. Reading a register straight after writing it still
     * agrees, which is why the card could report the format we asked for while
     * sending something else down the wire. Every SDK demo does this, and the
     * capture tool this card has been used with restores the old mode when it
     * exits, so the service is back in charge by the time ffmpeg starts. */
    ctx->device.GetTaskMode(ctx->saved_task_mode);

    /* AcquireStreamForApplication fails when another application holds the
     * card. Ignoring that let two processes each believe they owned it. With
     * -block_until_available, wait instead, so an incoming process can be
     * started before the outgoing one has let go. */
    {
        const int64_t started = av_gettime_relative();
        int said = 0;

        while (!ctx->device.AcquireStreamForApplication(AJA_FOURCC('f','f','m','p'),
                                                        (int32_t)getpid())) {
            if (!cctx->block_until_available) {
                av_log(avctx, AV_LOG_ERROR,
                       "AJA device is in use by another application. Pass "
                       "-block_until_available 1 to wait for it.\n");
                return AVERROR(EBUSY);
            }
            if (!said) {
                av_log(avctx, AV_LOG_INFO, "Waiting for the AJA device...\n");
                said = 1;
            }
            av_usleep(10000);
        }
        if (said)
            av_log(avctx, AV_LOG_INFO, "AJA device acquired after %.1f ms\n",
                   (av_gettime_relative() - started) / 1000.0);
    }
    ctx->device.SetTaskMode(NTV2_OEM_TASKS);
    av_log(avctx, AV_LOG_INFO, "AJA device: %s (was in %s)\n",
           ctx->device.GetDisplayName().c_str(),
           ::NTV2TaskModeToString(ctx->saved_task_mode).c_str());

    if ((ret = aja_setup_video(avctx, ctx, cctx, ctx->video_st)) < 0)
        return ret;
    if (ctx->audio_st && (ret = aja_setup_audio(avctx, ctx, cctx)) < 0)
        return ret;
    if ((ret = aja_start_output(avctx, ctx, cctx)) < 0)
        return ret;

#if CONFIG_LIBKLVANC
    if (ctx->cc_enabled && klvanc_context_create(&ctx->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create a klvanc context\n");
        return AVERROR(ENOMEM);
    }
    if (ctx->cc_enabled)
        av_log(avctx, AV_LOG_INFO,
               "CEA-708 captions from A53 side data go out on VANC line %d\n",
               AJA_HD_LINE_CC);
#endif

    ctx->device_ready = 1;
    return 0;
}

/* Pre-render: watch for the moment to take the card.
 *
 * Two triggers, either or both. A frame count fires once that many frames are
 * waiting in the output buffer, which is the one to use when the hand-over is
 * paced by the outgoing programme ending. A wall-clock time fires at that time,
 * which is the one to use when the schedule says so. With neither, it fires as
 * soon as there is anything to play, which is only useful for testing.
 *
 * The wait matters because claiming the card is what the outgoing process has
 * to have finished with, and -block_until_available makes the claim itself
 * wait. So the two together cover both orders: trigger before the other process
 * has let go, or after. */
static void *aja_trigger_thread(void *arg)
{
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    int ret;

    av_log(avctx, AV_LOG_INFO,
           "Pre-render: holding the card until %s%s%s\n",
           ctx->trigger_frames ? "the buffer fills" : "",
           (ctx->trigger_frames && ctx->trigger_time_us) ? " or " : "",
           ctx->trigger_time_us ? "the time comes" :
               (ctx->trigger_frames ? "" : "there is a frame to play"));

    while (!ctx->trigger_stop) {
        const int depth = aja_queue_depth(&ctx->queue);
        int fire = 0;

        if (ctx->trigger_frames > 0 && depth >= ctx->trigger_frames) {
            av_log(avctx, AV_LOG_INFO,
                   "Pre-render: %d frames buffered\n", depth);
            fire = 1;
        } else if (ctx->trigger_time_us > 0 && av_gettime() >= ctx->trigger_time_us) {
            av_log(avctx, AV_LOG_INFO, "Pre-render: the time came\n");
            fire = 1;
        } else if (!ctx->trigger_frames && !ctx->trigger_time_us && depth > 0) {
            fire = 1;
        }
        if (!fire) {
            av_usleep(10000);
            continue;
        }

        ctx->fired_us = av_gettime_relative();
        ret = aja_claim_and_init(avctx);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: could not take the card\n");
            ctx->thread_error = ret;
            aja_queue_stop(&ctx->queue);
        } else {
            av_log(avctx, AV_LOG_INFO,
                   "Pre-render: card ready %.1f ms after the trigger, %d frames "
                   "in hand\n", (av_gettime_relative() - ctx->fired_us) / 1000.0,
                   aja_queue_depth(&ctx->queue));
        }
        break;
    }
    return NULL;
}
int ff_ajantv2_write_header(AVFormatContext *avctx)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx;
    AVStream *video_st = NULL;
    unsigned i;
    int ret;

    ctx = new (std::nothrow) ajantv2_ctx();
    if (!ctx)
        return AVERROR(ENOMEM);
    cctx->ctx = ctx;
    ctx->avctx = avctx;
    ctx->buffered = cctx->output_buffer_size > 0;
    ctx->last_pts = AV_NOPTS_VALUE;
    ctx->saved_task_mode = NTV2_TASK_MODE_INVALID;
    ctx->late_threshold = cctx->late_threshold;
    pthread_mutex_init(&ctx->audio_lock, NULL);
    pthread_mutex_init(&ctx->tt_lock, NULL);
    ctx->channel = (NTV2Channel)(cctx->channel > 0 ? cctx->channel - 1 : 0);

    /* -output takes a comma-separated list of 1-based SDI connectors. Empty
     * means the connector matching the framestore, which is the usual case. */
    if (cctx->output && *cctx->output) {
        const char *p = cctx->output;
        while (*p) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                av_log(avctx, AV_LOG_ERROR, "Cannot parse -output '%s'\n", cctx->output);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            if (v < 1 || v > 8) {
                av_log(avctx, AV_LOG_ERROR, "SDI output %ld out of range 1-8\n", v);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            ctx->outputs.push_back((NTV2Channel)(v - 1));
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
    }
    /* An empty list is filled in once the device is open and can say how many
     * connectors it has. */

    for (i = 0; i < avctx->nb_streams; i++) {
        AVStream *st = avctx->streams[i];
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            video_st = st;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            if (st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
                ctx->teletext_st = st;
                ctx->teletext_enabled = 1;
            } else {
                av_log(avctx, AV_LOG_WARNING,
                       "Ignoring subtitle stream %u: only DVB teletext is output\n", i);
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (ctx->audio_st) {
                av_log(avctx, AV_LOG_WARNING,
                       "Ignoring audio stream %u: only one is embedded\n", i);
                break;
            }
            if (st->codecpar->codec_id != AV_CODEC_ID_PCM_S32LE &&
                st->codecpar->codec_id != AV_CODEC_ID_PCM_S16LE) {
                av_log(avctx, AV_LOG_ERROR,
                       "Audio must be pcm_s32le or pcm_s16le\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }
            ctx->audio_st = st;
            break;
        default:
            break;
        }
    }
    if (!video_st) {
        av_log(avctx, AV_LOG_ERROR, "A video stream is required\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    ctx->video_st = video_st;

    if (cctx->pre_render) {
        if (!ctx->buffered) {
            av_log(avctx, AV_LOG_ERROR,
                   "Pre-render needs -output_buffer_size: the frames have to go "
                   "somewhere while the card still belongs to another process.\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (ctx->teletext_enabled) {
            av_log(avctx, AV_LOG_ERROR,
                   "Pre-render and teletext do not go together: the generator is "
                   "configured from the video format, which is not known until "
                   "the card is claimed, and subtitles arrive before that.\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (cctx->pre_render_until &&
            av_parse_time(&ctx->trigger_time_us, cctx->pre_render_until, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot read -pre_render_until '%s'\n", cctx->pre_render_until);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        ctx->trigger_frames = cctx->pre_render_frames;
        ctx->pre_render = 1;
    }

    /* The buffer and its thread come up before the card, so that pre-render has
     * somewhere to put frames while it waits. */
    if (ctx->buffered) {
        aja_queue_init(&ctx->queue, cctx->output_buffer_size);
        if (pthread_create(&ctx->thread, NULL, aja_output_thread, ctx)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot start the output thread\n");
            aja_queue_destroy(&ctx->queue);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->thread_started = 1;
        av_log(avctx, AV_LOG_INFO, "Output buffer: %"PRId64" bytes\n",
               cctx->output_buffer_size);
    }

    if (ctx->pre_render) {
        if (pthread_create(&ctx->trigger_thread, NULL, aja_trigger_thread, ctx)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot start the pre-render thread\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->trigger_started = 1;
    } else if ((ret = aja_claim_and_init(avctx)) < 0) {
        goto fail;
    }

    if (ctx->teletext_enabled && ctx->buffered)
        av_log(avctx, AV_LOG_WARNING,
               "Teletext with an output buffer puts captions ahead of the "
               "picture by the depth of the buffer: subtitle packets reach the "
               "generator as they are muxed, while the frames they belong to "
               "wait in the queue.\n");

    if (ctx->teletext_enabled) {
        FFVbiTeletextConfig tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        tcfg.vbi_offset      = cctx->teletext_vbi_offset;
        tcfg.vbi_offset_frac = cctx->teletext_vbi_offset_frac;
        tcfg.shape           = cctx->teletext_shape;
        tcfg.rise_ns         = cctx->teletext_rise_ns;
        tcfg.level           = cctx->teletext_level;
        tcfg.continuous      = cctx->teletext_continuous;
        tcfg.burst_frames    = cctx->teletext_burst_frames;
        tcfg.blank_idle      = cctx->teletext_blank_idle;
        tcfg.dual_field      = cctx->teletext_dual_field;
        tcfg.both_fields     = cctx->teletext_fields == 0;
        tcfg.defer_erase     = cctx->teletext_defer_erase;
        tcfg.filler          = cctx->teletext_filler;
        tcfg.filler_ctrl     = cctx->teletext_filler_ctrl;
        tcfg.filler_subcode  = cctx->teletext_filler_subcode;
        tcfg.spare_p8ff      = cctx->teletext_spare_p8ff;
        tcfg.header_attr     = cctx->teletext_header_attr;
        {   /* The OP-42 s7 cleardown period, in frames of this mode. */
            const NTV2FrameRate fr(::GetNTV2FrameRateFromVideoFormat(ctx->video_format));
            const double fps(::GetFramesPerSecond(fr));
            tcfg.frames_per_10s = fps > 1.0 ? (int)(fps * 10.0 + 0.5) : 250;
        }
        ff_vbi_teletext_init(&ctx->tt, &tcfg);
    }
    return 0;

fail:
    delete ctx;
    cctx->ctx = NULL;
    return ret;
}

int ff_ajantv2_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];

    if (!ctx)
        return AVERROR(EINVAL);

    if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        /* The muxer interleaves by pts, so a caption that arrives here is due
         * now and can go straight into the generator. The DeckLink output
         * queues instead because it pre-renders ahead of the clock; this path
         * has no equivalent yet. */
        if (ctx->teletext_enabled) {
            pthread_mutex_lock(&ctx->tt_lock);
            ff_vbi_teletext_store(&ctx->tt, pkt->data, pkt->size,
                                  pkt->pts, pkt->duration, 1, avctx);
            pthread_mutex_unlock(&ctx->tt_lock);
        }
        return 0;
    }
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (!ctx->audio_fifo)
            return 0;
        pthread_mutex_lock(&ctx->audio_lock);
        if (st->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE) {
            /* The card embeds 32-bit samples, so widen on the way in rather
             * than make the caller do it. */
            const int16_t *src = (const int16_t *)pkt->data;
            const int n = pkt->size / 2;
            for (int i = 0; i < n; i++) {
                const int32_t v = (int32_t)src[i] << 16;
                if (av_fifo_write(ctx->audio_fifo, &v, 4) < 0)
                    break;
            }
        } else if (av_fifo_write(ctx->audio_fifo, pkt->data, pkt->size) < 0) {
            av_log(avctx, AV_LOG_WARNING, "Audio FIFO full, dropping a packet\n");
        }
        pthread_mutex_unlock(&ctx->audio_lock);
        return 0;
    }
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return 0;

    if (!ctx->buffered)
        return aja_deliver_frame(avctx, ctx, pkt, 0);

    if (ctx->thread_error)
        return ctx->thread_error;
    return aja_queue_put(&ctx->queue, pkt);
}

int ff_ajantv2_write_trailer(AVFormatContext *avctx)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)cctx->ctx;

    if (!ctx)
        return 0;

    /* Let the buffer empty onto the card before stopping anything, or the tail
     * of the programme never goes out. A thread that has already failed has
     * nothing to drain into. */
    if (ctx->trigger_started) {
        ctx->trigger_stop = 1;
        pthread_join(ctx->trigger_thread, NULL);
        ctx->trigger_started = 0;
        if (!ctx->device_ready)
            av_log(avctx, AV_LOG_WARNING,
                   "Pre-render never fired, so nothing went to air.\n");
    }

    if (ctx->thread_started) {
        int64_t waited = 0;

        while (aja_queue_depth(&ctx->queue) > 0 && !ctx->thread_error &&
               ctx->device_ready) {
            av_usleep(10000);
            if ((waited += 10000) % 1000000 == 0)
                av_log(avctx, AV_LOG_INFO,
                       "Output buffer draining: %d frames left\n",
                       aja_queue_depth(&ctx->queue));
        }
        ctx->thread_stop = 1;
        aja_queue_stop(&ctx->queue);
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = 0;
        aja_queue_destroy(&ctx->queue);
    }

    av_log(avctx, AV_LOG_INFO, "AJA output: %d frames sent\n", ctx->frames_out);
    if (ctx->device_ready) {
        AUTOCIRCULATE_STATUS st;
        if (ctx->device.AutoCirculateGetStatus(ctx->channel, st))
            av_log(avctx,
                   st.GetDroppedFrameCount() ? AV_LOG_WARNING : AV_LOG_INFO,
                   "AJA output: %u frames processed, %u dropped, %u still on "
                   "the card, state %s\n", st.GetProcessedFrameCount(),
                   st.GetDroppedFrameCount(), st.GetBufferLevel(),
                   ::NTV2AutoCirculateStateToString(st.acState).c_str());
    }
    if (ctx->audio_underruns)
        av_log(avctx, AV_LOG_WARNING, "AJA output: %"PRId64" audio underruns\n",
               ctx->audio_underruns);
    av_fifo_freep2(&ctx->audio_fifo);
#if CONFIG_LIBKLVANC
    if (ctx->vanc_ctx)
        klvanc_context_destroy(ctx->vanc_ctx);
#endif
    if (ctx->device_ready) {
        ctx->device.AutoCirculateStop(ctx->channel);
        ctx->device.UnsubscribeOutputVerticalEvent(ctx->channel);
        ctx->device.ReleaseStreamForApplication(AJA_FOURCC('f','f','m','p'),
                                                (int32_t)getpid());
    }
    /* Handing the card back to the AJA retail service is good manners for a
     * one-off run and wrong in a hand-over: between this process letting go and
     * the next one taking hold, the service reprograms the card from the control
     * panel's settings, and whatever that puts out goes to air in the gap
     * between two programmes. -restore_task_mode 0 leaves it in OEM mode. */
    if (NTV2_IS_VALID_TASK_MODE(ctx->saved_task_mode)) {
        if (cctx->restore_task_mode)
            ctx->device.SetTaskMode(ctx->saved_task_mode);
        else
            av_log(avctx, AV_LOG_INFO,
                   "Leaving the card in OEM mode for whoever takes it next\n");
    }
    pthread_mutex_destroy(&ctx->audio_lock);
    pthread_mutex_destroy(&ctx->tt_lock);
    delete ctx;
    cctx->ctx = NULL;
    return 0;
}

} /* extern "C" */
