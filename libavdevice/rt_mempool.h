/*
 * Real-Time Memory Pool - Bounded allocation for playout
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_MEMPOOL_H
#define AVDEVICE_RT_MEMPOOL_H

#include <stdint.h>
#include <pthread.h>
#include "libavutil/frame.h"

#define RT_MEMPOOL_MAX_FRAMES 128

/**
 * Real-time frame pool
 *
 * Pre-allocates all memory at startup to avoid malloc/free in hot path.
 * Uses spinlocks for bounded wait times.
 * Memory is locked in RAM to prevent page faults.
 */
typedef struct RTFramePool {
    AVFrame *frames[RT_MEMPOOL_MAX_FRAMES];
    uint8_t *backing_memory;     // mmap'd memory region
    size_t backing_size;

    int frame_width;
    int frame_height;
    enum AVPixelFormat format;
    int frame_size;              // Size of each frame in bytes

    int capacity;                // Total number of frames
    int in_use;                  // Currently allocated
    int high_water_mark;         // Peak usage

    pthread_spinlock_t lock;     // Spinlock for bounded latency

    // Statistics
    uint64_t allocations;
    uint64_t frees;
    uint64_t pool_exhaustions;
} RTFramePool;

/**
 * Initialize a frame pool
 *
 * @param pool      Pool to initialize
 * @param width     Frame width
 * @param height    Frame height
 * @param format    Pixel format
 * @param capacity  Number of frames to pre-allocate
 * @return 0 on success, negative on error
 */
int rt_frame_pool_init(RTFramePool *pool, int width, int height,
                       enum AVPixelFormat format, int capacity);

/**
 * Get a frame from the pool (O(1), bounded latency)
 *
 * @param pool Pool to allocate from
 * @return AVFrame on success, NULL if pool exhausted
 */
AVFrame* rt_frame_pool_get(RTFramePool *pool);

/**
 * Return a frame to the pool (O(1), no syscalls)
 *
 * @param pool  Pool to return to
 * @param frame Frame to return
 */
void rt_frame_pool_put(RTFramePool *pool, AVFrame *frame);

/**
 * Get pool statistics
 *
 * @param pool   Pool to query
 * @param stats  Output buffer
 * @param size   Size of buffer
 * @return Number of bytes written
 */
int rt_frame_pool_get_stats(RTFramePool *pool, char *stats, size_t size);

/**
 * Cleanup pool and free all memory
 *
 * @param pool Pool to cleanup
 */
void rt_frame_pool_cleanup(RTFramePool *pool);

#endif /* AVDEVICE_RT_MEMPOOL_H */
