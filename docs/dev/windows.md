# Windows port (developer notes)

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

User guide: `WINDOWS.md`. The librtlsdr fork's WinUSB fixes and the retune
speed work are in `librtlsdr-fork.md`.

## Windows (WINDOWS.md)

Native Windows 10 / 11 x64 build (not WSL / Cygwin): MSYS2's **UCRT64**
toolchain (MinGW-w64 GCC), the same Makefiles - every Windows part sits under
`WINDOWS := $(if $(findstring mingw,$(shell $(CXX) -dumpmachine)),1,)`, the
Linux build is unchanged. Results are static exes (no MSYS2 DLLs at run time):
`heimdall_v2/heimdall.exe`, `kraken_doa_v2/kraken_doa.exe`,
`plugins/<id>/build/decoder.exe`, `plugins/lib/build/codecs.exe`, and the
launcher `krakensdr.exe` (repo root, from `windows/krakensdr.cpp`).
- Entry points: `build_windows.bat` (installs MSYS2 into `C:\msys64` if
  missing - `windows/setup_msys2.ps1`, no admin - then runs `windows/build.sh`
  = install.sh's job: pacman packages, both apps + plugins, rtl_test, the
  launcher, a TLS cert, the git guards); `run.bat` = `krakensdr.exe` (run.sh's
  job); `windows/package.sh` -> `dist/KrakenSDR-windows/` + .zip (portable,
  runs without MSYS2). Dongles need the WinUSB driver (Zadig, interface 0 of
  each RTL2832U `0BDA:2838`)
- `krakensdr.exe`: Windows Terminal split (`wt -w krakensdr new-tab ... ;
  split-pane -H ...`, the tmux layout) or two console windows; supervisors
  (`__supervise`, crash -> restart after RESTART_DELAY; no restart loop on
  0xC000001D / missing-DLL exits; the app in a kill-on-close job), client pane
  (`__client_pane`: the 8091 convergence probe in C++, same exit-code rules as
  run.sh's python), `stop` / the start-up stale sweep (every heimdall /
  kraken_doa / supervisor of this user via its stop event, TerminateProcess
  after 10 s), `--headless` (logs\*.log, as NO_TMUX=1). Stop requests use the
  named event `Local\KrakenSDR_Stop_<pid>` that both apps and the launcher
  create (`platform::install_stop_handler`) - Windows has no SIGTERM
- Stops the OS ends a few seconds later are URGENT (`platform::stop_is_urgent`,
  always false on Linux): a closed console window, logoff / shutdown / an
  update restart (WM_ENDSESSION at a hidden top-level window in every exe -
  with user32 loaded CTRL_LOGOFF / SHUTDOWN_EVENT never come), and the
  launcher's `Local\KrakenSDR_StopUrgent_<pid>` (it sets that when it is
  ending itself; shutdown level 0x281, so supervisors hear before their
  apps and don't restart them). Urgent: heimdall waits <= 1.5 s for the
  settings lock, kraken_doa saves settings / incidents / noise floors /
  the decoder log first and cuts the recorder's waits. `taskkill` without
  /F (WM_CLOSE) is a normal stop
