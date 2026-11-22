#include "disk_scan.h"
#include "logger.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>

DiskRequest::DiskRequest(uint32_t req_id, uint32_t cyl, bool write, const void* data, size_t sz)
    : id(req_id), cylinder(cyl), is_write(write) {
    if (data && sz > 0) {
        buffer.assign(reinterpret_cast<const uint8_t*>(data),
                      reinterpret_cast<const uint8_t*>(data) + sz);
    } else {
        buffer.resize(sz > 0 ? sz : DISK_SECTOR_SIZE, 0);
    }
    pthread_mutex_init(&done_mutex, nullptr);
    pthread_cond_init(&done_cond, nullptr);
}

DiskRequest::~DiskRequest() {
    pthread_mutex_destroy(&done_mutex);
    pthread_cond_destroy(&done_cond);
}

ScanDiskController::ScanDiskController(const std::string& backing_file, uint32_t total_cylinders)
    : backing_file_(backing_file), total_cylinders_(total_cylinders) {
    pthread_mutex_init(&queue_mutex_, nullptr);
    pthread_cond_init(&queue_cond_, nullptr);

    // Initialize or verify flat file backing store swap.img
    std::fstream fs(backing_file_, std::ios::in | std::ios::out | std::ios::binary);
    if (!fs.is_open()) {
        std::ofstream create_fs(backing_file_, std::ios::out | std::ios::binary);
        std::vector<uint8_t> zero_sector(DISK_SECTOR_SIZE, 0);
        for (uint32_t i = 0; i < total_cylinders_; ++i) {
            create_fs.write(reinterpret_cast<const char*>(zero_sector.data()), zero_sector.size());
        }
        create_fs.close();
    }
}

ScanDiskController::~ScanDiskController() {
    stop();
    pthread_mutex_destroy(&queue_mutex_);
    pthread_cond_destroy(&queue_cond_);
}

bool ScanDiskController::start() {
    pthread_mutex_lock(&queue_mutex_);
    if (running_) {
        pthread_mutex_unlock(&queue_mutex_);
        return true;
    }
    running_ = true;
    int rc = pthread_create(&thread_, nullptr, disk_worker_thread, this);
    pthread_mutex_unlock(&queue_mutex_);
    if (rc != 0) {
        LOG_E("DISK_SCAN", "Failed to start disk controller worker thread");
        return false;
    }
    LOG_I("DISK_SCAN", "SCAN Disk Controller started (Head: %u, Dir: UP, Cylinders: %u)",
          current_head_, total_cylinders_);
    return true;
}

void ScanDiskController::stop() {
    pthread_mutex_lock(&queue_mutex_);
    if (!running_) {
        pthread_mutex_unlock(&queue_mutex_);
        return;
    }
    running_ = false;
    pthread_cond_broadcast(&queue_cond_);
    pthread_mutex_unlock(&queue_mutex_);

    pthread_join(thread_, nullptr);
    LOG_I("DISK_SCAN", "SCAN Disk Controller stopped. Total head travel: %llu cylinders",
          (unsigned long long)total_head_movement_);
}

bool ScanDiskController::read_cylinder(uint32_t cylinder, void* out_buffer, size_t size) {
    if (cylinder >= total_cylinders_ || !out_buffer || size == 0) return false;

    pthread_mutex_lock(&queue_mutex_);
    uint32_t req_id = next_req_id_++;
    auto* req = new DiskRequest(req_id, cylinder, false, nullptr, size);
    pending_queue_.push_back(req);
    pthread_cond_signal(&queue_cond_);
    pthread_mutex_unlock(&queue_mutex_);

    // Wait for elevator to service this cylinder
    pthread_mutex_lock(&req->done_mutex);
    while (!req->completed) {
        pthread_cond_wait(&req->done_cond, &req->done_mutex);
    }
    pthread_mutex_unlock(&req->done_mutex);

    bool ok = req->success;
    if (ok && out_buffer) {
        size_t copy_bytes = std::min(size, req->buffer.size());
        std::memcpy(out_buffer, req->buffer.data(), copy_bytes);
    }

    delete req;
    return ok;
}

