# AIS plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**AIS (`plugins/ais/`, ships):**
- Input 100 kHz (`sample_rate` = `min_vfo_rate` = 100 kHz = the VFO's "100
  kHz" bandwidth, 2.4 MHz / 24: no resampling), `fixed_freq_hz` 162.000
  MHz, manual only: one VFO holds channel A (161.975, -25 kHz) and B
  (162.025, +25 kHz). Per channel: mix, 49-tap 8 kHz low-pass decimated to
  50 kHz, FM discriminator (clamped +-12 kHz), 21-tap 5.5 kHz low-pass ->
  2 x 8 slicers (8 bit-timing phases at 5.21 samples/bit, linear
  interpolation; threshold = the slicer's running mean over ~64 bits, or
  the discriminator's mean over 48 bits centred on the bit - running sum
  `cs` - which follows scanner-audio baseline wander) -> NRZI -> HDLC
  (flag, destuffing, 7 ones = abort) -> CRC-16/X.25 -> payload bits (bytes
  LSB first on air). A frame decoded by several slicers (same channel +
  CRC, closing flag within 4 bits) is reported once
- Messages (`ais_msg.hpp`): 1-3 (class A position, status, ROT, SOG, COG,
  heading), 4 / 11 (base station), 5 (IMO, callsign, name, type, size,
  draught, ETA, destination), 9 (SAR aircraft), 18 / 19 (class B), 21 (aid
  to navigation), 24 A/B (class B static), 27 (long range). MMSI 970 / 972
  / 974 (SART / MOB / EPIRB) and status 14 -> "⚠" events (every 10 min).
  Outputs: map points (kind ship / station / point / aircraft / person,
  heading = true heading else COG, ttl 30 min), table (MMSI, name,
  callsign, type, class, status, SOG, COG, HDG, position, distance /
  bearing, destination, length, channel, signal, messages), facts, raw =
  NMEA !AIVDM (multi-sentence over 60 characters) + level. Vessels
  forgotten after 30 min. Option `range` (km, default 500)
- DoA: every frame -> `kp::Talker{packet, freq_hz = -25000 / +25000, bw_hz
  16000, avg_s 20, keep_s 1800}` (kept as long as the vessel list), span = training sequence (24 bits before the opening
  flag) .. closing flag, through the channel's filter delays (`to_time`:
  input index = 2 (t - 10.5) - 24); the spans land within 0.3 bit of the
  truth on synthetic signals
- Generator in the repo since 2026-10-15: `plugins/ais/tools/ais_gen.py`
  (message 1 per ship, channel A / B, times; GMSK BT 0.4, NRZI, HDLC, CRC)
  -> `plugins/sdk/tools/uca_scene.py` for ships at known bearings
- Tested (scratchpad e2e4/): synthetic GMSK (ais_gen.py: real layouts,
  NRZI, stuffing, BT 0.4, ships on both channels incl. simultaneous A / B
  transmissions) - every frame without a same-channel collision decoded,
  none from noise; real recordings (cu8 96 kHz two-channel ship162
  ais_96k.bin, sigidwiki 48 kHz IQ, AIS-catcher example, freerange
  rtl_fm discriminator files with aisdecoder NMEA, Long Beach / Helsinki
  scanner audio): all aisdecoder messages + more, at least the numpy
  probe's count on IQ (62 vs 70 on the 8-bit Long Beach audio)

