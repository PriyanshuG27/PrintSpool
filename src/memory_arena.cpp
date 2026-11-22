#include "memory_arena.h"
#include "logger.h"
#include <cstring>
#include <cassert>

MemoryArena::MemoryArena() {
    pthread_mutex_init(&mutex_, nullptr);
    init(ARENA_TOTAL_SIZE);
}

MemoryArena::~MemoryArena() {
    pthread_mutex_destroy(&mutex_);
}

MemoryArena& MemoryArena::instance() {
    static MemoryArena arena;
    return arena;
}

void MemoryArena::init(size_t total_size) {
    pthread_mutex_lock(&mutex_);
    total_size_ = (total_size <= ARENA_TOTAL_SIZE) ? total_size : ARENA_TOTAL_SIZE;
    std::memset(arena_, 0, sizeof(arena_));

    // Initialize root header as one large free block spanning the whole arena
    auto* root = reinterpret_cast<BlockHeader*>(&arena_[0]);
    root->size = static_cast<uint32_t>(total_size_);
    root->is_free = 1;
    root->magic = BLOCK_MAGIC;
    root->prev_offset = -1;
    root->next_offset = -1;
    root->job_id = 0;
    root->reserved = 0;

    pthread_mutex_unlock(&mutex_);
    LOG_I("ARENA", "Initialized 64 KB First-Fit Memory Arena (Size: %zu bytes, Header: %zu bytes)",
          total_size_, sizeof(BlockHeader));
}

void MemoryArena::reset() {
    init(ARENA_TOTAL_SIZE);
}

