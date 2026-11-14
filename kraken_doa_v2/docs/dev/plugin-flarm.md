# FLARM / OGN plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**FLARM / OGN (`plugins/flarm/`, README.md there) - v2.0, a CLEAN-ROOM rewrite (2026-10-22):**
- Why: v1.0 had been written with the OGN tracker's source
  (pjalocha/esp32-ogn-tracker - NO licence) as "documentation" and held its
  LDPC matrix verbatim. A spec writer read v1.0 + public documents (OGN
  wiki tracking-protocol page, FLARM FTD-012, nRF905) and wrote a no-code
  spec; a separate implementer wrote v2.0 from it alone; a similarity audit
  (winnowing, k 12) against v1.0, the OGN tracker and SoftRF found no
  third-party expression (0-2.4 % of fingerprints - v1.0's matrix file: 97
  %). Some likeness to v1.0's own generator / receiver idioms (same model,
  shared `uat_gen.py` template) - KrakenRF's code, not a licence issue.
  LICENCE since 2026-10-23: GPL-3.0, the plugin's own (`plugins/flarm/
  LICENSE`, README *Licence*; built without `lib/` like every plugin with a
  LICENSE). Code under GPL-3.0 or a licence compatible with it (the OGN
  project's glidernet/diy-tracker, SoftRF, ogn-rf) may be used here now,
  with its notices; code WITHOUT a licence - the author's esp32 / nRF52 /
  CubeCell / ogn-tracker / radiolib-ogn repositories - never. Still no
  decryption of FLARM's payload
- `ogn_ldpc_matrix.hpp` (GPL-3.0-only) = the protocol's LDPC(208,160)
  parity-check matrix (the code every tracker transmits) - hex rows in
  packet byte order, read by the decoder AND `tools/flarm_gen.py`. From the
  OGN project's glidernet/diy-tracker (`ldpc.cpp`, LDPC_ParityCheck_n208k160:
  48 / 48 rows identical, in the same order - checked 2026-10-23; GPL-3.0
  since 2015-06-25, its author Pawel Jalocha committed ldpc.cpp there after
  that, last in 2017). v1.0 / v2.0 until then had it from the unlicensed
  esp32-ogn-tracker (same rows)
- Files: decoder.cpp (plugin, band plans / tune, per-aircraft state,
  outputs), receiver.* (FFT burst detection, arbitration, demodulation, sync
  search), packet.* (FLARM CRC, OGN LDPC check + min-sum, TEA de-whitening,
  report decoding)
- Public interface = v1.0's except the version and one privacy fix: a
  no-track tracker's verbose event carries no altitude any more (no position
  element of a stealth tracker anywhere but the raw frame hex). The scene
  below decodes identically to v1.0 (33 valid, the five talkers 0.0 deg,
  the relay none); sensitivity = v1.0's at every point
- Glider / GA collision-avoidance radios: `sample_rate` = `min_vfo_rate`
  2.4 MHz, `lock_vfo`, `tunes` (once, when no plan channel is in the VFO:
  option `band` - auto from the station location -> 868.8 MHz for EU / NZ,
  921.6 AU / S. America, 915.2 North America, 916.6 Israel; option `tune`
  off = never), manual only, talkers, map. Channels: every plan channel
  within +-1.05 MHz of the VFO centre (868.2, 868.4, 868.8, 869.25, 866.0,
  920.9, the 902.2 + 0.4k grid of the US / AU hopping bands, option
  `extra`); no RF known (offline without --rf) = a 100 kHz grid
- Front end: 256-point FFT every 256 samples, Hann; power in +-75 kHz per
  channel vs its gated-mean floor > `sens` (6 dB); a 2-14 ms burst, decided
  4 frames after it ended (the strongest of overlapping bursts within 250
  kHz), mixed down from a 2^17-sample (55 ms) ring, 49-tap 110 kHz low-pass
  decimated to 400 kHz, fast-atan2 discriminator, summed per chip (4
  samples); sync search for both (last 56 chips of F531FAB6 / 0AF3656C,
  either polarity, <= 6 chip errors), timing +-1 sample tried. Bursts
  starting together (+-2 frames) on 3+ channels spanning >= 600 kHz = one
  broadband event, skipped (live: heimdall's calibration noise after a
  retune hit all four EU / NZ channels at once). `FLARM_DUMP=dir`: every
  demodulated burst's 400 kHz baseband + a log line (live diagnosis)
- FLARM: 26 bytes, CRC-16 CCITT (init FFFF) over 31 FA B6 + 24 bytes (also
  the inverted bits); ID bytes 0-2 LE + address type (byte 3 bits 4-5) only.
  The encrypted 20 bytes (unpublished keys) are NOT decoded - by the
  project's rules (no brute force / reverse engineering of undisclosed
  encryption)
- OGN (packet.cpp, the matrix above): hard check, else normalised min-sum
  (0.75, 16 iterations, <= 16
  bits changed); header parity checked; TEA (all-zero key, 8 rounds)
  de-whitening; position / status / info decoded (info: personal strings -
  pilot, crew, ICE, pilot ID, task - dropped); `Encrypted` = address only;
  `Stealth` = no map point, no position in table / talker / events (the
  bearing stays, "no-track"); relay > 0 = no talker
- Outputs: table (Aircraft, Protocol, type, Registration / CN, altitude,
  climb, speed, track, Lat / Lon, distance / bearing, Notes, MHz, Signal,
  Packets; details), map (trackable OGN only), events, raw `FLARM|OGN hex
  MHz dB`; table_keep 30 min
- Generator `tools/flarm_gen.py` (FLARM: real header + CRC, random bytes
  for the encrypted part; OGN: real fields, whitening, LDPC parity from the
  matrix file by GF(2) elimination; relays `same_as`; channels per packet;
  GFSK BT 0.5); `tools/scene_six.json`
- v1.0 tests (scratch t4/; v2.0 reproduces them): scene_six (2 FLARM, OGN glider, no-track OGN
  paraglider, owner-encrypted OGN, a relay; three packets at the same
  moment on 868.2 / 868.4 / 869.25): offline 44 / 44 packets, 5 talkers
  0.0 deg, the relay none; end to end 0.0 deg (285 / 289 packets with a
  bearing); UI headless Chromium (kind flarm, 🪂 / ✈, Every aircraft,
  distance + DF error, nothing of the no-track one's position).
  Sensitivity (160 kHz channel SNR): FLARM 98 % from 9.8 dB, OGN 100 % from
  7.8 dB (LDPC). CPU 2.9 % of a core live (end to end). Live (real array,
  2026-10-15 09:15-09:30, a weekday morning): no FLARM / OGN; the plugin
  retuned the receiver from 922.98 to 868.8 MHz itself (band auto: 868.8 MHz);
  a periodic narrow-FSK device near 868.166 MHz every 16 s and the
  calibration noise demodulated, nothing reported; 2.6 % of a core

