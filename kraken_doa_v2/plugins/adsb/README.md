# ADS-B / Mode S (1090 MHz)

Decodes the Mode S messages aircraft transmit on 1090 MHz - ADS-B extended
squitters and the transponder replies - and puts every aircraft with a
position on the web UI's 🗺 Map.

**Use:** pick "ADS-B (1090 MHz)" as a VFO's digital decoder. That sets the
VFO to the **2.4 MHz (No decimation)** bandwidth, puts it on its tuner's
centre and tunes the tuner to **1090.000 MHz** (in coherent mode the whole
array; in independent mode only that VFO's tuner). The VFO is then drawn as
one line at the centre of the spectrum and stays there. ADS-B never runs in
Auto detect (`manual_only`). Set the station location (sidebar → Station
Information) for distances and the range check. Keep beamforming off
for this VFO (aircraft come from every direction) but DoA ON in coherent
mode: then the plugin gets every antenna and combines them (Array combining
below - weak aircraft ~6 dB further down, overlapping messages separated,
a bearing per aircraft from its own pulses); with DoA off it gets one
channel. A 1090 MHz antenna helps a lot; in Independent mode one tuner can
stay on 1090 MHz.

**Demodulation:** pulse-position modulation, 0.5 us chips = 1.2 samples at
2.4 MHz. The magnitude is integrated over each chip at five sub-sample
timings (box-filter weights); a preamble candidate (pulses at 0, 1, 3.5 and
4.5 us clearly above the gaps between them) is scored at all five, and the
two best timings are sliced into bits (first half of the bit stronger = 1)
and checked by CRC-24 (generator 0xFFF409).

- DF17 / DF18 (ADS-B, CF 0/1/6): one bad bit is repaired from the syndrome
  table, two bad bits if both are among the eight least certain ones - only
  for aircraft already heard cleanly, so noise can't invent aircraft
- DF11 all-call replies, DF0/4/16/20 altitude and DF5/21 identity (squawk)
  replies: the address is XORed into the parity, so they are matched to
  aircraft already known
- An aircraft appears (events, map, frame count) from its second message

**Every antenna** (coherent mode with DoA on - kraken_doa then hands the
plugin every antenna, `kp::Info::multichannel`; option `array`, on by
default): the magnitude above becomes the noise-normalised sum of every
antenna's power (each antenna brought to the same noise level). Its noise
fluctuates far less than one antenna's Rayleigh magnitude, so the gate sits
lower (1.3 x the floor instead of 1.6) and weak preambles show. What
doesn't decode on that sum is BEAMFORMED, per candidate:
1. a preamble at least 1.35 x its gaps on the sum (and no gap above the
   pulses' mean - with four weak pulses against twelve gaps the strict
   "no gap as strong as a pulse" lost 40 % of the 5 dB messages)
2. maximum-ratio weights from the preamble pulses (the principal
   eigenvector of their noise-normalised covariance), the beam's magnitude
   checked with the strict preamble test and sliced
3. decision-directed: the weights again from every pulse of the message as
   sliced (~130 samples), sliced again
4. with `separate`: an MVDR beam aimed at the message's pulses minus its
   gaps (a message overlapping it is in both, so it cancels from the target)
   with the window's covariance inverted (loaded with one antenna's noise):
   what else is in the window from another direction is nulled
After a decode, `separate` also scans the message's span for a second
message starting inside it, with the first one's signature projected out of
every antenna (the null space's magnitude, then weights from the second
message's own preamble / pulses in that space). Each message's DoA
(`pulse_doa`) is the covariance of its own pulse samples - samples another
decoded message has a pulse in are left out - sent with the talker packet
(`kp::Talker::cov`): kraken_doa then keeps no stream ring for the VFO and
never drops such a packet for overlapping another aircraft's. Each aircraft
learns its spatial signature (the unit vector over the antennas) from the
messages that name it with an exact parity (DF17/18, DF11); a repaired
message, a reply repaired into its address and a DF11 with an interrogator
code must match it (|a^H s|^2 >= 0.7; a random direction gives 1/5 on
average), else it is refused.

**Bit repair** (`fix`): 0 off, 1 standard (above), 2 extended:
- DF17 / DF18: also three bad bits among the twelve least certain (a noise
  candidate would also need the address of an aircraft heard before - ~1 in
  10^12)
- DF11: one bad bit anywhere (interrogator code 0) or one / two among the
  eight least certain with any code; the address in the message must be an
  aircraft heard before
- address / parity replies: one or two among the eight least certain bits
  turning the syndrome into a known aircraft's address, at its level (within
  6 dB), saying what is known of it (altitude within 1000 ft of its last two
  minutes, identity = its squawk) - and, with the array, its signature

**Positions of weak aircraft** (`cpr`, on by default): an even / odd pair
may be up to 30 s apart instead of 10 while the aircraft's known ground
speed keeps it within 4 km of the first message (15 s with no velocity) -
global CPR decoding stays right while the pair is within ~5.6 km - and the
last position, moved on with the velocity, stays the reference for local
decoding for 3 minutes instead of 60 s.

