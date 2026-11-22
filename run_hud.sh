#!/usr/bin/env bash
# Script to launch the Smart Campus Print Spooler with real-time ANSI HUD
cd "$(dirname "$0")"
./print_spooler --workload workloads/sample_campus.txt --realtime
