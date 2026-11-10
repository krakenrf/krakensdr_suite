# KrakenSDR Wideband (downconverter) variant in heimdall

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

**KrakenSDR Wideband (downconverter) variant (`--wideband` / `-w`):**
- Hardware variant with per-tuner mixers driven by ONE shared LO: an on-board
  synthesizer speaking the Othernet moRFeus USB HID protocol (`10c4:eac9`,
  custom "WB fw" firmware; driver in `src/sdr/downconverter.cpp` - hidraw /
  Windows HID, no extra library). SET/GET frequency are implemented and the
  LO output is always on. heimdall programs the LO drive current (`WB_LO_CURRENT` default
  3; runtime `set_lo_current` control command, 0-7, surfaced as the "Cur"
  selector in the client UI next to Mix) and the frequency; the stock-moRFeus
  generator-mode command is not sent (unimplemented on this firmware, which
  answers unknown functions with an ident string). A current change is
  common-mode across channels (one shared LO) so it triggers no phase recal.
  Malformed HID frames wedge the parser until a power cycle - reports must be
  exactly 17 bytes with a leading 0 report-id byte.
- **Board MCU commands and Flash wear (2026-11):** the "moRFeus" is the board's
  EFM8 MCU (firmware repo KrakenRF/fw-krakendown), driving an RFFC5071. Its
  commands: `'r'` read, `'s'` set in RAM, `'w'` set AND save to the settings
  Flash (a ring of 8 x 64-byte pages, ~160k saves in the board's life).
  heimdall tunes and sets the drive current with `'s'` only (one ~6 ms round
  trip; `'w'` was ~11 ms and spent the Flash on every retune). One `'w'` of
  the LO frequency and current is sent at a clean exit (`downconverter_close()`
  from `release_hardware()`, `WB_SAVE_LO_ON_EXIT`) so the board powers up at
  the last LO; the firmware skips the save when nothing changed. If a
  firmware ever rejects `'s'`, the driver falls back to `'w'` and says so
  (status `fw.set_cmd`).
- **Replies are read:** every command is answered with a 16-byte input report
  `[cmd][addr][8 data bytes]` (or "Invalid param."). `query_locked()` writes
  the report and waits up to `WB_HID_REPLY_MS` for the matching reply,
  skipping stale ones (the queue is drained when the device is opened:
  non-blocking reads on hidraw, `HidD_FlushQueue` on Windows). A missing reply
  to `'s'` is counted (`fw.no_reply`) but not fatal.
- **Firmware probe:** `'r'` 136 at start: low word date code, high word build
  number; "Invalid param." = older than fw20261109. Older firmware saved its
  Flash on EVERY `'r'`, so below `WB_MIN_SENSOR_FW` nothing is read and the
  telemetry stays off (status `telemetry.enabled:false`, a banner in the client).
- **Board telemetry:** a detached poller thread (`WB_TELEMETRY_PERIOD_MS`,
  1 s) reads INA219 shunt + bus (address 80, register in data[0]), the
  humidity sensor (SHT4x 81 / 0xFD on Testunit_3v2, Si7020 82 / 0xF5 on 3v0,
  CRC checked) and the LO back ('r' 129). It only try-locks the HID mutex, so
  a retune waits for at most one query (Si7020: ~22 ms). Board revision =
  the sensor that answers, cross-checked with the shunt voltage window
  (10 mOhm: 4-35 mV, 100 mOhm: 40-350 mV) like the manufacturing test;
  `WB_R_SHUNT_MOHM` forces R6. The INA219 saturates at 320 mV (3.2 A on
  3v0): `iin_saturated`. Alarms (hysteresis, a level clears after 30 s back
  in range): current > 3.0 A warn / > 3.25 A alarm (3v0: saturated = alarm),
  voltage < 4.6 / < 4.4 V, temperature > 60 / > 70 C, humidity > 80 % warn,
  3 failed reads in a row = stale. Status JSON `downconverter.fw` and
  `downconverter.telemetry` (`downconverter_status_json_fields()`), one line on
  the terminal dashboard. kraken_doa relays the object as
  `system_status.wb_telemetry` (`src/wb_board.cpp`) for the "Wideband Board"
  sidebar card / top-bar chip / alarm banner, and logs it to `wb_logs/`.
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

