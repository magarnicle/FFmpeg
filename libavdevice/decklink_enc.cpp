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

#include <atomic>
#include <unistd.h>
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
#define DECKLINK_CAN_GET_TOTAL_RAM 1
#endif

using std::atomic;

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPIVersion.h>
#include <DeckLinkAPI.h>
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0e030000
#include <DeckLinkAPI_v14_2_1.h>
#endif

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/bytestream.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/time.h"
#include "avdevice.h"
}

#include "decklink_common.h"
#include "decklink_enc.h"
#if CONFIG_LIBKLVANC
#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"
#endif

/* Get total system RAM in bytes, returns 0 if unable to determine */
static int64_t get_total_system_ram(void)
{
#ifdef DECKLINK_CAN_GET_TOTAL_RAM
    av_log(NULL, AV_LOG_DEBUG, "YESSSSSSSSSSSSSSSS\n");
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0)
        return (int64_t)pages * page_size;
#endif
    av_log(NULL, AV_LOG_WARNING, "NOOOOOOOOOOOOOOOOO\n");
    return 0;
}

extern bool operator==(const REFIID& me, const REFIID& other){
    return me.byte0 == other.byte0 &&
	   me.byte1 == other.byte1 &&
	   me.byte2 == other.byte2 &&
	   me.byte3 == other.byte3 &&
	   me.byte4 == other.byte4 &&
	   me.byte5 == other.byte5 &&
	   me.byte6 == other.byte6 &&
	   me.byte7 == other.byte7 &&
	   me.byte8 == other.byte8 &&
	   me.byte9 == other.byte9 &&
	   me.byte10 == other.byte10 &&
	   me.byte11 == other.byte11 &&
	   me.byte12 == other.byte12 &&
	   me.byte13 == other.byte13 &&
	   me.byte14 == other.byte14 &&
	   me.byte15 == other.byte15;
}
/* DeckLink callback class declaration */
class decklink_frame : public IDeckLinkVideoFrame_v14_2_1
{
public:
    decklink_frame(struct decklink_ctx *ctx, AVFrame *avframe, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(avframe), _avpacket(NULL), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width),  _refs(1) { }
    decklink_frame(struct decklink_ctx *ctx, AVPacket *avpacket, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(NULL), _avpacket(avpacket), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _refs(1) { }
    virtual long           STDMETHODCALLTYPE GetWidth      (void)          { return _width; }
    virtual long           STDMETHODCALLTYPE GetHeight     (void)          { return _height; }
    virtual long           STDMETHODCALLTYPE GetRowBytes   (void)
    {
      if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
          return _avframe->linesize[0] < 0 ? -_avframe->linesize[0] : _avframe->linesize[0];
      else
          return ((GetWidth() + 47) / 48) * 128;
    }
    virtual BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat(void)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
            return bmdFormat8BitYUV;
        else
            return bmdFormat10BitYUV;
    }
    virtual BMDFrameFlags  STDMETHODCALLTYPE GetFlags      (void)
    {
       if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
           return _avframe->linesize[0] < 0 ? bmdFrameFlagFlipVertical : bmdFrameFlagDefault;
       else
           return bmdFrameFlagDefault;
    }

    virtual HRESULT        STDMETHODCALLTYPE GetBytes      (void **buffer)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
            if (_avframe->linesize[0] < 0)
                *buffer = (void *)(_avframe->data[0] + _avframe->linesize[0] * (_avframe->height - 1));
            else
                *buffer = (void *)(_avframe->data[0]);
        } else {
            *buffer = (void *)(_avpacket->data);
        }
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE GetTimecode     (BMDTimecodeFormat format, IDeckLinkTimecode **timecode) { return S_FALSE; }
    virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary)
    {
        *ancillary = _ancillary;
        if (_ancillary) {
            _ancillary->AddRef();
            return S_OK;
        } else {
            return S_FALSE;
        }
    }
    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary *ancillary)
    {
        if (_ancillary)
            _ancillary->Release();
        _ancillary = ancillary;
        _ancillary->AddRef();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv)
    {
        if (DECKLINK_IsEqualIID(riid, IID_IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
        } else if (DECKLINK_IsEqualIID(riid, IID_IDeckLinkVideoFrame_v14_2_1)) {
            *ppv = static_cast<IDeckLinkVideoFrame_v14_2_1*>(this);
        } else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return ++_refs; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)
    {
        int ret = --_refs;
        if (!ret) {
            av_frame_free(&_avframe);
            av_packet_free(&_avpacket);
            if (_ancillary)
                _ancillary->Release();
            delete this;
        }
        return ret;
    }

    struct decklink_ctx *_ctx;
    AVFrame *_avframe;
    AVPacket *_avpacket;
    AVCodecID _codec_id;
    IDeckLinkVideoFrameAncillary *_ancillary;
    int _height;
    int _width;

private:
    std::atomic<int>  _refs;
};

class decklink_output_callback : public IDeckLinkVideoOutputCallback_v14_2_1
{
public:
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame_v14_2_1 *_frame, BMDOutputFrameCompletionResult result)
    {
        decklink_frame *frame = static_cast<decklink_frame *>(_frame);
        struct decklink_ctx *ctx = frame->_ctx;

        /* Decode the result code for logging */
        const char *result_str;
        switch (result) {
            case bmdOutputFrameCompleted: result_str = "completed"; break;
            case bmdOutputFrameDisplayedLate: result_str = "displayed_late"; break;
            case bmdOutputFrameDropped: result_str = "dropped"; break;
            case bmdOutputFrameFlushed: result_str = "flushed"; break;
            default: result_str = "unknown"; break;
        }

        if (frame->_avframe) {
            av_frame_unref(frame->_avframe);
            if (result != bmdOutputFrameCompleted) {
                av_log(NULL, AV_LOG_WARNING, "AV Frame callback: result=%s (%d)\n", result_str, result);
            }
        } else if (result != bmdOutputFrameCompleted) {
            av_log(NULL, AV_LOG_WARNING, "Non-AV Frame callback: result=%s (%d)\n", result_str, result);
        }
        if (frame->_avpacket) {
            av_packet_unref(frame->_avpacket);
            if (result != bmdOutputFrameCompleted) {
                av_log(NULL, AV_LOG_WARNING, "AV Packet callback: result=%s (%d)\n", result_str, result);
            }
        }
        if (result != bmdOutputFrameCompleted) {
            uint32_t buffered = 0;
            ctx->dlo->GetBufferedVideoFrameCount(&buffered);
            av_log(NULL, AV_LOG_WARNING, "Frame issue: result=%s, buffered_frames=%d, slots_avail=%d\n",
                   result_str, buffered, ctx->frames_buffer_available_spots);
        } else {
            av_log(NULL, AV_LOG_DEBUG, "Frame completed normally\n");
        }

        bool active = true;
        HRESULT schedule_running = ctx->dlo->IsScheduledPlaybackRunning(&active);
        if (schedule_running != S_OK) {
            av_log(NULL, AV_LOG_WARNING, "decklink schedule running result is not ok: %d\n", schedule_running);
        }
        if (!active){
            av_log(NULL, AV_LOG_WARNING, "decklink active status is false - playback stopped?\n");
        }

        pthread_mutex_lock(&ctx->mutex);
        ctx->frames_buffer_available_spots++;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv)
    {
        if (DECKLINK_IsEqualIID(riid, IID_IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
        } else if (DECKLINK_IsEqualIID(riid, IID_IDeckLinkVideoOutputCallback_v14_2_1)) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback_v14_2_1*>(this);
        } else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return 1; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)                           { return 1; }
};

/* Forward declarations for use in consumer thread */
static int decklink_schedule_video_packet(AVFormatContext *avctx, AVPacket *pkt);
static int decklink_schedule_audio_packet(AVFormatContext *avctx, AVPacket *pkt);

