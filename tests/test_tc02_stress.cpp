#include "buffer.h"
#include <iostream>
#include <vector>
#include <atomic>
#include <cassert>

constexpr int NUM_PRODUCERS = 10;
constexpr int JOBS_PER_PRODUCER = 50;
constexpr int TOTAL_JOBS = NUM_PRODUCERS * JOBS_PER_PRODUCER; // 500 jobs
constexpr int BUFFER_CAPACITY = 16;
constexpr int NUM_CONSUMERS = 3;

std::atomic<int> g_consumed_count{0};
std::atomic<bool> g_producers_done{false};

struct ProducerArgs {
    BoundedRingBuffer* buffer;
    int producer_id;
};

void* producer_routine(void* arg) {
    auto* args = static_cast<ProducerArgs*>(arg);
    for (int i = 0; i < JOBS_PER_PRODUCER; ++i) {
        uint32_t job_id = args->producer_id * 1000 + i;
        auto* job = new JobControlBlock();
        job->id = job_id;
        job->role = ROLE_STUDENT;
        job->total_pages = 5;
        job->remaining_pages = 5;
        job->state = STATE_WAITING;

        // Will block when buffer reaches capacity 16
        args->buffer->enqueue(job);
    }
    return nullptr;
}

void* consumer_routine(void* arg) {
    auto* buffer = static_cast<BoundedRingBuffer*>(arg);
    while (true) {
        JobControlBlock* job = buffer->dequeue();
        if (!job) {
            if (buffer->is_shutdown()) break;
            continue;
        }
        g_consumed_count++;
        delete job;

        if (g_consumed_count >= TOTAL_JOBS) {
            buffer->shutdown();
            break;
        }
    }
    return nullptr;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << " TC-02: Bounded Buffer Concurrency & Deadlock Stress Test\n";
    std::cout << "=================================================================\n";
    std::cout << "Producers: " << NUM_PRODUCERS << " (Submitting " << JOBS_PER_PRODUCER << " jobs each)\n";
    std::cout << "Buffer Capacity: " << BUFFER_CAPACITY << " slots\n";
    std::cout << "Consumers: " << NUM_CONSUMERS << " consumer threads\n";
    std::cout << "Total Expected Jobs: " << TOTAL_JOBS << "\n\n";

    BoundedRingBuffer buffer(BUFFER_CAPACITY);

    pthread_t consumers[NUM_CONSUMERS];
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pthread_create(&consumers[i], nullptr, consumer_routine, &buffer);
    }

    pthread_t producers[NUM_PRODUCERS];
    ProducerArgs prod_args[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        prod_args[i].buffer = &buffer;
        prod_args[i].producer_id = i + 1;
        pthread_create(&producers[i], nullptr, producer_routine, &prod_args[i]);
    }

    // Wait for all producers to finish submitting
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pthread_join(producers[i], nullptr);
    }
    std::cout << "All 10 producer threads completed successfully!\n";

    // Wait for all consumers to finish processing
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pthread_join(consumers[i], nullptr);
    }

    std::cout << "Total Jobs Consumed: " << g_consumed_count.load() << "/" << TOTAL_JOBS << "\n";
    assert(g_consumed_count.load() == TOTAL_JOBS);
    assert(buffer.size() == 0);

    std::cout << "[PASS] TC-02 Verified: 500 jobs processed through capacity-16 buffer without race or deadlock!\n";
    return 0;
}
