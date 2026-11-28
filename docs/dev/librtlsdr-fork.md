# librtlsdr fork: WinUSB fixes, fast retunes, measurements

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

heimdall links krakenrf/librtlsdr statically and requires 578b43d or later
on every platform.

## Keeping the clone current (2026-11-25)

The fork's `master` - krakenrf/librtlsdr **9644d07** since 2026-11-25 - plus
**ff2bb7b** (2026-11-28: the R820T's IF centre 1.815 MHz at 2.4 MS/s,
`RTLSDR_R82XX_IF_1815`; a local commit on the fork clone's branch
`if-center-1815` and a patch in `docs/dev/librtlsdr-patches/` until it is
pushed - *The R820T IF filter*, *Adopted*) has everything heimdall uses: 578b43d (the fast retunes, the stall retry), 701a832
+ 3ad1746 (zero-copy buffers, the FIFO hold), 6da393d + 88993d3 (no retune
drops a sample - `RTLSDR_NO_STREAM_SLIP` -, `RTLSDR_TUNE_NO_DUMMY_READ`, the
register repair after an early lock read) and 9644d07 (the fast tuner gain
write, `RTLSDR_HAS_GAIN_FAST`). The newer five were local commits (the last
three on a branch `lock-read-no-slip`, as patches in `docs/dev/librtlsdr-patches/`
and `heimdall_v2/tools/`) until they were pushed on 2026-11-25; the sections
below keep the names they had while they were written.

heimdall builds and runs with any fork from 578b43d on; without the newer
commits it falls back (`#ifdef`): sweep / independent mode back to coherent
in 25-30 s instead of ~2 s (every lag re-acquired), the slower tune call per
sweep hop, ~9 ms per region gain write, no zero-copy / held stream start.

What brings the `librtlsdr/` clone up to the fork's latest:
- `./install.sh`: `git pull --ff-only` on an existing clone (it warns and
  builds the checkout as it is when that fails), a fresh clone of `master`
  otherwise.
- `make -C heimdall_v2` - and so `./run.sh` (`scripts/autobuild.sh`) and
  `build_windows.bat` (`windows/build.sh`; `krakensdr.exe` / `run.bat` never
  build) - by two marks in `heimdall_v2/Makefile`:
  - `RTLSDR_FIX_MARK` (`rtlsdr_finish_tune` in `src/librtlsdr.c`, 578b43d):
    the oldest fork heimdall builds with. A clone without it is fetched and
    fast-forwarded to its upstream, else the build stops with instructions
    (the hard path: *WinUSB fixes and fast retunes*, below).
  - `RTLSDR_FEATURE_MARK` (`RTLSDR_R82XX_IF_1815` in `include/rtl-sdr.h`,
    ff2bb7b; before it `rtlsdr_set_center_freq_gain_fast`, 9644d07): the
    newest fork feature heimdall uses -
    move it when heimdall starts using a newer one (and the Dockerfile's
    pin with it). A clone that has the first mark but not this one is
    updated SOFTLY by `scripts/librtlsdr-update.sh`, which make runs while
    it reads its rules (`$(shell)`, only for the goals `all` / `run` /
    `rebuild` / `heimdall`), so the same make sees the new sources: the
    library is rebuilt (`RTLSDR_SRCS`) and the objects that include
    `rtl-sdr.h` recompiled (their `.d` files), then heimdall relinked. The
    update is `git fetch` + `git merge --ff-only @{u}` and nothing else:
    nothing local is discarded, stashed or reset - git refuses a merge
    that would overwrite a local change. When it can't be done (git
    missing, not a git clone, a detached HEAD, no upstream, the fetch
    failed or took too long, local commits, local changes in the way, the
    upstream lacks the mark too) ONE line says why, what heimdall goes
    without and how to get it (`git -C librtlsdr pull --ff-only`, or
    `./install.sh`), and the build goes on with the clone as it is.
  - The fetch is bounded: no credential or host-key prompt
    (`GIT_TERMINAL_PROMPT=0`, stdin `/dev/null`, ssh `BatchMode` +
    `ConnectTimeout=10` unless `GIT_SSH_COMMAND` is set), an HTTP transfer
    under 1 KB/s for 10 s is dropped (`http.lowSpeedLimit` / `lowSpeedTime`),
    and a watchdog kills git and its remote helper after 30 s
    (`RTLSDR_FETCH_SECS`) - git has no connect timeout, and a black-holed
    address otherwise waits out the kernel's SYN retries (~2 min on Linux).
    Plain sh with `sleep` / `kill` (and `pgrep` when there is one): no
    `timeout` binary, which macOS lacks. The hard path's fetch goes through
    the same bound (`RTLSDR_FETCH`); its git output lands in
    `librtlsdr/build/.fork_fetch.log`.
  - When: a clone with the mark is never contacted. A failed attempt writes
    `librtlsdr/build/.fork_feature_tried` (HEAD commit + branch) and is not
    repeated before the HEAD or branch changes or the stamp is 7 days old -
    a Pi that first starts offline gets the update a week later at the
    latest (or at once with `git pull` / `./install.sh`); a successful one
    removes the stamp. `scripts/autobuild.sh check` asks the same script
    (`due`: greps, `git rev-parse`, the stamp - read-only, no network), so
    run.sh builds exactly when make would try, and after one attempt, done
    or failed, both agree that nothing is to do: no rebuild loop. The
    stamp lives in `librtlsdr/build/`, which install.sh deletes - its own
    pull comes first.
- Docker: the Dockerfile pins `LIBRTLSDR_COMMIT` = 9644d07 (a checkout of a
  commit; the image build copies `heimdall_v2/` but not `scripts/`, so it
  never fetches).

Tested 2026-11-25 on x86_64 Ubuntu with scratch upstreams made from the
fork's objects (no network): clones at 3ad1746 and 578b43d went to 9644d07
in one make (fetch + merge, cmake once, 12 heimdall objects, one link; the
binary then has `rtlsdr_set_center_freq_gain_fast` / `_tuner_gain_fast` and
`"gain_fast":true`, the old one `false`), the next check and make did
nothing and made no git network call; a clone at 00606c0 by the hard path
to the tip; one at 9644d07 with an upstream that would fail - no fetch. An
upstream that is gone or refuses: the line after 0.1 s; one that accepts
and never answers: 10.3 s (low-speed limit); one whose connect hangs (a
full accept queue - SYNs dropped): 30.0 s (watchdog), no git process left
behind. A local change, a local commit, a detached HEAD, no upstream, a
tree without `.git`: nothing in the clone changed, the line, the build went
on (the local change rebuilt the library, being a newer source). Retry: a
stamp 8 days old - one more attempt, 5 days old - none. The script under
dash and `bash --posix`; not run on macOS or MSYS2 (no Mac / MinGW here).

## WinUSB fixes and fast retunes

The two notes below were bullets of the Windows section.