**High sensitivity** (`sens`, off): lower gates (one antenna 1.35, array
1.2), every timing that passes is sliced (not only the best two), one
antenna also tries a preamble 1.6 x its gaps with the loose gap rule - more
CPU, a few more weak messages.

**Decoding:** identification (callsign, category), airborne position with
barometric (25 ft or Gillham) or GNSS altitude, surface position and speed,
velocity (ground speed + track, or airspeed + heading; vertical rate),
emergency / priority status with the squawk (TC 28). Positions: CPR global
decoding from an even + odd pair (10 s apart at most; surface positions need
the station or the aircraft's last position as a reference), then local
decoding against the last position. A position further than the `range`
option (default 500 km) from the station, or faster than ~Mach 2.5 from the
last one, is rejected.

**Output:**
- Table (above the facts in the decoder tab): one row per aircraft with
  everything decoded - ICAO, callsign, category, squawk, altitude, GNSS
  altitude, vertical rate, ground speed, track, heading, IAS, TAS, position,
  distance / bearing from the station, air / ground, emergency, signal,
  message count, position age (+ "Seen"). Sort by any column; click a row
  to show the aircraft on the map
- Facts: aircraft count (with position), messages (rate), max range, noise
  floor, antennas (combined or not, how many messages only the beams
  decoded, how many were separated from an overlap, bearings from the
  pulses), bit repair (how many messages, with 2-3 bits, replies, all-calls,
  refused by the signature)
- Events: new aircraft, its callsign, its first position, emergencies (7500,
  7600, 7700 or the emergency state); every message with "Log all"
- Map: one marker per aircraft (ttl 60 s), details in its popup - once
  "🗺 Plot on map" is ticked in the decoder tab
- Options (the decoder tab): `range` (300 / 500 / 1000 km / no limit),
  `fix` Bit repair (off / standard / extended), and on / off buttons
  `array` Array combining, `separate` Overlap separation, `pulse_doa` DoA
  from the pulses, `cpr` Long position pairing (all four on by default),
  `sens` High sensitivity (off). A VFO saved with the old `fix` = 1 keeps
  standard repair
- Unusual activity is reported as events starting with "⚠" (once each,
  with position and altitude): emergencies (7500 hijack / 7600 radio
  failure / 7700, or the emergency state message) and when they end, a
  squawk change, special squawks 7400 (lost link, unmanned aircraft), 7777
  (US military interception) and 0000, unusual aircraft types (high
  performance, balloon, parachutist, ultralight, UAV, space vehicle),
  climbs / descents of 6000 ft/min or more, and 400 kt or more below
  10000 ft. IDENT (the pilot's ident button, SPI) is logged too. A squawk
  read from a Mode S reply only counts when the next reply confirms it (a
  single bit error would otherwise look like a squawk change)
- DoA per aircraft: every accepted message of a confirmed aircraft goes to
  kraken_doa as a talker packet (`kp::Talker::packet`, its exact samples),
  with the aircraft's position at that moment (`Talker::lat / lon / alt_m`:
  the last position moved on with its ground speed, track and vertical
  rate - airborne only, a position at most 3 s old, a velocity for anything
  older than 0.1 s; GNSS altitude when sent, else barometric) - the ✈ array
  calibration uses them as transmitters at known positions. On a synthetic
  recording (20 aircraft, 20 s) the packet positions were within 2.4 m of
  the truth (median; 95 % within 12 m)
- Raw frames (decoder data log, "Raw frames" ticked): every message that
  passed its CRC as hex + its level in dB, `8D4840D6202CC371C32CE0576098
  -14.8` (the format dump1090's raw output uses, plus the level). Busy sky:
  ~100-300 MB a day before gzip

**Tests** (synthetic 2.4 MHz recordings, 8-bit like an RTL-SDR, 30 kHz
carrier offset, random phase and timing): the published examples of "The
1090 MHz Riddle" (J. Sun) decode to their documented values - KLM1023
(4840D6), 40621D at 52.2572, 3.9194 / 38000 ft, 485020 at 159 kt / 183 deg;
a synthetic aircraft's track, callsign, squawk and emergency; 100% of
messages at 15 dB SNR (per sample) and above, ~67% at 12 dB, none below
9 dB - the same slicer as dump1090's 2.4 MHz demodulator; nothing from 20 s
of noise. Decode time ~8% of real time on a Raspberry Pi 5 core. Not yet
tested on live 1090 MHz signals.

```bash
plugins/adsb/build/decoder --file capture.cu8 --rate 2400000 --station 52.0,4.0
# every antenna (sample by sample) - the array path, per-aircraft bearings; ADSB_STATS=1: path / repair counts
ADSB_STATS=1 plugins/adsb/build/decoder --file scene.cu8 --channels 5 --uca 0.12 --rf 1090e6 --station LAT,LON
```

**Array / repair / pairing tests** (2026-10-20; synthetic 8-bit 5-antenna
UCA scenes from `tools/adsb_gen.py`, per-sample SNR at every antenna;
scored against the generator's truth message by message; "wrong" = a
decoded message the scene never sent - none in any run below):

| scene | baseline (v1.0, one antenna) | v2.0 one antenna | + extended repair | array | array + extended |
|---|---|---|---|---|---|
| sweep: 12 aircraft 2-13 dB, 20 s (1304 msgs) | 18.0 % | 18.0 % | 20.2 % | 73.2 % | 75.0 % |
| busy: 30 aircraft 6-24 dB, 480 msgs/s | 66.1 % | 66.1 % | 68.2 % | 94.4 % | 94.4 % |
| sweep + 4 replies/s each (2240 msgs) | 14.9 % | 14.9 % | 19.4 % | 69.2 % | 73.7 % |
| busy + 6 replies/s each (1000 msgs/s) | 54.5 % | 54.5 % | 58.1 % | 89.3 % | 90.5 % |
| 20 s of noise | 0 | 0 | 0 | 0 | 0 |

- Per level (sweep, decoded): one antenna 10 dB 17 %, 11 dB 50 %, 12 dB
  68 %, 13 dB 82 %; the array 4 dB 40 %, 5 dB 63 %, 6 dB 82 %, 7 dB 90 %,
  8 dB 95 % (extended repair: 4 dB 47 %, 5 dB 66 %) - ~6 dB more sensitive
  (5 antennas: 7 dB in theory). The array
  path through kraken_doa's engine (tools/digi_test, SAMPLES_MC): the same
  frames as offline, 0 dropped
- One antenna, `sens`: sweep 22.7 % (10 dB 29 %, 11 dB 71 %); with extended
  repair 26.1 % (10 dB 47 %, 12 dB 92 %). Extended repair of replies without
  the content check gave 1 wrong DF20 in 6243 here (sens: ~6600 noise
  candidates a second reach the CRC); with it none
- `tools/scene_six.json` (three forced collisions): 191 / 197 one antenna,
  196 / 197 with the array (3 separated from an overlap), bearings 0.0 deg
- `separate` (busy): 93.2 % without, 94.4 % with; `pulse_doa` (busy +
  replies): 8926 packets with a bearing vs 8718 cut from the stream (the
  overlapping ones), mean error 0.03 deg both
- `cpr`: 6 aircraft at 15 dB sending every message type once in ~12 s for
  90 s: none on the map without it, all 6 with it (first fix after 15-23 s,
  within 2 m); 8 moving aircraft (250-600 kt, `"moving": true`), 120 s:
  every position within 0.14 km of the true track, none without it
- CPU (this PC, offline, % of real time): one antenna 4.0-4.6, the array
  7.1-9.9 - of which reading 5 antennas is ~1.5-2 (the host), the combining
  + beams 1.2-1.9, separation 0.4. Live, kraken_doa sends every antenna
  (SAMPLES_MC, 48 MB/s at 2.4 MHz instead of 19) and keeps no packet ring
  for the VFO (pulse DoA)

**Test transmission** (debug playback `adsb-six-aircraft`): `tools/adsb_gen.py`
writes a 2.4 MHz scene on the 5-element UCA (r 0.12 m) - DF17
identification (TC 4), airborne position (TC 11, CPR even / odd in turn),
velocity (TC 19) and DF11 all-call replies (opt-in per aircraft: DF4 / 5 /
20 / 21 address / parity replies, `replies`; per scene: `moving`), CRC-24, PPM built at 24 MHz
with ~70 ns edges and integrated to 2.4 MHz, random sub-sample timing and
carrier phase, a carrier offset per aircraft (-110..+160 kHz), 17-25 dB.
`tools/scene_six.json`: six aircraft (ANZ123 C81A2B 40°, QFA44 7C4A51 95°,
UAL917 A0B1C2 150°, BAW12 406B90 215°, ZKTCA C82F11 270°, DLH410 3C4AD7
330°) 25-120 km from the station at their compass bearings (the station's
position at generation time; fixed positions - the playback loops), three
pairs of messages forced to overlap. The generator's identification of
KLM1023 is the Riddle's frame byte for byte. Offline (`--channels 5 --uca
0.12`): 190 of 197 messages (the lost ones collide), every aircraft's
callsign / position / altitude / speed right, bearings 0.0° off, 20 s of
its noise nothing; through kraken_doa's debug playback 6 / 6 talkers at
0.0°.

**Real recording + reference decoder** (2026-10-16; dump1090 - antirez's,
BSD-3-Clause - built in scratch as the reference, not in this repo):
- dump1090's own test capture `testfiles/modes1.bin` (2 MS/s cu8, real
  messages of one aircraft, 4D2023 AMC421): dump1090 217 messages (111
  different); the plugin (resampled to 2.4 MHz) 328 - every one of
  dump1090's 217 among them, + 111 more (DF17 positions / velocities it
  misses at 2 MS/s, DF0 / DF11 replies of the known aircraft).
- The generator's `scene_six` (1 antenna) resampled to 2 MS/s through
  dump1090: 144 of 197 messages decoded (its 2 MS/s demodulator; the
  collisions), all byte-identical to what was sent, none it didn't send;
  its aircraft table = the scene (callsigns, 3500-38000 ft, speeds +-1 kt,
  tracks, positions to 0.001° by global CPR).
