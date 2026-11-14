#pragma once
// Process lifecycle + console, the parts that differ between Linux and
// Windows. The same file in heimdall_v2/src/core/ and
// kraken_doa_v2/include/utils/ (platform.cpp next to each: identical but for
// its #include line) - keep them identical. On Linux every function does what
// the code did before it existed.
#include <cstdint>
#include <string>

namespace platform {

// Stop requests -> on_stop(sig). on_stop only records the request (it may run
// in a signal handler on Linux, on a system thread on Windows).
//  Linux:   SIGINT, SIGTERM, SIGHUP (tmux kill-session / a closed terminal)
//  Windows: Ctrl+C (SIGINT) and Ctrl+Break (SIGBREAK) in the console, the
//           console window closing (SIGTERM; the OS ends the process ~5 s
//           later - see stop_is_urgent), logoff / shutdown / restart
//           (SIGTERM, urgent too: WM_ENDSESSION to a hidden window - with
//           user32 loaded the console handler never sees them), taskkill
//           without /F (SIGTERM, WM_CLOSE to that window), and a stop asked
//           for through the named event "Local\KrakenSDR_Stop_<pid>" (SIGTERM;
//           krakensdr.exe stop, the launcher's supervisors) or
//           "Local\KrakenSDR_StopUrgent_<pid>" (SIGTERM, urgent: the launcher
//           at a logoff / its window closed - it is ended soon). A console
//           control handler, not signal(): the UCRT resets SIGINT to the
//           default after the first Ctrl+C, so a second one killed the
//           process in the middle of its shutdown.
void install_stop_handler(void (*on_stop)(int sig));

// true once the OS will end the process soon whatever it does (Windows: the
// console window was closed or the session ends - about 5 s). Never on Linux.
bool stop_is_urgent();

// Sleeps up to max_ms; returns early once a stop was requested (Windows; on
// Linux it is the plain sleep it replaces).
void wait_stop_request(int max_ms);

// stdout is an interactive terminal (dashboard / live logs) - not a file,
// pipe or NUL. On Windows: a real console (_isatty() is true for NUL too).
bool stdout_is_terminal();

// What stdout is, for "discard routine output when logging to a file":
// Console = a terminal (the dashboard can run there); Pty = an interactive
// terminal reached through a pipe (Windows: mintty / MSYS2 / Git Bash - keep
// the output, but no dashboard: no console size there); Redirected = a
// file, pipe or NUL. Linux: Console or Redirected (isatty).
enum class StdoutKind { Console, Pty, Redirected };
StdoutKind stdout_kind();

// First thing in main() (Windows; no-op on Linux): UTF-8 console output
// (the manifest's UTF-8 code page doesn't change the console's), and
// QuickEdit off - a mouse click in a classic console window otherwise
// pauses every thread that writes to it until Esc (USB callbacks logging,
// the data receiver). Both restored by console_restore().
void console_init();

// Ends the process at once, without running destructors or (Windows) DLL
// detach code: _Exit on Linux, TerminateProcess on Windows (ExitProcess
// would run DLL_PROCESS_DETACH while other threads were killed holding locks).
[[noreturn]] void fast_exit(int code);

// Directory of the running executable (UTF-8, no trailing separator).
std::string exe_dir();

// Windows: when `marker` (a file the app loads from its working directory,
// e.g. index.html) is not in the cwd but next to the executable - started
// from a shortcut or another folder - make the exe's folder the cwd, which
// every runtime file path of the apps is relative to. No-op on Linux.
void chdir_to_exe_dir_if_missing(const char* marker);

// Console for the dashboard: ANSI/VT escape sequences on and UTF-8 output
// (box-drawing characters, degree signs). Windows only; false if stdout is no
// console. console_restore() puts the code page back (at exit).
bool console_enable_vt();
void console_restore();

// Visible console window size; false if unknown.
bool console_size(int* cols, int* rows);

// System load for the dashboards / status: cumulative CPU counters (idle,
// total; any unit - use the difference of two calls), RAM in bytes, CPU
// temperature in degrees C (-1 = unknown; always on Windows: no unprivileged
// source). Linux: /proc/stat, /proc/meminfo (MemTotal, MemAvailable),
// /sys/class/thermal/thermal_zone0.
bool cpu_times(uint64_t* idle, uint64_t* total);
bool mem_info(uint64_t* total_bytes, uint64_t* available_bytes);
float cpu_temperature_c();

// This machine's host name ("" if unknown) and, on Windows, its DNS FQDN
// when it is in a domain ("" otherwise; always "" on Linux, where the host
// name is what gethostname gives).
std::string host_name();
std::string host_fqdn();

// Windows: keep the PC from sleeping while the receiver runs (sleep stops
// the USB streams); ends with the process. macOS: an IOKit assertion
// against idle sleep, the same. No-op on Linux.
void keep_system_awake();

// "windows", "linux" or "macos": the web UIs show the matching instructions
// (status JSON "platform").
const char* os_name();

// Windows: the process at ABOVE_NORMAL priority (heimdall: the USB
// completion threads libusb creates can't be raised one by one). No-op on
// Linux.
void raise_process_priority();

}  // namespace platform
