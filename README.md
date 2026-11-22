# Smart Campus Print Spooler Daemon

An asynchronous, multi-threaded print-management daemon written in **C++17** using direct **POSIX threads (`pthreads`)** that simulates modern Operating System resource-management primitives entirely in user space.

---

## 👥 Team & Module Ownership

| Name | Roll Number | Role / Module Ownership | Key Files |
| :--- | :--- | :--- | :--- |
| **Priyanshu Gumber** | 2401030082 | Lead Systems Architect & Concurrency Engineer<br>• **Module 1**: Network & Ingestion Controller<br>• **Module 2**: Bounded Buffer Concurrency Manager | `include/ingest.h`<br>`src/ingest.cpp`<br>`include/buffer.h`<br>`src/buffer.cpp` |
| **Rashi Johari** | 2401030072 | Scheduler & Algorithm Specialist<br>• **Module 3**: Hybrid SRTF & Priority Aging Dispatcher<br>• **Module 4**: Emulated Hardware Printer Worker Pool | `include/scheduler.h`<br>`src/scheduler.cpp`<br>`include/printer_pool.h`<br>`src/printer_pool.cpp` |
| **Anant Kumar** | 2401030080 | Memory Management & Arena Engineer<br>• **Module 5**: Contiguous 64 KB Memory Arena Allocator<br>• **Module 6**: Virtual Memory & LRU Swap Engine | `include/memory_arena.h`<br>`src/memory_arena.cpp`<br>`include/swap_engine.h`<br>`src/swap_engine.cpp` |
| **Ram Pratap Singh** | 2401030084 | I/O Storage Controller & Telemetry Engineer<br>• **Module 7**: SCAN Disk Scheduling Controller<br>• **Module 8**: Telemetry, Gantt & ASCII HUD Dashboard | `include/disk_scan.h`<br>`src/disk_scan.cpp`<br>`include/telemetry.h`<br>`src/telemetry.cpp` |

---

## 🏗️ Architecture Overview

```
 CLI / Workload Replayer / Sockets
            │
   [M1 Ingestion] ── validate & construct JCB ──┐
                                                ▼
                             [M2 Bounded Ring Buffer]  (capacity: 16, mutex + 2 condvars)
                                                │
                                                ▼
              [M3 Dispatcher: SRTF + Role Priority + Dynamic Aging]  ◄── Job Table
                      │                         
        request memory│                         
                      ▼                         
        [M5 First-Fit Arena, 64 KB] ──full──► [M6 LRU Swap Engine]
                      │                                  │ I/O requests
                      │ allocated                        ▼
                      ▼                        [M7 SCAN Disk Controller]
        [M4 Printer Worker Pool]  ◄── jobs        (200 cylinders, swap.img)
          (N threads, free arena on done)

   [M8 Telemetry]: Non-blocking snapshots of M2–M7 → ASCII HUD, Gantt timeline, logs
```

---

## 🔄 Job State Machine

```
   [SUBMITTED] ──(Validation Passed)──► [WAITING]
                                            │
                      ┌─────────────────────┴─────────────────────┐
                      ▼ (RAM Allocated)                           ▼ (RAM Pressure)
                [IN_MEMORY] ◄──(Swapped In from Disk)─── [SWAPPED_TO_DISK]
                      │  ▲
        (Evicted LRU) │  │ (Restored)
                      ▼  │
             [SWAPPED_TO_DISK]
                      │
                      ▼ (Printer Worker Claims Job)
                 [PRINTING]
                      │
                      ▼ (Remaining Pages == 0)
                [COMPLETED]
```

---

## 🛠️ Build & Compilation

The project uses a GNU `Makefile` supporting Release, Debug, AddressSanitizer (`ASan`), UndefinedBehaviorSanitizer (`UBSan`), and ThreadSanitizer (`TSan`).

### Prerequisites
- GCC / G++ 11.4+ or Clang 14.0+ with POSIX threads (`-pthread`)
- Linux / WSL2 or MinGW-w64 on Windows

### Build Targets

```bash
# 1. Standard optimized release build (daemon + all 4 test suites)
make release

# 2. Debug build with symbols (-g -O0 -DDEBUG)
make debug

# 3. AddressSanitizer & UndefinedBehaviorSanitizer build
make asan

# 4. ThreadSanitizer race detector build
make tsan

# 5. Clean binaries and generated swap/log files
make clean
```

---

## 🧪 Verification & Test Suite

Run all verification tests with one command:
```bash
make test
```

### Verified Test Cases

| Test Case | Description | Expected Specification | Verification Result |
| :--- | :--- | :--- | :--- |
| **TC-01** | **Starvation Elimination via Dynamic Aging** | 80-page Faculty job submitted with continuous 1-page Student jobs. Aging dynamically raises priority. | **PASSED**: Faculty job dispatched at $T = 11.4$ s before indefinite starvation (matches the $T \approx 11.4$ s benchmark). |
| **TC-02** | **Bounded Buffer Concurrency & Deadlock Stress** | 10 producer threads each submitting 50 jobs into a capacity-16 buffer with 3 consumer threads. | **PASSED**: Exactly 500/500 jobs processed without race or deadlock. Verified clean under **ThreadSanitizer** (0 races). |
| **TC-03** | **First-Fit Arena Allocation & Swap Triggering** | Arena filled to ~92% occupancy; 16 KB job submitted; LRU victim evicted to `swap.img`. | **PASSED**: Reclaimed ~19 KB via swap-out; 16 KB job allocated; consistency check passed; swap-in restored successfully. |
| **TC-04** | **SCAN Disk Scheduling Seek Optimization** | Head at 53 moving UP on requests `[98, 183, 37, 122, 14, 124, 65, 67]`. Range 0..199. | **PASSED**: SCAN traversed **331 cylinders** vs **640 cylinders** for FCFS (**48.3% mechanical seek reduction**). |
| **TC-05** | **Swap-Path Integrity (regression)** | Arena filled with idle jobs; an urgent job forces an LRU eviction of a job still queued for printing; printers then drain everything. | **PASSED**: every job prints exactly once, pages printed == pages submitted, 0 illegal state transitions, arena fully released, no leaked swap cylinders. |