int32_t MemoryArena::allocate(size_t payload_bytes, uint32_t job_id) {
    if (payload_bytes == 0) return -1;

    // Align payload size up to 8 bytes
    size_t aligned_payload = (payload_bytes + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
    size_t needed_block_size = sizeof(BlockHeader) + aligned_payload;

    pthread_mutex_lock(&mutex_);

    int32_t current_offset = 0;
    int32_t allocated_offset = -1;

    // First-Fit search
    while (current_offset >= 0 && static_cast<size_t>(current_offset) < total_size_) {
        auto* block = reinterpret_cast<BlockHeader*>(&arena_[current_offset]);

        if (block->magic != BLOCK_MAGIC) {
            LOG_E("ARENA", "Corrupt block header encountered at offset %d! Allocation aborted.", current_offset);
            pthread_mutex_unlock(&mutex_);
            return -1;
        }

        if (block->is_free && block->size >= needed_block_size) {
            size_t remainder = block->size - needed_block_size;

            // Split block if remainder is large enough for a new header + min payload
            if (remainder >= sizeof(BlockHeader) + MIN_PAYLOAD_SIZE) {
                int32_t new_block_offset = current_offset + static_cast<int32_t>(needed_block_size);
                auto* new_block = reinterpret_cast<BlockHeader*>(&arena_[new_block_offset]);

                new_block->size = static_cast<uint32_t>(remainder);
                new_block->is_free = 1;
                new_block->magic = BLOCK_MAGIC;
                new_block->prev_offset = current_offset;
                new_block->next_offset = block->next_offset;
                new_block->job_id = 0;
                new_block->reserved = 0;

                // Update neighbor link
                if (block->next_offset >= 0 && static_cast<size_t>(block->next_offset) < total_size_) {
                    auto* next_neighbor = reinterpret_cast<BlockHeader*>(&arena_[block->next_offset]);
                    next_neighbor->prev_offset = new_block_offset;
                }

                block->size = static_cast<uint32_t>(needed_block_size);
                block->next_offset = new_block_offset;
            }

            block->is_free = 0;
            block->job_id = job_id;
            allocated_offset = current_offset;
            break;
        }

        current_offset = block->next_offset;
    }

    pthread_mutex_unlock(&mutex_);

    if (allocated_offset >= 0) {
        LOG_D("ARENA", "Allocated %zu B (Block %zu B) for Job #%u at offset %d",
              payload_bytes, needed_block_size, job_id, allocated_offset);
    } else {
        // Debug level: the dispatcher retries while waiting for RAM, so WARN would flood the console.
        LOG_D("ARENA", "Failed to allocate %zu B for Job #%u (RAM pressure / fragmentation)",
              payload_bytes, job_id);
    }

    return allocated_offset;
}

bool MemoryArena::can_allocate(size_t payload_bytes) const {
    if (payload_bytes == 0) return false;
    size_t aligned_payload = (payload_bytes + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
    return largest_free_block() >= sizeof(BlockHeader) + aligned_payload;
}

bool MemoryArena::deallocate(int32_t offset, uint32_t job_id) {
    if (offset < 0 || static_cast<size_t>(offset) >= total_size_) return false;

    pthread_mutex_lock(&mutex_);

    auto* block = reinterpret_cast<BlockHeader*>(&arena_[offset]);
    if (block->magic != BLOCK_MAGIC) {
        LOG_E("ARENA", "Cannot deallocate offset %d: invalid magic 0x%X", offset, block->magic);
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    if (block->is_free) {
        LOG_W("ARENA", "Double-free detected at offset %d for Job #%u", offset, job_id);
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    if (job_id != 0 && block->job_id != job_id) {
        LOG_E("ARENA", "Job #%u tried to free offset %d owned by Job #%u - refused", job_id, offset, block->job_id);
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    block->is_free = 1;
    block->job_id = 0;

    // Coalesce with next block if free
    if (block->next_offset >= 0 && static_cast<size_t>(block->next_offset) < total_size_) {
        auto* next_block = reinterpret_cast<BlockHeader*>(&arena_[block->next_offset]);
        if (next_block->magic == BLOCK_MAGIC && next_block->is_free) {
            block->size += next_block->size;
            block->next_offset = next_block->next_offset;
            next_block->magic = 0;  // absorbed header is no longer a block

            if (block->next_offset >= 0 && static_cast<size_t>(block->next_offset) < total_size_) {
                auto* after_next = reinterpret_cast<BlockHeader*>(&arena_[block->next_offset]);
                after_next->prev_offset = offset;
            }
        }
    }

    // Coalesce with previous block if free
    if (block->prev_offset >= 0 && static_cast<size_t>(block->prev_offset) < total_size_) {
        auto* prev_block = reinterpret_cast<BlockHeader*>(&arena_[block->prev_offset]);
        if (prev_block->magic == BLOCK_MAGIC && prev_block->is_free) {
            prev_block->size += block->size;
            prev_block->next_offset = block->next_offset;
            block->magic = 0;       // absorbed header is no longer a block

            if (block->next_offset >= 0 && static_cast<size_t>(block->next_offset) < total_size_) {
                auto* next_neighbor = reinterpret_cast<BlockHeader*>(&arena_[block->next_offset]);
                next_neighbor->prev_offset = block->prev_offset;
            }
        }
    }

    pthread_mutex_unlock(&mutex_);
    LOG_D("ARENA", "Freed memory at offset %d for Job #%u (Coalesced successfully)", offset, job_id);
    return true;
}

bool MemoryArena::verify_consistency() const {
    pthread_mutex_lock(&mutex_);

    size_t total_computed_size = 0;
    int32_t current_offset = 0;
    int32_t prev_check_offset = -1;

    while (current_offset >= 0 && static_cast<size_t>(current_offset) < total_size_) {
        const auto* block = reinterpret_cast<const BlockHeader*>(&arena_[current_offset]);

        if (block->magic != BLOCK_MAGIC) {
            LOG_E("ARENA_CHECK", "Magic check failed at offset %d (Expected: 0x%X, Got: 0x%X)",
                  current_offset, BLOCK_MAGIC, block->magic);
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        if (block->prev_offset != prev_check_offset) {
            LOG_E("ARENA_CHECK", "Prev link broken at offset %d (Block says %d, expected %d)",
                  current_offset, block->prev_offset, prev_check_offset);
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        if (block->size < sizeof(BlockHeader)) {
            LOG_E("ARENA_CHECK", "Block size %u smaller than header size at offset %d",
                  block->size, current_offset);
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        total_computed_size += block->size;
        prev_check_offset = current_offset;
        current_offset = block->next_offset;
    }

    bool consistent = (total_computed_size == total_size_);
    if (!consistent) {
        LOG_E("ARENA_CHECK", "Size mismatch: computed %zu bytes, expected %zu bytes",
              total_computed_size, total_size_);
    }

    pthread_mutex_unlock(&mutex_);
    return consistent;
}

uint8_t* MemoryArena::get_payload_ptr(int32_t offset) {
    if (offset < 0 || static_cast<size_t>(offset + sizeof(BlockHeader)) >= total_size_) {
        return nullptr;
    }
    return &arena_[offset + sizeof(BlockHeader)];
}

const uint8_t* MemoryArena::get_payload_ptr(int32_t offset) const {
    if (offset < 0 || static_cast<size_t>(offset + sizeof(BlockHeader)) >= total_size_) {
        return nullptr;
    }
    return &arena_[offset + sizeof(BlockHeader)];
}

size_t MemoryArena::allocated_bytes() const {
    pthread_mutex_lock(&mutex_);
    size_t alloc = 0;
    int32_t cur = 0;
    while (cur >= 0 && static_cast<size_t>(cur) < total_size_) {
        const auto* block = reinterpret_cast<const BlockHeader*>(&arena_[cur]);
        if (!block->is_free) alloc += block->size;
        cur = block->next_offset;
    }
    pthread_mutex_unlock(&mutex_);
    return alloc;
}

size_t MemoryArena::free_bytes() const {
    pthread_mutex_lock(&mutex_);
    size_t free_sz = 0;
    int32_t cur = 0;
    while (cur >= 0 && static_cast<size_t>(cur) < total_size_) {
        const auto* block = reinterpret_cast<const BlockHeader*>(&arena_[cur]);
        if (block->is_free) free_sz += block->size;
        cur = block->next_offset;
    }
    pthread_mutex_unlock(&mutex_);
    return free_sz;
}

size_t MemoryArena::largest_free_block() const {
    pthread_mutex_lock(&mutex_);
    size_t largest = 0;
    int32_t cur = 0;
    while (cur >= 0 && static_cast<size_t>(cur) < total_size_) {
        const auto* block = reinterpret_cast<const BlockHeader*>(&arena_[cur]);
        if (block->is_free && block->size > largest) {
            largest = block->size;
        }
        cur = block->next_offset;
    }
    pthread_mutex_unlock(&mutex_);
    return largest;
}

double MemoryArena::occupancy_ratio() const {
    size_t alloc = allocated_bytes();
    return static_cast<double>(alloc) / static_cast<double>(total_size_);
}

std::vector<ArenaBlockSnapshot> MemoryArena::get_snapshot() const {
    pthread_mutex_lock(&mutex_);
    std::vector<ArenaBlockSnapshot> snap;
    int32_t cur = 0;
    while (cur >= 0 && static_cast<size_t>(cur) < total_size_) {
        const auto* block = reinterpret_cast<const BlockHeader*>(&arena_[cur]);
        snap.push_back({cur, block->size, (block->is_free != 0), block->job_id});
        cur = block->next_offset;
    }
    pthread_mutex_unlock(&mutex_);
    return snap;
}
