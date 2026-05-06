/*
 * Blackmagic DeckLink shared memory buffer implementation
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

#include "decklink_shm.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "libavutil/log.h"
#include "libavutil/error.h"

/* Helper to get absolute time for timed waits */
static void get_abs_timeout(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

int decklink_shm_server_create(const char *name, uint32_t max_frames,
                                uint32_t frame_data_size, DecklinkShmBuffer **out_shm)
{
    int fd;
    size_t total_size;
    DecklinkShmBuffer *shm;
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    int ret;

    if (!name || !out_shm || max_frames == 0 || frame_data_size == 0)
        return AVERROR(EINVAL);

    total_size = decklink_shm_calc_size(max_frames, frame_data_size);

    /* Create shared memory object */
    fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            /* Try to unlink and recreate */
            shm_unlink(name);
            fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
        }
        if (fd < 0) {
            av_log(NULL, AV_LOG_ERROR, "shm_open(%s) failed: %s\n", name, strerror(errno));
            return AVERROR(errno);
        }
    }

    /* Set size */
    if (ftruncate(fd, total_size) < 0) {
        av_log(NULL, AV_LOG_ERROR, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        shm_unlink(name);
        return AVERROR(errno);
    }

    /* Map into memory */
    shm = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);  /* fd no longer needed after mmap */

    if (shm == MAP_FAILED) {
        av_log(NULL, AV_LOG_ERROR, "mmap failed: %s\n", strerror(errno));
        shm_unlink(name);
        return AVERROR(errno);
    }

    /* Initialize buffer header */
    memset(shm, 0, total_size);
    shm->magic = DECKLINK_SHM_MAGIC;
    shm->version = DECKLINK_SHM_VERSION;
    shm->total_size = total_size;
    shm->max_frames = max_frames;
    shm->frame_data_size = frame_data_size;
    shm->write_idx = 0;
    shm->read_idx = 0;
    shm->frame_count = 0;
    shm->server_active = 1;
    shm->shutdown = 0;
    shm->error = 0;

    /* Initialize process-shared mutex */
    ret = pthread_mutexattr_init(&mutex_attr);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_mutexattr_init failed: %s\n", strerror(ret));
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    ret = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_mutexattr_setpshared failed: %s\n", strerror(ret));
        pthread_mutexattr_destroy(&mutex_attr);
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    ret = pthread_mutex_init(&shm->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_mutex_init failed: %s\n", strerror(ret));
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    /* Initialize process-shared condition variables */
    ret = pthread_condattr_init(&cond_attr);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_condattr_init failed: %s\n", strerror(ret));
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    ret = pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_condattr_setpshared failed: %s\n", strerror(ret));
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    ret = pthread_cond_init(&shm->cond_not_full, &cond_attr);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_cond_init (not_full) failed: %s\n", strerror(ret));
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    ret = pthread_cond_init(&shm->cond_not_empty, &cond_attr);
    pthread_condattr_destroy(&cond_attr);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "pthread_cond_init (not_empty) failed: %s\n", strerror(ret));
        pthread_cond_destroy(&shm->cond_not_full);
        pthread_mutex_destroy(&shm->mutex);
        munmap(shm, total_size);
        shm_unlink(name);
        return AVERROR(ret);
    }

    av_log(NULL, AV_LOG_INFO, "Created shared memory buffer '%s': %zu bytes, %u frames, %u bytes/frame\n",
           name, total_size, max_frames, frame_data_size);

    *out_shm = shm;
    return 0;
}

