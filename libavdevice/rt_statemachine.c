/*
 * Real-Time State Machine - Implementation
 * Copyright (c) 2025
 */

#include "rt_statemachine.h"
#include "libavutil/log.h"
#include <string.h>

int rt_statemachine_init(RTStateMachine *sm) {
    if (!sm)
        return AVERROR(EINVAL);

    memset(sm, 0, sizeof(RTStateMachine));

    sm->state = RT_STATE_INIT;
    sm->state_entered_time = time(NULL);

    // Set default thresholds
    sm->thresholds.max_decode_errors_per_minute = 10;
    sm->thresholds.max_drops_per_minute = 30;  // 1.2% at 25fps
    sm->thresholds.degraded_threshold = 50;
    sm->thresholds.critical_threshold = 100;

    // Recovery settings
    sm->recovery.max_retries = 3;
    sm->recovery.backoff_ms = 100;

    av_log(NULL, AV_LOG_INFO, "RT State Machine initialized\n");

    return 0;
}

static void transition_state(RTStateMachine *sm, RTPlayoutState new_state) {
    if (sm->state == new_state)
        return;

    const char *from = rt_statemachine_get_state_name(sm);
    sm->prev_state = sm->state;
    sm->state = new_state;
    sm->state_entered_time = time(NULL);

    av_log(NULL, AV_LOG_WARNING, "State transition: %s -> %s\n",
           from, rt_statemachine_get_state_name(sm));

    // Reset degradation on transition to running
    if (new_state == RT_STATE_RUNNING) {
        sm->degradation.skip_b_frames = 0;
        sm->degradation.reduce_filter_quality = 0;
        sm->degradation.framerate_half = 0;
        sm->degradation.use_placeholders = 0;
    }
}

int rt_statemachine_handle_decode_error(RTStateMachine *sm) {
    if (!sm)
        return 1;

    sm->errors.decode_errors++;
    sm->errors.last_error_time = time(NULL);

    // Check error rate
    time_t now = time(NULL);
    time_t minute_ago = now - 60;

    // Simple rate limiting: if too many errors, degrade
    if (sm->errors.decode_errors > sm->thresholds.max_decode_errors_per_minute &&
        sm->errors.last_error_time > minute_ago) {

        if (sm->state == RT_STATE_RUNNING) {
            av_log(NULL, AV_LOG_WARNING,
                   "Too many decode errors (%u in last minute), entering degraded mode\n",
                   sm->errors.decode_errors);
            transition_state(sm, RT_STATE_DEGRADED);
            sm->degradation.skip_b_frames = 1;
            sm->degradation.use_placeholders = 1;
        }
    }

    // Return action based on state
    switch (sm->state) {
    case RT_STATE_RUNNING:
        return 2;  // Use placeholder frame

    case RT_STATE_DEGRADED:
        return 2;  // Use placeholder

    case RT_STATE_RECOVERING:
        // Try real decode once more
        sm->recovery.retry_count++;
        if (sm->recovery.retry_count > sm->recovery.max_retries) {
            transition_state(sm, RT_STATE_DEGRADED);
            return 2;
        }
        return 0;  // Continue

    default:
        return 1;  // Skip frame
    }
}

int rt_statemachine_handle_filter_error(RTStateMachine *sm) {
    if (!sm)
        return 1;

    sm->errors.filter_errors++;

    if (sm->errors.filter_errors > sm->thresholds.degraded_threshold) {
        if (sm->state == RT_STATE_RUNNING) {
            av_log(NULL, AV_LOG_WARNING, "Entering degraded mode due to filter errors\n");
            transition_state(sm, RT_STATE_DEGRADED);
            sm->degradation.reduce_filter_quality = 1;
        }
    }

    return sm->state == RT_STATE_DEGRADED ? 1 : 0;
}

int rt_statemachine_handle_hw_error(RTStateMachine *sm) {
    if (!sm)
        return 2;

    sm->errors.hw_errors++;

    if (sm->errors.hw_errors > 10) {
        av_log(NULL, AV_LOG_ERROR, "Too many hardware errors, aborting\n");
        transition_state(sm, RT_STATE_ERROR);
        return 2;  // Abort
    }

    // Retry with backoff
    if (sm->recovery.retry_count < sm->recovery.max_retries) {
        sm->recovery.retry_count++;
        return 0;  // Retry
    }

    return 1;  // Skip
}

