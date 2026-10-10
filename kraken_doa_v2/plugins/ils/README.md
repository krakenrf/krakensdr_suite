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

## Options

- **Magnetic variation** (degrees east, west negative): the VOR bearing is
  also shown as true (as the DoA shows it)

## Tests (synthetic, ICAO definitions)

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

Not yet tried on live signals.
