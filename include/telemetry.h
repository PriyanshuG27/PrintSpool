#pragma once

#include "common.h"
#include <vector>
#include <string>
#include <atomic>
#include <pthread.h>

class BoundedRingBuffer;
class Scheduler;
class MemoryArena;
class ScanDiskController;
class PrinterWorkerPool;

// ============================================================================
// Module 8: Telemetry, Gantt & ASCII HUD Dashboard
// ============================================================================
// Takes non-blocking snapshots of system state under short locks / rwlocks
// and renders an informative ASCII HUD at >= 2 Hz.
// Also collects Gantt timeline logs.
// ============================================================================

struct GanttRecord {
    double timestamp;
    uint32_t job_id;
    UserRole role;
    JobState state;
    int printer_id;
    uint32_t pages_left;
};

class TelemetryDashboard {
public:
    TelemetryDashboard(BoundedRingBuffer* buffer,
                       Scheduler* scheduler,
                       MemoryArena* arena,
                       ScanDiskController* disk,
                       PrinterWorkerPool* pool);
    ~TelemetryDashboard();

    bool start(double refresh_rate_hz = 4.0);
    void stop();

    void record_gantt_event(uint32_t job_id, UserRole role, JobState state, int printer_id, uint32_t pages_left);
    void render_hud();
    void render_json_frame(std::ostream& os);
    void set_ipc_mode(bool ipc) { ipc_mode_ = ipc; }
    void dump_gantt_csv(const std::string& filename) const;
    void print_summary_report() const;

private:
    static void* telemetry_thread(void* arg);
    void run_dashboard();

    BoundedRingBuffer* buffer_;
    Scheduler* scheduler_;
    MemoryArena* arena_;
    ScanDiskController* disk_;
    PrinterWorkerPool* pool_;

    double refresh_interval_sec_ = 0.25; // 4 Hz
    std::atomic<bool> running_{false};
    bool ipc_mode_ = false;
    pthread_t thread_;

    std::vector<GanttRecord> gantt_records_;
    mutable pthread_mutex_t gantt_mutex_;
};
