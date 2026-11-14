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
  `-fcx-fortran-rules` (filtered out; plugins via `CX_RULES`;
  `mac/fast_complex.h` stands in - see the section below), no
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

## std::complex arithmetic: `mac/fast_complex.h`

Added 2026-10-25 after the user asked whether libc++'s NaN checks were
worth removing. The header is MIT, identical in `heimdall_v2/mac/` and
`kraken_doa_v2/mac/`, and force-included (after `posix_compat.h`) into
every C++ file of heimdall, kraken_doa and the plugins by the three
Makefiles' `MACOS` blocks. Linux and Windows never include it.

### Why it exists

libc++ writes C99's inf / NaN recovery into `<complex>` itself: every
`complex * complex` tests the result for NaN and has a recovery branch,
and `norm` tests both parts for infinity. Apple's libc++ 13 (Apple clang
14, macOS 12) has it, and so does LLVM's libc++ 18. The branches keep
complex loops from vectorizing.

On Linux and Windows, GCC's library hands `std::complex` arithmetic to the
compiler, so `-fcx-fortran-rules` (heimdall, plugins) or `-ffast-math`
(kraken_doa) removes the recovery. On macOS no such flag reaches libc++'s
code. Apple clang 14 doesn't have `-fcx-fortran-rules` at all, and newer
clang's `-fcx-fortran-rules` / `-fcomplex-arithmetic=` only affect the
built-in C `_Complex` type. The one flag that does remove the branches,
`-ffinite-math-only`, also folds `std::isfinite` / `std::isnan` to
constants, which breaks NaN-as-unknown in the plugins (see
`compiler-flags.md`).

### What it does

The header adds plain (non-template) overloads for `complex<float>` and
`complex<double>`: `operator*`, `operator*=` and `norm`. They are declared
in libc++'s own namespace (`_LIBCPP_BEGIN_NAMESPACE_STD`). On an exact
match, overload resolution prefers a non-template to libc++'s templates,
so every such operation in our code takes them. The arithmetic is
libc++'s minus the recovery: four products, then ac - bd and ad + bc. The
only results that differ are those where libc++ would have turned a NaN
back into an infinity.

The overloads are `constexpr`, like libc++ 16+'s versions in C++20.

### Decisions and pitfalls

- **Overloads, not explicit specializations.** The first version
  specialized `operator*<float>`. Apple clang 14 accepted it, but libc++ 16+
  (Xcode 15 and later) rejects it: there `complex<float>::operator*=` is
  `constexpr`, so `<complex>` instantiates `operator*` while being read, and
  a specialization after that is a compile error ("explicit specialization
  after instantiation"). Checked with zig 0.13's libc++ 18 for x86_64 and
  arm64 macOS: overloads compile in C++17 and C++20 and are picked (same
  speed-up, identical sums).
- **Division is left to libc++.** Its cost is the `logb` / `scalbn` range
  scaling, not the checks. Without the checks a divide went from 23 to
  22 ns, and the 37 test signals showed no difference at all (multiply,
  `norm` and division without the checks vs multiply and `norm` only:
  42.98 vs 43.01 s). Keeping division also
  avoids copying libc++'s scaling code (Apache-2.0).
- **Formally not allowed.** C++ doesn't allow adding declarations to
  namespace `std` ([namespace.std]); Apple clang / libc++ accept it. A future
  libc++ that declared its own non-template operators with these
  signatures would fail to compile here, and could never compute anything
  different. If libc++ ever drops the recovery, the header just becomes
  redundant.
- **Bit-identity.** With the plugins' and heimdall's flags, results are
  bit-identical to stock. Under kraken_doa's `-ffast-math` the compiler may
  fuse a multiply-add once the branch is gone, giving last-bit differences
  (GCC on Linux already does the same).
- **Alternatives rejected.** Global `-ffinite-math-only` gains nothing
  more: kraken_doa live (heavy load) was 73.1 % of a core with it against
  73.3 % with only libc++'s complex checks removed. It would also break 434 `std::isfinite` uses, 346 NaN
  sentinels and implicit infinities such as `log10(0)`. Building with
  Homebrew's GCC would give the Linux flags, but on macOS 12 Homebrew
  probably has no ready-built GCC, arm64 Macs need a separately maintained
  port, and it would be a third toolchain to support. A `cmul` helper in hot
  loops would have meant hundreds of edits for part of the gain, which is
  spread over nearly every plugin.
- **Updating the compiler doesn't help.** The MacBook Air 2015 can't get
  past macOS 12 / Command Line Tools 14. Newer libc++ (checked: 18) still has
  the recovery code, and newer clang's complex flags don't reach it.

### Measurements

All on the MacBook Air 2015 (i5-5350U, 2 cores, Apple clang 14), stock vs
the header, alternating runs:

- **Plugins**, all 37 debug-playback test signals decoded offline
  (`decoder --file`, 3 rounds, min CPU): 54.0 -> 43.1 s (-20 %). By
  plugin: lora -36 %, uat / ssr1030 / cells -19 %, radar / flarm / mpt1327
  / tetra -17 %, dme -13 %, rds -12 % (single channel), ism -3.5 %. Decoded
  output was identical in 37 / 37; only the plugins' own CPU-time lines
  differed.
- **kraken_doa live** on heimdall's stream (2 rounds, % of a core): one
  WBFM VFO 37.1 -> 35.9; with 3 more VFOs (2 x 12.5 kHz, 240 kHz) and
  beamforming 89.9 -> 72.5 (-19 %).
- **Bearings**: in debug playback of adsb-six-aircraft,
  dmr-repeater-and-direct, p25-three-units and lora-mesh-five, all 19
  talkers came out on their scene bearings with both builds.
- **heimdall**: converged 24.5 -> 24.0 % of a core (its hot loops already
  avoid complex operators since 84e3e08). CONVERGED in 24-31 s either way,
  calibration check OK (|lag| 0.0, |phase| 0.1 deg), 0 coherence events.
- **Micro-benchmark** (multiply + `*=` + `norm` loop): 2-2.7x faster with
  libc++ 13 and with libc++ 18.
