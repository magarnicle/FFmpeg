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
#include <sys/time.h>
#include <errno.h>

using std::atomic;

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI_v14_2_1.h>

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
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) 
        {
            if (iid == IID_IDeckLinkVideoFrame_v14_2_1) 
            { 
                *ppv = (IDeckLinkVideoFrame_v14_2_1*)this; 
                AddRef(); 
                return S_OK; 
            }
            return E_NOINTERFACE; 
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

            if (frame->_avframe){
                av_frame_unref(frame->_avframe);
                if (result > 0) {
                    av_log(NULL, AV_LOG_WARNING, "AV Frame was not displayed, result code: %d\n", result);
                }
            } else if (result > 0) {
                av_log(NULL, AV_LOG_WARNING, "Non-AV Frame was not displayed, result code: %d\n", result);
            }
            if (frame->_avpacket) {
                av_packet_unref(frame->_avpacket);
                if (result > 0) {
                    av_log(NULL, AV_LOG_WARNING, "AV Packet was not displayed, result code: %d\n", result);
                }
            }
            if (result > 0) {
                av_log(NULL, AV_LOG_INFO, "decklink output result code: %d\n", result);
            }

            bool active = true;
            HRESULT schedule_running = ctx->dlo->IsScheduledPlaybackRunning(&active);
            if (schedule_running != S_OK) {
                av_log(NULL, AV_LOG_INFO, "decklink schedule running result is not ok: %d\n", schedule_running);
            }
            if (!active){
                av_log(NULL, AV_LOG_INFO, "decklink active status is false\n");
            }

            pthread_mutex_lock(&ctx->mutex);
            ctx->frames_buffer_available_spots++;
            pthread_cond_broadcast(&ctx->cond);
            pthread_mutex_unlock(&ctx->mutex);
            return S_OK;
        }
        virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv)
        {
            if (iid == IID_IDeckLinkVideoOutputCallback_v14_2_1)
            {
                *ppv = (IDeckLinkVideoOutputCallback_v14_2_1*)this;
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return 1; }
        virtual ULONG   STDMETHODCALLTYPE Release(void)                           { return 1; }
};

/* Forward declarations for use in consumer thread */
static int decklink_schedule_video_packet(AVFormatContext *avctx, AVPacket *pkt);
static int decklink_schedule_audio_packet(AVFormatContext *avctx, AVPacket *pkt);

/* Helper function for timed wait with timeout */
static int timed_wait_with_timeout(pthread_cond_t *cond, pthread_mutex_t *mutex, int timeout_ms)
{
    struct timespec ts;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + (timeout_ms / 1000);
    ts.tv_nsec = (tv.tv_usec * 1000) + ((timeout_ms % 1000) * 1000000);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(cond, mutex, &ts);
}

/* Helper function to initialize deferred device output */
static int decklink_initialize_deferred_output(struct decklink_ctx *ctx, AVFormatContext *avctx)
{
    pthread_mutex_lock(&ctx->device_mutex);

    /* Check if already initialized by another thread */
    if (ctx->device_output_ready) {
        pthread_mutex_unlock(&ctx->device_mutex);
        return 0;
    }

    av_log(avctx, AV_LOG_INFO, "Attempting to enable deferred video output...\n");

    /* Try to enable video output */
    HRESULT hr;
    if (ctx->supports_vanc) {
        hr = ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputVANC);
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_WARNING, "Could not enable video output with VANC! Trying without...\n");
            ctx->supports_vanc = 0;
        }
    }

    if (!ctx->supports_vanc) {
        hr = ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputFlagDefault);
        if (hr != S_OK) {
            pthread_mutex_unlock(&ctx->device_mutex);
            /* Device still not available - will retry on next packet */
            return -1;
        }
    }

    /* Enable audio if needed */
    if (ctx->audio) {
        hr = ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                          bmdAudioSampleType16bitInteger,
                                          ctx->channels,
                                          bmdAudioOutputStreamTimestamped);
        if (hr != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not enable audio output after deferred init!\n");
            pthread_mutex_unlock(&ctx->device_mutex);
            return -1;
        }

        if (ctx->dlo->BeginAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll after deferred init!\n");
            pthread_mutex_unlock(&ctx->device_mutex);
            return -1;
        }
    }

    /* Set frame completion callback */
    if (ctx->output_callback) {
        ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);
    }

    av_log(avctx, AV_LOG_INFO, "Device became available - video output enabled, continuing buffering\n");
    ctx->device_output_ready = 1;

    pthread_mutex_unlock(&ctx->device_mutex);
    return 0;
}

