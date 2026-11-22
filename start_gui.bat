@echo off
REM Launcher for Smart Campus Print Spooler Web GUI Dashboard
echo ==============================================================================
echo  Launching Smart Campus Print Spooler GUI Dashboard
echo ==============================================================================
cd /d "%~dp0"
python gui\server.py
pause