/* Video consumer thread - pulls from video queue and schedules to DeckLink.
 * This thread may block waiting for DeckLink buffer slots, which is why we need
 * a separate audio thread.
 */
static void *decklink_video_output_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    DecklinkPacketQueue *vq = &ctx->output_video_queue;
    AVPacket pkt;
    int ret;
    int idle_count = 0;
    int64_t frames_scheduled = 0;
    int64_t last_log_time = av_gettime_relative();

    av_log(avctx, AV_LOG_INFO, "Video output thread started\n");

    while (!ctx->output_thread_stop) {
        ret = ff_decklink_packet_queue_get(vq, &pkt, 0);
        if (ret <= 0) {
            idle_count++;
            usleep(idle_count < 10 ? 100 : 1000);
            continue;
        }

        idle_count = 0;

        /* Signal producers that space is available */
        pthread_mutex_lock(&vq->mutex);
        pthread_cond_broadcast(&vq->cond);
        pthread_mutex_unlock(&vq->mutex);

        ret = decklink_schedule_video_packet(avctx, &pkt);
        av_packet_unref(&pkt);

        frames_scheduled++;

        int64_t after_schedule = av_gettime_relative();

        /* Periodic status logging */
        if (after_schedule - last_log_time > 5000000) {  /* Every 5 seconds */
            uint32_t buffered_frames = 0;
            ctx->dlo->GetBufferedVideoFrameCount(&buffered_frames);
            av_log(avctx, AV_LOG_INFO, "Video thread: %"PRId64" frames scheduled, %d in DeckLink buffer, %d in queue, %d slots available\n",
                   frames_scheduled, buffered_frames, vq->nb_packets, ctx->frames_buffer_available_spots);
            last_log_time = after_schedule;
        }

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Video output thread: schedule failed with error %d. Stopping.\n", ret);
            ctx->output_thread_error = ret;
            break;
        }
    }

    av_log(avctx, AV_LOG_INFO, "Video output thread exiting\n");
    return NULL;
}

/* Audio consumer thread - pulls from audio queue and schedules to DeckLink.
 * Audio scheduling never blocks, so this thread runs independently of video.
 */
static void *decklink_audio_output_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    DecklinkPacketQueue *aq = &ctx->output_audio_queue;
    AVPacket pkt;
    int ret;
    int idle_count = 0;
    int64_t packets_scheduled = 0;
    int64_t last_log_time = av_gettime_relative();

    av_log(avctx, AV_LOG_INFO, "Audio output thread started\n");

    while (!ctx->output_thread_stop) {
        ret = ff_decklink_packet_queue_get(aq, &pkt, 0);
        if (ret <= 0) {
            idle_count++;
            usleep(idle_count < 10 ? 100 : 1000);
            continue;
        }

        idle_count = 0;

        /* Signal producers that space is available */
        pthread_mutex_lock(&aq->mutex);
        pthread_cond_broadcast(&aq->cond);
        pthread_mutex_unlock(&aq->mutex);

        ret = decklink_schedule_audio_packet(avctx, &pkt);
        av_packet_unref(&pkt);

        packets_scheduled++;

        /* Periodic status logging */
        int64_t now = av_gettime_relative();
        if (now - last_log_time > 5000000) {  /* Every 5 seconds */
            uint32_t buffered_samples = 0;
            ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered_samples);
            av_log(avctx, AV_LOG_INFO, "Audio thread: %"PRId64" packets scheduled, %d samples in DeckLink buffer, %d in queue\n",
                   packets_scheduled, buffered_samples, aq->nb_packets);
            last_log_time = now;
        }

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Audio output thread: schedule failed with error %d. Stopping.\n", ret);
            ctx->output_thread_error = ret;
            break;
        }
    }

    av_log(avctx, AV_LOG_INFO, "Audio output thread exiting\n");
    return NULL;
}

/* Blocking put for output queue - waits if queue is full */
static int decklink_output_queue_put_blocking(struct decklink_ctx *ctx, DecklinkPacketQueue *q, AVPacket *pkt)
{
    int pkt_size = pkt->size;
    int ret;

    if (av_packet_make_refcounted(pkt) < 0) {
        av_packet_unref(pkt);
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    /* Block while queue is full (unless stopping) */
    while ((int64_t)q->size >= q->max_q_size && !ctx->output_thread_stop) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    if (ctx->output_thread_stop) {
        pthread_mutex_unlock(&q->mutex);
        av_packet_unref(pkt);
        return -1;
    }

    ret = avpriv_packet_list_put(&q->pkt_list, pkt, NULL, 0);
    if (ret == 0) {
        q->nb_packets++;
        q->size += pkt_size + sizeof(AVPacket);
        pthread_cond_signal(&q->cond);
    } else {
        av_packet_unref(pkt);
    }

    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static int decklink_setup_video(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    int already_logged;
    AVCodecParameters *c = st->codecpar;

    if (ctx->video) {
    av_log(avctx, AV_LOG_ERROR, "Only one video stream is supported!\n");
    return -1;
}

    if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (c->format != AV_PIX_FMT_UYVY422) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
                    " Only AV_PIX_FMT_UYVY422 is supported.\n");
            return -1;
        }
        ctx->raw_format = bmdFormat8BitYUV;
    } else if (c->codec_id != AV_CODEC_ID_V210) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec type!"
                " Only V210 and wrapped frame with AV_PIX_FMT_UYVY422 are supported.\n");
        return -1;
    } else {
        ctx->raw_format = bmdFormat10BitYUV;
    }

    if (ff_decklink_set_configs(avctx, DIRECTION_OUT) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not set output configuration\n");
        return -1;
    }
    if (ff_decklink_set_format(avctx, c->width, c->height,
                st->time_base.num, st->time_base.den, c->field_order)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported video size, framerate or field order!"
                " Check available formats with -list_formats 1.\n");
        return -1;
    }
    if (ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputVANC) != S_OK) {
        av_log(avctx, AV_LOG_WARNING, "Could not enable video output with VANC! Trying without...\n");
        ctx->supports_vanc = 0;
    }
    already_logged = 0;
    while (!ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputFlagDefault) != S_OK) {
        if (!ctx->block_until_available) {
            av_log(avctx, AV_LOG_ERROR, "Could not enable video output!\n");
            return -1;
        };
        if (!already_logged){
            av_log(avctx, AV_LOG_DEBUG, "Could not enable video output, waiting for device...\n");
            already_logged = 1;
        }
        usleep(1000);
    }


    /* Set callback. */
    ctx->output_callback = new decklink_output_callback();
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);

    ctx->frames_preroll = st->time_base.den * ctx->preroll;
    if (st->time_base.den > 1000)
        ctx->frames_preroll /= 1000;

    /* Buffer twice as many frames as the preroll, minimum 8 for robustness.
     * When async output is enabled, we use a larger buffer (up to 60 frames = 2.4s at 25fps)
     * to handle timing jitter from system scheduling.
     */
    ctx->frames_buffer = FFMAX(ctx->frames_buffer, 8);  /* Minimum 8 frames */
    if (cctx->output_buffer_size > 0) {
        /* With async output, use maximum buffer for robustness against jitter.
         * Keep preroll minimal (2 frames) for fast on-air startup - the large
         * async buffer upstream provides the timing safety, not the preroll.
         */
        ctx->frames_buffer = 60;
    }
    ctx->frames_buffer = FFMIN(ctx->frames_buffer, 60);
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = ctx->frames_buffer;

    av_log(avctx, AV_LOG_DEBUG, "output: %s, preroll: %d, frames buffer size: %d\n",
            avctx->url, ctx->frames_preroll, ctx->frames_buffer);

    /* The device expects the framerate to be fixed. */
    avpriv_set_pts_info(st, 64, st->time_base.num, st->time_base.den);

    ctx->video = 1;



    return 0;
}

