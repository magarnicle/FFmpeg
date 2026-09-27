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

#include "outdev_feed.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <inttypes.h>

#include "libavutil/log.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/bswap.h"

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

int ff_outdev_shm_server_create(const char *name, uint32_t max_frames,
                                uint32_t frame_data_size, FFOutdevShmBuffer **out_shm)
{
    int fd;
    size_t total_size;
    FFOutdevShmBuffer *shm;
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    int ret;

    if (!name || !out_shm || max_frames == 0 || frame_data_size == 0)
        return AVERROR(EINVAL);

    total_size = ff_outdev_shm_calc_size(max_frames, frame_data_size);

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
    shm->magic = FF_OUTDEV_SHM_MAGIC;
    shm->version = FF_OUTDEV_SHM_VERSION;
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

void ff_outdev_shm_server_destroy(const char *name, FFOutdevShmBuffer *shm)
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

int ff_outdev_shm_server_read(FFOutdevShmBuffer *shm, FFOutdevFrameHeader *header,
                              void *data, uint32_t max_size, int timeout_ms)
{
    struct timespec ts;
    void *slot;
    FFOutdevFrameHeader *slot_header;
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
    slot = ff_outdev_shm_get_slot(shm, shm->read_idx);
    slot_header = ff_outdev_shm_get_header(slot);
    slot_data = ff_outdev_shm_get_data(slot);

    /* Validate */
    if (slot_header->magic != FF_OUTDEV_SHM_MAGIC) {
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

void ff_outdev_shm_server_shutdown(FFOutdevShmBuffer *shm)
{
    if (!shm)
        return;

    pthread_mutex_lock(&shm->mutex);
    shm->shutdown = 1;
    pthread_cond_broadcast(&shm->cond_not_full);
    pthread_cond_broadcast(&shm->cond_not_empty);
    pthread_mutex_unlock(&shm->mutex);
}

int ff_outdev_shm_client_attach(const char *name, FFOutdevShmBuffer **out_shm)
{
    int fd;
    struct stat st;
    FFOutdevShmBuffer *shm;

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
    if (shm->magic != FF_OUTDEV_SHM_MAGIC) {
        av_log(NULL, AV_LOG_ERROR, "Invalid shared memory magic\n");
        munmap(shm, st.st_size);
        return AVERROR(EINVAL);
    }

    if (shm->version != FF_OUTDEV_SHM_VERSION) {
        av_log(NULL, AV_LOG_ERROR, "Shared memory version mismatch: %u != %u\n",
               shm->version, FF_OUTDEV_SHM_VERSION);
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

void ff_outdev_shm_client_detach(FFOutdevShmBuffer *shm)
{
    if (!shm)
        return;

    munmap(shm, shm->total_size);
    av_log(NULL, AV_LOG_INFO, "Detached from shared memory buffer\n");
}

int ff_outdev_shm_client_write(FFOutdevShmBuffer *shm, const FFOutdevFrameHeader *header,
                               const void *data, int timeout_ms)
{
    struct timespec ts;
    void *slot;
    FFOutdevFrameHeader *slot_header;
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
    slot = ff_outdev_shm_get_slot(shm, shm->write_idx);
    slot_header = ff_outdev_shm_get_header(slot);
    slot_data = ff_outdev_shm_get_data(slot);

    memcpy(slot_header, header, sizeof(*header));
    slot_header->magic = FF_OUTDEV_SHM_MAGIC;  /* Ensure magic is set */
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

int ff_outdev_shm_client_server_active(FFOutdevShmBuffer *shm)
{
    if (!shm)
        return 0;
    return shm->server_active && !shm->shutdown;
}

/* ---------------------------------------------------------------------------
 * Unix socket frame server
 * ------------------------------------------------------------------------- */

static int socket_read_exact(int fd, void *buf, size_t n)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;

    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r <= 0) {
            if (r == 0)
                return -1;              /* the far end went away */
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += r;
    }
    return 0;
}

static void *outdev_socket_thread(void *arg)
{
    FFOutdevSocketServer *s = (FFOutdevSocketServer *)arg;
    void *logctx = s->logctx;
    FFOutdevSocketHeader header;
    uint8_t *frame = NULL;
    size_t frame_size = 0;
    int connections = 0;

    av_log(logctx, AV_LOG_INFO, "Socket server listening on %s\n", s->path);

    while (!s->stop) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        struct pollfd pfd;
        int64_t frames = 0;
        int client_fd;

        /* Poll rather than block in accept, so stopping does not wait for a
         * connection that may never come. */
        pfd.fd = s->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) <= 0)
            continue;

        client_fd = accept(s->fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN)
                av_log(logctx, AV_LOG_ERROR, "Socket accept failed: %s\n",
                       strerror(errno));
            continue;
        }

        connections++;
        av_log(logctx, AV_LOG_INFO, "Socket: client %d connected\n", connections);

        while (!s->stop) {
            uint32_t data_size;
            int64_t pts;

            if (socket_read_exact(client_fd, &header, sizeof(header)) < 0)
                break;                  /* disconnected */

            if (ntohl(header.magic) != FF_OUTDEV_SOCKET_MAGIC) {
                av_log(logctx, AV_LOG_ERROR,
                       "Socket: magic 0x%08X, expected 0x%08X\n",
                       ntohl(header.magic), FF_OUTDEV_SOCKET_MAGIC);
                break;
            }

            data_size = ntohl(header.data_size);
            pts = (int64_t)av_be2ne64(header.pts);

            if (data_size > frame_size) {
                av_free(frame);
                frame = av_malloc(data_size);
                if (!frame) {
                    av_log(logctx, AV_LOG_ERROR,
                           "Socket: cannot allocate %u bytes\n", data_size);
                    frame_size = 0;
                    break;
                }
                frame_size = data_size;
            }

            if (socket_read_exact(client_fd, frame, data_size) < 0) {
                av_log(logctx, AV_LOG_ERROR, "Socket: short frame\n");
                break;
            }

            if (s->cb(s->opaque, header.stream_type, frame, data_size, pts) < 0)
                break;
            frames++;
        }

        close(client_fd);
        av_log(logctx, AV_LOG_INFO,
               "Socket: client %d disconnected after %"PRId64" frames\n",
               connections, frames);
    }

    av_free(frame);
    av_log(logctx, AV_LOG_INFO, "Socket server stopped\n");
    return NULL;
}

