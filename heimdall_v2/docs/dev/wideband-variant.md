# KrakenSDR Wideband (downconverter) variant in heimdall

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

**KrakenSDR Wideband (downconverter) variant (`--wideband` / `-w`):**
- Hardware variant with per-tuner mixers driven by ONE shared LO: an on-board
  synthesizer speaking the Othernet moRFeus USB HID protocol (`10c4:eac9`,
  custom "WB fw" firmware; driver in `src/sdr/downconverter.cpp` - hidraw, no
  extra library). SET/GET frequency are implemented and the LO output is
  always on. heimdall programs the LO drive current (`WB_LO_CURRENT` default
  3; runtime `set_lo_current` control command, 0-7, surfaced as the "Cur"
  selector in the client UI next to Mix) and the frequency; the stock-moRFeus
  generator-mode command is not sent (unimplemented on this firmware, which
  answers unknown functions with an ident string). A current change is
  common-mode across channels (one shared LO) so it triggers no phase recal.
  Malformed HID frames wedge the parser until a power cycle - reports must be
  exactly 17 bytes with a leading 0 report-id byte.
- Tuners are parked at `WB_VARIANT_IF_HZ` (1268 MHz, the hardware IF filter);
  `update_sdr_settings()` maps a frequency change to an LO reprogram via
  `wideband_retune_rf()` (sdr_init.cpp), which every retune path uses
  (control port, web UI, scanner). The mixer side (`MixerSide`) places the
  LO: `LO = IF + RF` (high, RF 24-4132 MHz), `LO = IF - RF` (low, RF
  24-1183 MHz) or `LO = RF - IF` (below - LO under the RF, RF 1353-6668 MHz,
  the only side past the 5400 MHz synthesizer ceiling). The side follows the
  frequency: kept while it can reach the RF, else auto-selected
  (`downconverter_auto_side`: high up to its ceiling, below above it).
  `set_mixer_side` is a manual override that REJECTS sides unable to reach
  the current RF. The antenna ring also follows the frequency
  (`wb_ring_for_rf`: outer < 1 GHz, center < 2.5 GHz, inner above;
  `WB_RING_*_MIN_HZ` in config.h) - `wideband_retune_rf()` throws the RF
  switches on boundary crossings. Retune validation uses the union span
  (`downconverter_rf_union_range`, 24-6668 MHz). `current_frequency` stays
  the true RF (`uint64_t`) and is what packets/status report.
- High-side injection conjugates the baseband; the conversion hot loop negates
  Q (`sdr_pipeline.cpp`) so calibration, TCP clients and DoA all see an
  upright spectrum with true RF phases. Low and below arrive upright. Side
  changes run the same cooldown -> phase-recal flow as retunes (LO
  distribution phases move with LO frequency).
- **Noise-source RF path switching (critical for calibration):** unlike the
  standard KrakenSDR, where the coupler network keeps the noise source
  permanently coupled into every channel (GPIO0 merely powers it), the
  Wideband board has RF switches between the antenna rings / noise source and
  the mixers, driven by **GPIO1–6 of the channel-0 chip** (serial 1000).
  `wideband_set_noise_path()` (sdr_init.cpp) throws them; it is called from
  `set_bias_tee_all_devices()` (the single noise on/off choke point) and once
  after device init. Without it the tuners keep looking at the antennas while
  the noise source runs, the channels share no common signal, and lag/phase
  calibration never converges. Switch patterns are ported verbatim from the
  v1 heimdall_daq_fw wideband branch (rtl_daq.c).
- Three antenna rings (0 = outer, 1 = center, 2 = inner) selected by the same
  switch bank: `downconverter.array_select`, control command `set_array`
  (reported as `downconverter.array` in status JSON). A ring change runs the
  same cooldown -> phase-recal flow as a retune (different switch/cable path).
- Mutually exclusive with `OperatingMode::WIDEBAND_SCAN` (tuner spread);
  `set_wideband_mode` is refused while the variant is active.
- Missing moRFeus is fatal at startup in this mode; non-root access needs the
  hidraw udev rule (install.sh installs `99-morfeus.rules`). GPIO switching
  needs the librtlsdr fork with `rtlsdr_set_bias_tee_gpio` (the `librtlsdr/`
  source tree at the repo root, linked statically - see *Dependencies*).

