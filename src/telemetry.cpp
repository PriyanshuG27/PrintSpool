#include "telemetry.h"
#include "buffer.h"
#include "scheduler.h"
#include "memory_arena.h"
#include "disk_scan.h"
#include "printer_pool.h"
#include "clock.h"
#include "logger.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>

TelemetryDashboard::TelemetryDashboard(BoundedRingBuffer* buffer,
                                       Scheduler* scheduler,
                                       MemoryArena* arena,
                                       ScanDiskController* disk,
                                       PrinterWorkerPool* pool)
    : buffer_(buffer),
      scheduler_(scheduler),
      arena_(arena),
      disk_(disk),
      pool_(pool) {
    pthread_mutex_init(&gantt_mutex_, nullptr);
}

TelemetryDashboard::~TelemetryDashboard() {
    stop();
    pthread_mutex_destroy(&gantt_mutex_);
}

bool TelemetryDashboard::start(double refresh_rate_hz) {
    if (refresh_rate_hz > 0.0) {
        refresh_interval_sec_ = 1.0 / refresh_rate_hz;
    }
    running_ = true;
    int rc = pthread_create(&thread_, nullptr, telemetry_thread, this);
    if (rc != 0) {
        LOG_E("TELEMETRY", "Failed to start telemetry dashboard thread");
        running_ = false;
        return false;
    }
    LOG_I("TELEMETRY", "Telemetry Dashboard started at %.1f Hz refresh rate", 1.0 / refresh_interval_sec_);
    return true;
}

void TelemetryDashboard::stop() {
    if (!running_.exchange(false)) return;
    pthread_join(thread_, nullptr);
    LOG_I("TELEMETRY", "Telemetry Dashboard stopped");
}

void* TelemetryDashboard::telemetry_thread(void* arg) {
    auto* self = static_cast<TelemetryDashboard*>(arg);
    self->run_dashboard();
    return nullptr;
}

