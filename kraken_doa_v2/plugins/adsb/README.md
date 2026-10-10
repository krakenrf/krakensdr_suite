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
for this VFO (aircraft come from every direction). A 1090 MHz antenna helps a
lot; in Independent mode one tuner can stay on 1090 MHz.

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
  floor
- Events: new aircraft, its callsign, its first position, emergencies (7500,
  7600, 7700 or the emergency state); every message with "Log all"
- Map: one marker per aircraft (ttl 60 s), details in its popup - once
  "🗺 Plot on map" is ticked in the decoder tab
- Options: `range` (300 / 500 / 1000 km / no limit), `fix` (bit repair on/off)
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
```
