#include "scheduler.h"
#include "buffer.h"
#include "swap_engine.h"
#include "memory_arena.h"
#include "logger.h"
#include "clock.h"
#include <algorithm>
#include <chrono>
#include <thread>

Scheduler::Scheduler(BoundedRingBuffer* ring_buffer, SwapEngine* swap_engine, const SystemConfig& config)
    : ring_buffer_(ring_buffer), swap_engine_(swap_engine), config_(config) {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&ready_cond_, nullptr);
}

Scheduler::~Scheduler() {
    stop();
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&ready_cond_);

    // Clean up allocated JCBs
    for (auto* job : all_jobs_) {
        delete job;
    }
    all_jobs_.clear();
}

bool Scheduler::start() {
    pthread_mutex_lock(&mutex_);
    if (running_) {
        pthread_mutex_unlock(&mutex_);
        return true;
    }
    running_ = true;
    int rc = pthread_create(&thread_, nullptr, dispatcher_thread, this);
    pthread_mutex_unlock(&mutex_);
    if (rc != 0) {
        LOG_E("SCHEDULER", "Failed to start dispatcher thread");
        return false;
    }
    LOG_I("SCHEDULER", "Hybrid SRTF & Priority Aging Dispatcher started");
    return true;
}

void Scheduler::stop() {
    pthread_mutex_lock(&mutex_);
    if (!running_) {
        pthread_mutex_unlock(&mutex_);
        return;
    }
    running_ = false;
    pthread_cond_broadcast(&ready_cond_);
    pthread_mutex_unlock(&mutex_);

    pthread_join(thread_, nullptr);
    LOG_I("SCHEDULER", "Dispatcher stopped");
}

void Scheduler::add_job(JobControlBlock* job) {
    if (!job) return;
    pthread_mutex_lock(&mutex_);
    all_jobs_.push_back(job);
    pthread_cond_signal(&ready_cond_);
    pthread_mutex_unlock(&mutex_);
}

double Scheduler::compute_score(const JobControlBlock* job, double current_time) const {
    if (!job) return 999999.0;

    // Remaining pages penalty (SRTF factor)
    double pages_penalty = config_.weight_pages * static_cast<double>(job->remaining_pages.load());

    // Role priority benefit
    double role_benefit = 0.0;
    switch (job->role) {
        case ROLE_FACULTY:    role_benefit = config_.role_weight_faculty; break;
        case ROLE_RESEARCHER: role_benefit = config_.role_weight_researcher; break;
        case ROLE_STUDENT:    role_benefit = config_.role_weight_student; break;
    }

    // Dynamic Starvation Aging Boost
    double wait_time = std::max(0.0, current_time - job->arrival_time);
    double aging_benefit = 0.0;
    if (wait_time > config_.aging_threshold_sec) {
        aging_benefit = config_.aging_rate * (wait_time - config_.aging_threshold_sec);
    }

    // Urgency Bonus
    double urgency_benefit = job->urgency ? config_.urgency_bonus : 0.0;

    // Combined score: lower score is dispatched first!
    return pages_penalty - role_benefit - aging_benefit - urgency_benefit;
}

JobControlBlock* Scheduler::pick_next_job() {
    double now = SimClock::instance().now();
    JobControlBlock* best_job = nullptr;
    double best_score = 1e9;

    for (auto* job : all_jobs_) {
        if (!job) continue;
        if (job->state == STATE_WAITING || job->state == STATE_SWAPPED_TO_DISK) {
            double score = compute_score(job, now);
            job->effective_score = score;
            if (score < best_score) {
                best_score = score;
                best_job = job;
            }
        }
    }
    return best_job;
}

void Scheduler::wake_printers() {
    pthread_mutex_lock(&mutex_);
    pthread_cond_broadcast(&ready_cond_);
    pthread_mutex_unlock(&mutex_);
}

