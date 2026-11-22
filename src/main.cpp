#include "common.h"
#include "clock.h"
#include "logger.h"
#include "buffer.h"
#include "ingest.h"
#include "memory_arena.h"
#include "swap_engine.h"
#include "disk_scan.h"
#include "scheduler.h"
#include "printer_pool.h"
#include "telemetry.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

void print_help(const char* prog) {
    std::cout << "Smart Campus Print Spooler Daemon\n"
              << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --workload <file>     Replay workload file and run simulation\n"
              << "  --printers <N>        Number of hardware printer workers (default: 3)\n"
              << "  --page-time <sec>     Simulated seconds to print one page (default: 0.2)\n"
              << "  --realtime            Run with wall-clock pacing (default: fast virtual time)\n"
              << "  --no-hud              Disable interactive ANSI HUD display\n"
              << "  --ipc                 Run in IPC streaming mode for GUI frontend\n"
              << "  --help                Display this help screen\n";
}

int main(int argc, char* argv[]) {
    std::string workload_file = "";
    int num_printers = DEFAULT_PRINTER_WORKERS;
    bool enable_hud = true;
    bool realtime_mode = false;
    bool ipc_mode = false;
    double page_time = -1.0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workload_file = argv[++i];
        } else if (arg == "--printers" && i + 1 < argc) {
            try { num_printers = std::stoi(argv[++i]); } catch (...) { num_printers = 0; }
            if (num_printers < 1 || num_printers > 64) {
                std::cerr << "Error: --printers expects an integer between 1 and 64\n";
                return 2;
            }
        } else if (arg == "--page-time" && i + 1 < argc) {
            try { page_time = std::stod(argv[++i]); } catch (...) { page_time = -1.0; }
            if (page_time <= 0.0) {
                std::cerr << "Error: --page-time expects a positive number of simulated seconds per page\n";
                return 2;
            }
        } else if (arg == "--no-hud") {
            enable_hud = false;
        } else if (arg == "--realtime") {
            realtime_mode = true;
        } else if (arg == "--ipc") {
            ipc_mode = true;
            enable_hud = false;
        } else if (arg == "--help") {
            print_help(argv[0]);
            return 0;
        }
    }

    SystemConfig config;
    config.printer_workers = num_printers;
    config.use_virtual_time = !realtime_mode;
    if (page_time > 0.0) config.page_print_time_sec = page_time;

    SimClock::instance().set_mode(!realtime_mode, realtime_mode ? 1.0 : 100.0);
    Logger::instance().init("", LOG_INFO);
    if (ipc_mode) {
        Logger::instance().set_ipc_mode(true);
        Logger::instance().enable_console(false);
    } else {
        Logger::instance().enable_console(!enable_hud);
        std::cout << "Starting Smart Campus Print Spooler Daemon...\n";
    }

    // 1. Initialize Memory Arena (Module 5)
    MemoryArena::instance().init(config.arena_size);

    // 2. Initialize SCAN Disk Controller (Module 7)
    ScanDiskController disk("swap.img", config.cylinder_count);
    if (!disk.start()) {
        std::cerr << "Failed to start SCAN disk controller!\n";
        return 1;
    }

    // 3. Initialize LRU Swap Engine (Module 6)
    SwapEngine swap_engine(&disk);

    // 4. Initialize Bounded Ring Buffer (Module 2)
    BoundedRingBuffer ring_buffer(config.buffer_capacity);

    // 5. Initialize Ingestion Controller (Module 1)
    JobIngestionController ingestion(&ring_buffer);

    // 6. Initialize Dispatcher Scheduler (Module 3)
    Scheduler scheduler(&ring_buffer, &swap_engine, config);
    if (!scheduler.start()) {
        std::cerr << "Failed to start Scheduler!\n";
        return 1;
    }

    // 7. Initialize Printer Worker Pool (Module 4)
    PrinterWorkerPool printer_pool(&scheduler, config.printer_workers, config);
    if (!printer_pool.start()) {
        std::cerr << "Failed to start Printer Worker Pool!\n";
        return 1;
    }

    // 8. Initialize Telemetry HUD (Module 8)
    TelemetryDashboard telemetry(&ring_buffer, &scheduler, &MemoryArena::instance(), &disk, &printer_pool);
    printer_pool.set_telemetry(&telemetry);
    if (ipc_mode) {
        telemetry.set_ipc_mode(true);
    } else if (enable_hud) {
        telemetry.start(4.0); // 4 Hz refresh
    }

    // If workload file provided, replay it
    if (!workload_file.empty()) {
        if (!ipc_mode) {
            std::cout << "Loading workload from: " << workload_file << "\n";
        }
        if (!ingestion.replay_workload_file(workload_file, true)) {
            std::cerr << "Error: could not read workload file '" << workload_file << "'\n";
            if (enable_hud) telemetry.stop();
            scheduler.stop();
            printer_pool.stop();
            ring_buffer.shutdown();
            disk.stop();
            return 1;
        }
    } else if (!ipc_mode) {
        // Default sample interactive / demonstration workload (only in CLI mode)
        std::cout << "Submitting sample campus jobs...\n";
        ingestion.submit_job(1, "faculty_01", ROLE_FACULTY, 25, true, true, "Syllabus_Fall.pdf");
        ingestion.submit_job(2, "student_01", ROLE_STUDENT, 3, false, false, "Assignment_1.pdf");
        ingestion.submit_job(3, "research_01", ROLE_RESEARCHER, 12, false, true, "Paper_Draft.pdf");
        ingestion.submit_job(4, "student_02", ROLE_STUDENT, 1, false, false, "Receipt.pdf");
        ingestion.submit_job(5, "faculty_02", ROLE_FACULTY, 40, false, true, "Exam_Booklet.pdf");
    }

    if (ipc_mode) {
        std::atomic<bool> is_paused{false};
        std::atomic<bool> stop_requested{false};
        std::atomic<double> time_speed{1.0};
        std::atomic<uint32_t> next_auto_id{100};

        std::thread stdin_thread([&]() {
            std::string line;
            while (!stop_requested.load() && std::getline(std::cin, line)) {
                if (line.empty()) continue;
                std::istringstream iss(line);
                std::string cmd;
                iss >> cmd;
                if (cmd == "SUBMIT") {
                    uint32_t id;
                    std::string client_id, role_str, doc_name;
                    uint32_t pages;
                    int urgency_val = 0, duplex_val = 0;
                    if (iss >> id >> client_id >> role_str >> pages >> urgency_val >> duplex_val) {
                        if (!(iss >> doc_name)) {
                            doc_name = "doc_" + std::to_string(id) + ".pdf";
                        }
                        if (id == 0) id = next_auto_id.fetch_add(1);
                        UserRole role = string_to_user_role(role_str);
                        bool ok = ingestion.submit_job(id, client_id, role, pages, urgency_val != 0, duplex_val != 0, doc_name);
                        std::cout << "{\"type\":\"submit_ack\",\"id\":" << id
                                  << ",\"success\":" << (ok ? "true" : "false")
                                  << ",\"accepted\":" << ingestion.accepted_jobs_count()
                                  << ",\"rejected\":" << ingestion.rejected_jobs_count() << "}\n" << std::flush;
                    }
                } else if (cmd == "PAUSE") {
                    is_paused.store(true);
                    std::cout << "{\"type\":\"pause_ack\"}\n" << std::flush;
                } else if (cmd == "RESUME") {
                    is_paused.store(false);
                    std::cout << "{\"type\":\"resume_ack\"}\n" << std::flush;
                } else if (cmd == "STEP") {
                    double delta = 0.1;
                    iss >> delta;
                    SimClock::instance().advance_seconds(delta);
                    std::cout << "{\"type\":\"step_ack\",\"time\":" << SimClock::instance().now() << "}\n" << std::flush;
                } else if (cmd == "SPEED") {
                    double spd = 1.0;
                    iss >> spd;
                    time_speed.store(spd);
                    std::cout << "{\"type\":\"speed_ack\",\"speed\":" << spd << "}\n" << std::flush;
                } else if (cmd == "STOP") {
                    stop_requested.store(true);
                    std::cout << "{\"type\":\"stop_ack\"}\n" << std::flush;
                    break;
                }
            }
            stop_requested.store(true);
        });

        auto last_telem_tp = std::chrono::steady_clock::now();
        while (!stop_requested.load()) {
            if (!is_paused.load()) {
                if (config.use_virtual_time) {
                    bool has_work = (ring_buffer.size() > 0 || scheduler.unfinished_count() > 0);
                    if (has_work) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        SimClock::instance().advance_seconds(0.02 * time_speed.load());
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            auto now_tp = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_telem_tp).count() >= 100) {
                last_telem_tp = now_tp;
                telemetry.render_json_frame(std::cout);
                std::cout << std::flush;
            }
        }

        if (stdin_thread.joinable()) {
            stdin_thread.detach();
        }

        scheduler.stop();
        printer_pool.stop();
        ring_buffer.shutdown();
        telemetry.dump_gantt_csv("gantt_timeline.csv");
        disk.stop();
        std::cout << "{\"type\":\"stopped\"}\n" << std::flush;
        return 0;
    }

    // Let the simulation run until every accepted job has COMPLETED.
    // "Drained" means: the scheduler owns all accepted jobs (none still in the ring buffer or in
    // transit) and none is unfinished - this also covers swapped-out and currently printing jobs.
    const uint32_t accepted = ingestion.accepted_jobs_count();
    size_t last_progress = 0;
    auto last_progress_time = std::chrono::steady_clock::now();
    bool stalled = false;

    while (true) {
        if (config.use_virtual_time) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            SimClock::instance().advance_seconds(0.05);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (ring_buffer.size() == 0 &&
            scheduler.total_jobs_count() == accepted &&
            scheduler.unfinished_count() == 0) {
            break;
        }

        // Watchdog: abort with a diagnostic instead of hanging forever if nothing makes progress
        size_t progress = scheduler.completed_count() + printer_pool.total_pages_printed() + swap_engine.active_swapped_count();
        auto now_tp = std::chrono::steady_clock::now();
        if (progress != last_progress) {
            last_progress = progress;
            last_progress_time = now_tp;
        } else if (now_tp - last_progress_time > std::chrono::seconds(120)) {
            stalled = true;
            break;
        }
    }

    // Shut down workers first so every job field is quiescent (thread joins give happens-before),
    // then read final statistics.
    if (enable_hud) {
        telemetry.stop();
    }
    scheduler.stop();
    printer_pool.stop();
    ring_buffer.shutdown();

    telemetry.print_summary_report();
    telemetry.dump_gantt_csv("gantt_timeline.csv");

    disk.stop();

    if (stalled) {
        std::cerr << "ERROR: simulation stalled - no progress for 120 s (unfinished jobs: "
                  << scheduler.unfinished_count() << ")\n";
        return 3;
    }
    if (illegal_transition_count() != 0) {
        std::cerr << "ERROR: " << illegal_transition_count() << " illegal state transition(s) were detected\n";
        return 4;
    }

    std::cout << "Daemon exited cleanly. All resources freed.\n";
    return 0;
}
