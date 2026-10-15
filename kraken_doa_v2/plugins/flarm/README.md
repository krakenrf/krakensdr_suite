# flarm - FLARM / OGN trackers (868 / 915 MHz)

The collision-avoidance radios of gliders, tow planes, helicopters, light
aircraft, hang gliders and paragliders: **FLARM** (and the many devices that
speak its radio protocol) and the **OGN tracker** protocol of the Open Glider
Network, an open protocol on the same channels. Every aircraft gets its own
bearing; OGN trackers also give their position, altitude, climb, speed,
track, aircraft type and registration.

Pick "FLARM / OGN trackers (868 / 915 MHz)" as a VFO's digital decoder: like
`ism` it puts the VFO on its tuner's centre with the full 2.4 MHz
(`kp::Info::lock_vfo`) and listens to every channel of the band plans that
lies inside it. When none does, it retunes the receiver once
(`kp::Info::tunes`, option "Tune to the band") to its band (option "Band",
by default from the station location):

| Band | Channels (MHz) | Tuned to |
|---|---|---|
| Europe / Africa | 868.2, 868.4 (FLARM and OGN alternate between them per time slot) | 868.8 |
| New Zealand | 869.25 (one channel; FLARM's NZ region, OGN's NZ plan) | 868.8 |
| Australia / South America | 917.0 - 926.2, 400 kHz steps, 24 channels, frequency hopping | 921.6 |
| North America | 902.2 - 927.8, 400 kHz steps, 65 channels, frequency hopping | 915.2 |
| Israel | 916.2 | 916.6 |

868.8 MHz holds Europe's and New Zealand's channels together (868.2 /
868.4 / 869.25, and Russia's 868.8); in the hopping bands the 2.4 MHz holds
5-6 of the channels, so an aircraft is heard on the packets that hop into
them (about 1 in 4 in Australia, 1 in 11 in North America). Also received
when inside the VFO: India 866.0, Korea 920.9 and an "Extra channel MHz"
(option). Sources: the OGN tracker's frequency plans (FreqPlan), SoftRF's
region table, FLARM product documentation (NZ = the European hardware).

## Radio (both protocols, as publicly documented)

GFSK, 100 kchip/s, ±50 kHz (a chip 1 = +50 kHz), Manchester coded (IEEE: a
data 1 = chips 0 1; 50 kbit/s), a short preamble, a 32-bit sync, 26 bytes,
MSB first; ~4.9 ms per packet.

- **FLARM** (the legacy radio packet of the nRF905-based devices): sync
  ...F531FAB6 (31 FA B6 is the nRF905 address), 24 bytes + CRC-16 (CCITT,
  init FFFF, over 31 FA B6 + the 24 bytes). Bytes 0-2 = the 24-bit radio ID
  (little-endian), byte 3 = its address type (bits 4-5: random / ICAO /
  FLARM / anonymous) - in the clear. The other 20 bytes - position,
  altitude, speeds, aircraft type and the stealth / no-track flags - are
  ENCRYPTED. FLARM has not published that cipher's keys (the protocol is
  proprietary; third parties have reverse-engineered older versions from
  device firmware, and FLARM changed the protocol in March 2024). Per this
  project's rules nothing is done to decrypt them: **a FLARM aircraft is
  shown by its radio ID and its bearing only** ("position encrypted (not
  decoded)").
- **OGN tracker** (the open protocol; the OGN tracker source at
  github.com/pjalocha/esp32-ogn-tracker is its published reference, used as
  documentation): sync 0AF3656C, 20 bytes = five little-endian 32-bit words
  + 6 bytes of LDPC(208,160) parity (`ogn_ldpc.h`: the protocol's
  parity-check matrix). Header: 24-bit address, address type (random, ICAO,
  FLARM, OGN), non-position flag, parity (checked), relay count, encrypted,
  emergency. The four data words are "whitened" with TEA under the
  all-zero key, 8 rounds per word pair - a published cipher with a published
  key, so it is undone. Position report: latitude / longitude (8 / 600000 °
  and 16 / 600000 °), fix quality / mode, DOP, GNSS altitude and the
  pressure altitude's difference, speed, track, climb and turn rate
  (variable-resolution fields), aircraft type, stealth. Info report:
  7-bit strings with an XOR check - manufacturer, model, type, serial,
  registration, competition ID, class, base, hardware, software (the
  pilot / crew names, emergency contact, pilot ID and task are left out:
  personal). Status report: firmware / hardware, battery, Tx power,
  satellites. An OGN packet ENCRYPTED by its owner (a private key) is shown
  by its address only.
