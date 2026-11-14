# Drag retunes (fast path) and failed-retune replies

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

**Drag retunes (fast path, `update_sdr_settings` in sdr_init.cpp)**: a
coherent retune within `FAST_TUNE_SETTLE_MS` (400 ms) of the previous one, or
before the previous step's tuners were finished (at most
`FAST_TUNE_DRAG_WINDOW_MS`, 1 s), tunes with the fork's
`rtlsdr_set_center_freq_fast(RTLSDR_TUNE_KEEP_AUTOTUNE)`: the R820T's PLL
autotune clock stays at 128 kHz between steps (the full path writes 128 kHz
before N / SDM and 8 kHz after the lock: 2 control transfers per tuner and
step); the lock is still read on every step. Only when `fast_retune_allowed()`
(coherent, not `--kerberos` manual cal, no discrete scan, no recovery, no
downconverter); decided and tuned in one `device_io_mutex` hold; the rollback
of a failed step is always the full path. The WIDEBAND SCAN's retunes
(`setup_wideband_frequencies`: every tuner to its own spread frequency) take
it the same way (2026-10-21, the user's "dragging in wideband mode is
laggy"): `drag_step()` = the one decision for both modes, sharing
`last_drag_retune_ns` (renamed from last_coherent_retune_ns - every retune
of either mode is the drag's latest, so the watchdog's finish never runs
inside a wideband drag) and `fast_tune_pending`; `fast_wideband_allowed()`
(wideband scan, no discrete scan - its hops are seconds apart, flushed and
settled -, no recovery, no downconverter); a failed wideband drag step goes
once more on the full path (logged); `finish_fast_retunes` retunes a tuner
it finds unlocked at its wideband scan frequency when the library lost it.
Measured on the KrakenSDR (Windows, 5 tuners, 8092 `set_frequency` reply,
steps 50 ms apart; job scratch 0d0a7cca hw_*): 114.0 -> 74.0 ms median per
step (max 123 -> 83) for 25 / 100 / 250 kHz steps in the FM band, a drag
across the 109.5 MHz divider boundary and 100 kHz steps at 595 MHz; the
first step after a pause and typed jumps stay on the full path (153 / 194-216
ms both); after every drag all five tuners on their frequencies (status +
8091 headers), neighbours' spectra lined up bin for bin in their overlaps
(lag 0 wherever the overlap held a signal, identical to the old build), 0
coherence events, no failed step, no unlocked tuner; 199 of 204 steps on the
fast path. Mode switches straight after a drag (tuners pending): wideband ->
coherent recalibrated and CONVERGED in 25 s, coherent drag -> wideband (the
spread itself a drag step, 83 ms) and wideband -> independent -> wideband
with the tuners right and aligned. The Pi (parallel threads) not measured `finish_fast_retunes()`
(`rtlsdr_finish_tune`: lock read, then 8 kHz) puts the tuners back in the
full path's resting state - from the coherence watchdog once 400 ms have
passed without a retune, and from `set_bias_tee_all_devices(true)` in any
case. INVARIANT: every calibration switches the noise source on through
`set_bias_tee_all_devices(true)`, so it never runs on tuners still in the
drag state - a new calibration entry point that skips that call (noise
already on) must call `finish_fast_retunes()` itself. A tuner the finish
finds unlocked is retuned on the full path (twice), else
`signal_coherence_lost("PLL not locked after a drag")`. Closing the handles (element-count change, reopen) drops the
pending state. Windows tunes the dongles one by one, Linux in parallel threads
(`for_each_tuner`; each thread only its own handle and result slot, the
atomics written only under `device_io_mutex`), both with the fast path. A
step after the finish (0.4-1 s stepping) goes back to the full path: the
finish + the next 128 kHz write would cost more than it saves.
`RTLSDR_TUNE_SPAN_AUTOTUNE` (128 kHz inside the N / SDM write) is NOT
passed: it saved only a drag's first step (72.7 -> 61.7 ms, measured with an
earlier library), later steps cost the same. Measured 2026-10-19 (5
streaming tuners, 40 steps of 25 kHz 50 ms apart, 8092 `set_frequency`
reply; library details docs/dev/librtlsdr-fork.md):
- Windows (one by one): first step 112.8 ms (full path, 5 transfers per
  tuner), step 2 writes 128 kHz once (4), later steps 74.0 ms median (3: `R5`
  register 4, `W15+2` N / SDM, `R3` lock; fast path off 114.0 ms, 5); the
  watchdog's finish (`R3` lock read, `W1a` 8 kHz, 2 per tuner) 0.41-0.45 s
  after the last step; a recalibration started mid-drag finished the tuners
  0.11-0.14 s after the last step, before the noise-source GPO write; steps
  during a recovery took the full path and left nothing pending; typed jumps
  7-8 transfers per tuner (full path); a gain change 7, no PLL write; 0
  coherence events. With the 5000 us PLL settle wait (Pi numbers below
  too): later steps 73.9 ms, typed jumps +12-13 ms (5 tuners x ~3 ms at
  divider crossings)
