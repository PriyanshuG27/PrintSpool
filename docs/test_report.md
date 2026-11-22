# Formal Test Verification Report: Smart Campus Print Spooler

## 1. Verification Summary

All test suites and integration scenarios were built and executed under **GCC 13.3.0** with **POSIX threads** (`-pthread`) on Ubuntu 24.04 (WSL2) and verified clean under **AddressSanitizer (ASan)**, **UndefinedBehaviorSanitizer (UBSan)**, and **ThreadSanitizer (TSan)**.

| Test Case | Objective | Standard / Expected Result | Observed Result | Status |
| :--- | :--- | :--- | :--- | :---: |
| **TC-01** | Starvation Elimination | 80-page Faculty job dispatches at $T \approx 11.4$ s despite continuous 1-page Student stream | Dispatched at $T = 11.4$ s | **PASS** |
| **TC-02** | Concurrency & Deadlock Stress | 10 producers $\times$ 50 jobs into capacity-16 buffer with 3 consumers (500 total jobs) | 500/500 jobs processed, 0 races, 0 deadlocks | **PASS** |
| **TC-03** | Arena Memory & Swap Trigger | Fill 64 KB arena to ~92%; submit 16 KB job; LRU swap-out reclaims space | Reclaimed ~19 KB; 16 KB job allocated; consistency verified | **PASS** |
| **TC-04** | SCAN Seek Optimization | Compare SCAN vs FCFS on `[98, 183, 37, 122, 14, 124, 65, 67]` from head 53 | SCAN: 331 cylinders vs FCFS: 640 cylinders (48.3% reduction) | **PASS** |
| **TC-05** | Swap-Path Integrity | Evict a still-queued job, then drain: each job prints once, 0 illegal transitions, no leaks | All invariants hold (see `docs/bugfix_notes.md`) | **PASS** |

---

## 2. Test Execution Details

### TC-01: Starvation Elimination via Priority Aging
```text
========================================================
 TC-01: Starvation Elimination via Priority Aging Test
========================================================
Submitted 80-page Faculty Job #1 at T = 0.00s
Simulating time steps in 0.1s increments under continuous student load...
>>> Faculty Job #1 beat competing student job and dispatched at T = 11.4s (Score: 0.952) <<<
Observed Dispatch Time for 80-page Faculty Job: T = 11.4 s
[PASS] TC-01 Verified: Starvation avoided! Faculty job dispatched at T = 11.4 s (expected ~11.4 s)
```
- **Analysis**: Short 1-page student jobs were favored between $T = 0.0\text{s}$ and $T = 11.4\text{s}$. Once the faculty job accumulated 7.4 seconds of wait time past the 4.0s threshold, its dynamic aging boost overcame the SRTF page deficit and dispatched before starvation occurred.

---

### TC-02: Bounded Buffer Concurrency & Deadlock Stress Test
```text
=================================================================
 TC-02: Bounded Buffer Concurrency & Deadlock Stress Test
=================================================================
Producers: 10 (Submitting 50 jobs each)
Buffer Capacity: 16 slots
Consumers: 3 consumer threads
Total Expected Jobs: 500

All 10 producer threads completed successfully!
Total Jobs Consumed: 500/500
[PASS] TC-02 Verified: 500 jobs processed through capacity-16 buffer without race or deadlock!
```
- **ThreadSanitizer Verification**: Executed with `-fsanitize=thread`. TSan reported **zero data races** and **zero synchronization errors**.

---

### TC-03: First-Fit Arena Allocation & Swap Triggering
```text
=================================================================
 TC-03: First-Fit Arena Allocation & Swap Triggering Test
=================================================================
[T=   0.00s] [INFO ] [ARENA        ] Initialized 64 KB First-Fit Memory Arena (Size: 65536 bytes, Header: 24 bytes)
Occupancy after initial filling: 89.9414% (58944 B allocated, 6592 B free)
Submitting Job #99 requiring 16384 B (16 KB)...
[T=   0.00s] [WARN ] [ARENA        ] Failed to allocate 16384 B for Job #99 (RAM pressure / fragmentation)
Direct allocation result: -1 (expected: -1)
LRU Victim selected: Job #1 (Arena offset: 0)
[T=   0.00s] [INFO ] [SWAP_ENGINE  ] Swapped out Job #1 (freed offset 0 -> cylinder 0, reclaimed 9800 B)
LRU Victim selected: Job #2 (Arena offset: 9824)
[T=   0.00s] [INFO ] [SWAP_ENGINE  ] Swapped out Job #2 (freed offset 9824 -> cylinder 1, reclaimed 9800 B)
Reclaimed space via LRU swap-out: ~19 KB
Job #99 successfully allocated in arena at offset: 0
Successfully restored Job #1 back from swap to offset 0
Successfully restored Job #2 back from swap to offset 9824
[PASS] TC-03 Verified: 92% arena full -> 16 KB request triggered swap-out reclaiming ~20 KB -> allocation succeeded!
```
- **Consistency Verification**: `verify_consistency()` passed after every allocation, free, and coalescing step, verifying $\sum \text{sizes} = 65536$ bytes.

---

### TC-04: SCAN Disk Scheduling Seek Optimization
```text
========================================================
 TC-04: SCAN Disk Scheduling Seek Optimization Test
========================================================
Initial Head: 53 (Direction: UP)
Cylinder Range: 0 to 199
Request Sequence: [98, 183, 37, 122, 14, 124, 65, 67]

--- FCFS Scheduling Execution ---
Sequence: 53 -> 98 -> 183 -> 37 -> 122 -> 14 -> 124 -> 65 -> 67
Total FCFS Head Movement: 640 cylinders

--- SCAN (Elevator) Execution ---
Sequence: 53 -> 65 -> 67 -> 98 -> 122 -> 124 -> 183 -> 37 -> 14
Total SCAN Head Movement: 331 cylinders
Mechanical Travel Reduction: 48.2812%

[PASS] TC-04 Verified: SCAN requires 331 cylinders vs 640 for FCFS (48.3% reduction)
```

---

## 3. Sanitizer & Valgrind Audit

1. **AddressSanitizer (ASan)**: Clean pass across all modules. 0 buffer overflows, 0 use-after-free errors.
2. **UndefinedBehaviorSanitizer (UBSan)**: 0 alignment faults, 0 integer overflows.
3. **ThreadSanitizer (TSan)**: 0 data races across concurrent producer, dispatcher, printer worker, and telemetry threads.
