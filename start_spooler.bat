@echo off
REM Windows launcher for Smart Campus Print Spooler via WSL
echo Launching Smart Campus Print Spooler Daemon...
for /f "delims=" %%i in ('wsl -d Ubuntu-24.04 wslpath "%~dp0"') do set "WSL_DIR=%%i"
wsl -d Ubuntu-24.04 bash -c "cd '%WSL_DIR%' && ./print_spooler --workload workloads/sample_campus.txt"
pause
