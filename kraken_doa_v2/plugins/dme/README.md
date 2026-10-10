# DME / TACAN decoder

Decodes the ground beacons of DME (Distance Measuring Equipment) and TACAN
on 962-1213 MHz, and aircraft interrogating them on 1025-1150 MHz. Every
DME channel inside the VFO's band is decoded at once: a 2.4 MHz VFO ("No
decimation") covers up to three 1 MHz channels; a narrower VFO (600 kHz
or more) is resampled to 2.4 MHz.

## What it shows

Per channel (the panel's table) and for the strongest one (facts):

- **Channel**: e.g. `DME 40X: reply 1001 MHz, interrogation 1064 MHz`. On
  1025-1150 MHz a frequency is a Y-mode reply AND an interrogation
  frequency, so both channels are named; 1030 / 1090 MHz are flagged as SSR
  frequencies
- **Paired with**: the VOR or ILS localizer + glide slope the channel
  belongs to (ICAO Annex 10 Table A), e.g. `ILS localizer 110.30 MHz,
  glide slope 335.00 MHz`; channels 1-16 and 60-69 have none (DME / TACAN
  only)
- **Signal**: beacon replies (X: 12 us pairs, Y: 30 us), aircraft
  interrogations (X 12 us / Y 36 us, with "about N aircraft" from the
  distinct pair levels and their levels), or pairs that are no DME use
  of that frequency
- **Ident**: the Morse ident (`IAA  .. .- .-`), how often it was heard and
  when, the dot length and the keyed pair rate (1350 per second). A
  beacon sends it about every 30-40 s; the first copy is marked "first
  copy" in the event log
- **Pulse pairs** per second, **level** (dBFS of the pulse peaks) and SNR,
  **pulses** (pair spacing, half-amplitude width, 10-90 % rise time)
- **Carrier**: the beacon's exact frequency from the phase steps inside its
  pulses (the receiver's own frequency error is included)
- **TACAN**: when the beacon is a TACAN (north reference bursts at 15 Hz),
  the bearing from the beacon (coarse 15 Hz + fine 135 Hz), the 15 / 135 Hz
  modulation depths and the burst rate; else "no"

The event log has each channel when it is first heard, every ident, a
TACAN when found, and a beacon going quiet. Raw frames (decoder data log):
the idents.

## DoA per beacon

Clean, strong reply pairs (nothing within 15 us before or after) are
reported as `kp::Talker` packets of the channel (`40X`, label `IAA · 1001
MHz`), at most "DoA pairs per second" (default 100) per beacon, averaged
over 10 s. Each beacon then has its own bearing in the 🗺 Map's 📡 DF panel
(📡 beacon; "Every beacon") - independent of other signals in the VFO. With
the VFO's squelch on "Digital", the VFO's own DoA comes from those pairs
only.

## Options

- **Pulse threshold** (default 10 dB over the noise): lower finds weaker
  beacons, higher ignores more interference
- **DoA per beacon** on / off, **DoA pairs per second** (20-500)

## How it works

FFT channelizer (4096 points at 2.4 MHz, overlap-save, 1.2 MHz per
channel, flat to +-350 kHz, zero from +-550 kHz) -> magnitude -> noise =
median magnitude -> peaks above the threshold with a half-amplitude width
of 2.4-5.6 us (SSR / Mode S pulses are narrower) -> pairs 12 / 30 / 36 us
apart (+-1 us), amplitudes within 4 dB. What a pair is follows from the
frequency band (`role_of`). Ident: a reply pair with another one 1/1350 s
(+-5 us) earlier is part of a key-down; per 10 ms the key is down when most
pairs are (3-tick majority) -> Morse (`lib/navaid.cpp`, dot / dash from the
marks' own lengths). TACAN: runs of equally spaced pairs (12 µs pairs every
20-36 us; 9+ = north, else auxiliary) or single pulses (12 / 15 / 30 us
apart, Y mode: 13 single pulses) are reference bursts; with north bursts at
a steady 15 Hz, every other pulse's amplitude against its rotation phase
(time since the north burst / turn length) is fitted with 15 + 135 Hz
sinusoids (least squares, ~3 s memory); the north burst goes out as the
pattern's maximum points east and it turns clockwise, so bearing = 90° + the
15 Hz maximum's phase (coarse), refined by the 135 Hz one.

The VFO's frequency comes from `kp::Host::rf_hz()`; without it (an older
receiver, or an offline file without `--rf` / an `rf_hz` sidecar) the
centre of the input is decoded as one unnamed channel.

CPU (Pi 5): ~3.3 % of a core per decoded channel (two channels in a 2.4 MHz
VFO: 6.6 %).

## Tests

- Live recording (ai/sigtool.py, Auckland): 90 s of 1001 MHz at 480 kHz -
  40X, ident IAA three times (dots 120 ms, 1324 pairs/s keyed), 944
  pairs/s, 3.42-3.8 us pulses, carrier -5.2 kHz; a 2.4 MHz capture on
  1000.756 MHz - 1000 MHz (only a CW spur: nothing) and 1001 MHz decoded
  at once
- Synthetic (2.4 MHz, Gaussian 3.5 us pulses): an 18Y beacon (ident IBC)
  with three aircraft interrogating 81X on the same frequency (found as
  about 3 aircraft at their levels); a Y-mode TACAN at 237° (decoded
  236.9°, 15 Hz 20 %, 135 Hz 10 %); a 12 dB beacon in 2700 pairs/s
  (ident IXY); an X-mode TACAN at 123.4° (123.4°); SSR pulses on 1030 MHz
  and noise (nothing); a file without an RF (the centre decoded)
- End to end (fake heimdall streaming the live recording on 5 antennas from
  70° + a CW interferer from 200°, 127 mm UCA): beacon 40X's bearing
  69.4°, the whole VFO's 198°; with the Digital squelch the VFO's DoA is
  69-70° and the squelch stays open

Not tried on a live TACAN or on Y-mode beacons (none here); the TACAN
reference burst spacing differs between sources (X north burst pairs 30 or
24 us apart), so both are accepted.
