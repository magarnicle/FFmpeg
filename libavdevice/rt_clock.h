/*
 * Real-Time Clock Synchronization with Drift Correction
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_CLOCK_H
#define AVDEVICE_RT_CLOCK_H

#include <stdint.h>

/**
 * Real-time clock with drift correction
 *
 * Maintains precise timing over 24/7 operation using PID controller
 * to correct for system clock drift.
 */
typedef struct RTClock {
    // Reference timing
    int64_t reference_time_us;
    int64_t reference_pts;
    int64_t frame_period_us;

    // PID controller for drift correction
    struct {
        double kp, ki, kd;           // Gains
        double integral;
        double prev_error;
        int64_t last_sync_time_us;
    } pid;

    double drift_ppm;                // Drift in parts per million

    // Jitter statistics
    int64_t max_jitter_us;
    int64_t mean_jitter_us;
    int64_t jitter_samples;

    // Vsync tracking
    int64_t last_vsync_us;
    int64_t vsync_count;
} RTClock;

/**
 * Initialize real-time clock
 *
 * @param clk           Clock to initialize
 * @param frame_period_us Frame period in microseconds (e.g., 40000 for 25fps)
 * @return 0 on success
 */
int rt_clock_init(RTClock *clk, int64_t frame_period_us);

/**
 * Get corrected PTS for a frame number
 *
 * Applies drift correction to maintain accuracy over time.
 *
 * @param clk       Clock
 * @param frame_num Frame number
 * @return Corrected PTS in microseconds
 */
int64_t rt_clock_get_pts(RTClock *clk, int64_t frame_num);

/**
 * Synchronize clock with actual hardware time
 *
 * Should be called on each vsync or frame completion.
 * Uses PID controller to minimize drift over time.
 *
 * @param clk             Clock
 * @param actual_time_us  Actual hardware time
 * @param frame_num       Frame number that was displayed
 */
void rt_clock_sync(RTClock *clk, int64_t actual_time_us, int64_t frame_num);

/**
 * Get clock statistics
 *
 * @param clk   Clock
 * @param stats Output buffer
 * @param size  Buffer size
 * @return Bytes written
 */
int rt_clock_get_stats(RTClock *clk, char *stats, size_t size);

#endif /* AVDEVICE_RT_CLOCK_H */
