#include "buffer.h"
#include "logger.h"

BoundedRingBuffer::BoundedRingBuffer(size_t capacity)
    : capacity_(capacity > 0 ? capacity : RING_BUFFER_CAPACITY),
      buffer_(capacity_, nullptr) {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&not_full_, nullptr);
    pthread_cond_init(&not_empty_, nullptr);
}

BoundedRingBuffer::~BoundedRingBuffer() {
    shutdown();
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&not_full_);
    pthread_cond_destroy(&not_empty_);
}

bool BoundedRingBuffer::enqueue(JobControlBlock* job) {
    if (!job) return false;

    pthread_mutex_lock(&mutex_);
    while (count_ == capacity_ && !shutdown_) {
        // Must wait inside while loop to guard against spurious wakeups
        pthread_cond_wait(&not_full_, &mutex_);
    }

    if (shutdown_) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    buffer_[tail_] = job;
    tail_ = (tail_ + 1) % capacity_;
    count_++;

    LOG_D("RING_BUFFER", "Enqueued Job #%u (Occupancy: %zu/%zu)", job->id, count_, capacity_);

    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

JobControlBlock* BoundedRingBuffer::dequeue() {
    pthread_mutex_lock(&mutex_);
    while (count_ == 0 && !shutdown_) {
        // Must wait inside while loop to guard against spurious wakeups
        pthread_cond_wait(&not_empty_, &mutex_);
    }

    if (count_ == 0 && shutdown_) {
        pthread_mutex_unlock(&mutex_);
        return nullptr;
    }

    JobControlBlock* job = buffer_[head_];
    buffer_[head_] = nullptr;
    head_ = (head_ + 1) % capacity_;
    count_--;

    LOG_D("RING_BUFFER", "Dequeued Job #%u (Occupancy: %zu/%zu)", job->id, count_, capacity_);

    pthread_cond_signal(&not_full_);
    pthread_mutex_unlock(&mutex_);
    return job;
}

bool BoundedRingBuffer::try_enqueue(JobControlBlock* job) {
    if (!job) return false;

    pthread_mutex_lock(&mutex_);
    if (count_ == capacity_ || shutdown_) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    buffer_[tail_] = job;
    tail_ = (tail_ + 1) % capacity_;
    count_++;

    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

JobControlBlock* BoundedRingBuffer::try_dequeue() {
    pthread_mutex_lock(&mutex_);
    if (count_ == 0) {
        pthread_mutex_unlock(&mutex_);
        return nullptr;
    }

    JobControlBlock* job = buffer_[head_];
    buffer_[head_] = nullptr;
    head_ = (head_ + 1) % capacity_;
    count_--;

    pthread_cond_signal(&not_full_);
    pthread_mutex_unlock(&mutex_);
    return job;
}

void BoundedRingBuffer::shutdown() {
    pthread_mutex_lock(&mutex_);
    shutdown_ = true;
    pthread_cond_broadcast(&not_full_);
    pthread_cond_broadcast(&not_empty_);
    pthread_mutex_unlock(&mutex_);
}

bool BoundedRingBuffer::is_shutdown() const {
    pthread_mutex_lock(&mutex_);
    bool s = shutdown_;
    pthread_mutex_unlock(&mutex_);
    return s;
}

size_t BoundedRingBuffer::size() const {
    pthread_mutex_lock(&mutex_);
    size_t c = count_;
    pthread_mutex_unlock(&mutex_);
    return c;
}

size_t BoundedRingBuffer::capacity() const {
    return capacity_;
}

std::vector<JobControlBlock*> BoundedRingBuffer::get_snapshot() const {
    pthread_mutex_lock(&mutex_);
    std::vector<JobControlBlock*> snap;
    snap.reserve(count_);
    size_t idx = head_;
    for (size_t i = 0; i < count_; ++i) {
        if (buffer_[idx]) {
            snap.push_back(buffer_[idx]);
        }
        idx = (idx + 1) % capacity_;
    }
    pthread_mutex_unlock(&mutex_);
    return snap;
}
