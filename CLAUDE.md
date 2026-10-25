# CLAUDE.md

KrakenSDR suite: a coherent multi-channel RTL-SDR receiver (heimdall) and a
direction-finding / decoding client (kraken_doa) for the 5-channel KrakenSDR,
the 4-channel KerberosSDR and the KrakenSDR Wideband downconverter. The main
target is the Raspberry Pi 4/5 (aarch64, NEON); it also builds for x86_64
Linux, native Windows 10/11 (MSYS2 UCRT64) and macOS.

This file holds what every session needs. Details are in files that are not
loaded up front - read the one for the area you are changing. The docs record
measurements and decisions; when they disagree with the code, the code wins.

| Area | Read |
|---|---|
| heimdall: rules and invariants, then the internals (calibration, coherence recovery, pipeline, operating modes, drag retunes, calibration hold) | `heimdall_v2/CLAUDE.md`, then `heimdall_v2/docs/dev/` |
| kraken_doa: rules, the command path, testing; then one file per feature (index below) | `kraken_doa_v2/CLAUDE.md`, then `kraken_doa_v2/docs/dev/<topic>.md` |
| Writing, testing and licensing a decoder plugin | `kraken_doa_v2/plugins/SDK.md`, `kraken_doa_v2/plugins/<id>/README.md` |
| run.sh / install.sh internals, Docker, logging, network access checks, git secret guards | `docs/dev/run-and-deploy.md` |
| Windows port; the librtlsdr fork (WinUSB fixes, fast retunes, USB transfer counts) | `docs/dev/windows.md`, `docs/dev/librtlsdr-fork.md` |
| macOS port | `docs/dev/macos.md` |
| Compiler flags and the measurements behind them | `docs/dev/compiler-flags.md` |
| Licence of each part, audits, clean-room rewrites | `docs/dev/licensing.md`, `LICENSE` |
| Long-form suite overview (architecture, configuration, every feature, algorithms) | `docs/dev/suite-reference.md` |
| 8091 data / 8092 control wire protocols | `gr-krakensdr_v2/docs/heimdall_v2_data_protocol.md`, `..._control_protocol.md` |
| User docs | `README.md`, `DOCKER.md`, `WINDOWS.md` |

## Layout

```
heimdall_v2/      server (C++20): dongles -> coherent, phase-compensated IQ
  src/{core,sdr,dsp,net,web}/   config.h   index.html (web UI, read at runtime)
kraken_doa_v2/    client: spectrum, VFOs, audio, MUSIC DoA, decoders, map, recorder, AI assistant
  src/  include/config.hpp  kraken_doa.html (web UI, read at runtime)
  plugins/<id>/   decoder plugins, each its own executable
  plugins/lib/    shared DSP / FEC / vocoders (libkrakendig.a)
  plugins/sdk/    plugin API + host (MIT)
  liquid.mk  transcribe/ (whisper.cpp helper)  ai/ (assistant bridge)
gr-krakensdr_v2/  GNU Radio source for heimdall's stream
librtlsdr/        krakenrf/librtlsdr checkout (gitignored; built by make / install.sh)
windows/          MSYS2 setup, build.sh, package.sh, krakensdr.cpp (the launcher)
docker/ scripts/ .githooks/   container entrypoint; git clean filter + licence collection; pre-push hook
install.sh  run.sh  install-pi-service.sh  build_windows.bat  run.bat  Dockerfile  docker-compose.yml
```

heimdall module dependencies stay acyclic: `core` depends on nothing, `sdr`
on core, `dsp` and `net` on core + sdr, `web` on all four.

## Build and run

```bash
./install.sh         # deps (apt / Homebrew), librtlsdr fork, both apps, plugins; WHISPER=1 adds Voice to Text
./run.sh [-w|--wideband] [--kerberos|--kerberos_sw] [--ext_noise]  # tmux split; NO_TMUX=1 = headless, logs/
./run.sh stop

make -C heimdall_v2   [run|clean|rebuild|distclean|status|deps]   # fetches pinned uWebSockets + the fork's static lib
make -C kraken_doa_v2 [run|clean|rebuild|distclean|status|debug|whisper]   # also builds liquid-dsp and every plugin
make -C kraken_doa_v2/plugins PLUGIN=<id>                         # one plugin

build_windows.bat   run.bat   windows/package.sh                  # Windows build / run / portable package
```