void TelemetryDashboard::run_dashboard() {
    while (running_) {
        // Sleep between refreshes
        long long ms = static_cast<long long>(refresh_interval_sec_ * 1000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        if (!running_) break;

        // Render HUD outside of any subsystem lock!
        render_hud();
    }
}

void TelemetryDashboard::record_gantt_event(uint32_t job_id, UserRole role, JobState state,
                                           int printer_id, uint32_t pages_left) {
    pthread_mutex_lock(&gantt_mutex_);
    gantt_records_.push_back({SimClock::instance().now(), job_id, role, state, printer_id, pages_left});
    pthread_mutex_unlock(&gantt_mutex_);

    if (ipc_mode_) {
        std::printf("{\"type\":\"gantt\",\"time\":%.2f,\"job_id\":%u,\"role\":\"%s\",\"state\":\"%s\",\"printer_id\":%d,\"pages_left\":%u}\n",
                    SimClock::instance().now(), job_id, user_role_to_string(role), job_state_to_string(state), printer_id, pages_left);
        std::fflush(stdout);
    }
}

void TelemetryDashboard::render_json_frame(std::ostream& os) {
    size_t buf_size = buffer_ ? buffer_->size() : 0;
    size_t buf_cap = buffer_ ? buffer_->capacity() : RING_BUFFER_CAPACITY;
    auto buf_items = buffer_ ? buffer_->get_snapshot() : std::vector<JobControlBlock*>{};

    size_t ready_cnt = scheduler_ ? scheduler_->ready_count() : 0;
    size_t in_mem_cnt = scheduler_ ? scheduler_->in_memory_count() : 0;
    size_t swapped_cnt = scheduler_ ? scheduler_->swapped_count() : 0;
    size_t completed_cnt = scheduler_ ? scheduler_->completed_count() : 0;
    size_t unfinished_cnt = scheduler_ ? scheduler_->unfinished_count() : 0;
    size_t total_cnt = scheduler_ ? scheduler_->total_jobs_count() : 0;
    auto all_jobs = scheduler_ ? scheduler_->get_all_jobs_snapshot() : std::vector<JobControlBlock*>{};

    size_t arena_alloc = arena_ ? arena_->allocated_bytes() : 0;
    size_t arena_total = arena_ ? arena_->total_size() : ARENA_TOTAL_SIZE;
    size_t arena_free = arena_ ? arena_->free_bytes() : 0;
    double arena_ratio = arena_ ? arena_->occupancy_ratio() : 0.0;
    auto arena_blocks = arena_ ? arena_->get_snapshot() : std::vector<ArenaBlockSnapshot>{};

    uint32_t disk_head = disk_ ? disk_->current_head() : 0;
    bool disk_up = disk_ ? disk_->is_direction_up() : true;
    uint64_t disk_seeks = disk_ ? disk_->total_cylinders_traversed() : 0;
    size_t disk_pending = disk_ ? disk_->pending_requests_count() : 0;

    std::vector<PrinterWorkerStatus> workers = pool_ ? pool_->get_worker_statuses() : std::vector<PrinterWorkerStatus>{};
    uint32_t pages_printed = pool_ ? pool_->total_pages_printed() : 0;
    double sim_now = SimClock::instance().now();

    os << "{\"type\":\"telemetry\","
       << "\"sim_time\":" << std::fixed << std::setprecision(2) << sim_now << ","
       << "\"pages_printed\":" << pages_printed << ","
       << "\"completed_jobs\":" << completed_cnt << ","
       << "\"buffer\":{"
       << "\"size\":" << buf_size << ","
       << "\"capacity\":" << buf_cap << ","
       << "\"items\":[";
    for (size_t i = 0; i < buf_items.size(); ++i) {
        if (i > 0) os << ",";
        const auto* j = buf_items[i];
        if (!j) continue;
        os << "{\"id\":" << j->id
           << ",\"client\":\"" << j->client_id << "\""
           << ",\"role\":\"" << user_role_to_string(j->role) << "\""
           << ",\"pages\":" << j->total_pages
           << ",\"urgency\":" << (j->urgency ? 1 : 0)
           << ",\"duplex\":" << (j->is_duplex ? 1 : 0)
           << ",\"doc_name\":\"" << j->document_name << "\"}";
    }
    os << "]},"
       << "\"scheduler\":{"
       << "\"ready\":" << ready_cnt << ","
       << "\"in_memory\":" << in_mem_cnt << ","
       << "\"swapped\":" << swapped_cnt << ","
       << "\"completed\":" << completed_cnt << ","
       << "\"unfinished\":" << unfinished_cnt << ","
       << "\"total\":" << total_cnt << ","
       << "\"jobs\":[";
    for (size_t i = 0; i < all_jobs.size(); ++i) {
        if (i > 0) os << ",";
        const auto* j = all_jobs[i];
        if (!j) continue;
        double current_score = scheduler_ ? scheduler_->compute_score(j, sim_now) : j->effective_score;
        os << "{\"id\":" << j->id
           << ",\"client\":\"" << j->client_id << "\""
           << ",\"role\":\"" << user_role_to_string(j->role) << "\""
           << ",\"pages\":" << j->total_pages
           << ",\"remaining\":" << j->remaining_pages.load()
           << ",\"urgency\":" << (j->urgency ? 1 : 0)
           << ",\"duplex\":" << (j->is_duplex ? 1 : 0)
           << ",\"state\":\"" << job_state_to_string(j->state.load()) << "\""
           << ",\"state_id\":" << static_cast<int>(j->state.load())
           << ",\"arrival\":" << std::fixed << std::setprecision(2) << j->arrival_time
           << ",\"wait\":" << j->total_wait_time
           << ",\"start_print\":" << j->start_print_time
           << ",\"completion\":" << j->completion_time
           << ",\"arena_offset\":" << j->arena_offset.load()
           << ",\"arena_size\":" << j->arena_size
           << ",\"swap_cylinder\":" << j->swap_cylinder
           << ",\"score\":" << std::fixed << std::setprecision(2) << current_score
           << ",\"printer_id\":" << j->assigned_printer_id.load()
           << ",\"doc_name\":\"" << j->document_name << "\"}";
    }
    os << "]},"
       << "\"arena\":{"
       << "\"total_size\":" << arena_total << ","
       << "\"allocated_bytes\":" << arena_alloc << ","
       << "\"free_bytes\":" << arena_free << ","
       << "\"occupancy_ratio\":" << std::fixed << std::setprecision(4) << arena_ratio << ","
       << "\"blocks\":[";
    for (size_t i = 0; i < arena_blocks.size(); ++i) {
        if (i > 0) os << ",";
        const auto& b = arena_blocks[i];
        os << "{\"offset\":" << b.offset
           << ",\"size\":" << b.size
           << ",\"is_free\":" << (b.is_free ? 1 : 0)
           << ",\"job_id\":" << b.job_id << "}";
    }
    os << "]},"
       << "\"disk\":{"
       << "\"head\":" << disk_head << ","
       << "\"direction_up\":" << (disk_up ? "true" : "false") << ","
       << "\"total_seeks\":" << disk_seeks << ","
       << "\"pending_requests\":" << disk_pending << "},"
       << "\"printers\":[";
    for (size_t i = 0; i < workers.size(); ++i) {
        if (i > 0) os << ",";
        const auto& w = workers[i];
        os << "{\"id\":" << w.worker_id
           << ",\"busy\":" << (w.is_busy ? "true" : "false")
           << ",\"job_id\":" << w.active_job_id
           << ",\"current_page\":" << w.current_page
           << ",\"total_pages\":" << w.total_pages
           << ",\"role\":\"" << user_role_to_string(w.role) << "\""
           << ",\"duplex\":" << (w.is_duplex ? "true" : "false")
           << ",\"start_time\":" << std::fixed << std::setprecision(2) << w.start_time << "}";
    }
    os << "]}\n";
}

static void draw_progress_bar(std::ostream& os, double ratio, int width = 30) {
    ratio = std::max(0.0, std::min(1.0, ratio));
    int filled = static_cast<int>(ratio * width);
    os << "[";
    for (int i = 0; i < filled; ++i) os << "#";
    for (int i = filled; i < width; ++i) os << ".";
    os << "] " << std::fixed << std::setprecision(1) << (ratio * 100.0) << "%";
}

void TelemetryDashboard::render_hud() {
    // Collect non-blocking snapshots under short locks
    size_t buf_size = buffer_ ? buffer_->size() : 0;
    size_t buf_cap = buffer_ ? buffer_->capacity() : RING_BUFFER_CAPACITY;

    size_t ready_cnt = scheduler_ ? scheduler_->ready_count() : 0;
    size_t in_mem_cnt = scheduler_ ? scheduler_->in_memory_count() : 0;
    size_t swapped_cnt = scheduler_ ? scheduler_->swapped_count() : 0;
    size_t completed_cnt = scheduler_ ? scheduler_->completed_count() : 0;

    size_t arena_alloc = arena_ ? arena_->allocated_bytes() : 0;
    size_t arena_total = arena_ ? arena_->total_size() : ARENA_TOTAL_SIZE;
    double arena_ratio = arena_ ? arena_->occupancy_ratio() : 0.0;

    uint32_t disk_head = disk_ ? disk_->current_head() : 0;
    bool disk_up = disk_ ? disk_->is_direction_up() : true;
    uint64_t disk_seeks = disk_ ? disk_->total_cylinders_traversed() : 0;
    size_t disk_pending = disk_ ? disk_->pending_requests_count() : 0;

    std::vector<PrinterWorkerStatus> workers = pool_ ? pool_->get_worker_statuses() : std::vector<PrinterWorkerStatus>{};
    uint32_t pages_printed = pool_ ? pool_->total_pages_printed() : 0;
    double sim_now = SimClock::instance().now();

    // Render ANSI HUD Display
    std::cout << "\033[2J\033[H"; // Clear screen and home cursor
    std::cout << "==============================================================================\n";
    std::cout << "          SMART CAMPUS PRINT SPOOLER - LIVE TELEMETRY DASHBOARD               \n";
    std::cout << "==============================================================================\n";
    std::cout << " Sim Clock: " << std::fixed << std::setprecision(2) << sim_now << " s"
              << " | Total Pages Printed: " << pages_printed
              << " | Completed Jobs: " << completed_cnt << "\n";
    std::cout << "------------------------------------------------------------------------------\n";

    // 1. Ring Buffer
    std::cout << " [M2 Bounded Buffer] Occupancy (" << buf_size << "/" << buf_cap << "): ";
    draw_progress_bar(std::cout, static_cast<double>(buf_size) / buf_cap, 24);
    std::cout << "\n";

    // 2. Scheduler Queues
    std::cout << " [M3 Dispatcher]     Ready Pool: " << ready_cnt
              << " | In RAM: " << in_mem_cnt
              << " | Swapped: " << swapped_cnt
              << " | Completed: " << completed_cnt << "\n";

    // 3. Memory Arena Occupancy
    std::cout << " [M5 64 KB Arena]    Usage (" << arena_alloc << "/" << arena_total << " B): ";
    draw_progress_bar(std::cout, arena_ratio, 24);
    std::cout << "\n";

    // 4. SCAN Disk Controller Slider
    std::cout << " [M7 SCAN Disk]      Cylinder: [";
    int slider_width = 32;
    int head_pos = static_cast<int>((static_cast<double>(disk_head) / (DISK_TOTAL_CYLINDERS - 1)) * (slider_width - 1));
    for (int i = 0; i < slider_width; ++i) {
        if (i == head_pos) std::cout << "*";
        else std::cout << "-";
    }
    std::cout << "] " << disk_head << "/199"
              << " (" << (disk_up ? "UP ▲" : "DOWN ▼") << ")"
              << " | Seeks: " << disk_seeks
              << " | Queue: " << disk_pending << "\n";

    // 5. Printer Worker Pool Status
    std::cout << "------------------------------------------------------------------------------\n";
    std::cout << " [M4 Printer Worker Pool (" << workers.size() << " Hardware Printers)]:\n";
    for (const auto& w : workers) {
        std::cout << "   Printer #" << w.worker_id << ": ";
        if (w.is_busy) {
            uint32_t shown_page = std::min(w.current_page, w.total_pages);   // never display e.g. 271/22
            double page_ratio = (w.total_pages > 0) ? (static_cast<double>(shown_page) / w.total_pages) : 0.0;
            std::cout << "BUSY  | Job #" << w.active_job_id
                      << " (" << user_role_to_string(w.role) << ", " << (w.is_duplex ? "Duplex" : "Simplex") << ") ";
            draw_progress_bar(std::cout, page_ratio, 16);
            std::cout << " (" << shown_page << "/" << w.total_pages << " p)\n";
        } else {
            std::cout << "IDLE  | Waiting for dispatched jobs...\n";
        }
    }
    std::cout << "==============================================================================\n";
    std::cout << std::flush;
}

void TelemetryDashboard::dump_gantt_csv(const std::string& filename) const {
    pthread_mutex_lock(&gantt_mutex_);
    std::ofstream ofs(filename);
    if (!ofs.is_open()) {
        pthread_mutex_unlock(&gantt_mutex_);
        return;
    }
    ofs << "timestamp,job_id,role,state,printer_id,pages_left\n";
    for (const auto& r : gantt_records_) {
        ofs << std::fixed << std::setprecision(3) << r.timestamp << ","
            << r.job_id << ","
            << user_role_to_string(r.role) << ","
            << job_state_to_string(r.state) << ","
            << r.printer_id << ","
            << r.pages_left << "\n";
    }
    ofs.close();
    pthread_mutex_unlock(&gantt_mutex_);
    LOG_I("TELEMETRY", "Gantt timeline saved to %s", filename.c_str());
}

void TelemetryDashboard::print_summary_report() const {
    std::cout << "\n=== EXECUTION SUMMARY REPORT ===\n";
    if (scheduler_) {
        auto jobs = scheduler_->get_all_jobs_snapshot();
        std::cout << "Total Submitted Jobs: " << jobs.size() << "\n";
        std::cout << std::left << std::setw(8) << "JobID"
                  << std::setw(12) << "Role"
                  << std::setw(8) << "Pages"
                  << std::setw(12) << "Arrived(s)"
                  << std::setw(12) << "Wait(s)"
                  << std::setw(14) << "Completed(s)"
                  << std::setw(14) << "Final State" << "\n";
        std::cout << "----------------------------------------------------------------------\n";
        for (const auto* j : jobs) {
            if (!j) continue;
            std::cout << std::left << std::setw(8) << j->id
                      << std::setw(12) << user_role_to_string(j->role)
                      << std::setw(8) << j->total_pages
                      << std::setw(12) << std::fixed << std::setprecision(2) << j->arrival_time
                      << std::setw(12) << j->total_wait_time
                      << std::setw(14) << j->completion_time
                      << std::setw(14) << job_state_to_string(j->state.load()) << "\n";
        }
    }
    if (disk_) {
        std::cout << "Total Disk Head Movement: " << disk_->total_cylinders_traversed() << " cylinders\n";
    }
    if (pool_) {
        std::cout << "Total Pages Printed: " << pool_->total_pages_printed() << "\n";
    }
    std::cout << "================================\n";
}