bool ScanDiskController::write_cylinder(uint32_t cylinder, const void* in_buffer, size_t size) {
    if (cylinder >= total_cylinders_ || !in_buffer || size == 0) return false;

    pthread_mutex_lock(&queue_mutex_);
    uint32_t req_id = next_req_id_++;
    auto* req = new DiskRequest(req_id, cylinder, true, in_buffer, size);
    pending_queue_.push_back(req);
    pthread_cond_signal(&queue_cond_);
    pthread_mutex_unlock(&queue_mutex_);

    // Wait for elevator to service this cylinder
    pthread_mutex_lock(&req->done_mutex);
    while (!req->completed) {
        pthread_cond_wait(&req->done_cond, &req->done_mutex);
    }
    pthread_mutex_unlock(&req->done_mutex);

    bool ok = req->success;
    delete req;
    return ok;
}

size_t ScanDiskController::pending_requests_count() const {
    pthread_mutex_lock(&queue_mutex_);
    size_t sz = pending_queue_.size();
    pthread_mutex_unlock(&queue_mutex_);
    return sz;
}

void* ScanDiskController::disk_worker_thread(void* arg) {
    auto* self = static_cast<ScanDiskController*>(arg);
    self->run_controller();
    return nullptr;
}

void ScanDiskController::run_controller() {
    std::fstream disk_file;
    disk_file.open(backing_file_, std::ios::in | std::ios::out | std::ios::binary);

    while (true) {
        pthread_mutex_lock(&queue_mutex_);
        while (pending_queue_.empty() && running_) {
            pthread_cond_wait(&queue_cond_, &queue_mutex_);
        }

        if (!running_ && pending_queue_.empty()) {
            pthread_mutex_unlock(&queue_mutex_);
            break;
        }

        // Elevator SCAN Selection:
        // Find candidate in current direction.
        // If none, travel to boundary (199 or 0), reverse direction, and search.
        int best_idx = -1;
        uint32_t best_dist = 0xFFFFFFFF;

        if (direction_up_) {
            for (size_t i = 0; i < pending_queue_.size(); ++i) {
                uint32_t cyl = pending_queue_[i]->cylinder;
                if (cyl >= current_head_) {
                    uint32_t dist = cyl - current_head_;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_idx = static_cast<int>(i);
                    }
                }
            }

            if (best_idx < 0) {
                // Must reach boundary 199 before reversing!
                if (current_head_ < total_cylinders_ - 1) {
                    total_head_movement_ += (total_cylinders_ - 1 - current_head_);
                    current_head_ = total_cylinders_ - 1;
                }
                direction_up_ = false;

                // Pick closest in downward direction
                best_dist = 0xFFFFFFFF;
                for (size_t i = 0; i < pending_queue_.size(); ++i) {
                    uint32_t cyl = pending_queue_[i]->cylinder;
                    if (cyl <= current_head_) {
                        uint32_t dist = current_head_ - cyl;
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_idx = static_cast<int>(i);
                        }
                    }
                }
            }
        } else {
            // Direction DOWN
            for (size_t i = 0; i < pending_queue_.size(); ++i) {
                uint32_t cyl = pending_queue_[i]->cylinder;
                if (cyl <= current_head_) {
                    uint32_t dist = current_head_ - cyl;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_idx = static_cast<int>(i);
                    }
                }
            }

            if (best_idx < 0) {
                // Must reach boundary 0 before reversing!
                if (current_head_ > 0) {
                    total_head_movement_ += current_head_;
                    current_head_ = 0;
                }
                direction_up_ = true;

                // Pick closest in upward direction
                best_dist = 0xFFFFFFFF;
                for (size_t i = 0; i < pending_queue_.size(); ++i) {
                    uint32_t cyl = pending_queue_[i]->cylinder;
                    if (cyl >= current_head_) {
                        uint32_t dist = cyl - current_head_;
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_idx = static_cast<int>(i);
                        }
                    }
                }
            }
        }

        if (best_idx < 0) {
            pthread_mutex_unlock(&queue_mutex_);
            continue;
        }

        DiskRequest* req = pending_queue_[best_idx];
        pending_queue_.erase(pending_queue_.begin() + best_idx);

        // Track movement to target cylinder
        if (req->cylinder > current_head_) {
            total_head_movement_ += (req->cylinder - current_head_);
        } else {
            total_head_movement_ += (current_head_ - req->cylinder);
        }
        current_head_ = req->cylinder;

        pthread_mutex_unlock(&queue_mutex_);

        // Perform physical file seek & I/O
        bool io_ok = false;
        if (disk_file.is_open()) {
            std::streampos pos = static_cast<std::streampos>(req->cylinder) * DISK_SECTOR_SIZE;
            disk_file.seekp(pos);
            disk_file.seekg(pos);

            if (req->is_write) {
                disk_file.write(reinterpret_cast<const char*>(req->buffer.data()),
                                std::min(req->buffer.size(), static_cast<size_t>(DISK_SECTOR_SIZE)));
                disk_file.flush();
                io_ok = !disk_file.bad();
            } else {
                disk_file.read(reinterpret_cast<char*>(req->buffer.data()),
                               std::min(req->buffer.size(), static_cast<size_t>(DISK_SECTOR_SIZE)));
                io_ok = !disk_file.bad();
            }
        }

        pthread_mutex_lock(&req->done_mutex);
        req->success = io_ok;
        req->completed = true;
        pthread_cond_signal(&req->done_cond);
        pthread_mutex_unlock(&req->done_mutex);
    }

    if (disk_file.is_open()) {
        disk_file.close();
    }
}

