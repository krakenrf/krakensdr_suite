# flarm - FLARM / OGN trackers (868 / 915 MHz)

The collision-avoidance and tracking radios of gliders, tow planes,
paragliders, helicopters and light aircraft: **FLARM** (shown by its radio
ID and bearing - its position is encrypted and is not decoded) and **OGN
trackers** (the Open Glider Network's open protocol: position, altitude,
climb, speed, track, aircraft type, registration). Every aircraft gets its
own bearing in the 🗺 Map's 📡 DF panel.

Pick "FLARM / OGN trackers (868 / 915 MHz)" as a VFO's digital decoder. The
VFO goes to its tuner's centre with the full 2.4 MHz (`lock_vfo`) and every
FLARM / OGN channel inside it is received at once. Manual only. Version 2.0
is a clean-room rewrite of 1.0 from a specification (same interface, same
outputs - see *Tests*).

## Bands and channels

Every plan channel within ±1.05 MHz of the VFO centre is received, whatever
the `band` option says:

- 866.0, 868.2, 868.4, 868.8, 869.25 and 920.9 MHz, plus the grid
  902.2 + 0.4·k MHz (k = 0..64) of the hopping bands.
- At **868.8 MHz** the VFO holds 868.2 and 868.4 (Europe's two alternating
  FLARM / OGN channels), 868.8 and 869.25 (New Zealand's single channel).
- In the hopping bands only the channels inside the 2.4 MHz are heard: 921.6
  MHz gets 920.6 - 922.6 (Australia / South America), 915.2 MHz 914.2 - 916.2
  (North America), 916.6 MHz 915.8 - 917.4 (Israel's 916.2 among them).

When the RF is known and no channel is inside the VFO, the receiver is
retuned **once** to the band (option `tune` off = never):

| band | tuned to | |
|---|---|---|
| eu | 868.8 MHz | Europe / Africa / New Zealand (868.2, 868.4, 869.25 MHz) |
| nz | 868.8 MHz | New Zealand (869.25 MHz; 868.2 / 868.4 too) |
| au | 921.6 MHz | Australia / South America (917-926.2 MHz hopping) |
| us | 915.2 MHz | North America (902-928 MHz hopping) |
| il | 916.6 MHz | Israel (916.2 MHz) |

`auto` (the default) picks it from the station's location: New Zealand
(−50..−30° lat, 164..180° lon) nz; Australia (−45..−9°, 110..160°) au; the
Americas (−170..−30° lon) au south of 20° S, else us; Israel (29..34° lat,
34..36° lon) il; anywhere else, or no station, eu. Without a known RF
(an offline test without `--rf`) the plugin watches a window every 100 kHz.

## Options

| key | | |
|---|---|---|
| `band` | Band | `auto` / `eu` / `nz` / `au` / `us` / `il`: where to tune when no channel is inside the VFO (a change allows one more tune) |
| `tune` | Tune to the band | `1` on (default) / `0` off |
| `sens` | Burst detection | how far (dB, 3..20, default 6) a channel's power must rise above its noise floor to demodulate a burst |
| `extra` | Extra channel MHz | one more channel (MHz, inside the VFO) of another band plan; within 1 kHz of a plan channel it is that channel |

The SDK's `verbose` option logs every packet.

## Radio formats

Both protocols share one physical layer: GFSK, Gaussian BT 0.5, ±50 kHz,
100 kchip/s Manchester = 50 kbit/s (IEEE / 802.3 convention: a data bit 1 is
chips "0 1", a 0 is "1 0"; chip 1 = the +50 kHz tone). A preamble, a 32-bit
sync word, then 26 bytes MSB first (~4.96 ms on air). The receiver matches
the sync word's last 28 bits.

**FLARM** (legacy format, an nRF905 ShockBurst payload): sync `F531FAB6`
(the nRF905 address `31 FA B6` after the end of its preamble).

| bytes | |
|---|---|
| 0-2 | 24-bit radio ID, little endian |
| 3 | address type in bits 4-5 (0 random, 1 ICAO, 2 FLARM, 3 OGN); the rest is not interpreted |
| 4-23 | encrypted with keys FLARM has not published - **not decoded, not analysed** |
| 24-25 | CRC-16 (poly 0x1021, init 0xFFFF, no reflection, no final XOR = CRC-16/IBM-3740) over `31 FA B6` + bytes 0-23, MSB first |

A packet whose 26 bytes are all complemented (a radio whose data is
inverted against its sync) is accepted too and reported "(inverted)".

**OGN tracker** (OGNTP v1): sync `0AF3656C`. Bytes 0-19 = five 32-bit
little-endian words W0..W4, then 6 bytes of LDPC parity.

- **W0, the header** (never whitened): address (bits 0-23), address type
  (24-25: random / ICAO / FLARM / OGN), non-position report (26), parity
  (27: bits 0..27 even - a packet failing it is dropped), relay count (28-29;
  0 = sent by the aircraft itself), encrypted by its owner (30), emergency
  (31).
- **LDPC(208,160)**: 48 parity checks (`ogn_ldpc_matrix.hpp`). The hard
  bits are checked first; otherwise normalised min-sum soft decoding
  (scale 0.75, flooding, ≤ 16 iterations) is accepted when every check
  passes with at most 16 bits changed ("bits corrected").
- **Whitening**: unless encrypted, (W1, W2) and (W3, W4) are each TEA
  encrypted with an all-zero key, 8 cycles (the first word = TEA's v0); the
  decoder runs TEA decryption. An owner-encrypted packet is the owner's own
  format: only its address and bearing are used.
- **Position report** (data words D0..D3 after de-whitening): latitude /
  longitude (signed 24-bit, 0.0008' / 0.0016' units; the decoder returns the
  middle of the unit), second of the minute, fix quality (a position only
  with a fix), DOP, fix mode (the altitude counts only with a 3D fix), GNSS
  altitude, ground speed, turn rate, track (360/1024°), climb rate,
  stealth / no-track, aircraft type, and the pressure altitude as a 9-bit
  difference to the GNSS altitude (its MSB in D1: set = 0..255, clear =
  −256..−1; both parts zero = absent). Altitude, speed, turn and climb are
  variable-resolution numbers: a mantissa under a 2-bit range field, steps
  of 1 / 2 / 4 / 8, the decoder returning the middle of the step; turn and
  climb signed, with "absent" codes 0x80 / 0x100.
- **Status report** (type 0): firmware, hardware, battery, Tx power,
  satellites. **Info report** (type 1): 7-bit characters packed into 14
  bytes, each string ended by its field code, an XOR checksum. Shown:
  Manufacturer, Model, Type, Serial, Registration, Competition ID, Class,
  Base, Hardware, Software. **Personal fields - Pilot, Crew, ICE, Pilot ID,
  Task - are never stored, shown or logged.** Other report types count as
  packets without content.

Aircraft types (both protocols' 0..15): unknown, glider, tow plane,
helicopter, parachute, drop plane, hang glider, paraglider, powered
aircraft, jet, UFO, balloon, airship, UAV, ground support, static object.

## Receiver

`receiver.cpp`: a 256-point FFT (Hann) of every 256 input samples (106.7 µs,
9375 Hz bins); per channel the power of the 17 bins within ±75 kHz against
its own noise floor (learnt over 64 frames, then a slow average of the
quiet frames). A burst of 2-14 ms above `sens` becomes a candidate (one
still on after 24 ms is a carrier, dropped), decided 4 frames later:

- **broadband rule** - two or more other candidates starting within ±2
  frames, spanning ≥ 600 kHz together = a noise burst (heimdall's
  calibration noise source after a retune, impulse noise): skipped, counted;
- **leakage rule** - a stronger, overlapping candidate within 250 kHz still
  pending = this one is its spectral copy: skipped.

A candidate is mixed to 0 Hz from a 109 ms input history, filtered (49-tap
windowed sinc, 110 kHz) and decimated to 400 kS/s (4 samples per chip); the
FM discriminator is summed over each chip and both sync tails are searched
by correlation (either polarity, ≤ 6 wrong chips). The 208 data bits are
"second chip minus first chip"; the timing is tried at the best sample and
one either side. `packet.cpp` checks them (CRC / LDPC + header parity) and
decodes the fields; `decoder.cpp` keeps the aircraft and reports.

## Outputs

- **Talkers (ID separation)**: one `kp::Talker` packet per packet an
  aircraft sends ITSELF, id = `<prefix> <address>` with the prefix from the
  address type: `RND`, `ICA`, `FLR`, `OGN` (both protocols: a FLARM packet
  with address type 3 is `OGN ABCDEF`); its samples from just inside the
  first matched sync chip to the last data chip (4.71 ms), `freq_hz` = the
  packet's measured carrier, `bw_hz` 160 kHz (aircraft on different channels
  at the same moment keep their own bearings), `avg_s` 10, `keep_s` 1 h.
  Label: registration (or competition ID) · altitude · type · `no-track`
  ("ZK-GOX · 1258 m · glider", "paraglider · no-track", `FLARM`, `OGN`). An
  OGN packet with a relay count was re-sent by another tracker and names
  nobody who sent these samples: **no talker** (its content still updates
  the aircraft). A talker carries the aircraft's position only from the very
  packet (a trackable OGN tracker with a 3D fix).
- **Events**: a new FLARM aircraft / OGN tracker (with "(FLARM address)",
  "encrypted by its owner", "no-track (stealth)", "(relayed)", the channel),
  "⚠ EMERGENCY flag from ..." (once per aircraft), the one-time tune;
  verbose: every packet (FLARM: type byte, inverted, channel, level, SNR;
  OGN: position / report type, relayed, bits corrected, level).
- **Table** (kept 30 min): Aircraft, Protocol, Aircraft type, Registration,
  Altitude, Climb, Speed, Track, Lat, Lon, Distance, Bearing (from the
  station), Notes, MHz, Signal dBFS, Packets (own); details: info fields,
  status, pressure altitude, turn rate, the FLARM payload note, the privacy
  note. A row is refreshed at most about once a second.
- **Map**: trackable OGN trackers only (kind aircraft / person / balloon /
  vehicle / point by type, heading, altitude, speed, the details); removed
  300 s after the last position, at once when the tracker turns no-track or
  encrypted.
- **Facts**: Channels, Aircraft (`n FLARM, n OGN tracker`), Packets
  (accepted FLARM / OGN packets, bursts demodulated, broadband events
  skipped), every 2 s.
- **Raw** (decoder log, while asked): `FLARM|OGN <26 bytes hex> <MHz>
  <dBFS>` - the corrected OGN packet, an inverted FLARM complemented back.
- `FLARM_DUMP=<folder>` (environment, diagnostics): every demodulated
  burst's 400 kHz baseband as `burst_NNNNN_<MHz>.cf32` (float32 I / Q) and a
  log line with its time, channel, length, SNR, both syncs' correlation
  (≈ 0.66 for a clean packet) and level.

## Privacy

- A **no-track (stealth)** OGN tracker never gets a position anywhere: no
  map point (an existing one is removed at once), no position, altitude,
  pressure altitude, distance or bearing from the station in the table, the
  details, the talker or any event - its verbose line is just `OGN <id>
  no-track`, and the position in its packets is not even kept. Its bearing
  stays (labelled `no-track`), and its aircraft type is shown.
- An **owner-encrypted** OGN packet: address + bearing only.
- **FLARM**: radio ID + bearing only. The encrypted payload is never
  decoded, brute-forced or analysed.
- Personal info strings (Pilot, Crew, ICE, Pilot ID, Task) are dropped on
  decoding.
- A relayed packet gives no talker.

## Tests

`tools/flarm_gen.py SCENE.json OUT.cu8|OUT.cf32` writes every antenna of a
scene at 2.4 MHz (real framing, CRC, whitening, LDPC from this folder's
matrix file, GFSK BT 0.5, known bearings on kraken_doa's clockwise UCA, 0.1
s pieces) + the truth sidecar; `tools/scene_six.json` (2 FLARM, an OGN glider
with its registration, a no-track paraglider, an owner-encrypted tracker, a
relay; packets at the same moment on 868.2 / 868.4 / 869.25 MHz) is the
debug-playback entry `flarm-six` (`tools/testsignals.json`).

Run 2026-10-21 (the Windows build on an x86 PC), against the 1.0 binary's
outputs on the same files:

| test | result |
|---|---|
| `--info` | identical to 1.0's except version 2.0 |
| 1.0's cached `flarm-six` (old generator), 5 antennas, `--uca 0.12` | 33 valid frames, 30 talker packets from 5 talkers, bearings 310.0 / 60.0 / 140.0 / 100.0 / 230.0° (0.0° off), the relay no talker; the whole summary (events, facts, table, map, peak/mean ratios) identical to 1.0's |
| the same, antenna 0, `--verbose --raw` | 33 raw lines = the truth; packet spans identical to 1.0's (e.g. 0.4502463 .. 0.4549563 s); carriers within 1 Hz; only the no-track verbose lines differ (deliberately, see *Privacy*) |
| the new generator's `flarm-six` | every truth packet passes its CRC / 48 checks / header parity and decodes to the scene (3C5A21 1250-1258 m, 26.3 m/s, 209.9°, +1.8 m/s, +3.0 °/s, baro −40 m, ZK-GOX / OX; D00F11 no-track paraglider; 1A2B3C encrypted); 2.0 and 1.0 give byte-identical summaries on it, and 1.0's summary on it equals its own on the old scene but for ±0.1 dB levels |
| probe: FLARM address types 3 / 0, an emergency helicopter with a Pilot + Model info packet, a balloon | `OGN ABCDEF`, `RND 123456`; `⚠ EMERGENCY flag from ICA 0A0B0C at -36.8921, 174.7795, 298 m` before `New OGN tracker ICA 0A0B0C on 869.250 MHz`; `Model: R44`, "Pilot" nowhere; helicopter −2.5 m/s, 150 km/h, 45°, 263 m pressure altitude, −12.2 °/s; balloon kind balloon, ~25 000 m, +0.0 °/s; bearings 30.0 / 90.0 / 200.0 / 300.0°; 12 valid frames - as 1.0 but for the once-a-second refresh, which 2.0 does at 0.999 s (1.0 skipped one of packets exactly 1.000 s apart) |
| 10 s of noise, `sens` 4 and 6 | nothing: 0 valid, 0 events, 0 bursts |
| 5 ms broadband noise bursts (+15 dB) at 0.5 / 1.5 / 2.5 s | no packet; 8 broadband candidates skipped by the 2 s facts (4 channels × 2), 42 with the RF unknown (21 windows) |
| channel lists / tuning | 921.6, 915.2, 916.6, 866.5 MHz + `extra=867.3` as listed above; at 433.92 MHz exactly one tune request, by station: Munich / no station 868.8, −41,174 868.8, −33,151 921.6, 40,−100 915.2, 32,35 916.6, −23,−47 921.6; `tune=0` none |
| inverted FLARM (`invert_payload`) | decoded, "(inverted)", raw = the original bytes |
| `FLARM_DUMP` on the probe | 12 files of 18024 bytes (5 ms bursts); flarm / ogn correlation 0.66 / 0.44-0.48 on clean packets; log lines identical to 1.0's |

Sensitivity, one antenna, 3 seeds × 100 packets per point (SNR in 160 kHz =
the generator's per-sample `level_db` + 11.76 dB); 2.0 and 1.0 decode
exactly the same packets at every point; no false packet; at most 10 bits
corrected:

| FLARM SNR | 7.8 | 8.8 | 9.8 | 10.8 | 11.8 dB |
|---|---|---|---|---|---|
| decoded | 58.7 % | 88.0 % | 98.3 % | 100 % | 100 % |
| with a 15 kHz carrier offset | 35.0 % | 68.7 % | 92.7 % | 99.0 % | 100 % |

| OGN SNR | 4.8 | 5.8 | 6.8 | 7.8 dB |
|---|---|---|---|---|
| decoded (LDPC soft decoding) | 0.3 % | 51.7 % | 99.7 % | 100 % |

The OGN limit is the burst detector (`sens` 6 dB), not the LDPC code. A 15
kHz carrier offset costs FLARM about 1 dB.

CPU: the receiver alone 0.4 % of a core with the four 868.8 MHz channels,
0.6 % with 21 windows (RF unknown); the offline host prints 3.3-3.7 % of real
time, most of it its own per-sample mixer. Not measured on the Pi here
(1.0: 2.9 % of a Pi 5 core live).

Not run here: kraken_doa's 🧪 Debug Playback of `flarm-six` (the catalogue
passes `make_testsignal.py --check`, and the entry generates: 144 MB in 3 s).

## Limits

- FLARM positions are not decoded (encrypted, unpublished keys) - ID and
  bearing only.
- Only the legacy FLARM packet format is recognised.
- ADS-L, OGN v2, FANET and PilotAware are not decoded.
- In the hopping bands only the channels inside the 2.4 MHz are heard.
- A mirrored input (`invert`) is not supported.
- No real FLARM / OGN recording was available: the formats are checked
  against synthetic signals only. The real-world use of the status / info
  report layouts and of the non-European channels is not verified.

## Sources

- OGN wiki, "OGN tracking protocol" (OGNTP / OGN1),
  http://wiki.glidernet.org/ogn-tracking-protocol: the physical layer, the
  sync chips `AA 66 55 A5 96 99 96 5A`, 20 + 6 bytes, little-endian words,
  the header fields, the position layout, units and absent codes, TEA
  key-zero whitening, LDPC 160 + 48, relays, the 868.2 / 868.4 MHz channels.
- FLARM Technology, FTD-012 Data Port ICD v1.1 (2022-06-06): aircraft types
  0-F, ID types 0 / 1 / 2. FLARM operating manual: a proprietary protocol on
  regional bands (868 MHz Europe, 915 MHz USA).
- Nordic Semiconductor nRF905 Product Specification v1.5: GFSK, 50 kbit/s
  Manchester, ShockBurst preamble + address + payload + CRC.
- CRC: the RevEng catalogue, CRC-16/IBM-3740 (check 0x29B1).
- TEA: D. Wheeler, R. Needham, "TEA, a Tiny Encryption Algorithm", FSE 1994
  (public domain).
- Normalised min-sum: J. Chen, M. Fossorier, "Near optimum universal belief
  propagation based decoding of LDPC codes", IEEE Trans. Commun. 50(3), 2002.
- ETSI EN 300 220-2: the 863-870 MHz SRD sub-bands.
- Windows (Hann, Blackman): F. J. Harris, "On the use of windows ...", Proc.
  IEEE 1978. The Gaussian frequency pulse of GFSK: a textbook result.
- **The LDPC matrix** (`ogn_ldpc_matrix.hpp`) is the OGN protocol's
  parity-check matrix, the code every OGN tracker transmits, from the OGN
  project's [glidernet/diy-tracker](https://github.com/glidernet/diy-tracker)
  (`ldpc.cpp`, `LDPC_ParityCheck_n208k160`, by Pawel Jalocha and the OGN
  project; GNU GPL version 3) - see *Licence*. Converted to one hex mask per
  check in packet byte order (the same 48 rows in the same order); no other
  code of that project is used. The generator's systematic encoder is built
  from it by GF(2) elimination.

## Licence

This plugin is licensed under the **GNU General Public License version 3**
(GPL-3.0, `LICENSE` in this folder), because its OGN parity-check matrix
(`ogn_ldpc_matrix.hpp`, GPL-3.0-only) comes from the OGN project's
glidernet/diy-tracker, licensed GPL-3.0: that repository's matrix is
identical to the one OGN trackers use, row for row, and its author (Pawel
Jalocha) committed it there himself. Copyright (C) 2026 KrakenRF Inc. for
everything else in this folder, which is GPL-3.0-or-later. (Version 1.0
had taken the same matrix from the author's esp32-ogn-tracker repository,
which carries no licence.)

The plugin is a separate program from the KrakenSDR DoA software
(kraken_doa): it is built into its own executable (`build/decoder`), runs as
its own process and talks to kraken_doa only over the plugin protocol
(pipes, `plugins/sdk/kraken_plugin.hpp`). It is built only on the plugin SDK
(`plugins/sdk`, MIT), pffft (BSD-style, `kraken_doa_v2/external/pffft`) and
liquid-dsp (MIT) - not on `plugins/lib` (`plugins/Makefile` builds a plugin
with its own `LICENSE` that way). GPL-3.0 code - glidernet/diy-tracker,
SoftRF - may be used here, with its notices; code without a licence (the
esp32 / nRF52 / CubeCell OGN tracker repositories) may not.