void decklink_shm_server_destroy(const char *name, DecklinkShmBuffer *shm)
{
    if (!shm)
        return;

    /* Signal shutdown and wake all waiters */
    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = 1;
    shm->server_active = 0;
    pthread_cond_broadcast(&shm->cond_not_full);
    pthread_cond_broadcast(&shm->cond_not_empty);
    pthread_mutex_unlock(&shm->mutex);

    /* Give clients a moment to notice shutdown */
    usleep(100000);

    /* Cleanup synchronization primitives */
    pthread_cond_destroy(&shm->cond_not_empty);
    pthread_cond_destroy(&shm->cond_not_full);
    pthread_mutex_destroy(&shm->mutex);

    /* Unmap and unlink */
    munmap(shm, shm->total_size);
    if (name)
        shm_unlink(name);

    av_log(NULL, AV_LOG_INFO, "Destroyed shared memory buffer\n");
}

int decklink_shm_server_read(DecklinkShmBuffer *shm, DecklinkShmFrameHeader *header,
                              void *data, uint32_t max_size, int timeout_ms)
{
    struct timespec ts;
    void *slot;
    DecklinkShmFrameHeader *slot_header;
    void *slot_data;
    int ret = 0;

    if (!shm || !header || !data)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&shm->mutex);

    /* Wait for frame to be available */
    while (shm->frame_count == 0 && !shm->shutdown) {
        if (timeout_ms > 0) {
            get_abs_timeout(&ts, timeout_ms);
            ret = pthread_cond_timedwait(&shm->cond_not_empty, &shm->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&shm->mutex);
                return AVERROR(EAGAIN);
            }
        } else if (timeout_ms == 0) {
            pthread_mutex_unlock(&shm->mutex);
            return AVERROR(EAGAIN);
        } else {
            pthread_cond_wait(&shm->cond_not_empty, &shm->mutex);
        }
    }

    if (shm->shutdown) {
        pthread_mutex_unlock(&shm->mutex);
        return AVERROR_EOF;
    }

    /* Get frame from slot */
    slot = decklink_shm_get_slot(shm, shm->read_idx);
    slot_header = decklink_shm_get_header(slot);
    slot_data = decklink_shm_get_data(slot);

    /* Validate */
    if (slot_header->magic != DECKLINK_SHM_MAGIC) {
        pthread_mutex_unlock(&shm->mutex);
        av_log(NULL, AV_LOG_ERROR, "Invalid frame magic at slot %u\n", shm->read_idx);
        return AVERROR(EIO);
    }

    if (slot_header->size > max_size) {
        pthread_mutex_unlock(&shm->mutex);
        av_log(NULL, AV_LOG_ERROR, "Frame too large: %u > %u\n", slot_header->size, max_size);
        return AVERROR(ENOSPC);
    }

    /* Copy frame */
    memcpy(header, slot_header, sizeof(*header));
    memcpy(data, slot_data, slot_header->size);

    /* Advance read index */
    shm->read_idx++;
    shm->frame_count--;
    shm->frames_read++;

    /* Signal writers that space is available */
    pthread_cond_signal(&shm->cond_not_full);
    pthread_mutex_unlock(&shm->mutex);

    return slot_header->size;
}

void decklink_shm_server_shutdown(DecklinkShmBuffer *shm)
{
    if (!shm)
        return;

    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = 1;
    pthread_cond_broadcast(&shm->cond_not_full);
    pthread_cond_broadcast(&shm->cond_not_empty);
    pthread_mutex_unlock(&shm->mutex);
}

