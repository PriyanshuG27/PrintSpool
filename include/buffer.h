#pragma once

#include "common.h"
#include <vector>
#include <pthread.h>

// ============================================================================
// Module 2: Bounded Buffer Concurrency Manager
// ============================================================================
// Implements a thread-safe circular ring buffer protected by POSIX mutex
// and two condition variables (not_full, not_empty).
// Guards against spurious wakeups via while loops.
// ============================================================================
class BoundedRingBuffer {
public:
    explicit BoundedRingBuffer(size_t capacity = RING_BUFFER_CAPACITY);
    ~BoundedRingBuffer();

    // Producer interface: blocks if buffer is full
    bool enqueue(JobControlBlock* job);

    // Consumer interface: blocks if buffer is empty
    JobControlBlock* dequeue();

    // Non-blocking try operations
    bool try_enqueue(JobControlBlock* job);
    JobControlBlock* try_dequeue();

    // Shutdown signaling: wakes all waiting threads
    void shutdown();
    bool is_shutdown() const;

    // Snapshot query for telemetry
    size_t size() const;
    size_t capacity() const;
    std::vector<JobControlBlock*> get_snapshot() const;

private:
    size_t capacity_;
    std::vector<JobControlBlock*> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    bool shutdown_ = false;

    mutable pthread_mutex_t mutex_;
    pthread_cond_t not_full_;
    pthread_cond_t not_empty_;
};