int ff_outdev_socket_server_start(FFOutdevSocketServer *s, const char *path,
                                  FFOutdevFrameCb cb, void *opaque, void *logctx)
{
    struct sockaddr_un addr;
    int ret;

    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->cb = cb;
    s->opaque = opaque;
    s->logctx = logctx;

    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->fd < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot create a socket: %s\n", strerror(errno));
        return AVERROR(errno);
    }

    unlink(path);                       /* a stale one would refuse the bind */

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot bind %s: %s\n", path, strerror(errno));
        close(s->fd);
        s->fd = -1;
        return AVERROR(errno);
    }
    if (listen(s->fd, 5) < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot listen on %s: %s\n", path, strerror(errno));
        close(s->fd);
        unlink(path);
        s->fd = -1;
        return AVERROR(errno);
    }

    s->path = av_strdup(path);
    ret = pthread_create(&s->thread, NULL, outdev_socket_thread, s);
    if (ret) {
        av_log(logctx, AV_LOG_ERROR, "Cannot start the socket thread: %s\n",
               strerror(ret));
        close(s->fd);
        unlink(path);
        av_freep(&s->path);
        s->fd = -1;
        return AVERROR(ret);
    }
    s->thread_started = 1;
    return 0;
}

void ff_outdev_socket_server_stop(FFOutdevSocketServer *s)
{
    if (s->thread_started) {
        s->stop = 1;
        pthread_join(s->thread, NULL);
        s->thread_started = 0;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    if (s->path) {
        unlink(s->path);
        av_freep(&s->path);
    }
}