- Raspberry Pi 5 (parallel threads; transfers per dongle incl. the dummy
  read and the noise-source GPIO read + write; 3 runs): first step 18.5-19.5
  ms (8), step 2 7, later steps 14.3-14.5 ms median (6; fast path off 19.0
  ms, 8; 00606c0 48.1 ms, 20); the tuners are done 10.1 ms into a step (fast
  off 14.8, 00606c0 43.9). begin_retune_cooldown re-asserts noise-off on
  every retune (its skip when already off is `#ifdef _WIN32` only), and
  set_bias_tee_all_devices switches the dongles through for_each_tuner
  (results logged after the loop; ext_noise / wideband / kerberos switches
  stay sequential after it): the 5 dongles' `Sr3001` / `S3001` take 3.8 ms,
  9 of 10 transfers overlapping (one after another: 18.3 ms, the step 28.7
  ms - fast off 33.4, 00606c0 62.5). Jumps 19-31 ms (were 40-42), gain
  changes 28 ms (43). The finish (lock read, 8 kHz) 0.41-0.46 s after the
  last step; a recalibration started mid-drag (3 of 3 runs) finished every
  tuner 2.0-2.2 ms before the first noise-on GPO write (+10.8 to +36 ms after
  the last step: the noise-on hook, not the watchdog), the 5 noise-on
  writes within 0.3 ms; steps during the recovery took the full path (15-16
  ms); phases reconverged in 2.6-3.6 s; 0 coherence events
- Pi, FIXED (Nagle): every ~10th step used to reply ~12 ms later with the
  same USB span - a 2 Hz status broadcast fell inside each slow step and
  the reply came 40.1-47.0 ms after it: Nagle + Linux's 40 ms delayed ACK,
  as TCP_NODELAY on 8092 was set only on Windows; TCP_QUICKACK on the
  client removed it. heimdall now sets TCP_NODELAY on every platform
  (tcp_control_server.cpp; 8091 / 1234 stream bulk samples and keep
  Nagle). Measured 2026-10-20, no TCP_QUICKACK on the client: 0 late
  replies in 3 fast-path runs (4 of each 40-step drag's steps had a
  broadcast inside; steps 2+ 14.2 ms median, p90 14.6-14.8, max 16.7-16.8
  - once 39.0 ms with no broadcast inside: heimdall idle 25 ms between
  the tuner writes and the noise-GPIO read, cause unknown) and the fast
  path off run (18.9 ms median, max 21.6), 0 also in the mid-drag
  recalibration drags (were 2 per drag); a QUICKACK drag is now the same
  as a plain one. 00606c0's 48 ms step outlasts the 40 ms, so the test
  can't tell there
- Pi, once in 5 runs (2026-10-19): a 599 ms gap inside a step (all 5
  threads; cause unproven, likely the test's USB logging shim) made the
  next I2C write fail with PIPE on every dongle; the full-path rollback
  restored the previous settings without a coherence loss, but the reply
  said `{"status":"success","frequency":<new>}`. Both FIXED: the library's
  stall retry now runs on Linux too and recovered every such write (see
  docs/dev/librtlsdr-fork.md), and a change that still fails replies an error with
  the PREVIOUS value (`SdrUpdateResult`, the optional out-parameter of
  update_sdr_settings; its bool still means "hardware written,
  recalibrate"): set_frequency / set_gain answer
  `{"status":"error","message":"Retune failed on channel(s) X - previous
  settings restored","frequency":<previous Hz>,"failed_channels":[X]}`
  (gain: `"Gain change failed ..."`, `"gain":<previous dB>`), also for a
  failed restore (`"... and restoring the previous settings failed on
  channel(s) X - tuners inconsistent, coherence recovery forced"`), a
  failed Wideband LO retune and an independent-mode tuner that failed (the
  others took it). Measured 2026-10-20 with injected tuner-write failures
  (LIBUSB_ERROR_IO, never retried; Windows also drop / land / 4 PIPE
  stalls in a row) on Windows and the Pi 5: set_frequency, set_gain and a
  drag step replied the error with the previous frequency / gain (a drag
  step: the step before's; the next steps went through), every tuner's
  registers afterwards equal to those before the command (a drag step:
  only 0x1a differs, the full-path rollback's 8 kHz), all locked; cooldown
  + recalibration, reconverged in 3.9-4.1 s (Windows drag: 2.1 s after
  its end), 0 coherence events; a failed
  restore: coherence_events 0 -> 1, recovered in 11.5 s (Windows) / 13.3
  s (Pi); heimdall kept running. Windows replies 193-330 ms (normal drag
  steps 74). A single PIPE stall was retried by the library and replied
  success. Not tested: independent mode, the Wideband LO failure, the web
  UI path, a failed restore that leaves the tuners really on different
  frequencies (the forced recovery flushes and recalibrates, it doesn't
  retune - code reading). Every reply consumer only logs an error
  (kraken_doa discards replies and follows the 8091 header; the GR source
  logs a warning). The web UI has no error display; its "Current:" values
  come from current_frequency / current_gain

Needs a fork with the call (`#ifdef RTLSDR_TUNE_KEEP_AUTOTUNE`: the code
still compiles the full path only against an older one, but the Makefile and
CMake now require krakenrf/librtlsdr 578b43d - `RTLSDR_FIX_MARK`, every
platform, the Dockerfile pins it); they also rebuild / check
`../librtlsdr/build/src/librtlsdr.a` against the fork's sources on every
platform, since a new header with an old library would fail the link.

