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
- `kp::Info::lock_vfo_opt` (2026-11-23; inmarsat_aero `"mode=all"`, All
  channels in the spectrum): the `lock_vfo` lock only while one option of
  the plugin has one value on the VFO. `--info` / the plugin list carry
  `"lock_vfo_opt":"key=value"` only when set (`PluginInfo::lock_opt_key` /
  `lock_opt_val`; a key that is none of the plugin's options is dropped).
  One answer to "is this VFO locked", from the plugin info and the VFO's
  own options (`digital_opts.plugin`, a key not there = the option's
  default): the receiver's `dig::holds_vfo` / `decoder_holds_vfo`
  (dig_plugin.hpp), the page's `vfoLockPlugin(d)` (`vfoLocked(d)` =
  `!!vfoLockPlugin(d)`), so every page and a reload agree - the lock is
  derived from the receiver's state, never from what a page clicked.
  Bandwidth of the option lock: the whole 2.4 MHz (`lock_bandwidth_hz`,
  `digLockBwHz`), not `min_vfo_rate` (Aero's is 15 kHz). Applied
  (`digApplyLock`, the bandwidth + `SET_DECIMATOR_FREQ:id:0`, the tuner
  left alone) when the user sets the option to the value
  (`digSetOptFor` -> `digOptLockChange`) or picks / switches on the
  decoder on a VFO that already has it (`digApplyFixedTune`), never on a
  settings replay; the option set to another value gives back
  `bwAtLeastEfficient(min_vfo_rate)`, the VFO staying on the centre;
  another decoder / off unlocks with the bandwidth kept, as for the
  plugin locks. Consumers: the spectrum bar (drawBox: line, no
  `fo.boxes` entry), the waterfall overlay (`drawLockedLines`), the
  independent panes (line, no `pane.bars`; the pane click), the VFO cards'
  Freq / BW / Tuner fields and their tooltip ("<plugin> (<choice>) keeps
  this VFO on its tuner's centre ..."), the VFD digits (`vfdStep`),
  `centerActiveBarAt`, `sfTune` / `sfTuneHz` (a saved frequency retunes
  the tuner, the VFO stays at 0); receiver: the AI assistant's
  `set_decoder` / `set_vfo` (`vfo_held`, `apply_lock`; `set_option`
  applies / releases the lock as the page does; `list_decoders` says
  `whole_band_when`), the saved-frequency scanner (`vfo_held_by`, and the
  plan's `decoder_why_not` with the scan VFO's options). A receiver
  without the field: never locked by an option (the plugin's narrow
  fallback tells the user); an older page with a new receiver: as before
- Test of the option lock (2026-11-23; TEST receiver + fake heimdall,
  headless Chromium, real clicks on the decoder dropdown / tick box,
  drags, wheel): Channel -> all sends DIGITAL_OPT, SET_DECIMATOR_BW_HZ
  2400000, SET_DECIMATOR_FREQ 0 (no FREQ); one line, no `fo.boxes` /
  waterfall box / pane bar, fields disabled with the tooltip; a drag on
  the line retunes the receiver as for ism, the offset stays 0; a second
  page and a reload agree; Auto -> 15 kHz, movable; off + on again and
  another decoder + Aero again re-apply it. Coherent, independent and
  wideband scan: the same commands and state as ism (and ism / ADS-B the
  same as the unchanged build). The playback entry aero-band-all-channels
  comes up locked, 3 / 3 rows. New page + old receiver and old page + new
  receiver: no option lock, 15 kHz kept, the plugin's fact asks for 2.4
  MHz, ism still locks. AI assistant: set_decoder_option applies /
  releases it, set_vfo decoder=inmarsat_aero locks with mode=all and
  ignores freq_hz, the saved-frequency scan refuses the VFO. Known, from
  before: set_vfo WITHOUT a decoder argument still resizes / moves a VFO
  its decoder holds (ADS-B and ism too)
- Test: scratchpad t_adsb_lock (coherent + independent); e2e/t_ui_wide.py
  (lock_vfo: fields + reason, the commands a pick sends - no FREQ, ADS-B as
  the control sends FREQ:1090.000 -, the marker paths, DF devices);
  t_cells_lock.py (cells: offset 0 / 2.4 MHz without a FREQ, no grab box,
  marker tune, another decoder unlocks). The cells / dtv branch called the
  same lock `kp::Info::whole_band` before the merge (2026-10-15): one name
  now; `--info` "whole_band" still reads as lock_vfo

