/*
 * Real-Time Telemetry and Observability
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_TELEMETRY_H
#define AVDEVICE_RT_TELEMETRY_H

#include <stdint.h>
#include <pthread.h>

/**
 * Telemetry and metrics export for monitoring
 *
 * Provides Prometheus-compatible metrics export via HTTP endpoint.
 */
typedef struct RTTelemetry {
    // Counters
    uint64_t frames_processed_total;
    uint64_t frames_dropped_total;
    uint64_t decode_errors_total;
    uint64_t filter_errors_total;
    uint64_t hw_errors_total;

    // Gauges
    float cpu_usage_percent;
    float memory_usage_mb;
    uint32_t buffer_depth_frames;
    float processing_time_ms;
    float clock_drift_ppm;

    // Health status
    struct {
        int healthy;
        const char *status_message;
        uint64_t uptime_seconds;
    } health;

    // HTTP server
    pthread_t server_thread;
    int server_running;
    int server_stop;
    int server_port;
    int server_socket;

} RTTelemetry;

/**
 * Initialize telemetry
 *
 * @param tel  Telemetry structure
 * @param port HTTP port for metrics (e.g., 9090)
 * @return 0 on success
 */
int rt_telemetry_init(RTTelemetry *tel, int port);

/**
 * Start telemetry HTTP server
 *
 * @param tel Telemetry
 * @return 0 on success
 */
int rt_telemetry_start(RTTelemetry *tel);

/**
 * Stop telemetry server
 *
 * @param tel Telemetry
 */
void rt_telemetry_stop(RTTelemetry *tel);

/**
 * Update telemetry metrics
 *
 * @param tel Telemetry
 */
void rt_telemetry_update(RTTelemetry *tel);

/**
 * Export metrics in Prometheus format
 *
 * @param tel    Telemetry
 * @param buffer Output buffer
 * @param size   Buffer size
 * @return Bytes written
 */
int rt_telemetry_export_prometheus(RTTelemetry *tel, char *buffer, size_t size);

/**
 * Get health check status
 *
 * @param tel Telemetry
 * @return 1 if healthy, 0 if unhealthy
 */
int rt_telemetry_is_healthy(RTTelemetry *tel);

/**
 * Cleanup telemetry
 *
 * @param tel Telemetry
 */
void rt_telemetry_cleanup(RTTelemetry *tel);

#endif /* AVDEVICE_RT_TELEMETRY_H */
