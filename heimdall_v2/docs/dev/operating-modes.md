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
- the bound (proof in `serve_gate_now`; *Review of the hop loop*, 1):
  every sample of a kept packet was taken at or after T - L* - 22 us,
  where L* is the latency of the packet the dongle's line runs through -
  its fastest delivery among the last 32 packets (~1 ms on a PC). A late
  or burst-delivered packet, samples the RTL2832 lost, the 2-sample slip:
  none of them moves the line later. So the samples see settle - L*
  after the tune call returned - measured below and in *Hop timing*,
  through the real pipeline.
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
1.3-1.6). The defaults are left at 20 (the contract's; kraken_doa's
`SWEEP_DWELL_DEFAULT_MS` and the page show the same) - the proposal is in
the round's report.

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
- Not run: the Raspberry Pi, Windows (tunes one by one: nothing in the hop
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
