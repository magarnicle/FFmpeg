/*
 * Blackmagic DeckLink shared memory buffer
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

#ifndef AVDEVICE_DECKLINK_SHM_H
#define AVDEVICE_DECKLINK_SHM_H

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
 *   Encoder 2 ──┼──▶ [shared memory ring buffer] ──▶ playout FFmpeg ──▶ DeckLink
 *   Encoder 3 ─┘
 *
 * The server (playout instance) creates the shared memory segment.
 * Clients (encoder instances) attach and write frames.
 * The server reads frames and outputs to DeckLink.
 */

#define DECKLINK_SHM_MAGIC      0x444B534D  /* "DKSM" */
#define DECKLINK_SHM_VERSION    1
#define DECKLINK_SHM_MAX_FRAMES 120         /* ~4.8 seconds at 25fps */

/* Frame types */
#define DECKLINK_SHM_FRAME_VIDEO    0
#define DECKLINK_SHM_FRAME_AUDIO    1
#define DECKLINK_SHM_FRAME_SUBTITLE 2

/* Frame header in shared memory */
typedef struct DecklinkShmFrameHeader {
    uint32_t magic;             /* DECKLINK_SHM_MAGIC for validation */
    uint32_t type;              /* DECKLINK_SHM_FRAME_VIDEO/AUDIO/SUBTITLE */
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
} DecklinkShmFrameHeader;

/* Shared memory buffer header */
typedef struct DecklinkShmBuffer {
    uint32_t magic;             /* DECKLINK_SHM_MAGIC */
    uint32_t version;           /* DECKLINK_SHM_VERSION */
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
    /* Each slot is: DecklinkShmFrameHeader + frame_data_size bytes */
} DecklinkShmBuffer;

/* Calculate total shared memory size needed */
static inline size_t decklink_shm_calc_size(uint32_t max_frames, uint32_t frame_data_size)
{
    return sizeof(DecklinkShmBuffer) +
           max_frames * (sizeof(DecklinkShmFrameHeader) + frame_data_size);
}

/* Get pointer to frame slot */
static inline void *decklink_shm_get_slot(DecklinkShmBuffer *shm, uint32_t index)
{
    uint8_t *base = (uint8_t *)shm + sizeof(DecklinkShmBuffer);
    size_t slot_size = sizeof(DecklinkShmFrameHeader) + shm->frame_data_size;
    return base + (index % shm->max_frames) * slot_size;
}

/* Get frame header from slot */
static inline DecklinkShmFrameHeader *decklink_shm_get_header(void *slot)
{
    return (DecklinkShmFrameHeader *)slot;
}

/* Get frame data from slot */
static inline void *decklink_shm_get_data(void *slot)
{
    return (uint8_t *)slot + sizeof(DecklinkShmFrameHeader);
}

/*
 * Server functions (playout instance)
 */

/* Create and initialize shared memory buffer */
int decklink_shm_server_create(const char *name, uint32_t max_frames,
                                uint32_t frame_data_size, DecklinkShmBuffer **out_shm);

/* Destroy shared memory buffer */
void decklink_shm_server_destroy(const char *name, DecklinkShmBuffer *shm);

/* Read a frame from the buffer (blocks if empty) */
int decklink_shm_server_read(DecklinkShmBuffer *shm, DecklinkShmFrameHeader *header,
                              void *data, uint32_t max_size, int timeout_ms);

/* Signal shutdown to clients */
void decklink_shm_server_shutdown(DecklinkShmBuffer *shm);

/*
 * Client functions (encoder instances)
 */

/* Attach to existing shared memory buffer */
int decklink_shm_client_attach(const char *name, DecklinkShmBuffer **out_shm);

/* Detach from shared memory buffer */
void decklink_shm_client_detach(DecklinkShmBuffer *shm);

/* Write a frame to the buffer (blocks if full) */
int decklink_shm_client_write(DecklinkShmBuffer *shm, const DecklinkShmFrameHeader *header,
                               const void *data, int timeout_ms);

/* Check if server is still active */
int decklink_shm_client_server_active(DecklinkShmBuffer *shm);

#ifdef __cplusplus
}
#endif

#endif /* AVDEVICE_DECKLINK_SHM_H */
