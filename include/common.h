#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <atomic>
#include <pthread.h>

// ============================================================================
// Core Constants and Configuration Defaults
// ============================================================================
constexpr size_t ARENA_TOTAL_SIZE = 65536;        // 64 KB static arena
constexpr size_t ARENA_ALIGNMENT = 8;             // 8-byte alignment
constexpr size_t ARENA_MAX_JOB_MEM = 16384;       // 16 KB max spool window per job
constexpr size_t ARENA_MEM_PER_PAGE = 1024;       // 1 KB per page nominal memory
constexpr uint32_t DISK_TOTAL_CYLINDERS = 200;    // 0 to 199 cylinders
constexpr size_t DISK_SECTOR_SIZE = 512;          // 512 bytes per sector
constexpr size_t RING_BUFFER_CAPACITY = 16;       // Default ring buffer slots
constexpr int DEFAULT_PRINTER_WORKERS = 3;        // Number of printer threads

// ============================================================================
// Enums
// ============================================================================
enum JobState {
    STATE_SUBMITTED = 0,
    STATE_WAITING = 1,
    STATE_IN_MEMORY = 2,
    STATE_SWAPPED_TO_DISK = 3,
    STATE_PRINTING = 4,
    STATE_COMPLETED = 5,
    STATE_REJECTED = 6
};

enum UserRole {
    ROLE_STUDENT = 0,
    ROLE_RESEARCHER = 1,
    ROLE_FACULTY = 2
};

const char* job_state_to_string(JobState s);
const char* user_role_to_string(UserRole r);
UserRole string_to_user_role(const std::string& str);

// ============================================================================
// System Configuration Parameters
// ============================================================================
struct SystemConfig {
    size_t arena_size = ARENA_TOTAL_SIZE;
    uint32_t cylinder_count = DISK_TOTAL_CYLINDERS;
    size_t buffer_capacity = RING_BUFFER_CAPACITY;
    int printer_workers = DEFAULT_PRINTER_WORKERS;

    // Dispatcher Weights:
    // score = a * remaining_pages - b * role_weight - c * aging_boost(wait) - d * urgency
    // Lower score is dispatched first!
    double weight_pages = 1.0;          // a
    double role_weight_student = 0.0;   // b component
    double role_weight_researcher = 8.0;
    double role_weight_faculty = 16.0;
    double aging_threshold_sec = 4.0;   // Wait time threshold before aging begins
    double aging_rate = 5.0;            // c: score reduction per second after threshold
    double urgency_bonus = 20.0;        // d: urgency score reduction

    // Printing simulation timing (in simulated seconds per page)
    double page_print_time_sec = 0.2;
    double duplex_flip_penalty_sec = 0.1;
    double simulated_tick_step = 0.05;

    // Simulation Clock Mode
    bool use_virtual_time = true;       // Step-driven deterministic clock
    double real_time_scale = 1.0;       // Scale factor if using real-time sleeping
};

// ============================================================================
// Job Control Block (JCB)
// ============================================================================
struct JobControlBlock {
    uint32_t id = 0;
    std::string client_id = "";
    UserRole role = ROLE_STUDENT;
    uint32_t total_pages = 0;
    // Atomic: the owning printer decrements it while telemetry/scheduler read it.
    std::atomic<uint32_t> remaining_pages{0};
    bool urgency = false;
    bool is_duplex = false;

    // Lifecycle timestamps (in simulated seconds)
    double arrival_time = 0.0;
    double wait_start_time = 0.0;
    double total_wait_time = 0.0;
    double start_print_time = 0.0;
    double completion_time = 0.0;
    double last_state_change = 0.0;

    // State machine
    // Atomic: read by dispatcher/telemetry while printer workers and the swap engine change it.
    std::atomic<JobState> state{STATE_SUBMITTED};

    // Memory Arena Allocation info
    std::atomic<int32_t> arena_offset{-1};  // Offset in 64 KB arena, -1 if not allocated
    size_t arena_size = 0;              // Bytes allocated in arena

    // Disk Swapping info
    int32_t swap_cylinder = -1;         // Assigned cylinder in swap.img (0..199), -1 if none
    double last_used_timestamp = 0.0;   // LRU tracking timestamp

    // Scheduling metrics
    double effective_score = 0.0;
    std::atomic<int> assigned_printer_id{-1};  // -1 when unassigned, else printer 0..N-1

    // Simulated payload data
    std::string document_name = "";
};

// ============================================================================
// Global State Machine Transition Function
// ============================================================================
// Validates and executes state transitions according to the OS lifecycle specs.
// Returns true on valid transition, false on illegal transition (with log warning).
bool transition_state(JobControlBlock* job, JobState new_state, const char* reason = nullptr);

// Number of transitions rejected as illegal since process start (diagnostics / tests).
uint64_t illegal_transition_count();
