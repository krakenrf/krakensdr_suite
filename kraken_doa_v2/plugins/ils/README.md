# ILS / VOR / marker beacon decoder

The VHF / UHF aviation navigation aids, one per VFO (48 kHz, on the
station's frequency):

| Navaid | Frequency | What is decoded |
|---|---|---|
| ILS localizer | 108.10-111.95 MHz (odd tenths) | course deviation: DDM (90 Hz dominant = left of the course as an approaching aircraft sees it, 150 Hz = right), needle in uA (0.155 DDM = full scale 150 uA), SDM; two-carrier (course + clearance) systems; Morse ident (1020 Hz) |
| ILS glide slope | 329.15-335.00 MHz | DDM (90 Hz = above the glide path, 150 Hz = below; 0.175 = full scale), SDM, two carriers |
| VOR | 108.00-117.95 MHz | the radial (magnetic bearing from the station) and the station's bearing from here, also true with the "Magnetic variation" option; FM deviation; Morse ident; voice (ATIS) present |
| Marker beacon | 75 MHz | outer (400 Hz dashes), middle (1300 Hz dots and dashes), inner / fan (3000 Hz dots), with the keying rate |

With the VFO's frequency (`kp::Host::rf_hz()`) it also names the paired
frequencies: a localizer's glide slope and DME channel (e.g. 110.30 MHz ->
glide slope 335.00 MHz, DME 40X reply 1001 MHz - the `dme` plugin decodes
it), a glide slope's localizer, a VOR's DME channel. Without it the type
comes from the signal alone.

## How it works

Every 0.5 s the last 1 s at 48 kHz is analysed (1 Hz FFT bins): the
strongest carrier within +-20 kHz (15 dB over the noise floor), and a
second one 4-32 kHz away (a two-frequency ILS; VOR subcarrier sidebands
excluded); each carrier is cut out of the spectrum (+-12 kHz, less next to
a second carrier) -> envelope -> modulation m(t) = envelope / mean - 1 ->
its spectrum gives the depths of the 30, 90, 150, 400, 1020, 1300 and 3000
Hz tones and the 9960 Hz subcarrier. VOR: the subcarrier's band ->
analytic signal -> instantaneous frequency -> its 30 Hz phase (the
reference) minus the 30 Hz AM phase (the variable) = the radial; all
zero-phase, so no filter delays to correct; averaged over a few seconds.
Idents / marker keying: 10 ms Hann-windowed Goertzel ticks of each tone,
an adaptive on / off threshold (98th / 15th percentile of 30 s), Morse
from `lib/navaid.cpp` (started after a clear gap once the threshold is
known). The type needs three agreeing blocks (a marker also 75 MHz when
the frequency is known); each 1 s block of a recognised navaid is a valid
frame with its samples (Digital squelch). CPU (Pi 5): 0.7 % of a core.

## ID separation (DoA per navaid)
A localizer or VOR names itself in Morse. Every analysed block of the
recognised navaid is a talker packet "<ident> LOC" / "<ident> VOR" while its
ident was heard within the last 30 s on the same carrier (within 50 Hz of
where it was heard, so another navaid on the channel doesn't inherit it):
the middle 0.24 s of the block, filtered to the carrier (LOC 4 kHz, VOR 22
kHz); averaged over 10 s, kept 1 h. 🧭 rows in the 🗺 Map's 📡 DF panel.
Glide slopes and marker beacons send no ident: no talker. Test generator:
`tools/ils_gen.py` (LOC with DDM / VOR with radial, keyed 1020 Hz ident).
Two VORs sharing a channel (WN at 50°, then OH at 215°, 300 / 380 Hz
offsets): 50.0 / 215.0° (`--channels 5 --uca 0.12 --rf 114e6`).

## Options

- **Magnetic variation** (degrees east, west negative): the VOR bearing is
  also shown as true (as the DoA shows it)

## Tests (synthetic, ICAO definitions; one real VOR)

- Localizer 110.30 MHz DDM -0.050 + ident IAA: DDM -0.050, SDM 40.0 %,
  "right of the course - 49 uA fly left", IAA; paired glide slope 335.00
  MHz, DME 40X
- Two-frequency localizer (course -4 kHz DDM +0.030, clearance +4.5 kHz 8
  dB weaker DDM +0.300): +0.030 / +0.299, 8 dB, ident IBQ
- Glide slope 335.00 MHz DDM -0.100: -0.100, below, SDM 79.7 %
- VOR radials 0 / 45 / 90 / 180 / 237.5 / 300.3°: exact; 30 Hz at 29.8 /
  30.2 Hz: exact; a weak VOR (52 dB-Hz): 0.2° off; at 44 dB-Hz it is not
  recognised
- Markers: OM "dashes, 2.0 /s", MM "dots and dashes", IM "dots, 6.0 /s"
- A plain AM carrier and noise: no navaid

Real recording (2026-10-16; verified against it, not in this repo): the
sigidwiki "VOR" IQ (`Vor_IQ.zip`, 61 s, 64 kS/s 8-bit, station not named):
recognised as a VOR from 2 s on, 119 valid blocks, carrier 48 dB over the
floor, 30 Hz 23.6 % / 9960 Hz 28.7 % (FM +-377 Hz), voice on the carrier
(6 %), radial 295.0°. An independent Python measurement (AM envelope -> 30
Hz variable vs the 9960 Hz subcarrier's FM -> 30 Hz reference, 10 s
blocks): 295.1-296.6° - the radial's sense and value agree; the generator's
VOR at radial 295° reads 295.1-295.6° through the same script (modulation
30.1 / 29.7 %, +-485 Hz). Its ident: the 1020 Hz keying sits under voice /
ATIS audio; with the keyer's 10 ms Hann ticks (+-100 Hz) the key stood only
5.8x over the gaps (it wants 6) and never engaged. The 1020 Hz level is now
taken over 40 ms (Hann, centred on each 10 ms tick: +-25 Hz): 7.7x, keyed
("ident tone 9 %"), and on a noisy synthetic localizer IAA reads 2x where
the old keyer read nothing (clean LOC / VOR idents 6-12 wpm unchanged).
The real VOR's ident still isn't read: its voice key-ups (10-50 ms blips,
`ILS_KEY=1` prints the keying) break the gaps the Morse decoder needs - a
20 Hz-wide check shows ".- .-" groups every ~7-8 s, but the station is
unnamed, so it can't be confirmed; nothing false is shown. No real localizer, glide slope or marker beacon recording was
found: those stay synthetic only.

Not yet tried on live signals.
