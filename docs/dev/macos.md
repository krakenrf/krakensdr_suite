# macOS port (developer notes)

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

## macOS

Native build (2026-10-23, the user's request), the same Makefiles: every
macOS part sits under `MACOS := $(if $(filter Darwin,$(shell uname -s)),1,)`
(heimdall, kraken_doa, plugins/Makefile); the Linux build is unchanged.
`./install.sh` works there (Homebrew in place of apt, no sudo: `brew
install cmake pkg-config libusb eigen openssl@3 opus tmux` for what's
missing; the librtlsdr fork built but NOT installed into /usr/local -
Homebrew's prefix, its own librtlsdr would collide; rtl_test in
librtlsdr/build/src; no udev rules / DVB blacklist / limits.conf; RAM from
`sysctl hw.memsize` for JOBS), or `make deps-macos` + make in either app. Tested on a MacBook Air 2015 (Intel,
macOS 12.7, Apple clang 14.0.0, GNU make 3.81): heimdall, kraken_doa and all
26 plugins build; with the KrakenSDR heimdall converged in ~25 s, check OK
(|lag| 0, |phase| 0.1-0.2 deg), kraken_doa streamed from it, the web UI
served. Apple Silicon: `uname -m` says arm64 - the Makefiles map it to
aarch64 (`override ARCH`; NEON for pffft, which otherwise silently builds
its scalar code), and on macOS the aarch64 CPU flag is `MAC_CPU` (empty:
Apple clang's arm64 default, the M1 - older Apple clang rejects `native`
there; `make MAC_CPU=-mcpu=apple-m2`), liquid.mk the same. Checked by
cross-compiling with `-target arm64-apple-macos11` (pffft's NEON path,
the decimator, MUSIC, heimdall's correlation, plugins), not on a real
Apple Silicon Mac.
- `mac/posix_compat.h` (identical in both apps, MIT like `win/`,
  force-included with `-include` into every C++ file of kraken_doa, heimdall
  and the plugins): pipe2, `pthread_setname_np(thread, name)`, fdatasync
  (fsync), posix_fadvise / sync_file_range no-ops, `st_mtim` -> st_mtimespec,
  `std::bit_cast` when libc++ lacks it
- `net_compat.hpp` (`__APPLE__`): SOCK_CLOEXEC / SOCK_NONBLOCK / accept4 /
  MSG_NOSIGNAL emulated as on Windows, SO_NOSIGPIPE on every socket made
  there, an accepted socket's O_NONBLOCK cleared (BSD inherits the
  listener's), TCP_KEEPIDLE = TCP_KEEPALIVE
- `platform.cpp`: exe_dir (_NSGetExecutablePath), cpu_times / mem_info (Mach
  host statistics), os_name "macos" (the UIs show the Linux texts for
  anything but "windows"), keep_system_awake = an IOKit
  PreventUserIdleSystemSleep assertion (`-framework IOKit
  -framework CoreFoundation`): the Mac idle-slept under heimdall and every
  wake cost a recalibration (lag jumps of thousands of samples).
  `fs_mem_available` / recorder memory / system_stats through Mach too;
  disk_util's mount list from getmntinfo (+ `/Volumes/` drives); kraken_doa
  makes its 8080 certificate in-process as on Windows
- Apple clang 14 / its libc++ (macOS 12's Command Line Tools): no
  `-fcx-fortran-rules` (filtered out; plugins via `CX_RULES`), no
  floating-point `std::from_chars` (websocket_server's FFT_VIEW parser falls
  back to strtod under `!__cpp_lib_to_chars`), no structured-binding lambda
  captures (`[&key = k]`), unused-const warnings GCC doesn't give
  (`[[maybe_unused]]`). Homebrew's OpenSSL is keg-only: both Makefiles add
  `brew --prefix openssl@3` (uSockets too, via `USOCKETS_FLAGS`).
  `MACOSX_DEPLOYMENT_TARGET` exported (this macOS's version) so CMake's
  librtlsdr / liquid-dsp and the apps agree. The plugins' licence check
  normalises paths with python3 (no GNU `realpath -m` / `xargs -r`)
- Lag slips of exact multiples of 94 samples (whole 188-byte packets the
  RTL2832 drops on a FIFO overflow, the Windows RAW_IO signature in
  `librtlsdr-fork.md`) -
  FIXED 2026-10-24 (heimdall `set_thread_realtime`, `__APPLE__`): macOS has
  ONE priority scale (SCHED_RR 15..47, a normal thread at 31), so Linux's
  RT_PRIO_USB_READER 20 / RT_PRIO_SAMPLE_DRAIN 15 put the USB readers and
  the drain BELOW every ordinary thread, in the utility / background tiers
  macOS defers when idle. Mapped to 41 / 39 (`32 + (p*9+10)/20`, order
  kept). Before, on the MacBook Air: overnight idle with the display off
  the channels drifted 511-1716 x 94 samples apart (+ one 512-sample USB
  packet), and 3 min with every core busy gave 10 "L1 overflow" coherence
  events; after: the same load 0, and a 7 h soak (6.5 h display off,
  noise source on, every channel cross-correlated against ch0 every 2 s)
  0 slips, 0 coherence events
- run.sh (`brew install tmux`): bash 3.2 and the BSD tools work as they are
  (pgrep -g / -f, ps -o pgid); only headless mode's `tail --pid` is GNU -
  on Darwin the log follower is killed when heimdall ends (disowned: no job
  notice). Tested: tmux split (both dashboards), convergence wait, `stop`
  (session + a stale run in another process group), the supervisor's crash
  restart, headless start / SIGTERM
- Voice to Text: `transcribe/setup.sh` as on Linux (wc -c for the download
  size - BSD stat has no -c; Homebrew hints; `${models[@]+...}`: bash 3.2
  calls an empty array unbound under set -u - `make whisper` failed once the
  models were there). GPU (2026-10-24, the user's request): `WHISPER_GPU=
  auto|on|off` (setup.sh, `make whisper WHISPER_GPU=...`, install.sh) - auto
  = Metal on Apple Silicon only, on = Metal on any Mac (refused off macOS: no
  GPU backend tested there - experts: WHISPER_CMAKE_ARGS with
  -DKRAKEN_WHISPER_GPU=ON), off = CPU; remembered in build/.gpu_mode, the
  CMake args in build/.gpu_args (a change reconfigures, also from --update).
  CMake option `KRAKEN_WHISPER_GPU` -> `cp.use_gpu` (was always false; no GPU
  backend was ever compiled before) and "GPU on | " in front of the READY
  system line. `GGML_METAL_EMBED_LIBRARY` passed explicitly: its default is
  GGML_METAL's at the FIRST configure, so a build/ first made CPU-only and
  switched to Metal looked for kernels/*.metal beside the helper and failed
  to load the model. MacBook Air 2015 (Intel HD 6000), jfk.wav (11 s),
  base.en, 2 threads: CPU 2.6 s, Metal 4.6 s (the GPU shader compiler loaded
  = really on the GPU) - why auto leaves Intel Macs on the CPU. Apple
  Silicon untested here
- AI assistant: Claude Code's sandbox on macOS is the system's (Seatbelt) -
  `sandbox_status()` needs only /usr/bin/sandbox-exec. The bridge adds
  `~/Library/{Keychains,Cookies,Application Support,Safari,Mail,Messages,
  Containers,Group Containers,Accounts}` + zsh history to `denyRead`, and
  the SDK's / Homebrew's include folders to the readable ones (no
  /usr/include). Tested with Claude Code 2.1.309: writes outside its folders,
  network, `~/Library/Keychains` and `security find-generic-password`
  (keychain) all refused in the agent's shell; a compile + run in its folder
  works; a question through kraken_doa used the MCP server. ai_sandbox.cpp:
  `sandbox-exec -p <profile>` in place of bwrap (allow default, deny
  network* / file-write* except the rw folders, /dev/null & co. and a
  private TMPDIR - `/private/tmp/kraken_sandbox.XXXXXX`, one per kraken_doa
  run, as macOS can't give a private /tmp -, signals only to itself / its
  children). Checked with `sandbox_check()` on the live processes: a plugin
  on the list and its debug-playback generator ran sandboxed (and decoded)
- Debug playback: the generators run with `python3 -E` (not -I) as on
  Windows - Apple's python3 has no writable site-packages, `pip install
  --user numpy scipy` puts them where -I hides them (playback.cpp,
  make_testsignal.py, which also force-includes mac/posix_compat.h into the
  C++ generators)
- Not ported / not tested: CMakeLists.txt, the boot service (systemd -
a launchd agent would be the Mac's),
  GPIO / hidraw variants (`--wideband` moRFeus, `--kerberos_sw` - Linux only
  as before); memguard (the agent's Python memory ceiling) reads /proc and
  so does nothing - macOS doesn't enforce RLIMIT_DATA anyway
- Checks: Linux cross-compile with zig (`zig c++ -target x86_64-linux-gnu
  -c`; zig 0.13's `-fsyntax-only` fails with FileNotFound)

