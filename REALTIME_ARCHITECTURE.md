# FFmpeg Real-Time Playout Architecture Proposal

## Executive Summary

FFmpeg's current architecture is optimized for batch file transcoding, not real-time playout.
For stable 24/7 operation at 25fps to DeckLink hardware, fundamental architectural changes are needed.

## Core Problems

### 1. Threading Model - No Real-Time Guarantees

**Current Architecture:**
```
┌──────────┐    ┌─────────┐    ┌────────┐    ┌────────┐    ┌──────────┐
│ Demuxer  │───▶│ Decoder │───▶│ Filter │───▶│ Encode │───▶│ DeckLink │
│ Thread   │    │ Thread  │    │ Thread │    │ Thread │    │  Thread  │
└──────────┘    └─────────┘    └────────┘    └────────┘    └──────────┘
     │               │              │              │              │
     └───────────────┴──────────────┴──────────────┴──────────────┘
              Coordinated via unbounded queues
              No timing guarantees, no deadlines
```

**Issues:**
- Each stage runs "as fast as possible"
- No concept of frame deadlines
- Queue congestion can cause unbounded delays
- No priority ordering (old frames aren't differentiated from current)
- Thread priorities not set (compete equally with system threads)

**Proposed Real-Time Architecture:**
```
┌────────────────────────────────────────────────────────────┐
│                Real-Time Scheduler (New)                    │
│  - Maintains 25fps timeline clock                          │
│  - Assigns deadlines to each frame (PTS + processing time) │
│  - Prioritizes frames by urgency (deadline - current_time) │
│  - Pre-empts work on late frames                           │
│  - Tracks worst-case execution time (WCET) per stage       │
└────────────────────────────────────────────────────────────┘
         │         │         │         │         │
    ┌────▼───┐ ┌──▼────┐ ┌──▼────┐ ┌──▼────┐ ┌──▼─────┐
    │ Demux  │ │ Decode│ │ Filter│ │ Encode│ │DeckLink│
    │ Worker │ │ Worker│ │ Worker│ │ Worker│ │ Worker │
    │ Pool   │ │ Pool  │ │ Pool  │ │ Pool  │ │  RT    │
    └────────┘ └───────┘ └───────┘ └───────┘ └────────┘
         ↓         ↓         ↓         ↓         ↓
    Priority-ordered queues with deadline awareness
```

**Key Changes:**
1. **Central scheduler** tracks frame deadlines (PTS + WCET estimates)
2. **Worker pools** instead of dedicated threads (better CPU utilization)
3. **Priority queues** that order by urgency, not arrival time
4. **Deadline awareness** - can abort work on frames that will miss deadline
5. **Thread priorities** - output thread at RT priority, others normal

---

### 2. Memory Management - Unpredictable Latency

**Current:**
- Uses `av_malloc()/av_free()` which calls `malloc()/free()`
- Reference counting with atomic operations
- Deallocation can happen at any time (unbounded latency)
- Memory fragmentation over 24/7 operation

**Proposed:**
```c
// Pre-allocated frame pools per resolution/format
typedef struct RTFramePool {
    AVFrame *frames[MAX_FRAMES];
    int frame_size;
    int capacity;
    int in_use;
    pthread_spinlock_t lock;

    // Memory is allocated once at startup
    uint8_t *backing_memory;  // mmap'd with MAP_LOCKED
} RTFramePool;

// O(1) allocation with bounded latency
AVFrame* rt_frame_pool_get(RTFramePool *pool) {
    // Spinlock instead of mutex (bounded wait time)
    pthread_spin_lock(&pool->lock);

    if (pool->in_use >= pool->capacity) {
        pthread_spin_unlock(&pool->lock);
        return NULL;  // Pool exhausted - predictable failure
    }

    AVFrame *frame = pool->frames[pool->in_use++];
    pthread_spin_unlock(&pool->lock);
    return frame;
}

// O(1) free with no syscalls
void rt_frame_pool_put(RTFramePool *pool, AVFrame *frame) {
    pthread_spin_lock(&pool->lock);
    pool->frames[--pool->in_use] = frame;
    pthread_spin_unlock(&pool->lock);
    // No free(), no deallocation, no fragmentation
}
```

**Benefits:**
- Predictable allocation/deallocation time
- No memory fragmentation
- No malloc contention
- Memory locked in RAM (no page faults)

---

### 3. Clock Synchronization - Drift Over Time

**Current:**
- Uses `av_gettime()` (microsecond resolution)
- No correction for clock drift
- PTS calculated incrementally (errors accumulate)
- No sync with external reference

**Proposed:**
```c
typedef struct RTClock {
    // Hardware reference (e.g., genlock, PTP, NTP)
    int64_t reference_time_us;
    int64_t reference_frame_num;

    // Measured clock drift (parts per million)
    double drift_ppm;

    // PID controller for drift correction
    struct {
        double kp, ki, kd;
        double integral, prev_error;
    } pid;

    // Frame timing statistics
    struct {
        int64_t mean_frame_interval_us;
        int64_t stddev_us;
        int64_t max_jitter_us;
    } stats;
} RTClock;

// Calculate frame PTS with drift correction
int64_t rt_clock_get_frame_pts(RTClock *clk, int64_t frame_num) {
    // Base calculation
    int64_t pts = clk->reference_time_us +
                  (frame_num - clk->reference_frame_num) * 40000;  // 25fps

    // Apply drift correction
    double correction = (pts * clk->drift_ppm) / 1000000.0;
    pts -= (int64_t)correction;

    return pts;
}

// Measure and correct drift periodically
void rt_clock_sync(RTClock *clk, int64_t actual_hw_time) {
    int64_t expected_time = rt_clock_get_frame_pts(clk, clk->current_frame);
    int64_t error = actual_hw_time - expected_time;

    // PID controller update
    clk->pid.integral += error;
    double derivative = error - clk->pid.prev_error;

    double adjustment = clk->pid.kp * error +
                       clk->pid.ki * clk->pid.integral +
                       clk->pid.kd * derivative;

    clk->drift_ppm = adjustment;
    clk->pid.prev_error = error;
}
```

**Benefits:**
- Sub-microsecond timing accuracy maintained over days
- Automatic correction for system clock drift
- Synchronization with external references (genlock)
- Jitter measurement and compensation

---

### 4. Error Recovery - Fail Fast vs Fail Safe

**Current:**
- Most errors are fatal (`return AVERROR(...)` propagates up)
- No automatic retry logic
- No graceful degradation
- State is difficult to recover

**Proposed State Machine:**
```c
typedef enum {
    RT_STATE_INIT,
    RT_STATE_BUFFERING,
    RT_STATE_RUNNING,
    RT_STATE_DEGRADED,     // Running with reduced quality/framerate
    RT_STATE_RECOVERING,   // Attempting to return to normal
    RT_STATE_ERROR,
    RT_STATE_SHUTDOWN
} RTPlayoutState;

typedef struct RTStateMachine {
    RTPlayoutState state;
    RTPlayoutState prev_state;

    // Error counters
    struct {
        uint32_t decode_errors;
        uint32_t filter_errors;
        uint32_t hw_errors;
        uint32_t dropped_frames;
    } counters;

    // Recovery strategies
    struct {
        int retry_count;
        int max_retries;
        int backoff_ms;
        time_t last_retry;
    } recovery;

    // Degradation strategies
    struct {
        bool skip_bframes;           // Drop B-frames if struggling
        bool reduce_filter_quality;  // Disable expensive filters
        bool framerate_halving;      // Output 12.5fps instead of 25fps
    } degradation;

} RTStateMachine;

// Example: Decode error handler
int rt_handle_decode_error(RTStateMachine *sm, AVPacket *pkt) {
    sm->counters.decode_errors++;

    switch (sm->state) {
    case RT_STATE_RUNNING:
        if (sm->counters.decode_errors > 10) {
            // Too many errors, enter degraded mode
            av_log(NULL, AV_LOG_WARNING,
                   "Entering degraded mode due to decode errors\n");
            sm->state = RT_STATE_DEGRADED;
            sm->degradation.skip_bframes = true;
        }

        // Generate black frame as placeholder
        return rt_generate_placeholder_frame(pkt->pts);

    case RT_STATE_DEGRADED:
        // Already degraded, just skip this frame
        return rt_copy_previous_frame();

    case RT_STATE_RECOVERING:
        // Try full decode once more
        if (decode_succeeds) {
            sm->counters.decode_errors = 0;
            sm->state = RT_STATE_RUNNING;
        }
        return ret;
    }
}
```

**Recovery Strategies:**
1. **Placeholder Frames**: Generate black/freeze frame on decode errors
2. **Frame Duplication**: Repeat previous frame if current fails
3. **B-Frame Skipping**: Only decode I/P frames under load
4. **Filter Bypass**: Disable complex filters temporarily
5. **Framerate Reduction**: Output 12.5fps (duplicate each frame 2x)
6. **Quality Reduction**: Lower filter quality settings
7. **Source Retry**: Re-seek and retry file read
8. **Automatic Restart**: Restart pipeline after N failures

---

### 5. Resource Monitoring - Reactive vs Predictive

**Current:**
- No monitoring of system resources
- Reacts to failures after they happen
- No early warning system

**Proposed:**
```c
typedef struct RTResourceMonitor {
    // CPU usage tracking
    struct {
        float total_usage;
        float per_thread_usage[MAX_THREADS];
        float headroom_percent;
        bool throttle_required;
    } cpu;

    // Memory tracking
    struct {
        size_t allocated_bytes;
        size_t pool_utilization;
        size_t page_faults;
        float fragmentation_ratio;
    } memory;

    // I/O tracking
    struct {
        uint64_t bytes_read_per_sec;
        uint32_t read_latency_ms;
        uint32_t queue_depth;
        bool disk_congestion;
    } disk;

    // Network (if streaming input)
    struct {
        uint32_t packet_loss_percent;
        uint32_t jitter_ms;
        bool congestion_detected;
    } network;

    // Predictive alerts
    struct {
        bool cpu_overload_predicted;
        bool memory_exhaustion_predicted;
        bool io_starvation_predicted;
        uint32_t frames_until_critical;
    } predictions;

} RTResourceMonitor;

// Predictive monitoring (runs in separate thread)
void* rt_resource_monitor_thread(void *arg) {
    RTResourceMonitor *mon = arg;

    while (running) {
        // Measure resource usage
        rt_measure_cpu_usage(mon);
        rt_measure_memory_usage(mon);
        rt_measure_io_usage(mon);

        // Predict future state (100ms ahead)
        float cpu_trend = calculate_trend(mon->cpu.history, 10);
        if (mon->cpu.total_usage + cpu_trend * 2.5 > 95.0) {
            mon->predictions.cpu_overload_predicted = true;
            mon->predictions.frames_until_critical =
                (int)((95.0 - mon->cpu.total_usage) / cpu_trend * 25);

            // Trigger proactive action
            rt_trigger_degradation(DEGRADE_REDUCE_FILTER_QUALITY);
        }

        // Check memory pools
        float pool_usage = mon->memory.pool_utilization;
        if (pool_usage > 0.9) {
            av_log(NULL, AV_LOG_WARNING,
                   "Frame pool 90%% full - may run out in %d frames\n",
                   mon->predictions.frames_until_critical);

            // Reduce buffer targets to free memory
            rt_reduce_buffer_depth();
        }

        usleep(10000);  // Check every 10ms
    }
}
```

**Predictive Actions:**
- Reduce filter quality before CPU overload
- Flush buffers before memory exhaustion
- Increase read-ahead before I/O starvation
- Trigger backup source before network failure

---

### 6. Frame Pipeline - Flow Control

**Current:**
- No backpressure mechanism
- Queues can grow unbounded
- No awareness of downstream capacity

**Proposed:**
```c
typedef struct RTPipeline {
    // Each stage has input/output capacity
    struct {
        uint32_t capacity;      // Max frames this stage can handle
        uint32_t in_flight;     // Currently processing
        uint32_t queue_depth;   // Waiting to process
        float processing_time_ms;  // Average time per frame
    } stages[PIPELINE_STAGES];

    // Flow control
    struct {
        bool backpressure_active;
        uint32_t blocked_stage;  // Which stage is bottleneck
        uint32_t frames_to_skip;  // Frames to drop to catch up
    } flow_control;

} RTPipeline;

// Backpressure algorithm
bool rt_pipeline_can_accept_frame(RTPipeline *pipe, int stage) {
    // Check if this stage is full
    if (pipe->stages[stage].in_flight >= pipe->stages[stage].capacity) {
        return false;
    }

    // Check if downstream stages can keep up
    for (int i = stage + 1; i < PIPELINE_STAGES; i++) {
        float downstream_rate = 1000.0 / pipe->stages[i].processing_time_ms;
        float required_rate = 25.0;  // 25fps

        if (downstream_rate < required_rate * 1.1) {  // Need 10% headroom
            // Downstream can't keep up, apply backpressure
            pipe->flow_control.backpressure_active = true;
            pipe->flow_control.blocked_stage = i;
            return false;
        }
    }

    return true;
}

// Intelligent frame dropping
void rt_pipeline_apply_backpressure(RTPipeline *pipe) {
    int stage = pipe->flow_control.blocked_stage;

    // Calculate how many frames to skip to catch up
    float processing_rate = 1000.0 / pipe->stages[stage].processing_time_ms;
    float deficit = 25.0 - processing_rate;  // Frames per second deficit

    // Skip B-frames first, then P-frames if desperate
    if (deficit < 5.0) {
        rt_enable_b_frame_skip();
    } else if (deficit < 10.0) {
        rt_enable_p_frame_skip();
    } else {
        // Can't keep up even with frame skipping
        // Reduce frame rate or quality
        rt_trigger_severe_degradation();
    }
}
```

---

### 7. Deterministic Behavior - WCET Analysis

**Current:**
- Processing time varies wildly per frame
- No profiling of worst-case scenarios
- Can't guarantee frame deadlines

**Proposed:**
```c
typedef struct RTWCETProfile {
    // Worst-case execution time measurements
    struct {
        uint64_t decode_wcet_us;
        uint64_t filter_wcet_us;
        uint64_t encode_wcet_us;
        uint64_t total_wcet_us;
    } wcet;

    // Per-frame type statistics
    struct {
        uint64_t i_frame_time_us;
        uint64_t p_frame_time_us;
        uint64_t b_frame_time_us;
    } per_type;

    // Histogram of processing times
    uint32_t histogram[100];  // 100 buckets of 100us each

    // Percentile tracking
    struct {
        uint64_t p50_us;  // Median
        uint64_t p95_us;  // 95th percentile
        uint64_t p99_us;  // 99th percentile
        uint64_t p999_us; // 99.9th percentile (WCET estimate)
    } percentiles;

} RTWCETProfile;

// Profile processing time for each frame
void rt_profile_frame(RTWCETProfile *prof, FrameType type, uint64_t duration_us) {
    // Update WCET if this is the worst we've seen
    if (duration_us > prof->wcet.total_wcet_us) {
        prof->wcet.total_wcet_us = duration_us;
        av_log(NULL, AV_LOG_INFO, "New WCET: %"PRIu64" us\n", duration_us);
    }

    // Update per-type averages
    switch (type) {
        case FRAME_TYPE_I:
            prof->per_type.i_frame_time_us =
                (prof->per_type.i_frame_time_us * 0.95) + (duration_us * 0.05);
            break;
        // ... P and B frames
    }

    // Update histogram
    int bucket = duration_us / 100;
    if (bucket < 100) prof->histogram[bucket]++;

    // Recalculate percentiles every 1000 frames
    if (frame_count % 1000 == 0) {
        rt_calculate_percentiles(prof);
    }
}

// Admission control based on WCET
bool rt_can_meet_deadline(RTWCETProfile *prof, int64_t deadline_us) {
    int64_t time_available = deadline_us - av_gettime();

    // Use 99.9th percentile as conservative estimate
    // (will meet deadline 999 times out of 1000)
    if (time_available >= prof->percentiles.p999_us) {
        return true;
    }

    // Not enough time even in best case, skip this frame
    av_log(NULL, AV_LOG_WARNING,
           "Insufficient time for frame: need %"PRIu64"us, have %"PRId64"us\n",
           prof->percentiles.p999_us, time_available);
    return false;
}
```

---

### 8. Hardware Integration - Tighter Coupling

**Current:**
- DeckLink is just another muxer
- No special awareness of hardware timing requirements
- Hardware buffer managed reactively

**Proposed:**
```c
typedef struct RTHardwareInterface {
    // Direct hardware timing
    struct {
        int64_t (*get_hardware_time)(void);
        int64_t (*get_next_vsync_time)(void);
        void (*wait_for_vsync)(void);
    } timing;

    // Hardware buffer management
    struct {
        uint32_t optimal_depth;     // Ideal buffer depth for smooth playback
        uint32_t minimum_depth;     // Below this, stutter risk
        uint32_t maximum_depth;     // Above this, excessive latency
        uint32_t current_depth;

        // Predictive refill
        float drain_rate;           // Frames per second being consumed
        uint32_t frames_until_empty;
    } buffer;

    // Hardware health
    struct {
        bool genlock_locked;
        bool reference_present;
        uint32_t dropped_frames_hw;  // Hardware-reported drops
        float temperature_celsius;
    } health;

    // Interrupt-driven callbacks (not polled)
    struct {
        void (*on_vsync)(void *ctx);
        void (*on_buffer_low)(void *ctx, uint32_t frames_remaining);
        void (*on_genlock_lost)(void *ctx);
    } callbacks;

} RTHardwareInterface;

// Frame scheduling synchronized to hardware vsync
int rt_hardware_schedule_frame(RTHardwareInterface *hw, AVFrame *frame, int64_t pts) {
    // Wait until optimal time to submit (not too early, not too late)
    int64_t current_hw_time = hw->timing.get_hardware_time();
    int64_t next_vsync = hw->timing.get_next_vsync_time();
    int64_t time_until_vsync = next_vsync - current_hw_time;

    // If we have more than 2 vsyncs worth of time, wait
    if (time_until_vsync > 80000) {  // 2 frames @ 25fps = 80ms
        // Sleep until closer to vsync
        usleep(time_until_vsync - 40000);  // Wake up 1 frame before
    }

    // Now submit to hardware
    int ret = hw->submit_frame(frame, pts);

    // Update buffer prediction
    hw->buffer.current_depth = hw->get_buffer_depth();
    hw->buffer.drain_rate = calculate_drain_rate(hw);
    hw->buffer.frames_until_empty =
        hw->buffer.current_depth / hw->buffer.drain_rate * 25.0;

    return ret;
}

// Vsync interrupt handler
void rt_vsync_interrupt_handler(void *ctx) {
    RTHardwareInterface *hw = ctx;

    // Precise timing measurement
    int64_t vsync_time = hw->timing.get_hardware_time();

    // Check if we're drifting from expected timing
    int64_t expected_vsync = rt_clock_get_frame_pts(&global_clock,
                                                     global_frame_num);
    int64_t jitter = vsync_time - expected_vsync;

    if (abs(jitter) > 100) {  // More than 100us jitter
        // Adjust clock to resync
        rt_clock_sync(&global_clock, vsync_time);
    }

    // Signal playout thread that vsync occurred
    sem_post(&vsync_semaphore);
}
```

---

### 9. Telemetry and Observability

**Current:**
- Limited logging
- No metrics export
- No runtime introspection

**Proposed:**
```c
typedef struct RTTelemetry {
    // Prometheus-style metrics
    struct {
        uint64_t frames_processed_total;
        uint64_t frames_dropped_total;
        uint64_t decode_errors_total;
        uint64_t filter_errors_total;

        // Gauges
        float cpu_usage_percent;
        float memory_usage_percent;
        uint32_t buffer_depth_frames;
        float processing_time_ms;

        // Histograms
        uint64_t frame_latency_histogram[100];
        uint64_t processing_time_histogram[100];
    } metrics;

    // Export interface
    struct {
        int (*export_prometheus)(char *buf, size_t size);
        int (*export_json)(char *buf, size_t size);
        void (*publish_to_statsd)(const char *host, int port);
    } export;

    // Health check endpoint
    struct {
        bool healthy;
        const char *status_message;
        uint32_t uptime_seconds;
        uint32_t frames_until_problem;
    } health;

} RTTelemetry;

// HTTP endpoint for metrics (run in separate thread)
void* rt_telemetry_server(void *arg) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    // Bind to port 9090

    while (running) {
        int clientfd = accept(sockfd, NULL, NULL);

        char request[1024];
        read(clientfd, request, sizeof(request));

        if (strstr(request, "GET /metrics")) {
            char response[65536];
            rt_telemetry_export_prometheus(response, sizeof(response));

            write(clientfd, "HTTP/1.1 200 OK\r\n", 17);
            write(clientfd, "Content-Type: text/plain\r\n\r\n", 28);
            write(clientfd, response, strlen(response));
        }
        else if (strstr(request, "GET /health")) {
            if (global_telemetry.health.healthy) {
                write(clientfd, "HTTP/1.1 200 OK\r\n\r\n{\"status\":\"healthy\"}", 40);
            } else {
                write(clientfd, "HTTP/1.1 503 Service Unavailable\r\n\r\n", 35);
            }
        }

        close(clientfd);
    }
}
```

---

## Implementation Priority

### Phase 1 - Critical Foundation (1-2 months)
1. Real-time scheduler with deadline awareness
2. Memory pools (eliminate malloc/free in hot path)
3. State machine with error recovery
4. WCET profiling and admission control

### Phase 2 - Stability (1 month)
5. Clock synchronization with drift correction
6. Resource monitoring and prediction
7. Flow control and backpressure
8. Hardware vsync synchronization

### Phase 3 - Operations (2 weeks)
9. Telemetry and metrics export
10. Health check endpoints
11. Runtime configuration adjustment
12. Comprehensive logging

---

## Performance Targets

**Timing:**
- Frame delivery jitter: < 100μs (99.9th percentile)
- End-to-end latency: < 3 frames (120ms @ 25fps)
- Clock drift: < 1μs per hour

**Reliability:**
- MTBF: > 720 hours (30 days continuous)
- Recovery time: < 200ms for transient errors
- Zero crashes from OOM or resource exhaustion

**Resource Usage:**
- CPU headroom: > 30% (to handle I-frame spikes)
- Memory: Bounded and predictable
- Latency: All operations < 1ms (no page faults, no swap)

---

## Compatibility Considerations

These changes would require:
- New `avformat` flag: `AVFMT_FLAG_REALTIME`
- New codec caps: `AV_CODEC_CAP_REALTIME`
- New filter flag: `AVFILTER_FLAG_REALTIME`
- Separate `ffmpeg-rt` binary (not break existing ffmpeg)

---

## Testing Strategy

1. **Stress Testing**: Run for 30 days under load
2. **Fault Injection**: Simulate disk errors, CPU spikes, memory pressure
3. **Timing Analysis**: Measure jitter, latency, drift over 24 hours
4. **Resource Profiling**: Valgrind, perf, SystemTap analysis
5. **Hardware Integration**: Test with multiple DeckLink models
6. **Chaos Engineering**: Randomly kill threads, corrupt frames, etc.

---

## Conclusion

FFmpeg needs fundamental architectural changes for 24/7 real-time playout:

**Current**: Batch-oriented, best-effort, reactive, unbounded
**Needed**: Real-time, deadline-driven, predictive, deterministic

These changes would create a production-grade broadcast playout system
while maintaining backward compatibility with existing ffmpeg use cases.
