#include "scheduler.h"
#include "buffer.h"
#include "swap_engine.h"
#include "memory_arena.h"
#include "disk_scan.h"
#include "clock.h"
#include "logger.h"
#include <iostream>
#include <cassert>
#include <cmath>

int main() {
    std::cout << "========================================================\n";
    std::cout << " TC-01: Starvation Elimination via Priority Aging Test\n";
    std::cout << "========================================================\n";

    // Configure deterministic virtual clock
    SimClock::instance().reset();
    SimClock::instance().set_mode(true, 1.0);
    Logger::instance().enable_console(false);

    SystemConfig config;
    config.weight_pages = 1.0;
    config.role_weight_faculty = 16.0;
    config.role_weight_student = 0.0;
    config.aging_threshold_sec = 4.0;
    // Tuned so the faculty job's score first drops strictly below a 1-page student job's score
    // (1.0) at T = 11.4 s:  64.0 - aging_rate * (11.4 - 4.0) < 1.0  =>  aging_rate > 63.0 / 7.4 = 8.5135...
    // (the exact value 8.5135135 lands precisely on 1.0 at T = 11.4 and the strict '<' misses it by
    // one 0.1 s step, so a hair more is used).  At T = 11.3 the score is still ~1.8 (> 1.0).
    config.aging_rate = 8.52;

    MemoryArena::instance().init(config.arena_size);
    ScanDiskController disk("swap_tc01.img", config.cylinder_count);
    disk.start();
    SwapEngine swap_engine(&disk);
    BoundedRingBuffer ring_buffer(100);

    Scheduler scheduler(&ring_buffer, &swap_engine, config);

    // 1. Submit 80-page Faculty Job at T = 0.0 s
    auto* faculty_job = new JobControlBlock();
    faculty_job->id = 1;
    faculty_job->client_id = "prof_sharma";
    faculty_job->role = ROLE_FACULTY;
    faculty_job->total_pages = 80;
    faculty_job->remaining_pages = 80;
    faculty_job->arrival_time = 0.0;
    faculty_job->state = STATE_WAITING;
    scheduler.add_job(faculty_job);

    std::cout << "Submitted 80-page Faculty Job #1 at T = 0.00s\n";

    // 2. Simulate 20 one-page student jobs arriving at 1-second intervals (T = 0, 1, 2, ..., 20s)
    // Each 1-page student job is serviced in its 1-second window and completes.
    // The faculty job waits.
    double dispatch_time = -1.0;
    JobControlBlock* current_student = nullptr;
    uint32_t student_counter = 0;

    std::cout << "Simulating time steps in 0.1s increments under continuous student load...\n";

    for (double t = 0.0; t <= 20.01; t += 0.1) {
        SimClock::instance().advance_seconds(0.1);
        double current_time = SimClock::instance().now();

        // Every 1.0 second, a new student job arrives, and previous student job completes
        if (std::fmod(current_time + 1e-4, 1.0) < 0.15 && student_counter < 20) {
            if (current_student && current_student->state == STATE_WAITING) {
                current_student->state = STATE_COMPLETED; // Previous student completed
            }
            student_counter++;
            current_student = new JobControlBlock();
            current_student->id = 100 + student_counter;
            current_student->client_id = "student_" + std::to_string(student_counter);
            current_student->role = ROLE_STUDENT;
            current_student->total_pages = 1;
            current_student->remaining_pages = 1;
            current_student->arrival_time = current_time;
            current_student->state = STATE_WAITING;
            scheduler.add_job(current_student);
        }

        // Evaluate dispatch selection
        JobControlBlock* best = scheduler.pick_next_job();
        if (best && best->id == faculty_job->id && dispatch_time < 0.0) {
            dispatch_time = current_time;
            std::cout << ">>> Faculty Job #1 beat competing student job and dispatched at T = "
                      << current_time << "s (Score: " << best->effective_score << ") <<<\n";
            break;
        }
    }

    std::cout << "Observed Dispatch Time for 80-page Faculty Job: T = " << dispatch_time << " s\n";

    // Assert that dispatch time lands at 11.4 s
    if (dispatch_time < 11.2 || dispatch_time > 11.6) {
        std::cerr << "Assertion failed: dispatch_time=" << dispatch_time << " not in [11.2, 11.6]!\n";
        return 1;
    }
    std::cout << "[PASS] TC-01 Verified: Starvation avoided! Faculty job dispatched at T = "
              << dispatch_time << " s (expected ~11.4 s)\n";

    disk.stop();
    return 0;
}