static int decklink_setup_audio(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (ctx->audio) {
        av_log(avctx, AV_LOG_ERROR, "Only one audio stream is supported!\n");
        return -1;
    }

    if (c->codec_id == AV_CODEC_ID_AC3) {
        /* Regardless of the number of channels in the codec, we're only
           using 2 SDI audio channels at 48000Hz */
        ctx->channels = 2;
    } else if (c->codec_id == AV_CODEC_ID_PCM_S16LE) {
        if (c->sample_rate != 48000) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate!"
                   " Only 48kHz is supported.\n");
            return -1;
        }
        if (c->ch_layout.nb_channels != 2 && c->ch_layout.nb_channels != 8 && c->ch_layout.nb_channels != 16) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported number of channels!"
                   " Only 2, 8 or 16 channels are supported.\n");
            return -1;
        }
        ctx->channels = c->ch_layout.nb_channels;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec specified!"
               " Only PCM_S16LE and AC-3 are supported.\n");
        return -1;
    }

    if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                    bmdAudioSampleType16bitInteger,
                                    ctx->channels,
                                    bmdAudioOutputStreamTimestamped) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable audio output!\n");
        return -1;
    }
    if (ctx->dlo->BeginAudioPreroll() != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
        return -1;
    }

    /* The device expects the sample rate to be fixed. */
    avpriv_set_pts_info(st, 64, 1, 48000);

    ctx->audio = 1;

    return 0;
}

/* Wrap the AC-3 packet into an S337 payload that is in S16LE format which can be easily
   injected into the PCM stream.  Note: despite the function name, only AC-3 is implemented */
static int create_s337_payload(AVPacket *pkt, uint8_t **outbuf, int *outsize)
{
    /* Note: if the packet size is not divisible by four, we need to make the actual
       payload larger to ensure it ends on an two channel S16LE boundary */
    int payload_size = FFALIGN(pkt->size, 4) + 8;
    uint16_t bitcount = pkt->size * 8;
    uint8_t *s337_payload;
    PutByteContext pb;

    /* Sanity check:  According to SMPTE ST 340:2015 Sec 4.1, the AC-3 sync frame will
       exactly match the 1536 samples of baseband (PCM) audio that it represents.  */
    if (pkt->size > 1536)
        return AVERROR(EINVAL);

    /* Encapsulate AC3 syncframe into SMPTE 337 packet */
    s337_payload = (uint8_t *) av_malloc(payload_size);
    if (s337_payload == NULL)
        return AVERROR(ENOMEM);
    bytestream2_init_writer(&pb, s337_payload, payload_size);
    bytestream2_put_le16u(&pb, 0xf872); /* Sync word 1 */
    bytestream2_put_le16u(&pb, 0x4e1f); /* Sync word 1 */
    bytestream2_put_le16u(&pb, 0x0001); /* Burst Info, including data type (1=ac3) */
    bytestream2_put_le16u(&pb, bitcount); /* Length code */
    for (int i = 0; i < (pkt->size - 1); i += 2)
        bytestream2_put_le16u(&pb, (pkt->data[i] << 8) | pkt->data[i+1]);

    /* Ensure final payload is aligned on 4-byte boundary */
    if (pkt->size & 1)
        bytestream2_put_le16u(&pb, pkt->data[pkt->size - 1] << 8);
    if ((pkt->size & 3) == 1 || (pkt->size & 3) == 2)
        bytestream2_put_le16u(&pb, 0);

    *outsize = payload_size;
    *outbuf = s337_payload;
    return 0;
}

static int decklink_setup_subtitle(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    int ret = -1;

    switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
    case AV_CODEC_ID_EIA_608:
        /* No special setup required */
        ret = 0;
        break;
    case AV_CODEC_ID_DVB_TELETEXT:
        /* Teletext for VANC output */
        ctx->teletext_st = st;
        ff_decklink_packet_queue_init(avctx, &ctx->teletext_queue, 1024 * 1024);
        av_log(avctx, AV_LOG_INFO, "Teletext stream configured for VANC output\n");
        ret = 0;
        break;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported subtitle codec specified\n");
        break;
    }

    return ret;
}

static int decklink_setup_data(AVFormatContext *avctx, AVStream *st)
{
    int ret = -1;

    switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
    case AV_CODEC_ID_SMPTE_2038:
        /* No specific setup required */
        ret = 0;
        break;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported data codec specified\n");
        break;
    }

    return ret;
}

av_cold int ff_decklink_write_trailer(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    uint32_t buffered;
    /* Stop async output thread if running */
    if (ctx->output_thread_started) {
        av_log(avctx, AV_LOG_INFO, "Waiting for async output buffer to drain...\n");

        /* Wait for both queues to drain before stopping */
        int log_counter = 0;
        while (ff_decklink_packet_queue_size(&ctx->output_video_queue) > 0 ||
               ff_decklink_packet_queue_size(&ctx->output_audio_queue) > 0) {
            usleep(10000);  /* 10ms */
            if (++log_counter >= 100) {
                unsigned long long vqsize = ff_decklink_packet_queue_size(&ctx->output_video_queue);
                unsigned long long aqsize = ff_decklink_packet_queue_size(&ctx->output_audio_queue);
                unsigned long long total_size = vqsize + aqsize;
                av_log(avctx, AV_LOG_INFO, "Async buffer (draining): %llu / %"PRId64" bytes (%.1f%%), %d video + %d audio packets\n",
                       total_size, cctx->output_buffer_size,
                       100.0 * total_size / cctx->output_buffer_size,
                       ctx->output_video_queue.nb_packets,
                       ctx->output_audio_queue.nb_packets);
                log_counter = 0;
            }
        }

        /* Signal threads to stop */
        ctx->output_thread_stop = 1;
        pthread_mutex_lock(&ctx->output_video_queue.mutex);
        pthread_cond_broadcast(&ctx->output_video_queue.cond);
        pthread_mutex_unlock(&ctx->output_video_queue.mutex);
        pthread_mutex_lock(&ctx->output_audio_queue.mutex);
        pthread_cond_broadcast(&ctx->output_audio_queue.cond);
        pthread_mutex_unlock(&ctx->output_audio_queue.mutex);

        pthread_join(ctx->output_video_thread, NULL);
        pthread_join(ctx->output_audio_thread, NULL);
        ctx->output_thread_started = 0;
        av_log(avctx, AV_LOG_INFO, "Async output threads stopped\n");
    }

    if (ctx->playback_started) {
        BMDTimeValue actual;
        ctx->dlo->StopScheduledPlayback(ctx->last_pts * ctx->bmd_tb_num,
                                        &actual, ctx->bmd_tb_den);
        pthread_mutex_lock(&ctx->mutex);
        while (ctx->frames_buffer_available_spots < ctx->frames_buffer) {
                 pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);
        while (1){
            ctx->dlo->GetBufferedVideoFrameCount(&buffered);
            if (buffered == 0){
                break;
            }
            av_log(avctx, AV_LOG_DEBUG, "Waiting for %d buffered frames to finish\n", buffered);
            if (buffered < 5) {
                usleep(1);
            } else {
                usleep(300);
            }
        }
        av_log(avctx, AV_LOG_INFO, "All frames returned, finishing up\n");
        av_log(avctx, AV_LOG_INFO, "Stopped at %ld, requested %ld\n", actual, ctx->last_pts * ctx->bmd_tb_num);
        ctx->dlo->DisableVideoOutput();
        if (ctx->audio)
            ctx->dlo->DisableAudioOutput();
    }

    ff_decklink_cleanup(avctx);

    if (ctx->output_callback)
        delete ctx->output_callback;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

#if CONFIG_LIBKLVANC
    klvanc_context_destroy(ctx->vanc_ctx);
#endif
    ff_decklink_packet_queue_end(&ctx->vanc_queue);

    /* Clean up teletext queue if it was used */
    if (ctx->teletext_st)
        ff_decklink_packet_queue_end(&ctx->teletext_queue);

    /* Clean up async output queues if they were used */
    if (cctx->output_buffer_size > 0) {
        ff_decklink_packet_queue_end(&ctx->output_video_queue);
        ff_decklink_packet_queue_end(&ctx->output_audio_queue);
    }

    /* Free source filename tracking */
    av_freep(&ctx->last_logged_source_filename);

    ff_ccfifo_uninit(&ctx->cc_fifo);
    av_freep(&cctx->ctx);

    return 0;
}

