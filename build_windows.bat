@echo off
rem build_windows.bat - build the KrakenSDR v2 suite on Windows (the
rem install.sh of the Windows build): installs MSYS2 if it isn't there (no
rem administrator rights needed), then runs windows\build.sh in its UCRT64
rem environment - packages, librtlsdr fork, heimdall.exe, kraken_doa.exe, the
rem decoder plugins and krakensdr.exe. Start the stack afterwards with run.bat.
rem
rem   build_windows.bat              build (incremental)
rem   build_windows.bat clean        clean rebuild
rem   set JOBS=4 / set MSYS2_ROOT=D:\msys64 before running to override
rem   set WIN_MARCH=-march=x86-64-v2 for a CPU without AVX2 (WINDOWS.md)
setlocal
cd /d "%~dp0"
rem the build scripts print UTF-8 (check marks, arrows)
chcp 65001 >nul

for /f "usebackq tokens=1,* delims==" %%a in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows\setup_msys2.ps1"`) do (
    if "%%a"=="MSYS2_ROOT" set "MSYS2_DIR=%%b"
)
if not defined MSYS2_DIR (
    echo Error: MSYS2 could not be found or installed - see WINDOWS.md
    goto :fail
)

if /i "%~1"=="clean" set CLEAN=1
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
"%MSYS2_DIR%\usr\bin\bash.exe" -lc "windows/build.sh"
if errorlevel 1 goto :fail
echo.
echo Done. Start the stack with run.bat
endlocal
exit /b 0

:fail
echo.
echo Build FAILED - see the messages above.
endlocal
pause
exit /b 1
