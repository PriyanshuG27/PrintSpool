#include "common.h"
#include "logger.h"
#include "clock.h"
#include <algorithm>
#include <cctype>
#include <atomic>

static std::atomic<uint64_t> g_illegal_transitions{0};

uint64_t illegal_transition_count() { return g_illegal_transitions.load(); }

const char* job_state_to_string(JobState s) {
    switch (s) {
        case STATE_SUBMITTED:       return "SUBMITTED";
        case STATE_WAITING:         return "WAITING";
        case STATE_IN_MEMORY:       return "IN_MEMORY";
        case STATE_SWAPPED_TO_DISK: return "SWAPPED_TO_DISK";
        case STATE_PRINTING:        return "PRINTING";
        case STATE_COMPLETED:       return "COMPLETED";
        case STATE_REJECTED:        return "REJECTED";
        default:                    return "UNKNOWN";
    }
}

const char* user_role_to_string(UserRole r) {
    switch (r) {
        case ROLE_STUDENT:    return "STUDENT";
        case ROLE_RESEARCHER: return "RESEARCHER";
        case ROLE_FACULTY:    return "FACULTY";
        default:              return "UNKNOWN";
    }
}

UserRole string_to_user_role(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    if (s == "faculty") return ROLE_FACULTY;
    if (s == "researcher" || s == "scholar") return ROLE_RESEARCHER;
    return ROLE_STUDENT;
}

static bool is_valid_transition(JobState from, JobState to) {
    switch (from) {
        case STATE_SUBMITTED:       return to == STATE_WAITING || to == STATE_REJECTED;
        case STATE_WAITING:         return to == STATE_IN_MEMORY || to == STATE_SWAPPED_TO_DISK;
        case STATE_IN_MEMORY:       return to == STATE_PRINTING || to == STATE_SWAPPED_TO_DISK;
        case STATE_SWAPPED_TO_DISK: return to == STATE_IN_MEMORY;
        case STATE_PRINTING:        return to == STATE_COMPLETED;
        case STATE_COMPLETED:
        case STATE_REJECTED:        return false;  // terminal states
    }
    return false;
}

bool transition_state(JobControlBlock* job, JobState new_state, const char* reason) {
    if (!job) return false;

    // Atomic check-and-set: two threads racing to move the same job can never both
    // succeed, so a job can never be "claimed" twice.
    JobState old_state = job->state.load();
    for (;;) {
        if (!is_valid_transition(old_state, new_state)) {
            g_illegal_transitions.fetch_add(1);
            LOG_E("STATE_MACHINE", "Illegal transition rejected for Job #%u: %s -> %s (Reason: %s)",
                  job->id, job_state_to_string(old_state), job_state_to_string(new_state),
                  reason ? reason : "none");
            return false;
        }
        if (job->state.compare_exchange_weak(old_state, new_state)) break;
    }

    double now = SimClock::instance().now();
    job->last_state_change = now;

    if (new_state == STATE_WAITING && job->wait_start_time == 0.0) {
        job->wait_start_time = now;
    } else if (new_state == STATE_PRINTING) {
        job->start_print_time = now;
        job->total_wait_time = now - job->arrival_time;
    } else if (new_state == STATE_COMPLETED) {
        job->completion_time = now;
    }

    LOG_I("STATE_MACHINE", "Job #%u transitioned: %s -> %s [%s]",
          job->id, job_state_to_string(old_state), job_state_to_string(new_state),
          reason ? reason : "normal");

    return true;
}
