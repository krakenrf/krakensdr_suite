# librtlsdr fork: WinUSB fixes, fast retunes, measurements

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

heimdall links krakenrf/librtlsdr statically and requires 578b43d or later
on every platform. The two notes below were bullets of the Windows section.

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
  `LIBRTLSDR_COMMIT` = 578b43d; install.sh clones master (a fresh install)
  or `git pull --ff-only`s an existing clone (if that fails it warns and
  builds the old checkout - heimdall's make then updates or stops). Linux:
  the Makefile's update rebuilds only the static library heimdall links -
  the `rtl_*` tools in /usr/local stay until install.sh runs again. The
  library is rebuilt / checked against the fork's sources on every platform
  (`RTLSDR_SRCS` + touch in the Makefile, `IS_NEWER_THAN` in CMakeLists.txt:
  a new header with an old library would fail the link). heimdall opens /
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
  require it (`RTLSDR_FIX_MARK` = `rtlsdr_finish_tune`, see above), the
  Dockerfile pins it.
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