#if CONFIG_LIBKLVANC
static void construct_cc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                         AVPacket *pkt, struct klvanc_line_set_s *vanc_lines)
{
    struct klvanc_packet_eia_708b_s *cdp;
    uint16_t *cdp_words;
    uint16_t len;
    uint8_t cc_count;
    size_t size;
    int ret, i;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
    if (!data)
        return;

    cc_count = size / 3;

    ret = klvanc_create_eia708_cdp(&cdp);
    if (ret)
        return;

    ret = klvanc_set_framerate_EIA_708B(cdp, ctx->bmd_tb_num, ctx->bmd_tb_den);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Invalid framerate specified: %" PRId64 "/%" PRId64 "\n",
               ctx->bmd_tb_num, ctx->bmd_tb_den);
        klvanc_destroy_eia708_cdp(cdp);
        return;
    }

    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Illegal cc_count received: %d\n", cc_count);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    /* CC data */
    cdp->header.ccdata_present = 1;
    cdp->header.caption_service_active = 1;
    cdp->ccdata.cc_count = cc_count;
    for (i = 0; i < cc_count; i++) {
        if (data [3*i] & 0x04)
            cdp->ccdata.cc[i].cc_valid = 1;
        cdp->ccdata.cc[i].cc_type = data[3*i] & 0x03;
        cdp->ccdata.cc[i].cc_data[0] = data[3*i+1];
        cdp->ccdata.cc[i].cc_data[1] = data[3*i+2];
    }

    klvanc_finalize_EIA_708B(cdp, ctx->cdp_sequence_num++);
    ret = klvanc_convert_EIA_708B_to_words(cdp, &cdp_words, &len);
    klvanc_destroy_eia708_cdp(cdp);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting 708 packet to words\n");
        return;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, cdp_words, len, 11, 0);
    free(cdp_words);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
        return;
    }
}

/* See SMPTE ST 2016-3:2009 */
static void construct_afd(AVFormatContext *avctx, struct decklink_ctx *ctx,
                          AVPacket *pkt, struct klvanc_line_set_s *vanc_lines,
                          AVStream *st)
{
    struct klvanc_packet_afd_s *afd = NULL;
    uint16_t *afd_words = NULL;
    uint16_t len;
    size_t size;
    int f1_line = 12, f2_line = 0, ret;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_AFD, &size);
    if (!data || size == 0)
        return;

    ret = klvanc_create_AFD(&afd);
    if (ret)
        return;

    ret = klvanc_set_AFD_val(afd, data[0]);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Invalid AFD value specified: %d\n",
               data[0]);
        klvanc_destroy_AFD(afd);
        return;
    }

    /* Compute the AR flag based on the DAR (see ST 2016-1:2009 Sec 9.1).  Note, we treat
       anything below 1.4 as 4:3 (as opposed to the standard 1.33), because there are lots
       of streams in the field that aren't *exactly* 4:3 but a tiny bit larger after doing
       the math... */
    if (av_cmp_q((AVRational) {st->codecpar->width * st->codecpar->sample_aspect_ratio.num,
                    st->codecpar->height * st->codecpar->sample_aspect_ratio.den}, (AVRational) {14, 10}) == 1)
        afd->aspectRatio = ASPECT_16x9;
    else
        afd->aspectRatio = ASPECT_4x3;

    ret = klvanc_convert_AFD_to_words(afd, &afd_words, &len);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting AFD packet to words\n");
        goto out;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, afd_words, len, f1_line, 0);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
        goto out;
    }

    /* For interlaced video, insert into both fields.  Switching lines for field 2
       derived from SMPTE RP 168:2009, Sec 6, Table 2. */
    switch (ctx->bmd_mode) {
    case bmdModeNTSC:
    case bmdModeNTSC2398:
        f2_line = 273 - 10 + f1_line;
        break;
    case bmdModePAL:
        f2_line = 319 - 6 + f1_line;
        break;
    case bmdModeHD1080i50:
    case bmdModeHD1080i5994:
    case bmdModeHD1080i6000:
        f2_line = 569 - 7 + f1_line;
        break;
    default:
        f2_line = 0;
        break;
    }

    if (f2_line > 0) {
        ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, afd_words, len, f2_line, 0);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
            goto out;
        }
    }

out:
    if (afd)
        klvanc_destroy_AFD(afd);
    if (afd_words)
        free(afd_words);
}

/* Convert 8-bit byte to 10-bit VANC word with proper parity
 * Bit 8 = even parity of bits 0-7
 * Bit 9 = NOT bit 8
 */
static inline uint16_t vanc_parity(uint8_t byte)
{
    int p = byte;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    int b8 = (~p) & 1;  /* even parity */
    int b9 = !b8;
    return (b9 << 9) | (b8 << 8) | byte;
}

/* Hamming 8/4 decode table: maps encoded byte to 4-bit value (0xFF = invalid) */
static const uint8_t ham84_decode[256] = {
    0x01, 0xFF, 0x01, 0x01, 0xFF, 0x00, 0x01, 0xFF,
    0xFF, 0x02, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x00, 0x01, 0xFF, 0x00, 0x00, 0xFF, 0x00,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0x06, 0xFF, 0xFF, 0x07, 0xFF, 0x07, 0x07, 0x07,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x0D, 0xFF,
    0x06, 0x06, 0x06, 0xFF, 0x06, 0xFF, 0xFF, 0x07,
    0xFF, 0x02, 0x01, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x02, 0x02, 0xFF, 0x02, 0xFF, 0x02, 0x03, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x00, 0x03, 0xFF,
    0xFF, 0x02, 0x03, 0xFF, 0x03, 0xFF, 0x03, 0x03,
    0x04, 0xFF, 0xFF, 0x05, 0x04, 0x04, 0x04, 0xFF,
    0xFF, 0x02, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x07,
    0xFF, 0x05, 0x05, 0x05, 0x04, 0xFF, 0xFF, 0x05,
    0x06, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x01, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x0A, 0xFF, 0xFF, 0x0B, 0x0A, 0x0A, 0x0A, 0xFF,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x00, 0x0D, 0xFF,
    0xFF, 0x0B, 0x0B, 0x0B, 0x0A, 0xFF, 0xFF, 0x0B,
    0x0C, 0x0C, 0xFF, 0x0C, 0xFF, 0x0C, 0x0D, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x07,
    0xFF, 0x0C, 0x0D, 0xFF, 0x0D, 0xFF, 0x0D, 0x0D,
    0x06, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x0D, 0xFF,
    0x08, 0xFF, 0xFF, 0x09, 0xFF, 0x09, 0x09, 0x09,
    0xFF, 0x02, 0x0F, 0xFF, 0x0A, 0xFF, 0xFF, 0x09,
    0x08, 0x08, 0x08, 0xFF, 0x08, 0xFF, 0xFF, 0x09,
    0x08, 0xFF, 0xFF, 0x0B, 0xFF, 0x0E, 0x03, 0xFF,
    0xFF, 0x0C, 0x0F, 0xFF, 0x04, 0xFF, 0xFF, 0x09,
    0x0F, 0xFF, 0x0F, 0x0F, 0xFF, 0x0E, 0x0F, 0xFF,
    0x08, 0xFF, 0xFF, 0x05, 0xFF, 0x0E, 0x0D, 0xFF,
    0xFF, 0x0E, 0x0F, 0xFF, 0x0E, 0x0E, 0xFF, 0x0E,
};

