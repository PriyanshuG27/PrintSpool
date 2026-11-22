#pragma once

#include "common.h"
#include <vector>
#include <atomic>
#include <pthread.h>

class Scheduler;
class TelemetryDashboard;

// ============================================================================
// Module 4: Emulated Hardware Printer Worker Pool
// ============================================================================
// Runs N printer threads that simulate page rendering, duplex flip delays,
// progress tracking, and memory release upon completion.
// ============================================================================

struct PrinterWorkerStatus {
    int worker_id;
    bool is_busy;
    uint32_t active_job_id;
    uint32_t current_page;
    uint32_t total_pages;
    UserRole role;
    bool is_duplex;
    double start_time;
};

class PrinterWorkerPool {
public:
    PrinterWorkerPool(Scheduler* scheduler, int num_printers = DEFAULT_PRINTER_WORKERS,
                      const SystemConfig& config = SystemConfig{});
    ~PrinterWorkerPool();

    bool start();
    void stop();
    void set_telemetry(TelemetryDashboard* telemetry) { telemetry_ = telemetry; }

    // Query status of each printer worker for telemetry
    std::vector<PrinterWorkerStatus> get_worker_statuses() const;
    uint32_t total_pages_printed() const { return total_pages_printed_.load(); }

private:
    struct WorkerThreadArgs {
        PrinterWorkerPool* pool;
        int worker_id;
    };

    static void* worker_routine(void* arg);
    void run_worker(int worker_id);

    Scheduler* scheduler_;
    TelemetryDashboard* telemetry_ = nullptr;
    int num_printers_;
    SystemConfig config_;
    std::atomic<bool> running_{false};

    std::vector<pthread_t> threads_;
    std::vector<WorkerThreadArgs> thread_args_;
    std::vector<PrinterWorkerStatus> statuses_;

    std::atomic<uint32_t> total_pages_printed_{0};
    mutable pthread_mutex_t status_mutex_;
};
