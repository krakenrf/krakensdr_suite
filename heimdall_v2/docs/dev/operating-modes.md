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
5. the L1 + downstream flush (`clear_l1_buffer_quiet`: the drain's
   quiet-point flush, the same path as every other flush);
6. `retuning_in_progress` = 0, under `settings_mutex` and only if nothing
   moved `last_retune_ns` since the settle (a gain change: settle + flush
   again);
7. the dwell (`dwell_ms`), and at least until one set has gone out as valid
   (`sweep_state.valid_sets`, counted by the conversion worker when the flag
   was 0 before and after the header was built; at most
   `SWEEP_DWELL_GRACE_MS` 250 ms more) - see *Measurements* for why.
`round_ms` = 1 to 7 measured. Leaving the mode or a plan change mid-hop: the
waits end (`sweep_notify`), the retune re-checks under the lock, so the new
state is never retuned over and no tuner is left half way (a retune in
progress finishes first: `set_operating_mode` waits for `settings_mutex`).
Pausing or the pipeline stopping with the flag on: the thread finishes the
settle from `last_retune_ns`, flushes and clears the flag
(`settle_and_clear_flag`), so the flag is 0 whenever nothing hops - a stuck
1 would make every client drop every packet. A gain change in sweep mode
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
  the post-settle flush (`flush_downstream`) takes the same mutex and bumps
  the epoch, and the flag is cleared only after it. A header that reads the
  flag as 0 after hop h's clear was therefore built after hop h's
  post-settle flush, its set was dequeued after it, and its packets reached
  L1 after the drain had emptied L1 at the end of the settle. Its
  frequencies are hop h's (h + 1's are stored after h + 1 set the flag);
- a packet that reaches L1 after the flush began sampling up to one USB
  packet earlier (NUM_SAMPLES / SAMPLE_RATE = 6.8 ms) plus the USB latency;
  the drain serves the flush right after the first packet round that
  arrives after the request (and drops that round), so the first valid
  packet's samples start no earlier than about settle - USB latency after
  the retune - measured below, through the real pipeline.
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
- Not run: the Raspberry Pi, Windows (one-by-one tuning), macOS, the
  Wideband downconverter (refusal is the existing `target != COHERENT`
  check), `--kerberos` on hardware; the MinGW / zig syntax checks (no
  toolchain on this machine).

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
   through the repeater - don't); the cause inside the library / the R820T
   is not found. Fix: after a sweep, or tuners retuned unequally often
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