/* Extract and log text content from teletext data units for debugging.
 * Teletext data unit structure (46 bytes):
 *   [0]: data_unit_id
 *   [1]: data_unit_length
 *   [2]: field_parity + line_offset
 *   [3]: framing_code
 *   [4-5]: magazine/row address (Hamming 8/4 encoded)
 *   [6-45]: 40 characters with odd parity
 *
 * Row encoding in teletext:
 *   byte 4: magazine (bits 0-2) + row bit 0 (bit 3)
 *   byte 5: row bits 1-4
 *   Full row = (decoded_byte5 << 1) | ((decoded_byte4 >> 3) & 1)
 */
static void log_teletext_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int num_data_units = pkt->size / 46;
    char text_buf[41];  /* 40 chars + null terminator */

    av_log(avctx, AV_LOG_DEBUG, "Teletext packet: %d data units, %d bytes\n",
           num_data_units, pkt->size);

    for (int i = 0; i < num_data_units; i++) {
        uint8_t *du = pkt->data + (i * 46);

        /* Decode Hamming 8/4 encoded magazine/row bytes */
        uint8_t byte4_decoded = ham84_decode[du[4]];
        uint8_t byte5_decoded = ham84_decode[du[5]];

        int row;
        if (byte4_decoded == 0xFF || byte5_decoded == 0xFF) {
            /* Hamming decode error - show raw values */
            row = -1;
            av_log(avctx, AV_LOG_DEBUG, "  Data unit %d: Hamming error (raw: %02X %02X)\n",
                   i, du[4], du[5]);
        } else {
            /* Row = (row bits 1-4 from byte5) << 1 | (row bit 0 from byte4 bit 3) */
            int row_bit0 = (byte4_decoded >> 3) & 1;
            row = (byte5_decoded << 1) | row_bit0;
        }

        /* Extract 40 characters, stripping odd parity (bit 7) */
        for (int j = 0; j < 40; j++) {
            uint8_t c = du[6 + j] & 0x7F;  /* Strip parity bit */
            /* Convert control codes and non-printable to spaces */
            if (c < 0x20 || c > 0x7E)
                c = ' ';
            text_buf[j] = c;
        }
        text_buf[40] = '\0';

        /* Trim trailing spaces for cleaner output */
        int len = 40;
        while (len > 0 && text_buf[len - 1] == ' ')
            text_buf[--len] = '\0';

        /* Log all rows, even empty ones, for debugging */
        av_log(avctx, AV_LOG_INFO, "  Row %2d: \"%s\"%s\n",
               row, len > 0 ? text_buf : "(empty)",
               row == 0 ? " [header]" : "");
    }
}

/* Build OP47 VANC packet from teletext data units
 * OP47 is SMPTE RDD-8 which wraps EBU teletext for SDI VANC transport
 * VANC packet format: DID=0x43, SDID=0x02 (field 1) or 0x03 (field 2)
 */
static void construct_teletext(AVFormatContext *avctx, struct decklink_ctx *ctx,
                               struct klvanc_line_set_s *vanc_lines)
{
    AVPacket teletext_pkt;
    int ret;

    /* Process pending teletext packets */
    while (ff_decklink_packet_queue_size(&ctx->teletext_queue) > 0) {
        int64_t pts = ff_decklink_packet_queue_peekpts(&ctx->teletext_queue);
        if (pts > ctx->last_pts) {
            /* Packet is for a future frame */
            break;
        }

        ret = ff_decklink_packet_queue_get(&ctx->teletext_queue, &teletext_pkt, 0);
        if (ret <= 0)
            break;

        if (teletext_pkt.pts + 1 < ctx->last_pts) {
            av_log(avctx, AV_LOG_WARNING, "Teletext packet too old, discarding\n");
            av_packet_unref(&teletext_pkt);
            continue;
        }

        /* Log teletext content at debug level */
        log_teletext_packet(avctx, &teletext_pkt);

        /* The teletext encoder outputs data units (46 bytes each)
         * We build an OP47/SDP VANC packet per SMPTE RDD-8.
         *
         * VANC packet structure (10-bit words with parity):
         *   DID = 0x43
         *   SDID = 0x02 (field 1)
         *   DC = data count
         *   Payload = OP47 structure
         *
         * OP47 payload structure:
         *   [0-1]: Identifier (0x51, 0x15)
         *   [2]: Format code (0x02 = WST teletext)
         *   [3-12]: 5 line descriptors (2 bytes each)
         *   [13+]: Up to 5 x 45-byte teletext packets
         */
        if (teletext_pkt.size >= 46) {
            int num_data_units = teletext_pkt.size / 46;
            if (num_data_units > 5)
                num_data_units = 5;  /* OP47 supports max 5 data units */

            /* Calculate payload size:
             * 2 (identifier) + 1 (format) + 10 (5 line descriptors) + N*45 (teletext data)
             */
            int payload_size = 2 + 1 + 10 + (num_data_units * 45);

            /* VANC words: DID + SDID + DC + payload */
            int vanc_word_count = 3 + payload_size;
            uint16_t *vanc_words = (uint16_t *)av_malloc(vanc_word_count * sizeof(uint16_t));
            if (!vanc_words) {
                av_packet_unref(&teletext_pkt);
                continue;
            }

            int idx = 0;

            /* VANC header - DID, SDID, DC with parity */
            vanc_words[idx++] = vanc_parity(0x43);       /* DID for OP47 */
            vanc_words[idx++] = vanc_parity(0x02);       /* SDID for field 1 */
            vanc_words[idx++] = vanc_parity(payload_size & 0xFF);  /* DC */

            /* OP47 identifier */
            vanc_words[idx++] = vanc_parity(0x51);
            vanc_words[idx++] = vanc_parity(0x15);

            /* Format code: 0x02 = WST teletext */
            vanc_words[idx++] = vanc_parity(0x02);

            /* 5 line descriptors (2 bytes each) */
            for (int i = 0; i < 5; i++) {
                if (i < num_data_units) {
                    uint8_t *du = teletext_pkt.data + (i * 46);
                    /* First byte: field_parity (bit 7) + line_offset (bits 0-4) */
                    uint8_t field_line = du[2];  /* Already has field/line info */
                    vanc_words[idx++] = vanc_parity(field_line);
                    /* Second byte: wrapping/framing info */
                    vanc_words[idx++] = vanc_parity(du[3]);  /* Framing code */
                } else {
                    vanc_words[idx++] = vanc_parity(0xFF);  /* Not used */
                    vanc_words[idx++] = vanc_parity(0xFF);
                }
            }

            /* Teletext data - 45 bytes per data unit (skip data_unit_id byte) */
            for (int i = 0; i < num_data_units; i++) {
                uint8_t *du = teletext_pkt.data + (i * 46);
                /* Copy bytes 1-45 (skip byte 0 which is data_unit_id) */
                for (int j = 1; j < 46; j++) {
                    vanc_words[idx++] = vanc_parity(du[j]);
                }
            }

            /* Insert into VANC line 12 (typical for OP47 teletext in PAL) */
            ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, vanc_words,
                                     idx, 12, 0);
            av_free(vanc_words);

            if (ret != 0) {
                av_log(avctx, AV_LOG_WARNING, "Failed to insert teletext VANC line: %d\n", ret);
            }
        }

        av_packet_unref(&teletext_pkt);
    }
}

/* Parse any EIA-608 subtitles sitting on the queue, and write packet side data
   that will later be handled by construct_cc... */
