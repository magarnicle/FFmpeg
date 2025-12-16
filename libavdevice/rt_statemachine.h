/*
 * Real-Time State Machine for Error Recovery
 * Copyright (c) 2025
 *
 * This file is part of FFmpeg.
 */

#ifndef AVDEVICE_RT_STATEMACHINE_H
#define AVDEVICE_RT_STATEMACHINE_H

#include <stdint.h>
#include <time.h>

typedef enum {
    RT_STATE_INIT,           // Initializing
    RT_STATE_BUFFERING,      // Building buffer before start
    RT_STATE_RUNNING,        // Normal operation
    RT_STATE_DEGRADED,       // Running with reduced quality
    RT_STATE_RECOVERING,     // Attempting to return to normal
    RT_STATE_ERROR,          // Fatal error
    RT_STATE_SHUTDOWN        // Clean shutdown
} RTPlayoutState;

typedef struct RTStateMachine {
    RTPlayoutState state;
    RTPlayoutState prev_state;
    time_t state_entered_time;

    // Error counters
    struct {
        uint32_t decode_errors;
        uint32_t filter_errors;
        uint32_t hw_errors;
        uint32_t dropped_frames;
        time_t last_error_time;
    } errors;

    // Recovery control
    struct {
        int retry_count;
        int max_retries;
        int backoff_ms;
        time_t last_retry_time;
    } recovery;

    // Degradation strategies
    struct {
        int skip_b_frames;
        int reduce_filter_quality;
        int framerate_half;
        int use_placeholders;
    } degradation;

    // Thresholds for state transitions
    struct {
        uint32_t max_decode_errors_per_minute;
        uint32_t max_drops_per_minute;
        uint32_t degraded_threshold;
        uint32_t critical_threshold;
    } thresholds;

} RTStateMachine;

/**
 * Initialize state machine
 *
 * @param sm State machine to initialize
 * @return 0 on success
 */
int rt_statemachine_init(RTStateMachine *sm);

/**
 * Handle a decode error
 *
 * May transition to degraded state or generate placeholder frame.
 *
 * @param sm State machine
 * @return Suggested action: 0=continue, 1=skip frame, 2=use placeholder
 */
int rt_statemachine_handle_decode_error(RTStateMachine *sm);

/**
 * Handle a filter error
 *
 * @param sm State machine
 * @return Suggested action
 */
int rt_statemachine_handle_filter_error(RTStateMachine *sm);

/**
 * Handle hardware error
 *
 * @param sm State machine
 * @return Suggested action: 0=retry, 1=skip, 2=abort
 */
int rt_statemachine_handle_hw_error(RTStateMachine *sm);

/**
 * Handle dropped frame
 *
 * @param sm State machine
 */
void rt_statemachine_handle_dropped_frame(RTStateMachine *sm);

/**
 * Check if should enter degraded mode
 *
 * @param sm State machine
 * @return 1 if should degrade, 0 otherwise
 */
int rt_statemachine_should_degrade(RTStateMachine *sm);

/**
 * Attempt recovery to normal operation
 *
 * @param sm State machine
 * @return 1 if recovery successful, 0 if should stay degraded
 */
int rt_statemachine_try_recover(RTStateMachine *sm);

/**
 * Get current state name
 *
 * @param sm State machine
 * @return State name string
 */
const char* rt_statemachine_get_state_name(RTStateMachine *sm);

/**
 * Get statistics
 *
 * @param sm    State machine
 * @param stats Output buffer
 * @param size  Buffer size
 * @return Bytes written
 */
int rt_statemachine_get_stats(RTStateMachine *sm, char *stats, size_t size);

#endif /* AVDEVICE_RT_STATEMACHINE_H */