ScanSimulationResult ScanDiskController::simulate_scan_vs_fcfs(
    uint32_t initial_head, bool initial_dir_up,
    const std::vector<uint32_t>& requests,
    uint32_t max_cylinder) {

    ScanSimulationResult res{};
    res.scan_serviced_order.reserve(requests.size());
    res.fcfs_serviced_order = requests;

    // 1. Calculate FCFS head movement
    uint32_t fcfs_head = initial_head;
    uint32_t fcfs_movement = 0;
    for (uint32_t cyl : requests) {
        fcfs_movement += (cyl >= fcfs_head) ? (cyl - fcfs_head) : (fcfs_head - cyl);
        fcfs_head = cyl;
    }
    res.total_head_movement_fcfs = fcfs_movement;

    // 2. Calculate SCAN (elevator) head movement
    // Rules:
    // Moves in initial direction to the boundary (max_cylinder if UP, 0 if DOWN),
    // servicing all requests encountered along the way in sorted order,
    // then reverses and services all remaining requests in opposite direction.
    uint32_t scan_head = initial_head;
    uint32_t scan_movement = 0;
    std::vector<uint32_t> pending = requests;

    if (initial_dir_up) {
        // Collect all cylinders >= initial_head
        std::vector<uint32_t> up_reqs;
        std::vector<uint32_t> down_reqs;
        for (uint32_t c : requests) {
            if (c >= initial_head) up_reqs.push_back(c);
            else down_reqs.push_back(c);
        }
        std::sort(up_reqs.begin(), up_reqs.end());
        std::sort(down_reqs.rbegin(), down_reqs.rend()); // descending order

        // Service up_reqs
        for (uint32_t c : up_reqs) {
            scan_movement += (c - scan_head);
            scan_head = c;
            res.scan_serviced_order.push_back(c);
        }

        // Elevator goes to the end boundary before reversing if down requests remain
        if (!down_reqs.empty()) {
            if (scan_head < max_cylinder) {
                scan_movement += (max_cylinder - scan_head);
                scan_head = max_cylinder;
            }
            // Reverse direction to DOWN
            for (uint32_t c : down_reqs) {
                scan_movement += (scan_head - c);
                scan_head = c;
                res.scan_serviced_order.push_back(c);
            }
        }
    } else {
        // Initial dir DOWN
        std::vector<uint32_t> down_reqs;
        std::vector<uint32_t> up_reqs;
        for (uint32_t c : requests) {
            if (c <= initial_head) down_reqs.push_back(c);
            else up_reqs.push_back(c);
        }
        std::sort(down_reqs.rbegin(), down_reqs.rend());
        std::sort(up_reqs.begin(), up_reqs.end());

        for (uint32_t c : down_reqs) {
            scan_movement += (scan_head - c);
            scan_head = c;
            res.scan_serviced_order.push_back(c);
        }

        if (!up_reqs.empty()) {
            if (scan_head > 0) {
                scan_movement += scan_head;
                scan_head = 0;
            }
            for (uint32_t c : up_reqs) {
                scan_movement += (c - scan_head);
                scan_head = c;
                res.scan_serviced_order.push_back(c);
            }
        }
    }

    res.total_head_movement_scan = scan_movement;
    if (fcfs_movement > 0) {
        res.improvement_percentage = 100.0 * (static_cast<double>(fcfs_movement - scan_movement) / fcfs_movement);
    } else {
        res.improvement_percentage = 0.0;
    }

    return res;
}
