/*
 * Real-Time Memory Pool - Implementation
 * Copyright (c) 2025
 */

#include "rt_mempool.h"
#include "libavutil/log.h"
#include "libavutil/imgutils.h"
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

int rt_frame_pool_init(RTFramePool *pool, int width, int height,
                       enum AVPixelFormat format, int capacity) {
    if (!pool || capacity > RT_MEMPOOL_MAX_FRAMES)
        return AVERROR(EINVAL);

    memset(pool, 0, sizeof(RTFramePool));

    pool->frame_width = width;
    pool->frame_height = height;
    pool->format = format;
    pool->capacity = capacity;

    // Calculate frame size
    pool->frame_size = av_image_get_buffer_size(format, width, height, 32);
    if (pool->frame_size < 0)
        return pool->frame_size;

    // Allocate backing memory with mmap (locked in RAM)
    pool->backing_size = pool->frame_size * capacity;
    pool->backing_memory = mmap(NULL, pool->backing_size,
                                PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (pool->backing_memory == MAP_FAILED) {
        av_log(NULL, AV_LOG_ERROR, "Failed to mmap %zu bytes for frame pool\n",
               pool->backing_size);
        return AVERROR(ENOMEM);
    }

    // Lock memory in RAM to prevent page faults
    if (mlock(pool->backing_memory, pool->backing_size) < 0) {
        av_log(NULL, AV_LOG_WARNING, "Failed to mlock frame pool (may cause latency spikes)\n");
    }

    // Pre-allocate all AVFrame structures
    for (int i = 0; i < capacity; i++) {
        pool->frames[i] = av_frame_alloc();
        if (!pool->frames[i]) {
            rt_frame_pool_cleanup(pool);
            return AVERROR(ENOMEM);
        }

        // Set up frame with backing memory
        pool->frames[i]->width = width;
        pool->frames[i]->height = height;
        pool->frames[i]->format = format;

        uint8_t *ptr = pool->backing_memory + (i * pool->frame_size);
        av_image_fill_arrays(pool->frames[i]->data, pool->frames[i]->linesize,
                            ptr, format, width, height, 32);

        // Mark as available (we'll track in_use separately)
    }

    pthread_spin_init(&pool->lock, PTHREAD_PROCESS_PRIVATE);

    av_log(NULL, AV_LOG_INFO,
           "RT Frame Pool: %dx%d format %d, %d frames, %zu bytes total\n",
           width, height, format, capacity, pool->backing_size);

    return 0;
}

AVFrame* rt_frame_pool_get(RTFramePool *pool) {
    if (!pool)
        return NULL;

    pthread_spin_lock(&pool->lock);

    if (pool->in_use >= pool->capacity) {
        pool->pool_exhaustions++;
        pthread_spin_unlock(&pool->lock);
        av_log(NULL, AV_LOG_WARNING, "Frame pool exhausted (%d/%d in use)\n",
               pool->in_use, pool->capacity);
        return NULL;
    }

    AVFrame *frame = pool->frames[pool->in_use];
    pool->in_use++;
    pool->allocations++;

    if (pool->in_use > pool->high_water_mark)
        pool->high_water_mark = pool->in_use;

    pthread_spin_unlock(&pool->lock);

    return frame;
}

void rt_frame_pool_put(RTFramePool *pool, AVFrame *frame) {
    if (!pool || !frame)
        return;

    pthread_spin_lock(&pool->lock);

    // Find this frame in our pool
    int found = 0;
    for (int i = 0; i < pool->in_use; i++) {
        if (pool->frames[i] == frame) {
            // Swap with last in-use frame
            AVFrame *tmp = pool->frames[i];
            pool->frames[i] = pool->frames[pool->in_use - 1];
            pool->frames[pool->in_use - 1] = tmp;
            found = 1;
            break;
        }
    }

    if (found) {
        pool->in_use--;
        pool->frees++;
    }

    pthread_spin_unlock(&pool->lock);

    if (!found) {
        av_log(NULL, AV_LOG_ERROR, "Attempted to free frame not from pool!\n");
    }
}

int rt_frame_pool_get_stats(RTFramePool *pool, char *stats, size_t size) {
    if (!pool)
        return 0;

    pthread_spin_lock(&pool->lock);

    int written = snprintf(stats, size,
                          "Frame Pool: %d/%d in use (peak %d), "
                          "alloc=%"PRIu64" free=%"PRIu64" exhausted=%"PRIu64"\n",
                          pool->in_use, pool->capacity, pool->high_water_mark,
                          pool->allocations, pool->frees, pool->pool_exhaustions);

    pthread_spin_unlock(&pool->lock);
    return written;
}

void rt_frame_pool_cleanup(RTFramePool *pool) {
    if (!pool)
        return;

    pthread_spin_destroy(&pool->lock);

    for (int i = 0; i < pool->capacity; i++) {
        if (pool->frames[i]) {
            av_frame_free(&pool->frames[i]);
        }
    }

    if (pool->backing_memory && pool->backing_memory != MAP_FAILED) {
        munlock(pool->backing_memory, pool->backing_size);
        munmap(pool->backing_memory, pool->backing_size);
    }

    memset(pool, 0, sizeof(RTFramePool));
}
