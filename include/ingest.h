#pragma once

#include "common.h"
#include <vector>
#include <string>

class BoundedRingBuffer;

class JobIngestionController {
public:
    JobIngestionController(BoundedRingBuffer* ring_buffer);
    ~JobIngestionController();

    // Validation rules:
    // 1 <= pages <= 100
    // valid role
    // returns true if valid and enqueued, false otherwise
    bool submit_job(uint32_t id, const std::string& client_id, UserRole role,
                    uint32_t pages, bool urgency, bool is_duplex,
                    const std::string& doc_name = "");

    // Parse and submit from workload file line:
    // Format: <timestamp_sec> <job_id> <client_id> <role: student|researcher|faculty> <pages> <urgency: 0|1> <duplex: 0|1> [doc_name]
    bool replay_workload_file(const std::string& filepath, bool pace_by_arrival = true);

    uint32_t accepted_jobs_count() const { return accepted_count_; }
    uint32_t rejected_jobs_count() const { return rejected_count_; }

private:
    BoundedRingBuffer* buffer_ = nullptr;
    uint32_t accepted_count_ = 0;
    uint32_t rejected_count_ = 0;
    mutable pthread_mutex_t stats_mutex_;
};