- Not decoded: ADS-L (EASA's SRD-860 standard, sync F5724B18), OGN v2,
  FANET (LoRa), PilotAware (869.525 MHz).

## Privacy

- An OGN tracker with the stealth flag set (OGN's "don't track me") is
  never put on the 🗺 Map, its position is never in the table or the
  decoder data log (no map points, no position events), and its packets
  carry no position for the array calibration; its row says "no-track
  (stealth)" and its bearing is kept (the brief's rule).
- FLARM's own no-track / stealth flags are inside the encrypted part: they
  can't be read - and no FLARM position is shown at all.

## Front end (`decoder.cpp`)

A 256-point FFT every 256 samples (Hann, 9.4 kHz bins, 107 µs): the power
in ±75 kHz around each channel against its noise floor (a gated mean) finds
bursts; a burst of 2-14 ms is mixed down from a 55 ms history of the 2.4
MHz input, low-passed (110 kHz) and decimated to 400 kHz (4 samples per
chip), FM-demodulated (fast atan2), summed over each chip, and searched for
both syncs (the last 56 chips of each, either polarity, ≤ 6 chip errors);
the strongest of bursts that overlap within 250 kHz is the one
demodulated, and bursts that start together on 3+ channels spanning ≥ 600
kHz are one broadband event (the calibration noise source, impulse
noise), not packets. `FLARM_DUMP=dir` (environment) writes every
demodulated burst's 400 kHz baseband there and logs its channel, length,
level and sync correlations - for diagnosing what a band holds. Data bits = the chip pair difference (soft). FLARM: the CRC
(also with the bits inverted - the opposite Manchester convention some
radios use). OGN: the LDPC checks; failing that a normalised min-sum
decoder (16 iterations) on the soft bits.

## Output

- Table: Aircraft (id), Protocol, Aircraft type, Registration / CN,
  Altitude, Climb, Speed, Track, Lat / Lon, distance / bearing from the
  station (OGN only), Notes (position encrypted / encrypted by its owner /
  no-track / relayed), MHz, Signal, Packets; details (▸) with the info
  strings, status, pressure altitude, turn rate and why something isn't
  shown. Rows kept 30 min.
- 🗺 Map: OGN trackers that may be tracked (aircraft, person for
  paragliders / hang gliders / parachutes, balloon, vehicle).
- Events: new FLARM aircraft / OGN tracker (with the channel and what can't
  be shown), ⚠ the OGN emergency flag.
- Raw frames: `FLARM <hex> MHz dB` / `OGN <hex> MHz dB` (the 26 bytes).
- Options: Band, Tune to the band, Burst detection (dB over the floor),
  Extra channel MHz.

## ID separation

Every packet an aircraft sends ITSELF is a `kp::Talker` packet of its
address - `FLR DD1234` (FLARM ID), `ICA C81A2B` (ICAO), `OGN 3C5A21`, `RND
...` (random) / `ANO ...` (anonymous), the same id as its table row and map
point (a device sending both protocols with one address is one talker) -
span = its first sync chip to its last data chip, on its channel
(`freq_hz` = the burst's measured carrier, `bw_hz` 160 kHz: kraken_doa
filters that channel out of every antenna, so aircraft transmitting at the
same moment on different channels keep their own bearings), `avg_s` 10,
`keep_s` 1 h; label "registration · altitude · type" (OGN) or "FLARM". An
OGN packet with a relay count > 0 was re-sent by ANOTHER tracker - it
names nobody who sent these samples: no talker (its position still updates
the aircraft). UI kind `flarm`: ✈ (🪂 paraglider / hang glider /
parachute, 🚁 helicopter, 🎈 balloon), "Aircraft / Reg · altitude · type /
Pkts", position columns (the DF error against an OGN tracker's own
position), nearest first.

## Tests

`tools/flarm_gen.py` writes FLARM packets (real header and CRC; random
bytes stand in for the encrypted part) and OGN packets (real fields,
whitening, LDPC parity from the plugin's matrix) at known bearings on the
5-element UCA, 8-bit like the receiver. The LDPC matrix was checked to
reproduce the OGN tracker's own generator table (derived by GF(2)
elimination: identical).

- **No real FLARM / OGN recording found** (no IQ capture is published by
  the OGN, SoftRF, gr-flarm, flare or OrcSDR projects; sigidwiki has no
  entry): the plugin is tested on synthetic signals only. The GENERATOR was
  checked against independent references (2026-10-16, built in scratch
  from source, GPL - not in this repo):
  - FLARM: creaktive/flare's `nrf905_demod` (an nRF905 demodulator written
    against real FLARM traffic) at 1.6 MS/s: it decoded none of the first
    version's packets - their preamble was 0000 0000 before the sync's F5,
    the nRF905 sends 1111110101 (its last 8 bits = F5; SoftRF: preamble
    0x55 + Manchester(F531FAB6)). Fixed (FLARM preamble chips 01 01 ..);
    now every packet decodes with its CRC, bytes = the generator's
    (31 FA B6 + ID LE + type + 20 bytes + CRC); with the IQ conjugated
    none - so the chip polarity (1 = +50 kHz, data 1 = chips 0 1) is the
    real devices'. The plugin never used the preamble (it matches the
    last 28 sync bits): its results are unchanged.
  - OGN: the OGN tracker's own code (pjalocha/esp32-ogn-tracker `ogn1.h`,
    `ldpc.cpp`) on the generator's 28 OGN packets of `scene_six`: LDPC 0
    failed checks, parity = its `LDPC_Encode`, header parity good, and
    after its `Dewhiten()` every field as the scene sets it (3C5A21 1250 m,
    26.3 m/s, 210°, +1.8 m/s, 3°/s, glider, ZK-GOX / OX info packets;
    D00F11 stealth paraglider; 1A2B3C encrypted) - the plugin's decode of
    the same file gives the same values.