- librtlsdr fork on WinUSB: fixes in the fork itself since
  krakenrf/librtlsdr 00606c0 (bd357dd + its follow-up, all under `_WIN32`;
  first a patch the Makefile applied, `heimdall_v2/win/librtlsdr-windows.patch`,
  now gone): an RTL2832U stalls every control OUT after a quiet spell while
  streaming until a control IN is made (every lag-servo write failed: -9) -
  retried after a dummy read; the async cancel path freed transfers whose
  cancellation completes later on Windows (rtlsdr_close crashed) - it waits
  on an Interlocked in-flight count each callback drops as its very last
  statement (bd357dd dropped it earlier: a callback still running on the
  tuning thread could touch a freed transfer); and the bulk endpoint streams
  with WinUSB's RAW_IO pipe policy (libusb >= 1.0.30): WinUSB's default
  passes the queued reads to the controller one at a time, the RTL2832U's
  FIFO overflowed in the gaps and dropped whole 188-byte TS-sized packets -
  lag jumps of exact multiples of 94 samples, 3-4 a minute even with no
  tuner operations, so a phase-only recal after a retune "converged" on a
  misaligned array (check_calibration: |lag| 470 samples). RAW_IO: none in
  the same tests. RAW_IO is switched off when the stream ends (it takes only
  whole packets: a 200000-byte rtlsdr_read_sync after a stream failed with
  bd357dd), a stream whose transfers never got submitted returns the submit
  error, and a DLL build of the fork looks the RAW_IO functions up at run
  time (so rtlsdr.dll still loads next to an older libusb-1.0.dll, with a
  warning and no RAW_IO - heimdall links the static library). The cancel
  wait and RAW_IO stay `_WIN32`; the stall retry and the retune changes
  below run on every platform since krakenrf/librtlsdr 578b43d (pushed
  2026-10-20). heimdall's Makefile requires 578b43d on EVERY platform
  (`RTLSDR_FIX_MARK` = its `rtlsdr_finish_tune`; until then 00606c0's
  `rtlsdr_raw_io`, Windows only): a `../librtlsdr` clone from before
  (00606c0, bd357dd, 08fb081 ...) is fast-forwarded to its upstream (`git
  fetch` + `merge --ff-only`, what install.sh's `git pull --ff-only` does;
  Windows: a librtlsdr.c the old patch modified restored first; local
  changes the merge would overwrite stop it and are never discarded), else
  the build stops with instructions (offline, detached HEAD, no upstream,
  local changes). The working librtlsdr.c is checked on EVERY make
  (`RTLSDR_CHECK`), not only when the library is rebuilt - a library built
  from an older tree was linked again on the second make after a failed
  update; while it lacks the mark every make retries the update (a HEAD from
  before under a working file fixed by hand: once per HEAD,
  `build/.fork_update_tried`, was `.winusb_update_tried`). heimdall's
  CMakeLists.txt refuses such a tree too (both platforms), and a library
  older than the fork's sources. heimdall's code itself still compiles
  against an older fork (`#ifdef RTLSDR_TUNE_KEEP_AUTOTUNE`: full path
  only), but neither build accepts one any more. The Dockerfile pins
  `LIBRTLSDR_COMMIT` (578b43d then, 9644d07 since 2026-11-25); install.sh
  clones master (a fresh install) or `git pull --ff-only`s an existing
  clone (if that fails it warns and builds the old checkout - heimdall's
  make then updates or stops; a clone from 578b43d on that lacks the newest
  features is updated softly, *Keeping the clone current* above). Linux:
  the Makefile's update rebuilds only the static library heimdall links -
  the `rtl_*` tools in /usr/local stay until install.sh runs again. The
  library is rebuilt / checked against the fork's sources on every platform
  (`RTLSDR_SRCS` + touch in the Makefile, `IS_NEWER_THAN` in CMakeLists.txt:
  a new header with an old library would fail the link). That rebuild runs
  `cmake` again on the existing `build/` dir, and the fork's FindLibUSB
  sets its `-DHAVE_LIBUSB_HANDLE_EVENTS_TIMEOUT_COMPLETED=1` /
  `-DHAVE_LIBUSB_ERROR_NAME=1` only while `LIBUSB_FOUND` is not cached: a
  reconfigured library lost them, so `libusb_handle_events_timeout_completed`
  became plain `libusb_handle_events_timeout` and every stream cancel
  (heimdall's stop, a restart) waited out read_async's 1 s event timeout
  (measured on a Pi 5, 2026-11-13: SIGTERM stop 1.4-1.5 s vs 0.4-0.5 s for
  a freshly configured build). The Makefile passes `-ULIBUSB_FOUND` on every
  platform and configures a build dir again once when its CMake cache has
  the call but the library's `flags.make` lacks the define. install.sh and
  the Dockerfile always configure a fresh `build/`. heimdall opens /
  tunes the dongles ONE BY ONE on Windows (`for_each_tuner`, sdr_init.cpp):
  concurrent control transfers to the KrakenSDR's dongles time out (-7);
  Linux keeps its parallel threads (each its own handle)
- Retunes on Windows are slow, so they are coalesced and the fork makes as
  few USB transfers as it can. Measured on the dev PC (2026-10-18, Gigabyte
  X870E, the KrakenSDR's USB2517 hub on an AMD Promontory 21 chipset xHCI,
  PCI 1022:43FD): every USB control transfer takes whole milliseconds - 3 ms
  per write, 4 ms per read, even a standard GET_STATUS, the same straight
  through WinUSB without libusb and through the hub driver's own IOCTLs -
  and they run one at a time for everything behind one root port (5 dongles
  in parallel threads: 4.0 ms per transfer overall; a thread starved past
  librtlsdr's 300 ms control timeout - the -7 of parallel tuning); queuing
  8-16 at once doesn't overlap them. Most likely the controller retries a
  stage the device NAKs only once per 1 ms frame (a full-speed audio device
  on the same controller answers in 0.4 ms). Untested: a port on one of the
  CPU's own USB controllers (1022:15B6/15B7/15B8, empty on that PC) or the
  ASMedia USB4 one - a normal high-speed control transfer is ~0.25 ms.
  rtlsdr_set_center_freq used to be 18 transfers per dongle (+2 for the
  OUT-after-idle stall retry: each dongle idles while the other four tune),
  a coherent retune ~0.42 s; set_tuner_gain 10. Timer resolution is not
  involved (timeBeginPeriod(1) already, and no sleeps in the path).
  The librtlsdr fork (krakenrf/librtlsdr 578b43d "improve tuning speed, fix
  retry", on top of 00606c0; every platform, the stall retry too) cuts a
  retune to 5 transfers per dongle on Windows, 6 on Linux:
  the R820T driver skips writes of the value the
  chip already holds (osmocom rtl-sdr v2.0.2's 0b64f07; `regs_unsynced`
  marks registers a failed write left unknown), writes N / SDM as one
  ascending I2C write of the changed bytes 0x14-0x16 (0x15 always - osmocom
  2acb75c writes them in that order too), and the I2C repeater stays on
  between tuner accesses (`i2c_repeater_on` is set only after the write went
  through; a demod soft reset or a failed I2C transfer clears it, so the next
  tuner access switches it on again - one failed write used to cost only
  that call, and must not leave the tuner unreachable; outside Windows a
  tuner access with it already on starts with a dummy demod read - 1
  transfer instead of the 4 of switching it on and off: Sultan Qasim Khan's
  always-on repeater fork got intermittent I2C failures on tuner bursts
  without a control IN in front until it added that read (the stall retry
  below came later); on the Pi 5 without it 164 / 196 failed I2C writes and 5 / 15
  wrong dividers in two 20 min soaks, with it 0); the gain registers 0x05 /
  0x07 / 0x0c skip unchanged writes too - except
  `rtlsdr_set_tuner_if_mode()`, which always writes 0x0c (heimdall calls it
  after every gain change, so a 0x0c the chip lost is rewritten; a program
  calling only set_tuner_gain doesn't get that) - and a band change writes
  0x1a + 0x1b as one; plus the override-index fix in r82xx_write_arr (`pos`
  counted twice: the init write read override_mask[] past its end); plus the
  drag calls `rtlsdr_set_center_freq_fast` / `rtlsdr_finish_tune`
  (heimdall_v2/CLAUDE.md *Drag retunes*). Register 4 (VCO fine tune) is read
  on every tune, as 00606c0 does: an earlier version took it from the
  previous tune's lock read, stale right after a VCO-divider change (26.6 /
  54 / 109.5 / 220.25 / 441.5 / 884 MHz) - the next tune picked the wrong
  divider and sat off frequency with the lock bit set and no error, 43-44 of
  210 boundary crossings (the Windows build before this change too); caching
  it only when the divider didn't change still gave 1 (Windows) / 2 (Pi)
  wrong steps and 11 in a 30 min Pi soak. After a tune that changed the
  divider (0x10 [7:5]) or the band, and the first after init, the lock read
  waits `R82XX_PLL_SETTLE_US` (5000 us after N / SDM; busy wait on Windows):
  one transfer after the single N / SDM write (osmocom's four writes gave the
  PLL three more) it came too early - a lock retry at every divider point,
  and some dongles then held 0x0b / 0x0c = 0xff (IF VGA at its maximum, 13-15
  dB louder) with the cache at 0x8f / 0xf0. Windows: no retry from 1500 us
  on; Pi 5: 51 retries at 1500 us, none at 2000 / 2500 / 3000 / 5000 - the
  PLL needs > 1.5 ms plus the Pi's ~2.4 ms transfer (~4 ms from the N / SDM
  write), so 5000 is meant to cover hosts whose transfers are ~10x faster
  (x86 Linux, ~0.25 ms: reasoned, not measured). Re-run with 5000
  (2026-10-19, both OSes): the checks below clean again (soaks 15 min
  Windows / 20 min Pi), a crossing step +3.0-3.9 ms per tuner on Windows,
  plain steps and the Pi's sweep (14.35 ms per tune) unchanged. Plain drag
  steps don't wait. The lock and finish reads are 3 bytes as in 00606c0 (5,
  registers 3 / 4 too, left 0x0b / 0x0c corrupted on 2-5 Pi dongles per run
  with lock retries; 3 bytes on none). Measured (2026-10-19, KrakenSDR, 5
  streaming dongles, 40 steps of 25 kHz at 100 MHz, median per 5-tuner step;
  the same in 12 segments 30-1760 MHz across divider / band boundaries):
  Windows (one by one): 00606c0 422 ms (18 + 2 stall-retry transfers per
  tuner), full path 112.5 ms (5), `KEEP_AUTOTUNE` drag steps 72.4 ms (3; the
  first 93.5, a divider crossing 4, the finish 2 per tuner, 47.6 ms);
  Raspberry Pi 5 (Linux), one by one: 215 ms (18) / 71.4 ms (6) / 48.7 ms (4;
  finish 3 per tuner), in parallel threads as heimdall tunes there: 43.6 /
  14.5 / 9.9 ms. The register-4 read costs the 1 transfer per tune more than
  the earlier version (Windows KEEP 47.5 -> 72.4 ms). `SPAN_AUTOTUNE` (72.6
  ms, 3 transfers, measured with an earlier library) - not used by heimdall:
  with KEEP its steps cost the same. Full sweep 24-1766 MHz (3260 tunes):
  Windows 18 -> 5 transfers per tune (max 10), 77 -> 23 ms; Pi 18 -> 6 (max
  9), 42.9 -> 14.3 ms. Both OSes: 0 lock failures, 0 lock retries, 0 wrong
  dividers (210 divider crossings, 7560 drag steps across all 6 divider
  boundaries), registers = 00606c0's at every sweep point, a 30 min drag /
  jump / gain / sample-rate soak with 0 chip-vs-cache mismatches and 0 I2C
  failures; >= 99.8 % (Windows) / 98.2 % (Pi, 00606c0 the same) of the
  sample rate in every 200 ms window, no gap over 20 ms. heimdall's gain
  change (mode + gain + IF VGA) 27 -> 7 (Windows) / 25 -> 10 (Pi),
  set_tuner_gain 10 -> 2 / 3, bandwidth 29 -> 14 / 27 -> 16; registers,
  gains and return codes as 00606c0's. rtl_power 24-900 MHz on the Pi: the
  757-810 MHz LTE band reads higher with the fork (median +2.7 / +2.8 / +4.7
  dB per A/B pair, single hops up to +13) - AGC settling, not tuning: with
  a 50 ms sleep after each retune in both libraries the difference is gone
  (-0.67 / -0.08 / +0.09 dB), and the sleep moves the fork's reading (-4.3
  to -7.3 dB), not 00606c0's (-0.7 to +0.3); the faster retune leaves
  rtl_power's first block of a hop inside the RTL2832 AGC's settling (it
  leaves the IF VGA under that AGC; 00606c0's first hop into the band reads
  ~+4 dB against -7 settled too). The 4 LIBUSB_ERROR_PIPE stalls per dongle
  at open on the Pi (tuner probe: 2 absent tuners, OUT + IN each) are
  00606c0's too; the stall retry now repeats each failed probe OUT 3 times
  (6 more writes + 6 dummy reads per dongle - the Pi judges' ctl_fail at
  open went 20 -> 50 with 5 dongles; the probe result is unchanged).
  Linux (00606c0 too): a pause of >= 300 ms (200 ms: none) inside a tuner
  access fails its next I2C write with PIPE - seen once in heimdall on the
  Pi (a 599 ms gap inside a drag step, all 5 dongles; cause unproven,
  likely the test's USB logging shim) and reproduced with injected pauses.
  FIXED: `rtlsdr_ctrl_transfer_retry` (the dummy demod read, then the OUT
  again; only a PIPE on an OUT, at most 3 times) runs on every platform
  now. Measured on the Pi 5 2026-10-20 (shim pauses only, before every
  23rd tuner write; 00606c0 every 61st): with pauses of 300 / 600 / 1500
  ms every failed write was recovered - KEEP drags 31 of 31 (1 retry
  each), full path 48 of 48 (47 with 1 retry, 1 with 2), 0 failed steps;
  00606c0 with the same pauses 52 failed writes, none retried, 43 failed
  steps; 200 ms: 0 failed writes with the fork. With no pause the
  transfers per step are unchanged (KEEP 4, full path 6, 00606c0 18), and
  the fork's drag, sweep, divtest, xdrag, gain and soak results equal the
  previous build's on the Pi and on Windows (where the compiled library
  is the same code: identical disassembly).
  Pushed as 578b43d (2026-10-20): heimdall's Makefile and CMakeLists.txt
  require it (`RTLSDR_FIX_MARK` = `rtlsdr_finish_tune`, see above); the
  Dockerfile pinned it until 2026-11-25 (9644d07 since).
  A drag sends ~20 FREQ/s and kraken_doa forwards each at once,
  so heimdall used to run every one in order: a 2 s drag ended 15.5 s late.
  Now (all `#ifdef _WIN32`): the 8092 server reads everything waiting and
  skips a command whose setting the next queued command sets again -
  `set_frequency`, `set_gain`, `set_independent_tuner` per channel + field
  set (`superseding_key`, tcp_control_server.cpp), only when that next one's
  values pass process_command's checks (an invalid one runs and answers its
  error, never standing in for a valid one); the skipped one is still
  answered, `{"status":"success","superseded":true}`, so replies stay one
  per command, in order. The same drag now ends 0.58 s late (one stale
  retune + the last; 0.10-0.13 s with the fork's fast retune, which follows
  a drag ~11 times a second instead of ~2.4 - measured with an earlier
  library build). begin_retune_cooldown no longer
  switches an already off noise source off again (10 transfers, 40 ms per
  retune), and set_frequency also enters the cooldown BEFORE the sweep when
  the array is CONVERGED: the header said CONVERGED while the tuners sat on
  two frequencies for the whole first sweep of a drag, so clients computed
  DoA on a split array (now the state leaves CONVERGED ~0.13 s before the
  header frequency changes); TCP_NODELAY on both ends of 8092 (Nagle +
  Windows' ~200 ms delayed ACK; heimdall sets it on Linux too now - without
  it a Pi drag step came ~12 ms late every ~10th step, with it 0 late steps
  in 4 Pi runs, heimdall_v2/CLAUDE.md *Drag retunes*; kraken_doa's end,
  tcp_client.cpp, still only on Windows - its effect on the Pi not
  measured); the page (`retunePending`, server = Windows
  only) doesn't take a frame's centre back into the digits until a frame is
  at the frequency it sent (<= 2 s; not during a playback, which refuses
  FREQ).
  Not done (a plan exists, worth it only while transfers stay this slow):
  the page panning the spectrum locally while dragging, per-channel
  capture-time frequencies in the 8091 header (it reports current_frequency,
  set after ALL tuners, for every channel), retuning the displayed channel
  first, an interruptible sweep, an averaging reset on a retune

## A 3-register tuner read drops 2 samples (measured 2026-11-21)

Every retune moves that dongle's sample stream by exactly 2 samples against
the others (found with sweep mode: `heimdall_v2/docs/dev/operating-modes.md`,
*Review fixes* 8). The cause is not the PLL: it is the I2C READ OF EXACTLY 3
REGISTERS, the PLL lock check `r82xx_read(priv, 0x00, data, 3)` in
`r82xx_set_pll` (tuner_r82xx.c; osmocom's driver reads the same 3, and so
does `r82xx_set_pll_yc`). Each such read makes the RTL2832U drop 4 bytes
(2 I/Q samples) from its bulk stream.

Measured on the KrakenSDR without heimdall (job scratch `lagprobe.py`:
ctypes on the fork's librtlsdr.so, dongles 1000 / 1001 / 1002 streaming
continuously with the noise source on, the lag of 1001 and 1002 against
1000 from a 262144-point cross-correlation before and after ONE operation
repeated 100 times on 1001; 1002 the control - it never moved, and 2 s of
nothing moved nothing):

| Operation on one dongle, x 100 | Lag change, samples |
|---|---|
| `rtlsdr_set_center_freq`: + 100 kHz and back, to the SAME frequency, 433 MHz and back | -200.00 each (-2.000 a call) |
| `rtlsdr_set_center_freq_fast` (KEEP_AUTOTUNE) + `rtlsdr_finish_tune` | -202.00 |
| a gain write; `rtlsdr_is_tuner_PLL_locked` (reads 5) | 0.00 |
| one register rewritten unchanged: 0x05, 0x10, 0x12, 0x14, 0x15, 0x16, 0x17, 0x1a; the autotune clock 128 kHz then 8 kHz | 0.00 each |
| the same, written while the autotune clock is at 128 kHz | 0.00 each |
| read 1, 2, 4 or 5 registers (`rtlsdr_get_tuner_i2c_register`), autotune at 8 or 128 kHz | 0.00 each |
| read 3 registers, autotune at 8 kHz / at 128 kHz / toggled around it | -200.00 each (-2.000 a read) |
| a retune's I/O by hand (128 kHz, read 5, write 0x15, read 3, 8 kHz); the same without the write | -200.00 each |