JobControlBlock* Scheduler::acquire_job_for_printer(int worker_id, const std::atomic<bool>& keep_running) {
    pthread_mutex_lock(&mutex_);
    for (;;) {
        while (in_memory_queue_.empty() && running_ && keep_running) {
            pthread_cond_wait(&ready_cond_, &mutex_);
        }
        if (in_memory_queue_.empty() || !running_ || !keep_running) {
            pthread_mutex_unlock(&mutex_);
            return nullptr;
        }

        // Pick the best-scored resident job
        double now = SimClock::instance().now();
        auto best_it = in_memory_queue_.end();
        double best_score = 1e18;
        for (auto it = in_memory_queue_.begin(); it != in_memory_queue_.end(); ++it) {
            double score = compute_score(*it, now);
            if (score < best_score) {
                best_score = score;
                best_it = it;
            }
        }

        JobControlBlock* job = *best_it;
        in_memory_queue_.erase(best_it);

        // Claim under the lock: nobody else can pop, evict or re-dispatch this job anymore.
        // transition_state() is an atomic compare-and-set, so a stale entry is rejected here
        // instead of being printed twice.
        if (job->state.load() != STATE_IN_MEMORY || !transition_state(job, STATE_PRINTING, "printer worker claimed job")) {
            LOG_E("SCHEDULER", "Dropped stale ready-queue entry for Job #%u (state=%s)",
                  job->id, job_state_to_string(job->state.load()));
            continue;
        }
        job->assigned_printer_id = worker_id;
        pthread_mutex_unlock(&mutex_);
        return job;
    }
}

void Scheduler::on_job_completed(JobControlBlock* job) {
    if (!job) return;
    pthread_mutex_lock(&mutex_);
    if (swap_engine_) {
        swap_engine_->remove_job(job->id);
    }
    pthread_cond_broadcast(&ready_cond_);
    pthread_mutex_unlock(&mutex_);
    LOG_I("SCHEDULER", "Job #%u acknowledged completed. WaitTime=%.2fs, PrintDuration=%.2fs",
          job->id, job->total_wait_time, (job->completion_time - job->start_print_time));
}

void* Scheduler::dispatcher_thread(void* arg) {
    auto* self = static_cast<Scheduler*>(arg);
    self->run_dispatcher();
    return nullptr;
}

