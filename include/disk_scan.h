#pragma once

#include "common.h"
#include <vector>
#include <string>
#include <pthread.h>

// ============================================================================
// Module 7: SCAN Disk Scheduling Controller
// ============================================================================
// Manages simulated 200 cylinders (0..199) mapped to flat file swap.img.
// Implements elevator (SCAN) scheduling:
// - Services requests in current direction until the boundary cylinder (199 or 0),
//   then reverses direction.
// - Requesters block on condvars until I/O finishes.
// ============================================================================

struct DiskRequest {
    uint32_t id;
    uint32_t cylinder;
    bool is_write;
    std::vector<uint8_t> buffer;
    bool completed = false;
    bool success = false;
    pthread_cond_t done_cond;
    pthread_mutex_t done_mutex;

    DiskRequest(uint32_t req_id, uint32_t cyl, bool write, const void* data, size_t sz);
    ~DiskRequest();
};

struct ScanSimulationResult {
    uint32_t total_head_movement_scan;
    uint32_t total_head_movement_fcfs;
    std::vector<uint32_t> scan_serviced_order;
    std::vector<uint32_t> fcfs_serviced_order;
    double improvement_percentage;
};

class ScanDiskController {
public:
    explicit ScanDiskController(const std::string& backing_file = "swap.img",
                                uint32_t total_cylinders = DISK_TOTAL_CYLINDERS);
    ~ScanDiskController();

    bool start();
    void stop();

    // Blocking I/O operations
    bool read_cylinder(uint32_t cylinder, void* out_buffer, size_t size);
    bool write_cylinder(uint32_t cylinder, const void* in_buffer, size_t size);

    // Status queries
    uint32_t current_head() const { return current_head_; }
    bool is_direction_up() const { return direction_up_; }
    uint64_t total_cylinders_traversed() const { return total_head_movement_; }
    size_t pending_requests_count() const;

    // Pure algorithm simulation for TC-04 verification:
    // head = 53, dir = UP, requests = [98, 183, 37, 122, 14, 124, 65, 67]
    // Expects: 331 cylinders for SCAN, 640 for FCFS
    static ScanSimulationResult simulate_scan_vs_fcfs(
        uint32_t initial_head, bool initial_dir_up,
        const std::vector<uint32_t>& requests,
        uint32_t max_cylinder = DISK_TOTAL_CYLINDERS - 1);

private:
    static void* disk_worker_thread(void* arg);
    void run_controller();

    std::string backing_file_;
    uint32_t total_cylinders_;
    uint32_t current_head_ = 53;
    bool direction_up_ = true;
    uint64_t total_head_movement_ = 0;

    std::vector<DiskRequest*> pending_queue_;
    bool running_ = false;

    pthread_t thread_;
    mutable pthread_mutex_t queue_mutex_;
    pthread_cond_t queue_cond_;
    uint32_t next_req_id_ = 1;
};