At 1.2 MS/s the 3-register read costs the same -2.000 samples, so it is a
count of bytes (4) taken out of the stream, not time lost - something in the
RTL2832U's USB / FIFO handling of that control read; why a length of 3 and
not 1, 2, 4 or 5 is not known (longer reads could not be tried through the
public API). The sign: the same instant comes 2 samples EARLIER in that
dongle's stream. The 202 of the fast path is one read more than its 100
calls - consistent with one tune whose lock check ran twice (the loop reads
again after raising the VCO current).

What follows:
- A retune of every tuner together (coherent mode, the wideband scan) takes
  the same 2 samples out of every stream: no lag between channels. Only
  unequal counts show - sweep mode's blocks, independent tuners, a single
  tuner's retry or second lock read. heimdall counts retunes per tuner
  outside coherent mode and re-acquires every lag on the way back
  (`note_tuner_retuned`, `force_recalibration`); in coherent mode a 2-sample
  slip is caught by the lag check of the phase recalibration (> 1 sample).
- A lock check that reads another number of registers does not move the
  stream at all. Measured with scratch builds of the library whose lock
  check in `r82xx_set_pll` reads 5, and 4, registers (same probe, 200 calls
  each): full retunes to the same frequency, + 100 kHz and back, 433 MHz and
  back, a 24.5 / 1700 / 100 MHz round, 109.4 / 109.6 MHz across a divider
  boundary: 0.00 samples each (0.000 a call), no failed tune in 1000+; the
  fast path -2.0 for 200 calls + finish - `r82xx_finish_tune` reads 3
  (`uint8_t data[3]`), once per drag; a 3-register read as control -400.00.
  So the three reads of exactly 3 registers are the whole effect:
  `r82xx_set_pll`'s lock check, `r82xx_set_pll_yc`'s, `r82xx_finish_tune`'s.
  (Since FIXED in the fork - 6da393d, on its `master` since 2026-11-25: *The
  rule, and the fix*, below - the rest of this bullet is what was known
  before.)
  NOT changed in the fork: the lock read was cut to registers 0..2 because
  reading 3 / 4 while the PLL settled was followed by the 0x0b / 0x0c
  corruption (`R82XX_PLL_SETTLE_US`; that wait is in place now, which may
  make 5 safe - the probe did not check the IF VGA registers), so the
  KrakenSDR sweep test behind that note has to be repeated, on the Pi and on
  Windows, before the length changes.
- What the accumulated slip does to heimdall (it wraps into one USB buffer
  at every re-pairing flush, and where it does not): `heimdall_v2/docs/dev/
  operating-modes.md`, *Accumulated slip*.

## The rule, and the fix: no control IN of 3 mod 4 bytes (2026-11-21)

FIXED in the fork: krakenrf/librtlsdr 6da393d + 88993d3 (written as the
local branch `lock-read-no-slip`; on the fork's `master` since 2026-11-25,
with 9644d07 on top). Measured on the KrakenSDR attached to
the dev PC (x86_64 Ubuntu, kernel 6.8, the KrakenSDR's hub on the PC's xHCI),
every number from job scratch 52196024 `fork/` (`README.txt` there lists the
scripts and outputs).

**What drops samples.** Not the I2C read, not the tuner: ANY USB control IN
whose data stage is 3, 7, 11 ... bytes long (length % 4 == 3). Lag probe
(`xprobe.py`, after `e2e/lagprobe.py`: dongles streaming at 2.4 MS/s with the
noise source on, the operated dongle's lag against a reference by a
262144-point cross-correlation before and after 100 transfers; an
instrumented scratch build of the library adds a raw control call and
counters, `instrument.py`):

| 100 control INs on one streaming dongle | lag change |
|---|---|
| tuner I2C read of 3, 7, 11, 15 bytes | -200.00 (-2.000 each) |
| tuner I2C read of 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 16 bytes | 0.00 |
| USB block read (0x2000..) of 3, 7, 11, 15, 19, 23, 27, 31, 35, 63 bytes | -200.00 each |
| USB block read of 1, 2, 6, 14, 64 bytes | 0.00 |
| SYS block (GPO) / demod (page 0x0a) / EEPROM (I2C 0xa0) read of 3 bytes; demod read of 7 | -200.00 each |
| the same blocks, 1 byte; demod 2 bytes | 0.00 |
| standard GET_DESCRIPTOR (device / config / string 0) with wLength 3 or 7 | -200.00 each |
| GET_DESCRIPTOR with wLength 18 or 255 (returns 18 / 4 / 10 bytes), GET_STATUS (2) | 0.00 |
| an I2C read of 3 bytes from an absent address (fails: no data stage) | 0.00 |
| every write tried (tuner registers 0x05 ... 0x1a, GPO, the autotune clock) | 0.00 |

So it is the RTL2832U's USB device controller (a standard request does the
same as a vendor one), it counts bytes, not time (1.2 MS/s earlier: the same
2.000), and the samples the transfer takes are always 4 bytes, the same instant
then coming 2 samples earlier in that dongle's stream. The tuner's I2C reads
are limited to 16 bytes (17 ... 32 fail with PIPE); a 16-byte read returns
the chip's real registers 0x00 ... 0x0f (0x05 ... 0x0f equal the driver's
cache), which the corruption test below uses.