/* Consumer thread for async output buffer - pulls packets and schedules to decklink */
static void *decklink_output_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    DecklinkPacketQueue *q = &ctx->output_queue;
    AVPacket pkt;
    int ret;
    int device_wait_logged = 0;

    av_log(avctx, AV_LOG_INFO, "Async output thread started\n");

    while (!ctx->output_thread_stop) {
        /* If device initialization was deferred, check if device is ready */
        if (ctx->device_init_deferred && !ctx->device_output_ready) {
            /* Try to initialize device */
            ret = decklink_initialize_deferred_output(ctx, avctx);
            if (ret < 0) {
                /* Device not yet available */
                if (!device_wait_logged) {
                    av_log(avctx, AV_LOG_INFO, "Device not available yet - continuing to buffer frames while waiting...\n");
                    device_wait_logged = 1;
                }
                /* Continue buffering - don't schedule to hardware yet */
                usleep(100000);  /* 100ms between device checks */
                continue;
            } else {
                av_log(avctx, AV_LOG_INFO, "Device ready - will begin scheduling frames from buffer\n");
            }
        }

        /* Use non-blocking get so we can check stop flag periodically */
        ret = ff_decklink_packet_queue_get(q, &pkt, 0);
        if (ret <= 0) {
            /* No packet available, sleep briefly and retry */
            usleep(1000);  /* 1ms */
            continue;
        }

        /* Signal producers that space is available */
        pthread_mutex_lock(&q->mutex);
        pthread_cond_broadcast(&q->cond);
        pthread_mutex_unlock(&q->mutex);

        /* Only schedule to hardware if device is ready */
        if (ctx->device_output_ready) {
            /* Dispatch based on stream type */
            AVStream *st = avctx->streams[pkt.stream_index];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ret = decklink_schedule_video_packet(avctx, &pkt);
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ret = decklink_schedule_audio_packet(avctx, &pkt);
            } else {
                ret = 0;  /* Ignore other types in async path */
            }

            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Async output thread: schedule failed\n");
            }
        } else {
            /* Device not ready yet - this shouldn't happen but handle gracefully */
            av_log(avctx, AV_LOG_WARNING, "Packet dequeued but device not ready - requeuing\n");
            usleep(10000);
        }

        av_packet_unref(&pkt);
    }

    av_log(avctx, AV_LOG_INFO, "Async output thread exiting\n");
    return NULL;
}

