#include "scheduler.h"
#include "buffer.h"
#include "swap_engine.h"
#include "memory_arena.h"
#include "disk_scan.h"
#include "printer_pool.h"
#include "clock.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

// ============================================================================
// TC-05: Swap-path integrity (regression for the double-claim / runaway-page bug)
// ----------------------------------------------------------------------------
// Fills the arena with idle jobs, injects an urgent job that forces an LRU
// eviction of a job that is still queued for printing, then starts the printers.
// Invariants checked:
//   * every job completes exactly once with remaining_pages == 0
//   * pages printed == sum of job pages (no job printed twice / counter wrap)
//   * zero illegal state transitions
//   * arena fully released, no swap cylinders leaked, arena consistent
// ============================================================================
static JobControlBlock* make_job(uint32_t id, UserRole role, uint32_t pages, bool urgent) {
    auto* j = new JobControlBlock();
    j->id = id;
    j->client_id = "tc05_" + std::to_string(id);
    j->role = role;
    j->total_pages = pages;
    j->remaining_pages = pages;
    j->urgency = urgent;
    j->arrival_time = SimClock::instance().now();
    j->state = STATE_WAITING;
    return j;
}

static bool wait_until(bool (*pred)(void*), void* ctx, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (!pred(ctx)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << " TC-05: Swap-Path Integrity (no double-claim, no page overrun)\n";
    std::cout << "=================================================================\n";

    SimClock::instance().reset();
    SimClock::instance().set_mode(true, 1.0);
    Logger::instance().enable_console(false);

    SystemConfig config;
    config.printer_workers = 3;
    MemoryArena::instance().init(config.arena_size);

    ScanDiskController disk("swap_tc05.img", config.cylinder_count);
    disk.start();
    SwapEngine swap_engine(&disk);
    BoundedRingBuffer ring(config.buffer_capacity);
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
        if (!ok) ++failures;
    };

    uint32_t total_pages = 0;
    {
        Scheduler scheduler(&ring, &swap_engine, config);
        scheduler.start();

        // Advance simulated time in the background (deterministic virtual clock)
        std::atomic<bool> tick(true);
        std::thread clk([&] {
            while (tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                SimClock::instance().advance_seconds(0.02);
            }
        });

        // 1. Three idle 16-page student jobs fill ~75% of the arena (no printers yet)
        std::vector<JobControlBlock*> jobs;
        for (uint32_t i = 1; i <= 3; ++i) { jobs.push_back(make_job(i, ROLE_STUDENT, 16, false)); }
        for (auto* j : jobs) { total_pages += j->total_pages; scheduler.add_job(j); }
        struct Ctx { Scheduler* s; } ctx{&scheduler};
        bool resident = wait_until([](void* c) { return static_cast<Ctx*>(c)->s->in_memory_count() == 3; }, &ctx, 5000);
        check(resident, "3 idle jobs resident in RAM before the urgent job arrives");

        // 2. Urgent faculty job cannot fit -> dispatcher must evict an idle, still-queued job
        auto* urgent = make_job(4, ROLE_FACULTY, 16, true);
        total_pages += urgent->total_pages;
        scheduler.add_job(urgent);
        bool evicted = wait_until([](void* c) { return static_cast<Ctx*>(c)->s->swapped_count() >= 1; }, &ctx, 5000);
        check(evicted, "urgent job forced an LRU swap-out");
        jobs.push_back(urgent);

        // 3. Start printers; everything must drain correctly
        PrinterWorkerPool pool(&scheduler, config.printer_workers, config);
        pool.start();
        bool drained = wait_until([](void* c) { return static_cast<Ctx*>(c)->s->completed_count() == 4; }, &ctx, 20000);
        check(drained, "all 4 jobs reached COMPLETED");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let any phantom duplicates surface

        pool.stop();       // clock must keep ticking until workers can exit
        scheduler.stop();
        tick = false;
        clk.join();

        for (auto* j : jobs) {
            if (j->state != STATE_COMPLETED || j->remaining_pages != 0) {
                std::cout << "    Job #" << j->id << " state=" << job_state_to_string(j->state)
                          << " remaining=" << j->remaining_pages << "\n";
                ++failures;
            }
        }
        check(pool.total_pages_printed() == total_pages, "pages printed == sum of job pages (no double printing)");
        std::cout << "    printed=" << pool.total_pages_printed() << " expected=" << total_pages << "\n";
        check(illegal_transition_count() == 0, "zero illegal state transitions");
        check(MemoryArena::instance().allocated_bytes() == 0, "arena fully released");
        check(swap_engine.active_swapped_count() == 0, "no swap cylinders leaked");
        check(MemoryArena::instance().verify_consistency(), "arena consistent");
    }
    disk.stop();

    if (failures == 0) {
        std::cout << "[PASS] TC-05 Verified: swap path is race-free and every job prints exactly once\n";
        return 0;
    }
    std::cout << "[FAIL] TC-05: " << failures << " invariant violation(s)\n";
    return 1;
}
