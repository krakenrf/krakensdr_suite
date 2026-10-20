@echo off
rem run.bat - start the KrakenSDR v2 stack on Windows (what run.sh is on
rem Linux): Heimdall and the KrakenSDR DoA client with their dashboards, the
rem client after Heimdall's phase calibration has converged. All options of
rem krakensdr.exe pass through, e.g.:
rem   run.bat                 start (Windows Terminal split, else two windows)
rem   run.bat stop            stop everything
rem   run.bat --headless      no dashboards, logs in logs\
rem   run.bat --help          all options (--wideband, --kerberos, ...)
cd /d "%~dp0"
if not exist "%~dp0krakensdr.exe" (
    echo krakensdr.exe not found - build the suite first with build_windows.bat
    pause
    exit /b 1
)
"%~dp0krakensdr.exe" %*
if errorlevel 1 pause
