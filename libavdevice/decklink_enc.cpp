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
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
#define DECKLINK_CAN_GET_TOTAL_RAM 1
#endif

using std::atomic;

/*
 * Socket frame protocol for external frame input.
 * Allows other FFmpeg instances to send frames to the Decklink output.
 *
 * Each frame is sent as:
 *   - 4 bytes: magic number (0x444B4C4B = "DKLK")
 *   - 1 byte:  stream type (0 = video, 1 = audio)
 *   - 4 bytes: data size (network byte order)
 *   - 8 bytes: pts (network byte order)
 *   - 4 bytes: width (video only, network byte order)
 *   - 4 bytes: height (video only, network byte order)
 *   - N bytes: frame data
 */
#define DECKLINK_SOCKET_MAGIC 0x444B4C4B
#define DECKLINK_SOCKET_TYPE_VIDEO 0
#define DECKLINK_SOCKET_TYPE_AUDIO 1

struct decklink_socket_header {
    uint32_t magic;
    uint8_t  stream_type;
    uint32_t data_size;
    int64_t  pts;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

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
#include "decklink_shm.h"
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
/* Format a " -ss=N.NNNs" suffix for playout logs; defined below. */
static void format_seek_suffix(char *buf, size_t buf_size, int64_t seek_us);

/* DeckLink callback class declaration */
class decklink_frame : public IDeckLinkVideoFrame_v14_2_1
{
public:
    decklink_frame(struct decklink_ctx *ctx, AVFrame *avframe, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(avframe), _avpacket(NULL), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _source_filename(NULL), _pts(AV_NOPTS_VALUE), _source_seek_us(AV_NOPTS_VALUE), _refs(1) { }
    decklink_frame(struct decklink_ctx *ctx, AVPacket *avpacket, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(NULL), _avpacket(avpacket), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _source_filename(NULL), _pts(AV_NOPTS_VALUE), _source_seek_us(AV_NOPTS_VALUE), _refs(1) { }
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
            av_freep(&_source_filename);
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
    char *_source_filename;
    int64_t _pts;
    int64_t _source_seek_us;

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

        /* Log the segment the card actually puts on-air. This callback fires in
         * display order as each frame completes, so the first frame of a new
         * source filename marks the real on-air segment boundary (as opposed to
         * the earlier SCHEDULED log emitted when the frame was enqueued). Only
         * frames that were genuinely displayed count. */
        if (frame->_source_filename &&
            (result == bmdOutputFrameCompleted || result == bmdOutputFrameDisplayedLate) &&
            (!ctx->last_onair_source_filename ||
             strcmp(frame->_source_filename, ctx->last_onair_source_filename) != 0)) {
            char seek_suffix[64];
            format_seek_suffix(seek_suffix, sizeof(seek_suffix), frame->_source_seek_us);
            av_log(NULL, AV_LOG_INFO,
                   "decklink: ON-AIR new segment: %s (pts=%"PRId64")%s\n",
                   frame->_source_filename, frame->_pts, seek_suffix);
            av_free(ctx->last_onair_source_filename);
            ctx->last_onair_source_filename = av_strdup(frame->_source_filename);
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
            av_log(NULL, AV_LOG_INFO, "decklink active status is false - playback stopped?\n");
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

/* Promote the calling output thread to SCHED_FIFO so the realtime DeckLink
 * playout cannot be starved by the (SCHED_OTHER) decode/filter worker threads.
 * A CPU spike in the filtergraph at a concat segment boundary previously starved
 * the audio output thread long enough that it fell behind the card's playout
 * clock and tripped the late_threshold abort, even though the async buffer was
 * full. Any SCHED_FIFO priority preempts every SCHED_OTHER worker, so a modest
 * priority is sufficient; the audio thread is given a slightly higher priority
 * than video because it is the timing-critical one (no buffer-slot backpressure
 * to pace it, and the late check aborts the whole output).
 *
 * SCHED_FIFO requires CAP_SYS_NICE or a non-zero RLIMIT_RTPRIO. When that is
 * unavailable pthread_setschedparam() returns EPERM; we fall back to a
 * best-effort negative nice and warn loudly so the operator can grant the
 * privilege. Priority setup never fails the output itself. */
static void decklink_set_output_thread_priority(AVFormatContext *avctx,
                                                const char *name,
                                                int fifo_offset, int nice_fallback)
{
    int min_prio = sched_get_priority_min(SCHED_FIFO);
    int max_prio = sched_get_priority_max(SCHED_FIFO);
    struct sched_param param = {};
    int ret;

    if (min_prio < 0 || max_prio < 0) {
        av_log(avctx, AV_LOG_WARNING,
               "%s thread: SCHED_FIFO unavailable on this platform; running at "
               "default priority.\n", name);
        return;
    }

    param.sched_priority = FFMIN(min_prio + fifo_offset, max_prio);

    /* pthread_setschedparam() returns an errno value directly (0 on success);
     * it does not set the global errno. */
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (ret == 0) {
        av_log(avctx, AV_LOG_INFO,
               "%s thread: SCHED_FIFO priority %d set for realtime output.\n",
               name, param.sched_priority);
        return;
    }

    if (ret == EPERM) {
        /* On Linux the nice value is per-thread, addressed by the kernel tid. */
        errno = 0;
        if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice_fallback) == 0) {
            av_log(avctx, AV_LOG_WARNING,
                   "%s thread: no permission for SCHED_FIFO (need CAP_SYS_NICE "
                   "or a non-zero RLIMIT_RTPRIO); fell back to nice %d. Grant "
                   "realtime scheduling to prevent audio-late playout aborts "
                   "under CPU load.\n", name, nice_fallback);
        } else {
            av_log(avctx, AV_LOG_WARNING,
                   "%s thread: no permission for SCHED_FIFO (need CAP_SYS_NICE "
                   "or a non-zero RLIMIT_RTPRIO) and nice fallback failed (%s); "
                   "running at default priority. Audio-late playout aborts are "
                   "likely under CPU load.\n", name, strerror(errno));
        }
        return;
    }

    av_log(avctx, AV_LOG_WARNING,
           "%s thread: could not set SCHED_FIFO priority (%s); running at "
           "default priority.\n", name, strerror(ret));
}

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
    int pre_render_logged = 0;

    av_log(avctx, AV_LOG_INFO, "Video output thread started\n");
    decklink_set_output_thread_priority(avctx, "Video output", 1, -10);

    while (!ctx->output_thread_stop) {
        /* In pre-render mode, wait for device to be ready */
        if (ctx->pre_render_mode && !ctx->pre_render_device_ready) {
            if (!pre_render_logged) {
                av_log(avctx, AV_LOG_INFO, "Video thread: waiting for pre-render device init (buffer: %d frames)\n",
                       vq->nb_packets);
                pre_render_logged = 1;
            }
            usleep(10000);
            continue;
        }
        pre_render_logged = 0;

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
    int pre_render_logged = 0;

    av_log(avctx, AV_LOG_INFO, "Audio output thread started\n");
    decklink_set_output_thread_priority(avctx, "Audio output", 2, -19);

    while (!ctx->output_thread_stop) {
        /* In pre-render mode, wait for device to be ready */
        if (ctx->pre_render_mode && !ctx->pre_render_device_ready) {
            if (!pre_render_logged) {
                av_log(avctx, AV_LOG_INFO, "Audio thread: waiting for pre-render device init\n");
                pre_render_logged = 1;
            }
            usleep(10000);
            continue;
        }
        pre_render_logged = 0;

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

/* Read exactly n bytes from socket, handling partial reads */
static int socket_read_exact(int fd, void *buf, size_t n)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r <= 0) {
            if (r == 0) return -1;  /* EOF */
            if (errno == EINTR) continue;
            return -1;
        }
        total += r;
    }
    return 0;
}

/* Socket listener thread - accepts connections and queues frames from external sources */
static void *decklink_socket_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    struct decklink_socket_header header;
    uint8_t *frame_data = NULL;
    size_t frame_data_size = 0;
    int connection_count = 0;

    av_log(avctx, AV_LOG_INFO, "Socket server thread started, listening on %s\n", ctx->socket_path);

    while (!ctx->socket_thread_stop) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Accept with timeout using poll */
        struct pollfd pfd = { ctx->socket_fd, POLLIN, 0 };
        int poll_ret = poll(&pfd, 1, 100);  /* 100ms timeout */
        if (poll_ret <= 0) {
            if (poll_ret < 0 && errno != EINTR) {
                av_log(avctx, AV_LOG_ERROR, "Socket poll error: %s\n", strerror(errno));
            }
            continue;
        }

        int client_fd = accept(ctx->socket_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                av_log(avctx, AV_LOG_ERROR, "Socket accept error: %s\n", strerror(errno));
            }
            continue;
        }

        connection_count++;
        av_log(avctx, AV_LOG_INFO, "Socket: client %d connected\n", connection_count);
        int64_t frames_received = 0;

        /* Read frames from this client */
        while (!ctx->socket_thread_stop) {
            /* Read header */
            if (socket_read_exact(client_fd, &header, sizeof(header)) < 0) {
                break;  /* Client disconnected */
            }

            /* Validate magic */
            uint32_t magic = ntohl(header.magic);
            if (magic != DECKLINK_SOCKET_MAGIC) {
                av_log(avctx, AV_LOG_ERROR, "Socket: invalid magic 0x%08X, expected 0x%08X\n",
                       magic, DECKLINK_SOCKET_MAGIC);
                break;
            }

            uint32_t data_size = ntohl(header.data_size);
            int64_t pts = (int64_t)av_be2ne64(header.pts);

            /* Allocate/reallocate frame buffer if needed */
            if (data_size > frame_data_size) {
                av_free(frame_data);
                frame_data = (uint8_t *)av_malloc(data_size);
                if (!frame_data) {
                    av_log(avctx, AV_LOG_ERROR, "Socket: failed to allocate %u bytes\n", data_size);
                    frame_data_size = 0;
                    break;
                }
                frame_data_size = data_size;
            }

            /* Read frame data */
            if (socket_read_exact(client_fd, frame_data, data_size) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Socket: failed to read frame data\n");
                break;
            }

            /* Create packet and queue it */
            AVPacket *pkt = av_packet_alloc();
            if (!pkt) {
                av_log(avctx, AV_LOG_ERROR, "Socket: failed to allocate packet\n");
                break;
            }

            if (av_new_packet(pkt, data_size) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Socket: failed to allocate packet data\n");
                av_packet_free(&pkt);
                break;
            }

            memcpy(pkt->data, frame_data, data_size);
            pkt->pts = pts;
            pkt->dts = pts;

            if (header.stream_type == DECKLINK_SOCKET_TYPE_VIDEO) {
                pkt->stream_index = ctx->video_st ? ctx->video_st->index : 0;
                if (ctx->output_thread_started) {
                    decklink_output_queue_put_blocking(ctx, &ctx->output_video_queue, pkt);
                }
            } else if (header.stream_type == DECKLINK_SOCKET_TYPE_AUDIO) {
                pkt->stream_index = ctx->audio_st ? ctx->audio_st->index : 0;
                if (ctx->output_thread_started) {
                    decklink_output_queue_put_blocking(ctx, &ctx->output_audio_queue, pkt);
                }
            }

            av_packet_free(&pkt);

            frames_received++;
        }

