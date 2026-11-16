# Operating modes (coherent / wideband scan / independent)

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
  `check_calibration` / `reset_lag_compensation` (8092) and CHECK_CAL /
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
- Known, not changed: in today's KrakenSDR path `restore_operating_mode()`
  reads `persisted_tuner_freq/gain` after `set_operating_mode()` ->
  `note_mode()` has overwritten them with the common tuning, so independent
  mode's per-tuner tuning is not restored on a KrakenSDR restart.

