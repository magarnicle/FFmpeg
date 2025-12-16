/*
 * Real-Time Resource Monitoring - Predictive Failure Detection
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_MONITOR_H
#define AVDEVICE_RT_MONITOR_H

#include <stdint.h>
#include <pthread.h>

/**
 * Resource monitor for predictive failure detection
 *
 * Monitors CPU, memory, I/O and predicts problems before they occur.
 */
typedef struct RTResourceMonitor {
    // CPU tracking
    struct {
        float usage_percent;
        float per_thread_max;
        float headroom_percent;
        int throttle_required;
    } cpu;

    // Memory tracking
    struct {
        uint64_t allocated_bytes;
        uint64_t pool_usage_bytes;
        float fragmentation_ratio;
        int exhaustion_predicted;
    } memory;

    // I/O tracking
    struct {
        uint64_t bytes_read_per_sec;
        uint32_t read_latency_ms;
        int disk_congestion;
    } disk;

    // Predictions
    struct {
        int cpu_overload_predicted;
        int memory_exhaustion_predicted;
        int io_starvation_predicted;
        uint32_t frames_until_critical;
    } predictions;

    // History for trend analysis
    float cpu_history[100];
    uint64_t memory_history[100];
    int history_index;

    pthread_t monitor_thread;
    int thread_running;
    int thread_stop;

} RTResourceMonitor;

/**
 * Initialize resource monitor
 *
 * @param mon Monitor to initialize
 * @return 0 on success
 */
int rt_monitor_init(RTResourceMonitor *mon);

/**
 * Start monitoring thread
 *
 * @param mon Monitor
 * @return 0 on success
 */
int rt_monitor_start(RTResourceMonitor *mon);

/**
 * Stop monitoring thread
 *
 * @param mon Monitor
 */
void rt_monitor_stop(RTResourceMonitor *mon);

/**
 * Update resource measurements
 *
 * @param mon Monitor
 */
void rt_monitor_update(RTResourceMonitor *mon);

/**
 * Check if should trigger degradation
 *
 * @param mon Monitor
 * @return 1 if should degrade, 0 otherwise
 */
int rt_monitor_should_degrade(RTResourceMonitor *mon);

/**
 * Get statistics
 *
 * @param mon   Monitor
 * @param stats Output buffer
 * @param size  Buffer size
 * @return Bytes written
 */
int rt_monitor_get_stats(RTResourceMonitor *mon, char *stats, size_t size);

/**
 * Cleanup monitor
 *
 * @param mon Monitor
 */
void rt_monitor_cleanup(RTResourceMonitor *mon);

#endif /* AVDEVICE_RT_MONITOR_H */
