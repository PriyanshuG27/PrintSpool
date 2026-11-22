# Bug-fix notes: double-claimed job / runaway page counter

## Symptom
Dashboard showed two printers on the same job (`Job #8 ... 271/22 p`), `Completed` stuck at 9, the arena
never released the job's 16 KB block, `Total Pages Printed` grew forever and the daemon never exited.

## Root causes (all fixed)
| # | Defect | Effect | Fix |
|---|--------|--------|-----|
| 1 | `SwapEngine::swap_out()` evicted a job that was still in the scheduler's `in_memory_queue_` and left it there | A SWAPPED job could be popped by a printer; after swap-in the dispatcher queued it a second time -> two printers on one job | Dispatcher removes the victim from the ready queue (under the scheduler lock) *before* swapping it out; victim chosen with the LRU policy among lower-priority queued jobs |
| 2 | Printer "claim" (`IN_MEMORY -> PRINTING`) happened after the job had been popped, outside any lock; its return value was ignored | Window for eviction/duplicate dispatch; illegal transitions printed but the job was printed anyway | `acquire_job_for_printer(worker_id, ...)` pops **and** claims under the scheduler lock; `transition_state()` is an atomic compare-and-set |
| 3 | `remaining_pages` was `uint32_t`, decremented by two workers | Wrapped below 0 -> 4 billion pages, never completed | One owner per job, atomic counter, loop only decrements while > 0 |
| 4 | Dispatcher never called `swap_in()` for a SWAPPED job; it allocated fresh RAM instead | Disk read skipped, swap cylinder leaked forever | Dispatcher uses `swap_in()` (disk I/O done outside the scheduler lock) |
| 5 | `PrinterWorkerPool::stop()` waited on the *scheduler's* running flag | Deadlock when the pool is stopped first | Workers also honour the pool's own atomic flag |
| 6 | Drain check in `main` ignored SWAPPED / just-claimed jobs and jobs in transit from the ring buffer | Could exit early or hang | Exit when the scheduler owns every accepted job and `unfinished_count() == 0`; 120 s watchdog |
| 7 | Plain `bool`/`int` flags and job fields shared between threads (TSan found 4 races) | Undefined behaviour | `std::atomic` for job `state`, `remaining_pages`, `arena_offset`, `assigned_printer_id`, and the pool/telemetry flags |
| 8 | `MemoryArena::deallocate()` did not verify the owner; "Failed to allocate" logged at WARN every 20 ms | Wrong job could free a block; console flooded | Owner check, stale headers invalidated on coalesce, failure logged at DEBUG, `can_allocate()` pre-check |
| 9 | HUD printed raw `current_page` | `271/22` | Clamped to `total_pages` |

## Verification
* `make test` (TC-01..TC-05) passes; TC-05 fails on the original code (illegal transitions + leaked cylinder).
* Original code with `--realtime`: never terminates (4/4 runs); fixed code: 155 pages, 2 swap-outs, 2 swap-ins,
  743 cylinders of head travel, exit 0 (4/4 + 12/12 runs).
* ThreadSanitizer, AddressSanitizer and UBSan: 0 reports on all tests and on the full daemon.
* 40-run soak across 4 printer/workload configurations: 40/40 clean.
