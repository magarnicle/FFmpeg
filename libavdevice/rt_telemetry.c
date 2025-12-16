/*
 * Real-Time Telemetry - Implementation
 * Copyright (c) 2025
 */

#include "rt_telemetry.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>

static int64_t telemetry_start_time = 0;

static void* rt_telemetry_server_func(void *arg) {
    RTTelemetry *tel = (RTTelemetry*)arg;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    av_log(NULL, AV_LOG_INFO, "Telemetry HTTP server listening on port %d\n", tel->server_port);

    while (!tel->server_stop) {
        int client_fd = accept(tel->server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!tel->server_stop)
                av_log(NULL, AV_LOG_WARNING, "Accept failed\n");
            continue;
        }

        char request[1024];
        ssize_t n = read(client_fd, request, sizeof(request) - 1);
        if (n > 0) {
            request[n] = '\0';

            if (strstr(request, "GET /metrics")) {
                // Prometheus metrics endpoint
                char response[65536];
                int len = rt_telemetry_export_prometheus(tel, response, sizeof(response));

                char header[256];
                snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; version=0.0.4\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n", len);

                write(client_fd, header, strlen(header));
                write(client_fd, response, len);

            } else if (strstr(request, "GET /health")) {
                // Health check endpoint
                if (rt_telemetry_is_healthy(tel)) {
                    const char *response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "\r\n"
                        "{\"status\":\"healthy\",\"uptime\":%lld}\n";
                    char buf[256];
                    snprintf(buf, sizeof(buf), response, tel->health.uptime_seconds);
                    write(client_fd, buf, strlen(buf));
                } else {
                    const char *response =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Type: application/json\r\n"
                        "\r\n"
                        "{\"status\":\"unhealthy\"}\n";
                    write(client_fd, response, strlen(response));
                }
            }
        }

        close(client_fd);
    }

    av_log(NULL, AV_LOG_INFO, "Telemetry HTTP server exiting\n");
    return NULL;
}

int rt_telemetry_init(RTTelemetry *tel, int port) {
    if (!tel)
        return AVERROR(EINVAL);

    memset(tel, 0, sizeof(RTTelemetry));

    tel->server_port = port;
    tel->health.healthy = 1;
    tel->health.status_message = "OK";

    telemetry_start_time = av_gettime();

    av_log(NULL, AV_LOG_INFO, "RT Telemetry initialized on port %d\n", port);

    return 0;
}

int rt_telemetry_start(RTTelemetry *tel) {
    if (!tel || tel->server_running)
        return AVERROR(EINVAL);

    // Create socket
    tel->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tel->server_socket < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create telemetry socket\n");
        return AVERROR(errno);
    }

    // Allow reuse
    int opt = 1;
    setsockopt(tel->server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(tel->server_port);

    if (bind(tel->server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to bind telemetry socket to port %d\n",
               tel->server_port);
        close(tel->server_socket);
        return AVERROR(errno);
    }

    // Listen
    if (listen(tel->server_socket, 5) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to listen on telemetry socket\n");
        close(tel->server_socket);
        return AVERROR(errno);
    }

    // Start server thread
    tel->server_stop = 0;
    int ret = pthread_create(&tel->server_thread, NULL, rt_telemetry_server_func, tel);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create telemetry server thread\n");
        close(tel->server_socket);
        return AVERROR(ret);
    }

    tel->server_running = 1;
    return 0;
}

void rt_telemetry_stop(RTTelemetry *tel) {
    if (!tel || !tel->server_running)
        return;

    tel->server_stop = 1;
    shutdown(tel->server_socket, SHUT_RDWR);
    close(tel->server_socket);

    pthread_join(tel->server_thread, NULL);
    tel->server_running = 0;
}

void rt_telemetry_update(RTTelemetry *tel) {
    if (!tel)
        return;

    tel->health.uptime_seconds = (av_gettime() - telemetry_start_time) / 1000000;
}

int rt_telemetry_export_prometheus(RTTelemetry *tel, char *buffer, size_t size) {
    if (!tel)
        return 0;

    int written = 0;

    // Counters
    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_frames_processed_total Total frames processed\n"
                       "# TYPE ffmpeg_frames_processed_total counter\n"
                       "ffmpeg_frames_processed_total %"PRIu64"\n\n",
                       tel->frames_processed_total);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_frames_dropped_total Total frames dropped\n"
                       "# TYPE ffmpeg_frames_dropped_total counter\n"
                       "ffmpeg_frames_dropped_total %"PRIu64"\n\n",
                       tel->frames_dropped_total);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_errors_total Total errors by type\n"
                       "# TYPE ffmpeg_errors_total counter\n"
                       "ffmpeg_errors_total{type=\"decode\"} %"PRIu64"\n"
                       "ffmpeg_errors_total{type=\"filter\"} %"PRIu64"\n"
                       "ffmpeg_errors_total{type=\"hardware\"} %"PRIu64"\n\n",
                       tel->decode_errors_total,
                       tel->filter_errors_total,
                       tel->hw_errors_total);

    // Gauges
    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_cpu_usage_percent CPU usage percentage\n"
                       "# TYPE ffmpeg_cpu_usage_percent gauge\n"
                       "ffmpeg_cpu_usage_percent %.2f\n\n",
                       tel->cpu_usage_percent);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_memory_usage_mb Memory usage in MB\n"
                       "# TYPE ffmpeg_memory_usage_mb gauge\n"
                       "ffmpeg_memory_usage_mb %.2f\n\n",
                       tel->memory_usage_mb);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_buffer_depth_frames Current buffer depth\n"
                       "# TYPE ffmpeg_buffer_depth_frames gauge\n"
                       "ffmpeg_buffer_depth_frames %u\n\n",
                       tel->buffer_depth_frames);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_clock_drift_ppm Clock drift in PPM\n"
                       "# TYPE ffmpeg_clock_drift_ppm gauge\n"
                       "ffmpeg_clock_drift_ppm %.3f\n\n",
                       tel->clock_drift_ppm);

    written += snprintf(buffer + written, size - written,
                       "# HELP ffmpeg_uptime_seconds Uptime in seconds\n"
                       "# TYPE ffmpeg_uptime_seconds counter\n"
                       "ffmpeg_uptime_seconds %"PRIu64"\n\n",
                       tel->health.uptime_seconds);

    return written;
}

int rt_telemetry_is_healthy(RTTelemetry *tel) {
    if (!tel)
        return 0;

    // Check if drop rate is acceptable
    if (tel->frames_processed_total > 0) {
        float drop_rate = (float)tel->frames_dropped_total / tel->frames_processed_total;
        if (drop_rate > 0.05) {  // More than 5% drops
            tel->health.healthy = 0;
            tel->health.status_message = "High drop rate";
            return 0;
        }
    }

    tel->health.healthy = 1;
    tel->health.status_message = "OK";
    return 1;
}

void rt_telemetry_cleanup(RTTelemetry *tel) {
    if (!tel)
        return;

    rt_telemetry_stop(tel);
    memset(tel, 0, sizeof(RTTelemetry));
}