/* Hardware health monitoring thread - checks device state periodically */
static void *decklink_health_monitor_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    uint32_t video_buffered = 0, audio_buffered = 0;
    int64_t check_interval_us = 100000;  // 100ms

    av_log(avctx, AV_LOG_INFO, "Hardware health monitor thread started\n");

    while (!ctx->health_thread_stop) {
        usleep(check_interval_us);

        if (!ctx->playback_started)
            continue;

        /* Check if playback is still running */
        bool active = true;
        HRESULT hr = ctx->dlo->IsScheduledPlaybackRunning(&active);
        if (hr != S_OK) {
            ctx->consecutive_hardware_errors++;
            if (ctx->consecutive_hardware_errors >= 5) {
                av_log(avctx, AV_LOG_ERROR, "Hardware health check failed %d times consecutively. "
                       "Device may be unresponsive.\n", ctx->consecutive_hardware_errors);
                if (ctx->consecutive_hardware_errors >= 50) {
                    av_log(avctx, AV_LOG_FATAL, "Hardware appears permanently unresponsive. "
                           "Consider restarting playout.\n");
                }
            }
        } else if (!active) {
            av_log(avctx, AV_LOG_ERROR, "Scheduled playback has stopped unexpectedly! "
                   "Device may have encountered a fatal error.\n");
            ctx->consecutive_hardware_errors++;
        } else {
            ctx->consecutive_hardware_errors = 0;
        }

        /* Check video buffer health */
        if (ctx->video) {
            ctx->dlo->GetBufferedVideoFrameCount(&video_buffered);
            if (video_buffered <= 2) {
                ctx->consecutive_low_buffer_warnings++;
                if (ctx->consecutive_low_buffer_warnings == 1) {
                    av_log(avctx, AV_LOG_WARNING, "Low video buffer detected: %d frames\n", video_buffered);
                } else if (ctx->consecutive_low_buffer_warnings % 10 == 0) {
                    av_log(avctx, AV_LOG_WARNING, "Video buffer critically low for %d checks: %d frames. "
                           "Processing may not be keeping up with real-time.\n",
                           ctx->consecutive_low_buffer_warnings, video_buffered);
                }
            } else {
                if (ctx->consecutive_low_buffer_warnings > 0) {
                    av_log(avctx, AV_LOG_INFO, "Video buffer recovered: %d frames (after %d low buffer warnings)\n",
                           video_buffered, ctx->consecutive_low_buffer_warnings);
                }
                ctx->consecutive_low_buffer_warnings = 0;
            }
        }

        /* Check audio buffer health */
        if (ctx->audio) {
            ctx->dlo->GetBufferedAudioSampleFrameCount(&audio_buffered);
            if (!audio_buffered) {
                av_log(avctx, AV_LOG_WARNING, "Audio buffer empty! Audio may stutter.\n");
            }
        }

        /* Check async buffer watermark (if enabled) */
        if (ctx->output_thread_started && ctx->prefill_complete) {
            unsigned long long queue_size = ff_decklink_packet_queue_size(&ctx->output_queue);
            int64_t queue_max = ctx->output_queue.max_q_size;
            int buffer_percent = (int)((queue_size * 100) / queue_max);

            /* Warn if buffer drops below 25% (but only once per minute) */
            if (buffer_percent < 25) {
                int64_t now = av_gettime();
                if (now - ctx->last_buffer_low_warning > 60000000) {  // 60 seconds
                    av_log(avctx, AV_LOG_WARNING, "Async buffer low: %d%% (%llu / %"PRId64" bytes). "
                           "Processing may not be keeping ahead of playout.\n",
                           buffer_percent, queue_size, queue_max);
                    ctx->last_buffer_low_warning = now;
                }
            }
        }

        /* Log periodic health status (every 10 seconds) */
        int64_t now = av_gettime();
        if (now - ctx->last_health_check_time > 10000000) {  // 10 seconds
            ctx->last_health_check_time = now;

            if (ctx->output_thread_started) {
                unsigned long long queue_size = ff_decklink_packet_queue_size(&ctx->output_queue);
                int64_t queue_max = ctx->output_queue.max_q_size;
                int buffer_percent = (int)((queue_size * 100) / queue_max);
                av_log(avctx, AV_LOG_INFO, "Health: video_hw_buf=%d audio_hw_buf=%d async_buf=%d%% dropped_total=%d dropped_recent=%d\n",
                       video_buffered, audio_buffered, buffer_percent, ctx->dropped_total, ctx->dropped_recent);
            } else {
                av_log(avctx, AV_LOG_INFO, "Health: video_hw_buf=%d audio_hw_buf=%d dropped_total=%d dropped_recent=%d\n",
                       video_buffered, audio_buffered, ctx->dropped_total, ctx->dropped_recent);
            }
            ctx->dropped_recent = 0;  // Reset recent counter every 10 seconds
        }
    }

    av_log(avctx, AV_LOG_INFO, "Hardware health monitor thread exiting\n");
    return NULL;
}

