#include "memory_arena.h"
#include "swap_engine.h"
#include "disk_scan.h"
#include "clock.h"
#include <iostream>
#include <vector>
#include <cassert>

int main() {
    std::cout << "=================================================================\n";
    std::cout << " TC-03: First-Fit Arena Allocation & Swap Triggering Test\n";
    std::cout << "=================================================================\n";

    MemoryArena& arena = MemoryArena::instance();
    arena.init(ARENA_TOTAL_SIZE);
    assert(arena.verify_consistency());

    ScanDiskController disk("swap_tc03.img", DISK_TOTAL_CYLINDERS);
    disk.start();
    SwapEngine swap_engine(&disk);

    std::cout << "Initial Arena Free: " << arena.free_bytes() << " / " << arena.total_size() << " B\n";

    // 1. Fill arena to approximately 92% occupancy (~60,000 bytes)
    // We allocate 6 jobs of ~10,000 bytes each
    std::vector<JobControlBlock*> active_jobs;
    size_t chunk_size = 9800; // ~10 KB payload per job
    for (uint32_t i = 1; i <= 6; ++i) {
        auto* job = new JobControlBlock();
        job->id = i;
        job->client_id = "user_" + std::to_string(i);
        job->role = (i == 1) ? ROLE_STUDENT : ROLE_RESEARCHER;
        job->total_pages = 10;
        job->remaining_pages = 10;
        job->arena_size = chunk_size;

        int32_t offset = arena.allocate(chunk_size, job->id);
        assert(offset >= 0);
        job->arena_offset = offset;
        job->state = STATE_IN_MEMORY;
        swap_engine.record_access(job->id);

        active_jobs.push_back(job);
        assert(arena.verify_consistency());
    }

    double initial_occupancy = arena.occupancy_ratio() * 100.0;
    std::cout << "Occupancy after initial filling: " << initial_occupancy
              << "% (" << arena.allocated_bytes() << " B allocated, "
              << arena.free_bytes() << " B free)\n";
    assert(initial_occupancy >= 88.0 && initial_occupancy <= 96.0);

    // 2. Submit a further job requiring 16 KB (16,384 bytes)
    auto* big_job = new JobControlBlock();
    big_job->id = 99;
    big_job->role = ROLE_FACULTY;
    big_job->total_pages = 16;
    big_job->remaining_pages = 16;
    size_t big_job_bytes = 16384;
    big_job->arena_size = big_job_bytes;

    std::cout << "Submitting Job #99 requiring " << big_job_bytes << " B (16 KB)...\n";

    // Attempt direct allocation - must fail due to memory exhaustion
    int32_t alloc_offset = arena.allocate(big_job_bytes, big_job->id);
    std::cout << "Direct allocation result: " << alloc_offset << " (expected: -1)\n";
    assert(alloc_offset == -1);

    // 3. Trigger LRU Swap Out
    // We need to free ~16 KB, which requires evicting 2 victim jobs of ~10 KB each (~20 KB reclaimed)
    size_t bytes_reclaimed = 0;
    size_t initial_free = arena.free_bytes();

    while (alloc_offset < 0) {
        JobControlBlock* victim = swap_engine.select_lru_victim(active_jobs);
        assert(victim != nullptr);
        std::cout << "LRU Victim selected: Job #" << victim->id << " (Arena offset: " << victim->arena_offset << ")\n";

        bool swapped = swap_engine.swap_out(victim);
        (void)swapped;
        assert(swapped);
        assert(victim->state == STATE_SWAPPED_TO_DISK);
        assert(victim->swap_cylinder >= 0);
        assert(arena.verify_consistency());

        alloc_offset = arena.allocate(big_job_bytes, big_job->id);
    }

    bytes_reclaimed = arena.free_bytes() + big_job_bytes - initial_free;
    std::cout << "Reclaimed space via LRU swap-out: ~" << (bytes_reclaimed / 1024) << " KB\n";
    assert(alloc_offset >= 0);
    big_job->arena_offset = alloc_offset;
    big_job->state = STATE_IN_MEMORY;
    swap_engine.record_access(big_job->id);

    std::cout << "Job #99 successfully allocated in arena at offset: " << alloc_offset << "\n";
    assert(arena.verify_consistency());

    // 4. Test Swap-In: Free big job, then restore swapped victim back into arena
    arena.deallocate(big_job->arena_offset, big_job->id);
    assert(arena.verify_consistency());

    for (auto* j : active_jobs) {
        if (j->state == STATE_SWAPPED_TO_DISK) {
            bool in_ok = swap_engine.swap_in(j);
            (void)in_ok;
            assert(in_ok);
            assert(j->state == STATE_IN_MEMORY);
            assert(j->arena_offset >= 0);
            assert(arena.verify_consistency());
            std::cout << "Successfully restored Job #" << j->id << " back from swap to offset " << j->arena_offset << "\n";
        }
        delete j;
    }
    delete big_job;

    disk.stop();
    std::cout << "[PASS] TC-03 Verified: 92% arena full -> 16 KB request triggered swap-out reclaiming ~20 KB -> allocation succeeded!\n";
    return 0;
}