static void parse_608subs(AVFormatContext *avctx, struct decklink_ctx *ctx, AVPacket *pkt)
{
    size_t cc_size = ff_ccfifo_getoutputsize(&ctx->cc_fifo);
    uint8_t *cc_data;

    if (!ff_ccfifo_ccdetected(&ctx->cc_fifo))
        return;

    cc_data = av_packet_new_side_data(pkt, AV_PKT_DATA_A53_CC, cc_size);
    if (cc_data)
        ff_ccfifo_injectbytes(&ctx->cc_fifo, cc_data, cc_size);
}

static int decklink_construct_vanc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                   AVPacket *pkt, decklink_frame *frame,
                                   AVStream *st)
{
    struct klvanc_line_set_s vanc_lines = { 0 };
    int ret = 0, i;

    if (!ctx->supports_vanc)
        return 0;

    parse_608subs(avctx, ctx, pkt);
    construct_cc(avctx, ctx, pkt, &vanc_lines);
    construct_afd(avctx, ctx, pkt, &vanc_lines, st);

    /* Process any pending teletext packets */
    if (ctx->teletext_st)
        construct_teletext(avctx, ctx, &vanc_lines);

    /* See if there any pending data packets to process */
    while (ff_decklink_packet_queue_size(&ctx->vanc_queue) > 0) {
        AVStream *vanc_st;
        AVPacket vanc_pkt;
        int64_t pts;

        pts = ff_decklink_packet_queue_peekpts(&ctx->vanc_queue);
        if (pts > ctx->last_pts) {
            /* We haven't gotten to the video frame we are supposed to inject
               the oldest VANC packet into yet, so leave it on the queue... */
            break;
        }

        ret = ff_decklink_packet_queue_get(&ctx->vanc_queue, &vanc_pkt, 1);
        if (vanc_pkt.pts + 1 < ctx->last_pts) {
            av_log(avctx, AV_LOG_WARNING, "VANC packet too old, throwing away\n");
            av_packet_unref(&vanc_pkt);
            continue;
        }

        vanc_st = avctx->streams[vanc_pkt.stream_index];
        if (vanc_st->codecpar->codec_id == AV_CODEC_ID_SMPTE_2038) {
            struct klvanc_smpte2038_anc_data_packet_s *pkt_2038 = NULL;

            klvanc_smpte2038_parse_pes_payload(vanc_pkt.data, vanc_pkt.size, &pkt_2038);
            if (pkt_2038 == NULL) {
                av_log(avctx, AV_LOG_ERROR, "failed to decode SMPTE 2038 PES packet");
                av_packet_unref(&vanc_pkt);
                continue;
            }
            for (int i = 0; i < pkt_2038->lineCount; i++) {
                struct klvanc_smpte2038_anc_data_line_s *l = &pkt_2038->lines[i];
                uint16_t *vancWords = NULL;
                uint16_t vancWordCount;

                if (klvanc_smpte2038_convert_line_to_words(l, &vancWords,
                                                           &vancWordCount) < 0)
                    break;

                ret = klvanc_line_insert(ctx->vanc_ctx, &vanc_lines, vancWords,
                                         vancWordCount, l->line_number, 0);
                free(vancWords);
                if (ret != 0) {
                    av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
                    break;
                }
            }
            klvanc_smpte2038_anc_data_packet_free(pkt_2038);
        }
        av_packet_unref(&vanc_pkt);
    }

    IDeckLinkVideoFrameAncillary *vanc;
    int result = ctx->dlo->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create vanc\n");
        ret = AVERROR(EIO);
        goto done;
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output */
    for (i = 0; i < vanc_lines.num_lines; i++) {
        struct klvanc_line_s *line = vanc_lines.lines[i];
        int real_line;
        void *buf;

        if (!line)
            break;

        /* FIXME: include hack for certain Decklink cards which mis-represent
           line numbers for pSF frames */
        real_line = line->line_number;

        result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
        if (result != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get VANC line %d: %d", real_line, result);
            continue;
        }

        /* Generate the full line taking into account all VANC packets on that line */
        result = klvanc_generate_vanc_line_v210(ctx->vanc_ctx, line, (uint8_t *) buf,
                                                ctx->bmd_width);
        if (result) {
            av_log(avctx, AV_LOG_ERROR, "Failed to generate VANC line\n");
            continue;
        }
    }

    result = frame->SetAncillaryData(vanc);
    vanc->Release();
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set vanc: %d", result);
        ret = AVERROR(EIO);
    }

done:
    for (i = 0; i < vanc_lines.num_lines; i++)
        klvanc_line_free(vanc_lines.lines[i]);

    return ret;
}
#endif

/* Extract source filename from packet metadata */
static char *get_packet_source_filename(AVPacket *pkt)
{
    size_t size;
    const uint8_t *side_metadata;
    AVDictionary *metadata = NULL;
    AVDictionaryEntry *entry = NULL;
    char *filename = NULL;

    side_metadata = av_packet_get_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA, &size);
    if (!side_metadata || size == 0)
        return NULL;

    if (av_packet_unpack_dictionary(side_metadata, size, &metadata) < 0)
        return NULL;

    /* Try both possible key names */
    entry = av_dict_get(metadata, "lavf.source_filename", NULL, 0);
    if (!entry)
        entry = av_dict_get(metadata, "lavf.source_basename", NULL, 0);

    if (entry)
        filename = av_strdup(entry->value);

    av_dict_free(&metadata);
    return filename;
}

/* Schedule a video packet to decklink - called directly or from consumer thread */
static int decklink_schedule_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVFrame *avframe = NULL, *tmp = (AVFrame *)pkt->data;
    AVPacket *avpacket = NULL;
    decklink_frame *frame;
    uint32_t buffered;
    HRESULT hr;

    /* Log source filename when it changes */
    char *source_filename = get_packet_source_filename(pkt);
    if (source_filename) {
        /* Only log if filename changed to avoid log spam */
        if (!ctx->last_logged_source_filename ||
            strcmp(source_filename, ctx->last_logged_source_filename) != 0) {
            av_log(avctx, AV_LOG_INFO,
                   "Now playing from source: %s (pts=%"PRId64")\n",
                   source_filename, pkt->pts);
            av_free(ctx->last_logged_source_filename);
            ctx->last_logged_source_filename = av_strdup(source_filename);
        }
        av_free(source_filename);
    }

    /* Check if frame is late and should be dropped or errored */
    if (ctx->playback_started) {
        BMDTimeValue stream_time;
        double speed;
        if (ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &stream_time, &speed) == S_OK) {
            int64_t stream_pts = stream_time / ctx->bmd_tb_num;
            double ahead_secs = (double)(pkt->pts - stream_pts) * ctx->bmd_tb_num / ctx->bmd_tb_den;

            /* Log timing info at debug level */
            av_log(avctx, AV_LOG_DEBUG, "Video timing: pkt_pts=%"PRId64" stream_pts=%"PRId64" ahead=%.3fs speed=%.2f\n",
                   pkt->pts, stream_pts, ahead_secs, speed);

            if (pkt->pts < stream_pts) {
                double behind_secs = -ahead_secs;

                /* Error if too far behind */
                if (cctx->late_threshold > 0 && behind_secs > cctx->late_threshold) {
                    av_log(avctx, AV_LOG_ERROR, "Video frame too late: %.2fs behind (threshold: %.2fs). Aborting.\n",
                           behind_secs, cctx->late_threshold);
                    return AVERROR(EIO);
                }

                av_log(avctx, AV_LOG_WARNING, "Dropping late video frame: pts=%"PRId64" < stream=%"PRId64" (%.2fs behind)\n",
                       pkt->pts, stream_pts, behind_secs);
                ctx->dropped++;
                return 0;  /* Drop frame, but don't error */
            }
        }
    }

    ctx->last_pts = FFMAX(ctx->last_pts, pkt->pts);

    if (st->codecpar->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (tmp->format != AV_PIX_FMT_UYVY422 ||
            tmp->width  != ctx->bmd_width ||
            tmp->height != ctx->bmd_height) {
            av_log(avctx, AV_LOG_ERROR, "Got a frame with invalid pixel format or dimension.\n");
            return AVERROR(EINVAL);
        }

        avframe = av_frame_clone(tmp);
        if (!avframe) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avframe, st->codecpar->codec_id, avframe->height, avframe->width);
    } else {
        avpacket = av_packet_clone(pkt);
        if (!avpacket) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avpacket, st->codecpar->codec_id, ctx->bmd_height, ctx->bmd_width);

