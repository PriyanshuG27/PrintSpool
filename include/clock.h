#pragma once

#include <pthread.h>
#include <cstdint>

// ============================================================================
// Simulated Clock Interface
// ============================================================================
// Provides deterministic time control. Supports both:
// 1. Virtual discrete steps (tick-based, zero drift, 100% reproducible tests)
// 2. Real-time scaled mode with real sleep
// ============================================================================
class SimClock {
public:
    static SimClock& instance();

    void reset();
    void set_mode(bool virtual_time, double scale = 1.0);
    bool is_virtual() const;

    double now();                        // Current time in simulated seconds
    uint64_t ticks();                    // Current tick count
    void advance_seconds(double delta);  // Advance virtual time by seconds
    void sleep_simulated(double sec);    // Sleep/wait for specified simulated duration

private:
    SimClock();
    ~SimClock();

    mutable pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    double current_time_sec_ = 0.0;
    uint64_t tick_count_ = 0;
    bool virtual_mode_ = true;
    double time_scale_ = 1.0;
};