/* Blocking put for output queue - waits if queue is full with timeout */
static int decklink_output_queue_put_blocking(struct decklink_ctx *ctx, AVPacket *pkt)
{
    DecklinkPacketQueue *q = &ctx->output_queue;
    int pkt_size = pkt->size;
    int ret;
    int timeout_count = 0;

    if (av_packet_make_refcounted(pkt) < 0) {
        av_packet_unref(pkt);
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    /* Block while queue is full (unless stopping) with timeout protection */
    while ((int64_t)q->size >= q->max_q_size && !ctx->output_thread_stop) {
        ret = timed_wait_with_timeout(&q->cond, &q->mutex, 5000);  // 5 second timeout
        if (ret == ETIMEDOUT) {
            timeout_count++;
            if (timeout_count == 1) {
                av_log(ctx->avctx, AV_LOG_WARNING, "Async output queue full for 5 seconds. "
                       "Processing may not be keeping up. Queue: %llu / %"PRId64" bytes\n",
                       q->size, q->max_q_size);
            } else if (timeout_count >= 12) {  // 60 seconds total
                av_log(ctx->avctx, AV_LOG_ERROR, "Async output queue blocked for 60 seconds. "
                       "Aborting to prevent deadlock. Consider increasing output_buffer_size.\n");
                pthread_mutex_unlock(&q->mutex);
                av_packet_unref(pkt);
                return AVERROR(ETIMEDOUT);
            } else if (timeout_count % 6 == 0) {  // Log every 30 seconds
                av_log(ctx->avctx, AV_LOG_WARNING, "Async output queue still full after %d seconds\n",
                       timeout_count * 5);
            }
        }
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
    /* If async buffer is enabled and device is blocked, defer hardware initialization
     * This allows ffmpeg to continue encoding/filtering into the buffer while waiting */
    if (ctx->device_init_deferred) {
        av_log(avctx, AV_LOG_INFO, "Device initialization deferred - will enable output when device becomes available\n");
        ctx->device_output_ready = 0;
    } else {
        /* Normal path: try to enable output immediately */
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
                av_log(avctx, AV_LOG_INFO, "Could not enable video output, waiting for device...\n");
                already_logged = 1;
            }
            usleep(1000);
        }
        av_log(avctx, AV_LOG_INFO, "Device available, video output enabled\n");
        ctx->device_output_ready = 1;
    }


    /* Set callback (even if device init is deferred, we'll need this) */
    ctx->output_callback = new decklink_output_callback();
    if (!ctx->device_init_deferred) {
        ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);
    }

    ctx->frames_preroll = st->time_base.den * ctx->preroll;
    if (st->time_base.den > 1000)
        ctx->frames_preroll /= 1000;

    /* Buffer twice as many frames as the preroll. */
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = FFMAX(ctx->frames_preroll * 2, 30);

    av_log(avctx, AV_LOG_INFO, "output: %s, preroll: %d, frames buffer size: %d\n",
            avctx->url, ctx->frames_preroll, ctx->frames_buffer_available_spots);

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

    /* Audio output will be enabled later if device init is deferred */
    if (!ctx->device_init_deferred) {
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
    } else {
        av_log(avctx, AV_LOG_INFO, "Audio output initialization deferred until device is available\n");
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
    int ret = -1;

    switch(st->codecpar->codec_id) {
#if CONFIG_LIBKLVANC
        case AV_CODEC_ID_EIA_608:
            /* No special setup required */
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

    /* Stop health monitoring thread if running */
    if (ctx->health_thread_started) {
        av_log(avctx, AV_LOG_INFO, "Stopping hardware health monitor...\n");
        ctx->health_thread_stop = 1;
        pthread_join(ctx->health_thread, NULL);
        ctx->health_thread_started = 0;
        av_log(avctx, AV_LOG_INFO, "Health monitor thread stopped\n");
    }

    /* Stop async output thread if running */
    if (ctx->output_thread_started) {
        av_log(avctx, AV_LOG_INFO, "Waiting for async output buffer to drain...\n");

        /* Wait for queue to drain before stopping with timeout */
        int drain_timeout = 0;
        while (ff_decklink_packet_queue_size(&ctx->output_queue) > 0 && drain_timeout < 100) {
            usleep(10000);  /* 10ms */
            drain_timeout++;
        }

        if (drain_timeout >= 100) {
            av_log(avctx, AV_LOG_WARNING, "Async output queue did not drain within 1 second. "
                   "Forcing shutdown. %llu bytes remaining.\n",
                   ff_decklink_packet_queue_size(&ctx->output_queue));
        }

        /* Signal thread to stop */
        ctx->output_thread_stop = 1;
        pthread_mutex_lock(&ctx->output_queue.mutex);
        pthread_cond_broadcast(&ctx->output_queue.cond);
        pthread_mutex_unlock(&ctx->output_queue.mutex);

        pthread_join(ctx->output_thread, NULL);
        ctx->output_thread_started = 0;
        av_log(avctx, AV_LOG_INFO, "Async output thread stopped\n");
    }

    if (ctx->playback_started) {
        BMDTimeValue actual;
        ctx->dlo->StopScheduledPlayback(ctx->last_pts * ctx->bmd_tb_num,
                &actual, ctx->bmd_tb_den);
        av_log(avctx, AV_LOG_INFO, "Stopped at %ld, requested %ld\n", actual, ctx->last_pts * ctx->bmd_tb_num);
        while (1){
            ctx->dlo->GetBufferedVideoFrameCount(&buffered);
            if (buffered == 0){
                break;
            }
            av_log(avctx, AV_LOG_INFO, "Waiting for %d buffered frames to finish\n", buffered);
            if (buffered < 5) {
                usleep(1);
            } else {
                usleep(300);
            }
        }
        av_log(avctx, AV_LOG_INFO, "All frames returned, finishing up\n");

        ctx->dlo->DisableVideoOutput();
        if (ctx->audio)
            ctx->dlo->DisableAudioOutput();
    }

    ff_decklink_cleanup(avctx);

    if (ctx->output_callback)
        delete ctx->output_callback;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->device_mutex);

#if CONFIG_LIBKLVANC
    klvanc_context_destroy(ctx->vanc_ctx);
#endif
    ff_decklink_packet_queue_end(&ctx->vanc_queue);

    /* Clean up async output queue if it was used */
    if (cctx->output_buffer_size > 0) {
        ff_decklink_packet_queue_end(&ctx->output_queue);
    }

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

    /* Check if frame is late and should be dropped or errored */
    if (ctx->playback_started) {
        BMDTimeValue stream_time;
        double speed;
        if (ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &stream_time, &speed) == S_OK) {
            int64_t stream_pts = stream_time / ctx->bmd_tb_num;
            if (pkt->pts < stream_pts) {
                double behind_secs = (double)(stream_pts - pkt->pts) * ctx->bmd_tb_num / ctx->bmd_tb_den;

                /* Error if too far behind */
                if (cctx->late_threshold > 0 && behind_secs > cctx->late_threshold) {
                    av_log(avctx, AV_LOG_ERROR, "Video frame too late: %.2fs behind (threshold: %.2fs). Aborting.\n",
                           behind_secs, cctx->late_threshold);
                    return AVERROR(EIO);
                }

                /* Track dropped frames with exponential backoff logging */
                ctx->dropped_total++;
                ctx->dropped_recent++;

                /* Log with exponential backoff to avoid log spam */
                if (ctx->drop_log_interval == 0)
                    ctx->drop_log_interval = 1;

                if (ctx->dropped_total % ctx->drop_log_interval == 0) {
                    av_log(avctx, AV_LOG_WARNING, "Dropping late video frame #%d: pts=%"PRId64" < stream=%"PRId64" (%.2fs behind)\n",
                           ctx->dropped_total, pkt->pts, stream_pts, behind_secs);

                    /* Increase log interval exponentially, max every 100 drops */
                    if (ctx->drop_log_interval < 100)
                        ctx->drop_log_interval *= 2;
                } else if (ctx->dropped_total == 1) {
                    /* Always log the first drop */
                    av_log(avctx, AV_LOG_WARNING, "First dropped frame: pts=%"PRId64" < stream=%"PRId64" (%.2fs behind)\n",
                           pkt->pts, stream_pts, behind_secs);
                }

                return 0;  /* Drop frame, but don't error */
            } else {
                /* Frame is on time, reset log interval if we had been dropping */
                if (ctx->dropped_total > 0 && ctx->drop_log_interval > 1) {
                    ctx->drop_log_interval = 1;
                }
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

    /* Wait for decklink buffer slot with timeout protection */
    pthread_mutex_lock(&ctx->mutex);
    int wait_attempts = 0;
    while (ctx->frames_buffer_available_spots == 0) {
        int ret = timed_wait_with_timeout(&ctx->cond, &ctx->mutex, 5000);  // 5 second timeout
        if (ret == ETIMEDOUT) {
            wait_attempts++;
            if (wait_attempts == 1) {
                av_log(avctx, AV_LOG_WARNING, "Waiting for hardware buffer slot for 5 seconds. "
                       "Hardware may not be returning frames. Buffered frames: %d\n",
                       ctx->frames_buffer_available_spots);
            } else if (wait_attempts >= 6) {  // 30 seconds total
                av_log(avctx, AV_LOG_ERROR, "Hardware buffer timeout after 30 seconds. "
                       "Hardware appears to have stopped returning frames. Aborting.\n");
                pthread_mutex_unlock(&ctx->mutex);
                av_frame_free(&avframe);
                av_packet_free(&avpacket);
                if (frame)
                    frame->Release();
                return AVERROR(ETIMEDOUT);
            } else if (wait_attempts % 2 == 0) {  // Log every 10 seconds
                av_log(avctx, AV_LOG_WARNING, "Still waiting for hardware buffer after %d seconds\n",
                       wait_attempts * 5);
            }
        }
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    if (ctx->first_pts == AV_NOPTS_VALUE)
        ctx->first_pts = pkt->pts;

    /* Schedule frame for playback. */
    hr = ctx->dlo->ScheduleVideoFrame((class IDeckLinkVideoFrame_v14_2_1 *) frame,
            pkt->pts * ctx->bmd_tb_num,
            ctx->bmd_tb_num, ctx->bmd_tb_den);
    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x.\n", (uint32_t) hr);
        return AVERROR(EIO);
    }

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    if (buffered <= 2) {
        av_log(avctx, AV_LOG_WARNING, "Low video buffer: %d frames. Video may stutter!\n", (int) buffered);
    }

    /* Preroll video frames and wait for async buffer to fill if enabled */
    if (!ctx->playback_started && pkt->pts > (ctx->first_pts + ctx->frames_preroll)) {
        /* If async buffer is enabled, wait for prefill before starting */
        if (ctx->output_thread_started && !ctx->prefill_complete) {
            unsigned long long queue_size = ff_decklink_packet_queue_size(&ctx->output_queue);
            int buffer_percent = (int)((queue_size * 100) / ctx->prefill_target_bytes);

            if (queue_size < ctx->prefill_target_bytes) {
                /* Log prefill progress every 10% */
                static int last_logged_percent = -1;
                if (buffer_percent >= last_logged_percent + 10 || buffer_percent >= 75) {
                    av_log(avctx, AV_LOG_INFO, "Prefilling async buffer: %d%% (%llu / %"PRId64" bytes)\n",
                           buffer_percent, queue_size, ctx->prefill_target_bytes);
                    last_logged_percent = buffer_percent;
                }
                return 0;  /* Continue buffering, don't start playback yet */
            }

            /* Prefill complete */
            ctx->prefill_complete = 1;
            av_log(avctx, AV_LOG_INFO, "Async buffer prefill complete: %llu bytes (target: %"PRId64" bytes)\n",
                   queue_size, ctx->prefill_target_bytes);
        }

        av_log(avctx, AV_LOG_INFO, "Ending audio preroll.\n");
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

    /* If async buffer is enabled, queue the packet */
    if (ctx->output_thread_started) {
        AVPacket *pkt_copy = av_packet_clone(pkt);
        if (!pkt_copy) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone packet for async buffer.\n");
            return AVERROR(ENOMEM);
        }
        int ret = decklink_output_queue_put_blocking(ctx, pkt_copy);
        av_packet_free(&pkt_copy);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue packet to async buffer.\n");
            return ret;
        }

        /* Log buffer fill level periodically (every ~100 video frames) */
        static int log_counter = 0;
        if (++log_counter >= 100) {
            unsigned long long qsize = ff_decklink_packet_queue_size(&ctx->output_queue);
            av_log(avctx, AV_LOG_INFO, "Async buffer: %llu / %"PRId64" bytes (%.1f%%), %d packets\n",
                   qsize, cctx->output_buffer_size,
                   100.0 * qsize / cctx->output_buffer_size,
                   ctx->output_queue.nb_packets);
            log_counter = 0;
        }
        return 0;
    }

    /* Synchronous mode - check if device is ready */
    if (!ctx->device_output_ready) {
        av_log(avctx, AV_LOG_ERROR, "Device output not ready in synchronous mode. This should not happen.\n");
        return AVERROR(EIO);
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
            if (pkt->pts < stream_time) {
                double behind_secs = (double)(stream_time - pkt->pts) / 48000.0;

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
    if (!buffered) {
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

    /* If async buffer is enabled, queue the packet */
    if (ctx->output_thread_started) {
        AVPacket *pkt_copy = av_packet_clone(pkt);
        if (!pkt_copy) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone audio packet for async buffer.\n");
            return AVERROR(ENOMEM);
        }
        int ret = decklink_output_queue_put_blocking(ctx, pkt_copy);
        av_packet_free(&pkt_copy);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to queue audio packet to async buffer.\n");
            return ret;
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

    ff_ccfifo_extractbytes(&ctx->cc_fifo, pkt->data, pkt->size);

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

        /* Initialize health monitoring fields */
        ctx->health_thread_started = 0;
        ctx->health_thread_stop = 0;
        ctx->last_health_check_time = av_gettime();
        ctx->consecutive_low_buffer_warnings = 0;
        ctx->consecutive_hardware_errors = 0;
        ctx->dropped_total = 0;
        ctx->dropped_recent = 0;
        ctx->last_drop_log_time = 0;
        ctx->drop_log_interval = 0;
        ctx->prefill_complete = 0;
        ctx->prefill_target_bytes = 0;
        ctx->last_buffer_low_warning = 0;

        /* Initialize device state */
        ctx->device_output_ready = 0;
        ctx->device_init_deferred = 0;
        pthread_mutex_init(&ctx->device_mutex, NULL);

        /* Decide if we should defer device initialization:
         * Only defer if BOTH async buffer is enabled AND block_until_available is set
         * This allows buffering while waiting for device to become available */
        if (cctx->output_buffer_size > 0 && cctx->block_until_available) {
            ctx->device_init_deferred = 1;
            av_log(avctx, AV_LOG_INFO, "Async buffer enabled with block_until_available - will defer device initialization to allow buffering while waiting\n");
        }
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
            ctx->avctx = avctx;  /* Store for consumer thread access */
            ctx->output_thread_stop = 0;
            ff_decklink_packet_queue_init(avctx, &ctx->output_queue, cctx->output_buffer_size);

            /* Calculate prefill target based on percentage */
            ctx->prefill_target_bytes = (cctx->output_buffer_size * cctx->output_buffer_prefill) / 100;
            if (ctx->prefill_target_bytes < 1)
                ctx->prefill_target_bytes = cctx->output_buffer_size;  /* If 0%, fill completely */

            ret = pthread_create(&ctx->output_thread, NULL, decklink_output_thread, ctx);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to create async output thread: %s\n", av_err2str(AVERROR(ret)));
                ff_decklink_packet_queue_end(&ctx->output_queue);
                goto error;
            }
            ctx->output_thread_started = 1;
            av_log(avctx, AV_LOG_INFO, "Async output buffer enabled: %"PRId64" bytes (will prefill to %d%% = %"PRId64" bytes before starting)\n",
                   cctx->output_buffer_size, cctx->output_buffer_prefill, ctx->prefill_target_bytes);
        } else {
            ctx->avctx = avctx;  /* Store for health monitor even without async buffer */
            ctx->prefill_complete = 1;  /* No async buffer, so prefill is "complete" */
        }

        /* Start hardware health monitoring thread */
        ctx->health_thread_stop = 0;
        ret = pthread_create(&ctx->health_thread, NULL, decklink_health_monitor_thread, ctx);
        if (ret != 0) {
            av_log(avctx, AV_LOG_WARNING, "Failed to create health monitor thread: %s. Continuing without monitoring.\n",
                   av_err2str(AVERROR(ret)));
            ctx->health_thread_started = 0;
        } else {
            ctx->health_thread_started = 1;
            av_log(avctx, AV_LOG_INFO, "Hardware health monitoring enabled\n");
        }

        return 0;

error:
        ff_decklink_cleanup(avctx);
        return ret;
    }

    int ff_decklink_write_packet(AVFormatContext *avctx, AVPacket *pkt)
    {
        AVStream *st = avctx->streams[pkt->stream_index];

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
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
