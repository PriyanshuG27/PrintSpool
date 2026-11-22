#include "disk_scan.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

int main() {
    std::cout << "========================================================\n";
    std::cout << " TC-04: SCAN Disk Scheduling Seek Optimization Test\n";
    std::cout << "========================================================\n";

    uint32_t initial_head = 53;
    bool initial_dir_up = true;
    std::vector<uint32_t> requests = {98, 183, 37, 122, 14, 124, 65, 67};
    uint32_t max_cylinder = 199;

    std::cout << "Initial Head: " << initial_head << " (Direction: UP)\n";
    std::cout << "Cylinder Range: 0 to " << max_cylinder << "\n";
    std::cout << "Request Sequence: [";
    for (size_t i = 0; i < requests.size(); ++i) {
        std::cout << requests[i] << (i + 1 < requests.size() ? ", " : "");
    }
    std::cout << "]\n\n";

    ScanSimulationResult result = ScanDiskController::simulate_scan_vs_fcfs(
        initial_head, initial_dir_up, requests, max_cylinder);

    std::cout << "--- FCFS Scheduling Execution ---\n";
    std::cout << "Sequence: 53";
    for (uint32_t cyl : result.fcfs_serviced_order) {
        std::cout << " -> " << cyl;
    }
    std::cout << "\nTotal FCFS Head Movement: " << result.total_head_movement_fcfs << " cylinders\n\n";

    std::cout << "--- SCAN (Elevator) Execution ---\n";
    std::cout << "Sequence: 53";
    for (uint32_t cyl : result.scan_serviced_order) {
        std::cout << " -> " << cyl;
    }
    std::cout << "\nTotal SCAN Head Movement: " << result.total_head_movement_scan << " cylinders\n";
    std::cout << "Mechanical Travel Reduction: " << result.improvement_percentage << "%\n\n";

    // Verification against project specifications:
    // FCFS must be 640 cylinders
    // SCAN must be 331 cylinders (146 up to 199 + 185 down to 14)
    // Reduction must be ~48.3%
    assert(result.total_head_movement_fcfs == 640);
    assert(result.total_head_movement_scan == 331);
    assert(std::fabs(result.improvement_percentage - 48.28125) < 0.1);

    std::cout << "[PASS] TC-04 Verified: SCAN requires 331 cylinders vs 640 for FCFS (48.3% reduction)\n";
    return 0;
}
