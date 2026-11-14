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