        close(client_fd);
        av_log(avctx, AV_LOG_INFO, "Socket: client %d disconnected after %"PRId64" frames\n",
               connection_count, frames_received);
    }

    av_free(frame_data);
    av_log(avctx, AV_LOG_INFO, "Socket server thread exiting\n");
    return NULL;
}

/* Initialize socket server for external frame input */
static int decklink_init_socket_server(AVFormatContext *avctx, struct decklink_ctx *ctx, const char *path)
{
    struct sockaddr_un addr;

    /* Create socket */
    ctx->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create socket: %s\n", strerror(errno));
        return AVERROR(errno);
    }

    /* Remove existing socket file if present */
    unlink(path);

    /* Bind to path */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(ctx->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to bind socket to %s: %s\n", path, strerror(errno));
        close(ctx->socket_fd);
        ctx->socket_fd = -1;
        return AVERROR(errno);
    }

    /* Listen for connections */
    if (listen(ctx->socket_fd, 5) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to listen on socket: %s\n", strerror(errno));
        close(ctx->socket_fd);
        unlink(path);
        ctx->socket_fd = -1;
        return AVERROR(errno);
    }

    /* Store path for cleanup */
    ctx->socket_path = av_strdup(path);

    /* Start listener thread */
    ctx->socket_thread_stop = 0;
    int ret = pthread_create(&ctx->socket_thread, NULL, decklink_socket_thread, ctx);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create socket thread: %s\n", strerror(ret));
        close(ctx->socket_fd);
        unlink(path);
        av_freep(&ctx->socket_path);
        ctx->socket_fd = -1;
        return AVERROR(ret);
    }

    ctx->socket_thread_started = 1;
    av_log(avctx, AV_LOG_INFO, "Socket server initialized on %s\n", path);
    return 0;
}

/* Cleanup socket server */
static void decklink_cleanup_socket_server(struct decklink_ctx *ctx)
{
    if (ctx->socket_thread_started) {
        ctx->socket_thread_stop = 1;
        pthread_join(ctx->socket_thread, NULL);
        ctx->socket_thread_started = 0;
    }

    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
        ctx->socket_fd = -1;
    }

    if (ctx->socket_path) {
        unlink(ctx->socket_path);
        av_freep(&ctx->socket_path);
    }
}

/*
 * Shared memory reader thread - reads frames from shared memory buffer
 * and queues them for DeckLink output.
 *
 * Handles PTS rebasing: when a new client connects (detected by PTS going
 * backward), we recalculate the PTS offset to make frames appear "on time"
 * relative to the current stream position.
 */
static void *decklink_shm_reader_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    DecklinkShmBuffer *shm = (DecklinkShmBuffer *)ctx->shm_buffer;
    DecklinkShmFrameHeader header;
    uint8_t *frame_data = NULL;
    uint32_t frame_data_size = 0;
    int64_t video_frames = 0, audio_packets = 0;

    /* PTS rebasing state - each client sends PTS starting from 0,
     * server adds cumulative offset so videos concatenate seamlessly.
     * Track video and audio separately since they have different timebases. */
    int64_t video_pts_offset = 0;
    int64_t audio_pts_offset = 0;
    int64_t last_incoming_video_pts = AV_NOPTS_VALUE;
    int64_t last_incoming_audio_pts = AV_NOPTS_VALUE;
    int64_t last_outgoing_video_pts = 0;
    int64_t last_outgoing_audio_pts = 0;
    int64_t video_frame_duration = 1;
    int64_t audio_packet_duration = 1;
    int64_t last_frame_walltime = 0;  /* Wall clock time of last frame received */
    const int64_t GAP_THRESHOLD_US = 2000000;  /* 2 seconds - if gap longer, rebase to current time */
    int video_gap_detected = 0;  /* Set when video detects a gap, cleared when audio processes its first packet */

    av_log(avctx, AV_LOG_INFO, "Shared memory reader thread started\n");

    /* Allocate frame buffer */
    frame_data_size = shm->frame_data_size;
    frame_data = (uint8_t *)av_malloc(frame_data_size);
    if (!frame_data) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate frame buffer\n");
        ctx->output_thread_error = AVERROR(ENOMEM);
        return NULL;
    }

    while (!ctx->shm_reader_stop) {
        int ret = decklink_shm_server_read(shm, &header, frame_data, frame_data_size, 100);

        if (ret == AVERROR(EAGAIN)) {
            continue;  /* Timeout, check stop flag */
        }
        if (ret == AVERROR_EOF) {
            av_log(avctx, AV_LOG_INFO, "Shared memory shutdown signaled\n");
            break;
        }
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Shared memory read error: %d\n", ret);
            ctx->output_thread_error = ret;
            break;
        }

        /* PTS rebasing - track video and audio separately */
        int64_t current_walltime = av_gettime_relative();
        int has_gap = last_frame_walltime > 0 &&
                      (current_walltime - last_frame_walltime) > GAP_THRESHOLD_US;

        if (header.type == DECKLINK_SHM_FRAME_VIDEO) {
            int64_t incoming_pts = header.pts;

            if (header.duration > 0)
                video_frame_duration = header.duration;

            /* Detect new client: PTS jumped backward (new video starting from 0) */
            if (last_incoming_video_pts != AV_NOPTS_VALUE &&
                incoming_pts < last_incoming_video_pts) {

                if (has_gap && ctx->playback_started && ctx->dlo) {
                    /* Gap detected - rebase to current Decklink stream time */
                    BMDTimeValue stream_time;
                    double speed;
                    if (ctx->dlo->GetScheduledStreamTime(ctx->bmd_tb_den, &stream_time, &speed) == S_OK) {
                        int64_t current_stream_pts = stream_time / ctx->bmd_tb_num;
                        video_pts_offset = current_stream_pts - incoming_pts;
                        video_gap_detected = 1;  /* Signal audio to also rebase */
                        av_log(avctx, AV_LOG_INFO, "SHM: New video client after gap (%.1fs), "
                               "rebasing to stream time %"PRId64", offset %"PRId64"\n",
                               (current_walltime - last_frame_walltime) / 1000000.0,
                               current_stream_pts, video_pts_offset);
                    }
                } else {
                    /* No gap - seamless continuation */
                    video_pts_offset = last_outgoing_video_pts + video_frame_duration;
                    av_log(avctx, AV_LOG_INFO, "SHM: New video client (PTS %"PRId64" -> %"PRId64"), "
                           "offset now %"PRId64"\n",
                           last_incoming_video_pts, incoming_pts, video_pts_offset);
                }
            }

            last_incoming_video_pts = incoming_pts;
            last_outgoing_video_pts = incoming_pts + video_pts_offset;
            last_frame_walltime = current_walltime;
        } else if (header.type == DECKLINK_SHM_FRAME_AUDIO) {
            int64_t incoming_pts = header.pts;

            if (header.duration > 0)
                audio_packet_duration = header.duration;

            /* Detect new client: PTS jumped backward (new audio starting from 0) */
            if (last_incoming_audio_pts != AV_NOPTS_VALUE &&
                incoming_pts < last_incoming_audio_pts) {

                if ((has_gap || video_gap_detected) && ctx->playback_started && ctx->dlo) {
                    /* Gap detected (either directly or via video) - rebase to current stream time */
                    BMDTimeValue stream_time;
                    double speed;
                    if (ctx->dlo->GetScheduledStreamTime(48000, &stream_time, &speed) == S_OK) {
                        audio_pts_offset = stream_time - incoming_pts;
                        av_log(avctx, AV_LOG_INFO, "SHM: New audio client after gap, "
                               "rebasing to stream time %"PRId64", offset %"PRId64"\n",
                               stream_time, audio_pts_offset);
                    }
                    video_gap_detected = 0;  /* Clear the flag */
                } else {
                    /* No gap - seamless continuation */
                    audio_pts_offset = last_outgoing_audio_pts + audio_packet_duration;
                    av_log(avctx, AV_LOG_INFO, "SHM: New audio client (PTS %"PRId64" -> %"PRId64"), "
                           "offset now %"PRId64"\n",
                           last_incoming_audio_pts, incoming_pts, audio_pts_offset);
                }
            }

            last_incoming_audio_pts = incoming_pts;
            last_outgoing_audio_pts = incoming_pts + audio_pts_offset;
        }

        /* Create packet and queue it */
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet\n");
            continue;
        }

        if (av_new_packet(pkt, header.size) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet data\n");
            av_packet_free(&pkt);
            continue;
        }

        memcpy(pkt->data, frame_data, header.size);
        pkt->duration = header.duration;

        /* Apply appropriate PTS offset based on stream type */
        if (header.type == DECKLINK_SHM_FRAME_VIDEO) {
            pkt->pts = header.pts + video_pts_offset;
            pkt->dts = header.dts + video_pts_offset;
            pkt->stream_index = ctx->video_st ? ctx->video_st->index : 0;
            if (ctx->output_thread_started) {
                decklink_output_queue_put_blocking(ctx, &ctx->output_video_queue, pkt);
            }
            video_frames++;
        } else if (header.type == DECKLINK_SHM_FRAME_AUDIO) {
            pkt->pts = header.pts + audio_pts_offset;
            pkt->dts = header.dts + audio_pts_offset;
            pkt->stream_index = ctx->audio_st ? ctx->audio_st->index : 0;
            if (ctx->output_thread_started) {
                decklink_output_queue_put_blocking(ctx, &ctx->output_audio_queue, pkt);
            }
            audio_packets++;
        }

        av_packet_free(&pkt);

        /* Periodic logging */
        if ((video_frames + audio_packets) % 1000 == 0) {
            av_log(avctx, AV_LOG_INFO, "SHM reader: %"PRId64" video, %"PRId64" audio, "
                   "buffer: %u/%u frames, v_offset=%"PRId64", a_offset=%"PRId64"\n",
                   video_frames, audio_packets, shm->frame_count, shm->max_frames,
                   video_pts_offset, audio_pts_offset);
        }
    }

    av_free(frame_data);
    av_log(avctx, AV_LOG_INFO, "Shared memory reader thread exiting: %"PRId64" video, %"PRId64" audio\n",
           video_frames, audio_packets);
    return NULL;
}