- Use at most 3 parallel jobs on a Pi (`-j3`, never `-j4` / `nproc`): more
  can crash it (install.sh picks 1-3 jobs by RAM).
- The web UIs are read at runtime: edit and refresh the browser, no rebuild.
- Builds use `-march=native` / `-mcpu=native`, so build on the machine that
  runs the result (a Pi 5 build fails on a Pi 4 with illegal instruction). A
  flags stamp (`build/.flags`, `obj/.flags`, `plugins/lib/build/.flags`)
  recompiles everything when the flags change.
- heimdall also has a CMakeLists.txt; it needs
  `../librtlsdr/build/src/librtlsdr.a` built first.
- Bring-up check: heimdall reaches CONVERGED ~25-30 s after start (web UI
  http://localhost:8070), then kraken_doa serves https://localhost:8080.
  `echo '{"command":"get_status"}' | nc localhost 8092` prints heimdall's status.

## Ports

| Port | App | Use |
|---|---|---|
| 8070 | heimdall | web UI (HTTP + WebSocket) |
| 8091 | heimdall | multi-channel IQ stream; the header carries channel count, frequency, phase state |
| 8092 | heimdall | JSON control; no auth, but hangs up on any HTTP request |
| 1234 | heimdall | RTL-TCP read-only tap (tuning commands ignored; a busy port is not fatal) |
| 8080 | kraken_doa | web UI (HTTPS + WSS) |
| 8081 | kraken_doa | plain-HTTP DoA value page for the Android app |
| 8079 | kraken_doa | AI assistant's MCP server, 127.0.0.1 only, bearer token per run |
| 8021 | kraken_doa | optional local web-mapper broadcast |

## How it works

**heimdall**: per-dongle RTL-SDR callbacks (realtime threads) -> L1 buffers
(allocation-free; an overflow is reported as a coherence loss, never dropped
silently) -> sample drain (realtime) -> L2-raw -> conversion worker (uint8 ->
phase-compensated samples, broadcast on 8091) -> correlation processor (pffft
cross-correlation, lag servo, phase calibration). A coherence watchdog flushes
and recalibrates after a desync. Calibration uses the KrakenSDR's internal
noise source, switched only through `set_bias_tee_all_devices()`
(`heimdall_v2/src/sdr/sdr_init.cpp`); every hardware variant hooks into that
one function.

Calibration decisions, settled by measurement (don't reopen them without new
measurements; reasons in `heimdall_v2/docs/dev/calibration.md`):
- Lag: a proportional servo on the sample-clock correction register only
  (`rtlsdr_set_sample_freq_correction_f`; the tuner LO is untouched),
  MEASURING -> SERVOING -> CONVERGED (within +-0.02 samples).
- Phase: phase-only, from the dominant eigenvector of the noise-source
  covariance; one unit phasor per channel, applied in the conversion loop.
- Gain is measured but not corrected: the noise source compresses the front
  end, so its gain estimates are off by up to ~2.4 dB.
- The R820T IF VGA is fixed (`rtlsdr_set_tuner_if_mode(10000 + step)`,
  `R820T_IF_VGA_*`: step 8 with manual gain, 11 with tuner AGC). The step
  shifts each dongle's phase differently, so calibration and operation must
  use the same step; the noise source's clipping at step 8 is accepted.

**Element count**: `NUM_DEVICES` (8) is only a compile-time ceiling. The
live count N comes from `-n`, else the saved `num_elements`, else the number
of expected serials ("1000".."1007") attached; `--kerberos` defaults to 4.
Changing N at runtime closes every handle, reopens the first N and
recalibrates (rolling back on failure); dongles beyond N stay free for other
programs. Clients follow the channel count in the 8091 header
(`MAX_CHANNELS` / `DOA_NUM_ELEMENTS` = 8 are ceilings).

**kraken_doa**: TCP client (8091) -> uint8 to complex (a plain loop the
compiler vectorizes; it beats hand-written NEON - keep it) -> one
`SharedDecimator` per VFO (integer decimation, block-FIR cascade chosen by
`planStages`; all VFOs in one pass per antenna) -> spectrum (pffft), audio
mixer (Opus to the page), MUSIC DoA / beamformer, decoder plugins (child
processes over pipes), per-talker DoA, map, recorder -> uWebSockets on 8080.
Every control command (browser messages, the settings replay, the web
mapper, the AI assistant's MCP tools, the scanners) goes through
`ControlHandler::handle_websocket_message` / `apply_command` and runs only on
the uWS loop thread; from any other thread, post it with `loop->defer`.

**8091 phase-state field**: the low byte is the calibration state; higher
bits are flags (0x100 KerberosSDR manual mode, 0x200 calibration stale,
0x400 wideband scan, 0x800 independent). Every consumer masks the low byte.

## Modes and hardware variants

- **Operating modes** (top bar): coherent (the array: calibration, DoA,
  beamforming), wideband scan (tuners side by side in one stitched spectrum;
  discrete scanner), independent (each tuner its own frequency, gain and
  pane; no calibration). The client follows heimdall's mode from the header
  flags and parks DoA / beamforming outside coherent mode.
- **`--wideband` / `-w`** (KrakenSDR Wideband downconverter, both apps):
  tuners stay at the 1268 MHz IF and a retune reprograms one shared moRFeus
  LO. The user-facing frequency is always the true RF. Mixer side and antenna
  ring follow the frequency (`wideband_retune_rf()`). The `WB_*` constants in
  `heimdall_v2/config.h` and `kraken_doa_v2/include/config.hpp` must match;
  frequencies are `uint64_t` end to end. Not the same thing as wideband scan
  mode, which is refused with this variant.
- **`--kerberos`** (KerberosSDR, no noise-source switch): it never
  calibrates on its own, because the antennas must be disconnected by hand;
  only the web UI's Full Recalibration runs one, and a retune marks the
  calibration STALE. **`--kerberos_sw`**: third-party antenna switches on Pi
  GPIO 23/24 (gpio v2 character device) restore automatic calibration; it
  falls back to manual mode if the GPIOs are unavailable.
- **`--ext_noise`**: an add-on array's noise source, powered from the CH0
  bias tee (GPIO1), replaces the internal one (GPIO0); CH0's antenna bias tee
  is reserved. Refused together with `--wideband` or `--kerberos*`.
- UCA arrays are wired clockwise; `uca_angle_sign()`
  (`kraken_doa_v2/include/globals.hpp`) is the single chirality choke point.

## Client feature index

One file per feature in `kraken_doa_v2/docs/dev/` (the full index is at
the end of `kraken_doa_v2/CLAUDE.md`). Decoders also have
`kraken_doa_v2/plugins/<id>/README.md`.

- Spectrum: `spectrum-views.md` (`FFT_VIEW:`, message type 7),
  `noise-floor.md`
- VFOs: `vfo-bandwidth.md` (any width 1 kHz - 2.4 MHz, the decimator),
  `vfo-audio.md`, `operating-modes.md` (also VFO cards, the tuning step,
  zoom)
- Decoders (every decoder is a plugin): `decoder-plugins.md`,
  `digital-decoders.md`, `digital-voice.md`, `digital-squelch.md`,
  `doa-per-talker.md`, `fixed-frequency-decoders.md`, and
  `plugin-<id>.md` per plugin
- Scanning: `saved-frequency-scan.md` (the discrete, continuous and
  saved-frequency scanners are mutually exclusive), `saved-frequencies.md`
- Map: `map.md`, `map-search.md`, `station-on-map.md`, `osm-masts.md`,
  `viewshed.md`, `path-loss.md`, `incident-map.md`, `mobile-df.md`,
  `log-replay.md`
- Recording and playback: `recorder.md` (`SIGMF.md`), `debug-playback.md`,
  `decoder-log.md`, `voice-to-text.md`
- Other: `ai-assistant.md`, `web-mapper.md`, `array-calibration.md`
  (experimental), `array-calculator.md`, `wideband-variant.md`,
  `fft-pffft.md`, `liquid-dsp.md`

## Rules

### Portability
- A Windows or macOS change must not alter the Linux build: platform code
  goes under the Makefiles' `WINDOWS` / `MACOS` conditionals and `_WIN32` /
  `__APPLE__`. Check each changed file with MinGW (`g++ -fsyntax-only` with
  the Windows flags) and for Linux glibc (`zig c++ -target
  x86_64-linux-gnu -c`; zig 0.13's `-fsyntax-only` is broken). A Linux
  regression is the main risk.
- Windows is LLP64 (`long` is 32-bit): Hz, epoch ms, byte counts and sample
  counters are `int64_t` / `long long`; use `llround`, `stoll`, `%lld` /
  `PRId64`.
- Use the compat layer, not raw POSIX: `net_compat.hpp` `sock_*` (on
  Windows `close()` on a socket does nothing, and a busy port must fail to
  bind), `fs_replace` (renaming over an existing file fails on Windows),
  `platform::` stop handling (Windows has no SIGTERM: a named stop event),
  `kproc` for child processes. `win/posix_compat.h` and
  `mac/posix_compat.h` are force-included and define POSIX names only with
  their Linux meaning.
- Keep shared copies identical in both apps: `host_check.hpp`,
  `log_sanitize.hpp`, `net_compat.hpp`, `platform.*` and `fs_compat.*` (only
  the include path differs), `win/`, `mac/` (`posix_compat.h`,
  `fast_complex.h`),
  `external/pffft/`. Also keep equal: `UWS_COMMIT` in both Makefiles and the
  Dockerfile; the `WB_*` constants in both config files;
  `src/rdf_engine.cpp` and its JavaScript port in kraken_doa.html (check with
  `kraken_doa_v2/tools/replay_validate.mjs --cpp`).

### Numerics
- Never build with `-Ofast` or `-ffinite-math-only`: they turn
  `std::isfinite` / `std::isnan` into constants, and NaN is how plugin map
  points say "unknown" and how bad JSON input is rejected. kraken_doa uses
  `-O3 -ffast-math -fno-finite-math-only`; heimdall and the plugins use
  `-O3 -fno-math-errno -fcx-fortran-rules` without fast-math. Use
  `is_finite_value()` for finiteness checks.
- Apple clang has no `-fcx-fortran-rules`, and libc++ puts its inf / NaN
  recovery in the `<complex>` code itself, so on macOS the force-included
  `mac/fast_complex.h` supplies `std::complex` `*`, `*=` and `norm` without
  it (~20 % of the plugins' and kraken_doa's CPU). It uses overloads, not
  specializations, because libc++ 16+ can't take a specialization;
  `docs/dev/macos.md` has the details.

### Runtime files, logging, web security
- Every runtime file path is cwd-relative (Docker runs each app in
  `/data/<app>`). A new static file an app serves must also go into the
  Dockerfile `COPY`, `docker/entrypoint.sh`'s `link_files` and the
  `.dockerignore` allow-list.
- Information goes to stdout, faults to stderr. When stdout is not a
  terminal it is discarded at startup, so deployed logs hold only errors
  (`HEIMDALL_VERBOSE_LOG=1` / `KRAKEN_DOA_VERBOSE_LOG=1` keep it). Throttle
  warnings that can repeat. All output passes a sanitizer that replaces
  control characters, so log lines must not contain escape codes (the
  dashboards write theirs straight to the terminal).
- Both apps check a Host allow-list (DNS rebinding; `KRAKEN_ALLOWED_HOSTS`
  adds names) and Origin = Host on browser requests; keep new routes behind
  those checks. kraken_doa has an optional API token.

### Decoders
- Every decoder is an out-of-process plugin; kraken_doa itself contains no
  protocol code. Plugins move between receivers as source folders only -
  there is deliberately no web upload or rebuild.
- ID separation is part of every decoder: if the signal carries a
  per-transmitter ID, report it as a talker (`kp::Host::talker`) and test
  with several transmitters at known bearings (`plugins/SDK.md`
  *ID separation*, `plugins/sdk/tools/uca_scene.py`,
  `decoder --file x.cf32 --channels 5 --uca 0.12`).
- Each plugin has a debug-playback catalogue
  (`plugins/<id>/tools/testsignals.json`) for end-to-end checks through the
  real pipeline.

### Licensing
- `kraken_doa_v2/` (including `plugins/` and `plugins/lib/`) is under the
  KrakenSDR Source Available Licence. No GPL / AGPL / LGPL code in it or
  linked into it (LGPL only as a dynamic system library), and nothing
  translated from GPL projects (rtl_433, dump1090 forks, readsb, rs1729/RS,
  OP25, osmo-tetra, gr-lora*, srsRAN, ...). Use those only as documentation
  of facts and as test references; write from the standards and
  measurements.
- Exception: a plugin folder with its own `LICENSE` is a separate program
  under that licence (`ism` GPL-2.0-or-later, `flarm` GPL-3.0).
  `plugins/Makefile` builds it without `plugins/lib` or kraken_doa headers
  and fails on a stray include; never copy `plugins/lib` or kraken_doa code
  into such a plugin.
- Code ported from permissive projects keeps its full notice and gets a row
  in that app's `THIRD_PARTY_NOTICES.md`; a dependency compiled into a
  binary also goes into `scripts/collect-licenses.sh`.
- `heimdall_v2/`, `gr-krakensdr_v2/`: GPL-3.0-or-later. `plugins/sdk/` and
  `win/`: MIT. Root build / run / packaging scripts, `docker/`, `windows/`,
  `scripts/`, `.githooks/`: MIT.

### Secrets and git
- `doa_settings.json`, `api_token` and `server.key` stay untracked. A clean
  filter (`scripts/git-secrets-clean`) blanks API keys and station
  coordinates in committed settings JSON, and `.githooks/pre-push` blocks
  secrets, voice codecs and any `*.exe` / `*.dll`. install.sh registers both
  per clone.
- Voice codecs (ETSI TETRA, AMBE / mbelib) are never committed or packaged
  (licensing); users install them themselves.

### Testing run.sh
- Running a copy of run.sh still stops the live stack: it replaces the tmux
  session `krakensdr` and sweeps stale heimdall / kraken_doa processes. Give
  the copy its own `TMUX_SESSION` and stub out `stop_stale`.

## Dependencies

- **librtlsdr**: the krakenrf fork, linked statically from
  `librtlsdr/build/src/librtlsdr.a` (a distro `librtlsdr0` would shadow a
  shared build and lose the sample-clock servo and GPIO control). heimdall
  requires 578b43d or later on every platform (`RTLSDR_FIX_MARK` =
  `rtlsdr_finish_tune`); its Makefile fast-forwards an older clone or stops
  with instructions, and the Dockerfile pins `LIBRTLSDR_COMMIT`. Don't
  install `librtlsdr-dev`; distro rtl-sdr packages may stay installed.
- **uWebSockets** is pinned (`UWS_COMMIT` fe7da4cb): newer master defines
  `uWS::time_ms()` without `inline` in `HttpCache.h`, and the link fails. The
  Makefiles move a checkout on another commit back to the pin.
- **FFT**: pffft, vendored in both apps; the plugins use it through
  `plugins/sdk/kraken_fft.hpp`. No program of the suite links FFTW (only
  gr-krakensdr_v2 does).
- **liquid-dsp**: built from source by `kraken_doa_v2/liquid.mk` (pinned
  v1.7.0, static, without FFTW / libfec - Debian's build links both).
- **System**: libusb-1.0, cmake, Eigen3, OpenSSL, Opus, zlib, SQLite,
  pkg-config, a C++20 compiler (`make deps` / install.sh install them).

## Platform notes

**Windows** (`docs/dev/windows.md`, `docs/dev/librtlsdr-fork.md`):
- Static exes from the same Makefiles under MSYS2 UCRT64, with a portable
  `WIN_MARCH ?= -march=x86-64-v3`. Exes link to `.tmp` and are moved into
  place (a running exe can't be overwritten). `krakensdr.exe` (from
  `windows/krakensdr.cpp`) does run.sh's job. Dongles need the WinUSB
  driver (Zadig).
- USB control transfers take 3-4 ms each and run one at a time, so heimdall
  opens and tunes dongles one by one on Windows (`for_each_tuner`; parallel
  tuning times out with -7) and coalesces queued 8092 retunes.
- Claude Code has no sandbox on Windows: the AI assistant gets no shell
  there, and a plugin it changed doesn't run until trusted.

**macOS** (`docs/dev/macos.md`): the same Makefiles (`MACOS`), Homebrew in
place of apt, compatible with Apple clang 14 (no `-fcx-fortran-rules` -
`mac/fast_complex.h` stands in -, no floating-point `std::from_chars`). Docker can't pass USB through on macOS -
use the native install. CMakeLists.txt and the boot service aren't ported.

## Maintaining this file

Keep it under ~25k characters (Claude Code warns at 40k). Add a rule or fact
here only if most sessions need it; put details, measurements and history in
the component `CLAUDE.md` files or `docs/dev/`, and link them from here. The
same holds for `heimdall_v2/CLAUDE.md` and `kraken_doa_v2/CLAUDE.md` (loaded
whenever work touches those folders): rules there, a feature's design,
measurements and test history in that folder's `docs/dev/`.
