/*
 * Frame feed for output devices: a shared-memory ring and a
 * Unix-socket server, for getting frames from other processes
 * Copyright (c) 2024
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

#ifndef AVDEVICE_OUTDEV_FEED_H
#define AVDEVICE_OUTDEV_FEED_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared memory ring buffer for cross-process frame transfer.
 *
 * Architecture:
 *   Encoder 1 ─┐
 *   Encoder 2 ──┼──▶ [shared memory ring buffer] ──▶ playout FFmpeg ──▶ card
 *   Encoder 3 ─┘
 *
 * The server (playout instance) creates the shared memory segment.
 * Clients (encoder instances) attach and write frames.
 * The server reads frames and plays them out. Neither half knows which card
 * is on the end, so a DeckLink server and an AJA one take the same feed.
 */

#define FF_OUTDEV_SHM_MAGIC      0x444B534D  /* "DKSM" */
#define FF_OUTDEV_SHM_VERSION    1
#define FF_OUTDEV_SHM_MAX_FRAMES 120         /* ~4.8 seconds at 25fps */

/* Frame types */
#define FF_OUTDEV_FRAME_VIDEO    0
#define FF_OUTDEV_FRAME_AUDIO    1
#define FF_OUTDEV_FRAME_SUBTITLE 2

/* Frame header in shared memory */
typedef struct FFOutdevFrameHeader {
    uint32_t magic;             /* FF_OUTDEV_SHM_MAGIC for validation */
    uint32_t type;              /* FF_OUTDEV_FRAME_VIDEO/AUDIO/SUBTITLE */
    uint32_t size;              /* Size of frame data following header */
    uint32_t flags;             /* Reserved for future use */
    int64_t  pts;               /* Presentation timestamp */
    int64_t  dts;               /* Decode timestamp */
    int64_t  duration;          /* Frame duration */
    uint32_t stream_index;      /* Stream index */
    uint32_t width;             /* Video width (video only) */
    uint32_t height;            /* Video height (video only) */
    uint32_t sample_rate;       /* Audio sample rate (audio only) */
    uint32_t channels;          /* Audio channels (audio only) */
    uint32_t _reserved[4];      /* Padding for future use */
} FFOutdevFrameHeader;

/* Shared memory buffer header */
typedef struct FFOutdevShmBuffer {
    uint32_t magic;             /* FF_OUTDEV_SHM_MAGIC */
    uint32_t version;           /* FF_OUTDEV_SHM_VERSION */
    uint32_t total_size;        /* Total shared memory size */
    uint32_t max_frames;        /* Maximum frames in ring buffer */
    uint32_t frame_data_size;   /* Size allocated per frame slot */

    /* Ring buffer indices */
    volatile uint32_t write_idx;    /* Next slot to write */
    volatile uint32_t read_idx;     /* Next slot to read */
    volatile uint32_t frame_count;  /* Frames currently in buffer */

    /* Synchronization (PTHREAD_PROCESS_SHARED) */
    pthread_mutex_t mutex;
    pthread_cond_t  cond_not_full;
    pthread_cond_t  cond_not_empty;

    /* Status */
    volatile int server_active;     /* Server is running */
    volatile int shutdown;          /* Shutdown requested */
    volatile int error;             /* Error code if any */

    /* Video/audio format (set by server, clients must match) */
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_fps_num;
    uint32_t video_fps_den;
    uint32_t audio_sample_rate;
    uint32_t audio_channels;

    /* Statistics */
    volatile uint64_t frames_written;
    volatile uint64_t frames_read;
    volatile uint64_t frames_dropped;

    uint32_t _reserved[16];     /* Future use */

    /* Frame slots start here (array of max_frames slots) */
    /* Each slot is: FFOutdevFrameHeader + frame_data_size bytes */
} FFOutdevShmBuffer;

/* Calculate total shared memory size needed */
static inline size_t ff_outdev_shm_calc_size(uint32_t max_frames, uint32_t frame_data_size)
{
    return sizeof(FFOutdevShmBuffer) +
           max_frames * (sizeof(FFOutdevFrameHeader) + frame_data_size);
}

