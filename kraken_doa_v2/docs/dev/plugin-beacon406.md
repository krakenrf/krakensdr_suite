# 406 MHz distress beacons plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**406 MHz distress beacons (`plugins/beacon406/`, README.md there):**
- Cospas-Sarsat EPIRB / ELT / PLB / SSAS from the public C/S T.001 Issue 4
  Rev.13 (Annex A bit layouts, Annex B BCH), T.012 (channel plan), T.018
  (second generation), G.005 (worked examples): `sample_rate` =
  `min_vfo_rate` 100 kHz, `fixed_freq_hz` 406.050 MHz (every 406.0-406.1
  MHz channel in the VFO; picking it tunes there), manual only, talkers, map
- `rx.cpp`: 1024-point FFT / 512 hop, gated floor per bin; a carrier =
  local max >= 10 dB, confirmed 3 of its first 4 frames, followed +-4 bins
  >= 7 dB, its bin from its strongest frame of the first 150 ms; spurs of
  a stronger burst (biphase harmonics 400 Hz x odd) not demodulated
  (`SHADOW_*`). A burst of 0.12-1.0 s is demodulated 0.62 s after its onset
  from a 2.6 s ring: mix, 401-tap +-1.5 kHz low-pass, 10 kHz (25 samples /
  bit), frequency = the burst's strongest line (16384-point FFT), bit +
  frame sync NON-coherently (|sum template x signal| - phase-free), both
  frame syncs, bit rate -1 / 0 / +1 %, carrier phase / amplitude / C/N0
  from 150..12 ms before bit 1, then integrate-and-dump half-bits with a
  mid-bit-transition timing loop and a decision-free phase loop (a biphase
  bit's mean = the residual carrier cos 1.1). Accepted: bit sync <= 2 and
  frame sync <= 1 wrong; a 3-bit BCH-1 correction only if all three bits
  were weak (|soft| < 0.8: a 4-error burst must not become a phantom ID).
  Second generation: the MEDIAN bin power of +-30 kHz around 406.05 MHz vs
  its floor (excluding carriers' bins biased a spread band low - the high
  bins got claimed), the VFO's edges (>= 40 kHz away) as a reference: on
  at +1.1 dB and > 3x the edges' rise, 0.9-1.15 s, mean rise >= -5 dB, >=
  6 of 8 sub-bands lit -> detected. `B406_WIDE_TRACE=1` (offline) prints
  the band / edge rises
- `bch.hpp`: BCH-1 (82,61) t 3 / BCH-2 (38,26) t 2 by syndrome tables
  (weight <= 2 in a hash map, weight 3 by one more bit) - exact: d >= 7 / 5.
  A long-message self-test stopping at bit 112 (T.001 Rev.13 4.5.4) =
  `tail_q` < 0.3 -> no PDF-2
- `cs406.cpp`: every protocol (user: maritime, call sign with BCD digits,
  aviation, serial user incl. 24-bit address / operator designator / TAC,
  test, orbitography, national; short messages' bits 107-112 = activation
  + emergency codes Tables A4 / A5; user-location 4'; standard location 1/4
  deg + offset; national 2' + offset (bit 110 = offsets present); RLS 1/2
  deg + offset, RLM bits, provider, TAC series; ELT(DT) identity types,
  activation, altitude band, fix age, rotating-field operator,
  cancellation), the 15-hex ID with the position defaults (standard
  ...FFBFF, national ...F81FE0, RLS / ELT(DT) 0 11111111 0 111111111),
  `mid.hpp` = the ITU MID list
- `decoder.cpp`: per 15-hex ID - bursts (normal / self-test), mean
  interval, last 12 bursts (channel, C/N0, bits corrected) in the row's
  details, position-change events; status `⚠ DISTRESS` / `⚠ SECURITY
  ALERT` (normal frame sync, not a test / orbitography protocol) vs
  `self-test` / `test protocol` / `orbitography`; ⚠ event on the first
  burst + every 5 min ("report it to the rescue coordination centre");
  self-tests "(NOT an alert)". Channel names from the T.012 raster
  (406.022 + 3 kHz, A..S). Map point kind ship / aircraft / person by type.
  Raw = FFFE2F / FFFED0 + hex. Options `doa_avg`, `keep_h`
- Generator `tools/beacon_gen.py` (every protocol, BCH, positions with
  offsets, `--test-at`, `--bit-err`, `--hex`, `--snr`, `--sgb-at` = random-
  chip DSSS of the T.018 rate), `tools/scene_five.json`, playback entry
  `beacon406-five`
- Tests (scratch t7/, numbers in the README): the T.001 Annex B / G.005
  Annex A examples decode as published (10 messages, 15-hex IDs equal), 17
  protocol round trips, 10 / 10 bursts down to C/N0 34 dBHz (6-7 / 10 at 32),
  +-1 % bit rate, no burst in 60 s of noise; scene (two bursts at the same
  moment on 406.028 / 406.040 MHz, short ELT +0.5 % rate, a self-test, an
  SGB) offline 40.0 / 130.0 / 220.0 / 300.0 / 260.0 deg, end to end through
  the LIVE kraken_doa's debug playback 5 / 5 at 0.0 deg, settings files
  byte-identical. CPU (Pi 5): 0.6 % of a core on noise, 3 % in the scene.
  Live (2026-10-15, heimdall retuned to 405.75 MHz via 8092 + restored):
  ~9 min of 406.0-406.1 MHz, nothing heard; the first SGB detector
  (band mean minus carrier bins, no reference) fired 6 times in 1.5 min on
  the receiver's own level changes - fixed as above, 0 on 3 min re-decoded.
  SGB detection 2 / 2 down to -6 dB SNR at 100 kHz
- Not decoded: second-generation messages (despreading with the public
  PRN, BCH(250,202), 23-hex ID - a follow-up), 121.5 MHz homing (swept AM
  tone - a follow-up), orbitography contents (T.006)

