#pragma once
// One child-process API for kraken_doa and its plugins (kproc).
//   Linux:   posix_spawn / fork + exec, pipe2(O_CLOEXEC), closefrom(3), poll,
//            waitpid, kill / killpg - the code dig_plugin.cpp, ai_manager.cpp,
//            playback.cpp and voice2text.cpp had, moved here unchanged
//            (src/utils/process_posix.cpp)
//   Windows: CreateProcessW with an explicit inherited-handle list (the
//            closefrom(3) equivalent), OVERLAPPED named pipes - Windows pipes
//            have no poll() and no O_NONBLOCK; one read is kept outstanding per
//            output pipe and one write per stdin pipe, which gives the same
//            non-blocking write_some / read_some / wait_io semantics on one
//            thread - and Job objects (src/utils/process_win.cpp)
// No <windows.h> and no POSIX headers here (their macros: ERROR, min, max).
// Thread rules: one thread at a time drives a Process (spawn, I/O, try_wait,
// close); kill(), terminate() and pid() may be called from any thread, also
// while another thread blocks in wait() / read().
// plugins/Makefile compiles the same sources into libkrakendig.a (the TETRA
// codec helpers of plugins/lib/dig_vocoder.cpp).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kproc {

// ---- executables / PATH ----------------------------------------------------
#ifdef _WIN32
inline constexpr const char* EXE_SUFFIX = ".exe";
inline constexpr char PATH_LIST_SEP = ';';
#else
inline constexpr const char* EXE_SUFFIX = "";
inline constexpr char PATH_LIST_SEP = ':';
#endif

// "plugins/x/build/decoder" -> itself on Linux, "plugins/x/build/decoder.exe"
// on Windows (unchanged if it already ends in .exe, any case)
std::string exe_path(const std::string& path_without_suffix);

// Linux: a regular file with access(X_OK). Windows: a regular file ending in
// .exe or .com, or an App Execution Alias (WindowsApps\python3.exe) - MinGW's
// access(X_OK) is true for ANY existing file
bool is_executable(const std::string& path);

// execvp-style lookup of a bare name (no '/', on Windows also no '\\' or ':'):
// every PATH entry in order; Windows tries name, name.exe, name.com and never
// the current or the application directory (CreateProcess's own search would).
// "" if not found.
std::string find_in_path(const std::string& name);

// The Python 3 interpreter as an argv prefix, resolved once (thread-safe):
//   Linux:   {"python3"} (spawn with search_path = true, as posix_spawnp did)
//   Windows: {<path>, "-X", "utf8"}: $KRAKEN_PYTHON, else "py -3", else
//            python3.exe / python.exe on PATH - each probed with a 5 s run of
//            `-c "import sys; sys.exit(sys.version_info[0] != 3)"`, so the
//            Microsoft Store stub (exit 9009) is skipped. Empty = none found.
const std::vector<std::string>& python_argv();
std::vector<std::string> python_cmd(std::vector<std::string> script_and_args);   // python_argv() + args

// Lower the CALLING process's priority (child side, kraken_whisper):
// Linux setpriority(PRIO_PROCESS, 0, 10); Windows BELOW_NORMAL_PRIORITY_CLASS
void lower_own_priority();

// ---- spawning ---------------------------------------------------------------
enum class Stdio : uint8_t {
    Inherit,        // the parent's own stream (Windows: an inheritable duplicate of GetStdHandle; NUL if none)
    Null,           // /dev/null (Windows: NUL)
    Pipe,           // a pipe to this Process object (parent end non-blocking / overlapped)
    SameAsStdout,   // stderr only: the very same pipe / handle as stdout (2>&1)
    Handle,         // an end of a PipePair (chaining two children)
};
enum class Stream : uint8_t { Out = 1, Err = 2 };
enum class Priority : uint8_t {
    Normal,
    Below,          // Linux nice 10; Windows BELOW_NORMAL_PRIORITY_CLASS (inherited by grandchildren)
    Idle,           // Linux nice 19; Windows IDLE_PRIORITY_CLASS
};
enum class Console : uint8_t {   // Windows only (ignored on Linux)
    Auto,           // None when no stream is Inherit, else Share
    Share,          // our console; Hidden when we have none
    Hidden,         // CREATE_NO_WINDOW: its own invisible console, shared by its console grandchildren
    None,           // DETACHED_PROCESS: no console at all - only for leaves (a console grandchild of a
                    // detached process opens a VISIBLE window)
};

class PipePair;   // below

struct SpawnOptions {
    std::vector<std::string> argv;            // UTF-8; argv[0] = program (a path, or a bare name with search_path)
    bool search_path = false;                 // posix_spawnp semantics (find_in_path) for a bare argv[0]
    Stdio in = Stdio::Inherit, out = Stdio::Inherit, err = Stdio::Inherit;
    const PipePair* in_pair = nullptr;        // in == Handle: the pair's read end
    const PipePair* out_pair = nullptr;       // out == Handle: the pair's write end
    size_t in_pipe_bytes = 0;                 // stdin pipe buffer: Linux F_SETPIPE_SZ (best effort);
                                              // Windows the named pipe's buffer; 0 = system default (64 KB)
    size_t out_pipe_bytes = 0, err_pipe_bytes = 0;   // Windows only; 0 = 64 KB
    std::vector<std::pair<std::string, std::string>> env_set;   // added / replaced; the rest is inherited
    std::vector<std::string> env_unset;
    std::string cwd;                          // "" = inherit
    Priority priority = Priority::Normal;
    bool kill_tree = false;                   // Linux: its own process group, kill()/terminate() signal the
                                              //   group; Windows: they end its whole Job object (every child
                                              //   has one: KILL_ON_JOB_CLOSE) + CREATE_NEW_PROCESS_GROUP
    Console console = Console::Auto;
};

