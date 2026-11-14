# Fixed-frequency decoders

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Fixed-frequency decoders (`kp::Info::fixed_freq_hz`, ADS-B 1090 MHz):**
- Done in the web UI (kraken_doa.html, `digApplyFixedTune`, from
  `digSetModeFor` / `digToggle` - i.e. when a user picks the decoder, not on
  a settings replay): narrowest bandwidth >= `min_vfo_rate`
  (`setDecimatorBandwidthFromUI`), `SET_DECIMATOR_FREQ:id:0` (a queued drag
  dropped first), then the tuner through the normal paths - `paneSendFreq`
  (TUNER_FREQ, the VFO's tuner) in independent mode, `sendFreqChange`
  (FREQ / WIDEBAND_FREQ) otherwise
- `vfoLocked(d)` (the VFO's decoder is such a plugin): `drawLockedVfoLine`
  draws one solid line + "D<n> <plugin name>" in drawBox, the waterfall
  overlay (`drawLockedLines`) and the panes; the VFO gets NO entry in
  `fo.boxes` / `waterfall.overlay.boxes` / `pane.bars`, so it can't be
  grabbed, resized or squelch-dragged; `centerActiveBarAt` and the pane
  click don't move it when selected; `digRenderCards` disables its Freq / BW
  / Tuner fields. Picking another decoder unlocks it (bandwidth stays)
- `kp::Info::lock_vfo` (ism, radar, cells, dtv; `--info` "lock_vfo", `PluginInfo::lock_vfo`,
  the plugin list's `lock_vfo`): the same lock with no frequency - the
  bandwidth + `SET_DECIMATOR_FREQ:id:0`, the tuner left alone
  (`digFixedPlugin` returns such plugins too, `digApplyFixedTune` skips the
  retune). Fields' tooltip: "... keeps this VFO on its tuner's centre with
  the full bandwidth - tune the receiver instead". `sfTuneHz` (a saved
  frequency - sidebar, spectrum tag, 📍 marker) on such a VFO retunes the
  tuner (FREQ: whole kHz, VFO stays at 0) where a fixed-frequency one
  refuses
- Test: scratchpad t_adsb_lock (coherent + independent); e2e/t_ui_wide.py
  (lock_vfo: fields + reason, the commands a pick sends - no FREQ, ADS-B as
  the control sends FREQ:1090.000 -, the marker paths, DF devices);
  t_cells_lock.py (cells: offset 0 / 2.4 MHz without a FREQ, no grab box,
  marker tune, another decoder unlocks). The cells / dtv branch called the
  same lock `kp::Info::whole_band` before the merge (2026-10-15): one name
  now; `--info` "whole_band" still reads as lock_vfo

