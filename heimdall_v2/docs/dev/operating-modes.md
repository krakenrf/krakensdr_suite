# Operating modes (coherent / wideband scan / independent / sweep)

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

### Operating Modes (coherent / wideband scan / independent)

`OperatingMode` (types.hpp): COHERENT, WIDEBAND_SCAN (tuners spread side by
side, `setup_wideband_frequencies`) and INDEPENDENT (every tuner its own
receiver). `set_operating_mode()` (sdr_init.cpp) is the one switch; 8092
`set_operating_mode {"mode":...}` / the old `set_wideband_mode`:
- Leaving COHERENT parks the calibration (`park_calibration_locked`: phase
  machine parked, per-bin EQ dropped, noise source off, FFT/correlation off).
  Every "calibration stands down" check is `operating_mode != COHERENT`
  (phase/lag stages, retune cooldown, `handle_settings_change`, flush-only
  recovery, periodic monitor, element-count changes refused)
- INDEPENDENT: no compensation applied in the conversion
  (`snapshot_conversion_state` -> identity); per tuner `set_independent_tuner`
  (frequency and/or gain; `wideband_config.tuner_frequencies` /
  `tuner_gains`); `set_frequency` / `set_gain` / `SDR_SETTINGS` set EVERY tuner
  (no-op shortcut and rollback skipped in `update_sdr_settings`); the discrete
  scanner and the web UI's noise-source switch are refused
- Returning to COHERENT (`return_to_coherent_locked`): every tuner back on
  `current_frequency` and the common gain, then the phase recalibration (or
  the KerberosSDR manual-calibration rules)
- 8091: per-channel frequency outside coherent, per-channel gain in
  independent; phase-state flags 0x400 (wideband) / 0x800 (independent). Status
  JSON: `operating_mode`, `tuner_frequencies`, `tuner_gains`; web STATE:
  `operating_mode` (index.html shows a banner)
- Persisted (`operating_mode`, `tuner_freq<N>`, `tuner_gain<N>` in
  heimdall_settings.conf via `settings::note_mode`) and re-entered at startup
  before the pipeline threads start (`restore_operating_mode`)
- Unavailable in the downconverter variant (tuners parked at the IF)

### Generic RTL-SDR mode (no KrakenSDR attached; added 2026-11-16)

Ordinary RTL-SDR dongles (1 to `NUM_DEVICES`, any serials - often all
"00000001", sometimes unreadable) run as independent receivers; coherent mode
is unavailable. `generic_rtlsdr_mode` (core/config.hpp, `generic_rtlsdr()`),
decided once in main() before the dongles open, never changed (no hotplug).
- Chosen when ALL hold: no `--serials`, `--kerberos(_sw)`, `--wideband`,
  `--ext_noise`; `count_expected_devices_present()` < 2; fewer than 2 attached
  dongles carry an expected serial (anywhere in the list) and not 1 of them
  plus a dongle whose serial can't be read (`survey_rtlsdr_usb`) - a KrakenSDR
  with a dead or busy channel, or without the WinUSB driver on some dongles,
  keeps the old failure; at least one RTL-SDR on USB. Otherwise exactly as
  before. `-n 1` is accepted only here (`-n` caps the count opened).
- Opening (`init_generic_rtlsdr_devices`, sdr_init.cpp): by USB index, one by
  one, up to `-n` / `NUM_DEVICES`; a dongle that fails to open is skipped
  (stderr) and the channels stay contiguous. `devices` gets one SDRDevice per
  dongle OPENED, `expected_serials` their (display-sanitised) serials and
  `active_num_elements` N, so `max_elements == N`. Nothing reopens by serial:
  `open_active_devices()` refuses, the element-count change is refused, and
  Windows' stream-loss reopen uses the serial only if it is unique among the
  opened dongles, else the same USB index. index.html is reloaded after the
  open so `{{SERIALS}}` lists the generic dongles.
- No GPIO is ever written (GPIO0 is the ANTENNA bias tee on an RTL-SDR Blog
  dongle): `init_rtlsdr_device()` skips the noise-source call,
  `set_bias_tee_all_devices()` returns at once (`bias_tee_enabled` false
  from detection on), `apply_antenna_bias_tees()` refuses, and the Windows
  reopen skips its GPIO block. The persisted `antenna_bias_tee_mask` is the
  KrakenSDR's and stays as saved.
- No calibration: `enter_generic_independent_mode()` (main, right after the
  open, before any thread starts) switches to INDEPENDENT, so every "stands
  down" check applies; `recover_coherence` is flush-only and the periodic
  monitor skips (both also check `generic_rtlsdr()`). Refused with a clear
  error: coherent mode (`generic_mode_refusal()` in `set_operating_mode`,
  every entry point; 8092 replies its text), wideband scan with N = 1,
  `check_calibration` / `reset_lag_compensation` / `set_antenna_bias_tees`
  (8092; the status says `antenna_bias_tees_available:false`) and CHECK_CAL /
  FORCE_RECAL / RESET_LAG_COMPENSATION / ANT_BIAS_MASK (web), the
  element-count change. Wideband scan with N >= 2 works; leaving it
  (`set_wideband_mode(false)`) lands in INDEPENDENT and stops the discrete
  scanner.
- Tuners: `set_fixed_if_vga` returns success for a tuner without the R820T IF
  VGA (E4000, FC001x, FC2580); dithering already returns 1 there.
- Persistence: `note_mode()` never writes `operating_mode` or the KrakenSDR's
  `tuner_*` keys in generic mode; independent mode's tuning goes to
  `generic_tuner_freq<N>` / `generic_tuner_gain<N>` and is restored at the
  next generic start (snapshotted before the mode switch, whose note_mode
  would overwrite it). `center_freq` / `gain` are shared as before, and
  `num_elements` is never written.
- Reported: 8091 phase-state flag `0x1000` (with `0x800` / `0x400`), the 8092
  status and web STATE `generic_rtlsdr`, the startup banner, the dashboard
  (HARDWARE line) and the web page's banner, which also hides the element
  count, calibration, noise-source and bias-tee controls.
