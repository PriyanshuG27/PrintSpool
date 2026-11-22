#include "printer_pool.h"
#include "scheduler.h"
#include "telemetry.h"
#include "memory_arena.h"
#include "logger.h"
#include "clock.h"
#include <algorithm>
#include <chrono>
#include <thread>

PrinterWorkerPool::PrinterWorkerPool(Scheduler* scheduler, int num_printers, const SystemConfig& config)
    : scheduler_(scheduler),
      num_printers_(num_printers > 0 ? num_printers : DEFAULT_PRINTER_WORKERS),
      config_(config),
      statuses_(num_printers_) {
    pthread_mutex_init(&status_mutex_, nullptr);
    for (int i = 0; i < num_printers_; ++i) {
        statuses_[i].worker_id = i;
        statuses_[i].is_busy = false;
        statuses_[i].active_job_id = 0;
        statuses_[i].current_page = 0;
        statuses_[i].total_pages = 0;
        statuses_[i].role = ROLE_STUDENT;
        statuses_[i].is_duplex = false;
        statuses_[i].start_time = 0.0;
    }
}

PrinterWorkerPool::~PrinterWorkerPool() {
    stop();
    pthread_mutex_destroy(&status_mutex_);
}

bool PrinterWorkerPool::start() {
    pthread_mutex_lock(&status_mutex_);
    if (running_) {
        pthread_mutex_unlock(&status_mutex_);
        return true;
    }
    running_ = true;
    threads_.resize(num_printers_);
    thread_args_.resize(num_printers_);

    for (int i = 0; i < num_printers_; ++i) {
        thread_args_[i].pool = this;
        thread_args_[i].worker_id = i;
        pthread_create(&threads_[i], nullptr, worker_routine, &thread_args_[i]);
    }
    pthread_mutex_unlock(&status_mutex_);
    LOG_I("PRINTER_POOL", "Started %d printer worker threads", num_printers_);
    return true;
}

void PrinterWorkerPool::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // Workers wait on the scheduler's condition variable; wake them so they observe running_ == false
    if (scheduler_) {
        scheduler_->wake_printers();
    }

    for (int i = 0; i < num_printers_; ++i) {
        pthread_join(threads_[i], nullptr);
    }
    LOG_I("PRINTER_POOL", "All printer worker threads terminated");
}

void* PrinterWorkerPool::worker_routine(void* arg) {
    auto* args = static_cast<WorkerThreadArgs*>(arg);
    args->pool->run_worker(args->worker_id);
    return nullptr;
}

void PrinterWorkerPool::run_worker(int worker_id) {
    while (running_) {
        // The scheduler hands the job over already claimed (removed from the ready queue and
        // moved IN_MEMORY -> PRINTING under its lock), so exactly one printer ever owns a job.
        JobControlBlock* job = scheduler_->acquire_job_for_printer(worker_id, running_);
        if (!job) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const uint32_t total_pages = job->total_pages;

        pthread_mutex_lock(&status_mutex_);
        statuses_[worker_id].is_busy = true;
        statuses_[worker_id].active_job_id = job->id;
        statuses_[worker_id].total_pages = total_pages;
        statuses_[worker_id].current_page = total_pages - std::min(total_pages, job->remaining_pages.load());
        statuses_[worker_id].role = job->role;
        statuses_[worker_id].is_duplex = job->is_duplex;
        statuses_[worker_id].start_time = SimClock::instance().now();
        pthread_mutex_unlock(&status_mutex_);

        if (telemetry_) {
            telemetry_->record_gantt_event(job->id, job->role, STATE_PRINTING, worker_id, job->remaining_pages.load());
        }

        LOG_I("PRINTER_WORKER", "[Printer %d] Started printing Job #%u (%s, %u pages, Duplex=%d)",
              worker_id, job->id, user_role_to_string(job->role), total_pages, job->is_duplex);

        // Page-by-page printing simulation. This worker is the sole owner of the job, and the
        // counter is only ever decremented while it is > 0, so it can neither wrap nor overrun.
        // A job that has started is always finished (never marked COMPLETED half-printed).
        uint32_t remaining = job->remaining_pages.load();
        while (remaining > 0) {
            SimClock::instance().sleep_simulated(config_.page_print_time_sec);

            // Duplex paper flip penalty on alternating pages
            if (job->is_duplex && ((total_pages - remaining) % 2 == 1)) {
                SimClock::instance().sleep_simulated(config_.duplex_flip_penalty_sec);
            }

            --remaining;
            job->remaining_pages.store(remaining);
            total_pages_printed_.fetch_add(1);

            const uint32_t printed = total_pages - remaining;
            pthread_mutex_lock(&status_mutex_);
            statuses_[worker_id].current_page = printed;
            pthread_mutex_unlock(&status_mutex_);

            if (telemetry_) {
                telemetry_->record_gantt_event(job->id, job->role, STATE_PRINTING, worker_id, remaining);
            }

            LOG_D("PRINTER_WORKER", "[Printer %d] Job #%u page printed (%u/%u)",
                  worker_id, job->id, printed, total_pages);
        }

        // Job completion & cleanup
        transition_state(job, STATE_COMPLETED, "all pages printed");
        if (telemetry_) {
            telemetry_->record_gantt_event(job->id, job->role, STATE_COMPLETED, worker_id, 0);
        }

        // Deallocate First-Fit arena block and return RAM
        int32_t offset = job->arena_offset.exchange(-1);
        if (offset >= 0) {
            MemoryArena::instance().deallocate(offset, job->id);
        }
        job->assigned_printer_id = -1;

        pthread_mutex_lock(&status_mutex_);
        statuses_[worker_id].is_busy = false;
        statuses_[worker_id].active_job_id = 0;
        statuses_[worker_id].current_page = 0;
        statuses_[worker_id].total_pages = 0;
        pthread_mutex_unlock(&status_mutex_);

        LOG_I("PRINTER_WORKER", "[Printer %d] Completed Job #%u successfully", worker_id, job->id);

        scheduler_->on_job_completed(job);
    }
}

std::vector<PrinterWorkerStatus> PrinterWorkerPool::get_worker_statuses() const {
    pthread_mutex_lock(&status_mutex_);
    std::vector<PrinterWorkerStatus> s = statuses_;
    pthread_mutex_unlock(&status_mutex_);
    return s;
}