**Where the library made such reads**: the R820T lock check in
`r82xx_set_pll` (once, twice when the first read shows no lock),
`r82xx_set_pll_yc` and `r82xx_finish_tune` (3 bytes each - the whole slip of
every retune); `rtlsdr_get_tuner_i2c_register()` with len 3 (it reads
min(len, 5)); the FC0012 driver's register dump (any length up to 16);
`rtlsdr_ir_query()` (the IR code's byte count). Every other read is 1, 2, 4
or 5 bytes (the demod / USB / SYS / GPIO register reads, the dummy read,
EEPROM bytes, the E4000 / FC0013 / FC2580 drivers' 1-byte reads, the
register-4 read of a tune, `rtlsdr_is_tuner_PLL_locked`, the R820T gain
path's 4), or a libusb string-descriptor read (even lengths).

**The fix** (two parts, every platform):
- `rtlsdr_ctrl_transfer_retry` (librtlsdr.c, the one path of every control
  transfer of the library): a control IN of length % 4 == 3 (<= 255) is made
  one byte longer into a bounce buffer and the caller gets its own bytes -
  the one more register / byte read has no effect on any read the library
  makes (status registers, the IR code RAM the query resets afterwards, the
  R820T's read-only registers 0..4). Covers the lock reads, the public
  register read, the other drivers and any future read.
- The R820T lock check reads 4 registers explicitly (`R82XX_LOCK_READ_LEN`,
  tuner_r82xx.c) - 4 and not 5 for the corruption result below.
- `RTLSDR_NO_STREAM_SLIP` in rtl-sdr.h: applications (heimdall) can tell at
  compile time.

**The lock read length and the register corruption.** The lock read had been
cut to 3 registers because longer reads were followed by 0x0b / 0x0c = 0xff
on some dongles (Pi, 2026-10, with lock retries - *above*). How that showed:
a dongle 13-15 dB louder for the rest of the session (0x0c is the IF VGA;
the driver's cache still held the right value, so nothing rewrote it). The
test that would catch it (`tsoak2.py`): all five dongles streaming (noise
source on, gain 0, 2.4 MS/s), tuned in parallel threads (a barrier per step,
as for_each_tuner), each through the `bounds` plan from its own offset -
both sides of every VCO divider boundary (26.6 / 54 / 109.5 / 220.25 /
441.5 / 884 MHz) and band boundary of the driver, 0.4 MHz each way, and the
range ends 24 / 1766 MHz (108 tunes a pass, ~31 % of them at a settle
point); after every tune and 12 ms: the stream's power (one 16384-sample
buffer) against the median of that dongle at that frequency, then the
chip's registers 0x05 ... 0x0f read back (16 bytes) against the cache, a
mismatch rewritten from the cache; a level more than 6 dB off is diagnosed
(GPO read, registers 0x10 ... 0x1f rewritten one at a time from the cache
until the level is back). The scratch library sets the lock read length
and the settle wait at run time. The level repeats to 0.02-0.03 dB (median
|deviation|, p99.9 0.1-0.3 dB), so either kind of corruption stands out.

Provoked (settle wait shortened; x86_64 here makes ~2 ms per control
transfer, like the Pi), 5 min a run, ~5900 tunes per dongle, 29500 in all:

| settle | lock read | lock retries (all at a settle point) | 0x0b(/0x0c) = 0xff (tunes) | level -27 ... -32 dB, fixed by rewriting 0x1b / 0x1c |
|---|---|---|---|---|
| 0 | 3 | 2510 | 0 | 5 (2 / 3) |
| 0 | 4 | 2440 | 15 (0x0b only; each with a lock retry) | 10 (0x1c) |
| 0 | 5 (4 min) | 1957 | 19 (0x0b + 0x0c, +12.3 ... +14.4 dB; each with a retry) | 12 |
| 1500 | 4 | 1578 | 6 (5 with a retry, 1 without) | 4 |
| 2500 | 4 | 1 (on an ordinary step) | 0 | 0 |
| 3000 | 4 | 2 | 0 | 0 |
| 5000 (the default) | 4 (10 min, 55920 tunes, 17070 settle points) | 0 | 0 | 0 |
| 5000 | 5 (27935 tunes) | 0 | 0 | 0 |

(8-pass runs, 4320 tunes each, before: settle 0 - read 3 / 4 / 5: 354 /
350 / 347 retries, 0 / 0 / 4 corrupted tunes; 1500: 193 / 194 / 205
retries, 0 / 1 level drop / 2; 5000: 0 retries, nothing.) So:
- the hazard is a lock read that comes before the PLL has settled after a
  divider or band change - every lock retry was at a settle point but one
  (2500 us: one ordinary step); the old 0x0b / 0x0c finding reproduces
  exactly (+13 dB); a tune whose lock bit was off again at rest (12+ ms
  after a tune that had reported the lock; level normal, the next tune
  fine) came about once in 30000 tunes at 2500 / 3000 / 5000 us (read 5)
  and settle 0 (read 4), never in the 56000 of 5000 us / read 4 - not tied
  to the length, and nothing a driver could see at the tune;
- 3 registers were not safe either: 0x1b (tracking filter) / 0x1c (mixer /
  power detector top) corrupted by early reads of any length, the stream
  ~30 dB weaker until the next band change rewrites them (0x1b) or forever
  (0x1c: written only at init) - never noticed before because no test
  looked at the level after each tune; reading 4 adds 0x0b, 5 also 0x0c;
- what protects is the settle wait (`R82XX_PLL_SETTLE_US`, 5000 us): on this
  host early reads stop between 1500 us (~1600 retries in 5 min) and 2500 /
  3000 us (1-2 in 30000 tunes), none at 5000 - a margin of less than 2x over
  the last value with a retry, so a host whose transfers are much faster
  than ~2 ms (the 0.25 ms of an ordinary high-speed device) is not covered
  by this measurement; with the wait no read came early and nothing was
  corrupted at either length. 4 is the shortest length that doesn't drop
  samples and the one that, should a read still come early, corrupts least.

**Big jumps read the lock early too - FIXED (default path).** The same test
with random frequencies 24-1766 MHz (`--plan random`, 500 a pass, 3 min, 5
dongles), settle 5000 us, lock read 4: full path 548 lock retries in 16490
tunes (3.3 %), KEEP + NO_DUMMY_READ 635 in 17950 - none at a settle point;
1 / 4 tunes left 0x0b = 0xff, and the full-path run had levels down to -31
dB (0x1b / 0x1c). Every retry was a jump inside one divider and one band:
in the top divider (RF 885-1766 MHz, VCO 1777-3540 MHz) the driver's
frequency ranges are all one band, so a jump from 900 to 1700 MHz never
waited. By jump size (same divider): < 400 MHz RF 0 retries in 4597 tunes
(790 of 300-400 MHz in the top divider), 400-450 MHz 5 / 215, 450-500 27 /
503, 500-550 205 / 393, > 550 MHz 66-81 % of the tunes. Fix
(`R82XX_PLL_SETTLE_VCO_JUMP_KHZ`, tuner_r82xx.c): the settle wait also
after a tune that moves the VCO by more than 400 MHz (half the smallest jump
seen to need it; a sweep's 2.4 MHz hops move it by at most 154 MHz, at 24
MHz). Measured with it: random plan, full path 0 retries in 16505 tunes, 0
register mismatches (2912 of 3301 tunes per dongle now settle points). This
was in 578b43d / the 3-register build too (the 0x1b / 0x1c corruption
happens at any read length): a typed jump in coherent mode or a wideband
spread across the top divider could leave a dongle ~30 dB deaf.

**Retune speed: what the library can give a sweep** (2026-11-21, same host).
Where the time goes (`tim.py`, the instrumented library times every control
transfer; 5 dongles streaming, 5 operated in parallel threads as
for_each_tuner; load ~2):

| | ms | control transfers |
|---|---|---|
| GET_STATUS / USB / SYS register read (1 byte) | 1.8 | 1 |
| demod register read (the dummy read) | 2.4 | 1 |
| tuner read 1 / 5 / 16 bytes (dummy + I2C IN) | 4.5 / 5.0 / 6.3 | 2 |
| tuner register write through the public API (dummy + I2C OUT) | 4.7 | 2 |
| full tune, same band (dummy, 0x1a 128 kHz, read 5, N / SDM, lock read, 0x1a 8 kHz) | 14.4 | 6 |
| SPAN_AUTOTUNE (128 kHz inside the N / SDM write) | 12.6 | 5 |
| KEEP_AUTOTUNE (no 0x1a writes) | 9.8 | 4 |
| KEEP_AUTOTUNE + NO_DUMMY_READ (new) | 7.5 | 3 |
| full / KEEP / KEEP + NO_DUMMY_READ: sweep 24-1766 MHz by 2.4 MHz | 14.0 / 9.8 / 7.5 | 6.0 / 4.0 / 3.1 |
| full / KEEP: band change 100 <-> 433 MHz (incl. the 5 ms settle) | 24.2 / 19.7 | 8 / 6 |
| a KEEP tune + rtlsdr_finish_tune (dummy, lock read, 0x1a 8 kHz) | 19.2 | 8 |

With no dongle streaming and one dongle operated, every row is within 0.5
ms of these (GET_STATUS 1.73 ms): a control transfer costs ~1.7 ms here
whatever it carries and however many dongles stream or tune at once (the
five parallel threads cost no more than one) - the host / hub / RTL2832U
side, not something the library can shorten; the I2C bytes add ~0.11 ms
each (~80 kHz bus, SYS_I2CCR 0x3040 = 23). So the library's lever is the
number of transfers:
- the 0x1a autotune-clock writes: KEEP_AUTOTUNE leaves the clock at 128
  kHz between hops (the drag fast path, `rtlsdr_finish_tune` at the end);
- the dummy demod read at the start of every tuner access (Linux; it keeps
  a control OUT from coming first after a quiet spell - measured on the Pi,
  *above*): new `RTLSDR_TUNE_NO_DUMMY_READ` makes it only when the access's
  first transfer is a write - a same-band KEEP hop starts with the
  register-4 read, an IN, so the read is not needed in front of it (the
  rule the dummy read implements - an IN before the first OUT - still
  holds). Behind a flag, not the default, because the dummy read has a
  measured reason on the Pi and this was measured only here;
- kept: the register-4 read (wrong dividers without it, *above*), the lock
  read (the only way a failed tune is seen), the settle wait (above).

Is data on the KEEP path (autotune clock at 128 kHz while the data is taken)
as clean as on the full path? `spec.py atune`: 8 frequencies (FM band, the
Tait DMR / MPT1327 band at 415.5 MHz with 64 dB carriers, P25 at 499.3 and
142.3 MHz, quiet 700 / 1200 / 1700 MHz, 27 MHz), all 5 dongles, noise
source off (antennas), A = full tune / B = KEEP tune from 400 kHz away,
alternated 4 times, 0.41 s each, 4096-point PSDs: noise floor B - A within
+-0.2 dB at every frequency but 415.5 MHz (-0.1 ... -1.2 dB there, a band
whose bursts vary 50 dB between the A halves themselves); the largest
per-bin |B - A| (0.6-0.9 dB in quiet bands, 1-6 dB in busy ones) below the
spread between the two halves of A alone (1.0-1.4 / 3.6-55 dB) everywhere:
no spur, no floor change, the carriers' shape the same.

Frequency truth (`spec.py truth`): five known carriers (415.6375, 416.5875,
456.50625, 499.1875, 142.35 MHz control channels), per dongle a baseline PSD
(65536 points, 36.6 Hz bins), then 15 rounds of every dongle to a random
frequency 24-1766 MHz and back; the carrier's shift against the baseline
(cross-correlation of the PSDs, sub-bin): full path max 1.6-5.6 Hz for the
strong carriers, 11-26 Hz for the weak VHF one (SNR 20-28 dB); KEEP +
NO_DUMMY_READ max 1.6-14 Hz / 7-13 Hz: on the same bin on every dongle, 0
failed tunes.

The sweep path itself, final library (KEEP_AUTOTUNE + NO_DUMMY_READ, a
finish every 20th step): full range 24-1766 MHz by 2.4 MHz, 8 min, 5
dongles in parallel: 40700 tunes, 0 lock retries / failures / I2C errors /
PIPE retries / register mismatches, 3.0 transfers a hop, 8.3 ms per
5-tuner step (incl. the 16-byte readback the test adds); the bounds plan
with an idle spell of 600 ms before every 7th step (the stall the dummy read
guards against comes after >= 300 ms of quiet), 5 min: 1600 tunes after a
pause, 0 PIPE retries, 0 I2C errors - but whether this host stalls at all
without the dummy read is not known (no positive control: the Pi showed
it, this PC was not tried without the read); random jumps 17990 tunes: 0
retries, 0 corruption; the FM band 5 min, 31540 tunes: all zeros, level
max |deviation| 0.14 dB. Lag probe on that path (300 tunes a pattern, the
others streaming): 0.000 samples per tune for 88-108 MHz steps, the full
range, divider / band boundaries, random jumps, KEEP|SPAN, NO_DUMMY_READ on
the full path; a raw 3-byte IN -2.000.

**The I2C clock** (SYS_I2CCR, 0x3040, read 23 at power-up; the library never
writes it): it is the bus clock divider - one dongle, idle, 60 reads each,
16-byte tuner read 6.06 ms at 23 (~80 kHz), 4.82 at 11, 4.31 at 5, 8.29 at
46; every read OK. At 5 a KEEP + NO_DUMMY_READ hop would lose ~1.3 of its
7.5 ms (its ~15 I2C bytes). A soak at 5 (bounds plan, KEEP +
NO_DUMMY_READ, 5 min, 34305 tunes, restored to 23 afterwards): 0 I2C
errors, 0 register mismatches, 0 level outliers - but 1 lock retry on an
ordinary step (0 in every run at 23 with the default settle): a faster bus
brings the lock read closer to the N / SDM write, i.e. towards the early
read that corrupts registers. Not adopted; it would need its own settle
margin measured, and the R820T's / board's I2C margins are not known.

**Lag probe proof with the final library** (`proof.sh`, `xprobe.py`; all
five streaming, each dongle in turn the operated one; 1000 tunes a pattern
on 1001, 300 on the others): full path to the same frequency, 25 kHz steps,
+100 kHz and back, 433 MHz and back, both sides of every divider boundary,
of every band boundary, the range ends 24 / 1766 MHz, random frequencies;
KEEP steps + finish, KEEP across a boundary, a finish after every step; the
library's 3-byte tuner read: 0.000 samples per call for every pattern on
every dongle (largest |lag change| over a whole pattern 0.01-0.02 samples),
the other four never moved; the raw 3-byte IN in the same run -2.000 (-2000
for 1000). 0 failed calls, 0 lock retries (random jumps included).
heimdall's other calls while streaming (200 each on 1001): gain, gain mode,
IF mode, heimdall's gain change (mode + gain + IF VGA), AGC on / off,
GPIO0 / bias tee / GPIO1 rewrites, the sample-clock register rewritten (0.0)
and toggled 1e-7 / 0 (-0.10 in all: the clock moves on purpose),
`rtlsdr_is_tuner_PLL_locked`, `rtlsdr_get_tuner_i2c_register` (5),
finish with nothing pending: 0.000. The rule set again on the fixed library
(the library's own path): every length and block 0.000.

**Long runs with the final library** (default settle, `tsoak2.py`, 5
dongles in parallel): the bounds plan 9 min, 50245 tunes (15350 settle
points): 0 lock retries / failures / I2C errors / PIPE retries, 0 register
mismatches, 0 level outliers (max |deviation| 0.28 dB); 3 times dongle 1000's
lock bit was off at rest at 53.6 MHz (just below the 54 MHz divider point,
the same tune as in the 3000 us run; level normal). The full range 24-1766
MHz by 2.4 MHz, 9 min, 56395 tunes: 0 retries / failures / errors /
mismatches; the level outliers (845) are all dongle 1001 at 1560-1726 MHz -
the two-state level below, the library without the fix shows it too (314 in
3 min, 1565-1726 MHz). The FM band (88.2-108 MHz, 12 hops, band changes at
90 / 100 MHz), 9 min, 49990 tunes: all zeros, level max |deviation| 0.16
dB. 27 min of tuning in all, 156630 tunes, no failed tune.

**A two-state level at the top of the range (found, not fixed).** Dongle
1001 at 1.56-1.75 GHz reads ~13-15 dB louder when tuned there from above
(from 1766 MHz, or on a downward sweep) than from below - the same divider
and VCO fine-tune bits, the chip's registers 0x00 ... 0x0f read back and
the driver's whole cache 0x05 ... 0x1f byte for byte identical in both
states (only status register 1 varies, in both), and rewriting 0x10 ...
0x1f from the cache doesn't change it (`hyst.py`, `tsoak2.py` diagnosis):
the tuner's internal state differs, not the driver's settings. In the
random plan dongles 1000 / 1002 / 1003 showed it too (+-12-16 dB, 1.60-1.74
GHz). Present with the fork's master (3-byte lock read) as well; on the full
path and the fast path alike. A sweep that steps upward sees the "from
below" level except where its blocks start (a wrap from a higher block).
Cause unknown (the VCO at the top of its range: LO 1.6-1.77 GHz = VCO
3.2-3.54 GHz, vco_max 3.54 GHz).

**Review round (the second commit, 88993d3, 2026-11-21).** An
independent review found no hard defect; three risks and three nits were
closed:
1. *A lock retry left corrupted registers unrepaired* (the cache keeps the
   right values, so nothing rewrote them). Now `r82xx_repair_early_read()`
   (tuner_r82xx.c): after a lock retry and when `rtlsdr_finish_tune` finds
   no lock, it waits `R82XX_PLL_SETTLE_US` and writes 0x0b-0x0f and
   0x1b-0x1f back from the cache (counted: `early_read_repairs`). How it
   got there, all provoked at settle 0 (bounds plan, 5 min, 5 dongles,
   about 24000-29500 tunes and 2000-2400 retried tunes a run; job scratch
   `fork/out/q7.out` - `q11.out`):

   | repair | tunes left with a register off the cache (16-byte readback) | level drops > 6 dB | lock off at rest |
   |---|---|---|---|
   | none | 17 (0x0b, each with a retry) | 6 (0x1b / 0x1c) | 0 |
   | at a retry and at every settle point, written at once | 6 (0x0d-0x0f, no retry: the repair's own writes) | 6 (fixed by 0x19) | 6 |
   | after a retry only, 5 ms wait first, 0x0b-0x0f + 0x17-0x1f | 0 | 4 (0x1c, no retry) | 0 |
   | at a retry and at every settle point, wait, 0x0b-0x0f + 0x17-0x1f | 0 | 12 (put right only by PLL registers) | 0 |
   | at a retry and at every settle point, wait, 0x0b-0x0f + 0x1b-0x1f | 0 | 6 | 6 |
   | after a retry only, wait, 0x0b-0x0f + 0x1b-0x1f (THE FIX) | 0 in 2261 retried tunes | 5 (0x1c, no retry) | 0 |

   So the repair makes a retried tune whole; what it can't reach is an
   early read that already saw the lock (no retry) and hit 0x1b / 0x1c -
   repairing after every settle point instead traded those for PLL-unlock
   drops and costs ~12 ms per settle point, so it isn't done. At the
   default settle wait neither case was ever seen (above).
   The committed library once more (settle 0, 5 min): 27850 tunes, 2278
   retried and repaired, 0 tunes off the cache, 0 lock off at rest, 1
   level drop (0x1c, no retry). Re-run on the committed library (3adc8e6,
   load ~2.1-2.3): lag probe with the settle at 0 (300 tunes a pattern on
   1001, all five streaming) - 119 retried and repaired tunes, 0.000
   samples per tune on every pattern, the raw 3-byte IN -2.000: the
   repair's writes don't move the stream; default path, bounds plan 5 min,
   27900 tunes: 0 retries / failures / I2C errors / mismatches, level max
   |deviation| 0.16 dB, 0 repairs needed; sweep path (KEEP_AUTOTUNE |
   NO_DUMMY_READ) full range 5 min, 36400 tunes: the same zeros, 3.0
   transfers a hop, 8.3 ms per 5-tuner step (outliers only 1001's
   1.56-1.73 GHz two-state level, below); lag probe on the sweep path (on
   1002): 0.000 for every pattern, raw control -2.000. 0 lazy-read retries
   in every run.
2. *NO_DUMMY_READ*: when the access's first transfer - the real register
   read - fails under the flag, the dummy read is made and the read tried
   once more (`i2c_in_lazy_retries`); the header says plainly that the
   stall was a Pi finding with no positive control here. Not provoked (no
   read failed in any run: 0 lazy retries).
3. `RTLSDR_NO_STREAM_SLIP`'s header now says exactly what holds (every
   tune, gain / IF / AGC / GPIO / lock call and every other control
   transfer the library makes) and what is outside (`rtlsdr_reset_buffer`,
   `rtlsdr_set_sample_rate` - a demod soft reset -, direct sampling and
   offset tuning; the sample-clock calls move the stream on purpose), and
   the rule as measured: the length of the data stage actually
   transferred (the RTL2832U's vendor reads return wLength; libusb's own
   string-descriptor reads ask for 255 and get the descriptor's even
   length - measured 0.00).
4. The VCO-jump settle was measured only in the top divider; elsewhere it
   is margin. The tunes it makes wait (same divider and band, LO jump >
   400 MHz / mix_div): mix_div 64 (LO 27.7-55 MHz) > 6.25 MHz within LO
   27.7-50; 32 none (bands <= 10 MHz); 16 > 25 MHz inside 140-180 /
   180-220; 8 > 50 MHz inside 310-442; 4 > 100 MHz inside 450-588 /
   650-885; 2 > 200 MHz. Sweep hops and drag steps never cross these.
5. `r82xx_set_pll_yc` (vco_algo 2) has no settle wait and now reads 4
   registers - dead code: the `vcoalgo=` option writes `vco_curr_max`, so
   vco_algo is always 0. Left inactive, noted at the function.
6. Wording: the threshold is 400 MHz of VCO (the retries were jumps of >
   400 MHz of RF = 800 MHz of VCO). The padded IR read reads one more byte
   of `IR_RX_BUF` - code RAM at 0xFC00 that `rtlsdr_ir_query` resets right
   after: harmless, said at the padding.

**On a Raspberry Pi 5 (2026-11-21).** The test Pi (a
Pi 5 Model B Rev 1.1, 8 GB, kernel 6.18.34, the KrakenSDR behind its hub on
the Pi's xHCI bus 1, 480M; idle apart from the other workers' turns), the
fork built there from clean clones (88993d3 = the two commits above, and
3ad1746 without them), the same tools (job scratch `fork/pi/`, outputs
`fork/pi/out/`). 46-53 °C, `get_throttled` 0x0, load 0.0-3.3 in every run.
Everything the PC showed, the Pi shows too:
- the rule: raw control INs of 1-16 bytes, USB block and tuner I2C reads
  alike: 3, 7, 11, 15 bytes -2.000 samples each, every other length 0.000;
- the slip (lag probe, 300 tunes a pattern on 1001, all five streaming):
  without the fix -2.000 per tune on every lock-read path (fast KEEP +
  finish -2.013, a finish after every step -4.000, random jumps -2.127 with
  18 lock retries - no VCO-jump settle there), 0.000 for gain / IF / AGC /
  GPIO / lock-API calls; with it 0.000 on every pattern (largest change over
  a pattern 0.02), the sweep path included, 0 retries on random jumps;
- the time: a control transfer 1.87 ms (PC 1.73-1.83), the demod read 2.37;
  full tune 14.4 ms / 6 transfers, KEEP 9.9 / 4, KEEP + NO_DUMMY_READ 7.5 /
  3, the finish 19.2 incl. its KEEP tune, a band change 24.2 / 19.6, sweep
  hops of 1.9125 MHz at 89-108 / 600-619 / 1200-1219 MHz full 14.5 / 14.5 /
  14.5, KEEP + NO_DUMMY_READ 7.6 / 7.5 / 7.5, a 900 <-> 1500 MHz VCO jump
  (the new settle) 19.5 / 12.6 - five parallel threads cost what one does,
  idle or streaming, as on the PC;
- NO_DUMMY_READ: the sweep path over the full range 8 min, 59720 tunes, and
  the same without the flag (KEEP only) 56555 tunes: 0 lock retries /
  failures / I2C errors / register mismatches / lazy-read retries, no
  control error during tuning (only the tuner probe's 2 IN + 8 OUT PIPE
  stalls at `rtlsdr_open`, on every dongle, as since 578b43d); 3.0 against
  4.0 transfers a hop, 7.95 against 10.34 ms per 5-tuner step - the flag
  is safe here as far as that goes, but the stall it relates to was not
  produced on the Pi either (no positive control);
- heimdall's sweep exit on the bare library (`exitsim.py`, 50 repetitions,
  ~49000 KEEP + NO_DUMMY_READ hops of all five in parallel, a finish, a
  full tune of all five, noise on, the lags measured +-131072 samples): 0
  lag changes >= 1 sample (largest 0.05), the delivered buffers equal on
  all five each time, 46582 bulk transfers per dongle all COMPLETED (0
  error / stall / overflow / short);
- frequency truth on both paths: the strong carriers within 1.0-12.3 Hz
  (bins 36.6 Hz), the weak ones here (SNR 8-17 dB - another antenna /
  site) up to 243 Hz of estimation noise, 0 failed tunes.
- every dongle as the operated one (100 tunes a pattern): 0.000 on every
  pattern, the raw 3-byte IN -2.000;
- random jumps 24-1766 MHz (the VCO-jump settle), 27500 tunes: 0 lock
  retries, 0 failures, 0 mismatches (the PC before that settle: 548 retries
  in 16490);
- the sweep path with a 600 ms idle spell before every 7th step, 11235
  tunes (1605 after a pause): 0 PIPE, 0 I2C errors, 0 lazy-read retries;
- the default path, 20000 scanner-like full-path tunes on one dongle at a
  time (FM steps of 1.9125 MHz, every 13th a band change to 433.92 MHz,
  every 29th a random frequency; 17.6 ms each), the others streaming:
  each of the five dongles in turn, 100000 tunes in all: lag change over
  the 20000 -0.02 ... +0.03 samples (0.000 a tune), 0 lock retries, 0
  failures, 0 control errors; a raw 3-byte IN x 100 after each -200.00;
- The two-state level at the top of the range: here only dongle 1002 at
  1698.4 MHz in the random plan (-14.7 dB, 3 times), not in the full-range
  sweeps (the PC: 1001 at 1.56-1.73 GHz).
- the settle margin (bounds plan, 5 dongles, the committed library with the
  early-read repair):

  | settle | tunes | lock retries (repaired) | tunes off the cache | level drops (0x1c, no retry) | lock off at rest | PC (same library) |
  |---|---|---|---|---|---|---|
  | 5000 (8 min) | 44615 | 0 | 0 | 0 | 0 | 0 retries in 27900 |
  | 3000 | 29665 | 0 | 0 | 0 | 2 | 2 retries in ~30000 (read 4, no repair) |
  | 2500 | 29665 | 0 | 0 | 0 | 0 | 1 |
  | 1500 | 28125 | 1609 | 0 | 11 | 0 | 1578 (no repair) |
  | 0 | 27605 | 2214 | 0 | 6 | 2 | 2278, 0, 1 |

  So the Pi's PLL needs what the PC's does: early reads start between
  2500 and 1500 us, 5000 us is a 2x margin over the last value without a
  retry on both - no change needed.

The library's async path, read for a whole-buffer loss (asked after a
sweep exit in heimdall left one channel without a correlation peak on the
Pi): on Linux a bulk transfer that ends ERROR, STALL or OVERFLOW (TIMED_OUT
can't: `BULK_TIMEOUT` 0) is neither delivered nor resubmitted and nothing
is logged until `xfer_errors` (ERROR only) reaches the buffer count - that
dongle's stream silently loses up to one transfer (16384 samples) and runs
on with one transfer fewer in flight; a failed resubmit (its result ignored)
shrinks the queue without losing data. None of those statuses occurred in
any run here.

**Not tested** (say so plainly): a Raspberry Pi 4, and the Pi 5 with the
noise source off for the margin runs (all with it on); Windows (3-4 ms per transfer, one dongle at a
time: the transfer counts above should carry over, the times not; the
padding and the VCO-jump settle are plain C, every platform), macOS; other
tuners than the R820T (the padding covers their reads; their tune paths
make no 3-byte read); a dongle without a KrakenSDR's noise source; a
positive control for NO_DUMMY_READ (the stall the dummy read guards was
produced neither on the PC nor on the Pi 5 above - the flag was clean on
both, 60k + 11k tunes on the Pi); the I2C clock change under load. No MinGW / zig on this machine: the Windows and
glibc syntax checks of the suite rule were not run (the changes are plain C99
/ C++ without new platform calls).

## A fast tuner gain write (2026-11-23, the third commit: 9644d07)

Sweep mode's automatic gain writes a tuner's gain with its retune whenever
the next hop wants another (30-44 writes in a 4 s sweep of 911 hops). The
ordinary `rtlsdr_set_tuner_gain` costs 4 control transfers on Linux - the
dummy demod read, `r82xx_set_gain`'s read of tuner registers 0..3 (its
result is not used), the LNA write (0x05), the mixer write (0x07) - and
heimdall's gain change (gain mode + gain + IF mode) 10.

The commit (krakenrf/librtlsdr 9644d07, on 88993d3; macro
`RTLSDR_HAS_GAIN_FAST`):
- `r82xx_set_gain_fast()`: the manual gain the ordinary call sets - the same
  table and rounding (`r82xx_get_rf_gain_index`), the same cache and
  `last_*` values, the IF VGA left as it is - without the 0..3 read, and
  with LNA and mixer in ONE I2C write of 0x05..0x07 when both change (0x06
  goes out with its cached value); one changed: that register; neither: no
  transfer. In AGC or extended mode: the ordinary path (which switches to
  manual, as `rtlsdr_set_tuner_gain` does).
- `rtlsdr_set_tuner_gain_fast(dev, gain, flags)`: that write alone (the
  dummy read stays - a write comes first).
- `rtlsdr_set_center_freq_gain_fast(dev, freq, gain, flags)`: the fast tune
  (`flags` as for `rtlsdr_set_center_freq_fast`), then the gain in the same
  hold of the I2C repeater - the tune's transfers come first, so the gain
  needs no dummy read: +1 transfer when it changes, +0 when not. The gain is
  written even when the tune fails; the tune's error is returned first.
- Other tuners than the R820T / R828D, direct sampling: the ordinary calls.
  Neither call reads 3 mod 4 bytes: no sample is dropped.

Measured on the dev machine's KrakenSDR (2026-11-23, x86_64, all five
streaming, five in parallel threads, 200 calls each, load 4-9 - other jobs;
`gaintest.py`, job scratch `fork/out/g9_x11.out`):

| | ms (median) | transfers |
|---|---|---|
| `rtlsdr_set_tuner_gain`, 20.7 <-> 29.7 dB | 9.13 | 4.0 |
| heimdall's gain change (mode 1 + gain + IF mode) | 22.82 | 10.0 |
| `rtlsdr_set_tuner_gain_fast`, the same gains | 4.79 | 2.0 |
| `rtlsdr_set_tuner_gain_fast`, unchanged / LNA only | 0.00 / 4.54 | 0 / 2.0 |
| fast tune (KEEP_AUTOTUNE \| NO_DUMMY_READ) alone, FM hops | 7.44 | 3.38 |
| that fast tune + the ordinary gain (today's sweep) | 16.58 | 7.36 |
| `rtlsdr_set_center_freq_gain_fast`, gain changing every hop | 9.86 | 4.36 |
| the same, gain unchanged | 7.40 | 3.36 |
| the same on the full path (flags 0) | 16.63 | 7.18 |

So a hop that changes the gain costs 2.4 ms more than one that doesn't,
instead of 9.1 ms. The gain itself: every value of the table (29) and 30
odd ones from -10 to 509, set from a far gain by the ordinary call, by
`rtlsdr_set_tuner_gain_fast` and by `rtlsdr_set_center_freq_gain_fast` on
all five: the same LNA / mixer registers in the cache and on the chip (the
16-register readback, 0x05..0x0f = the cache every time), the same
`rtlsdr_get_tuner_gain`, the same stream level (noise source, 100 MHz)
within 0.07 dB (median 0.03); 0 of 59 differ. The cache after fast calls:
20 rounds of 30 tune + gain calls per dongle followed by heimdall's ordinary
calls (gain mode, gain, IF mode, a full tune) - 0 of 100 readbacks with the
chip off the cache; the fast call in AGC mode switched to manual exactly as
the ordinary one (chip = cache). The stream: 200 fast gain writes, and 200
tune + gain calls, on 1001 with all five streaming - lag change 0.00 / -0.01
samples (0.0000 a call), the others 0.00. The earlier lag-probe sets (every
tune path, the sweep path) on this library (`fork/out/g9_probe.out`, 100
tunes a pattern on 1001): 0.000 samples a tune on every pattern, the same
times as before (full 13.8 ms / 6 transfers, KEEP 9.7 / 4, the sweep path
KEEP + NO_DUMMY_READ 9.5 / 3.45 over the full range), 0 retries; the raw
3-byte IN -2.000.

Not tested: Windows (expected, from the code: Windows makes no dummy read,
so the gain is 1 transfer inside the tune and 1 alone, against 3 for the
ordinary call - read 0..3, LNA, mixer - at 3-4 ms each: a gain-changing hop
~+3.5 ms instead of ~+10.5), the Raspberry Pi
(its transfers measured like the PC's: the same counts and times expected),
other tuners (they take the ordinary call), heimdall's use of it (worker S).

## Zero-copy USB buffers (investigated 2026-11-12/14)

**State since 2026-11-14** (krakenrf/librtlsdr 701a832 + 3ad1746 - local
commits until they were pushed on 2026-11-25; the rest of this section is
the history that led there, written
while zero-copy was still off): zero-copy is compiled in on Linux by default
(`ENABLE_ZEROCOPY 1`; `-DENABLE_ZEROCOPY=0` leaves it out), and a handle
uses it only if its application called `rtlsdr_prealloc_async()` - heimdall
does, at open; rtl_test / rtl_fm / rtl_tcp and any other user of the library
get user-space buffers as before. heimdall's terminal dashboard shows the
result on its SYSTEM line (`zerocopy on` / `off`, `k/n` when some dongles
fell back; `n/a` off Linux). Not for every handle because (measured)
mapping inside `rtlsdr_read_async` delays the stream start, and on a Pi the
mapped buffers are slow to read byte by byte (zctest's callback 7-10x
slower), while one memcpy per buffer - heimdall's callback - is cheaper than
without zero-copy. Measurements, 5-channel KrakenSDR, 5 elements:
- Pi 4 (Rev 1.5, VL805, 1 GB), plain zero-copy mapped at stream start, no
  start-sync: 5 of 8 cold starts never converged (first lags 8651 / 11976 /
  13468 where it did; first callback 28-32 ms after the stream start against
  7 ms) - the 2021 "sync errors". Mapped at open: 8 of 8, 1894 / 2598 / 5780
  (unpatched fork 2655 / 3720 / 5328). Pi 5: plain zero-copy converged 8 of
  8 (1741 / 2088 / 3651; fork 991 / 1404 / 1773).
- Pi 4, four 60 min soaks with retunes, gain and element-count changes and a
  Full Recalibration, zero-copy on vs off, both with the held stream start
  (order on, off, off, on): `coherence_events` 0, no non-routine stderr
  line, 8 of 8 calibration checks OK in all four; readers 12.4-12.5 % vs
  15.6-15.8 % of one core, whole process 31.7 % vs 34.7-34.9 %, VmRSS 95.0
  vs 102.4-103.1 MB. Pi 5, one 600 s soak each (no start-sync): readers 22.6
  vs 30.2 s, process 60.7 vs 69.0 s.
- The default library on the Pi 4 (2026-11-14): heimdall logs "Allocating 32
  zero-copy buffers" per dongle, 6 of 6 starts and a 30 min soak clean, the
  usbfs budget back to 511 x 32 KB after exit; zctest and rtl_test without
  the prealloc call get user-space buffers with the fork's timings.
- "Detected Kernel usbfs mmap() bug" never appeared (Pi OS kernel 6.18).
- Not measured: zero-copy under CPU load on a Pi 4 (it overheats under
  synthetic load), a soak with the held start on a Pi 5, `--wideband`,
  `--kerberos`. Three times a dongle stopped delivering at a stream
  (re)start on the Pi 4 - twice with zero-copy off on the old code, once
  with the default library; only a USB reset of that dongle revived it.
  Unexplained; heimdall does not recover from it.

The fork built without libusb's usbfs zero-copy buffers (`ENABLE_ZEROCOPY
0`): krakenrf/librtlsdr d8ececb (2021-12-13) switched it off with the message
"causes sync errors with rtl-sdr", no cause recorded. Upstream osmocom has
it on (with f68bb2f's check for the kernel usbfs mmap() bug).

Measured on x86_64 Ubuntu (kernel 6.8) with a 5-channel KrakenSDR:
- Byte-exact loss test (`heimdall_v2/tools/zctest.c`, counter test mode,
  heimdall's 32 x 32 KB transfers, idle and under CPU load): no lost,
  corrupted, short or duplicated buffer, zero-copy off or on.
- heimdall start series (cold start, then 5->4->5 element changes): plain
  zero-copy skews the streams' start - cold-start max |lag| 4060 / 6567 /
  10435 samples (min / median / max of the starts) against 675 / 1171 /
  2168 with it off, convergence median 31 s against 25 s. The buffers are
  mapped in `rtlsdr_read_async()`, i.e. after `rtlsdr_reset_buffer()`, and
  that delays each reader's first transfer by a different amount. With a
  simulated slow mapping (100 us per buffer) plain zero-copy failed to
  converge in 4 of 4 starts - 2-3 channels beyond the correlator's
  +-16384 samples (heimdall_v2/docs/dev/calibration.md, *No-peak warning*).
  With the buffers mapped at open (`rtlsdr_prealloc_async()`): 679 / 1159
  / 1705 samples, convergence median 24 s; with that and the slow mapping
  3 of 3 starts converged (max |lag| <= 987).
- CPU: zero-copy saves the copy from the kernel's buffers - reader threads
  37.7 s -> 33.0 s CPU per 600 s, about 1.3 % of one core for the whole
  process.

The change (krakenrf/librtlsdr 701a832 + 3ad1746 on 578b43d; until they were
pushed, 2026-11-25, a patch in `heimdall_v2/tools/` stood in for them): as
first written zero-copy stayed OFF by
default (see the state paragraph above for today's default); in a zero-copy
build the buffers are
mapped once per handle and kept until `rtlsdr_close()` (or a read with
another geometry), and the new `rtlsdr_prealloc_async(dev, buf_num,
buf_len)` maps them ahead of the stream (macro `RTLSDR_HAS_PREALLOC_ASYNC`;
returns 1 = nothing done without zero-copy, so the default build behaves as
before); upstream's usbfs-mmap-bug check (falls back to user-space buffers);
and the stop path waits (<= 5 s) for every cancelled transfer before the
buffers are reused, leaking them rather than freeing a transfer libusb still
owns. heimdall calls `rtlsdr_prealloc_async()` in `init_rtlsdr_device()`
just before `rtlsdr_reset_buffer()`, under `#ifdef RTLSDR_HAS_PREALLOC_ASYNC`
- a fork older than 701a832 (578b43d) lacks it, and heimdall builds against
it unchanged.

The patch set with zero-copy OFF (the default), hardware check 2026-11-13
(heimdall with the patched library and its `rtlsdr_prealloc_async()` call,
same machine): 10 cold starts, each followed by a 5->4->5 element change and
`check_calibration` - cold-start max |lag| 504 / 1009 / 3051 samples (min /
median / max), after the element changes 210 / 669 / 1155; convergence
21.3 / 25.7 / 27.5 s cold, 20.5-27.3 s after the element changes; every
check OK (|lag| 0.0, |phase| 0.2-0.3 deg); `coherence_events` 0. One start
pinned to one core (`taskset`): max |lag| 8973, 33.0 s, element changes
30.5 / 32.3 s. Retunes (100 <-> 433 MHz), gain changes, a Full
Recalibration, wideband-scan and independent mode and back, a calibration
hold: all reconverged, `coherence_events` 0. Plain zero-copy ON (no
prealloc call possible with that library), 4 starts: max |lag| 419 /
4907 / 11593, convergence 24.0-35.8 s, element changes 26.0-32.8 s,
`coherence_events` 0. The earlier OFF baseline: 675 / 1171 / 2168, 23-29 s.

(Written before the Pi runs - their results are in the state paragraph
above.) The check that was planned for a Pi 4 and a Pi 5 (heimdall stopped): `zctest -s 1000,1001,1002,1003,1004 -t
600` against an OFF and an ON (`-DENABLE_ZEROCOPY=1`) library, idle and
under load (`-p`, and `-r 5 -R` for the restart path; build line in
`heimdall_v2/tools/README.md`), then a heimdall start series per library
(at least 10 cold starts with the 5->4->5 element change and
`check_calibration`): first lags, convergence times, `coherence_events`
and `lag_no_peak`, and the reader threads' CPU.

## FIFO hold: starting several streams together (2026-11-13)

Part of the same two commits (krakenrf/librtlsdr 701a832 + 3ad1746, on the
fork's `master`). Pi 4 (2026-11-14, 8 starts each): cold-start
max |first lag| 2416 / 3695 / 8089 before, 332 / 611 / 1382 with the held
start (zero-copy on: 472 / 760 / 1655); release completion spread 270 / 498
/ 577 us; against the unpatched fork (gate + flush) 2062 / 2346 / 3228. Two calls (macro `RTLSDR_HAS_FIFO_HOLD`):
`rtlsdr_set_fifo_hold(dev, 1 / 0)` empties the RTL2832's USB FIFO and keeps
it empty / releases it, and `rtlsdr_release_fifos(devs, n, timeout_ms,
done_us)` waits until each device's `rtlsdr_read_async()` (running in its own thread)
has submitted its transfers, then releases all the held FIFOs with
asynchronous control transfers submitted back to back and waits for them (a
release not confirmed is repeated synchronously, so no FIFO stays held;
`done_us`, if not NULL, gets each release's completion time in us after the
first was submitted - heimdall logs them, "USB streams released together",
with their spread).
`rtlsdr_read_async()` itself only gained the flag that it has submitted.
heimdall uses them on Linux (`HELD_STREAM_START`, heimdall_v2/src/sdr/sdr_init.hpp;
heimdall_v2/docs/dev/calibration.md, *Stream start*).

What the hardware does, measured on x86_64 Ubuntu (kernel 6.8), the
5-channel KrakenSDR behind its one hub, with a scratch tool that starts
the five streams like heimdall (noise source on, 2.4 MSPS, 32 x 32 KB
transfers, SCHED_RR readers) and cross-correlates buffers 12-15 of each
stream against channel 0, paired by count:
- `USB_EPA_CTL` 0x1002 (what `rtlsdr_reset_buffer()` writes first, and the
  value the init leaves) makes the bulk endpoint STALL: a transfer pending
  meanwhile ends with `LIBUSB_TRANSFER_STALL`, which the async callback
  does not resubmit - with the releases one after another every stream was
  dead (0 buffers) in most starts, with each thread releasing its own 3 of
  12 starts lost a dongle. Bit 12 (0x1000) alone does the same; bit 1
  (0x0002) alone empties the FIFO and holds it with the endpoint NAKing:
  0 stalls in every run since. The hold therefore writes 0x0002, and
  `rtlsdr_reset_buffer()` must never be called while transfers are queued.
- Without a hold every stream starts with the FIFO's stale content: in the
  RTL2832's counter test mode the first buffer of every one of 120 stream
  starts held 3248 or 3760 bytes (two values a 512-byte USB packet apart)
  from before, then a gap; held and released, none (20 starts here, and
3510 stream starts in the zctest runs below; zctest's own restart series
without the hold: 397 of 400).
- A synchronous control write takes 1.65-2.0 ms here (one thread, no
  stream running; `rtlsdr_reset_buffer()` = 2 writes, 3.9 ms during
  streaming) - not the ~0.25 ms of a plain high-speed control transfer.
  Five releases written one after another started the streams 7.3 ms
  (median) apart, and five threads each releasing one dongle at the same
  moment 0.13 ms; five asynchronous ones submitted back to back completed
  within 0.03 ms (median, max 0.15) of each other.
- Max |lag| per start (min / median / max samples; 15 starts each,
  stress cases 8-10):

  | start | normal | heimdall pinned to one core | zero-copy, mapping 100 us per buffer, no prealloc |
  |---|---|---|---|
  | as today (each reader submits when it runs) | 220 / 633 / 1948 | 6738 / 7336 / 27248 | 25542 / 41820 / 42371 |
  | readers released together (gate) | 105 / 682 / 1384 | 7314 / 7728 / 8600 | 21000 / 34614 / 42112 |
  | each reader holds, submits, releases its own | 173 / 347 / 995 | 1249 / 7038 / 7649 | 20806 / 42168 / 42621 |
  | held, five threads release together (sync) | 211 / 357 / 962 | - | - |
  | held, `rtlsdr_release_fifos` (async, back to back) | 145 / 286 / 411 | 151 / 252 / 410 | 153 / 290 / 324 |

  Only the coordinated release is independent of when each reader gets to
  run. Its floor, ~120 us (~290 samples), is the timing of the release on
  the device side: the completions come back within 0.03 ms, the stream
  starts do not.
- A FIFO reset during streaming (the alternative: reset all five once they
  run and flush): `rtlsdr_reset_buffer()` on all five every 2 s for 300 s,
  in counter test mode - every stream died at the first reset (pending
  transfers STALLed with 1.5-21 KB in them, nothing more until the stop,
  `rtlsdr_read_async()` then returned -5). With the 0x0002 hold + release
  instead, 148 resets: the streams continue, but each reset loses 1.75-2.24
  ms of samples per dongle - different per dongle by 0.17 ms on average
  (407 samples), at most 0.40 ms (970) - inside a transfer, so it shifts
  each channel's alignment instead of re-anchoring it, and 77 buffers came
  out with more than one discontinuity (the FIFO cut mid-packet). Not used.
- What it predicts elsewhere: on Windows control transfers take 3-4 ms and
  run one at a time for everything behind one root port (*above*), so even
  asynchronous releases would complete ~3.5 ms apart - the five streams
  ~14 ms (~34000 samples) apart, worse than starting them at the first
  transfer as now; heimdall does not use the hold there. The Raspberry Pi's
  control transfers take ~2.4 ms, like this machine's, and run concurrently
  (heimdall's parallel tuning is ~5x faster than one by one there), so the
  release should behave as here - not measured yet.

Library checks (zctest with `-W`: the FIFO held at open and before every
restart, all streams released by one `rtlsdr_release_fifos()`):
zero-copy OFF and ON (`-DENABLE_ZEROCOPY=1`) builds, 300 s each: idle
(1 release) - 0 gaps, 0 corrupt, 0 short buffers, 0 timing steps, 0 startup
gaps on all five; the restart series (`-r 1.5`, same handles, 200 restarts x
5 = 1000 stops per build) and the close / reopen series (`-r 2 -R`, 150 x 5
= 750 stops per build) - the same zeros, every `rtlsdr_release_fifos()` call
found all streams submitted (202 / 203 / 700 / 717 calls, 0 late, 0 errors,
the longest 1.8-9.9 ms), and the zero-copy build never reported a transfer
still in flight at a stop (it prints "transfers did not finish cancelling"
when one is): 3500 stops, no such line, stderr empty.

The stop path in the zero-copy-OFF build: preprocessed with the default
`ENABLE_ZEROCOPY 0`, `rtlsdr_cancel_async`, the async callback,
`rtlsdr_close` and the buffer allocation / freeing are identical to
578b43d's, and `rtlsdr_read_async` differs only by the three stores of the
new "submitted" flag - the map-once patch's waits for cancelled transfers
are all under `ZEROCOPY_BUILD`. A slower stop measured with an OFF build of
the patched library (Raspberry Pi 5: 1.4-1.5 s against 0.4-0.5 s) therefore
does not come from code the patch compiles into the library's stop path
(compared on x86_64; the guards are not architecture-specific).

## The R820T IF filter: what 359a94d changed, and the band-edge phase (measured 2026-11-26)

Fork commit 359a94d (2022-03-18) is titled "disable filters on reg 0A and 0B
as they cause noticable phase distortion, especially at rolloff". Checked on
the x86 dev machine's KrakenSDR (serials 1000-1004), 100 MHz unless stated,
with a test build of the fork that takes the tuner's filter registers from
environment variables (`heimdall_v2/tools/rtl_filter_phase/`). Summary:

- The commit does not switch a filter off. On heimdall's path it changes one
  field, the channel filter's fine bandwidth code (R10 bits 3:0, FILT_CODE)
  from 1111 (narrowest) to 0000 (widest). The filter stays powered; powering
  it down (R10 bit 7 = 0, or the IF filter by R9 bit 7 = 1) leaves no usable
  signal (no calibration at all).
- The distortion is real and it is that filter: with the old value the
  phase between channels reaches 13-26° at -0.4 fs (16-31° measured without
  the noise source's clipping), with the fork's value 1.6-2.2° there. It is
  the low-pass corner, which sits inside the streamed band with code 1111
  and differs from chip to chip by 0.22 MHz (8 %).
- What the fork's setting leaves, 3.4-7.5° at +0.4 fs, is the same filter's
  high-pass corner at the other band edge (-3 dB at IF 0.47-0.51 MHz, which
  is +0.45 to +0.47 fs), which the commit did not touch.
- Stock osmocom rtl-sdr's values for 2.4 MS/s (each chip's own calibration
  code, IF centre 1.815 MHz) leave 0.5-1.5° on this unit, and moving only
  the IF centre to 1.815 MHz 1.1-2.2° - both less than the fork's, from 70
  to 1575 MHz. The IF centre 1.815 MHz was adopted on 2026-11-28 (ff2bb7b),
  after the measurements with the per-bin EQ in *Adopted* below.
- Why the calibration does not remove any of this, and what the per-bin EQ
  does with it: `heimdall_v2/docs/dev/calibration.md`, *Band-edge phase*.

### What the code writes

heimdall never calls `rtlsdr_set_tuner_bandwidth`, so `rtlsdr_set_sample_rate`
passes the sample rate (2.4 MHz) to `r82xx_set_bandwidth()` (`tuner_r82xx.c`),
which picks the `IFi[]` row `{3, 2200+0, 1600, 0, 0x0F, 0x8F, 0x00}`: IF centre
1.6 MHz. The tuner's LO is above the signal, so the IF spectrum is inverted:
baseband +f sits at IF = 1.6 MHz - f (confirmed: a fit over six IF centres
holds to 0.03-0.08 dB this way and 0.4-2.2 dB the other way). heimdall
streams IF 0.4-2.8 MHz; +0.4 fs is IF 0.64 MHz, -0.4 fs is IF 2.56 MHz.

| at 2.4 MS/s | R10 [3:0] FILT_CODE | R10 [4] FILT_Q | R11 | IF centre |
|---|---|---|---|---|
| stock osmocom rtl-sdr (in the fork's history: d892279) | the chip's own calibration result | 0 | 0x8F | 1.815 MHz |
| the fork's base (hayguen's librtlsdr table, ec5ebcc / 22a920d), before 359a94d | 1111 | 1 | 0x8F | 1.6 MHz |
| the fork since 359a94d | 0000 | 1 | 0x8F | 1.6 MHz |
| old-dab/rtlsdr master (read 2026-11-26; not measured) | 9 + (calibration result - 8), row 2410 kHz | as its init leaves it | 0x8E | 1.825 MHz |

- R10 is written with mask 0x0F, so bits 7:4 stay as `r82xx_set_tv_standard`
  left them: 0xD (bit 7 PWD_FILT = 1, channel filter on; PW_FILT 10; FILT_Q
  1). R11 = 0x8F (bits 7:5 FILT_BW = 100, bits 3:0 HP_COR = 1111, the lowest
  high-pass corner) is what the table row holds anyway; R30 bit 6 (filter
  extension) is written 0 by the fork and its base (stock osmocom does not
  write it); R9 is not written. Both drivers that tune the filter per chip
  (stock osmocom, old-dab) also put the IF centre near 1.82 MHz.
- Read back on the hardware (`RTL_R820_PRINT_I2C=1`, all five dongles): R9
  0x40, R10 0xD0, R11 0x8F, R30 0x0E.
- `r82xx_set_tv_standard` runs the chip's filter calibration at every open
  and stores the result (`fil_cal_code`); stock osmocom writes it to
  FILT_CODE, the fork's base and the fork overwrite it. This unit's chips:
  5, 3, 6, 7, 7 (serials 1000-1004; 1000 read 4 in 2 of 47 starts).
- For the other bandwidths (not heimdall's path: a program that asks for a
  narrower tuner bandwidth) 359a94d also replaces every row's R11 by 0x8F
  and keeps the row's IF centre, so the request no longer narrows the filter.

### Method

heimdall (the test build, registers from the environment, otherwise today's
code with the 16-tap trim filter) calibrates as usual; in CONVERGED the
noise source is switched on and 1024-point cross-spectra of every channel
against CH0 are averaged over ~2.5 s of the 8091 stream; then a full
recalibration (`reset_lag_compensation`) and the next round (5 rounds for
the main settings, 3 for sweeps). Reported: the phase of each channel
against CH0 after removing the constant and the slope (a fit weighted by
the cross-spectrum over |f| <= 0.42 fs - what one phasor and one delay per
channel can remove), "detrended" below. The phase as heimdall left it
("raw", referred to the band centre) is 0-3° larger: worst over |f| <= 0.4
fs 3.5-8.6° with the fork's value, 15-29° with the old one, 1.9-2.4° with
stock osmocom's. The trim filter's own response is divided out.
Round-to-round sd <= 0.2°, between heimdall starts 0.2-0.5° at -0.4 fs and
0.9-1.5° at +0.4 fs (7 starts over 13 hours). Only |f| <= 0.4 fs is used:
beyond 0.42 fs the stream holds a term that follows each channel's trim
(-6° per sample of trim at +0.42 fs, -47° at +0.45 fs, <= 1.1° at 0.40 fs;
taken to be aliasing through the RTL2832U's decimation filter - not proven).
Zero coherence events in every run.

### The old value against the fork's (A/B)

Detrended phase against CH0, CH1 / CH2 / CH3 / CH4, two heimdall starts each:

| FILT_CODE | -0.4 fs | +0.4 fs | worst over \|f\| <= 0.4 fs | level difference between channels at -0.4 fs |
|---|---|---|---|---|
| 0000 (fork) | 1.8 / -1.7 / -1.7 / -2.0° | 3.4 / -5.3 / -5.0 / -7.0° | 7.0° (CH4, +0.40 fs) | <= 0.09 dB |
| 0000, second start | 1.9 / -1.6 / -1.7 / -2.0° | 3.6 / -5.2 / -6.4 / -6.6° | 6.5° | |
| 1111 (before 359a94d) | 26.0 / -13.4 / -14.5 / -17.3° | 8.6 / -7.7 / -8.2 / -10.1° | 26.0° (CH1, -0.40 fs) | 3.1 dB |
| 1111, second start | 25.2 / -12.6 / -15.1 / -16.8° | 8.4 / -7.5 / -8.8 / -10.2° | 25.1° | 3.0 dB |

At +-0.2 / 0.3 fs: fork <= 1.6°, old value up to 4.4° (the delay fit tilts
the whole band when one edge bends). The same at RF gain 28.0 and 20.7 and
at IF VGA steps 6 and 4 (the noise source clips 8-26 % of CH0's samples at
every one of them - 14 % with the fork's value, 26 % with the old one).
Without clipping (RF gain 0, 0 % at the rails) the fork's curve is the same
(+0.4 fs: 4.0 / -5.8 / -6.4 / -7.4°), the old value's is larger: 31.0 /
-15.5 / -16.9 / -19.2° at -0.4 fs. The clipped noise source shows a steep
corner about 20 % too small; the same 30-32° appear at 1296 and 1575 MHz,
where the noise source is weaker and clips 0.3-0.6 %.

Over RF (70, 162, 433, 868, 1296, 1575 MHz, 3 rounds each): each setting's
curve differs from its 100 MHz curve by <= 0.9° (fork), <= 0.6° (stock
osmocom's values), <= 0.4° (IF centre moved only) - no more than two
100 MHz measurements of one run differ (0.6°). Over time after ONE
calibration (heimdall started after the tuners had been closed for 10 min,
a capture every 30 s for 20 min, nothing recalibrated): the phase at +-0.3
/ 0.4 fs moved <= 0.44° (fork), <= 0.38° (stock values), <= 0.55° (old
value, 12 min), the band centre <= 0.21°, the lag <= 0.001 sample - about
the noise of one capture (0.06-0.15°); no warm-up to see.

### The cause: the corners of the tuner's IF band-pass

Moving the IF centre (`RTL_R820_IF_CENTER`, 1.0-2.8 MHz in 0.3 MHz steps; the
tuned frequency stays, FM stations keep their place within 2.4 kHz) lays
each channel's response out against absolute IF frequency:

| -3 dB corner (MHz) | CH0 (cal 5) | CH1 (cal 3) | CH2 (cal 6) | CH3 (cal 7) | CH4 (cal 7) |
|---|---|---|---|---|---|
| high-pass (HP_COR 1111), FILT_CODE 0000 | 0.484 | 0.470 | 0.505 | 0.505 | 0.509 |
| low-pass, FILT_CODE 1111 | 2.649 | 2.541 | 2.731 | 2.743 | 2.759 |
| low-pass, FILT_CODE 0000 | > 3.76 | 3.73 | > 3.76 | > 3.76 | > 3.76 |

- The phase between channels is a function of the IF frequency (it follows
  the IF centre to 0.1-0.2°; a curve fixed in baseband misses by 1.2-9.4°):
  with FILT_CODE 0000, 7-11° at IF 0.6 MHz, <= 2.4° at 0.8 MHz, <= 0.5°
  from 1.0 to 2.2 MHz, 1-2° at 2.6 MHz, 3-6° at 3.0 MHz. It is analog, in
  the tuner's IF.
- The level also has a part fixed in baseband and equal in all five chips
  (-0.3 dB at 0.3 fs, -2.1 dB at +-0.4 fs): the RTL2832U's decimation filter.
  It adds no phase between channels.
- Both corners are ordered by the chip's calibration code (lowest code =
  lowest corner): one RC spread per chip, of which the calibration code
  describes the low-pass.
- The high-pass peaks by +1.0-1.6 dB at IF 0.6-0.7 MHz before it falls,
  which hides the decimation filter's roll-off at the + edge (the level at
  +0.4 fs is -0.4 dB against -1.7 dB at -0.4 fs).

One register field at a time (CH1 and CH4 against CH0, detrended):

| changed | - edge (-0.4 fs, IF 2.56 MHz) | + edge (+0.4 fs, IF 0.64 MHz) |
|---|---|---|
| FILT_CODE 2 / 4 / 6 / 8 / 10 / 12 / 14 (0 = fork, 15 = old) | CH1 2.4 / 2.8 / 3.7 / 5.0 / 8.2 / 13.8 / 21.8°, CH4 -2.4 ... -13.0° | unchanged (CH4 -6.4 ... -9.3°) |
| FILT_BW (R11 bits 7:5) 0 / 3 / 5 / 6 / 7 (4 = fork) | CH1 0.5 / 0.5 / 5.1 / 4.8 / 21.2° | unchanged |
| HP_COR 14 / 13 / 12 / 11 (15 = fork) | unchanged (2.1-3.4°) | corner 0.60 / 0.71 / 0.80 / 1.02 MHz (CH0); CH0's level at +0.4 fs -3.4 / -7.4 / -9.0 / -10.5 dB; the worst point moves in to +0.36 / 0.33 / 0.27 / 0.17 fs |
| IF centre 1.9 MHz | 1.0-2.0° | 1.2-1.8° |
| FILT_Q 0; R9 bits 7:6 = 0 / 1; R30 bit 6 = 1 | unchanged | unchanged |
| R10 bit 7 = 0 (filter off); R9 bit 7 = 1 | no calibration: correlation peak / mean 2.0-2.7 against 130-200 | |

So the - edge follows the low-pass setting and nothing else, the + edge
follows the high-pass corner and the IF centre and nothing else.

### Alternatives measured (2026-11-26; the IF centre adopted since - *Adopted* below)

Worst detrended phase over |f| <= 0.4 fs, CH1 / CH2 / CH3 / CH4, 100 MHz:

| setting | worst | note |
|---|---|---|
| fork: code 0000, FILT_Q 1, IF 1.6 MHz | 3.4 / 5.3 / 5.0 / 7.0° | + edge; 6.4-7.4° (worst channel) from 70 to 1575 MHz |
| chip's calibration code, IF 1.6 MHz | 2.1 / 4.2 / 5.2 / 6.0° | - edge <= 1.1°, + edge as the fork |
| code 0000, IF 1.815 MHz | 1.9 / 1.1 / 1.4 / 1.6° | only the IF centre moved; 1.9-2.2° from 70 to 1575 MHz |
| code 0000, FILT_Q 0, IF 1.815 MHz | 1.8 / 1.5 / 1.9 / 2.0° | |
| calibration code, FILT_Q 1, IF 1.815 MHz | 1.2 / 1.4 / 3.5 / 2.7° | - edge worse |
| calibration code, FILT_Q 0, IF 1.815 MHz (stock osmocom) | 0.8 / 0.7 / 1.3 / 0.8° | second start 0.6 / 0.6 / 1.5 / 0.5°; 1.0-1.5° from 70 to 1575 MHz |

With the stock values the noise-off spectrum showed no new features (same
floor, -69 to -70.5 dBFS per bin), the noise source clips 17.8 % of CH0's
samples against 13.8 %, and the level at +0.4 fs is -1.6 dB against -0.4 dB
(the high-pass peak no longer lifts it).

Why moving the IF centre helps: at 1.6 MHz the streamed band (IF 0.4-2.8
MHz) reaches below the high-pass corner (0.47-0.51 MHz); at 1.815 MHz it is
IF 0.615-3.015 MHz, clear of the high-pass by 0.1 MHz, and with code 0000
the low-pass is still 0.7 MHz above it. An IF centre of 1.9 MHz measured
the same (1.5-1.9°); at 2.2 MHz the - edge starts to bend on the chip with
the lowest corner (3.9°), at 2.5 MHz 9°.

Open before anything is changed in the fork:
- One KrakenSDR, one machine, room temperature, 20 minutes. Corners that
  spread 8 % between five chips will spread differently on another unit;
  the calibration codes and the two starts in 47 where one chip's code
  changed (5 -> 4) say the low-pass moves in steps of the code.
- With the IF centre at 1.815 MHz the band's - edge loses the protection the
  high-pass stopband gives against aliasing (the term that follows the trim
  was +17° per sample of trim at -0.45 fs with the stock values against
  +6.5° with the fork's, +114° against +22° at -0.48 fs); inside +-0.4 fs
  nothing was seen. Not tested
  with a strong signal just outside the band.
- Not tested: image rejection (a broadband source cannot show it), the
  wideband variant (tuners at 1268 MHz), a Pi, other sample rates, strong
  off-air signals, temperature.
- A side observation: in one of five runs at 1575 MHz CH1 lost its stream
  pairing on the retune (no correlation peak, peak / mean ~5, one coherence
  event, no lock within 90 s); the fork's own filter setting, not repeated
  in four later runs.

Data: job scratch 14951841 `rtlfilt/w_hw1/` (register A/B, sweeps, IF map;
`analysis/SUMMARY_ALL.txt`, `REGISTERS.txt`, `stitch_fc0.txt`) and
`rtlfilt/w_hw2/` (per-bin EQ, frequency, time; `analysis/D*_COMPACT.txt`).

### Adopted: IF centre 1.815 MHz (ff2bb7b, measured 2026-11-28)

The question (the user, 2026-11-28): the live spectrum showed a distorted top
edge - with the per-bin EQ on by default, is the IF centre still worth moving,
can the low-pass go back to narrow, is there a width in between. Measured
with heimdall's product code (12-tap EQ on by default) built against a copy
of the fork with the register knobs (`r82xx-env-test.patch`, plus an EQ
fit-band knob in that scratch build only), 100 MHz and gain 49.6 unless
stated; then the fork commit itself on both Pis against the fork as it was,
same heimdall source. Phase: worst channel against CH0, mean of two
calibrations per EQ state, "raw" = referred to the band centre as the
stream leaves heimdall, "detrended" = each curve's constant and slope
removed (the slope is the lag servo's residual, up to +-3° at 0.4 fs and
different at every calibration, so raw comparisons between single runs
carry it); 31-bin median against FM stations leaking past the noise source.
Strips: 0.40-0.45 fs on each side.

The change (`src/tuner_r82xx.c`, the 2.2 MHz `IFi[]` row, chosen for
bandwidth requests of ~2.08-2.6 MHz): IF centre 1600 -> 1815 kHz. The
stream then covers IF 0.615-3.015 MHz, clear of the high-pass corner
(0.47-0.51 MHz), the low-pass (FILT_CODE 0, > 3.7 MHz) still above it.

| unit, frequency | detrended in band, EQ off: IF 1.6 -> 1.815 | EQ on | lower strip, EQ on | upper strip, EQ on |
|---|---|---|---|---|
| x86 dev machine, 100 MHz | 7.4 -> 2.3° | 0.6 -> 0.5° | 6.8 -> 4.0° | 4.1 -> 4.8° |
| x86 dev machine, 868 MHz | 7.3 -> 1.8° | 0.7 -> 0.5° | 7.5 -> 3.7° | 5.0 -> 4.7° |
| Pi 4, 100 MHz | 2.2 -> 0.8° | 0.6 -> 0.4° | 5.2 -> 5.0° | 4.5 -> 4.0° |
| Pi 4, 868 MHz | 2.1 -> 0.8° | 0.5 -> 0.4° | 5.6 -> 4.8° | 2.6 -> 4.1° |
| Pi 5, 100 MHz | 1.7 -> 0.9° | 0.5 -> 0.4° | 6.5 -> 5.5° | 4.5 -> 3.0° |
| Pi 5, 868 MHz | 1.9 -> 0.9° | 0.4 -> 0.5° | 6.6 -> 4.8° | 3.1 -> 3.4° |

- The level, what the spectrum shows (CH0 against the band centre, EQ off,
  noise source): at +0.45 / +0.47 fs -5.0 / -7.4 dB -> -3.4 / -4.0 dB
  (x86), -5.9 / -8.1 -> -3.3 / -4.0 (Pi 5), -6.0 / -8.0 -> -3.2 / -3.9
  (Pi 4); the high-pass's +1-1.6 dB peak before it is gone. The top edge
  now rolls off like the bottom one (-4.0 to -4.3 dB at -0.45 fs, the
  RTL2832U's decimation filter). The level spread between the dongles at
  +0.45 fs: 2.5 -> 0.3 dB (x86, 100 MHz), 2.1 -> 1.0 dB (868 MHz); the
  Pis' units matched better already: 0.2-1.2 dB before, 0.3-0.7 dB after.
- Raw, single runs: the x86 unit 7.5 -> 4.3° (100 MHz) and 9.4 -> 3.6°
  (868 MHz) with the EQ off, 1.0 -> 0.8° and 1.2 -> 0.75° with it; on the
  Pi 5 at 868 MHz one run read worse (EQ off 2.3 -> 3.8°, on 0.6 -> 1.3°,
  Check Calibration 0.4-0.5°): a slope of 4° at 0.4 fs in that run's
  calibrations, the same curve without it (table).
- Check Calibration 0.1-0.4° in every run (0.4-0.5° in that one), 0
  coherence events, recalibration times unchanged (full 3.15 s, the EQ on
  / off 0.76-1.28 s on x86).
- In band the IF centre is better or equal on all three units at both
  frequencies, the lower strip better by 0.2-3.8°, the upper strip mixed
  (-1.5 to +1.5°). The strips stay at 3-6° with any setting: past 0.42 fs the noise
  source's own noise folded in by the RTL2832U's decimation sits in the
  measurement (*Method* above), so these numbers say little about a real
  signal there.

Measured on the way (x86 unit, 100 MHz, gain 49.6):

- Stock osmocom's values (each chip's calibration code, FILT_Q 0, IF 1.815
  MHz): raw in band 2.8° EQ off, 1.0° on - no better than the IF centre
  alone once the EQ is on.
- The old narrow low-pass (FILT_CODE 15) with the EQ on: 3.9° in band at
  IF 1.6 (5.0° at 1.815), the channels' levels 4.0 / 5.6 dB apart at -0.40
  / -0.45 fs (the EQ is phase-only), CH0 -8 dB at -0.45 fs. At RF gain 0
  (noise source unclipped) the EQ takes it to 0.5° in band: the clipped
  noise source is what limits it at a working gain. Not usable.
- In-between low-pass codes, IF 1.6, raw in band EQ off / on, lower strip
  EQ on: code 0 7.5 / 1.0°, 7.6°; 4: 9.4 / 1.5°, 6.7°; 6: 7.6 / 1.1°,
  4.7°; 8: 9.0 / 1.6°, 5.6°; 10: 10.6 / 1.8°, 9.0°. What they filter
  beyond the band's lower edge (IF centre moved to 2.6 MHz so the stream
  shows IF 1.4-3.8 MHz; against code 0, mean of the dongles), at 0 / 0.2 /
  0.4 / 0.6 / 0.8 MHz past the edge: code 4 0.0 / -0.1 / -0.5 / -2.2 /
  -5.2 dB; 6: -0.1 / -0.4 / -2.2 / -5.9 / -7.9; 8: -0.4 / -1.6 / -5.3 /
  -8.9 / -7.7; 10: -1.0 / -4.4 / -8.7 / -9.7 / -9.0 - the corners' spread
  between dongles 5-8 dB near each corner. Lower side only (the upper side
  is the high-pass). Code 6 costs ~0.1° in band with the EQ at IF 1.6; at
  IF 1.815 the band's lower edge sits 0.2 MHz closer to the corner (not
  measured). Not adopted: at most 8-10 dB, one side.
- The EQ fitted to +-0.45 fs instead of +-0.42 (scratch knob): strips 3.1 /
  3.4° (both IF centres) for +0.3° in band - measured on the noise source,
  whose strips carry the folded noise; not adopted without a real signal.
- Not measured: image rejection, and the calibration as a real signal sees
  it across the band - the bench's FM stations were too weak and not
  coherent enough between antennas (per-bin coherence 0.15-0.55, phases
  moving 3-14° between tunings where the noise source showed < 1°); that
  needs a CW generator through a splitter. Also not tested: the Wideband
  variant (tuners at 1268 MHz), other KrakenSDR units than these three,
  KerberosSDR, other sample rates in the row's range (2.08-2.6 MS/s),
  Windows and macOS (the same table).

Data: job scratch 14951841 `rtlfilt2/` (`ifm.py`, `campaign*.sh`,
`analyze2.py`, `detrend_cmp.py`, `oob.py` / `oob_analyze.py`,
`stationtest.py`, `out/`), `pi/res_if_pi4/`, `pi/res_if_pi5/`
(`ifm_pi.py`, `run_if.sh`).