#if CONFIG_LIBKLVANC
        if (decklink_construct_vanc(avctx, ctx, pkt, frame, st))
            av_log(avctx, AV_LOG_ERROR, "Failed to construct VANC\n");
#endif
    }

    if (!frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not create new frame.\n");
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return AVERROR(EIO);
    }

    /* Wait for decklink buffer slot */
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->frames_buffer_available_spots == 0) {
        av_log(avctx, AV_LOG_DEBUG, "Video scheduling: waiting for buffer slot (pts=%"PRId64")\n", pkt->pts);
    }
    while (ctx->frames_buffer_available_spots == 0) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    if (ctx->first_pts == AV_NOPTS_VALUE)
        ctx->first_pts = pkt->pts;

    /* Schedule frame for playback. */
    hr = ctx->dlo->ScheduleVideoFrame(frame,
                                      pkt->pts * ctx->bmd_tb_num,
                                      ctx->bmd_tb_num, ctx->bmd_tb_den);
    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x (pts=%"PRId64").\n", (uint32_t) hr, pkt->pts);
        return AVERROR(EIO);
    }

    av_log(avctx, AV_LOG_DEBUG, "Scheduled video frame pts=%"PRId64" (slots_remaining=%d)\n",
           pkt->pts, ctx->frames_buffer_available_spots);

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    if (pkt->pts > 2 && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "Low video buffer: %d frames. Video may stutter!\n", (int) buffered);

    /* Preroll video frames. */
    if (!ctx->playback_started && pkt->pts > (ctx->first_pts + ctx->frames_preroll)) {
        av_log(avctx, AV_LOG_DEBUG, "Ending audio preroll.\n");
        if (ctx->audio && ctx->dlo->EndAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not end audio preroll!\n");
            return AVERROR(EIO);
        }
        av_log(avctx, AV_LOG_INFO, "Starting scheduled playback.\n");
        if (ctx->dlo->StartScheduledPlayback(ctx->first_pts * ctx->bmd_tb_num, ctx->bmd_tb_den, 1.0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not start scheduled playback!\n");
            return AVERROR(EIO);
        }
        ctx->playback_started = 1;
    }

    return 0;
}

/* Entry point for video packets - queues to async buffer or schedules directly */
static int decklink_write_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    /* If async buffer is enabled, queue the packet to the video queue */
    if (ctx->output_thread_started) {
        /* Check for fatal errors from output thread */
        if (ctx->output_thread_error) {
            av_log(avctx, AV_LOG_ERROR, "Output thread encountered fatal error: %d\n", ctx->output_thread_error);
            return ctx->output_thread_error;
        }

        AVPacket *pkt_copy = av_packet_clone(pkt);
        if (!pkt_copy) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone packet for async buffer.\n");
            return AVERROR(ENOMEM);
        }
        int ret = decklink_output_queue_put_blocking(ctx, &ctx->output_video_queue, pkt_copy);
        av_packet_free(&pkt_copy);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue video packet to async buffer.\n");
            return ret;
        }

        /* Check again after queuing in case error occurred during queueing */
        if (ctx->output_thread_error) {
            av_log(avctx, AV_LOG_ERROR, "Output thread encountered fatal error: %d\n", ctx->output_thread_error);
            return ctx->output_thread_error;
        }

        /* Log buffer fill level periodically (every ~100 video frames) */
        static int log_counter = 0;
        if (++log_counter >= 100) {
            unsigned long long vqsize = ff_decklink_packet_queue_size(&ctx->output_video_queue);
            unsigned long long aqsize = ff_decklink_packet_queue_size(&ctx->output_audio_queue);
            unsigned long long total_size = vqsize + aqsize;
            av_log(avctx, AV_LOG_INFO, "Async buffer: %llu / %"PRId64" bytes (%.1f%%), %d video + %d audio packets\n",
                   total_size, cctx->output_buffer_size,
                   100.0 * total_size / cctx->output_buffer_size,
                   ctx->output_video_queue.nb_packets,
                   ctx->output_audio_queue.nb_packets);
            log_counter = 0;
        }
        return 0;
    }

    /* Otherwise schedule directly (synchronous mode) */
    return decklink_schedule_video_packet(avctx, pkt);
}

/* Schedule audio packet to decklink - called directly or from consumer thread */
static int decklink_schedule_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    int sample_count;
    uint32_t buffered;
    uint8_t *outbuf = NULL;
    int ret = 0;

    /* Check if audio is late and should be dropped or errored */
    if (ctx->playback_started) {
        BMDTimeValue stream_time;
        double speed;
        if (ctx->dlo->GetScheduledStreamTime(bmdAudioSampleRate48kHz, &stream_time, &speed) == S_OK) {
            double ahead_secs = (double)(pkt->pts - stream_time) / 48000.0;

            /* Log timing info at debug level */
            av_log(avctx, AV_LOG_DEBUG, "Audio timing: pkt_pts=%"PRId64" stream_time=%"PRId64" ahead=%.3fs speed=%.2f\n",
                   pkt->pts, (int64_t)stream_time, ahead_secs, speed);

            if (pkt->pts < stream_time) {
                double behind_secs = -ahead_secs;

                /* Error if too far behind */
                if (cctx->late_threshold > 0 && behind_secs > cctx->late_threshold) {
                    av_log(avctx, AV_LOG_ERROR, "Audio too late: %.2fs behind (threshold: %.2fs). Aborting.\n",
                           behind_secs, cctx->late_threshold);
                    return AVERROR(EIO);
                }

                av_log(avctx, AV_LOG_WARNING, "Dropping late audio: pts=%"PRId64" < stream=%"PRId64" (%.2fs behind)\n",
                       pkt->pts, (int64_t)stream_time, behind_secs);
                return 0;  /* Drop but don't error */
            }
        }
    }

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    if (pkt->pts > 1 && !buffered){
        av_log(avctx, AV_LOG_WARNING, "No buffered audio. Audio may stutter!\n");
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_AC3) {
        /* Encapsulate AC3 syncframe into SMPTE 337 packet */
        int outbuf_size;
        ret = create_s337_payload(pkt, &outbuf, &outbuf_size);
        if (ret < 0)
            return ret;
        sample_count = outbuf_size / 4;
    } else {
        sample_count = pkt->size / (ctx->channels << 1);
        outbuf = pkt->data;
    }

    if (ctx->dlo->ScheduleAudioSamples(outbuf, sample_count, pkt->pts,
                                       bmdAudioSampleRate48kHz, NULL) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule audio samples.\n");
        ret = AVERROR(EIO);
    }

    if (st->codecpar->codec_id == AV_CODEC_ID_AC3)
        av_freep(&outbuf);

    return ret;
}