void rt_statemachine_handle_dropped_frame(RTStateMachine *sm) {
    if (!sm)
        return;

    sm->errors.dropped_frames++;

    // Check drop rate
    if (sm->errors.dropped_frames > sm->thresholds.max_drops_per_minute) {
        if (sm->state == RT_STATE_RUNNING) {
            av_log(NULL, AV_LOG_WARNING,
                   "High frame drop rate (%u), entering degraded mode\n",
                   sm->errors.dropped_frames);
            transition_state(sm, RT_STATE_DEGRADED);
            sm->degradation.skip_b_frames = 1;
        }
    }
}

int rt_statemachine_should_degrade(RTStateMachine *sm) {
    if (!sm)
        return 0;

    time_t now = time(NULL);
    time_t minute_ago = now - 60;

    // Count errors in last minute
    uint32_t recent_errors = 0;
    if (sm->errors.last_error_time > minute_ago) {
        recent_errors = sm->errors.decode_errors + sm->errors.filter_errors;
    }

    return recent_errors > sm->thresholds.degraded_threshold;
}

int rt_statemachine_try_recover(RTStateMachine *sm) {
    if (!sm || sm->state != RT_STATE_DEGRADED)
        return 0;

    time_t now = time(NULL);
    time_t in_degraded_for = now - sm->state_entered_time;

    // Don't try to recover immediately
    if (in_degraded_for < 10)
        return 0;

    // Check if error rate has decreased
    if (sm->errors.decode_errors < sm->thresholds.max_decode_errors_per_minute / 2) {
        av_log(NULL, AV_LOG_INFO, "Error rate decreased, attempting recovery\n");
        transition_state(sm, RT_STATE_RECOVERING);
        sm->recovery.retry_count = 0;

        // Gradual recovery: remove degradations one by one
        if (sm->degradation.framerate_half) {
            sm->degradation.framerate_half = 0;
        } else if (sm->degradation.reduce_filter_quality) {
            sm->degradation.reduce_filter_quality = 0;
        } else if (sm->degradation.skip_b_frames) {
            sm->degradation.skip_b_frames = 0;
        } else {
            // All degradations removed, back to normal
            transition_state(sm, RT_STATE_RUNNING);
            return 1;
        }
    }

    return 0;
}

const char* rt_statemachine_get_state_name(RTStateMachine *sm) {
    if (!sm)
        return "NULL";

    switch (sm->state) {
    case RT_STATE_INIT:       return "INIT";
    case RT_STATE_BUFFERING:  return "BUFFERING";
    case RT_STATE_RUNNING:    return "RUNNING";
    case RT_STATE_DEGRADED:   return "DEGRADED";
    case RT_STATE_RECOVERING: return "RECOVERING";
    case RT_STATE_ERROR:      return "ERROR";
    case RT_STATE_SHUTDOWN:   return "SHUTDOWN";
    default:                  return "UNKNOWN";
    }
}

int rt_statemachine_get_stats(RTStateMachine *sm, char *stats, size_t size) {
    if (!sm)
        return 0;

    int written = snprintf(stats, size,
                          "State: %s (for %ld sec)\n",
                          rt_statemachine_get_state_name(sm),
                          time(NULL) - sm->state_entered_time);

    written += snprintf(stats + written, size - written,
                       "Errors: decode=%u filter=%u hw=%u dropped=%u\n",
                       sm->errors.decode_errors,
                       sm->errors.filter_errors,
                       sm->errors.hw_errors,
                       sm->errors.dropped_frames);

    if (sm->state == RT_STATE_DEGRADED) {
        written += snprintf(stats + written, size - written,
                           "Degradations: skip_b=%d reduce_filter=%d half_fps=%d placeholders=%d\n",
                           sm->degradation.skip_b_frames,
                           sm->degradation.reduce_filter_quality,
                           sm->degradation.framerate_half,
                           sm->degradation.use_placeholders);
    }

    return written;
}
