#pragma once

#include "common.h"
#include <list>
#include <unordered_map>
#include <vector>
#include <pthread.h>

class ScanDiskController;

// ============================================================================
// Module 6: Virtual Memory & LRU Swap Engine
// ============================================================================
// Detects memory exhaustion and evicts least-recently-used idle jobs
// from the 64 KB arena to simulated disk storage (swap.img).
// Rules:
// - Only jobs in STATE_IN_MEMORY (not STATE_PRINTING) can be victimized
// - Swapped jobs transition to STATE_SWAPPED_TO_DISK
// - LRU tracking uses list + hash-map for O(1) touch and eviction
// ============================================================================
class SwapEngine {
public:
    explicit SwapEngine(ScanDiskController* disk_controller);
    ~SwapEngine();

    // LRU cache maintenance
    void record_access(uint32_t job_id);
    void remove_job(uint32_t job_id);

    // Victim selection: returns pointer to LRU eligible job, or nullptr
    JobControlBlock* select_lru_victim(const std::vector<JobControlBlock*>& candidates);

    // Evicts victim job to disk: serializes JCB, frees arena block, sets STATE_SWAPPED_TO_DISK
    bool swap_out(JobControlBlock* victim);

    // Restores swapped job from disk back into arena
    bool swap_in(JobControlBlock* job);

    // Cylinder management on simulated disk
    int32_t allocate_cylinder();
    void free_cylinder(int32_t cylinder);
    size_t active_swapped_count() const;

private:
    ScanDiskController* disk_controller_ = nullptr;
    std::list<uint32_t> lru_list_;
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_map_;

    // Cylinder allocation bitmap (200 cylinders)
    bool cylinder_used_[DISK_TOTAL_CYLINDERS];

    mutable pthread_mutex_t mutex_;
};