/* Get pointer to frame slot */
static inline void *ff_outdev_shm_get_slot(FFOutdevShmBuffer *shm, uint32_t index)
{
    uint8_t *base = (uint8_t *)shm + sizeof(FFOutdevShmBuffer);
    size_t slot_size = sizeof(FFOutdevFrameHeader) + shm->frame_data_size;
    return base + (index % shm->max_frames) * slot_size;
}

/* Get frame header from slot */
static inline FFOutdevFrameHeader *ff_outdev_shm_get_header(void *slot)
{
    return (FFOutdevFrameHeader *)slot;
}

/* Get frame data from slot */
static inline void *ff_outdev_shm_get_data(void *slot)
{
    return (uint8_t *)slot + sizeof(FFOutdevFrameHeader);
}

/*
 * Server functions (playout instance)
 */

/* Create and initialize shared memory buffer */
int ff_outdev_shm_server_create(const char *name, uint32_t max_frames,
                                uint32_t frame_data_size, FFOutdevShmBuffer **out_shm);

/* Destroy shared memory buffer */
void ff_outdev_shm_server_destroy(const char *name, FFOutdevShmBuffer *shm);

/* Read a frame from the buffer (blocks if empty) */
int ff_outdev_shm_server_read(FFOutdevShmBuffer *shm, FFOutdevFrameHeader *header,
                              void *data, uint32_t max_size, int timeout_ms);

/* Signal shutdown to clients */
void ff_outdev_shm_server_shutdown(FFOutdevShmBuffer *shm);

/*
 * Client functions (encoder instances)
 */

/* Attach to existing shared memory buffer */
int ff_outdev_shm_client_attach(const char *name, FFOutdevShmBuffer **out_shm);

/* Detach from shared memory buffer */
void ff_outdev_shm_client_detach(FFOutdevShmBuffer *shm);

/* Write a frame to the buffer (blocks if full) */
int ff_outdev_shm_client_write(FFOutdevShmBuffer *shm, const FFOutdevFrameHeader *header,
                               const void *data, int timeout_ms);

/* Check if server is still active */
int ff_outdev_shm_client_server_active(FFOutdevShmBuffer *shm);

#ifdef __cplusplus
}
#endif


/* ---------------------------------------------------------------------------
 * Unix socket frame server
 *
 * The other way into a playout process, for frames that come from something
 * that is not another ffmpeg. One connection at a time, frames one after
 * another, each a packed header then its data:
 *
 *   uint32  magic, FF_OUTDEV_SOCKET_MAGIC, network byte order
 *   uint8   stream type, FF_OUTDEV_FRAME_VIDEO or FF_OUTDEV_FRAME_AUDIO
 *   uint32  data size, network byte order
 *   int64   pts, network byte order
 *   uint32  width, video only, network byte order
 *   uint32  height, video only, network byte order
 *   N bytes frame data
 *
 * The wire format is the one the DeckLink output has always spoken, magic
 * included, so anything already feeding that keeps working and can feed either
 * card.
 * ------------------------------------------------------------------------- */

#define FF_OUTDEV_SOCKET_MAGIC 0x444B4C4B

typedef struct FFOutdevSocketHeader {
    uint32_t magic;
    uint8_t  stream_type;
    uint32_t data_size;
    int64_t  pts;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) FFOutdevSocketHeader;

/* Called for each frame that arrives, on the server's own thread. Returning a
 * negative value drops the connection. */
typedef int (*FFOutdevFrameCb)(void *opaque, int type, const uint8_t *data,
                               int size, int64_t pts);

typedef struct FFOutdevSocketServer {
    int              fd;
    char            *path;
    pthread_t        thread;
    int              thread_started;
    volatile int     stop;
    FFOutdevFrameCb  cb;
    void            *opaque;
    void            *logctx;
} FFOutdevSocketServer;

/* Bind, listen and start the accept thread. */
int  ff_outdev_socket_server_start(FFOutdevSocketServer *s, const char *path,
                                   FFOutdevFrameCb cb, void *opaque,
                                   void *logctx);

/* Stop the thread, close the socket and remove the path. Safe on a server that
 * never started. */
void ff_outdev_socket_server_stop(FFOutdevSocketServer *s);

#endif /* AVDEVICE_OUTDEV_FEED_H */