/* Initialize shared memory server (playout instance) */
static int decklink_init_shm_server(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                     const char *name, int max_frames, uint32_t frame_size)
{
    DecklinkShmBuffer *shm = NULL;
    int ret;

    ret = decklink_shm_server_create(name, max_frames, frame_size, &shm);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create shared memory server: %d\n", ret);
        return ret;
    }

    ctx->shm_buffer = shm;
    ctx->shm_name = av_strdup(name);
    ctx->shm_is_server = 1;

    /* Set video/audio format in shared memory */
    shm->video_width = ctx->bmd_width;
    shm->video_height = ctx->bmd_height;
    shm->video_fps_num = ctx->bmd_tb_den;
    shm->video_fps_den = ctx->bmd_tb_num;
    shm->audio_sample_rate = 48000;  /* TODO: get from audio stream */
    shm->audio_channels = ctx->channels;

    /* Start reader thread */
    ctx->shm_reader_stop = 0;
    ret = pthread_create(&ctx->shm_reader_thread, NULL, decklink_shm_reader_thread, ctx);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create shm reader thread: %s\n", strerror(ret));
        decklink_shm_server_destroy(name, shm);
        ctx->shm_buffer = NULL;
        av_freep(&ctx->shm_name);
        return AVERROR(ret);
    }

    ctx->shm_reader_started = 1;
    av_log(avctx, AV_LOG_INFO, "Shared memory server initialized: %s, %d frames, %u bytes/frame\n",
           name, max_frames, frame_size);
    return 0;
}

/* Initialize shared memory client (encoder instance) */
static int decklink_init_shm_client(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                     const char *name)
{
    DecklinkShmBuffer *shm = NULL;
    int ret;

    ret = decklink_shm_client_attach(name, &shm);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to attach to shared memory: %d\n", ret);
        return ret;
    }

    ctx->shm_buffer = shm;
    ctx->shm_name = av_strdup(name);
    ctx->shm_is_server = 0;

    av_log(avctx, AV_LOG_INFO, "Shared memory client attached: %s\n", name);
    return 0;
}

/* Cleanup shared memory */
static void decklink_cleanup_shm(struct decklink_ctx *ctx)
{
    DecklinkShmBuffer *shm = (DecklinkShmBuffer *)ctx->shm_buffer;

    if (!shm)
        return;

    if (ctx->shm_reader_started) {
        ctx->shm_reader_stop = 1;
        if (ctx->shm_is_server) {
            decklink_shm_server_shutdown(shm);
        }
        pthread_join(ctx->shm_reader_thread, NULL);
        ctx->shm_reader_started = 0;
    }

    if (ctx->shm_is_server) {
        decklink_shm_server_destroy(ctx->shm_name, shm);
    } else {
        decklink_shm_client_detach(shm);
    }

    ctx->shm_buffer = NULL;
    av_freep(&ctx->shm_name);
}

/* Pre-render mode: parse time string (HH:MM:SS or Unix timestamp) to microseconds */
static int64_t parse_pre_render_time(const char *time_str)
{
    if (!time_str || !*time_str)
        return 0;

    /* Parse ISO 8601 format: YYYY-MM-DDTHH:MM:SS[.ffffff]Z (UTC) */
    int year, month, day, hour, min, sec;
    int n;

    n = sscanf(time_str, "%d-%d-%dT%d:%d:%d",
               &year, &month, &day, &hour, &min, &sec);
    if (n != 6)
        return 0;  /* Invalid format */

    /* Verify Z suffix (must be UTC) */
    const char *z = strrchr(time_str, 'Z');
    if (!z || z[1] != '\0')
        return 0;  /* Must end with Z */

    /* Find and parse fractional seconds if present */
    int64_t microsec = 0;
    const char *dot = strchr(time_str, '.');
    if (dot && dot < z) {
        char frac_buf[7] = "000000";
        const char *frac_start = dot + 1;
        int i = 0;
        while (*frac_start >= '0' && *frac_start <= '9' && i < 6) {
            frac_buf[i++] = *frac_start++;
        }
        microsec = strtol(frac_buf, NULL, 10);
    }

    /* Convert to Unix timestamp (UTC) using timegm */
    struct tm tm_utc = {0};
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon = month - 1;
    tm_utc.tm_mday = day;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min = min;
    tm_utc.tm_sec = sec;

    time_t target = timegm(&tm_utc);
    if (target == (time_t)-1)
        return 0;

    return (int64_t)target * 1000000LL + microsec;
}

/* Pre-render mode: complete device initialization (called when trigger fires) */
static int decklink_pre_render_init_device(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    int ret;
    int already_logged = 0;

    av_log(avctx, AV_LOG_INFO, "Pre-render: initializing DeckLink device\n");

    /* Initialize DeckLink device */
    ret = ff_decklink_init_device(avctx, avctx->url);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Pre-render: failed to initialize device\n");
        return ret;
    }

    /* Get output device */
    if (ctx->dl->QueryInterface(IID_IDeckLinkOutput_v14_2_1, (void **) &ctx->dlo) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Pre-render: could not open output device\n");
        ff_decklink_cleanup(avctx);
        return AVERROR(EIO);
    }

    /* Apply stored format */
    if (ff_decklink_set_configs(avctx, DIRECTION_OUT) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Pre-render: could not set output configuration\n");
        ff_decklink_cleanup(avctx);
        return AVERROR(EIO);
    }

    if (ff_decklink_set_format(avctx, ctx->deferred_video_width, ctx->deferred_video_height,
                               ctx->deferred_video_tb.num, ctx->deferred_video_tb.den,
                               (AVFieldOrder)ctx->deferred_video_field_order)) {
        av_log(avctx, AV_LOG_ERROR, "Pre-render: unsupported video format\n");
        ff_decklink_cleanup(avctx);
        return AVERROR(EIO);
    }

    /* Enable video output with retry loop */
    if (ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputVANC) != S_OK) {
        av_log(avctx, AV_LOG_WARNING, "Could not enable video output with VANC! Trying without...\n");
        ctx->supports_vanc = 0;
    }
    while (!ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputFlagDefault) != S_OK) {
        if (!ctx->block_until_available) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: could not enable video output\n");
            ff_decklink_cleanup(avctx);
            return AVERROR(EIO);
        }
        if (!already_logged) {
            av_log(avctx, AV_LOG_DEBUG, "Pre-render: waiting for device...\n");
            already_logged = 1;
        }
        usleep(1000);
    }

    /* Set callback */
    ctx->output_callback = new decklink_output_callback();
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);

    /* Configure buffer sizes */
    ctx->frames_preroll = ctx->deferred_video_tb.den * ctx->preroll;
    if (ctx->deferred_video_tb.den > 1000)
        ctx->frames_preroll /= 1000;
    ctx->frames_buffer = ctx->frames_preroll * 2;
    ctx->frames_buffer = FFMAX(ctx->frames_buffer, 8);
    if (cctx->output_buffer_size > 0)
        ctx->frames_buffer = 60;
    ctx->frames_buffer = FFMIN(ctx->frames_buffer, 60);

    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = ctx->frames_buffer;

    /* Enable audio output if we have audio */
    if (ctx->audio) {
        if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                        ctx->audio_depth == 32 ? bmdAudioSampleType32bitInteger : bmdAudioSampleType16bitInteger,
                                        ctx->channels,
                                        bmdAudioOutputStreamTimestamped) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: could not enable audio output\n");
            ff_decklink_cleanup(avctx);
            return AVERROR(EIO);
        }
        if (ctx->dlo->BeginAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: could not begin audio preroll\n");
            ff_decklink_cleanup(avctx);
            return AVERROR(EIO);
        }
    }

    ctx->pre_render_device_ready = 1;
    av_log(avctx, AV_LOG_INFO, "Pre-render: device initialized successfully\n");

    return 0;
}

/* Pre-render mode: trigger thread monitors conditions and starts playback */
static void *decklink_pre_render_trigger_thread(void *arg)
{
    struct decklink_ctx *ctx = (struct decklink_ctx *)arg;
    AVFormatContext *avctx = ctx->avctx;
    int ret;

    av_log(avctx, AV_LOG_INFO, "Pre-render trigger thread started\n");

    /* Wait for trigger condition */
    while (!ctx->pre_render_trigger_stop) {
        int should_trigger = 0;

        /* Check frame count trigger */
        if (ctx->pre_render_target_frames > 0) {
            int buffered = ctx->output_video_queue.nb_packets;
            if (buffered >= ctx->pre_render_target_frames) {
                av_log(avctx, AV_LOG_INFO, "Pre-render: frame count trigger (%d frames buffered)\n", buffered);
                should_trigger = 1;
            }
        }

        /* Check time trigger */
        if (!should_trigger && ctx->pre_render_start_time > 0) {
            int64_t now = av_gettime();
            if (now >= ctx->pre_render_start_time) {
                av_log(avctx, AV_LOG_INFO, "Pre-render: time trigger fired\n");
                should_trigger = 1;
            }
        }

        /* If no specific trigger, wait for buffer to have some content and device to be available */
        if (!should_trigger && ctx->pre_render_target_frames == 0 && ctx->pre_render_start_time == 0) {
            int buffered = ctx->output_video_queue.nb_packets;
            if (buffered > 0) {
                av_log(avctx, AV_LOG_INFO, "Pre-render: default trigger (buffer has %d frames)\n", buffered);
                should_trigger = 1;
            }
        }

        if (should_trigger) {
            /* Initialize device */
            ret = decklink_pre_render_init_device(avctx);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Pre-render: failed to initialize device, error %d\n", ret);
                ctx->output_thread_error = ret;
            }
            break;
        }

        usleep(10000);  /* Check every 10ms */
    }

    av_log(avctx, AV_LOG_INFO, "Pre-render trigger thread exiting\n");
    return NULL;
}

