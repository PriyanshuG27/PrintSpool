#pragma once

#include "common.h"
#include <vector>
#include <pthread.h>

// ============================================================================
// Module 5: Contiguous Memory Arena Allocator
// ============================================================================
// Manages a 64 KB contiguous byte array with block headers embedded inside.
// Features:
// - First-fit allocation
// - Block splitting when remainder >= header + MIN_PAYLOAD
// - Immediate coalescing with adjacent free blocks on deallocation
// - Strict 8-byte alignment
// - Complete consistency verification (sum of blocks == 65536)
// ============================================================================

constexpr uint16_t BLOCK_MAGIC = 0x5A5A;
constexpr size_t MIN_PAYLOAD_SIZE = 16;

#pragma pack(push, 8)
struct BlockHeader {
    uint32_t size;         // Total block size including this header (multiple of 8)
    uint16_t is_free;      // 1 if free, 0 if occupied
    uint16_t magic;        // BLOCK_MAGIC for sanity check
    int32_t prev_offset;   // Offset of previous block (-1 if head)
    int32_t next_offset;   // Offset of next block (-1 if tail)
    uint32_t job_id;       // Associated Job ID (0 if free)
    uint32_t reserved;     // Padding for 8-byte alignment
};
#pragma pack(pop)

struct ArenaBlockSnapshot {
    int32_t offset;
    uint32_t size;
    bool is_free;
    uint32_t job_id;
};

class MemoryArena {
public:
    static MemoryArena& instance();

    void init(size_t total_size = ARENA_TOTAL_SIZE);
    void reset();

    // Allocate contiguous block using First-Fit algorithm
    // Returns offset in arena (>= 0), or -1 if insufficient memory
    int32_t allocate(size_t payload_bytes, uint32_t job_id);

    // True if a first-fit allocation of this size would currently succeed (no logging, no side effects)
    bool can_allocate(size_t payload_bytes) const;

    // Free block at given offset and coalesce with free neighbors (job_id must own the block)
    bool deallocate(int32_t offset, uint32_t job_id);

    // Verify arena integrity: total bytes == ARENA_TOTAL_SIZE, headers valid
    bool verify_consistency() const;

    // Direct access to buffer payload
    uint8_t* get_payload_ptr(int32_t offset);
    const uint8_t* get_payload_ptr(int32_t offset) const;

    // Metrics & Telemetry
    size_t total_size() const { return total_size_; }
    size_t allocated_bytes() const;
    size_t free_bytes() const;
    size_t largest_free_block() const;
    double occupancy_ratio() const;
    std::vector<ArenaBlockSnapshot> get_snapshot() const;

private:
    MemoryArena();
    ~MemoryArena();

    uint8_t arena_[ARENA_TOTAL_SIZE];
    size_t total_size_ = ARENA_TOTAL_SIZE;
    mutable pthread_mutex_t mutex_;
};