- **ID separation** (`tools/scene_six.json`, 8 s at 868.8 MHz): two FLARM
  aircraft (868.2 / 868.4 alternating, 869.25), an OGN glider with
  registration (868.4 / 868.2), a no-track OGN paraglider, an OGN tracker
  encrypted by its owner, and a relay re-sending the glider's packets from
  20° - three of them transmitting AT THE SAME MOMENT on 868.2, 868.4 and
  869.25. Offline (`decoder --file six.cu8 --channels 5 --uca 0.12 --rf
  868.8e6`): 44 / 44 packets, the five aircraft 0.0° off (60 / 140 / 230 /
  310 / 100°), the relay no talker; end to end (fake heimdall into
  kraken_doa): the same five at 0.0°, 285 of 289 packets with a bearing.
  UI (headless Chromium): kind flarm, 🪂 / ✈, "Every aircraft (5)", the
  glider's distance + DF error, no map point / position for the no-track
  one, its details say why.
- **Sensitivity** (single antenna, SNR in the 160 kHz channel): FLARM 54 %
  at 7.8 dB, 98-100 % from 9.8 dB; OGN (LDPC soft decoding) 40 % at 5.8 dB,
  100 % from 7.8 dB; a 15 kHz carrier offset costs FLARM ~1 dB. 10 s of
  noise: nothing.
- **CPU** (Pi 5): plugin 2.9 % of a core end to end (19 MB/s pipe
  included); offline 9.5 % of real time on noise, most of it the offline
  host's own work.
- **Live** (Auckland, the real array, 2026-10-15 09:15-09:30 NZDT - a
  Thursday morning; the gliding clubs fly at weekends): no FLARM / OGN
  packet. Started on the user's 922.98 MHz, the plugin retuned the
  receiver itself to 868.8 MHz (band auto: 868.8 MHz) and listened on 868.2 /
  868.4 / 868.8 / 869.25; it demodulated ~1 burst per 10 s - a periodic
  device near 868.166 MHz every 16 s (8-12 ms, a narrow FSK: neither sync
  correlated, > 0.4 of a clean one never) and the noise source's
  calibration bursts after the retune on all four channels at once (now
  skipped as one broadband event) - and reported no packet: nothing false.
  Plugin 2.6 % of a core.

## Limits

- FLARM positions are not decoded (above): ID and bearing only.
- The FLARM sync / CRC are the legacy packet's; packets FLARM devices send
  in other formats would not be recognised.
- In the hopping bands only the channels inside the 2.4 MHz are heard.
- SoftRF and the OGN tracker (GPL) served as documentation only; the LDPC
  parity-check matrix is the OGN protocol's constant (`ogn_ldpc.h`), no
  code of theirs is in this plugin.