/* Write frame to shared memory (client mode) */
static int decklink_shm_write_packet(AVFormatContext *avctx, AVPacket *pkt, int type)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    DecklinkShmBuffer *shm = (DecklinkShmBuffer *)ctx->shm_buffer;
    DecklinkShmFrameHeader header;

    if (!shm || ctx->shm_is_server)
        return AVERROR(EINVAL);

    memset(&header, 0, sizeof(header));
    header.magic = DECKLINK_SHM_MAGIC;
    header.type = type;
    header.size = pkt->size;
    header.pts = pkt->pts;
    header.dts = pkt->dts;
    header.duration = pkt->duration;
    header.stream_index = pkt->stream_index;

    if (type == DECKLINK_SHM_FRAME_VIDEO) {
        header.width = ctx->bmd_width;
        header.height = ctx->bmd_height;
    } else if (type == DECKLINK_SHM_FRAME_AUDIO) {
        header.sample_rate = 48000;
        header.channels = ctx->channels;
    }

    /* Block indefinitely (-1) or timeout after 1 second based on option */
    int timeout_ms = cctx->shm_block ? -1 : 1000;
    return decklink_shm_client_write(shm, &header, pkt->data, timeout_ms);
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
    ctx->frames_buffer = ctx->frames_preroll * 2;
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

    /* Shared memory client mode - just cleanup shm and exit */
    if (ctx->shm_buffer && !ctx->shm_is_server) {
        decklink_cleanup_shm(ctx);
        av_log(avctx, AV_LOG_INFO, "Shared memory client finished\n");
        av_freep(&cctx->ctx);
        return 0;
    }

    /* Pre-render: if ffmpeg reached input EOF before the scheduled start time,
     * the entire program fit inside the async output buffer.  Stopping the
     * trigger thread now (below) would tear it down before it ever fires, so
     * the DeckLink is never started and the SDI output stays black across the
     * whole slot.  Hold here until the trigger fires and the device becomes
     * ready, so playout actually begins; the real-time drain below then plays
     * the buffered program out.  Only wait when a future time trigger is
     * pending and we have buffered content to play. */
    if (ctx->pre_render_trigger_started && !ctx->pre_render_device_ready &&
        ctx->pre_render_start_time > 0 && !ctx->output_thread_error &&
        ff_decklink_packet_queue_size(&ctx->output_video_queue) > 0) {
        int64_t now = av_gettime();
        if (now < ctx->pre_render_start_time)
            av_log(avctx, AV_LOG_INFO,
                   "Pre-render: input ended %.1f s before scheduled start; "
                   "holding buffered program until playout trigger fires\n",
                   (ctx->pre_render_start_time - now) / 1000000.0);
        while (!ctx->pre_render_device_ready && !ctx->output_thread_error) {
            if (avctx->interrupt_callback.callback &&
                avctx->interrupt_callback.callback(avctx->interrupt_callback.opaque)) {
                av_log(avctx, AV_LOG_WARNING,
                       "Pre-render: interrupted while waiting for trigger; "
                       "aborting before playout started\n");
                break;
            }
            usleep(10000);  /* 10ms */
        }
    }

    /* Stop pre-render trigger thread first */
    if (ctx->pre_render_trigger_started) {
        ctx->pre_render_trigger_stop = 1;
        pthread_join(ctx->pre_render_trigger_thread, NULL);
        ctx->pre_render_trigger_started = 0;
        av_log(avctx, AV_LOG_INFO, "Pre-render trigger thread stopped\n");
    }

    /* Stop shared memory first so no more frames come in */
    decklink_cleanup_shm(ctx);

    /* Stop socket server first so no more frames come in */
    decklink_cleanup_socket_server(ctx);

    /* Stop async output thread if running */
    if (ctx->output_thread_started) {
        av_log(avctx, AV_LOG_INFO, "Waiting for async output buffer to drain...\n");

        /* Wait for both queues to drain before stopping */
        int log_counter = 0;
        while ((ff_decklink_packet_queue_size(&ctx->output_video_queue) > 0 ||
                ff_decklink_packet_queue_size(&ctx->output_audio_queue) > 0) &&
               !ctx->output_thread_error) {
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
        if (ctx->output_thread_error) {
            av_log(avctx, AV_LOG_WARNING, "Output thread had fatal error, skipping buffer drain\n");
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
        if (!ctx->output_thread_error) {
            while (ctx->frames_buffer_available_spots < ctx->frames_buffer) {
                     pthread_cond_wait(&ctx->cond, &ctx->mutex);
            }
        }
        pthread_mutex_unlock(&ctx->mutex);
        if (!ctx->output_thread_error) {
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
    av_freep(&ctx->last_scheduled_source_filename);
    av_freep(&ctx->last_onair_source_filename);

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

/* Australian OP-47 teletext implementation
 * Per Free TV Australia Operational Practice OP-47 Issue 6 (May 2018):
 *
 * HD-SDI VANC (OP47 SDP packets):
 *   - DID=0x43, SDID=0x02 for Subtitling Distribution Packet
 *   - Insert on HD line 12 (field 1) and HD line 575 (field 2) for 1080i
 *   - VBI packet descriptors set for SD lines 21/334 (for down-conversion)
 *   - Caption data must appear on both fields
 *   - Only one OP47 packet per field (no multi-packets in Australia)
 *
 * SD PAL VBI (raw teletext waveforms):
 *   - Insert on VBI lines 21 (field 1) and 334 (field 2)
 *   - ITU-R System B Teletext (45-byte packets)
 *   - Waveform: clock run-in, framing code 0xE4, 42 data bytes
 */

/* OP-47 SDP packet constants */
#define OP47_DID            0x43
#define OP47_SDID           0x02  /* SDID=0x102 with parity, same for both fields per OP-47 4.2(ii) */
#define OP47_IDENTIFIER_1   0x51
#define OP47_IDENTIFIER_2   0x15
#define OP47_FORMAT_WST     0x02
#define OP47_FOOTER_ID      0x74  /* 0x274 with parity = footer marker */
#define OP47_MAX_VBI_PACKETS 5

/* Australian conventions */
#define AUS_SD_LINE_FIELD1  21    /* SD line 21 for field 1 */
#define AUS_SD_LINE_FIELD2  334   /* SD line 334 for field 2 */
#define AUS_HD_LINE_FIELD1  12    /* HD line 12 for field 1 (1080i) */
#define AUS_HD_LINE_FIELD2  575   /* HD line 575 for field 2 (1080i) */

/* Static sequence counter for SDP Footer Sequence Counter (FSC) */
static uint16_t sdp_sequence_counter = 0;

/* Build an OP-47 SDP (Subtitling Distribution Packet) per Australian conventions
 * Returns the number of 10-bit words written, or 0 on error
 *
 * Per OP-47 5.1, the SDP structure is:
 *   UDW:
 *     IDENTIFIER (0x51, 0x15)
 *     LENGTH (total words from IDENTIFIER to SDP CHECKSUM)
 *     FORMAT CODE (0x02 = WST teletext)
 *     5 x VBI Packet Descriptor Structure A (field/line info)
 *     1-5 x Packet Descriptor Structure B (45-byte teletext packets)
 *     FOOTER ID (0x74)
 *     FOOTER SEQUENCE COUNTER (2 bytes, 16-bit big-endian)
 *     SDP CHECKSUM
 */
static int build_op47_sdp_packet(uint16_t *vanc_words, int max_words,
                                  const uint8_t *teletext_data, int num_data_units,
                                  int field, int sd_line)
{
    if (num_data_units < 1 || num_data_units > OP47_MAX_VBI_PACKETS)
        return 0;

    /* Calculate payload size per OP-47 5.1:
     * 2 (identifiers) + 1 (length) + 1 (format) + 5 (descriptors A)
     * + num_data_units * 45 (structure B) + 1 (footer ID) + 2 (FSC) + 1 (SDP checksum)
     */
    int sdp_payload_size = 2 + 1 + 1 + 5 + (num_data_units * 45) + 1 + 2 + 1;
    int total_words = 3 + sdp_payload_size;  /* DID + SDID + DC + payload */

    if (total_words > max_words)
        return 0;

    int idx = 0;
    uint8_t checksum = 0;

    /* VANC header - DID, SDID, DC with parity */
    vanc_words[idx++] = vanc_parity(OP47_DID);
    vanc_words[idx++] = vanc_parity(OP47_SDID);
    vanc_words[idx++] = vanc_parity(sdp_payload_size & 0xFF);

    /* OP47 identifiers */
    vanc_words[idx++] = vanc_parity(OP47_IDENTIFIER_1);
    checksum += OP47_IDENTIFIER_1;
    vanc_words[idx++] = vanc_parity(OP47_IDENTIFIER_2);
    checksum += OP47_IDENTIFIER_2;

    /* Length word - total from first IDENTIFIER to SDP CHECKSUM inclusive */
    int length_value = sdp_payload_size;
    vanc_words[idx++] = vanc_parity(length_value & 0xFF);
    checksum += (length_value & 0xFF);

    /* Format code: 0x02 = WST teletext */
    vanc_words[idx++] = vanc_parity(OP47_FORMAT_WST);
    checksum += OP47_FORMAT_WST;

    /* 5 VBI Packet Descriptor Structure A words (field/line descriptors)
     * Per OP-47 5.4.1:
     *   bits 0-4: line number (for field 1: line 6-22, for field 2: line 319-335)
     *   bits 5-6: reserved/structure B indicator (0x60 = structure B exists per this spec)
     *   bit 7: field (0=even/field2, 1=odd/field1)
     *
     * Australian convention: always set for SD lines 21/334 to allow down-conversion
     */
    for (int i = 0; i < 5; i++) {
        if (i < num_data_units) {
            uint8_t descriptor;
            if (field == 1) {
                /* Field 1 (odd): line 21 -> offset 15 from line 6 */
                descriptor = (sd_line - 6) | 0x60 | 0x80;
            } else {
                /* Field 2 (even): line 334 -> offset 15 from line 319 */
                descriptor = (sd_line - 319) | 0x60;
            }
            vanc_words[idx++] = vanc_parity(descriptor);
            checksum += descriptor;
        } else {
            vanc_words[idx++] = vanc_parity(0x00);  /* Not used */
        }
    }

    /* Packet Descriptor Structure B - 45 bytes per teletext packet
     * Per OP-47 5.5.2:
     *   run-in code: 2 x 0x55 (reversed from 0xAA)
     *   framing code: 0x27 (reversed from 0xE4)
     *   MRAG: 2 bytes (magazine/row address with Hamming)
     *   data: 40 bytes with parity
     */
    for (int i = 0; i < num_data_units; i++) {
        const uint8_t *du = teletext_data + (i * 46);
        /* Structure B starts at byte 4 of data unit (after data_unit_id, length, field/line, framing)
         * But we need to reconstruct with proper run-in/framing codes */

        /* Run-in code: 2 x 0x55 (bit-reversed 0xAA pattern) */
        vanc_words[idx++] = vanc_parity(0x55);
        checksum += 0x55;
        vanc_words[idx++] = vanc_parity(0x55);
        checksum += 0x55;

        /* Framing code: 0x27 (bit-reversed 0xE4) */
        vanc_words[idx++] = vanc_parity(0x27);
        checksum += 0x27;

        /* 42 bytes of teletext data (MRAG + 40 data bytes from data unit) */
        for (int j = 0; j < 42; j++) {
            uint8_t byte = du[4 + j];  /* Start after header bytes */
            vanc_words[idx++] = vanc_parity(byte);
            checksum += byte;
        }
    }

    /* Footer ID: 0x74 (lower 8 bits of 0x274) */
    vanc_words[idx++] = vanc_parity(OP47_FOOTER_ID);
    checksum += OP47_FOOTER_ID;

    /* Footer Sequence Counter (FSC) - 16-bit big-endian */
    uint16_t fsc = sdp_sequence_counter++;
    uint8_t fsc_hi = (fsc >> 8) & 0xFF;
    uint8_t fsc_lo = fsc & 0xFF;
    vanc_words[idx++] = vanc_parity(fsc_hi);
    checksum += fsc_hi;
    vanc_words[idx++] = vanc_parity(fsc_lo);
    checksum += fsc_lo;

    /* SDP Checksum - value that makes sum mod 256 = 0 */
    uint8_t sdp_checksum = (256 - (checksum & 0xFF)) & 0xFF;
    vanc_words[idx++] = vanc_parity(sdp_checksum);

    return idx;
}

/* Generate teletext VBI waveform for SD PAL output in V210 format
 * Per ITU-R BT.653-3 System B:
 *   - Data rate: 6.9375 Mbit/s
 *   - Sample rate: 13.5 MHz (PAL)
 *   - Samples per bit: ~1.946
 *   - Active samples: 720
 *
 * Waveform structure:
 *   - Clock run-in: 16 bits of alternating 1/0 (0xAAAA)
 *   - Framing code: 0xE4
 *   - MRAG: 2 bytes (magazine/row address)
 *   - Data: 40 bytes with odd parity
 *
 * V210 format: 6 pixels per 16 bytes (4 x 32-bit words)
 *   Word 0: Cb0[9:0], Y0[9:0], Cr0[9:0], xx
 *   Word 1: Y1[9:0], Cb1[9:0], Y2[9:0], xx
 *   Word 2: Cr1[9:0], Y3[9:0], Cb2[9:0], xx
 *   Word 3: Y4[9:0], Cr2[9:0], Y5[9:0], xx
 */
static void generate_teletext_vbi_waveform(uint8_t *line_buf, int line_width,
                                            const uint8_t *teletext_data, int data_len,
                                            int vbi_offset)
{
    /* Teletext timing parameters for PAL/625:
     * Sample rate: 13.5 MHz
     * Data rate: 6.9375 Mbps
     * Samples per bit: 13.5 / 6.9375 = 1.946
     */
    const int SAMPLES_PER_BIT_FP = 498;  /* 1.946 * 256 (fixed point) */
    const int FP_SHIFT = 8;

    /* 10-bit luma levels for teletext signal per OP-42 Section 3
     * Binary "1": 70% +/- 3% of peak white = 64 + 0.70*(940-64) = 677
     * Binary "0": 0% +/- 2% (black level) = 64
     * Note: ETS 300 706 specifies 66%, but OP-42 requires 70% for Australian broadcast
     */
    const uint16_t LUMA_HIGH = 677;   /* 70% per OP-42 */
    const uint16_t LUMA_LOW  = 64;    /* Black level (0 IRE) */
    const uint16_t LUMA_BLACK = 64;   /* Black level */
    const uint16_t CHROMA_NEUTRAL = 512;  /* Neutral chroma */

    /* Create array of 10-bit luma values for the line */
    uint16_t luma[720];
    for (int i = 0; i < 720; i++)
        luma[i] = LUMA_BLACK;

    /* Start position for teletext data in the VBI buffer
     *
     * The DeckLink VBI buffer is 720 samples (same width as PAL active video).
     * The teletext waveform requires approximately 700 samples:
     *   - 16 bits clock run-in × 1.946 samples/bit = 31 samples
     *   - 8 bits framing code × 1.946 = 16 samples
     *   - 336 bits data × 1.946 = 654 samples
     *   - Total: ~700 samples
     *
     * To fit the waveform within the buffer: max start = 720 - 700 = 20 samples.
     * The vbi_offset parameter (configurable via -teletext_vbi_offset) sets
     * the start position, defaulting to 18.
     */
    int pixel_pos = vbi_offset;
    int bit_pos_fp = 0;

    /* Generate clock run-in: 16 bits of alternating 1/0, starting with 1 */
    for (int bit = 0; bit < 16; bit++) {
        uint16_t value = (bit & 1) ? LUMA_LOW : LUMA_HIGH;
        int start_pixel = pixel_pos + (bit_pos_fp >> FP_SHIFT);
        int end_pixel = pixel_pos + ((bit_pos_fp + SAMPLES_PER_BIT_FP) >> FP_SHIFT);
        for (int p = start_pixel; p < end_pixel && p < 720; p++)
            luma[p] = value;
        bit_pos_fp += SAMPLES_PER_BIT_FP;
    }

    /* Generate framing code: 0xE4 per ETS 300 706, transmitted LSB-first */
    uint8_t framing = 0xE4;
    for (int bit = 0; bit < 8; bit++) {
        uint16_t value = (framing & (1 << bit)) ? LUMA_HIGH : LUMA_LOW;
        int start_pixel = pixel_pos + (bit_pos_fp >> FP_SHIFT);
        int end_pixel = pixel_pos + ((bit_pos_fp + SAMPLES_PER_BIT_FP) >> FP_SHIFT);
        for (int p = start_pixel; p < end_pixel && p < 720; p++)
            luma[p] = value;
        bit_pos_fp += SAMPLES_PER_BIT_FP;
    }

    /* Generate 42 bytes of teletext data (MRAG + 40 data bytes), LSB first */
    for (int byte_idx = 0; byte_idx < 42 && byte_idx < data_len; byte_idx++) {
        uint8_t byte = teletext_data[byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            uint16_t value = (byte & (1 << bit)) ? LUMA_HIGH : LUMA_LOW;
            int start_pixel = pixel_pos + (bit_pos_fp >> FP_SHIFT);
            int end_pixel = pixel_pos + ((bit_pos_fp + SAMPLES_PER_BIT_FP) >> FP_SHIFT);
            for (int p = start_pixel; p < end_pixel && p < 720; p++)
                luma[p] = value;
            bit_pos_fp += SAMPLES_PER_BIT_FP;
        }
    }

    /* Convert to V210 format: 6 pixels per 16 bytes */
    uint32_t *v210 = (uint32_t *)line_buf;
    for (int i = 0; i < 720; i += 6) {
        uint16_t y0 = luma[i];
        uint16_t y1 = luma[i + 1];
        uint16_t y2 = luma[i + 2];
        uint16_t y3 = luma[i + 3];
        uint16_t y4 = luma[i + 4];
        uint16_t y5 = luma[i + 5];

        /* V210 packing (little-endian 32-bit words):
         * Word 0: Cb0 | Y0 | Cr0
         * Word 1: Y1 | Cb1 | Y2
         * Word 2: Cr1 | Y3 | Cb2
         * Word 3: Y4 | Cr2 | Y5
         */
        *v210++ = (CHROMA_NEUTRAL) | (y0 << 10) | (CHROMA_NEUTRAL << 20);
        *v210++ = (y1) | (CHROMA_NEUTRAL << 10) | (y2 << 20);
        *v210++ = (CHROMA_NEUTRAL) | (y3 << 10) | (CHROMA_NEUTRAL << 20);
        *v210++ = (y4) | (CHROMA_NEUTRAL << 10) | (y5 << 20);
    }
}

/* Dummy header teletext packet per OP-42 Section 8 (Time Filling Headers)
 * Magazine 8 (encoded as 0), Row 0 (page header), Page FF
 *
 * Per OP-42: "time filling headers... are by convention 0,FF (Magazine 0
 * known as 8, page FF) with a subcode of 0x3F7E"
 *
 * Page header structure per ETS 300 706:
 *   Bytes 0-1:   MRAG (magazine 8 encoded as 0, row 0)
 *   Bytes 2-3:   Page FF
 *   Bytes 4-9:   Subcode + C4-C7 (all 0 for dummy header)
 *   Bytes 10-11: C8-C14 (all 0)
 *   Bytes 12-41: 30 display characters (spaces)
 *
 * Hamming 8/4 values: ham84(0)=0x15, ham84(0xF)=0xEA
 */
static const uint8_t teletext_filler_packet[42] = {
    0x15, 0x15,  /* MRAG: magazine 8 (encoded 0), row 0 */
    0xEA, 0xEA,  /* Page FF (units=F, tens=F) */
    0x15, 0x15,  /* S1 low, S1 high + C4=0 */
    0x15, 0x15,  /* S2 low, S2 high + C5=0 */
    0x15, 0x15,  /* S3, S4 + C6=0 + C7=0 */
    0x15, 0x15,  /* C8-C11=0, C12-C14=0 */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,  /* 30 spaces */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20
};

/* Filler teletext data unit for HD VANC (OP-47 SDP format)
 * Contains dummy header per OP-42 Section 8 (page 8FF)
 * Structure: data_unit_id (0x02=non-subtitle), length (0x2C=44),
 *            field/line (0x00), framing_code (0xE4),
 *            42-byte payload (dummy header for page 8FF)
 */
static const uint8_t teletext_filler_data_unit[46] = {
    0x02,        /* data_unit_id: EBU teletext non-subtitle (dummy header) */
    0x2C,        /* data_unit_length: 44 */
    0x00,        /* reserved/field/line */
    0xE4,        /* framing_code */
    0x15, 0x15,  /* MRAG: magazine 8 (encoded 0), row 0 */
    0xEA, 0xEA,  /* Page FF (units=F, tens=F) */
    0x15, 0x15,  /* S1 low, S1 high + C4=0 */
    0x15, 0x15,  /* S2 low, S2 high + C5=0 */
    0x15, 0x15,  /* S3, S4 + C6=0 + C7=0 */
    0x15, 0x15,  /* C8-C11=0, C12-C14=0 */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,  /* 30 spaces */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20
};

/* Insert OP-47 VANC packet on specified line */
static int insert_op47_vanc_line(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                  struct klvanc_line_set_s *vanc_lines,
                                  const uint8_t *teletext_data, int num_data_units,
                                  int field, int vbi_line, int vanc_line)
{
    int max_vanc_words = 3 + 2 + 1 + 1 + 5 + (5 * 45) + 1 + 2 + 1;
    uint16_t *vanc_words = (uint16_t *)av_malloc(max_vanc_words * sizeof(uint16_t));
    if (!vanc_words)
        return AVERROR(ENOMEM);

    int word_count = build_op47_sdp_packet(vanc_words, max_vanc_words,
                                            (uint8_t *)teletext_data, num_data_units,
                                            field, vbi_line);
    int ret = 0;
    if (word_count > 0) {
        ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, vanc_words,
                                 word_count, vanc_line, 0);
        if (ret != 0) {
            av_log(avctx, AV_LOG_WARNING, "Failed to insert teletext VANC line (field %d): %d\n",
                   field, ret);
        }
    }

    av_free(vanc_words);
    return ret;
}

/* Build OP47 VANC packet from teletext data units
 * Supports HD VANC (OP47 SDP) modes per Australian OP-47.
 *
 * Each encoder packet contains multiple data units (rows). We store all rows
 * and cycle through them, sending one row per frame.
 */
static void construct_teletext(AVFormatContext *avctx, struct decklink_ctx *ctx,
                               struct klvanc_line_set_s *vanc_lines)
{
    AVPacket teletext_pkt;
    int ret;

    /* Check for new teletext packets and extract all data units */
    while (ff_decklink_packet_queue_size(&ctx->teletext_queue) > 0) {
        int64_t pts = ff_decklink_packet_queue_peekpts(&ctx->teletext_queue);
        if (pts > ctx->last_pts) {
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

        log_teletext_packet(avctx, &teletext_pkt);

        /* Parse all data units from the packet (each is 46 bytes) */
        int num_units = teletext_pkt.size / 46;
        if (num_units > 5)
            num_units = 5;  /* Limit to 5 rows max */

        if (num_units > 0) {
            ctx->teletext_row_count = num_units;
            ctx->teletext_row_index = 0;  /* Reset to start of new content */
            ctx->has_teletext_data = 1;

            for (int i = 0; i < num_units; i++) {
                uint8_t *du = teletext_pkt.data + (i * 46);
                /* Copy 42-byte payload (bytes 4-45 of each data unit) */
                memcpy(ctx->teletext_rows[i], du + 4, 42);
            }
        }

        av_packet_unref(&teletext_pkt);
    }

    /* Build data unit to send (with header for VANC insertion) */
    static uint8_t data_unit[46];
    const uint8_t *teletext_data;

    if (ctx->has_teletext_data && ctx->teletext_row_count > 0) {
        teletext_data = ctx->teletext_rows[ctx->teletext_row_index];
        /* Advance to next row for next frame */
        ctx->teletext_row_index = (ctx->teletext_row_index + 1) % ctx->teletext_row_count;
    } else {
        teletext_data = teletext_filler_packet;
    }

    /* Build data unit with header */
    data_unit[0] = 0x02;  /* data_unit_id: EBU teletext subtitle */
    data_unit[1] = 0x2C;  /* data_unit_length: 44 */
    data_unit[2] = 0xE4;  /* field/line (placeholder) */
    data_unit[3] = 0xE4;  /* framing code */
    memcpy(data_unit + 4, teletext_data, 42);

    /* Determine field 2 line based on video mode */
    int f2_line;
    switch (ctx->bmd_mode) {
    case bmdModeHD1080i50:
    case bmdModeHD1080i5994:
    case bmdModeHD1080i6000:
        f2_line = AUS_HD_LINE_FIELD2;
        break;
    case bmdModePAL:
        f2_line = AUS_SD_LINE_FIELD2;
        break;
    default:
        f2_line = 0;
        break;
    }

    /* Insert field 1 packet (odd field) */
    if (ctx->teletext_fields != TELETEXT_FIELDS_EVEN) {
        insert_op47_vanc_line(avctx, ctx, vanc_lines, data_unit, 1,
                              1, AUS_SD_LINE_FIELD1, AUS_HD_LINE_FIELD1);
    }

    /* Insert field 2 packet for interlaced modes (even field) */
    if (f2_line > 0 && ctx->teletext_fields != TELETEXT_FIELDS_ODD) {
        insert_op47_vanc_line(avctx, ctx, vanc_lines, data_unit, 1,
                              2, AUS_SD_LINE_FIELD2, f2_line);
    }
}

/* Insert teletext VBI waveform into specified line */
static void insert_teletext_vbi_line(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                      IDeckLinkVideoFrameAncillary *vanc,
                                      int line_num, const uint8_t *teletext_data)
{
    void *line_buf;
    HRESULT result = vanc->GetBufferForVerticalBlankingLine(line_num, &line_buf);
    if (result == S_OK) {
        generate_teletext_vbi_waveform((uint8_t *)line_buf, ctx->bmd_width,
                                        teletext_data, 42, ctx->teletext_vbi_offset);
        av_log(avctx, AV_LOG_INFO,
               "Inserted teletext VBI line %d: MRAG=%02x%02x data=%02x%02x%02x%02x... (buf=%p)\n",
               line_num, teletext_data[0], teletext_data[1],
               teletext_data[2], teletext_data[3], teletext_data[4], teletext_data[5],
               line_buf);
    } else {
        av_log(avctx, AV_LOG_WARNING,
               "Failed to get VBI line %d buffer: HRESULT=0x%08x\n", line_num, (unsigned int)result);
    }
}

/* Insert teletext VBI waveforms directly into SD PAL VBI lines
 * This is called for SD PAL mode where we write raw teletext waveforms
 * to VBI lines 21 (field 1) and 334 (field 2) per Australian OP-47.
 *
 * Each encoder packet contains multiple data units (rows). We store all rows
 * and cycle through them, sending one row per frame. This allows the full
 * teletext page to be transmitted over multiple frames.
 */
static void construct_teletext_vbi_sd(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                       IDeckLinkVideoFrameAncillary *vanc)
{
    AVPacket teletext_pkt;
    int ret;

    /* Only process if we're in SD PAL mode */
    if (ctx->bmd_mode != bmdModePAL)
        return;

    /* Check for new teletext packets and extract all data units */
    while (ff_decklink_packet_queue_size(&ctx->teletext_queue) > 0) {
        int64_t pts = ff_decklink_packet_queue_peekpts(&ctx->teletext_queue);
        if (pts > ctx->last_pts) {
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

        log_teletext_packet(avctx, &teletext_pkt);

        /* Parse all data units from the packet (each is 46 bytes) */
        int num_units = teletext_pkt.size / 46;
        if (num_units > 5)
            num_units = 5;  /* Limit to 5 rows max */

        if (num_units > 0) {
            ctx->teletext_row_count = num_units;
            ctx->teletext_row_index = 0;  /* Reset to start of new content */
            ctx->has_teletext_data = 1;

            av_log(avctx, AV_LOG_DEBUG, "Teletext: storing %d data units from packet\n", num_units);

            for (int i = 0; i < num_units; i++) {
                uint8_t *du = teletext_pkt.data + (i * 46);
                /* Copy 42-byte payload (bytes 4-45 of each data unit) */
                memcpy(ctx->teletext_rows[i], du + 4, 42);
            }
        }

        av_packet_unref(&teletext_pkt);
    }

    /* Determine which data to transmit */
    const uint8_t *data_to_send;
    if (ctx->has_teletext_data && ctx->teletext_row_count > 0) {
        /* Send current row and advance index for next frame */
        data_to_send = ctx->teletext_rows[ctx->teletext_row_index];

        av_log(avctx, AV_LOG_DEBUG, "Teletext: sending row %d of %d\n",
               ctx->teletext_row_index, ctx->teletext_row_count);
        av_log(avctx, AV_LOG_INFO,
               "  MRAG + data bytes 0-19: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               data_to_send[0], data_to_send[1], data_to_send[2], data_to_send[3],
               data_to_send[4], data_to_send[5], data_to_send[6], data_to_send[7],
               data_to_send[8], data_to_send[9], data_to_send[10], data_to_send[11],
               data_to_send[12], data_to_send[13], data_to_send[14], data_to_send[15],
               data_to_send[16], data_to_send[17], data_to_send[18], data_to_send[19]);

        /* Advance to next row for next frame, wrapping around */
        ctx->teletext_row_index = (ctx->teletext_row_index + 1) % ctx->teletext_row_count;
    } else {
        /* No data yet - send filler */
        data_to_send = teletext_filler_packet;
        av_log(avctx, AV_LOG_DEBUG, "Teletext: sending filler (no data)\n");
    }

    /* Insert on VBI line 21 (field 1 / odd field) */
    if (ctx->teletext_fields != TELETEXT_FIELDS_EVEN) {
        insert_teletext_vbi_line(avctx, ctx, vanc, AUS_SD_LINE_FIELD1, data_to_send);
    }

    /* Insert on VBI line 334 (field 2 / even field) */
    if (ctx->teletext_fields != TELETEXT_FIELDS_ODD) {
        insert_teletext_vbi_line(avctx, ctx, vanc, AUS_SD_LINE_FIELD2, data_to_send);
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

    /* Process any pending teletext packets for HD modes (OP47 VANC)
     * SD PAL VBI teletext is handled separately after VANC creation */
    if (ctx->teletext_st && ctx->bmd_mode != bmdModePAL)
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
    /* Always use 10-bit YUV to match v210 video format */
    int result = ctx->dlo->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create ancillary data\n");
        ret = AVERROR(EIO);
        goto done;
    }

    /* For SD PAL mode, insert teletext as raw VBI waveforms on lines 21/334
     * This must be done after ancillary data creation so we can access VBI line buffers */
    if (ctx->teletext_st && ctx->bmd_mode == bmdModePAL) {
        /* Diagnostic: test which VBI lines the SDK accepts */
        static int diag_done = 0;
        if (!diag_done) {
            diag_done = 1;
            av_log(avctx, AV_LOG_INFO, "Testing available VBI lines for SD PAL:\n");
            for (int test_line = 1; test_line <= 30; test_line++) {
                void *test_buf;
                HRESULT hr = vanc->GetBufferForVerticalBlankingLine(test_line, &test_buf);
                av_log(avctx, AV_LOG_INFO, "  Line %d: %s (0x%08x)\n",
                       test_line, (hr == S_OK) ? "OK" : "FAIL", (unsigned int)hr);
            }
            for (int test_line = 310; test_line <= 345; test_line++) {
                void *test_buf;
                HRESULT hr = vanc->GetBufferForVerticalBlankingLine(test_line, &test_buf);
                av_log(avctx, AV_LOG_INFO, "  Line %d: %s (0x%08x)\n",
                       test_line, (hr == S_OK) ? "OK" : "FAIL", (unsigned int)hr);
            }
        }
        construct_teletext_vbi_sd(avctx, ctx, vanc);
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output (HD modes only) */
    for (i = 0; i < vanc_lines.num_lines && ctx->bmd_mode != bmdModePAL; i++) {
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

/* Extract source filename from packet metadata. If seek_us is non-NULL it is
 * set to the input's whole-file seek offset (e.g. -ss) in AV_TIME_BASE units,
 * or AV_NOPTS_VALUE if the input was not seeked. */
static char *get_packet_source_filename(AVPacket *pkt, int64_t *seek_us)
{
    size_t size;
    const uint8_t *side_metadata;
    AVDictionary *metadata = NULL;
    AVDictionaryEntry *entry = NULL;
    char *filename = NULL;

    if (seek_us)
        *seek_us = AV_NOPTS_VALUE;

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

    if (seek_us) {
        entry = av_dict_get(metadata, "lavf.source_seek_us", NULL, 0);
        if (entry)
            *seek_us = strtoll(entry->value, NULL, 10);
    }

    av_dict_free(&metadata);
    return filename;
}

/* Format a seek suffix for playout logs, e.g. " -ss=630.000s", or an empty
 * string if the input was not seeked. */
static void format_seek_suffix(char *buf, size_t buf_size, int64_t seek_us)
{
    if (seek_us == AV_NOPTS_VALUE || seek_us == 0)
        buf[0] = '\0';
    else
        snprintf(buf, buf_size, " (-ss=%.3fs)", (double)seek_us / AV_TIME_BASE);
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

    /* Attach the source filename to the frame so the completion callback can
     * log when the card actually puts a new segment on-air. Also emit a
     * debug-level SCHEDULED boundary here (fires when the frame is enqueued,
     * ahead of playout) for lead-time diagnostics. Ownership of the string
     * transfers to the frame and is freed in decklink_frame::Release(). */
    int64_t source_seek_us = AV_NOPTS_VALUE;
    char *source_filename = get_packet_source_filename(pkt, &source_seek_us);
    if (source_filename) {
        if (!ctx->last_scheduled_source_filename ||
            strcmp(source_filename, ctx->last_scheduled_source_filename) != 0) {
            char seek_suffix[64];
            format_seek_suffix(seek_suffix, sizeof(seek_suffix), source_seek_us);
            av_log(avctx, AV_LOG_DEBUG,
                   "decklink: SCHEDULED new segment: %s (pts=%"PRId64")%s\n",
                   source_filename, pkt->pts, seek_suffix);
            av_free(ctx->last_scheduled_source_filename);
            ctx->last_scheduled_source_filename = av_strdup(source_filename);
        }
        frame->_source_filename = source_filename;
        frame->_pts = pkt->pts;
        frame->_source_seek_us = source_seek_us;
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

    /* Preroll video frames, then end audio preroll and start playback.
     *
     * This transition runs on the video thread WITHOUT a lock and may execute
     * concurrently with the audio thread's ScheduleAudioSamples().  That is
     * deliberate: in pre_render the audio thread floods the DeckLink audio
     * buffer during preroll, and ScheduleAudioSamples() blocks once that buffer
     * is full - it only drains after StartScheduledPlayback() runs here.  Any
     * lock held across both would deadlock (audio waits for playback to start,
     * playback waits for the lock the blocked audio thread holds).  Instead the
     * audio thread tolerates the brief E_ACCESSDENIED that ScheduleAudioSamples
     * returns while this transition is in flight (see decklink_schedule_audio_
     * packet). */
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
        /* Mark the output as on-air so fftools can report speed relative to
         * playout start, excluding the pre_render buffering phase. Set once. */
        av_dict_set(&avctx->metadata, "ffmpeg.onair", "1", 0);
    }

    return 0;
}

/* Entry point for video packets - queues to async buffer or schedules directly */
static int decklink_write_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    /* If shared memory server mode, ignore input - shm reader provides frames */
    if (ctx->shm_buffer && ctx->shm_is_server) {
        return 0;
    }

    /* If shared memory client mode, write to shm buffer */
    if (ctx->shm_buffer && !ctx->shm_is_server) {
        int ret = decklink_shm_write_packet(avctx, pkt, DECKLINK_SHM_FRAME_VIDEO);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write video to shared memory: %d\n", ret);
            return ret;
        }
        return 0;
    }

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

    /* ScheduleAudioSamples() can briefly return E_ACCESSDENIED (0x80000009)
     * while the video thread takes the audio stream out of preroll: EndAudio-
     * Preroll()/StartScheduledPlayback() run concurrently and without a lock
     * (see decklink_schedule_video_packet for why a lock would deadlock). The
     * transition completes in well under a millisecond, so retry the same
     * packet for a bounded window instead of aborting the whole output. The
     * ceiling is configurable via the audio_schedule_retry option (seconds);
     * 0 disables retry. It is bounded so a genuinely wedged card fails loudly
     * rather than hanging and holding the device. */
    HRESULT audio_hr;
    int access_denied_retries = 0;
    int max_retries = (int)(cctx->audio_schedule_retry * 1000);  /* seconds -> 1ms steps */
    for (;;) {
        audio_hr = ctx->dlo->ScheduleAudioSamples(outbuf, sample_count, pkt->pts,
                                                  bmdAudioSampleRate48kHz, NULL);
        if (audio_hr != E_ACCESSDENIED || access_denied_retries >= max_retries)
            break;
        if (access_denied_retries == 0)
            av_log(avctx, AV_LOG_DEBUG, "Audio schedule denied during preroll->playback "
                   "transition; retrying up to %.3gs (pts=%"PRId64").\n",
                   cctx->audio_schedule_retry, pkt->pts);
        access_denied_retries++;
        usleep(1000);
    }
    if (audio_hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule audio samples."
               " error %08x (pts=%"PRId64", samples=%d, playback_started=%d, retries=%d).\n",
               (uint32_t) audio_hr, pkt->pts, sample_count, ctx->playback_started, access_denied_retries);
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

    /* If shared memory server mode, ignore input - shm reader provides frames */
    if (ctx->shm_buffer && ctx->shm_is_server) {
        return 0;
    }

    /* If shared memory client mode, write to shm buffer */
    if (ctx->shm_buffer && !ctx->shm_is_server) {
        int ret = decklink_shm_write_packet(avctx, pkt, DECKLINK_SHM_FRAME_AUDIO);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write audio to shared memory: %d\n", ret);
            return ret;
        }
        return 0;
    }

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
    case AV_CODEC_ID_DVB_TELETEXT: {
        /* Queue teletext packets for VANC insertion.
         *
         * The drain side (construct_teletext / construct_teletext_vbi_sd) gates
         * each queued packet against ctx->last_pts, which tracks the DeckLink
         * video frame clock in the mode timebase (ctx->bmd_tb_num/bmd_tb_den,
         * e.g. 1/25). Subtitle packets, however, reach us in the subtitle stream
         * timebase (typically 1/1000000). Without rescaling, a microsecond-scale
         * PTS always compares as far in the future ("pts > last_pts"), so the
         * gate never releases a packet: no teletext is ever inserted and the
         * queue simply fills until it overruns. Rescale the PTS into the video
         * frame timebase so the comparison is meaningful.
         *
         * pkt->time_base is set by the muxing layer (fftools/ffmpeg_mux.c) to the
         * stream timebase the PTS is expressed in; fall back to st->time_base if
         * it is not populated.
         *
         * The target is the video frame clock that ctx->last_pts is tracked in.
         * Once the DeckLink device is open that is bmd_tb_num/bmd_tb_den, but in
         * pre_render mode teletext packets are queued during the fill, before the
         * device is initialized (bmd_tb_num/den are still 0). Fall back to the
         * deferred video timebase captured at write_header, which is the same
         * video stream timebase last_pts is measured in. Without this the rescale
         * is skipped during pre_render and the microsecond PTS never drains. */
        AVRational frame_tb = (ctx->bmd_tb_num && ctx->bmd_tb_den)
                                  ? av_make_q(ctx->bmd_tb_num, ctx->bmd_tb_den)
                                  : ctx->deferred_video_tb;
        AVRational src_tb   = (pkt->time_base.num && pkt->time_base.den)
                                  ? pkt->time_base : st->time_base;
        if (frame_tb.num && frame_tb.den && src_tb.num && src_tb.den)
            av_packet_rescale_ts(pkt, src_tb, frame_tb);
        if (ff_decklink_packet_queue_put(&ctx->teletext_queue, pkt) < 0) {
            av_log(avctx, AV_LOG_WARNING, "Failed to queue teletext packet\n");
        }
        break;
    }
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
    ctx->teletext_fields = cctx->teletext_fields;
    ctx->teletext_vbi_offset = cctx->teletext_vbi_offset;
    ctx->first_pts    = AV_NOPTS_VALUE;
    ctx->socket_fd    = -1;
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

    /* Shared memory client mode - skip Decklink init, just attach to shm */
    if (cctx->shm_name && cctx->shm_client) {
        ctx->avctx = avctx;
        ret = decklink_init_shm_client(avctx, ctx, cctx->shm_name);
        if (ret < 0) {
            av_freep(&cctx->ctx);
            return ret;
        }
        av_log(avctx, AV_LOG_INFO, "Running in shared memory client mode\n");
        return 0;
    }

    /* Pre-render mode - buffer frames before initializing DeckLink */
    if (cctx->pre_render) {
        ctx->avctx = avctx;
        ctx->pre_render_mode = 1;
        ctx->pre_render_device_ready = 0;

        /* Parse time trigger if specified */
        if (cctx->pre_render_until) {
            ctx->pre_render_start_time = parse_pre_render_time(cctx->pre_render_until);
            if (ctx->pre_render_start_time > 0) {
                time_t start_sec = ctx->pre_render_start_time / 1000000LL;
                struct tm *tm_start = localtime(&start_sec);
                char time_buf[64];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_start);
                av_log(avctx, AV_LOG_INFO, "Pre-render: will start playback at %s\n", time_buf);
            } else {
                av_log(avctx, AV_LOG_ERROR, "Pre-render: invalid time format '%s'\n", cctx->pre_render_until);
                av_freep(&cctx->ctx);
                return AVERROR(EINVAL);
            }
        }

        /* Set frame trigger if specified */
        ctx->pre_render_target_frames = cctx->pre_render_frames;
        if (ctx->pre_render_target_frames > 0) {
            av_log(avctx, AV_LOG_INFO, "Pre-render: will start playback after %d frames buffered\n",
                   ctx->pre_render_target_frames);
        }

        /* Extract video format from streams */
        for (n = 0; n < avctx->nb_streams; n++) {
            AVStream *st = avctx->streams[n];
            AVCodecParameters *c = st->codecpar;
            if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
                ctx->deferred_video_width = c->width;
                ctx->deferred_video_height = c->height;
                ctx->deferred_video_tb = st->time_base;
                ctx->deferred_video_field_order = c->field_order;
                ctx->video_st = st;
                ctx->video = 1;

                /* Validate codec */
                if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
                    if (c->format != AV_PIX_FMT_UYVY422) {
                        av_log(avctx, AV_LOG_ERROR, "Pre-render: unsupported pixel format\n");
                        av_freep(&cctx->ctx);
                        return AVERROR(EINVAL);
                    }
                    ctx->raw_format = bmdFormat8BitYUV;
                } else if (c->codec_id == AV_CODEC_ID_V210) {
                    ctx->raw_format = bmdFormat10BitYUV;
                } else {
                    av_log(avctx, AV_LOG_ERROR, "Pre-render: unsupported video codec\n");
                    av_freep(&cctx->ctx);
                    return AVERROR(EINVAL);
                }

                /* Set pts info for proper timing */
                avpriv_set_pts_info(st, 64, st->time_base.num, st->time_base.den);
                break;
            }
        }

        /* Extract audio format from streams */
        for (n = 0; n < avctx->nb_streams; n++) {
            AVStream *st = avctx->streams[n];
            AVCodecParameters *c = st->codecpar;
            if (c->codec_type == AVMEDIA_TYPE_AUDIO) {
                ctx->audio_st = st;
                ctx->audio = 1;
                if (c->codec_id == AV_CODEC_ID_AC3) {
                    ctx->channels = 2;
                } else {
                    ctx->channels = c->ch_layout.nb_channels;
                }
                ctx->audio_depth = c->bits_per_raw_sample == 32 ? 32 : 16;
                avpriv_set_pts_info(st, 64, 1, 48000);
                break;
            }
        }

        if (!ctx->video) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: no video stream found\n");
            av_freep(&cctx->ctx);
            return AVERROR(EINVAL);
        }

        /* Pre-render requires output_buffer_size */
        if (cctx->output_buffer_size <= 0) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render mode requires output_buffer_size to be set\n");
            av_freep(&cctx->ctx);
            return AVERROR(EINVAL);
        }

        /* Initialize output buffers */
        int64_t total_ram = get_total_system_ram();
        int64_t max_buffer_size = cctx->output_buffer_size;
        if (total_ram > 0) {
            int64_t ram_limit = (int64_t)(total_ram * 0.8);
            if (cctx->output_buffer_size > ram_limit)
                max_buffer_size = ram_limit;
        }
        int64_t audio_buffer_size = FFMAX(max_buffer_size / 20, 100 * 1024 * 1024);
        int64_t video_buffer_size = FFMAX(max_buffer_size - audio_buffer_size, 100 * 1024 * 1024);

        ctx->output_thread_stop = 0;
        ctx->output_thread_error = 0;
        ff_decklink_packet_queue_init(avctx, &ctx->output_video_queue, video_buffer_size);
        ff_decklink_packet_queue_init(avctx, &ctx->output_audio_queue, audio_buffer_size);

        /* Create output threads (they will wait for device ready) */
        ret = pthread_create(&ctx->output_video_thread, NULL, decklink_video_output_thread, ctx);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: failed to create video output thread\n");
            ff_decklink_packet_queue_end(&ctx->output_video_queue);
            ff_decklink_packet_queue_end(&ctx->output_audio_queue);
            av_freep(&cctx->ctx);
            return AVERROR(ret);
        }

        ret = pthread_create(&ctx->output_audio_thread, NULL, decklink_audio_output_thread, ctx);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: failed to create audio output thread\n");
            ctx->output_thread_stop = 1;
            pthread_join(ctx->output_video_thread, NULL);
            ff_decklink_packet_queue_end(&ctx->output_video_queue);
            ff_decklink_packet_queue_end(&ctx->output_audio_queue);
            av_freep(&cctx->ctx);
            return AVERROR(ret);
        }
        ctx->output_thread_started = 1;

        /* Start trigger thread to monitor conditions and initialize device */
        ctx->pre_render_trigger_stop = 0;
        ret = pthread_create(&ctx->pre_render_trigger_thread, NULL, decklink_pre_render_trigger_thread, ctx);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Pre-render: failed to create trigger thread\n");
            ctx->output_thread_stop = 1;
            pthread_join(ctx->output_video_thread, NULL);
            pthread_join(ctx->output_audio_thread, NULL);
            ff_decklink_packet_queue_end(&ctx->output_video_queue);
            ff_decklink_packet_queue_end(&ctx->output_audio_queue);
            av_freep(&cctx->ctx);
            return AVERROR(ret);
        }
        ctx->pre_render_trigger_started = 1;

        av_log(avctx, AV_LOG_INFO, "Pre-render mode enabled: buffering frames until trigger\n");
        return 0;
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
         * so allocate 95% to video and 5% to audio. Minimum 100MB each to ensure
         * adequate buffering even with small requested sizes.
         */
        int64_t audio_buffer_size = FFMAX(max_buffer_size / 20, 100 * 1024 * 1024);
        int64_t video_buffer_size = FFMAX(max_buffer_size - audio_buffer_size, 100 * 1024 * 1024);

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

    /* Initialize socket server if requested */
    if (cctx->socket_path && cctx->socket_listen) {
        if (!ctx->output_thread_started) {
            av_log(avctx, AV_LOG_ERROR, "Socket server requires output_buffer_size to be set\n");
            ret = AVERROR(EINVAL);
            goto error;
        }
        ret = decklink_init_socket_server(avctx, ctx, cctx->socket_path);
        if (ret < 0) {
            goto error;
        }
    }

    /* Initialize shared memory if requested */
    if (cctx->shm_name) {
        /* Calculate frame size for v210: 128-byte aligned rows */
        uint32_t frame_size;
        if (ctx->video) {
            int blocks_per_row = (ctx->bmd_width + 5) / 6;
            int row_bytes = ((blocks_per_row * 16 + 127) / 128) * 128;
            frame_size = row_bytes * ctx->bmd_height;
            /* Add some headroom for audio interleaving */
            frame_size = FFMAX(frame_size, 16 * 1024 * 1024);  /* 16MB minimum */
        } else {
            frame_size = 16 * 1024 * 1024;
        }

        if (cctx->shm_server) {
            if (!ctx->output_thread_started) {
                av_log(avctx, AV_LOG_ERROR, "Shared memory server requires output_buffer_size to be set\n");
                ret = AVERROR(EINVAL);
                goto error;
            }
            ret = decklink_init_shm_server(avctx, ctx, cctx->shm_name,
                                           cctx->shm_max_frames, frame_size);
            if (ret < 0) {
                goto error;
            }
        } else if (cctx->shm_client) {
            ret = decklink_init_shm_client(avctx, ctx, cctx->shm_name);
            if (ret < 0) {
                goto error;
            }
        }
    }

    return 0;

error:
    decklink_cleanup_shm(ctx);
    decklink_cleanup_socket_server(ctx);
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