struct ExitStatus {
    enum class Kind : uint8_t { None, Exited, Signaled };
    Kind kind = Kind::None;
    int code = 0;              // Exited: Linux WEXITSTATUS (0..255); Windows the full exit code (e.g. 9009)
    int signal = 0;            // Signaled: Linux WTERMSIG. Windows pseudo-signal: our kill() 9, terminate() 15,
                               //   0xC0000005 / 0xC00000FD -> 11, 0xC0000409 -> 6, 0xC000001D -> 4,
                               //   0xC000008E..0xC0000095 -> 8, 0xC000013A -> 2, other 0xC....... -> 0
    uint32_t raw = 0;          // Linux waitpid status; Windows GetExitCodeProcess value
    bool killed_by_us = false; // kill() / terminate() was called and the code is the one it set

    bool exited() const { return kind == Kind::Exited; }
    bool signaled() const { return kind == Kind::Signaled; }
    bool ok() const { return exited() && code == 0; }
    int shell_code() const { return exited() ? code : 128 + signal; }   // ai_manager's "exit" event
    // Linux strsignal(signal); Windows "access violation, 0xC0000005", "abort / fail-fast, 0xC0000409",
    // "stack overflow, 0xC00000FD", "illegal instruction, 0xC000001D", "a DLL is missing, 0xC0000135",
    // "DLL entry point missing, 0xC0000139", "DLL init failed, 0xC0000142", "killed", "terminated",
    // else "NTSTATUS 0x%08X"
    std::string signal_name() const;
    std::string describe() const;   // "exited with status N" | "crashed (<signal_name>)"
#ifndef _WIN32
    static ExitStatus from_wait_status(int st);
#endif
};

class Process {
public:
    Process();
    ~Process();   // still running: kill() + wait(); then close()
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // false + *err (strerror-style: "No such file or directory", "Exec format error", "Argument list too
    // long", "Permission denied"; Windows adds " (Windows error N)") and *err_code (an errno value)
    bool spawn(const SpawnOptions& o, std::string* err, int* err_code = nullptr);
    bool started() const;
    long pid() const;   // for log lines; -1 before spawn

    // ---- pipes (streams spawned as Stdio::Pipe) ----
    // Non-blocking write: > 0 = bytes accepted (Windows: copied into the in-flight buffer, at most 1 MiB, and
    // handed to an overlapped WriteFile), 0 = would block (pipe full / a write still in flight),
    // -1 = broken pipe / stdin closed
    ptrdiff_t write_some(const void* p, size_t n);
    // Non-blocking read: > 0 bytes, 0 = nothing now, -1 = EOF (everything the child wrote before closing or
    // exiting has been returned; also for a stream that isn't a Pipe), -2 = error
    ptrdiff_t read_some(Stream s, void* p, size_t n);
    // Blocking read: > 0 bytes, 0 = timeout, -1 = EOF, -2 = error; timeout_ms < 0 = forever
    ptrdiff_t read(Stream s, void* p, size_t n, int timeout_ms);
    // Until stdout is readable / at EOF, or (want_write_in) stdin can take more, or ms passed
    // (= poll() on {out POLLIN, in POLLOUT})
    void wait_io(int ms, bool want_read_out, bool want_write_in);
    bool stdin_open() const;
    void close_stdin();   // EOF for the child once it has read everything write_some accepted; never blocks
                          //   (Windows: a write still in flight is finished by a helper thread, which then
                          //   closes the pipe)

    // ---- exit ----
    bool try_wait(ExitStatus* st);            // non-blocking reap
    ExitStatus wait();                        // blocking
    bool wait_for(int ms, ExitStatus* st);    // reaped within ms?
    void kill();        // Linux SIGKILL (killpg with kill_tree); Windows TerminateJobObject / TerminateProcess
                        //   (exit code 137); no-op once reaped
    void terminate();   // Linux SIGTERM (killpg with kill_tree); Windows: a kill_tree child on our console gets
                        //   CTRL_BREAK_EVENT, anything else kill() with exit code 143
    void close();       // cancel pending I/O, close pipes / process / job (Windows: closing its job ends what is
                        //   left of its tree); a child not reaped yet is kill()ed and reaped first (as the
                        //   destructor), so no zombie / orphan is left. spawn() on a used Process close()s it first.

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Runs argv with stdin = Null, stdout = Pipe (first max_bytes kept, the rest drained), stderr = Null; at
// timeout_ms kill() + wait(). true = finished in time and exited 0 (dig::run_and_capture's rule).
// console (Windows): None for a leaf program (a plugin's --info); Hidden for one that starts console
// programs itself (a Python script running the claude CLI) - under a None parent each of those would open a
// visible console window.
bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out, ExitStatus* st = nullptr,
                 size_t max_bytes = 1u << 20, bool search_path = false, Console console = Console::None);

// An anonymous pipe whose ends go to two children (TetraCodec: cdecoder's stdout -> sdecoder's stdin).
// close() after both children are spawned.
class PipePair {
public:
    PipePair();
    ~PipePair();
    bool create(size_t buffer_bytes, std::string* err);
    void close();

private:
    friend class Process;
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace kproc
