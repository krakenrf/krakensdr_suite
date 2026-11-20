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
logged at most every 10 s; nothing calls `signal_coherence_lost`, so the
sweep never counts as a coherence loss or starts a recovery. The realtime
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
The reordering of the header's reads is invisible in coherent mode (the
same values; the discrete scanner's hops are ordered the same way and gain
the same property).

**Coherent mode** is unchanged except: the header's reads reordered (above),
the conversion worker's per-set mode load, `for_each_tuner` moved to
`for_each_tuner.hpp` (the same code), the status JSON / STATE carry a
`sweep` object, and the element-count refusal text names sweep mode.