void Scheduler::run_dispatcher() {
    while (running_) {
        // 1. Ingest newly validated jobs from ring buffer
        if (ring_buffer_) {
            JobControlBlock* incoming = ring_buffer_->try_dequeue();
            while (incoming != nullptr) {
                pthread_mutex_lock(&mutex_);
                all_jobs_.push_back(incoming);
                pthread_mutex_unlock(&mutex_);
                incoming = ring_buffer_->try_dequeue();
            }
        }

        pthread_mutex_lock(&mutex_);
        JobControlBlock* candidate = pick_next_job();

        if (candidate) {
            const bool was_swapped = (candidate->state.load() == STATE_SWAPPED_TO_DISK);

            // Memory requirement (spool window). A swapped-out job keeps its original size.
            size_t needed_mem = was_swapped
                ? candidate->arena_size
                : std::min(static_cast<size_t>(candidate->remaining_pages.load()) * ARENA_MEM_PER_PAGE,
                           ARENA_MAX_JOB_MEM);
            if (!was_swapped) candidate->arena_size = needed_mem;

            bool placed = false;

            // Try to place the candidate (fresh allocation, or a real swap-in from disk)
            auto try_place = [&]() -> bool {
                if (!MemoryArena::instance().can_allocate(needed_mem)) return false;
                if (was_swapped) {
                    // Disk read goes through the SCAN controller: never hold the scheduler lock while waiting on I/O
                    pthread_mutex_unlock(&mutex_);
                    bool ok = swap_engine_ && swap_engine_->swap_in(candidate);
                    pthread_mutex_lock(&mutex_);
                    return ok;
                }
                int32_t off = MemoryArena::instance().allocate(needed_mem, candidate->id);
                if (off < 0) return false;
                candidate->arena_offset = off;
                transition_state(candidate, STATE_IN_MEMORY, "First-Fit memory allocated");
                return true;
            };

            placed = try_place();

            // RAM exhausted: evict the LRU idle job that has WORSE priority than the candidate.
            if (!placed && swap_engine_) {
                double now = SimClock::instance().now();
                double candidate_score = compute_score(candidate, now);

                // Only jobs waiting in the ready queue (resident, not yet claimed by a printer) are eligible
                std::vector<JobControlBlock*> eligible;
                for (auto* j : in_memory_queue_) {
                    if (j && j != candidate && j->state.load() == STATE_IN_MEMORY &&
                        j->assigned_printer_id.load() < 0 && compute_score(j, now) > candidate_score) {
                        eligible.push_back(j);
                    }
                }

                JobControlBlock* victim = swap_engine_->select_lru_victim(eligible);
                if (victim) {
                    // Take the victim OUT of the ready queue first: a printer can no longer claim it
                    // and it cannot be queued twice once it is swapped back in later.
                    in_memory_queue_.erase(std::find(in_memory_queue_.begin(), in_memory_queue_.end(), victim));

                    LOG_W("SCHEDULER", "RAM exhausted for Job #%u (score %.2f). Evicting LRU lower-priority Job #%u (score %.2f)...",
                          candidate->id, candidate_score, victim->id, compute_score(victim, now));

                    pthread_mutex_unlock(&mutex_);
                    bool swapped = swap_engine_->swap_out(victim);   // disk I/O outside the lock
                    pthread_mutex_lock(&mutex_);

                    if (swapped) {
                        placed = try_place();
                    } else {
                        in_memory_queue_.push_back(victim);          // eviction failed: victim stays resident
                    }
                }
            }

            if (placed) {
                if (swap_engine_) swap_engine_->record_access(candidate->id);
                in_memory_queue_.push_back(candidate);
                pthread_cond_broadcast(&ready_cond_);
                LOG_I("DISPATCHER", "Dispatched Job #%u (Role=%s, Pages=%u, Score=%.2f, ArenaOffset=%d)",
                      candidate->id, user_role_to_string(candidate->role),
                      candidate->remaining_pages.load(), candidate->effective_score, candidate->arena_offset.load());
            }
        }

        // Wait on condition or sleep between scheduling slices
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20000000; // 20ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&ready_cond_, &mutex_, &ts);
        pthread_mutex_unlock(&mutex_);
    }
}

std::vector<JobControlBlock*> Scheduler::get_all_jobs_snapshot() const {
    pthread_mutex_lock(&mutex_);
    std::vector<JobControlBlock*> snap = all_jobs_;
    pthread_mutex_unlock(&mutex_);
    return snap;
}

size_t Scheduler::ready_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = 0;
    for (auto* j : all_jobs_) {
        if (j && (j->state == STATE_WAITING || j->state == STATE_IN_MEMORY)) cnt++;
    }
    pthread_mutex_unlock(&mutex_);
    return cnt;
}

size_t Scheduler::in_memory_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = in_memory_queue_.size();
    pthread_mutex_unlock(&mutex_);
    return cnt;
}

size_t Scheduler::swapped_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = 0;
    for (auto* j : all_jobs_) {
        if (j && j->state == STATE_SWAPPED_TO_DISK) cnt++;
    }
    pthread_mutex_unlock(&mutex_);
    return cnt;
}

size_t Scheduler::total_jobs_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = all_jobs_.size();
    pthread_mutex_unlock(&mutex_);
    return cnt;
}

size_t Scheduler::unfinished_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = 0;
    for (auto* j : all_jobs_) {
        if (j && j->state.load() != STATE_COMPLETED && j->state.load() != STATE_REJECTED) cnt++;
    }
    pthread_mutex_unlock(&mutex_);
    return cnt;
}

size_t Scheduler::completed_count() const {
    pthread_mutex_lock(&mutex_);
    size_t cnt = 0;
    for (auto* j : all_jobs_) {
        if (j && j->state == STATE_COMPLETED) cnt++;
    }
    pthread_mutex_unlock(&mutex_);
    return cnt;
}
