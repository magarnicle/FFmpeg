/*
 * Real-Time Playout Scheduler - Implementation
 * Copyright (c) 2025
 */

#include "rt_scheduler.h"
#include "libavutil/time.h"
#include "libavutil/log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int compare_uint64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

static void rt_calculate_percentiles(RTStageProfile *prof) {
    if (prof->sample_count == 0)
        return;

    uint64_t sorted[1000];
    uint32_t count = prof->sample_count < 1000 ? prof->sample_count : 1000;
    memcpy(sorted, prof->samples, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_uint64);

    prof->p999_us = sorted[(count * 999) / 1000];
    if (sorted[count-1] > prof->wcet_us)
        prof->wcet_us = sorted[count-1];
}

static void rt_record_sample(RTStageProfile *prof, uint64_t duration_us) {
    prof->samples[prof->sample_index] = duration_us;
    prof->sample_index = (prof->sample_index + 1) % 1000;

    if (prof->sample_count < 1000)
        prof->sample_count++;

    if (prof->sample_count % 100 == 0)
        rt_calculate_percentiles(prof);
}

int rt_scheduler_init(RTScheduler *sched, AVRational frame_rate) {
    if (!sched)
        return AVERROR(EINVAL);

    memset(sched, 0, sizeof(RTScheduler));

    sched->reference_time_us = av_gettime();
    sched->frame_rate_num = frame_rate.num;
    sched->frame_rate_den = frame_rate.den;
    sched->vsync_period_us = (int64_t)(1000000.0 * frame_rate.den / frame_rate.num);

    for (int i = 0; i < RT_STAGE_COUNT; i++) {
        sched->stages[i].wcet_us = 10000;
        sched->stages[i].p999_us = 8000;
    }

    pthread_mutex_init(&sched->mutex, NULL);

    av_log(NULL, AV_LOG_INFO, "RT Scheduler: %d/%d fps, period %"PRId64" us\n",
           frame_rate.num, frame_rate.den, sched->vsync_period_us);

    return 0;
}

int rt_scheduler_submit_frame(RTScheduler *sched, int64_t pts) {
    pthread_mutex_lock(&sched->mutex);

    int frame_id = -1;
    for (int i = 0; i < RT_SCHEDULER_MAX_FRAMES; i++) {
        if (sched->frames[i].state == RT_FRAME_STATE_PENDING ||
            sched->frames[i].state == RT_FRAME_STATE_COMPLETED ||
            sched->frames[i].state == RT_FRAME_STATE_FAILED) {
            frame_id = i;
            break;
        }
    }

    if (frame_id < 0) {
        pthread_mutex_unlock(&sched->mutex);
        return AVERROR(EAGAIN);
    }

    RTFrameInfo *info = &sched->frames[frame_id];
    memset(info, 0, sizeof(RTFrameInfo));
    info->pts = pts;
    info->deadline_us = sched->reference_time_us + pts;
    info->state = RT_FRAME_STATE_PENDING;

    pthread_mutex_unlock(&sched->mutex);
    return frame_id;
}

int rt_scheduler_should_process(RTScheduler *sched, int frame_id, RTStage stage) {
    pthread_mutex_lock(&sched->mutex);

    RTFrameInfo *info = &sched->frames[frame_id];
    int64_t now = av_gettime();
    int64_t time_to_deadline = info->deadline_us - now;

    int64_t time_needed = 0;
    for (int s = stage; s < RT_STAGE_COUNT; s++) {
        time_needed += sched->stages[s].p999_us > 0 ?
                       sched->stages[s].p999_us : 10000;
    }

    pthread_mutex_unlock(&sched->mutex);

    if (time_to_deadline < time_needed) {
        av_log(NULL, AV_LOG_WARNING,
               "Frame %d stage %d: skipping (need %"PRId64"us, have %"PRId64"us)\n",
               frame_id, stage, time_needed, time_to_deadline);
        return 0;
    }

    return 1;
}

void rt_scheduler_stage_start(RTScheduler *sched, int frame_id, RTStage stage) {
    pthread_mutex_lock(&sched->mutex);
    RTFrameInfo *info = &sched->frames[frame_id];
    info->current_stage = stage;
    info->state = RT_FRAME_STATE_PROCESSING;
    info->start_time_us = av_gettime();
    pthread_mutex_unlock(&sched->mutex);
}

void rt_scheduler_stage_complete(RTScheduler *sched, int frame_id, RTStage stage, int success) {
    pthread_mutex_lock(&sched->mutex);

    RTFrameInfo *info = &sched->frames[frame_id];
    int64_t duration = av_gettime() - info->start_time_us;
    info->stage_duration_us[stage] = duration;

    rt_record_sample(&sched->stages[stage], duration);

    if (success) {
        if (stage == RT_STAGE_OUTPUT) {
            info->state = RT_FRAME_STATE_COMPLETED;
            sched->frames_processed++;
        }
    } else {
        info->state = RT_FRAME_STATE_FAILED;
        sched->frames_dropped++;
    }

    pthread_mutex_unlock(&sched->mutex);
}

int rt_scheduler_get_stats(RTScheduler *sched, char *stats, size_t size) {
    pthread_mutex_lock(&sched->mutex);

    int written = snprintf(stats, size,
                          "RT Scheduler: processed=%"PRIu64" dropped=%"PRIu64" (%.2f%%)\n",
                          sched->frames_processed, sched->frames_dropped,
                          sched->frames_processed > 0 ?
                              100.0 * sched->frames_dropped / sched->frames_processed : 0.0);

    const char *names[] = {"Demux", "Decode", "Filter", "Encode", "Output"};
    for (int i = 0; i < RT_STAGE_COUNT; i++) {
        written += snprintf(stats + written, size - written,
                           "  %s: p999=%"PRIu64"us wcet=%"PRIu64"us\n",
                           names[i], sched->stages[i].p999_us, sched->stages[i].wcet_us);
    }

    pthread_mutex_unlock(&sched->mutex);
    return written;
}

void rt_scheduler_cleanup(RTScheduler *sched) {
    if (!sched)
        return;
    pthread_mutex_destroy(&sched->mutex);
    memset(sched, 0, sizeof(RTScheduler));
}
