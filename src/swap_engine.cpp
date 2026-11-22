#include "swap_engine.h"
#include "disk_scan.h"
#include "memory_arena.h"
#include "logger.h"
#include "clock.h"
#include <cstring>
#include <algorithm>

SwapEngine::SwapEngine(ScanDiskController* disk_controller)
    : disk_controller_(disk_controller) {
    pthread_mutex_init(&mutex_, nullptr);
    for (uint32_t i = 0; i < DISK_TOTAL_CYLINDERS; ++i) {
        cylinder_used_[i] = false;
    }
}

SwapEngine::~SwapEngine() {
    pthread_mutex_destroy(&mutex_);
}

void SwapEngine::record_access(uint32_t job_id) {
    pthread_mutex_lock(&mutex_);
    auto it = lru_map_.find(job_id);
    if (it != lru_map_.end()) {
        lru_list_.erase(it->second);
    }
    lru_list_.push_front(job_id); // Most Recently Used at front
    lru_map_[job_id] = lru_list_.begin();
    pthread_mutex_unlock(&mutex_);
}

void SwapEngine::remove_job(uint32_t job_id) {
    pthread_mutex_lock(&mutex_);
    auto it = lru_map_.find(job_id);
    if (it != lru_map_.end()) {
        lru_list_.erase(it->second);
        lru_map_.erase(it);
    }
    pthread_mutex_unlock(&mutex_);
}

JobControlBlock* SwapEngine::select_lru_victim(const std::vector<JobControlBlock*>& candidates) {
    pthread_mutex_lock(&mutex_);

    // Search from back (Least Recently Used)
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
        uint32_t candidate_id = *it;
        for (auto* job : candidates) {
            if (job && job->id == candidate_id) {
                // Rule: ONLY jobs in STATE_IN_MEMORY and NOT currently PRINTING can be evicted
                if (job->state == STATE_IN_MEMORY && job->assigned_printer_id < 0) {
                    pthread_mutex_unlock(&mutex_);
                    return job;
                }
            }
        }
    }

    pthread_mutex_unlock(&mutex_);
    return nullptr;
}

int32_t SwapEngine::allocate_cylinder() {
    pthread_mutex_lock(&mutex_);
    for (int32_t c = 0; c < static_cast<int32_t>(DISK_TOTAL_CYLINDERS); ++c) {
        if (!cylinder_used_[c]) {
            cylinder_used_[c] = true;
            pthread_mutex_unlock(&mutex_);
            return c;
        }
    }
    pthread_mutex_unlock(&mutex_);
    return -1; // Disk full
}

void SwapEngine::free_cylinder(int32_t cylinder) {
    if (cylinder < 0 || cylinder >= static_cast<int32_t>(DISK_TOTAL_CYLINDERS)) return;
    pthread_mutex_lock(&mutex_);
    cylinder_used_[cylinder] = false;
    pthread_mutex_unlock(&mutex_);
}

size_t SwapEngine::active_swapped_count() const {
    pthread_mutex_lock(&mutex_);
    size_t count = 0;
    for (uint32_t i = 0; i < DISK_TOTAL_CYLINDERS; ++i) {
        if (cylinder_used_[i]) count++;
    }
    pthread_mutex_unlock(&mutex_);
    return count;
}

bool SwapEngine::swap_out(JobControlBlock* victim) {
    if (!victim || victim->state != STATE_IN_MEMORY || victim->arena_offset < 0) {
        return false;
    }

    int32_t cyl = allocate_cylinder();
    if (cyl < 0) {
        LOG_E("SWAP_ENGINE", "Swap-out failed for Job #%u: disk storage full!", victim->id);
        return false;
    }

    // Prepare simulated serialized payload
    std::vector<uint8_t> swap_buf(DISK_SECTOR_SIZE, 0);
    // Write header summary into first bytes of swap block
    std::snprintf(reinterpret_cast<char*>(swap_buf.data()), swap_buf.size(),
                  "JCB#%u|ROLE:%s|PAGES:%u|DOC:%s",
                  victim->id, user_role_to_string(victim->role),
                  victim->remaining_pages.load(), victim->document_name.c_str());

    // Submit write through SCAN Disk Controller
    if (disk_controller_ && !disk_controller_->write_cylinder(cyl, swap_buf.data(), swap_buf.size())) {
        LOG_E("SWAP_ENGINE", "Disk controller write failed at cylinder %d for Job #%u", cyl, victim->id);
        free_cylinder(cyl);
        return false;
    }

    // Free block from First-Fit memory arena
    int32_t old_offset = victim->arena_offset.exchange(-1);
    MemoryArena::instance().deallocate(old_offset, victim->id);

    victim->swap_cylinder = cyl;

    remove_job(victim->id);

    transition_state(victim, STATE_SWAPPED_TO_DISK, "LRU evicted to disk");
    LOG_I("SWAP_ENGINE", "Swapped out Job #%u (freed offset %d -> cylinder %d, reclaimed %zu B)",
          victim->id, old_offset, cyl, victim->arena_size);

    return true;
}

bool SwapEngine::swap_in(JobControlBlock* job) {
    if (!job || job->state != STATE_SWAPPED_TO_DISK || job->swap_cylinder < 0) {
        return false;
    }

    // Attempt allocation in 64 KB First-Fit arena
    int32_t new_offset = MemoryArena::instance().allocate(job->arena_size, job->id);
    if (new_offset < 0) {
        LOG_W("SWAP_ENGINE", "Swap-in deferred for Job #%u: insufficient arena RAM", job->id);
        return false;
    }

    // Read payload from disk cylinder via SCAN controller
    std::vector<uint8_t> swap_buf(DISK_SECTOR_SIZE, 0);
    if (disk_controller_) {
        disk_controller_->read_cylinder(job->swap_cylinder, swap_buf.data(), swap_buf.size());
    }

    // Release cylinder on simulated disk
    free_cylinder(job->swap_cylinder);
    job->swap_cylinder = -1;
    job->arena_offset = new_offset;

    record_access(job->id);

    transition_state(job, STATE_IN_MEMORY, "swapped in from disk to RAM");
    LOG_I("SWAP_ENGINE", "Swapped in Job #%u (restored into arena offset %d, allocated %zu B)",
          job->id, new_offset, job->arena_size);

    return true;
}