/* Entry point for audio packets - queues to async buffer or schedules directly */
static int decklink_write_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    /* If async buffer is enabled, queue the packet to the audio queue */
    if (ctx->output_thread_started) {
        /* Check for fatal errors from output thread */
        if (ctx->output_thread_error) {
            av_log(avctx, AV_LOG_ERROR, "Output thread encountered fatal error: %d\n", ctx->output_thread_error);
            return ctx->output_thread_error;
        }

        AVPacket *pkt_copy = av_packet_clone(pkt);
        if (!pkt_copy) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone audio packet for async buffer.\n");
            return AVERROR(ENOMEM);
        }
        int ret = decklink_output_queue_put_blocking(ctx, &ctx->output_audio_queue, pkt_copy);
        av_packet_free(&pkt_copy);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue audio packet to async buffer.\n");
            return ret;
        }

        /* Check again after queuing in case error occurred during queueing */
        if (ctx->output_thread_error) {
            av_log(avctx, AV_LOG_ERROR, "Output thread encountered fatal error: %d\n", ctx->output_thread_error);
            return ctx->output_thread_error;
        }

        return 0;
    }

    /* Otherwise schedule directly (synchronous mode) */
    return decklink_schedule_audio_packet(avctx, pkt);
}

static int decklink_write_subtitle_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];

    switch (st->codecpar->codec_id) {
    case AV_CODEC_ID_EIA_608:
        ff_ccfifo_extractbytes(&ctx->cc_fifo, pkt->data, pkt->size);
        break;
    case AV_CODEC_ID_DVB_TELETEXT:
        /* Queue teletext packets for VANC insertion */
        if (ff_decklink_packet_queue_put(&ctx->teletext_queue, pkt) < 0) {
            av_log(avctx, AV_LOG_WARNING, "Failed to queue teletext packet\n");
        }
        break;
    default:
        av_log(avctx, AV_LOG_WARNING, "Unsupported subtitle codec in packet\n");
        break;
    }

    return 0;
}

static int decklink_write_data_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ff_decklink_packet_queue_put(&ctx->vanc_queue, pkt) < 0) {
        av_log(avctx, AV_LOG_WARNING, "Failed to queue DATA packet\n");
    }

    return 0;
}

extern "C" {

av_cold int ff_decklink_write_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    unsigned int n;
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->preroll      = cctx->preroll;
    ctx->block_until_available      = cctx->block_until_available;
    ctx->duplex_mode  = cctx->duplex_mode;
    ctx->first_pts    = AV_NOPTS_VALUE;
    if (cctx->link > 0 && (unsigned int)cctx->link < FF_ARRAY_ELEMS(decklink_link_conf_map))
        ctx->link = decklink_link_conf_map[cctx->link];
    cctx->ctx = ctx;
#if CONFIG_LIBKLVANC
    if (klvanc_context_create(&ctx->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create VANC library context\n");
        return AVERROR(ENOMEM);
    }
    ctx->supports_vanc = 1;
#endif

    /* List available devices and exit. */
    if (ctx->list_devices) {
        ff_decklink_list_devices_legacy(avctx, 0, 1);
        return AVERROR_EXIT;
    }

    ret = ff_decklink_init_device(avctx, avctx->url);
    if (ret < 0)
        return ret;

    /* Get output device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkOutput_v14_2_1, (void **) &ctx->dlo) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open output device from '%s'\n",
               avctx->url);
        ret = AVERROR(EIO);
        goto error;
    }

    /* List supported formats. */
    if (ctx->list_formats) {
        ff_decklink_list_formats(avctx);
        ret = AVERROR_EXIT;
        goto error;
    }

    /* Setup streams. */
    ret = AVERROR(EIO);
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if        (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (decklink_setup_audio(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (decklink_setup_video(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_DATA) {
            if (decklink_setup_data(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            if (decklink_setup_subtitle(avctx, st))
                goto error;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            goto error;
        }
    }

    /* Reconfigure the data/subtitle stream clocks to match the video */
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;

        if(c->codec_type == AVMEDIA_TYPE_DATA ||
           c->codec_type == AVMEDIA_TYPE_SUBTITLE)
            avpriv_set_pts_info(st, 64, ctx->bmd_tb_num, ctx->bmd_tb_den);
    }
    ff_decklink_packet_queue_init(avctx, &ctx->vanc_queue, cctx->vanc_queue_size);

    ret = ff_ccfifo_init(&ctx->cc_fifo, av_make_q(ctx->bmd_tb_den, ctx->bmd_tb_num), avctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failure to setup CC FIFO queue\n");
        goto error;
    }

    /* Initialize async output buffer if requested */
    if (cctx->output_buffer_size > 0) {
        int64_t total_ram = get_total_system_ram();
        av_log(avctx, AV_LOG_DEBUG, "Total RAM: %"PRId64" bytes\n", total_ram);
        int64_t max_buffer_size = cctx->output_buffer_size;

        /* Limit buffer size to 80% of total system RAM */
        if (total_ram > 0) {
            int64_t ram_limit = (int64_t)(total_ram * 0.8);
            if (cctx->output_buffer_size > ram_limit) {
                av_log(avctx, AV_LOG_WARNING,
                       "Requested output_buffer_size %"PRId64" exceeds 80%% of system RAM (%"PRId64"), "
                       "clamping to %"PRId64" bytes\n",
                       cctx->output_buffer_size, total_ram, ram_limit);
                max_buffer_size = ram_limit;
            }
        }

        /* Split buffer between video and audio queues.
         * Video frames are much larger (~5MB for 1080p v210) vs audio (~4KB per packet),
         * so allocate 95% to video and 5% to audio. Minimum 100MB for audio to ensure
         * adequate buffering even with large video buffers.
         */
        int64_t audio_buffer_size = FFMAX(max_buffer_size / 20, 100 * 1024 * 1024);  /* 5% or 100MB min */
        int64_t video_buffer_size = max_buffer_size - audio_buffer_size;

        ctx->avctx = avctx;  /* Store for consumer thread access */
        ctx->output_thread_stop = 0;
        ctx->output_thread_error = 0;
        ff_decklink_packet_queue_init(avctx, &ctx->output_video_queue, video_buffer_size);
        ff_decklink_packet_queue_init(avctx, &ctx->output_audio_queue, audio_buffer_size);

        /* Create separate threads for video and audio to prevent blocking issues.
         * Video scheduling can block waiting for DeckLink buffer slots, so audio
         * needs its own thread to ensure continuous scheduling.
         */
        ret = pthread_create(&ctx->output_video_thread, NULL, decklink_video_output_thread, ctx);
        if (ret != 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, AVERROR(ret));
            av_log(avctx, AV_LOG_ERROR, "Failed to create video output thread: %s\n", errbuf);
            ff_decklink_packet_queue_end(&ctx->output_video_queue);
            ff_decklink_packet_queue_end(&ctx->output_audio_queue);
            goto error;
        }

        ret = pthread_create(&ctx->output_audio_thread, NULL, decklink_audio_output_thread, ctx);
        if (ret != 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, AVERROR(ret));
            av_log(avctx, AV_LOG_ERROR, "Failed to create audio output thread: %s\n", errbuf);
            ctx->output_thread_stop = 1;
            pthread_join(ctx->output_video_thread, NULL);
            ff_decklink_packet_queue_end(&ctx->output_video_queue);
            ff_decklink_packet_queue_end(&ctx->output_audio_queue);
            goto error;
        }

        ctx->output_thread_started = 1;
        av_log(avctx, AV_LOG_INFO, "Async output buffer enabled: %"PRId64" bytes video + %"PRId64" bytes audio\n",
               video_buffer_size, audio_buffer_size);
    }

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{

    AVStream *st = avctx->streams[pkt->stream_index];

    if      (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return decklink_write_video_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        return decklink_write_audio_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
        return decklink_write_data_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        return decklink_write_subtitle_packet(avctx, pkt);
    return AVERROR(EIO);
}

int ff_decklink_list_output_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list)
{
    return ff_decklink_list_devices(avctx, device_list, 0, 1);
}

} /* extern "C" */