- N = 1: no compensation thread (none per non-reference channel),
  `process_correlations` returns the set to the pool (it leaked it when the
  page's FFT graphs were on), the correlation message has 0 pairs.
- Fixed 2026-11-16: the KrakenSDR's `restore_operating_mode()` now also
  snapshots `persisted_tuner_freq/gain` before the mode switch (it read them
  after `note_mode()` had overwritten them with the common tuning, so
  independent mode's per-tuner tuning was lost on a restart).

### Sweep mode (added 2026-11-20)

The user's request: "a "sweep" mode which allows us to use all available
dongles, or just one or some to do a wideband sweep across a swath of
spectrum, creating a wideband FFT and waterfall ... not live, and does not
allow for demodulation". The interface between heimdall, kraken_doa and the
page is fixed in `kraken_doa_v2/docs/dev/sweep-mode.md` ("The contract");
this is heimdall's side. Code: `src/sdr/sweep.cpp` (+ `sweep.hpp`), the
plan and live state `SweepPlan` / `SweepState sweep_state` (`types.hpp`),
constants `SWEEP_*` (`config.h`).

**The mode.** `OperatingMode::SWEEP` (3, `"sweep"`), entered and left only
through `set_operating_mode()` like the others. Every `!= COHERENT` check
already stands calibration down (`park_calibration_locked` on entry from
coherent: noise source off, FFT off, phase machine parked). Decisions at the
`== INDEPENDENT` / `== WIDEBAND_SCAN` uses:
- conversion: identity compensation, like independent
  (`snapshot_conversion_state`) - the tuners are on different frequencies;
- 8091: per-channel frequency from `wideband_config.tuner_frequencies`
  (where each tuner is), per-channel gain = the common gain, flags `0x800 |
  0x2000`, the three scanner fields from `sweep_state`;
- `set_gain` / `SDR_SETTINGS` gain: every tuner (`update_sdr_settings`, the
  common gain; no rollback difference); a frequency there is refused
  (`SdrUpdateResult::sweep_refused`, logged at most every 10 s); 8092
  `set_frequency` is refused at the command, `set_independent_tuner` by its
  own mode check, `start_scanner` and the element-count change (precheck)
  too; entering sweep is refused while the discrete scanner runs and on the
  Wideband downconverter variant (the existing `target != COHERENT` checks);
- generic RTL-SDR mode: allowed, also with one dongle (`generic_mode_refusal`
  returns nullptr; its log throttle array grew to 4 modes);
- leaving: to coherent `return_to_coherent_locked` (every tuner back on
  `current_frequency`, which the sweep never changes, then the phase
  recalibration; KerberosSDR: its manual rules); to independent the tuners
  stay where the sweep left them (their `tuner_frequencies` are exact: a
  tuner's entry is written only after it took the frequency); to wideband
  `setup_wideband_frequencies` as usual. Entering from independent puts every
  tuner back on the common gain (`restore_common_gain_locked`).
- The 8070 page's banner says "Sweep mode · no calibration · <range> · <n>
  tuners · <round> ms a round" (STATE.sweep), and its frequency box / Set are
  disabled in sweep mode; the terminal dashboard has a SWEEP line.

**The plan.** `set_sweep` (8092; `apply_sweep_request`): `first_center_hz`,
`step_hz`, `steps` (together), `tuners`, `dwell_ms`, `settle_ms`, `run`.
Accepted in every mode, used in sweep mode; every field is checked before
anything changes. A plan or tuner change bumps `plan_generation` and resets
`round` / `sweeps`; dwell, settle and run changes take effect at once
without a restart (the thread's waits re-read the plan on every wake,
`change_seq` + a condition variable). Entering the mode sets `run` (the
client's "entering the mode runs") and starts at round 0. The default plan
(before any `set_sweep`) is 88-108 MHz on every tuner. The tuner mask keeps
the user's choice; bits of tuners that aren't open are ignored, none left =
every open tuner (`sweep_tuners`). Saved in `heimdall_settings.conf`:
`operating_mode=sweep` (note_mode, not in generic mode - its rule),
`sweep_first_center_hz` / `sweep_step_hz` / `sweep_steps` / `sweep_tuners`,
and `sweep_dwell_ms` / `sweep_settle_ms` only when not the default (so a
better default reaches existing installs); `run` is not saved. Loaded
before the dongles open (an invalid plan falls back to the default, logged);
`restore_operating_mode` re-enters sweep mode and the thread starts hopping
once the pipeline runs.

**Who measures what** (the contract): K' = min(K, H) of the selected tuners
(lowest indices) take contiguous blocks `[k H / K', (k + 1) H / K')`; round r
moves each to `base + r` if its block has one. Blocks, not interleaved: the
tuners' noise floors differ by a few dB, which then shows as K' - 1 steps in
the stitched spectrum instead of a comb. A tuner already on its hop is not
retuned (K' = H: every round is the same, so the stream runs on without a
gap and a one-hop "sweep" is live); one whose last retune failed is, every
round (its header frequency still shows the old one).

**The hop** (`sweep_thread`, one thread, started in main):
1. `retuning_in_progress` = 1 (only when a tuner moves);
2. downstream flush (`clear_l2_buffer_quiet`: what is queued is the old hop's
   and would go out flagged anyway);
3. under `settings_mutex` + `device_io_mutex`, with the mode, the plan
   generation and `run` re-checked: `rtlsdr_set_center_freq` (the FULL path,
   not the drag fast path) on the round's tuners through `for_each_tuner`
   (parallel threads on Linux, one by one on Windows; one retry each), then
   their new frequencies into `tuner_frequencies` (only the ones that took
   it), `last_retune_ns`, `round`, `frequency_change_counter` + 1;
4. the settle (`settle_ms` from `last_retune_ns`);
5. the gate (`sweep_gate_l1`, served by the drain; since 2026-11-21 - until
   then the drain's quiet-point L1 flush): every dongle drops the packets
   that can hold a sample from before the settle's end, judged by its
   earliest-arrival line, each on its own; then the downstream flush (*Hop
   timing*, *Review of the hop loop*);
6. `retuning_in_progress` = 0, under `settings_mutex` and only if nothing
   moved `last_retune_ns` since the settle (a gain change: settle + gate
   again);
7. the dwell: `sweep_hop_packets(dwell_ms)` sets out on 8091 as valid
   (`sweep_state.valid_sets`, counted by the conversion worker when the flag
   was 0 before and after the header was built; at most
   `SWEEP_DWELL_GRACE_MS` 250 ms longer than they take), then at once the
   next hop (*Hop timing*; until 2026-11-21 `dwell_ms` of wall time and at
   least one valid set).
`round_ms` = 1 to 7 measured. Leaving the mode or a plan change mid-hop: the
waits end (`sweep_notify`), the retune re-checks under the lock, so the new
state is never retuned over and no tuner is left half way (a retune in
progress finishes first: `set_operating_mode` waits for `settings_mutex`).
Pausing or the pipeline stopping with the flag on: the thread finishes the
settle from `last_retune_ns`, flushes and clears the flag
(`settle_and_clear_flag`), so the flag is 0 whenever nothing hops and every
dongle streams - a stuck 1 would make every client drop every packet. A
dongle that delivers nothing keeps it at 1 (no set can be formed then
anyway): status `stalled_tuners`, a log line every 10 s, the gate retried
until it streams again (*Review of the hop loop*, 2). A gain change in sweep mode
sets the flag too (`sweep_note_tuner_change`, before the tuners are
written, under `settings_mutex`). No output per hop: the flushes are the
quiet variants (no "Clearing ... buffers" lines; the drain's "did not
respond" throttled), failed retunes are counted (`failed_retunes`) and
logged at most every 10 s; nothing in the hop calls `signal_coherence_lost`,
so the sweep never counts as a coherence loss or starts a recovery (the one
exception: leaving for coherent when the re-pairing flush finds no quiet
point - *Review fixes*, 4). The realtime
paths (callback, drain) are unchanged.

**The guarantee** (the client builds on it): a packet with
`retuning_in_progress` = 0 holds only samples taken at the header's
per-channel frequencies, after the settle. From the code:
- the flag is stored (seq_cst) before any tuner moves; a tuner's new
  frequency is stored (seq_cst) only after it took it; the header
  (`tcp_data_server.cpp`) now reads the per-channel frequencies FIRST and
  the flag after them - so a header holding a new frequency reads the flag
  set by that hop (or a later clear, next point);
- the header is built while the conversion worker holds `l2_flush_mutex`,
  with the set's flush epoch unchanged since before the set was dequeued;
  the gate's downstream flush (`flush_downstream`) takes the same mutex and
  bumps the epoch, and the flag is cleared only after it. A header that
  reads the flag as 0 after hop h's clear was therefore built after hop h's
  gate, its set was dequeued and collected after it (the drain drops its
  partial set when it serves a gate), and each of its packets is one the
  gate kept for its dongle: numbered after the first packet s whose
  earliest-arrival line f(s) reached the settle's end T. Its frequencies
  are hop h's (h + 1's are stored after h + 1 set the flag);
- the bound (proof in `serve_gate_now`; *Review of the hop loop*, 1, and
  its third round): every sample of a kept packet was taken at or after
  **retune end + settle - L* - 33 us**, where L* is the fastest USB
  delivery among the dongle's last RTL_USB_BUF_COUNT + 16 (48) packets.
  That window outlasts any backlog a stalled reader can deliver (at most
  RTL_USB_BUF_COUNT packets: the transfers that completed while it stood),
  so it always holds packets a running reader delivered and L* is the USB
  transfer's floor (~1 ms on this PC); a late straddler, a burst, samples
  the RTL2832 lost or a packet the callback dropped don't move it. What
  the bound does NOT cover: a reader that runs steadily behind (every
  packet of the window late - a host loaded for longer than the window,
  ~330 ms): then L* is that lateness. So **the settle has to cover the
  host's USB latency** - the default 5 ms leaves ~4 ms of margin on the
  PC; neither a Raspberry Pi nor a loaded reader is measured. Measured
  below and in *Hop timing*, through the real pipeline. (A Pi 5 since:
  *On the Raspberry Pi 5* - its packets arrive ~0.7 ms later than the PC's,
  and the default is 2 ms now.)
One exception, negligible: the lag FIR's 7-sample tail (`frac_tail`, kept
across sets for the coherent conversion) is not reset by a flush, so the
first 3-7 output samples of each channel in a hop's first valid packet are
a weighted sum reaching into the last pre-flush set (a fraction of a
sample's energy, out of 16384).
The reordering of the header's reads is invisible in coherent mode (the
same values; the discrete scanner's hops are ordered the same way and gain
the same property).

**Coherent mode** is unchanged except: the header's reads reordered (above),
the conversion worker's per-set mode load, `for_each_tuner` moved to
`for_each_tuner.hpp` (the same code), the status JSON / STATE carry a
`sweep` object, and the element-count refusal text names sweep mode.

**Measurements** (2026-11-20, the KrakenSDR, 5 tuners, x86_64 Linux PC,
parallel tuning; job scratch 52196024 `heimdall/`):
- The settle (`t_contrast.py`): one tuner alternating between hop A (a
  station at baseband +0.6 MHz, 16 dB over B's floor there at 49.6 dB gain;
  92.0 / 94.0 MHz) and hop B; in B's packets the narrow band at +0.6 MHz,
  per 1024-sample segment (0.43 ms). Positive control: of the packets sent
  with the flag set and B's frequency already in the header, 68-78 per 10 s
  had segments > +10 dB (worst +17 dB: A's samples really are in the stream
  while the flag is 1). The FIRST valid packet of each B dwell, worst
  segment over 71-81 dwells: +3.3 / +3.1 / +3.7 / +3.6 dB at settle 0 / 2 /
  5 / 10 ms, against +3.2 ... +5.6 dB max for the later packets of the same
  dwells (segment 0 median +0.1 dB): no trace of A at any settle. A large
  jump (29 hops 24.8 ... 92.0 MHz, the wrap 92.0 -> 24.8 MHz, station 24 dB
  over the floor, `--steps 29 --step 2.4e6`): first valid packets +3.1 /
  +2.5 dB worst at settle 0 / 5, later packets max +3.9 / +3.3; flagged
  packets up to +15.7 dB. The FM band with every tuner (`t_settle.py`, 11
  hops, dwell 100 ms, ~300 tuner-dwells per settle 0 / 2 / 5 / 10 / 20 ms,
  at 29.7 and 49.6 dB): the first valid packet's per-segment power, shape
  and worst band matched the later packets of the same dwell at every
  settle (e.g. 49.6 dB, settle 0: worst-segment |dP| median 0.77 / later
  0.76 dB, dS 2.39 / 2.32 dB). So settle 0 already holds here: the tune
  call returns with the PLL locked, and the post-settle flush (served at the
  first packet round after the request, which it drops) is itself a guard of
  about a packet. `SWEEP_SETTLE_MS` = 5 ms is that plus margin for slower
  USB hosts (the Raspberry Pi is NOT measured; re-run `t_contrast.py` there).
  (A Pi 5 since: *On the Raspberry Pi 5* - `t_contrast.py` clean at settle
  0 / 2 / 5 with carriers 31 / 36 dB strong.)
- Speed (`t_speed.py`, ms per round from the 8091 counter, 4 s each): the
  hop costs ~25-30 ms beyond the dwell (the retune - not timed alone -, the
  flush waits for the next packet round, the first valid set comes a packet
  later). dwell 20 / settle 5: 42-49 ms a round for K = 1-5,
  1.3-1.6 valid packets a hop; dwell 5 / settle 0: 34-39 ms, 1.0 valid
  packets a hop (the "at least one valid set" rule - without it, measured on
  the fake librtlsdr below, a 20 ms dwell had 1 hop in ~75 without data and
  a 5 ms one most); dwell 50: 75-80 ms, 5.9 packets; dwell 100: 121-125 ms,
  12.3 packets. 88-108 MHz (11 hops): 2.1 / 3.9 / 5.4 / 7.6 / 6.8 sweeps a
  second for K = 1-5 at dwell 20 / settle 5 (2.6 / 4.9 / 6.6 / 9.8 / 8.5 at
  dwell 5 / settle 0). 24-1765 MHz (911 hops): 45 ms a round at every K ->
  40.9 / 20.7 / 13.7 / 10.3 / 8.4 s a sweep for K = 1-5 (dwell 20 / settle
  5; timed directly: K = 5 7.73 s, K = 1 40.96 s), 31.9 ... 6.5 s at dwell 5
  / settle 0. More tuners = faster as asked, nearly in proportion (K = 5 is
  a little slower a round than K = 4 at the FM band: five parallel tunes).
  Windows tunes one by one (~3-4 ms a USB control transfer): expect a hop
  ~5x the Linux retune there (not measured).
- Without the "at least one valid set" rule, a dwell's first valid packet
  reaches 8091 about a packet after the flush, so a 20 ms dwell yielded 0-2
  (one hop in ~75 none, on the fake librtlsdr) and a 5 ms dwell mostly none.
- CPU (this PC, the 8091 reader on headers only, 30 s each): independent
  mode 11.1 %, sweep mode (24-1765 MHz, 5 tuners) 15.0 %. Memory flat: RSS
  43.7 MB from 60 s to 600 s of a full-range sweep.

**Tests** (2026-11-20, all on the KrakenSDR unless marked fake; scripts and
logs in job scratch 52196024 `heimdall/`, `README.txt` there):
- `t_basic.py`: coherent start CONVERGED in 19.7 s, check_calibration OK
  (|lag| 0.0, |phase| 0.3 deg), coherence_events 0. Then sweep mode, 4 s a
  case: K = 1 ... 5 on 88-108 MHz, one hop with 5 tuners, 3 hops with 5
  tuners (tuners 3-4 never move), tuners 1,3,4 (0 and 2 never move),
  430-440 MHz: every case PASS - every valid packet's per-channel
  frequencies exactly the block assignment of its round, the unselected
  tuners never moving (also in flagged packets), counter + 1 per round,
  round + 1 mod R, flags 0x2800 on every packet; 0 failed retunes, 0 L1
  flushes without a quiet point.
- `t_contrast.py`, `t_settle.py`: the settle (above).
- `t_speed.py`: the speed (above).
- `t_cmds.py` (every check PASS): set_gain 20.7 dB (reply, every header
  channel, status); pause (counter and tuners frozen, flag 0, status run
  false), pause at 10 random points of a hop (flag 0 again within 0.25 s
  every time), resume; 40 plan / tuner changes 0-120 ms apart (no valid
  packet off the plans' grids, 217 packets), a plan change replies round 0 /
  sweeps 0 and the stream then follows it; 30 bad set_sweep requests (each
  field: missing plan fields, steps 0 / 20001, step 99999 / 2400001, centre
  below 24 MHz / last above 1766 MHz, fractions, negatives, strings, 1e400,
  tuners [] / [5] / [0,0] / [-1] / [0.5] / ["a"] / 3, dwell 4 / 5001 / -1 /
  20.5, settle 1001 / -1, run "yes" / 1, a good field with a bad one) all
  refused, the plan unchanged; whitespace around colons parses;
  set_frequency, set_independent_tuner, start_scanner, set_num_elements
  refused; no tuner reached the refused 433 MHz; four log lines in all.
- `t_web.py`: STATE carries operating_mode sweep and the sweep object; the
  page's `SDR_SETTINGS` frequency refused (one log line, no tuner moved),
  its gain applied. `t_page.py` (headless Chromium): banner "Sweep mode ·
  no calibration · 88.0-109.0 MHz · 5 tuners · 54 ms a round", frequency box
  disabled.
- `t_modes.py` (every check PASS): 60 s of sweeping (424 sweeps) without a
  coherence event; sweep -> coherent: reply 0.59 s, tuners back on 100 MHz,
  CONVERGED 23.0 s after the switch, check_calibration OK (|lag| 0.0, |phase|
  0.3 deg), coherence_events 0, lag_no_peak empty; coherent -> sweep during
  a full recalibration (state 0): noise source off in sweep mode, hopping;
  back: CONVERGED 16.7 s, check OK; sweep -> independent (tuners stay where
  the sweep left them, status = header), set_independent_tuner tuner 4 to
  433.92 MHz / 10 dB, independent -> sweep with tuners 0-3 (tuner 4 stays on
  433.92 MHz, every tuner on the common gain); sweep -> wideband (spread
  around 100 MHz) -> sweep; sweep refused while the scanner runs; final
  check OK, coherence_events 0.
- `t_restart.py` (every check PASS): settings saved (`operating_mode=sweep`, the plan,
  dwell 33, settle 12, tuners 0,2,4); paused, stopped, restarted: mode and
  plan restored, running, the first valid sweep packet 1.85 s after the
  start, hops per the plan.
- `t_long.py`: 10 min full range, 5 tuners, dwell 20 / settle 5: 72 sweeps
  (8.4 s each), 0 failed retunes, 0 coherence events, 0 valid packets off
  the grid (18690 valid, 29651 flagged), RSS flat 43.7 MB, CPU 15-25 %
  (machine shared). SIGTERM during a hop: exit 0 in 0.42 s ("Heimdall
  shutdown complete"); SIGINT during a hop (30 s run): exit 0 in 0.27 s.
- Fake librtlsdr (`fakertl/`, a build of heimdall against a fake library
  that models a retune, its settling garbage and the USB latency):
  `t_generic.py` - generic RTL-SDR mode with 1 and 2 dongles: sweep works
  (flags 0x3800, hops per plan), coherent refused, independent and back,
  `operating_mode=sweep` not saved; `t_kerberos.py` - `--kerberos`:
  sweep works, back to coherent stays uncalibrated, the noise source never
  on; the terminal dashboard in a pty ("mode SWEEP", "SWEEP on 89.0-108.1
  MHz hops 11 tuners 0,1,2,3,4 round 0/3 sweeps 68 ..."). t_basic /
  t_cmds / t_modes / t_restart / t_settle / t_long pass on it too.
- Not run: the Raspberry Pi (a Pi 5 since: *On the Raspberry Pi 5*),
  Windows (one-by-one tuning), macOS, the
  Wideband downconverter (refusal is the existing `target != COHERENT`
  check), `--kerberos` on hardware; the MinGW / zig syntax checks (no
  toolchain on this machine).

**Hop timing** (2026-11-21; the user's question "see if there is any way
to speed up retunes to make sweeping potentially faster"; job scratch
52196024 `speed/`, `README.txt` there). The goal: a hop costs the retune,
what has to be thrown away and the data asked for - nothing else.

*Measuring.* The status `sweep` object has `"timing"` (averages of the last
64 hops, reset when the plan, dwell or settle changes; nothing per hop on
any output): `flag_ms` (flag on + downstream flush), `locked_ms`,
`retune_ms` (for_each_tuner, spawn to join) and `tune_max_ms` (the slowest
single tune call), `settle_ms`, `gate_ms` (settle end -> L1 gate +
downstream flush returned), `clear_ms`, `first_valid_ms` (flag cleared ->
the first valid set broadcast), `dwell_ms` (flag cleared -> the hop's
end), `round_ms` (+ min / max), `valid_per_hop` (+ min / max). Test:
`t_hop.py` (every plan / K / dwell / settle case; ms per round and valid
packets per hop also from the 8091 headers). The fake librtlsdr (`fakertl/`)
with `FAKE_TUNE_MS=14` reproduces the KrakenSDR's old numbers within a few
ms (41.0 / 34.1 ms a round at dwell 20 / settle 5 and 5 / 0, 1.46-1.58 /
1.00 valid packets; KrakenSDR 41-48 / 34-39, 1.4-1.6 / 1.0). Where it
differs: its tune takes the same time on every tuner and frequency (the
KrakenSDR's tune call took 14.0-19.4 ms depending on the round's
frequencies: the FM plan with K = 3 and K = 5 ~18-19 ms, K = 2 and 4 ~14),
its five streams share one time base (the KrakenSDR's drift apart by 2
samples a retune - *Accumulated slip* - until the library fix), and its
"USB latency" includes generating the packet (~1-2 ms). New fake options
for the spread case: `FAKE_SPREAD_US` (device i's packet boundaries i x
that later), `FAKE_JITTER_US` (random extra delivery latency),
`FAKE_TUNE_JITTER_MS`.

*The old hop, step by step* (KrakenSDR, 5 tuners, dwell 20 / settle 5, ms
from the hop's start): flag + L2 flush 0.05, retune 14.1-18.9 (thread
spawn + join +0.13 over the slowest tune), settle 5.08, L1 flush 1.5-1.75
(the drain serves it at the next packet round + its quiet point), flag
off, first valid set **13.5** later (two packet periods), dwell end 20.1
after the flag: 41-46 ms, 1.4-1.6 valid packets. Three things beyond the
retune, the settle and the packet that straddles the retune's end:
1. *The first valid set was always dropped.* The conversion worker reads
   the flush epoch BEFORE it waits for the next set (100 ms waits); a wait
   spanning the gate's flush kept the old epoch, so the first set after it
   - the first valid one - was discarded as "converted across a flush".
   Every hop lost a whole packet (6.83 ms). In sweep mode the worker now
   waits in 2 ms slices (`SWEEP_CONVERT_WAIT_MS`) and re-reads the epoch:
   the first valid set goes out a packet period after the gate (7.1-7.2
   ms). Safety unchanged: every dequeue still follows its own epoch read.
   (This was also why settle 0 "already held" in the first contrast runs:
   the first VALID packet was really the second after the flush - re-run
   below.)
2. *The dwell was wall time.* `dwell_ms` from the flag clear: the packet
   whose samples were taken at the dwell's end arrived after the next hop
   had started and was thrown away (20 ms = 2.9 packets, 1.3-1.6 of them
   delivered). Now `dwell_ms` is whole packets: a hop delivers
   `sweep_hop_packets(dwell_ms)` = `ceil(dwell_ms x 2.4e6 / (1000 x
   16384))` valid sets per tuner (5 ms -> 1, 10 -> 2, 20 -> 3, 50 -> 8,
   100 -> 15; status `"hop_packets"`), and the next retune starts the
   moment the last of them is out (`sweep_note_valid_set` wakes the hop
   thread; at most `SWEEP_DWELL_GRACE_MS` 250 ms longer than they take).
   kraken_doa's Auto resolution model follows (sweep-mode.md).
3. *The post-settle L1 flush waited for a cross-dongle quiet point.* That
   point serves pairing (the N-th buffer of every dongle the same instant),
   which sweep mode doesn't need; with spread buffer timing it never comes
   and every hop waited four packet periods (fake, `FAKE_SPREAD_US=1500`:
   L1 flush 28.7-32.4 ms, rounds 60-68 ms, 225 unquiet flushes in 12 s).
   Now the drain serves a per-dongle gate (`sweep_gate_l1`,
   `serve_gate_now`): each dongle drops the packets that can hold an
   earlier sample, each on its own, then the downstream flush. (The first
   version judged a packet by its own arrival stamp with a "steady gap";
   the review showed that a late straddler or a backlog burst defeats it -
   replaced by the earliest-arrival line, *Review of the hop loop*.) Pairing
   is restored where it matters, on the way out (`clear_l1_buffer_paired`
   in `set_operating_mode`, unchanged; a gate still pending then is
   completed by the drain without dropping anything, and the drain serves a
   pending gate before a pending flush).
Not changed: the retune itself (one place, `sweep_tune`, for the library's
call to be swapped - worker F's subject), the thread spawn of
`for_each_tuner` (0.13-0.2 ms), the hop-start L2 flush (0.05 ms).

*What a hop costs now.* The hop starts when the last valid set of the
previous hop is out, i.e. just after a packet arrived. The retune ends
14-19 ms later; the gate waits for the first packet of each dongle stamped
after retune + settle (the straddler, dropped); the dwell is the next M
packets. So a round is a whole number of packet periods: on the fake (14 ms
tune, aligned streams) exactly 4 / 5 / 6 periods (27.3 / 34.1 / 41.0 ms)
for 1 / 2 / 3 packets, settle 0 and 5 alike (retune + 5 ms still ends
before the third packet boundary); spread streams add up to a period
(the latest dongle decides: `FAKE_SPREAD_US=1500` 27.3 / 39.5 / 46.2 ms).
KrakenSDR (5 tuners, x86 PC, both loops the same night, back to back, load
4.6-5.9; `t_hop.py`, 3 s a case; `hw_ab.out`), ms a round and valid packets
a hop, K = 1 / 2 / 3 / 4 / 5:

| dwell / settle | old loop | new loop |
|---|---|---|
| 5 / 0 | 35.4 / 34.1 / 37.4 / 34.1 / 38.7, 1.0 | 28.4 / 27.3 / 30.6 / 27.3 / 31.9, 1 |
| 5 / 5 | 38.8 / 39.4 / 43.5 / 38.7 / 42.9, 1.0 | 31.9 / 30.3 / 36.2 / 32.9 / 36.4, 1 |
| 10 / 5 | 38.5 / 38.5 / 44.3 / 39.3 / 44.1, 1.0 | 38.7 / 37.8 / 42.7 / 39.0 / 43.3, 2 |
| 20 / 5 | 42.8 / 41.1 / 44.5 / 41.4 / 48.3, 1.38-1.59 | 43.4 / 41.1 / 47.0 / 41.9 / 49.0, 3 |
| 20 / 0 | 42.1 / 40.9 / 44.4 / 41.0 / 45.3, 1.29-1.47 | 42.2 / 41.0 / 44.4 / 41.0 / 45.2, 3 |
| 50 / 5 | 74.9 / 73.7 / 77.6 / 73.4 / 79.0, 6 | 80.4 / 78.9 / 81.9 / 80.3 / 83.5, 8 |
| 100 / 5 | 119.6 / 119.3 / 123.2 / 119.0 / 124.6, 13 | 124.9 / 124.9 / 130.0 / 125.3 / 131.2, 15 |

Every new hop delivered exactly its packets (min = max). The new
timeline (K = 5, dwell 5 / settle 5): retune 19.3, settle 5.1, gate 4.8,
first valid 7.2, end 7.2 after the flag. Per valid packet: old 20 / 5
~30 ms, new 5 / 0 28-32, new 20 / 5 14-16. 88-108 MHz (11 hops), sweeps a
second K = 1-5: old 20 / 5 2.12 / 4.06 / 5.61 / 8.04 / 6.90; new 5 / 0
3.20 / 6.10 / 8.17 / 12.21 / 10.46, 5 / 5 2.85 / 5.51 / 6.91 / 10.12 /
9.16, 10 / 5 2.35 / 4.41 / 5.85 / 8.54 / 7.69, 20 / 5 2.09 / 4.05 / 5.31 /
7.95 / 6.80. 24-1765 MHz (911 hops, rounds x ms): K = 5 old 20 / 5 7.78 s
(timed: 7.77), new 5 / 0 5.16 s, 10 / 5 7.41 s, 20 / 5 8.69 s (timed 8.58);
K = 1 old 38.5 s (timed 37.6), new 5 / 0 25.5, 10 / 5 35.3, 20 / 5 41.7
(timed 41.6). So: at the same dwell the new loop is up to 20 % faster for
dwell 5 and delivers 2x the data for dwell 10 / 20 at the old speed (a
dwell of 50 / 100 now gets the 8 / 15 packets it holds, a few ms longer).
The KrakenSDR's gate took 4-6 ms at settle 5 against the fake's 0.7-1: its
tune is 0.5-5 ms longer and its streams are spread (the slip), so settle 5
pushes the straddler a packet later there (+3.6-4.5 ms a round against
settle 0). CPU: 11.1 % of a core for the whole new run (the old 9.8 %, the
reader of 8091 on headers only).

*The dwell to use.* The fastest sweep that is still readable at Auto
(kraken_doa: >= 8 frames a hop): dwell 5 (1 packet, Auto 2048 at 88-108
MHz with 15 overlapped frames, 512 for the full range) - FM 10.5 sweeps a
second with 5 tuners, the full range 5.2 s; dwell 10 gives today's speed
with exactly 2 packets a hop (Auto 4096, as today's default gave from
1.3-1.6). DECIDED 2026-11-21, on the final path's numbers (the fast tune
call and worker F's library, *Review fixes of the hop loop* below): the
default dwell is 5 ms - one packet a hop (`SWEEP_DWELL_MS`; kraken_doa's
`SWEEP_DWELL_DEFAULT_MS`, its settings schema and the page's field follow;
until then 20, and every number above that names "the default" means 20).
The default settle is 2 ms (`SWEEP_SETTLE_MS`; 5 until then, and 5 again
for a few hours on the second review's point that the gate's bound is
"retune end + settle - L*", L* the dongle's fastest USB delivery, ~1 ms
here, with neither a Raspberry Pi nor a reader running behind under load
measured; a Pi 5 since: *On the Raspberry Pi 5*). What decided it, the orchestrator's end-to-end runs on the
KrakenSDR with kraken_doa and the page attached (the hop then starts ~2.5 ms
after a packet boundary - the last packet's way through the pipeline): at
settle 5 the first packet that counts falls one packet period later in
nearly every hop, 28 ms a round against 21 at settle 2 - the whole range
with 5 tuners 5.14 s against 3.96 s, one tuner 22.8 s against 18.9 s,
88-108 MHz 10.4 against 11.1 sweeps a second (the standalone `t_hop.py`
runs above, with only a header reader on 8091, show less of it: 4.1-4.3 s
at settle 5). And the margin is larger than "2 - L*" suggests: the stream
measured clean at settle 0 with its positive control, i.e. from ~1 ms before
the tune call returns (the PLL is written a control transfer or more before
that; a slower USB host returns later still), so 2 ms stands ~3 ms after
the measured-clean point. NOT measured: a Raspberry Pi (a Pi 5 since: *On
the Raspberry Pi 5* - clean at settle 0 / 2 / 5, and 2 ms is also the faster
setting there), Windows, a reader
thread held up for longer than the gate's window - run `t_contrast.py`
there; a host that shows the previous hop in a hop's first valid packet
needs a larger settle (`set_sweep` `settle_ms`, or the constant).

*The guarantee, re-tested* (the first valid packet now comes a packet
earlier than before, so what "settle 0 holds" meant changed):
- fake (`t_fakeguard.py`: every channel of every packet, per 1024-sample
  segment: the fake's settling garbage or a tone of the old hop), garbage 8
  ms after the tune call, tune 14 ms, dwell 5: valid packets with garbage
  at settle 0 / 4: 147 / 147 of 147 (as designed: settle < garbage); at
  settle 6 / 8 / 9 / 10 / 12: 0 of 117-118 each, while 118 of ~353 flagged
  packets had it (positive control). Garbage 4 ms: 0 contaminated at settle
  0 / 4 (the straddler absorbs it), 1-2 of 147 at settle 3 / 5 - the
  semantics: the samples see settle minus the latency (here the fake's ~2
  ms including generating the packet). Spread streams and jitter
  (`FAKE_SPREAD_US=1500 FAKE_JITTER_US=1500 FAKE_TUNE_JITTER_MS=4`,
  garbage 8 ms): contaminated 130 / 84 / 0 / 0 of 130 / 109 / 97 / 90
  valid packets at settle 0 / 6 / 11 / 14, flagged ones 90 / 109 / 178 /
  180 - again clean from settle >= garbage + latency (here up to ~3.5 ms),
  and at settle 0 the old hop's tones too (the slowest tuner's samples
  from just before its tune call returned, within the latency): what
  "after the settle" relies on is a settle covering the USB latency - on
  the KrakenSDR the tune call itself returns well after the PLL moved, so
  settle 0 already measured clean there (below), and the default 5 ms is
  the margin.
- KrakenSDR, `t_contrast.py` on the new loop (tuner 0 alternating 92.0 /
  94.0 MHz, the station 15.4-15.6 dB over B's floor at +0.6 MHz, dwell 40
  = 6 packets): the first valid packet of each B hop, worst 1024-sample
  segment over 147-164 hops (20 s a settle): +4.0 / +4.0 / +3.3 dB at
  settle 0 / 2 / 5 against +4.0 / +4.0 / +4.3 for the later packets of the
  same hops; segment 0 (where old-hop samples would sit) p90 +1.6 / +1.7 /
  +1.6 against +1.5 / +1.4 / +1.4 for segments 1-15; positive control: 163
  of 326 / 164 of 328 / 147 of 441 flagged B packets with a segment > +10
  dB (worst +16.8 dB). A first 10 s run: +3.7 / +5.6 / +3.3 / +3.5 at
  settle 0 / 2 / 5 / 10 (82 / 82 / 74 / 68 hops; the +5.6 not seen again
  in the 163 hops of the rerun - noise); the large jump (29 hops 24.8 ...
  92.0 MHz, station 25 dB over the floor, 5 B hops in 10 s): +2.4 / +3.3
  at settle 0 / 5 against later maxima +3.2 / +2.7. So settle 0 still
  holds with the first valid packet a packet earlier than before - the tune
  call returns well after the PLL moved (lock check, autotune) - and 5 ms
  stays the default as margin for slower USB hosts (the Pi not measured).
  (Since then: the default is 2 ms, and on a Pi 5 - *On the Raspberry Pi 5*
  - the slower USB host needs no more: its packets arrive ~0.7 ms later
  than the PC's, `t_contrast.py` is clean at settle 0 / 2 / 5, and settle 5
  costs a packet period a hop more there than settle 2 at K = 1, 2, 4.)

*Everything else, re-tested on the new loop* (job scratch `speed/`):
- fake (`regress_fake.sh`, `FAKE_TUNE_MS=14`): t_basic (every case PASS;
  the one-hop / three-hop K' = H cases 20.5 ms a round = 3 packets, no
  flag), t_cmds (set_gain, pause at 10 random points of a hop - flag 0
  within 0.25 s every time -, 40 plan changes 0-120 ms apart with no valid
  packet off the grids, 30 bad requests, refusals: 0 failures), t_modes,
  t_fixes (failed tuner -> frequency 0, slow gain writes, the settings
  file), t_restart, t_generic, t_kerberos, t_long 2 min: all PASS.
  `t_race.py` prints "BOTH" for its tries 2 / 4 / 6 / 8 - identically on
  the old loop's build (b0e7898): after try 1 the count is already the
  one asked for ("already active"), so those tries race nothing; a test
  artefact, not a finding.
- KrakenSDR: t_basic (coherent CONVERGED 22.9 s, check_calibration |lag|
  0.0 / |phase| 0.2 deg; every sweep case PASS; 0 coherence events, 0
  unquiet flushes), t_cmds 0 failures, t_web PASS, t_modes 0 failures (60
  s of sweeping, 399 sweeps, 0 coherence events; sweep -> coherent reply
  0.60 s, CONVERGED 23.1 s; mid-calibration entry and back 22.1 s; final
  18.4 s), `t_long.py --minutes 9 --dwell 5` (full range, 5 tuners, settle
  5): 101 sweeps in 542 s (5.0-6.0 s each, 27 ms a round), 18534 valid /
  41677 flagged packets, 0 off the grid, 0 failed retunes, 0 coherence
  events, RSS flat 41.97-41.98 MB, CPU 16-17.5 %; SIGTERM during a hop:
  exit 0 in 0.47 s.
- kraken_doa's suite against its fake heimdall (which already delivered
  ceil(dwell / 6.83 ms) packets a hop), test build and ASan + UBSan build:
  t_sweep 206 / 206, t_edge 50 / 50, t_modes 15 / 15, t_old 9 / 9, t_mcp 9 /
  9, t_fix 29 / 29 after its Auto expectations followed the new model
  (dwell 20: 8192, not 4096; 50: 16384; explicit 16384 at dwell 20
  averages 5 frames, not 1-2, so "auto is smoother" is now sd 1.27 vs 1.69
  dB, a ratio of 0.75 - the check takes < 0.85); no sanitizer reports.
- Not run: the Raspberry Pi (a Pi 5 since: *On the Raspberry Pi 5*),
  Windows (tunes one by one: nothing in the hop
  assumes a short retune - the gate and the dwell's timeout run from the
  retune's end), macOS, the MinGW / zig syntax checks (no toolchain here).

**Review of the hop loop** (2026-11-21, an independent review of the hop
loop's first commit, and worker F's tune call; job scratch `speed/`; fake
tests with the fake librtlsdr's new knobs `FAKE_LATE_EVERY` /
`FAKE_LATE_US` (every N-th packet delivered late), `FAKE_STALL_FILE` /
`FAKE_STALL_DEV` / `FAKE_BURST_US` / `FAKE_STALL_KEEP` (a reader stalls
while the file exists, then delivers its backlog - at most 15 packets,
older samples lost - that far apart), `FAKE_FAST_TUNE_MS`,
`FAKE_FAST_FAIL_PPM`, `FAKE_FINISH_RC`; "g1" = a build of the first gate):
1. *The gate's bound was weaker than written.* It judged each dongle's
   straddler by that packet's own arrival stamp and a "steady" gap of
   0.5-1.5 periods. (a) A straddler delivered up to half a period late
   after an on-time packet still looked steady, and the packet after it
   could start up to ~3.4 ms + the latency before T. (b) After four
   periods without a steady packet it took whatever was stamped after T: a
   reader still emptying a backlog had the rest of the backlog - samples
   from before the retune - go out as valid. Now the decision follows when
   a packet's samples were TAKEN: the callback numbers every packet and
   keeps its arrival stamp in a ring (`l1_put`, `ring_t` / `ring_n`,
   `sdr_device.hpp`; allocation-free, a seqlock per slot), and the gate
   draws each dongle's earliest-arrival line `f(s) = min over the last 32
   packets n <= s of (A_n + (s - n) P)`. Since a packet arrives after its
   last sample (latency >= 0) and a dongle's packets end at least P apart
   in its own time (lost samples, the 2-sample slip and a packet the
   callback drops only add), the last sample of packet s was taken at or
   after `f(s) - L* - d`, L* = the latency of the packet the minimum runs
   through (the fastest delivery of the window), d <= 32 P x 100 ppm = 22
   us of sample-clock error. The gate drops every packet up to and
   including the first s with `f(s) >= T` and keeps s + 1 on, so **every
   sample of a kept packet was taken at or after T - L* - d** - whatever a
   single late delivery or a burst does (a late packet only raises its own
   term). It also fixed an overdrop the first gate didn't have: the packet
   that wakes the drain is already in the dropped partial set, and counts.
   Tests (fake, tune 14 ms + 0-7 ms jitter, dwell 5, `t_fakeguard.py`;
   contaminated = a valid packet with the fake's settling garbage or the
   old hop's tones in any 1024-sample segment of any channel):
   - late straddlers (`FAKE_LATE_EVERY=2 FAKE_LATE_US=2700`, garbage 4 ms):
     g1 contaminated 65 / 29 / 9 of 149 / 141 / 134 valid packets at settle
     5 / 6 / 7 (worst 26.5 dB), 0 from settle 8; the line gate 0 of 135 /
     131 / 126 / 127 / 122 at settle 5 / 6 / 7 / 8 / 10 (flagged ones with
     garbage 97-229: the positive control). At settle 0 both see the
     garbage (180 / 98 contaminated): settle < garbage, as designed.
   - a reader stalling 80 ms, its backlog delivered 4 ms apart
     (`FAKE_STALL_DEV=2 FAKE_BURST_US=4000`, 120 ms +-50 % between stalls,
     garbage 2 ms): g1 contaminated 43 of 190 / 24 of 156 valid packets at
     settle 5 / 10 - the old hop's tones at 49.5 dB; the line gate 0 of 175
     / 0 of 158 (163 late packets counted, `gates_late`). With faster
     bursts (1.5-3 ms apart) g1 stayed clean too (0 of 121-169).
   - garbage 8 ms, no jitter: 196 / 196 contaminated at settle 0 / 4, 0 at
     6 / 8 / 10 (147-148 each; flagged with garbage 147-148).
2. *A silent dongle kept the flag at 1 forever, silently,* with the 8091
   counter stepping on every retry and pause unable to clear it. Now: the
   gate tells a stalled dongle (nothing for 100 ms) from a slow line
   (400 ms cap, a stream restart's gap) and from leaving the mode;
   `sweep_gate` logs "tuner(s) N delivered no data ... the 8091 flag stays
   1" at once, then at most every 10 s; status `stalled_tuners`,
   `gates_failed`, `gates_late` (replacing the first version's
   `gates_unsteady`); the counter steps once per round, not per retry; the
   drain's stuck-device alarm (3 s of silence -> the coherence watchdog, as
   outside the gate - its wait loop isn't reached while a gate is pending)
   is raised by the gate itself, at most every 3 s; the requester stops
   waiting as soon as the mode is left. What sweep mode does with a silent
   dongle: nothing can be measured (a set needs every active dongle), so
   the flag stays 1 - the guarantee rules out clearing it, the dongle's
   backlog may hold samples from before the retune - and the gate is
   retried until the dongle streams. `t_stall.py` (fake, tuner 2 silent):
   2 s silent - status retuning true, `stalled_tuners` [2], round / sweeps
   frozen, no packet out, the counter 107 -> 108 over the silence, hops on
   at once after it; pause while silent - answered in 0.00 s, run false,
   flag 1 until the dongle streams, then 0 with 205 valid packets; leaving
   for independent while silent - answered in 0.00 s; 0 coherence events
   from silences under 3 s in sweep mode (the gate keeps every L1 empty);
   4 s silent - the stuck-device event and its log line, sweeping again
   after; the silence logged twice in all. (Independent mode with a silent
   dongle raised 2 L1-overflow events - the drain's usual reaction there,
   not sweep mode's.)
3. Nits: the timeline's `first_valid` and `t_clear` are taken before the
   flag clears (no set counts as valid while it is on: no hop loses its
   stamp, no negative value); the drain's comment on a gate pending after
   the mode was left (the partial set is still dropped - a one-packet slip
   of those channels, repaired by the paired flush on every way back to
   coherent). kraken_doa's `hop_samples` with an OLDER heimdall (wall-time
   dwell, 1.3-1.6 packets at 20 ms): Auto then plans for 3 packets, a hop
   averages 3-7 frames instead of >= 8 - noisier, it works; not detected
   (the client's status reader is in data_receiver.cpp, outside this
   change), and the two ship together.
4. *The tune call* (worker F): `sweep_tune` uses the fork's fast path,
   `rtlsdr_set_center_freq_fast(dev, f, RTLSDR_TUNE_KEEP_AUTOTUNE |
   RTLSDR_TUNE_NO_DUMMY_READ)` where the header has the second flag, only
   `KEEP_AUTOTUNE` where it has the first, the full path otherwise; a
   failure retries once on the full path (frequency 0 if that fails too).
   The fast path leaves the 128 kHz PLL autotune clock: `fast_pending`
   (bit per tuner) until `finish_fast_tunes` - `rtlsdr_finish_tune`, and on
   1 (not locked) or < 0 the tuner's frequency once more on the full path,
   logged - runs wherever the sweep stops moving tuners: in
   `sweep_quiesce_flushes()` (called by `set_operating_mode` with
   `settings_mutex` held, on the way to coherent, before
   `return_to_coherent_locked` retunes every tuner and the recalibration is
   requested); in the hop thread's idle branch under `settings_mutex` (the
   mode left for independent / wideband, paused, the pipeline down); in the
   retune section for tuners out of the plan or on their block's only hop
   (K' = H). In sweep mode the finish is covered by the flag (on before,
   settle + gate after, as a gain change). `t_finish.py` (fake): no finish
   while hopping; pause -> 5 finishes, flag 0 after; K' = H -> 5 finishes,
   then no tune / finish, the stream valid; -> independent 5; -> coherent:
   the finishes before "Mode: tuners retuned by the sweep", CONVERGED, 0
   coherence events; with `FAKE_FINISH_RC=1` every tuner "PLL not locked at
   the end of its fast retunes ... retuned on the full path" (5 lines), no
   failure. Fast tunes failing 10 % (`FAKE_FAST_FAIL_PPM=100000`):
   `t_basic.py` every case PASS, 0 failed retunes (the full-path retry).
   On the fake with a 7.5 ms fast tune (F's 3-transfer figure) a round is
   3 / 4 / 5 packet periods for dwell 5 / 10 / 20 at settle 5: 20.5-21.6 /
   27.3-27.9 / 34.2-34.6 ms (FM, 5 tuners: 15.5 / 11.9 / 9.7 sweeps a
   second; dwell 5 / settle 0 16.3).
5. *On the KrakenSDR with F's final library* (fork branch
   `lock-read-no-slip`: no control IN of 3 mod 4 bytes, so no tune shifts a
   stream; the 5 ms PLL settle also after a VCO jump > 400 MHz;
   `RTLSDR_TUNE_NO_DUMMY_READ`), 5 tuners, x86 PC, 03:46-04:23, load
   1.9-2.5. The library rebuilt from scratch in the worktree (the copied
   build dir named the user's checkout); verified by its header
   (`RTLSDR_NO_STREAM_SLIP`, `RTLSDR_TUNE_NO_DUMMY_READ`) and by the
   status timing: the slowest single tune of a round (`tune_max_ms`) 7.65
   - 7.72 ms on the FM plan with K = 2 / 4 (same-band steps; F: 7.5 ms, 3
   transfers), 9.3 - 9.6 ms with K = 1, 12.5 - 14.2 ms with K = 3 / 5 (a
   tuner's wrap to the start of its block: the VCO settle), 8.0 - 8.8 ms on
   the full range - against 14.5 - 19.7 ms on the full path with the same
   library. Old tune call (the test tree's `sweep_tune` forced onto the
   full path, `run_fullpath/`) against the new one (`run_final/`), back to
   back, `t_hop.py` (3 s a case; `hwf_full.json`, `hwf_fast.json`), ms a
   round at settle 5 and FM sweeps a second, K = 1 / 2 / 3 / 4 / 5:

   | dwell | full path, ms | fast path, ms | fast path, sweeps / s |
   |---|---|---|---|
   | 5 | 31.6 / 29.7 / 36.2 / 31.9 / 36.6 | 23.1 / 22.7 / 28.8 / 21.9 / 29.4 | 3.93 / 7.34 / 8.68 / 15.24 / 11.34 |
   | 10 | 37.0 / 37.1 / 42.9 / 36.7 / 42.7 | 30.2 / 27.6 / 34.4 / 28.6 / 36.5 | 3.01 / 6.05 / 7.26 / 11.65 / 9.14 |
   | 20 | 43.3 / 41.7 / 48.8 / 43.2 / 49.6 | 36.5 / 34.4 / 41.1 / 34.7 / 42.9 | 2.49 / 4.85 / 6.09 / 9.61 / 7.76 |

   (full path sweeps a second at dwell 5 / 10 / 20, K = 5: 9.10 / 7.80 /
   6.72; dwell 5 / settle 0, fast: 21.7 / 20.5 / 23.8 / 20.5 / 25.0 ms.)
   Every hop delivered exactly its packets (min = max). 24-1765 MHz, one
   sweep timed, dwell 5 / 10 / 20 at settle 5: 5 tuners 4.25 / 5.36 / 6.59
   s (full path 5.96 / 7.02 / 8.55), 1 tuner 18.97 / 25.23 / 31.45 s (full
   path 26.96 / 32.08 / 38.61). CPU 10.9 % of a core for the fast-path
   run (12.2 % full path). A round is still a whole number of packet
   periods, and where retune + settle ends against the packet grid moves it
   by one: `t_basic.py` after a coherent start (the streams aligned by the
   calibration) gave 41-45 ms at dwell 20 / settle 5 where `t_hop.py`
   (streams as started) gave 34-43.
   The rest on the final path: `t_contrast.py` (station 16.4 dB over B's
   floor, 164-184 hops a settle, 20 s each) - the first valid packet's
   worst segment +3.8 / +3.6 / +3.4 dB at settle 0 / 2 / 5 against the
   later packets' +4.2 / +4.0 / +4.5, segment 0 p90 +1.4 / +1.4 / +1.3
   against +1.5 / +1.5 / +1.4; flagged B packets with the station: 0 of 184
   / 0 of 184 / 163 of 327. At settle 0 / 2 no flagged packet holds the
   old hop any more (with the shorter tune the straddler - dropped - is the
   only packet that does), so the positive control there is a test build
   without the gate (`run_nogate/`: the test tree's hop skips the gate and
   its flush, the flag cleared right after the settle): at settle 0 the
   first valid packet's segment 0 is +16.0 dB (median of 158 hops; worst
   +16.7, segments 0-2 over the later packets' max, p90 of segment 0 +16.3
   against +2.8 for the others) - the old station, i.e. exactly what the
   gate removes; at settle 5 the same build is clean (+4.4 worst against
   +4.2; the straddler goes out flagged during the settle: 138 of 276
   flagged packets with the station). (A build with the gate's T 10 ms
   early stayed clean at settle 0 too, +3.4 dB: packets the drain
   collected before it served the gate are flushed downstream either way.)
   Settle 2 / 3 on the final path (`t_hop.py`, 04:27, load 2.1-2.4), ms a
   round K = 1-5: settle 2, dwell 5 / 10 / 20: 21.7 / 20.5 / 23.9 / 20.4 /
   25.0, 28.6 / 27.3 / 30.8 / 27.2 / 32.1, 35.2 / 34.1 / 37.4 / 34.1 /
   38.7 (FM sweeps a second at dwell 5: 4.19 / 8.14 / 10.47 / 16.31 /
   13.35); settle 3: 22.9 / 20.5 / 26.8 / 20.5 / 29.4, 29.7 / 27.3 / 34.1
   / 27.3 / 36.5, 36.5 / 34.1 / 40.9 / 34.1 / 43.0. Settle 2 runs as fast
   as settle 0 (a 7.5-9.5 ms tune + 2 ms still ends before the second
   packet boundary), settle 5 costs a packet period in most rounds.
   `t_basic.py` (CONVERGED 19.1 s, check |lag| 0.0
   / |phase| 0.2 deg, every case PASS), `t_cmds.py` 0 failures,
   `t_modes.py --sweep-seconds 60` 0 failures (447 sweeps, 0 coherence
   events; sweep -> coherent: reply 0.60 s, CONVERGED 6.5 s, check clean;
   mid-calibration entry and back 8.5 s; final 3.9 s; no "PLL not locked"
   in its log), `t_long.py --minutes 9 --dwell 5` (full range, 5 tuners,
   settle 5): 130 sweeps in 542 s (4.30 s each, 20 ms a round), 23920 valid
   / 30339 flagged packets, 0 off the grid, 0 failed retunes, 0 coherence
   events, RSS 42.02 MB flat, CPU 14.8 %, SIGTERM during a hop: exit 0 in
   0.37 s. No level jumps were looked for at 1.6-1.75 GHz (F's open
   question) - the tests here don't measure levels there.

*Third review round* (2026-11-21, the gate of 790f060 together with worker
F's return path; defaults then dwell 5, settle 5):
1. *The burst protection rested on an accident.* With a window of 32 and
   `RTL_USB_BUF_COUNT` 32 (a knob documented to be raised), a stalled
   reader's backlog could fill the whole window; L* is then the stall
   length, and only the transfer count (a kept packet's transfer could not
   be resubmitted before the burst was out) kept the data fresh. The
   reviewer's simulation (`review_s2/burst.cpp`, my arithmetic; `speed/
   burst2.cpp` with the window as now): backlog B = 33 / 40 / 64 with a
   window of 32 - first kept sample 84 ms before T after a 300 ms stall,
   284 ms after 500 ms; with the window `B + 16` - 0.50 ms at every B and
   stall (= the simulation's floor latency). Now: the window is
   `SDRDevice::LINE_PACKETS = RTL_USB_BUF_COUNT + 16` (48), the stamp ring
   `2 x 48 + 32`, both `static_assert`ed; a window counts only when it is
   complete - all 48 stamps readable and none before a stream (re)start or
   a short transfer - else the packet counts as "before T" (a packet judged
   late, after the ring overwrote part of its window, can no longer be
   judged by its newer half alone). And the proof now says what L* is: the
   fastest delivery in the window (every term bounds E_s), small because
   the window always holds packets a running reader delivered. Fake, a
   reader stalling 300 ms (150 ms +-50 % apart), tune 14 + 0-7 ms, garbage
   2 ms, settle 5, 15 s: backlog 32 delivered 0.5 ms apart - 0 of 83 valid
   packets contaminated; backlog 40 - 0 of 149 (inside the window too);
   backlog 32 delivered 4 ms apart, i.e. a reader that then keeps running
   behind for ~75 packets - 5 of 59 contaminated (the old hop's tones, 49
   dB): the case the bound doesn't cover, below.
2. The bound in those words: **retune end + settle - L***, L* the fastest
   USB delivery among the window's 48 packets. The settle has to cover
   L*; L* is the USB floor (~1 ms on the PC) unless the reader runs
   steadily behind for longer than the window (~330 ms) - then L* is that
   lateness, and nothing here bounds it. Not measured: a Raspberry Pi, a
   loaded host. Hence the default settle 5 (orchestrator). (Since then 2 ms;
   a Pi 5 measured: *On the Raspberry Pi 5*.)
3. *A stale gate after the re-pairing flush.* A gate filed just before the
   mode was left (its requester stops waiting then) could be served after
   `set_operating_mode`'s paired flush, mid-collection, dropping a partial
   set - a one-packet slip nothing repairs any more with the phase-only
   return. Now `sweep_quiesce_flushes()` (under `flush_mutex`: no gate can
   be filed any more) waits in `sweep_gates_quiesce()` until the drain has
   served every gate ever filed (pending, in flight - it returns at its
   next 100 us poll once the mode is left - or stale), clears the hold, and
   only then returns to `set_operating_mode`, which files the paired flush.
   Proof: every drop a gate causes (L1 in `serve_gate_now`, the partial set
   in the drain's collection loop, the hold's drops) happens on the drain
   before it marks that gate completed, or (the hold) only while the mode
   is SWEEP; after the quiesce no gate is pending and the mode isn't
   SWEEP, so the drain's next L1 touch is the paired flush. A test cannot
   force that interleaving; `t_exitstress.py` (sweep -> coherent at a
   random point of a hop, 20 times, fake): every return CONVERGED with
   check_calibration |lag| 0.0, 0 coherence events, `lag_no_peak` empty.
4. *Slow dongles went unreported.* A gate that timed out with every dongle
   delivering (the line short of T: lost packets) neither set nor cleared
   the stalled mask, and a stall of an earlier visit stayed in the status.
   Now `slow_tuners` (status) and its own log line ("deliver, but their
   packets don't reach the hop's start in 400 ms (packets lost?)"), both
   masks cleared by a gate that passes and when the mode is entered.
   `t_stall3.py` (fake, tuner 3 losing 3 of 4 packets): `slow_tuners`
   [3], `stalled_tuners` never, the line logged; the sweep crawls on (96
   valid packets in 3 s: its line reaches T after ~1 s, the retries keep
   T); left and entered again - both masks cleared.
5. *The other dongles' L1 during a silence.* Kept small only by the ring
   cutting a retried gate's backlog (peaks ~92-96 of 100 estimated). Now,
   by design: after a failed gate the drain keeps every L1 empty
   (`gate_hold`) until the next gate is served or the mode is left; while a
   gate waits, a dongle already past its straddler keeps at most its newest
   16 packets (`GATE_KEEP_PACKETS`); a dwell in which no set has gone out
   for 100 ms + a period ends early (the stream stopped - the drain then
   waits for the silent dongle with every other L1 filling); and in sweep
   mode the drain waits for a dongle in 5 ms slices (100 ms elsewhere;
   the stuck-device alarm counts the time waited, unchanged at 3 s), so a
   gate filed while it waits is served at once. `t_stall3.py`: over a 2.8 s
   silence of tuner 2 the other dongles' L1 peaked at 15 of 100 (status
   `l1_depth`, added, polled every ~10 ms; before the dwell rule and the
   slices 26-30), 0 coherence events; with a dongle losing 3 of 4 packets
   21 (the drain forms sets at its rate between gates).
6. A tune that fails on both paths now counts as fast-pending (the driver
   may have left the 128 kHz autotune clock): the finish runs for it too.
7. A short transfer (fewer than NUM_SAMPLES x 2 bytes: stream start /
   cancel) restarts the line after it (`stream_first`), so no window spans
   it.
8. The unreachable `reconfig_in_progress` branch in `finish_fast_tunes`
   removed (an element-count change requires coherent mode).
Fake regression on these changes (`regress_fake3.sh`): t_basic, t_cmds,
t_modes, t_fixes, t_restart, t_generic, t_kerberos, t_long 2 min, t_stall,
t_stall3, t_finish (rc 0 and 1), t_exitstress: 0 failures; t_race "BOTH"
as on the old build (a test flaw); t_fakeguard: late straddlers
(`FAKE_LATE_EVERY=2 FAKE_LATE_US=2700`, garbage 4 ms) 95 / 1 / 0 / 0
contaminated of 159 / 133 / 130 / 125 at settle 0 / 5 / 6 / 8 (at 5 the
latency floor of the fake, ~2 ms incl. generating the packet, leaves
garbage 4 + 2 > 5: the semantics); garbage 8 ms: 196 of 196 at settle 0,
0 of 147 at 6 and 10.
KrakenSDR on these changes (worker F's first library patch,
`lock-read-no-slip` - his second commit was not passed before these runs;
06:29-07:06, load 1.9-2.5, `hw_r3.out`): `t_contrast.py` (station 14.8-15.0
dB over B's floor, 20 s a settle) - first valid packet's worst segment
+4.2 / +3.7 dB at settle 5 / 0 against +4.5 / +4.4 for the later
packets, segment 0 p90 +1.3 / +1.6 against +1.5 / +1.5; flagged B packets
with the station 163 of 327 / 0 of 184; the no-gate build at settle 0
(positive control): segment 0 +14.7 dB median over 158 hops, segments 0-2
over the later max. `t_hop.py`, settle 5, FM ms a round K = 1-5 (sweeps
a second): dwell 5 - 23.2 / 21.2 / 27.9 / 20.6 / 29.4 (3.92 / 7.86 / 8.97
/ 16.20 / 11.35); dwell 10 - 29.7 / 27.4 / 34.2 / 27.3 / 36.5 (3.06 /
6.09 / 7.31 / 12.21 / 9.14); dwell 20 - 36.6 / 35.1 / 41.6 / 36.3 / 43.0
(2.48 / 4.75 / 6.01 / 9.19 / 7.75); every hop exactly its packets;
24-1765 MHz timed at dwell 5 / 10 / 20: 5 tuners 4.11 / 5.58 / 6.86 s, 1
tuner 18.95 / 25.17 / 31.42 s; CPU 12.6 % of a core (10.9 % in round 2 -
the drain's 5 ms wait slices in sweep mode). `t_modes.py --sweep-seconds
60`: 0 failures, 447 sweeps, 0 coherence events; sweep -> coherent (the
phase-only return, "the library keeps the streams in step") reply 0.61
s, CONVERGED 2.1 s, check_calibration |lag| 0.0 / |phase| 0.1 deg;
mid-calibration entry and back 4.0 s; final 1.0 s; no "PLL not locked".
`t_exitstress.py --n 15`: 15 returns at random points of a hop, every
check_calibration |lag| 0.0 / |phase| 0.1-0.3 deg, 0 coherence events,
`lag_no_peak` empty. `t_long.py --minutes 9` at the defaults (dwell 5,
settle 5, full range, 5 tuners): 130 sweeps in 542 s (4.30 s each), 23924
valid / 30209 flagged packets, 0 off the grid, 0 failed retunes, 0
coherence events, RSS 41.98-41.99 MB flat, CPU 14.6 %, SIGTERM during a
hop: exit 0 in 0.27 s.

**Review fixes** (2026-11-20, an independent review of the merged diff and
the orchestrator's end-to-end run; tests on the fake librtlsdr build -
`t_fixes.py` - and the KrakenSDR - `t_lagshift.py`, `regress.sh`):
1. `--kerberos`: a sweep came back with the manual calibration restored as
   valid (STALE only if `current_frequency` or the per-bin EQ changed - the
   sweep changes neither), after hundreds of PLL rewrites per tuner. The
   wideband scan and independent mode had the same hole (same restore; the
   scan's spread retunes every tuner). Now every PLL / gain write outside
   coherent (`note_tuners_changed_outside_coherent`: sweep hops, the spread,
   `set_independent_tuner`, gain / frequency through `update_sdr_settings`;
   reset when coherent is left) makes the restore STALE. Fake: calibrated ->
   sweep / wideband / independent + a tuner / independent + a gain change ->
   coherent all STALE (0x304, `calibration_state` stale); independent
   untouched stays calibrated (0x104).
2. A failed retune was sent as valid data at the OLD frequency - but the
   tuner is not there: `tuner_r82xx.c` / `tuner_e4k.c` fail after N / SDM
   are written and librtlsdr sets `dev->freq = 0`. Now its
   `tuner_frequencies` entry is 0 ("no valid frequency", on no client's
   grid) from that hop until a retune succeeds (retried every round);
   `failed_tuners` in the status. Contract and data-protocol doc updated.
   Fake, every tune into 95-97 MHz fails (hop 4 = tuner 2): 66 failures in
   6 s, 84 tuner-packets with frequency 0, 0 valid packets with a tuner off
   its assigned hop, one log line.
3. A gain change's settle ran from BEFORE the writes (`sweep_note_tuner_change`
   was its only stamp): writes longer than settle + flush (always on Windows)
   let samples from during the writes through. Now
   `sweep_note_tuner_change_done()` stamps again after the writes (and the
   rollback); the flag clear checks the stamp under `settings_mutex`, which
   the writes hold. Fake (gain writes 40 ms, garbage until 5 ms after,
   settle 0, one hop so the stream runs on): 18 gain changes (12 hopping, 6
   paused), 0 of 681 valid packets with garbage; 11 of 175 flagged ones had
   it (positive control).
4. An unquiet sweep flush could slip a channel by a packet and the phase-only
   return could not repair it. Now leaving sweep for coherent waits until no
   sweep flush runs (`sweep_quiesce_flushes`, a mutex every sweep flush
   takes with the mode re-checked), then flushes L1 again and requires the
   quiet point (`clear_l1_buffer_paired`, 3 tries); none -> the coherence
   recovery (`signal_coherence_lost`). And the return now always re-acquires
   the lags (8.). Not observed (`l1_unquiet_flushes` stayed 0 in every run).
5. The settings file took `sweep_first_center_hz=-1000000000` (stoull wraps)
   and computed the last centre with wrap-around. Now digits only, the three
   keys together, and `validate_sweep_plan` (moved to `types.hpp`, shared
   with 8092; overflow-safe). Fake: negative centre, last centre past 1766
   MHz, two of three keys -> the default plan and a log line; bad tuners /
   dwell ignored with a line; a valid plan loads.
6. The quiet flush's "drain did not respond" throttle exchanged on every
   occurrence (a fault recurring under 10 s logged once, then never); it now
   throttles against the last log line.
7. `set_num_elements` then at once `set_operating_mode sweep` on one
   connection could run the mode change between the 8092 precheck and the
   detached worker's flag. `reconfigure_num_elements` now re-checks mode and
   scanner under `settings_mutex`; `set_operating_mode` refuses while
   `reconfig_in_progress` (8092: "Element-count reconfiguration in
   progress"); the hop re-checks `pipeline_running` and the flag under the
   lock. What remains of the old pattern: `start_scanner` doesn't take
   `settings_mutex`, so a scan started in that window is only caught by the
   re-check if it got there first; other commands in the window wait for the
   mutex and then run on the reopened devices (as before).
8. (end-to-end run) After sweeps with unequal retunes per tuner (a tuner
   subset; H not a multiple of K' - the 11-hop FM plan with 5 tuners gives
   tuner 4 a hop every round, the others 2 of 3), the return to coherent
   took 93-125 s: every channel came back with a lag of hundreds to
   thousands of samples, re-acquired one at a time inside the phase
   recalibration (~25 s each). Measured (`t_lagshift.py`, old build): 200
   full-path retunes of tuner 0 alone (independent mode, 100 / 102 MHz) ->
   every other channel's lag 399.97-400.01 samples; a 20 s sweep of tuner 0
   alone (473 rounds) -> 945.98-945.99; 200 gain changes of tuner 0 alone ->
   no lag, CONVERGED 3.1 s. So every `rtlsdr_set_center_freq` shifts that
   dongle's sample stream by exactly 2 samples (gain writes - also I2C
   through the repeater - don't). The cause, found 2026-11-21: the PLL lock
   check's I2C read of exactly 3 registers makes the RTL2832U drop 4 bytes
   of its stream - any 3-register read does, no write and no read of
   another length (`../../../docs/dev/librtlsdr-fork.md`, *A 3-register
   tuner read drops 2 samples*); the fast path reads the same 3. Tuners
   retuned together lose the same 2 samples, which is why coherent retunes
   and the wideband scan never showed it. Fix: after a sweep, or tuners retuned unequally often
   (counted per tuner, `note_tuner_retuned`; the same path serves
   independent mode - its per-tuner retunes had the same problem: 200
   retunes -> 92.9 s), the return to coherent sets `force_recalibration`:
   the watchdog's full recalibration, every lag servo in parallel as at
   startup, then the phase (not a coherence event; `--kerberos`: none, its
   restore is STALE). Equal counts (the wideband scan, a common retune) keep
   the phase-only return. KrakenSDR, new build: tuner 0 alone 40 s on the
   full range (956 rounds), then tuners 0-2 10 s, then all 5 10 s ->
   coherent CONVERGED 30.2 s; 200 retunes of tuner 0 (independent) -> 25.8
   s; 20 s of all 5 on 88-108 MHz (unequal: 278 samples on channel 4) ->
   22.5 s; 50 gain changes -> 2.8 s (phase-only); check_calibration OK
   (|lag| 0.0, |phase| <= 0.3 deg) after each, 0 coherence events. The
   phase recalibration that starts first may still log one channel's lag
   before the full recalibration takes over 0.1 s later (harmless).

**Accumulated slip** (measured 2026-11-21, the user's question: can the 2
samples per retune outgrow the +-16384-sample correlation window?). The
difference between two tuners grows without limit while they are retuned
unequally often, and the sets as they are paired then (the N-th buffer of
every dongle) do leave the window. What the calibration meets is smaller:
every flush with a quiet point re-pairs the buffers by arrival time, so the
lag starts again inside one buffer. KrakenSDR, merged build, single tuners
retuned in independent mode (14.4 ms each), then coherent (job scratch
`e2e/skewtest.py`):

| Before coherent | Lag as paired | After the recovery's flush | CONVERGED |
|---|---|---|---|
| tuner 1 x 2048 retunes | -4096.0 (peak/mean 147) | -4096 | 27.2 s |
| tuner 1 x 4096 | -8192.0 (94) | -8192 | 35.2 s |
| tuner 1 x 6144 | -12288.0 (50) | +4095.9 (146) | 29.7 s |
| tuner 1 x 9000 (18000 samples) | no peak (5.0) | -1616.1 (18000 - 16384) | 25.8 s |
| tuners 1 / 2 / 3 x 2048 / 4096 / 6144, 15 s of sweep mode, then coherent | - | +12287 (34), +8192 (95), +4096 (148), 0 | 39.6 s |

Each: check_calibration OK (|lag| 0.0, |phase| <= 0.3 deg). The servo pulls
the lag in on the sample clock, which also puts that dongle's buffers back
in step with the others - after CONVERGED nothing of the slip is left.
Two limits the last row shows, both where the dongles' buffer arrivals are
spread over more than half a period (several tuners thousands of retunes
apart), so that NO quiet point exists:
- Sweep mode itself slows down: every hop's L1 flush waits its four packet
  periods and flushes unpaired - 81 ms a round against 47 ms from an aligned
  array (the same 10-step plan), 191 unquiet flushes in 15 s, the "no quiet
  point" line every 10 s. The data stays valid (the wait only adds settle).
- The way back cannot re-pair (`clear_l1_buffer_paired` fails three times):
  it takes the coherence-loss path (one coherence event counted), and the
  pairing its flush leaves is whatever arrives next - lags up to a whole
  buffer, here 12287 samples (25 % of the window overlapping, still a
  peak). A channel that lands within a few hundred samples of +-16384 has
  no peak: the servo never locks, `lag_no_peak` warns after 60 s, the
  recovery is given up after 120 s, and it stays uncalibrated until a Full
  Recalibration (by then the other channels' servos have re-aligned their
  buffers, so that flush finds its quiet point). Not seen in these runs;
  how often it happens is not measured.
How fast the slip builds: a sweep with one tuner ~46 samples a second
against the idle tuners (half a buffer in 3 minutes); the 11-step FM plan on
5 tuners ~14 a second for tuner 4 (10 minutes); the full range on 5 tuners
one retune a sweep (hours); coherent mode and the wideband scan none.
Not changed: the cure is in the library - a lock check that does not read
exactly 3 registers costs no samples at all (`../../../docs/dev/
librtlsdr-fork.md`, *A 3-register tuner read drops 2 samples*).

**The library fix and the way back to coherent** (2026-11-21). The fork
branch `lock-read-no-slip` makes no control IN of 3 mod 4 bytes (the cause:
any such IN drops 4 bytes of the stream) and defines `RTLSDR_NO_STREAM_SLIP`
(`../../../docs/dev/librtlsdr-fork.md`, *The rule, and the fix*). heimdall
(`set_operating_mode`, sdr_init.cpp): built against a library with the macro,
the return to coherent after a sweep or after unequal retunes
(`retunes_unequal`) no longer forces the full recalibration - the lags are as
they were, so the phase-only return (cooldown, noise on, phase snapshots)
runs, logged "Mode: tuners retuned ... - the library keeps the streams in
step: phase recalibration". What stays: the quiet-point re-pairing after a
sweep (`clear_l1_buffer_paired`, else the coherence recovery), the lag check
of the phase recalibration (`check_lag_during_phase_recal`: a channel more
than 1 sample off goes back to its servo) as the net, `--kerberos` (STALE
after any retune outside coherent, unchanged), and the retune counters (only
logged now). Built against a library without the macro (the Dockerfile's
pinned 578b43d, an older clone) heimdall behaves as before (the forced full
recalibration). Measurements: below (*Return-path measurements*).

*Return-path measurements* (2026-11-21, the KrakenSDR, x86_64 PC, clean
test builds against the fixed library and against the fork's master without
the fix; job scratch 52196024 `fork/htest/`, `fork/out/q6.out`; seconds to
CONVERGED from the mode reply, `t_lagshift.py`):

| before coherent | fixed library | without the fix |
|---|---|---|
| 200 retunes of tuner 0 alone (independent) | 3.1 s, no lag logged | 26.9 s (full recalibration; channel 1 logged 399.998 samples) |
| sweep, tuner 0 alone, 20 s (446 rounds) | 2.9 s | - |
| the orchestrator's sequence (tuner 0 40 s full range, 0-2 10 s, all 5 10 s) | 2.9 s | 28.5 s (1648.01 samples) |
| all 5 on 88-108 MHz 20 s (11 hops: uneven) | 2.9 s | - |

`t_uneven.py` (sweep 20 s, then coherent; seconds from the command): the
11-hop FM plan on 5 tuners 1.1 s, tuners 1 and 3 on 24-1765 MHz 1.1 s, an
even 10-hop plan 1.1 s - each "full recalibration: no", no lag logged,
check_calibration |lag| 0.0 / |phase| 0.1-0.2 deg, l1_unquiet_flushes 0,
coherence_events 0. The same without the fix: 19.2 s (full recalibration,
channel 4 logged -267.991 samples) and 25.3 s (channel 1 -890.004). And
8.5 min of the plans cycling every 20 s (the 11-hop FM plan on 5 tuners,
tuners 1,3 on 24-1765 MHz, 0,2,4 on 400-470 MHz, tuner 2 alone on the FM
band, all 5 on 24-1765 MHz; 26 blocks, ~11000 rounds, round_ms 41-47, 0
unquiet flushes, 0 failed retunes) -> coherent 1.1 s, no lag, check |lag|
0.0 / |phase| 0.2 deg. Coherent start CONVERGED 19.4-22.0 s, `t_basic.py` all
PASS; drags (`t_drag.py`): 200 steps of 25 kHz 50 ms apart (reply median
14.4 ms), 60 steps across 109.2 / 109.8 MHz, 40 jumps 100 <-> 433 MHz - each
CONVERGED 2.5-3.4 s after the last step, no lag logged, check |lag| 0.0.
`skewtest.py` (the *Accumulated slip* table's cases, fixed library): tuner 1
x 2048 / 4096 / 9000 retunes (independent, 14.3-14.6 ms each) -> coherent
1.6 s each (were 27.2 / 35.2 / 25.8 s), no servo engaged, no "no quiet
point"; the spread (tuners 1 / 2 / 3 x 2048 / 4096 / 6144) then 15 s of
sweep mode: round_ms 54 (81 before: no unquiet flush now, 191 before),
0 unquiet flushes, back 1.6 s (39.6 s before); the same sweep from an
aligned array: round_ms 53, back 1.6 s. Every check |lag| 0.0, |phase|
0.1-0.3 deg, coherence_events 0. Positive control, the build without the
fix, case A: channel 1 at -4096.0 samples, the full recalibration, 27.7 s.
The first heimdall worker's regression set on the fixed build: `t_basic`
every case PASS (coherent CONVERGED 22.0 s, check OK); `t_contrast` (settle
0 / 5 ms) first valid packets' worst segment +3.7 / +3.6 dB against later
packets' max +4.2 / +4.4 and flagged ones up to +16.7 / +17.1 dB (81 / 74
with the old hop in them); `t_cmds`, `t_web` 0 failures; `t_modes` 0
failures - sweep -> coherent CONVERGED 2.1 s after the switch (22.6 s with
the forced full recalibration), mid-calibration -> sweep -> coherent 4.1 s,
final 1.0 s, every check |lag| 0.0; `t_long` (3 min, full range, 5 tuners,
heimdall's sweep still on today's full-path hop): 21 sweeps of 7.5 s, 0
failed retunes, 0 coherence events, 0 unquiet flushes, 0 valid packets off
the grid (7006 valid), RSS flat 42.0 MB, SIGTERM during a hop exit 0 in
0.32 s.

Nits: `whole()` takes the JSON number grammar only (not `12abc`, `0x10`,
`+5`, `1e2x`), `run` needs a delimiter after the literal (not `truex`);
`set_sweep` bumps `plan_generation` before zeroing `round` / `sweeps`
(under `settings_mutex` + the plan mutex, and the thread stores a round /
counts a sweep only with the generation unchanged under the plan mutex);
`sweep_state.rounds` removed; the `kp == 0` branch settles and clears the
flag; the settings file is noted only for a change it holds (not `run`, not
a no-op); a failed gain rollback in sweep mode no longer calls
`signal_coherence_lost`; the exits log "Mode:" / "<mode> mode ended". Left:
a key that first appears inside a string value is taken as absent (the
parser is the port's find-based one; set_sweep has no string fields);
`tcp_data_server` reading the mode before the frequencies - after sweep ->
coherent the next packets carry the recalibration's phase state (not
CONVERGED, noise on) and `return_to_coherent_locked` flushes L2, so nothing
uses those packets; `frac_tail` noted at the guarantee.
Finding 7 on the fake (`t_race.py`, `set_num_elements` + `set_operating_mode
sweep` in one TCP send, 10 tries): when the reconfiguration started, either
it aborted on its re-check under the lock ("switch to coherent mode ...",
the count unchanged, sweep mode on) or the mode change was refused
("Element-count reconfiguration in progress", the new count, coherent) -
never both. Regression set on the KrakenSDR after the fixes (`regress.sh`):
T1 CONVERGED 22.6 s, check OK (|lag| 0.0, |phase| 0.2 deg); T2 every case
PASS; T3 (`t_contrast`, station 14.4 dB over the floor) first valid packets
worst segment +4.1 / +3.6 dB at settle 0 / 5 against later packets' max
+3.9 / +3.4 and flagged packets up to +16 dB (81 / 75 flagged packets with
the old hop in them) - noise level, no trace of the old hop; T5 every check
PASS; the web STATE / SDR_SETTINGS PASS; T6 every check PASS (60 s of
sweeping, 429 sweeps, 0 coherence events; sweep -> coherent CONVERGED 22.6
s, check OK; coherent -> sweep mid-calibration -> coherent 20.6 s); 3 min
full range with 5 tuners: 23 sweeps, 0 failed retunes, 0 coherence events,
0 valid packets off the grid, RSS flat 43.2 MB, SIGTERM during a hop exit 0
in 0.72 s.

**The failed return on the Pi** (2026-11-21, the Raspberry Pi 5 Rev 1.1,
8 GB, KrakenSDR, the fork with `RTLSDR_NO_STREAM_SLIP`). In `t_exitstress.py
--n 10` (sweep mode left at a random point of a hop, settle 5), return #2
left channel 1 with no correlation peak against channel 0. The first lag
readings came during the 500 ms noise wait right after the re-pairing flush
and the full retunes ("lag 361.006 samples ... (sample slip)" at peak/mean
5.7; a real lag reads 150-190). Then 60 s of servo readings scattered over
-4711 .. 2610 at peak/mean ~5, and `lag_no_peak`. The array stayed
uncalibrated until the next mode change, whose paired flush repaired it.
The log has no USB error, no L1 overflow, no unquiet flush and no coherence
event. The other 9 returns were clean.

The root cause is not known. It has not happened again: 1 failure in 590
returns on the Pi (95 % interval 0.004-0.9 % per return). The other 580
were the probe build, net off: 30; the net build: 2 x 30; and the long soak,
`t_exitsoak.py`, 3 holds of 8.5 min. In the soak, 520 of 520 were CONVERGED
1.6 s after the command, with 105 calibration checks all |lag| 0.0, the net
never fired, and l1_put was equal after every return; 46-49 C, throttled
0x0, 9-15 % per core. On x86 there have been none: 35 earlier returns,
20 + 40 now. What the measurements rule out:
- The library: worker F ran this exit on the bare library 50 times (all
  five sweeping in parallel, finish, full retune, lags over +-131072
  samples). 0 lag changes of 1 sample or more, delivered buffers equal,
  every bulk transfer COMPLETED.
- Spread buffer boundaries (a quiet point rare on the Pi). Over the sweeps,
  max - min of the five dongles' latest delivery: median 75 us, p90 105,
  max 164 on the Pi; 84 / 137 / 345 us on x86 (period 6827 us).
- `rtlsdr_reset_buffer` while streaming: it is called only at init (and
  not at all with HELD_STREAM_START).

The offset was already there right after the paired flush, and a later
quiet flush repaired it. So it is in the pairing (one L1 a packet deeper
than the others), not in a stream. Two mechanisms fit. One: a USB transfer
lost after the flush. The library neither delivers nor resubmits a bulk
transfer that ends ERROR / STALL / OVERFLOW, and logs nothing (worker F;
never seen in F's runs). Two: the flush cut while one reader ran >= P/2
late for 2+ packets. `flush_l1_now`'s quiet test then sees every latest
arrival within P/2 and steady gaps, but that dongle's latest packet is a
round older. The two can be told apart: a lost transfer leaves that dongle
a packet short of the others for good, while a bad pairing cut leaves the
counts equal. The status now carries `sweep.l1_put` (packets delivered into
L1 per dongle) and `sweep.arrival_phase_us`, and when the net fires its log
line gives both the counts against channel 0 and the deepest L1 over 8 ms.
`t_exitstress.py` prints the counts after every return. The next
occurrence will be classified from these.

The safety net (`calibration.md`, *Lag check during the phase recal*, *No
peak at all*) handles either kind: a channel with no peak in the phase
recalibration raises a coherence loss, and the recovery's flush re-pairs.
Forced offset on the fake (channel 1 loses one packet as the noise comes on,
`FAKE_DROP1_FILE`): the build before it showed `lag_no_peak` [1] and never
recovered. With the net, x86 recovered 5 of 5 and the Pi 4 of 4 on their
own, CONVERGED 5.2-5.5 s after the switch, check |lag| 0.0, one coherence
event each, with log lines such as "packets delivered vs channel 0: 0 -1 0 0
0; deepest L1 over 8 ms: 0 0 1 1 1". Clean returns raise no event. `t_hop`
is unchanged (FM K=1..5: 20.5 / 27.2 / 34.1 ms at dwell 5 / 10 / 20).

**On the Raspberry Pi 5** (2026-11-21, round 4 of the sweep work; job
scratch 52196024 `speed/pi/`). The board: a Raspberry Pi 5 Model B Rev 1.1,
8 GB, kernel 6.18.34, the KrakenSDR (5 x 0bda:2838) behind the board's hub
on a USB 2 port. Clean test builds with the ports moved, against the fork
with `RTLSDR_NO_STREAM_SLIP`; heimdall at the defaults (dwell 5, settle 2).
Every run 43-52 C, `vcgencmd get_throttled` 0x0, load 0.0-1.8. Beside each
figure the x86_64 PC's with the same KrakenSDR model (load 6-8: other
workers' builds).

*Hardware.* `t_hop.py --cases fm` (the FM plan: 88-108 MHz, 11 hops, so
K = 1 / 2 / 4 tune the same-band blocks, K = 3 and 5 have the longer
tunes), ms per round, K = 1..5:

| dwell / settle | Pi 5 | PC |
|---|---|---|
| 5 / 2 | 21.8 / 20.5 / 27.2 / 20.5 / 29.3 | 21.7 / 20.5 / 23.9 / 20.4 / 25.0 |
| 10 / 2 | 29.6 / 27.3 / 34.1 / 27.3 / 36.5 | 28.6 / 27.3 / 30.8 / 27.2 / 32.1 |
| 20 / 2 | 36.7 / 34.1 / 40.9 / 34.1 / 42.9 | 35.2 / 34.1 / 37.4 / 34.1 / 38.7 |
| 5 / 5 | 28.3 / 27.2 / 30.8 / 27.2 / 31.7 | 23.2 / 21.2 / 27.9 / 20.6 / 29.4 |
| 10 / 5 | 35.1 / 34.1 / 37.2 / 34.1 / 38.5 | 29.7 / 27.4 / 34.2 / 27.3 / 36.5 |
| 20 / 5 | 42.0 / 40.8 / 44.2 / 40.7 / 45.0 | 36.6 / 35.1 / 41.6 / 36.3 / 43.0 |

The tune calls are the PC's. Pi: K = 2 / 4 7.62-7.68 ms, K = 1 9.2-9.4,
K = 3 12.5, K = 5 14.1-14.2. PC: 7.5-7.8, 9.4-9.6, 12.1-12.7, 13.8-14.4.
The Pi's packets arrive later: first_valid 7.9 ms against 7.1-7.2. The gate
takes 2.7-5.4 ms at settle 2 and 4.7-6.6 at settle 5, against 1.7-3.6 and
0.4-3.3 on the PC. Those ~0.7 ms push the hop past a packet boundary in two
places. At settle 2, K = 3 and 5 cost +3.3 / +4.3 ms a round (dwell 5);
K = 1 / 2 / 4 lose nothing. At settle 5, K = 1, 2 and 4 cost a whole
period (+5.1 / +6.0 / +6.6 ms). So on the Pi, settle 5 is a packet period
slower than settle 2 for the common case, one more reason for the 2 ms
default.

Full range 24-1765 MHz timed at dwell 5 / 10 / 20 (settle 2): 5 tuners
4.02 / 5.28 / 6.52 s, 1 tuner 18.93 / 25.15 / 31.37 s. The PC: 4.11 / 5.58 /
6.86 and 18.95 / 25.17 / 31.42 s, at settle 5, the default then.

CPU: the sweep run 21.1 % of a core (PC 12.6 %). Per thread
(`t_cpu_threads.py`, 20 s per mode, % of a core):

| mode | total | convert | each rtl-rx | sample-drain | per core |
|---|---|---|---|---|---|
| coherent | 37.1 | 32.6 | 0.8 | 0.4 | 8-12 % |
| independent | 36.2 | 31.6 | 0.8 | 0.4 | 8-12 % |
| sweep | 31.8 | 25.5 | 0.9 | 0.8 | 8-12 % |

On the PC only the totals were taken.

`t_modes.py --sweep-seconds 60`: 0 failures, 470 sweeps, 0 coherence
events (PC 447 sweeps). Sweep -> coherent (phase-only, no PLL lines):
reply 0.61 s, CONVERGED 2.1 s after the switch, check |lag| 0.0 / |phase|
0.2 deg (PC 0.61 / 2.1 s, 0.0 / 0.1 deg). Mid-calibration and back 3.5 s
(PC 4.0), final 1.0 s (PC 1.0). The coherent start CONVERGED in
18.8-23.3 s (PC 19.4-22.0).

`t_long.py --minutes 9 --dwell 5` at the defaults (full range, 5 tuners):
134 sweeps of 3.75-4.29 s, 24583 valid / 29942 flagged packets, 0 off the
grid, 0 failed retunes, 0 coherence events, 0 unquiet flushes, RSS
38.7 MB flat, CPU 22.3 % of a core, 5-8 % per core, 45.5-49.9 C; SIGTERM
during a hop: exit 0 in 0.41 s. The PC, then at settle 5: 130 sweeps of
4.30 s, RSS 42.0 MB, CPU 14.6 %, exit 0.27 s. t_long does not print the
gate counters (`gates_failed`, `gates_late`) or `stalled_tuners` /
`slow_tuners`, so this run has no figure for them.

`t_contrast.py` (tuner 0, gain 29.7) on the two narrow carriers the
413-419 MHz scan found, 416.59 MHz (+42 dB) and 415.64 MHz (+46 dB), steady
over two scans an hour apart. A sits 0.6 MHz below the carrier and B = A + 2
MHz sees only the floor there; the carrier stands 31.2 / 36.2-36.4 dB over
B's floor. Valid packets show no trace of it. The first valid packet's
worst segment is +3.8 / +4.2 / +3.8 dB at settle 0 / 2 / 5 (416.59 MHz) and
+4.3 / +4.3 / +3.6 dB (415.64 MHz), against later packets' max of +3.9 to
+4.7. Segment 0's p90 is +1.5 to +1.7 dB, and no segment exceeds the later
max. The flagged packets carry the carrier at up to +31.4 / +36.7 dB. The
no-gate positive control (the gate replaced by "done") is clean too: first
valid worst +3.8 dB, segment 0 p90 +1.5. Why: the tune call (7.6 ms) is
longer than a packet (6.83 ms). The packet that straddles the retune's end
holds only samples from inside the tune, where the PLL moves and no
carrier shows. Samples from before the tune can only be in a packet that
ended at least 0.8 ms before the retune's end, and that packet has arrived
before the hop clears L1. So the KrakenSDR's hardware cannot exercise the
gate's bound ("retune end + settle - the fastest USB delivery"); the fake
does (`t_fakeguard.py`, below). The return to coherent: *The failed
return on the Pi* above.

*Software, the fake librtlsdr on the same Pi* (the net build, 5035cd2).
Green: `t_basic` (coherent CONVERGED 2.2 s, PC 2.1; T2 34.1-34.3 ms a
round, PC 34.1), `t_cmds`, `t_modes`, `t_fixes`, `t_restart`
(61.0 ms a round, PC 58.0), `t_generic` / `t_kerberos` (20.5-20.6 ms),
`t_long` 1 min (CPU 38.5-42.1 %, PC 21-37 %), `t_stall`, `t_stall3`
(77 valid packets in 3 s while tuner 3 loses packets; PC 96-101),
`t_finish` rc 0 / 1 (CONVERGED 1.1 s), `t_exitstress` 10 / 10,
`t_netfake` and `t_deadnoise` (also with `--kerberos`). `t_race` shows the
known BOTH artefact, as on the PC. `t_fakeguard`, contaminated packets:
- garbage for 4 ms after the tune call, every other packet 2.7 ms late, at
  settle 0 / 5 / 6 / 8: 105 / 7 / 0 / 0 (PC 94 / 1 / 0 / 0);
- a stalled reader's backlog delivered in bursts, settle 5: 0 / 0 / 3
  (backlog 32 / 40 / 32 packets, bursts 500 / 500 / 4000 us apart). PC
  (32 packets, 4000 us): 2 at settle 5, 1 at settle 10;
- garbage for 8 ms, settle 0 / 6 / 10: 195 / 0 / 0 (PC 196 / 0 / 0).

Fake hop timing (`t_hop.py --cases fm`), ms per round at dwell 5 / 10 / 20,
the same for K = 1..5 unless split:

| fast tune / settle | Pi 5 | PC |
|---|---|---|
| 7.5 ms / 2 | 20.4 / 27.2 / 34.1 | 20.5 / 27.2 / 34.1 |
| 7.5 ms / 5 | 27.1 / 34.0 / 40.7 | 23-26 / 32.6-33.6 / 39.8-40.7 |
| 10 ms / 2 | 20.9 (K = 1), 26.5-27.3 (K >= 2) / 34.1 / 40.7-40.9 | 21.5-22.8 / 27.9-31.4 / 34.5-36.4 |
| 10 ms / 5 | 27.2 / 34.0 / 40.7 | 27.1 / 34.0 / 40.8 |

At 7.5 ms / settle 2 the gate takes 2.6-2.8 ms and first_valid is 8.0 ms
(PC 2.8-3.3 and 7.4-7.8). Fake CPU, coherent / independent / sweep: 47.8 /
46.4 / 44.4 % of a core (PC 37.8 / 35.7 / 36.9). The fake generates its
samples in the reader threads (5.5 % each), so its readers' share is not
the hardware's.

Realtime scheduling as the Pi gives it: the test user has `ulimit -r` 30.
The five readers run SCHED_RR 20, the sample drain SCHED_RR 15, the
conversion worker SCHED_OTHER, and `mlockall` (on fault) succeeds
(`configuration.md`).

A second board, a Pi 5 Rev 1.0 (4 GB, no KrakenSDR), ran the fake set
parts A-C before the round moved to the board above: all pass, `t_long`
2 min CPU 44-47 % of a core (PC 26-37 %).
