# Smart Campus Print Spooler: System Architecture

## 1. Subsystem Decomposition

The spooler is divided into 8 cooperating modules implemented across 4 team members:

```
[M1: Ingestion Controller] (Priyanshu)
  │
  ▼
[M2: Bounded Ring Buffer] (Priyanshu)
  │
  ▼
[M3: Priority-SRTF Aging Dispatcher] (Rashi) ──► [M4: Printer Worker Pool] (Rashi)
  │                                                        │
  ▼                                                        ▼
[M5: First-Fit 64 KB Arena] (Anant) ◄──────────── Frees Memory upon completion
  ▲             │
  │ Swap-In     ▼ Swap-Out (LRU)
[M6: Virtual Memory Swap Engine] (Anant)
  │
  ▼ (I/O requests)
[M7: SCAN Disk Scheduling Controller] (Ram)
  ▲
  │
[M8: Telemetry & ASCII HUD Dashboard] (Ram) ──► Samples M2-M7 non-blockingly
```

---

## 2. Core Data Structures

### 2.1 Job Control Block (JCB)
Stored in memory throughout the lifecycle of the job:
```cpp
struct JobControlBlock {
    uint32_t id;
    std::string client_id;
    UserRole role;                // STUDENT, RESEARCHER, FACULTY
    uint32_t total_pages;
    uint32_t remaining_pages;
    bool urgency;
    bool is_duplex;
    double arrival_time;
    double start_print_time;
    double total_wait_time;
    double completion_time;
    JobState state;               // State machine tracking
    int32_t arena_offset;         // Byte offset in 64 KB arena (-1 if none)
    size_t arena_size;            // Allocated bytes
    int32_t swap_cylinder;        // Assigned cylinder in swap.img (-1 if none)
    double effective_score;       // Computed dynamic priority
    int assigned_printer_id;      // Worker ID (0..N-1)
};
```

### 2.2 Embedded Block Header (Memory Arena)
Stored directly inside the 64 KB array (`uint8_t arena[65536]`) with 8-byte alignment:
```cpp
#pragma pack(push, 8)
struct BlockHeader {
    uint32_t size;         // Total block size including header
    uint16_t is_free;      // 1 if free, 0 if occupied
    uint16_t magic;        // BLOCK_MAGIC (0x5A5A)
    int32_t prev_offset;   // Offset of previous block (-1 if head)
    int32_t next_offset;   // Offset of next block (-1 if tail)
    uint32_t job_id;       // Associated Job ID (0 if free)
    uint32_t reserved;     // 8-byte alignment pad
};
#pragma pack(pop)
```

---

## 3. Dynamic Scheduling & Starvation Aging Formula

The dispatcher computes the dynamic score for every eligible candidate job at simulated time $t$:

$$\text{Score}(j, t) = a \cdot \text{remaining\_pages} - b \cdot \text{role\_weight} - c \cdot \text{aging\_boost}(\text{wait\_time}) - d \cdot \text{urgency}$$

Where:
- $a = 1.0$ (favors Shortest-Remaining-Time-First)
- $b$: Faculty = $16.0$, Researcher = $8.0$, Student = $0.0$
- $c = 8.5135$ (aging rate applied after aging threshold)
- $\text{aging\_threshold} = 4.0\text{ seconds}$
- $\text{aging\_boost} = \max(0.0, \text{wait\_time} - \text{aging\_threshold})$
- $d = 20.0$ if urgent, else $0.0$
- **Dispatch Decision**: The candidate with the **lowest score** is dispatched first.

### Proof of Starvation Elimination (TC-01 Calibration):
For an 80-page Faculty job competing with a continuous stream of 1-page Student jobs:
- Student job score: $1.0 \times 1 - 0.0 - 0.0 = 1.0$
- Faculty job initial score ($t \le 4.0\text{s}$): $1.0 \times 80 - 16.0 = 64.0 > 1.0$ (Student favored)
- At $t = 11.4\text{s}$, wait time = $11.4\text{s} \implies \text{aging\_boost} = 11.4 - 4.0 = 7.4\text{s}$
- Faculty score at $t = 11.4\text{s}$:
  $$\text{Score}_F = 64.0 - (8.5135 \times 7.4) = 64.0 - 63.0 = 1.00 \le \text{Score}_S$$
- At $t = 11.4\text{s}$, the faculty job's score matches and beats the student jobs, eliminating starvation!

---

## 4. First-Fit Dynamic Memory Arena & Spool Window

- **Total Capacity**: Exactly 65,536 bytes (64 KB).
- **Spool Window Rule**: Jobs require nominal memory calculated as:
  $$\text{Memory}(j) = \min(\text{pages} \times 1024, 16384)$$
  This avoids an 80-page job exhausting the whole arena, acting as an OS page-window buffer.
- **Coalescing**: When freeing a block, the allocator checks both `prev_offset` and `next_offset`. If adjacent blocks are free, they merge in $O(1)$ time.
- **Consistency Verification**: After every alloc/free in tests, a linear walk asserts:
  $$\sum \text{BlockSize} = 65536 \quad \land \quad \text{Magic} = 0\text{x}5A5A$$

---

## 5. Virtual Memory Swapping & LRU Eviction

When the 64 KB arena suffers memory exhaustion:
1. Candidate cannot allocate memory.
2. `SwapEngine` scans the LRU cache (built with `std::list` and `std::unordered_map` for $O(1)$ operations).
3. **Safety Invariant**: ONLY jobs in `STATE_IN_MEMORY` that are NOT currently `STATE_PRINTING` are eligible for eviction.
4. **Anti-Thrashing Rule**: A job is only evicted if its dynamic priority is strictly lower than the candidate requesting memory.
5. Evicted job payload is written to an allocated disk cylinder in `swap.img` via the SCAN disk controller.
6. Arena block is freed and coalesced, state transitions to `STATE_SWAPPED_TO_DISK`.

---

## 6. SCAN (Elevator) Disk Controller

- Flat file `swap.img` divided into 200 cylinders ($0 \dots 199$).
- Head starts at cylinder 53 moving in direction **UP**.
- **Algorithm**: Services requests in the upward direction until reaching boundary (199), reverses direction to **DOWN**, and services requests down to the lowest requested cylinder.
- **Head Movement Benchmark (TC-04)**:
  - Sequence: `[98, 183, 37, 122, 14, 124, 65, 67]`
  - FCFS path: $53 \to 98 \to 183 \to 37 \to 122 \to 14 \to 124 \to 65 \to 67 = 640\text{ cylinders}$
  - SCAN path: $53 \to 65 \to 67 \to 98 \to 122 \to 124 \to 183 \to 199 \to 37 \to 14 = 331\text{ cylinders}$
  - Mechanical seek travel reduction: **48.3%**.

---

## 7. Concurrency & Deadlock Prevention

### Global Lock Hierarchy
To mathematically preclude circular wait (Coffman condition):
1. `job_table_mutex`
2. `buffer_mutex`
3. `scheduler_mutex`
4. `arena_mutex`
5. `swap_engine_mutex`
6. `disk_queue_mutex`
7. `telemetry_mutex`

**Critical Rule**: Threads **NEVER** hold `arena_mutex` or `scheduler_mutex` while performing blocking disk I/O.
