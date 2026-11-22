#include "ingest.h"
#include "buffer.h"
#include "logger.h"
#include "clock.h"
#include <fstream>
#include <sstream>

JobIngestionController::JobIngestionController(BoundedRingBuffer* ring_buffer)
    : buffer_(ring_buffer) {
    pthread_mutex_init(&stats_mutex_, nullptr);
}

JobIngestionController::~JobIngestionController() {
    pthread_mutex_destroy(&stats_mutex_);
}

bool JobIngestionController::submit_job(uint32_t id, const std::string& client_id, UserRole role,
                                        uint32_t pages, bool urgency, bool is_duplex,
                                        const std::string& doc_name) {
    // Validation check: pages must be between 1 and 100
    if (pages < 1 || pages > 100) {
        pthread_mutex_lock(&stats_mutex_);
        rejected_count_++;
        pthread_mutex_unlock(&stats_mutex_);
        LOG_W("INGESTION", "Job #%u rejected: invalid page count %u (allowed: 1..100)", id, pages);
        return false;
    }

    if (role != ROLE_STUDENT && role != ROLE_RESEARCHER && role != ROLE_FACULTY) {
        pthread_mutex_lock(&stats_mutex_);
        rejected_count_++;
        pthread_mutex_unlock(&stats_mutex_);
        LOG_W("INGESTION", "Job #%u rejected: invalid user role", id);
        return false;
    }

    auto* job = new JobControlBlock();
    job->id = id;
    job->client_id = client_id.empty() ? ("client_" + std::to_string(id)) : client_id;
    job->role = role;
    job->total_pages = pages;
    job->remaining_pages = pages;
    job->urgency = urgency;
    job->is_duplex = is_duplex;
    job->document_name = doc_name.empty() ? ("doc_" + std::to_string(id) + ".pdf") : doc_name;
    job->arrival_time = SimClock::instance().now();
    job->state = STATE_SUBMITTED;

    // Transition from SUBMITTED to WAITING upon ingestion validation
    if (!transition_state(job, STATE_WAITING, "ingestion validation passed")) {
        delete job;
        pthread_mutex_lock(&stats_mutex_);
        rejected_count_++;
        pthread_mutex_unlock(&stats_mutex_);
        return false;
    }

    // Submit to thread-safe bounded circular ring buffer
    if (!buffer_ || !buffer_->enqueue(job)) {
        LOG_E("INGESTION", "Failed to enqueue Job #%u into ring buffer (shutdown or full)", id);
        delete job;
        pthread_mutex_lock(&stats_mutex_);
        rejected_count_++;
        pthread_mutex_unlock(&stats_mutex_);
        return false;
    }

    pthread_mutex_lock(&stats_mutex_);
    accepted_count_++;
    pthread_mutex_unlock(&stats_mutex_);

    LOG_I("INGESTION", "Job #%u accepted: Role=%s, Pages=%u, Urg=%d, Duplex=%d",
          job->id, user_role_to_string(job->role), job->total_pages, job->urgency, job->is_duplex);

    return true;
}

bool JobIngestionController::replay_workload_file(const std::string& filepath, bool pace_by_arrival) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        LOG_E("INGESTION", "Cannot open workload file: %s", filepath.c_str());
        return false;
    }

    std::string line;
    uint32_t line_num = 0;

    LOG_I("INGESTION", "Replaying workload from: %s", filepath.c_str());

    while (std::getline(file, line)) {
        line_num++;
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        double arrival_sec;
        uint32_t job_id;
        std::string client_id;
        std::string role_str;
        uint32_t pages;
        int urgency_val = 0;
        int duplex_val = 0;
        std::string doc_name = "";

        if (!(iss >> arrival_sec >> job_id >> client_id >> role_str >> pages)) {
            LOG_W("INGESTION", "Malformed workload entry at line %u: %s", line_num, line.c_str());
            continue;
        }

        iss >> urgency_val >> duplex_val;
        if (!(iss >> doc_name)) {
            doc_name = "doc_" + std::to_string(job_id) + ".pdf";
        }

        if (pace_by_arrival) {
            double current_sim_time = SimClock::instance().now();
            if (arrival_sec > current_sim_time) {
                double diff = arrival_sec - current_sim_time;
                if (SimClock::instance().is_virtual()) {
                    SimClock::instance().advance_seconds(diff);
                } else {
                    SimClock::instance().sleep_simulated(diff);
                }
            }
        }

        UserRole role = string_to_user_role(role_str);
        submit_job(job_id, client_id, role, pages, (urgency_val != 0), (duplex_val != 0), doc_name);
    }

    LOG_I("INGESTION", "Completed workload replay: %u accepted, %u rejected",
          accepted_count_, rejected_count_);
    return true;
}
