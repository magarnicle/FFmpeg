/*
 * Real-Time Resource Monitoring - Implementation
 * Copyright (c) 2025
 */

#include "rt_monitor.h"
#include "libavutil/log.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>

// Calculate linear regression trend
static float calculate_trend(float *history, int count) {
    if (count < 2)
        return 0.0f;

    float sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    for (int i = 0; i < count; i++) {
        sum_x += i;
        sum_y += history[i];
        sum_xy += i * history[i];
        sum_x2 += i * i;
    }

    float n = (float)count;
    float slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
    return slope;
}

static void* rt_monitor_thread_func(void *arg) {
    RTResourceMonitor *mon = (RTResourceMonitor*)arg;

    av_log(NULL, AV_LOG_INFO, "Resource monitor thread started\n");

    while (!mon->thread_stop) {
        rt_monitor_update(mon);

        // Predict future state
        if (mon->history_index >= 10) {
            float cpu_trend = calculate_trend(mon->cpu_history, 10);

            // Predict CPU overload 2.5 seconds ahead
            float predicted_cpu = mon->cpu.usage_percent + (cpu_trend * 2.5f * 25.0f);

            if (predicted_cpu > 95.0f) {
                mon->predictions.cpu_overload_predicted = 1;
                mon->predictions.frames_until_critical =
                    (int)((95.0f - mon->cpu.usage_percent) / cpu_trend);

                if (!mon->cpu.throttle_required) {
                    av_log(NULL, AV_LOG_WARNING,
                           "CPU overload predicted in %u frames (%.1f%%)\n",
                           mon->predictions.frames_until_critical, predicted_cpu);
                    mon->cpu.throttle_required = 1;
                }
            } else {
                mon->predictions.cpu_overload_predicted = 0;
                mon->cpu.throttle_required = 0;
            }
        }

        usleep(100000);  // Check every 100ms
    }

    av_log(NULL, AV_LOG_INFO, "Resource monitor thread exiting\n");
    return NULL;
}

int rt_monitor_init(RTResourceMonitor *mon) {
    if (!mon)
        return AVERROR(EINVAL);

    memset(mon, 0, sizeof(RTResourceMonitor));

    av_log(NULL, AV_LOG_INFO, "RT Resource Monitor initialized\n");

    return 0;
}

int rt_monitor_start(RTResourceMonitor *mon) {
    if (!mon || mon->thread_running)
        return AVERROR(EINVAL);

    mon->thread_stop = 0;
    int ret = pthread_create(&mon->monitor_thread, NULL, rt_monitor_thread_func, mon);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to create monitor thread\n");
        return AVERROR(ret);
    }

    mon->thread_running = 1;
    return 0;
}

void rt_monitor_stop(RTResourceMonitor *mon) {
    if (!mon || !mon->thread_running)
        return;

    mon->thread_stop = 1;
    pthread_join(mon->monitor_thread, NULL);
    mon->thread_running = 0;
}

void rt_monitor_update(RTResourceMonitor *mon) {
    if (!mon)
        return;

    // Measure CPU usage (simple approach using /proc/stat)
    static unsigned long long prev_total = 0, prev_idle = 0;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long long user, nice, system, idle;
        if (fscanf(fp, "cpu %llu %llu %llu %llu", &user, &nice, &system, &idle) == 4) {
            unsigned long long total = user + nice + system + idle;
            unsigned long long total_diff = total - prev_total;
            unsigned long long idle_diff = idle - prev_idle;

            if (total_diff > 0) {
                mon->cpu.usage_percent = 100.0f * (1.0f - ((float)idle_diff / total_diff));

                // Update history
                mon->cpu_history[mon->history_index % 100] = mon->cpu.usage_percent;

                // Calculate headroom
                mon->cpu.headroom_percent = 100.0f - mon->cpu.usage_percent;
            }

            prev_total = total;
            prev_idle = idle;
        }
        fclose(fp);
    }

    // Measure memory usage (simple approach using /proc/self/status)
    fp = fopen("/proc/self/status", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                unsigned long rss_kb;
                if (sscanf(line + 6, "%lu", &rss_kb) == 1) {
                    mon->memory.allocated_bytes = rss_kb * 1024;
                    mon->memory_history[mon->history_index % 100] = mon->memory.allocated_bytes;
                }
                break;
            }
        }
        fclose(fp);
    }

    mon->history_index++;
}

int rt_monitor_should_degrade(RTResourceMonitor *mon) {
    if (!mon)
        return 0;

    // Degrade if CPU usage is high or predicted to become high
    if (mon->cpu.usage_percent > 90.0f || mon->predictions.cpu_overload_predicted) {
        return 1;
    }

    // Degrade if memory is predicted to exhaust
    if (mon->predictions.memory_exhaustion_predicted) {
        return 1;
    }

    return 0;
}

int rt_monitor_get_stats(RTResourceMonitor *mon, char *stats, size_t size) {
    if (!mon)
        return 0;

    int written = snprintf(stats, size,
                          "Resources: CPU=%.1f%% (headroom %.1f%%), MEM=%"PRIu64" MB\n",
                          mon->cpu.usage_percent,
                          mon->cpu.headroom_percent,
                          mon->memory.allocated_bytes / (1024*1024));

    if (mon->predictions.cpu_overload_predicted) {
        written += snprintf(stats + written, size - written,
                           "PREDICTION: CPU overload in %u frames\n",
                           mon->predictions.frames_until_critical);
    }

    return written;
}

void rt_monitor_cleanup(RTResourceMonitor *mon) {
    if (!mon)
        return;

    rt_monitor_stop(mon);
    memset(mon, 0, sizeof(RTResourceMonitor));
}
