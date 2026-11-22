#pragma once

#include "common.h"
#include <vector>
#include <atomic>
#include <pthread.h>

class BoundedRingBuffer;
class SwapEngine;

// ============================================================================
// Module 3: Hybrid SRTF & Priority Aging Dispatcher
// ============================================================================
// Selects the next job for dispatch using dynamic score:
// score = a * remaining_pages - b * role_weight - c * aging_boost(wait) - d * urgency
// Lower score is dispatched first!
// Dispatches job by allocating memory in 64 KB arena, triggering swap if full.
// ============================================================================
class Scheduler {
public:
    Scheduler(BoundedRingBuffer* ring_buffer, SwapEngine* swap_engine, const SystemConfig& config);
    ~Scheduler();

    bool start();
    void stop();

    // Add job directly to scheduler ready pool
    void add_job(JobControlBlock* job);

    // Compute dynamic score for a job at the current simulated time
    double compute_score(const JobControlBlock* job, double current_time) const;

    // Select the best candidate among waiting/resident jobs
    JobControlBlock* pick_next_job();

    // Atomically claim the best resident job for printer `worker_id`: the job is removed from the
    // ready queue AND moved IN_MEMORY -> PRINTING under the scheduler lock, so no other printer
    // (or the eviction logic) can ever see it. Blocks until a job is available, the scheduler
    // stops, or `keep_running` becomes false (then returns nullptr).
    JobControlBlock* acquire_job_for_printer(int worker_id, const std::atomic<bool>& keep_running);

    // Return completed job or notify memory freed
    void on_job_completed(JobControlBlock* job);
    void wake_printers();

    // Snapshot for telemetry
    std::vector<JobControlBlock*> get_all_jobs_snapshot() const;
    size_t ready_count() const;
    size_t in_memory_count() const;
    size_t swapped_count() const;
    size_t completed_count() const;
    // Jobs that are not yet COMPLETED/REJECTED (includes swapped-out and currently printing jobs)
    size_t unfinished_count() const;
    // Total jobs the scheduler has taken ownership of (any state)
    size_t total_jobs_count() const;

private:
    static void* dispatcher_thread(void* arg);
    void run_dispatcher();

    BoundedRingBuffer* ring_buffer_ = nullptr;
    SwapEngine* swap_engine_ = nullptr;
    SystemConfig config_;

    std::vector<JobControlBlock*> all_jobs_;
    std::vector<JobControlBlock*> in_memory_queue_; // Jobs ready for printing

    std::atomic<bool> running_{false};
    pthread_t thread_;
    mutable pthread_mutex_t mutex_;
    pthread_cond_t ready_cond_;
};
