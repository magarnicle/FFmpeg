/*
 * Real-Time Playout Scheduler
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_SCHEDULER_H
#define AVDEVICE_RT_SCHEDULER_H

#include <stdint.h>
#include <pthread.h>
#include "libavutil/avutil.h"
#include "libavutil/rational.h"

#define RT_SCHEDULER_MAX_FRAMES 256

typedef enum {
    RT_STAGE_DEMUX = 0,
    RT_STAGE_DECODE,
    RT_STAGE_FILTER,
    RT_STAGE_ENCODE,
    RT_STAGE_OUTPUT,
    RT_STAGE_COUNT
} RTStage;

typedef enum {
    RT_FRAME_STATE_PENDING,
    RT_FRAME_STATE_PROCESSING,
    RT_FRAME_STATE_COMPLETED,
    RT_FRAME_STATE_FAILED,
    RT_FRAME_STATE_SKIPPED
} RTFrameState;

typedef struct RTFrameInfo {
    int64_t pts;
    int64_t deadline_us;
    int64_t start_time_us;
    int64_t stage_duration_us[RT_STAGE_COUNT];

    RTFrameState state;
    RTStage current_stage;
    int priority;
} RTFrameInfo;

typedef struct RTStageProfile {
    uint64_t wcet_us;
    uint64_t p999_us;
    uint64_t samples[1000];
    uint32_t sample_index;
    uint32_t sample_count;
} RTStageProfile;

typedef struct RTScheduler {
    RTFrameInfo frames[RT_SCHEDULER_MAX_FRAMES];
    RTStageProfile stages[RT_STAGE_COUNT];

    int frame_rate_num;
    int frame_rate_den;
    int64_t reference_time_us;
    int64_t vsync_period_us;

    uint64_t frames_processed;
    uint64_t frames_dropped;

    pthread_mutex_t mutex;
} RTScheduler;

int rt_scheduler_init(RTScheduler *sched, AVRational frame_rate);
int rt_scheduler_submit_frame(RTScheduler *sched, int64_t pts);
int rt_scheduler_should_process(RTScheduler *sched, int frame_id, RTStage stage);
void rt_scheduler_stage_start(RTScheduler *sched, int frame_id, RTStage stage);
void rt_scheduler_stage_complete(RTScheduler *sched, int frame_id, RTStage stage, int success);
int rt_scheduler_get_stats(RTScheduler *sched, char *stats, size_t size);
void rt_scheduler_cleanup(RTScheduler *sched);

#endif /* AVDEVICE_RT_SCHEDULER_H */