---

## 🖥️ Interactive Web GUI Frontend

In addition to the terminal HUD, the project includes a full graphical web dashboard featuring real-time visualization of all 8 OS modules, dynamic runtime job submission, pre-execution staging, and traffic burst generators.

### Launching the GUI

#### Option 1: One-Click Launch (Windows)
Double-click `start_gui.bat` in either the root project folder or `PrintSpooler-main/`. It will start the IPC server and automatically open your default browser at:
```
http://localhost:8080
```

#### Option 2: Command Line
```bash
python gui/server.py
```

### Key Frontend Capabilities
1. **Dynamic Runtime Job Ingestion**:
   - Submit new print jobs dynamically on the fly *while the spooler daemon is actively executing*.
   - Adjust Role (Student, Researcher, Faculty), Page count (1–100), Urgency (+20 priority score bonus), Duplex printing, and Document name.
   - Immediate feedback as the Ingestion Controller validates and enqueues the job into the Bounded Buffer in real time.
2. **Pre-Execution Staging & Benchmark Presets**:
   - Stage custom job queues before starting the daemon.
   - Built-in one-click benchmark presets:
     - *Campus Daily Operations*: Standard balanced campus workload.
     - *TC-01: Starvation & Priority Aging Demo*: 80-page Faculty job competing with short student jobs.
     - *TC-03: Memory Pressure & LRU Swap Stress*: Large documents filling 64 KB RAM and forcing disk swapping.
3. **Interactive Subsystem Visualizers**:
   - **Bounded Buffer (Module 2)**: 16 visual slots with live occupancy animation.
   - **Printer Worker Pool (Module 4)**: Live printer cards with animated page progress bars, duplex status, and active job details.
   - **First-Fit Memory Arena (Module 5)**: Continuous color-coded 64 KB memory bar showing allocated blocks, sizes, offsets, and free holes.
   - **SCAN Disk Controller (Module 7)**: Visual 200-cylinder rail with animated head movement, direction (UP/DOWN), and seek counters.
   - **Virtual Memory Swap Engine (Module 6)**: Table of LRU evicted jobs residing on `swap.img`.
   - **Dynamic Aging & SRTF Scheduler (Module 3)**: Live priority queue table with calculated scores and dynamic starvation aging bonus tags ($T_{wait} > 4.0$ s).
   - **Gantt Chart & Timeline (Module 8)**: Visual timeline of printer allocations with CSV export.
   - **Real-Time Event Log**: Filterable streaming terminal of all OS events and state machine transitions.

---

## 🚀 Running the Daemon (CLI Mode)

```bash
# Replay campus workload with interactive ASCII HUD
./print_spooler --workload workloads/sample_campus.txt

# Run in headless mode (logs only)
./print_spooler --workload workloads/sample_campus.txt --no-hud

# Run with custom number of printer workers (e.g. 5 hardware printers)
./print_spooler --workload workloads/sample_campus.txt --printers 5

# Run in real-time wall clock mode (jobs overlap, so the LRU swap path is exercised)
./print_spooler --workload workloads/sample_campus.txt --realtime

# Slow the simulated printers down (sim-seconds per page) to create RAM pressure in fast mode
./print_spooler --workload workloads/stress_swap.txt --printers 2 --page-time 0.4 --no-hud
```

The process exits `0` only if every accepted job completed and no illegal state transition occurred
(`3` = stalled for 120 s, `4` = illegal transition detected).

---

## 📊 Live Telemetry HUD Interface

When running in HUD mode, the dashboard displays real-time updates at $\ge 2$ Hz:

```
==============================================================================
          SMART CAMPUS PRINT SPOOLER - LIVE TELEMETRY DASHBOARD               
==============================================================================
 Sim Clock: 11.50 s | Total Pages Printed: 75 | Completed Jobs: 6
------------------------------------------------------------------------------
 [M2 Bounded Buffer] Occupancy (0/16): [........................] 0.0%
 [M3 Dispatcher]     Ready Pool: 1 | In RAM: 1 | Swapped: 0 | Completed: 6
 [M5 64 KB Arena]    Usage (37960/65536 B): [##############..........] 57.9%
 [M7 SCAN Disk]      Cylinder: [--------*-----------------------] 53/199 (UP ▲) | Seeks: 0 | Queue: 0
------------------------------------------------------------------------------
 [M4 Printer Worker Pool (3 Hardware Printers)]:
   Printer #0: BUSY  | Job #10 (FACULTY, Duplex) [###############.] (18/18 p)
   Printer #1: BUSY  | Job #1 (FACULTY, Duplex)   [###########.....] (28/35 p)
   Printer #2: BUSY  | Job #6 (FACULTY, Duplex)   [##..............] (6/50 p)
==============================================================================
```
