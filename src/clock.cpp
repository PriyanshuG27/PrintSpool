#include "clock.h"
#include <chrono>
#include <thread>

SimClock::SimClock() {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&cond_, nullptr);
}

SimClock::~SimClock() {
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&cond_);
}

SimClock& SimClock::instance() {
    static SimClock clk;
    return clk;
}

void SimClock::reset() {
    pthread_mutex_lock(&mutex_);
    current_time_sec_ = 0.0;
    tick_count_ = 0;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void SimClock::set_mode(bool virtual_time, double scale) {
    pthread_mutex_lock(&mutex_);
    virtual_mode_ = virtual_time;
    time_scale_ = (scale > 0.0001) ? scale : 1.0;
    pthread_mutex_unlock(&mutex_);
}

bool SimClock::is_virtual() const {
    pthread_mutex_lock(&mutex_);
    bool v = virtual_mode_;
    pthread_mutex_unlock(&mutex_);
    return v;
}

double SimClock::now() {
    pthread_mutex_lock(&mutex_);
    double t = current_time_sec_;
    pthread_mutex_unlock(&mutex_);
    return t;
}

uint64_t SimClock::ticks() {
    pthread_mutex_lock(&mutex_);
    uint64_t tc = tick_count_;
    pthread_mutex_unlock(&mutex_);
    return tc;
}

void SimClock::advance_seconds(double delta) {
    if (delta <= 0.0) return;
    pthread_mutex_lock(&mutex_);
    current_time_sec_ += delta;
    tick_count_++;
    pthread_cond_broadcast(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void SimClock::sleep_simulated(double sec) {
    if (sec <= 0.0) return;

    pthread_mutex_lock(&mutex_);
    if (!virtual_mode_) {
        pthread_mutex_unlock(&mutex_);
        double real_ms = (sec / time_scale_) * 1000.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(real_ms)));
        pthread_mutex_lock(&mutex_);
        current_time_sec_ += sec;
        tick_count_++;
        pthread_mutex_unlock(&mutex_);
    } else {
        // Virtual mode: wait until external driver advances clock to target time
        double target = current_time_sec_ + sec;
        while (current_time_sec_ < target) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        pthread_mutex_unlock(&mutex_);
    }
}