- Network folders: a recorder / Decoder Logging folder on a share
  (`\\host\share`, `\\?\UNC`, `\??\` ...; `fs_is_network_path`) is refused
  unless `KRAKEN_ALLOW_NETWORK_DIRS=1` - a web page could otherwise make
  Windows sign in to a host it names as the user. Mapped drive letters work
- TLS (`tls_compat`): the Windows root store goes into OpenSSL (self-signed
  non-root certificates of the CA store left out); a chain that misses its
  issuer goes to `CertGetCertificateChain` (Windows downloads roots it hasn't
  got yet on demand), then OpenSSL verifies it again with Windows' root as the
  only anchor (host name, dates, purpose stay OpenSSL's checks). The 8080
  `server.key` is written owner-only (an explicit DACL)
- Shared compat layer (identical copies in both apps, like host_check.hpp):
  - `win/posix_compat.h` (force-included into every Windows C++ TU,
    `-include`): POSIX names MinGW lacks WITH their Linux meaning only
    (localtime_r, timegm, setenv, strptime, fsync, mkdir(p, mode), pread,
    O_CLOEXEC, SIG* placeholders ...) + `nanosleep` -> `kraken_nanosleep`
    (winpthreads rounds sleep_for up to the 15.6 ms tick even after
    timeBeginPeriod - the drain's 100 us poll became a busy spin)
  - `win/win_init.cpp` (linked into every exe, init_priority 101): BINARY
    file mode for open/fopen/iostreams and fds 0-2 (UCRT64's binmode.o is
    empty - a plugin stopped at the first 0x1A in its sample stream), 1 ms
    timers, no power throttling, WSAStartup, SEM_FAILCRITICALERRORS, a POSIX
    TZ dropped; `win/app.rc` + `app.manifest`: UTF-8 active code page (char*
    APIs, argv, getenv are UTF-8) + longPathAware
  - `net_compat.hpp` (heimdall `src/core/`, kraken_doa `include/utils/`):
    `sock_*` - int sockets, errno set from WSAGetLastError (EWOULDBLOCK ==
    EAGAIN), closesocket (close() on a socket does NOTHING on Windows),
    SO_REUSEADDR -> SO_EXCLUSIVEADDRUSE (a busy port must fail to bind),
    MSG_DONTWAIT emulated, `sock_fits_fd_set` (Winsock fd_set is a list:
    socket values >= FD_SETSIZE are normal), timeouts as EAGAIN
  - `platform.hpp/.cpp` (namespace platform): stop handler (persistent
    console ctrl handler - the UCRT resets SIGINT after one Ctrl+C - + the
    stop event), `fast_exit` (TerminateProcess: no DLL detach), `stdout_kind`
    (Console / mintty Pty / Redirected; `_isatty` is true for NUL),
    `console_init` (UTF-8 output, QuickEdit off - a click froze every logging
    thread), console size / VT, cpu / mem stats, keep-awake, priority
  - `fs_compat.hpp/.cpp`: `fs_replace` (rename over an EXISTING file fails
    on Windows - every settings save after the first was lost), fsync of the
    dir, mtime, `fs_portable_name` (ADS ':', CON/NUL..., trailing dots),
    `FsMappedFile`
  - kraken_doa only: `include/utils/process.hpp` + `src/utils/process_{posix,win}.cpp`
    (`kproc`: posix_spawn / CreateProcessW with an explicit handle list =
    closefrom(3), OVERLAPPED named pipes for the non-blocking plugin I/O -
    anonymous pipes have no poll / O_NONBLOCK -, one kill-on-close job per
    child), `utils/tls_compat.*` (Windows root store into OpenSSL; the 8080
    cert is generated in-process when missing - no openssl CLI)
- Windows flags: `-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
  -DNOGDI` (wingdi's `ERROR` macro), `-D_USE_MATH_DEFINES` (M_PI under
  -std=c++20), `-D_FILE_OFFSET_BITS=64` (off_t / st_size are 32-bit
  otherwise: a 5 GB lseek landed at 705 MB), `-DPFFFT_STATIC_DEFINE` (else
  pffft is dllimport), `-DLIBUS_USE_LIBUV` (uSockets on libuv - no epoll),
  `WIN_MARCH ?= -march=x86-64-v3` (portable exes; liquid-dsp's CMake RUNS
  SIMD tests on the build PC, so its AVX-512 is switched off explicitly),
  `-Wa,-muse-unaligned-vector-move` (GCC can't 32-byte-align the Win64
  stack), `-static`; exes link to `.tmp` and are `mv`ed (a running exe can't
  be overwritten - MSYS2's mv moves it aside)
- LLP64: `long` is 32-bit on Windows - Hz, epoch ms, byte counts and sample
  counters must be `int64_t` / `long long`, `llround` not `lround`, `stoll`
  not `stol`, `%lld` / PRId64 not `%ld` (fixed suite-wide; keep it that way)
- Checks for Windows-port changes: every changed file compiles with the
  Windows flags (MinGW g++ -fsyntax-only) AND for Linux (glibc; e.g. zig c++
  -target x86_64-linux-gnu) - a Linux regression is the main risk
- AI assistant (`ai_manager.cpp`, `ai_mcp.cpp`, `ai_capture.cpp`,
  `ai_map.cpp`, `ai_sandbox.cpp`, `ai/kraken_ai.py`): runs on Windows except
  for its sandboxes. Enabled with `py -3 ai/kraken_ai.py setup` on the PC
  (Claude Code's native installer + Git for Windows; only a `claude.exe` is
  used - an npm `claude.cmd` runs through cmd.exe, whose 8191-character
  command line can't hold the ~10 K system prompt); the bridge through
  kproc (`python_cmd`, UTF-8, `KRAKEN_MCP_*` passed in `env_set` /
  `env_unset`), Ctrl+Break = Stop (`SIGBREAK`). Claude Code has NO sandbox on
  Windows, so the agent never gets Bash there (`sandbox_status()`: "Claude
  Code has no sandbox on Windows" - upstream's rule: no sandbox = no shell):
  it answers, reads / edits within the path rules, uses the web and every
  MCP tool, but builds and runs nothing. No bwrap either: `aisandbox::
  available()` is false, so a plugin the assistant changed doesn't run at all
  (`--info`, decoders, debug-playback generators refused with
  `NO_SANDBOX_FIX`) until `py -3 ai/kraken_ai.py trust <id>`; changes are
  detected by mtime / creation time (Windows has no ctime; the agent has no
  shell to set one back); a turn that changed `plugins/sdk`, `plugins/lib`
  or a file directly in `plugins/` puts EVERY plugin on the list (a backstop:
  the rules refuse those writes). Permission rules in Claude Code's POSIX form
  (`//c/...`, `rule_path`; Claude Code 2.1.304 matched them through case,
  8.3 short name, trailing dot / space, `..` and `::$DATA` variants - tested
  2026-10-20); no `/usr/include`; a screen context over the
  32 K command line goes in `ai/sessions/<id>/context.json`
  (`--context-file`, Windows only, read + deleted by the bridge); `ai/run`
  (the MCP token while a question runs) gets an owner-only ACL (`icacls`,
  `private_dir`), and kraken_doa deletes the bridge's `mcp-*.json` /
  `settings-*.json` there when a job ends and at startup (a Stop without a
  console to share kills the job at once - the bridge's own cleanup never
  runs); the MCP server binds with `SO_EXCLUSIVEADDRUSE`
  (`LIBUS_LISTEN_EXCLUSIVE_PORT` - uSockets' Windows default SO_REUSEADDR let
  any program share the port); tokens / IDs from OpenSSL's `RAND_bytes` (no
  /dev/urandom); `map_plot` files: drive paths, '\', case-insensitive
  containment, shares refused before any access
- Python children (Debug Playback generators, the AI assistant's bridge and
  sigtool): the interpreter
  `kproc::python_argv` finds (`KRAKEN_PYTHON`, `py -3`, python3 / python -
  the Store stub fails the probe), in UTF-8 mode; where Linux passes `-I`,
  Windows passes `-E`: `-I` also hides the user site-packages, where the
  Microsoft Store Python's pip puts numpy / scipy
- Measured on the KrakenSDR (Ryzen 9 9950X3D, Windows 11): heimdall converges
  in ~30 s from start (as on the Pi), live retunes / gain / 5->4->5 elements
  with 0 coherence events, `check_calibration` OK (|lag| 0) after every
  retune and gain change; the launcher brings the stack up in ~25 s; debug
  playback adsb-six-aircraft: all six aircraft at their scene bearings

