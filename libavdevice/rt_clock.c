/*
 * Real-Time Clock Synchronization - Implementation
 * Copyright (c) 2025
 */

#include "rt_clock.h"
#include "libavutil/time.h"
#include "libavutil/log.h"
#include <string.h>
#include <math.h>

int rt_clock_init(RTClock *clk, int64_t frame_period_us) {
    if (!clk)
        return AVERROR(EINVAL);

    memset(clk, 0, sizeof(RTClock));

    clk->reference_time_us = av_gettime();
    clk->reference_pts = 0;
    clk->frame_period_us = frame_period_us;

    // PID controller gains (Ziegler-Nichols tuning)
    clk->pid.kp = 0.1;    // Proportional gain
    clk->pid.ki = 0.01;   // Integral gain (prevent steady-state error)
    clk->pid.kd = 0.05;   // Derivative gain (damping)

    clk->pid.last_sync_time_us = clk->reference_time_us;

    av_log(NULL, AV_LOG_INFO,
           "RT Clock initialized: frame_period=%"PRId64"us (%.2f fps)\n",
           frame_period_us, 1000000.0 / frame_period_us);

    return 0;
}

int64_t rt_clock_get_pts(RTClock *clk, int64_t frame_num) {
    if (!clk)
        return 0;

    // Base PTS calculation
    int64_t pts = clk->reference_pts + (frame_num * clk->frame_period_us);

    // Apply drift correction
    double correction = (pts * clk->drift_ppm) / 1000000.0;
    pts -= (int64_t)correction;

    return pts;
}

void rt_clock_sync(RTClock *clk, int64_t actual_time_us, int64_t frame_num) {
    if (!clk)
        return;

    // Calculate expected time for this frame
    int64_t expected_time = clk->reference_time_us +
                           rt_clock_get_pts(clk, frame_num);

    // Calculate error (how far off we are)
    int64_t error = actual_time_us - expected_time;

    // Update jitter statistics
    int64_t abs_error = error < 0 ? -error : error;
    if (abs_error > clk->max_jitter_us) {
        clk->max_jitter_us = abs_error;
    }

    // Exponential moving average for mean jitter
    if (clk->jitter_samples == 0) {
        clk->mean_jitter_us = abs_error;
    } else {
        clk->mean_jitter_us = (clk->mean_jitter_us * 95 + abs_error * 5) / 100;
    }
    clk->jitter_samples++;

    // PID controller update
    int64_t now = av_gettime();
    double dt = (now - clk->pid.last_sync_time_us) / 1000000.0;  // seconds

    if (dt > 0.0) {
        // Proportional term
        double p_term = clk->pid.kp * error;

        // Integral term (prevent windup)
        clk->pid.integral += error * dt;
        if (clk->pid.integral > 100000.0) clk->pid.integral = 100000.0;
        if (clk->pid.integral < -100000.0) clk->pid.integral = -100000.0;
        double i_term = clk->pid.ki * clk->pid.integral;

        // Derivative term
        double derivative = (error - clk->pid.prev_error) / dt;
        double d_term = clk->pid.kd * derivative;

        // Calculate adjustment
        double adjustment = p_term + i_term + d_term;

        // Update drift (in parts per million)
        clk->drift_ppm += adjustment / 10000.0;  // Scale down adjustment

        // Clamp drift to reasonable range
        if (clk->drift_ppm > 100.0) clk->drift_ppm = 100.0;
        if (clk->drift_ppm < -100.0) clk->drift_ppm = -100.0;

        clk->pid.prev_error = error;
        clk->pid.last_sync_time_us = now;
    }

    // Log significant drift
    if (fabs(clk->drift_ppm) > 10.0 && clk->vsync_count % 1000 == 0) {
        av_log(NULL, AV_LOG_INFO,
               "Clock drift: %.3f ppm, jitter: %"PRId64"us avg / %"PRId64"us max\n",
               clk->drift_ppm, clk->mean_jitter_us, clk->max_jitter_us);
    }

    clk->last_vsync_us = actual_time_us;
    clk->vsync_count++;
}

int rt_clock_get_stats(RTClock *clk, char *stats, size_t size) {
    if (!clk)
        return 0;

    return snprintf(stats, size,
                   "Clock: drift=%.3f ppm, jitter avg=%"PRId64"us max=%"PRId64"us, "
                   "vsyncs=%"PRId64"\n",
                   clk->drift_ppm, clk->mean_jitter_us, clk->max_jitter_us,
                   clk->vsync_count);
}