int decklink_shm_client_attach(const char *name, DecklinkShmBuffer **out_shm)
{
    int fd;
    struct stat st;
    DecklinkShmBuffer *shm;

    if (!name || !out_shm)
        return AVERROR(EINVAL);

    /* Open existing shared memory */
    fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        av_log(NULL, AV_LOG_ERROR, "shm_open(%s) failed: %s\n", name, strerror(errno));
        return AVERROR(errno);
    }

    /* Get size */
    if (fstat(fd, &st) < 0) {
        av_log(NULL, AV_LOG_ERROR, "fstat failed: %s\n", strerror(errno));
        close(fd);
        return AVERROR(errno);
    }

    /* Map */
    shm = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm == MAP_FAILED) {
        av_log(NULL, AV_LOG_ERROR, "mmap failed: %s\n", strerror(errno));
        return AVERROR(errno);
    }

    /* Validate */
    if (shm->magic != DECKLINK_SHM_MAGIC) {
        av_log(NULL, AV_LOG_ERROR, "Invalid shared memory magic\n");
        munmap(shm, st.st_size);
        return AVERROR(EINVAL);
    }

    if (shm->version != DECKLINK_SHM_VERSION) {
        av_log(NULL, AV_LOG_ERROR, "Shared memory version mismatch: %u != %u\n",
               shm->version, DECKLINK_SHM_VERSION);
        munmap(shm, st.st_size);
        return AVERROR(EINVAL);
    }

    if (!shm->server_active) {
        av_log(NULL, AV_LOG_ERROR, "Server is not active\n");
        munmap(shm, st.st_size);
        return AVERROR(ENOENT);
    }

    av_log(NULL, AV_LOG_INFO, "Attached to shared memory buffer '%s': %u frames, %u bytes/frame\n",
           name, shm->max_frames, shm->frame_data_size);

    *out_shm = shm;
    return 0;
}

void decklink_shm_client_detach(DecklinkShmBuffer *shm)
{
    if (!shm)
        return;

    munmap(shm, shm->total_size);
    av_log(NULL, AV_LOG_INFO, "Detached from shared memory buffer\n");
}

int decklink_shm_client_write(DecklinkShmBuffer *shm, const DecklinkShmFrameHeader *header,
                               const void *data, int timeout_ms)
{
    struct timespec ts;
    void *slot;
    DecklinkShmFrameHeader *slot_header;
    void *slot_data;
    int ret = 0;

    if (!shm || !header || !data)
        return AVERROR(EINVAL);

    if (header->size > shm->frame_data_size) {
        av_log(NULL, AV_LOG_ERROR, "Frame too large for buffer: %u > %u\n",
               header->size, shm->frame_data_size);
        return AVERROR(ENOSPC);
    }

    pthread_mutex_lock(&shm->mutex);

    /* Check server status */
    if (!shm->server_active || shm->shutdown) {
        pthread_mutex_unlock(&shm->mutex);
        return AVERROR_EOF;
    }

    /* Wait for space to be available */
    while (shm->frame_count >= shm->max_frames && !shm->shutdown) {
        if (timeout_ms > 0) {
            get_abs_timeout(&ts, timeout_ms);
            ret = pthread_cond_timedwait(&shm->cond_not_full, &shm->mutex, &ts);
            if (ret == ETIMEDOUT) {
                shm->frames_dropped++;
                pthread_mutex_unlock(&shm->mutex);
                return AVERROR(EAGAIN);
            }
        } else if (timeout_ms == 0) {
            shm->frames_dropped++;
            pthread_mutex_unlock(&shm->mutex);
            return AVERROR(EAGAIN);
        } else {
            pthread_cond_wait(&shm->cond_not_full, &shm->mutex);
        }
    }

    if (shm->shutdown) {
        pthread_mutex_unlock(&shm->mutex);
        return AVERROR_EOF;
    }

    /* Write frame to slot */
    slot = decklink_shm_get_slot(shm, shm->write_idx);
    slot_header = decklink_shm_get_header(slot);
    slot_data = decklink_shm_get_data(slot);

    memcpy(slot_header, header, sizeof(*header));
    slot_header->magic = DECKLINK_SHM_MAGIC;  /* Ensure magic is set */
    memcpy(slot_data, data, header->size);

    /* Advance write index */
    shm->write_idx++;
    shm->frame_count++;
    shm->frames_written++;

    /* Signal readers that data is available */
    pthread_cond_signal(&shm->cond_not_empty);
    pthread_mutex_unlock(&shm->mutex);

    return 0;
}

int decklink_shm_client_server_active(DecklinkShmBuffer *shm)
{
    if (!shm)
        return 0;
    return shm->server_active && !shm->shutdown;
}
