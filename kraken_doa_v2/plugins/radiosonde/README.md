# Radiosonde decoder plugin

Weather-balloon radiosondes on 400-406 MHz, every supported type at once on
one VFO (48 kHz input, a VFO of at least 24 kHz):

| Type (family) | Models shown | Signal | What it gives |
|---|---|---|---|
| Vaisala `RS41` | `RS41`, then the variant the sonde sends (`RS41-SG`, `RS41-SGP`, `RS41-SGM` ...) | GFSK 4800 bit/s, whitened, 2 x RS(255,231) + a CRC per sub-block | serial, frame counter, GPS time + ECEF position / velocity, satellites, battery, TX frequency, mainboard + firmware, kill / burst timers, calibration progress; with the calibration table complete: air temperature, humidity, humidity-sensor temperature, pressure (SGP); auxiliary instrument data (hex); RS41-SGM encrypted frames still give serial, counter, battery |
| Graw `DFM` | `DFM-06`, `DFM-09`, `DFM-17`, `DFM-09P`, `DFM-17P`, `DFM-06P`, `PS-15` | 2-FSK 2500 chips/s Manchester, Hamming(8,4), continuous | serial (from the config channels), GPS date / time, position, speed, direction, climb, satellites, air temperature, battery, CPU temperature, frame counter |
| Meteomodem `M10` | `M10`, `M10+`, `M2K2` | 2-FSK 9615 chips/s biphase-mark | serial, GPS time, position + velocity (with a fix), satellites, air temperature, battery, humidity (approximate model) |
| Meteomodem `M20` | `M20` | 2-FSK 9600 chips/s biphase-mark | serial, GPS time, position + velocity, air temperature, humidity, humidity-sensor temperature, pressure, battery |
| InterMet `iMet-4` | `iMet-4` | FM + Bell 202 AFSK 1200 baud | GPS time of day, position (+ velocity from GPSX), satellites, PTU / PTUX (temperature, humidity, pressure, internal / sensor temperatures, battery), ozone XDATA |
| InterMet `iMet-54` | `iMet-54` (`iMet-50`) | 2-FSK 4800 bit/s 8N1, interleaved Hamming(8,4) | serial, time of day, position, temperature, humidity, humidity-sensor temperature, pressure (if fitted); speed / direction / climb derived from successive fixes |
| Lockheed Martin `LMS6` | `LMS6-403` (`LMS6-403-2`, `LMSX-403`) | 2-FSK 4800 sym/s, CCSDS K=7 r=1/2 + RS(255,223) | serial, frame counter, time of day, position, east / north / vertical speed (no PTU on the air) |
| Meteo-Radiy `MRZ` | `MRZ` | 2-FSK 2400 chips/s Manchester | serial (sonde + sensor serial), date + time, ECEF position + velocity, satellites, temperature, humidity, pressure (if fitted) |

Not supported: RS92 (its GPS needs downloaded ephemeris), the 1680 MHz sondes
(LMS6-1680, RS92-NGP), Meisei iMS-100 / RS-11G, Meteosis MTS01, Windsond and
the Chinese types. An MTS01 recording gives nothing.

## Use

Put a VFO (>= 24 kHz, e.g. 50 kHz) on the sonde's frequency and pick
**Radiosonde**. Each sonde (type + serial) gets a row in the decoder tab's
table (type, serial, altitude, climb, temperature, humidity, pressure -
measured, else from the standard atmosphere -, wind, distance), a 🎈 point on
the 🗺 Map (kept 3 h: landed sondes go on transmitting) and, for the latest
one, the facts: position, altitude (+ maximum), climb, flight phase
(ascending / descending / burst / landed / on the ground), temperature,
humidity, dew point, pressure, wind (the direction the sonde drifts from),
sensor temperatures, battery, satellites, TX frequency, distance and bearing
from the station. Events: a new sonde, every standard pressure level passed
on the way up (1000 ... 1 hPa, "(est.)" when the sonde measures no pressure),
the burst, the landing. Facts, table and map are refreshed at most every
0.75 s per sonde - every frame of a sonde that sends one every 0.8 s or more
(LMS6, DFM), about once a second for MRZ's repeats. Raw frames for the decoder data log (`TYPE SERIAL HEX`) only
while the log asks for them.

Offline:

```
decoder --file rec.cf32 --format cf32 --rate 96000 --station -36.85,174.76 [--verbose] [--raw]
SONDE_DUMP=1 decoder --file ...        # one FRAME line per decoded frame on stderr
```

## How it works

- Two channel filters on the 48 kHz input (Blackman windowed sinc: 41 taps
  +-7 kHz for the 2400-4800 Bd types, 31 taps +-12 kHz for the Meteomodems'
  9600 chips/s and the iMet-4's AFSK), an FM discriminator after each.
- `fsk_rx.cpp`: one receiver per family (two for the Meteomodems, two for
  LMS6 / LMSX). A one-symbol running mean is read at fractional positions by
  6 or 8 timing hypotheses on FIXED grids (letting a tracking loop move the
  search grids pulls them onto noise between frames, and the next sync is
  missed). Each slices against its own running level; a match of the frame
  sync - or its complement, an inverted spectrum - within a few errors gives
  the levels from the sync's own samples (centre = the carrier offset,
  reported to the host's AFC; the deviation's sign = the polarity), then a
  capture of the frame on its own clock with a Gardner timing detector at
  symbol changes. A frame caught by several hypotheses is taken once.
- `rscode.cpp`: Reed-Solomon over GF(2^8) (Berlekamp-Massey, Chien, Forney)
  for the RS41 (0x11D, roots 0..23) and CCSDS (0x187, beta = alpha^11, roots
  beta^112..143) codes; LMS6's convolutional code through the SDK's soft
  Viterbi, started from the known zero tail + ASM.
- One file per family: `rs41.cpp`, `dfm.cpp`, `modem.cpp` (M10 / M20),
  `imet4.cpp`, `imet54.cpp`, `lms6.cpp`, `mrz.cpp`; shared conversions in
  `sonde_util.cpp` (GPS time, WGS-84, ICAO standard atmosphere, Magnus dew
  point, Hyland-Wexler saturation pressure, CRCs).
- `decoder.cpp`: merges each frame into its sonde's state, derives velocity
  for types without a velocity field (from the fix 3-10 s back, 5+
  satellites), runs the flight events and publishes.

## ID separation

Every frame of a sonde that passed its checks is a talker packet of its key
(`RS41 N3920808`, `DFM 637797`, `M20 911-2-00269` ... = the table / map key),
label model + altitude: its FIRST 0.24 s from the sync (the RS41 receiver
always captures the long-frame length, which can run past a short frame into
whatever transmits next), the whole VFO (the host's AFC centres the sonde),
15 kHz wide (24 kHz for M10 / M20 / iMet-4), averaged over 10 s, kept 3 h.
Several sondes on one frequency (they transmit in their own part of the
second) get their own bearings. iMet-4 sondes send no serial: they get no
talker (one id would merge them all).

## Tests

Recordings: the radiosonde_auto_rx test set (96 kHz complex float), decoded
with `--station -36.85,174.76`, compared frame by frame (`SONDE_DUMP`) with
the previous plugin (1.1). Offline decode time on the development PC (Ryzen 9
9950X3D): 0.6-0.9 % of real time (the previous plugin: 0.8-1.2 %).

| Recording | Frames (previous) | First | Result |
|---|---|---|---|
| rs41 | 119 (119) | 1.78 s | `RS41-SG N3920808`, frame 229, 2019-02-10 05:18:17 UTC, -34.72081, 138.69280, 69 m, climb -0.4, 8 sats, 3.00 V, TX 402.500 MHz, `RSM412 fw 20213`, `30600 s (burst kill on)`, `50/51 fragments`; T 26.2, TU 31.0, RH 38.4, dew point 10.9; raw frames byte-identical (119 / 119) |
| dfm09 | 104 (111) | 16.94 s | `DFM-09 637797` (N = 3, M = 0xA), frame 199, 2019-02-10 05:34:17 UTC, -34.72072, 138.69282, 99 m, T 60.1, internal 32.4, 5.90 V, 7 sats; all 104 frames equal to the previous (T identical in 104 / 104), raw text identical |
| m10 | 120 (120) | 0.35 s | `M10 803-2-10732`, no fix (0 sats), 2019-04-07 00:00:00 UTC, T 23.5, RH 50.0 + `Humidity model`, 5.74 V; raw identical |
| m20 | 120 (120) | 0.60 s | `M20 911-2-00269`, 2022-07-23 01:19:45 UTC, -34.72079, 138.69277, 95 m, T 19.8, TU 20.2, RH 62.7, 1010.4 hPa measured, 2.80 V; raw identical |
| imet4 | 119 (119) | 0.93 s | `iMet-4 iMet`, frame 192, 05:39:44 UTC, T 26.4, RH 39.6, 1001.8 hPa, TU 26.8, internal 29.7, 5.90 V, `29.70 °C`, 4 sats, -34.72072, 138.69284, 84 m (last 9 frames); no talkers; raw identical |
| imet54 | 241 (241) | 0.62 s | `iMet-54 55064062`, 23:58:00 -> 00:02:01 UTC, -25.90310, 28.33486, 1518 m, T 16.7, TU -65.0, RH 0.1, derived climb 0.0 / wind from 180° at 0.0 km/h; every frame equal; raw identical |
| lms6-400 | 120 (120) | 2.59 s | `LMS6-403 8097164`, frame 523, 02:15:59 UTC, 37.77813, -77.60924, 97 m, climb 0.1; every frame equal; raw identical |
| mrz | 71 (49) | 13.60 s | `MRZ 5667-39155`, 2021-02-22 12:18:14 UTC, 54.68138, 21.22008, 14323 m, climb 5.9, T -61.0, RH 15.3, 20 sats, wind from 303° at 37.1 km/h, 134.0 hPa from altitude; the 49 frames of the previous plugin all found with equal values, 22 more repeats decoded |
| mts01 | 0 (0) | - | nothing |
| 60 s noise, FM / FSK interferer | 0 | - | nothing |

Expected differences from the previous plugin (the permitted sources'
formulas, see the design notes):

- RS41 humidity -0.77 %RH on average (69 frames, -0.70 .. -0.80): the
  humidity-sensor temperature is the plain sensor polynomial (31.0 °C, was
  31.3) and is the temperature the saturation-pressure ratio uses; RH 38.4 %
  at the last frame (was 39.2), dew point 10.9 °C (was 11.2). The air
  temperature needs all six correction coefficients (calibration fragment 7,
  frame 161 here): the first T comes one frame later than before (frame 161,
  was 160); every later T is identical.
- M10 battery x 0.00668 V per count: +0.05 V (5.74 V at the end, was 5.69).
- M20 battery x 3/228 V: +0.04-0.05 V (2.80, was 2.76); humidity-sensor
  temperature by its Steinhart-Hart fit 20.2 °C (was 19.8); RH -0.1 %RH
  (62.7, was 62.8; one frame -0.2).
- iMet-4 time of day as sent (GMT): +18 s (05:39:44, was 05:39:26).
- DFM: the serial (and so the first record) from the config channels'
  structure: N after two equal measurements, M at the first wrap after the
  highest slow channel with a split serial, the serial after one complete
  number - 16.9 s here (the previous plugin: 9.8 s), so the first 7 records
  of the recording are not shown.
- MRZ: the 48-chip sync `AA BF 35` decodes more of the repeated packets (71
  vs 49 records); every packet the previous plugin decoded comes out equal.
- Carrier offsets differ by a few tens of Hz (the levels of the sync
  symbols); nothing depends on them offline.

Generated scene (`tools/sonde_gen.py` through `sdk/tools/make_testsignal.py
radiosonde-rs41-m20 --lat -36.85 --lon 174.76`; 5 antennas, UCA 0.12 m,
`decoder --file scene.dat --format cf32 --rate 48000 --channels 5 --uca 0.12
--rf 403000000`): 24 / 24 frames (first at 0.930 s), both talkers 12 / 12
packets, `M20 512-2-14077` at 250.0°, `RS41 U2410853` at 40.0°; facts as the
playback entry describes (`2026-10-15 22:14:53 UTC`, -36.69833, 174.60238,
4257 m, 5.2 m/s ascending, 596.0 hPa from altitude, wind from 270° at 28.8
km/h, 2.90 V, 22 km at 320°, `12/51 fragments`, frame 5156, 9 sats, 403.000
MHz), M20 row 11854 m, 19.7 °C, RH 63, 1010.5 hPa, 2.79 V. The previous
plugin decodes this generator's scene identically (24 / 24, 40.0° / 250.0°,
every per-frame value equal), and this plugin decodes the previous
generator's scene identically too.

Unit checks (scratch harness, not in the repo): the CRCs' catalogue check
values, the Meteomodem check (0x64 -> 0x003E, "123456789" -> 0xF6D4), both
Reed-Solomon codes (2000 random codewords each with up to t errors restored,
t + 1 refused), the previous plugin's 119 RS41 frames from the recording with
up to 12 random byte errors per codeword (4760 / 4760 restored), the
generator's RS41 frames with no correction needed, RS41 518-byte extended
frames (+ auxiliary data) and RS41-SGM encrypted frames with byte errors, the
LMS6 ASM's coded symbols, the DFM and iMet-54 Hamming tables (every single
bit error corrected, every two-bit error refused), WGS-84 round trips
(to 1e-9 m), the standard atmosphere's layer bases, Magnus and Hyland-Wexler
values, and a simulated flight (ascent at 5 m/s to 30 km: 15 standard levels;
burst, descent, landing) plus velocity derivation across midnight. Sensitivity on the
generator's signals in white noise equals the previous plugin's (RS41 30 / 30
frames down to 2 dB SNR per 48 kHz sample, 19 / 30 at 0 dB; M20 28 / 30 at 4
dB, 6 / 30 at 2 dB).

Not tested on a recording (none available): the RS41 0x82 / 0x83 blocks of
newer firmware, DFM-06 / DFM-17 / P models / PS-15, iMet-54 frames with the
documented CRC-32, the LMS6-403-2 sync variant, and the LMSX-403 receiver.

## How it was written, and its sources

Version 2.0 is a clean-room rewrite (2026-10-22): version 1.x contained code
translated from rs1729/RS (GPL-3.0). A spec writer described 1.x's behaviour
and the sondes' formats from permitted sources, without code; a separate
implementer wrote 2.0 from that description only, without reading 1.x,
rs1729/RS, radiosonde_auto_rx or any other GPL decoder; a similarity audit
found no rs1729 code expression in it.

- **ra-firmware** (DF9DQ, github.com/einergehtnochrein/ra-firmware,
  BSD-3-Clause) - frame layouts, sync words, whitening, coding and the sensor
  models of RS41, M10 / M20, iMet-4 (CRC seed), iMet-54, LMS6 and MRZ, and
  the DFM channel structure. The RS41 temperature / humidity / pressure code
  (`rs41.cpp`) and the M10 / M20 sensor code (`modem.cpp`) follow its
  metrology code closely and carry its BSD-3 notice (also in
  `../../THIRD_PARTY_NOTICES.md`).
- NOAA "iMet-1-RSB Radiosonde Protocol" v1.11 (J. Wendell, A. Jordan, 2009) -
  the iMet-4 packets.
- Standards: CCSDS 131.0-B (convolutional code, ASM, RS(255,223)), WGS-84,
  ICAO Doc 7488 / US Standard Atmosphere 1976, WMO-No. 8 (Magnus), Hyland &
  Wexler (1983), IERS Bulletin C (GPS - UTC = 18 s), the CRC catalogue.
- **Facts first published by the rs1729/RS project** (its reverse
  engineering of the sondes; GPL-3.0 code - none of it is used, these are
  numbers, layouts and rules of the sondes): the M10 humidity model
  (`Humidity model: approximate (M10)`), the M20 main air temperature from the
  M10 NTC network (2 MOhm on range 2), the M20 pressure fraction byte of
  firmware >= 7, the DFM temperature fit (Steinhart-Hart, 220 kOhm / 332 kOhm
  on DFM-17, channels 1 / 5 / 6 on P models - kept instead of ra-firmware's
  beta model, which is 2-7 K off from -20 to -80 degC) and the DFM model
  rules (`DFM-09P` / `DFM-17P` / `DFM-06P`, serial >= 23000000 -> `DFM-17`),
  the iMet-54 status flags check and `iMet-50`, the `24 54 00 05` sync
  variant (`LMS6-403-2`), the LMSX-403 layout (untested), and the receivers'
  symbol rates 9615 (M10), 4798 (iMet-54), 2399 (MRZ), 4797.8 (LMSX).
- Other facts kept from version 1.x (the same community's findings): the M10
  type bytes 0xAF / 0x8F (`M10+`, `M2K2`), the Meteomodem GPS week rollover
  rule and the M10 leap-second byte, the M20 serial text (`512-2-14077` =
  bytes `C7 B4 BF`: the number's leading digit in bit 23 - the serial other
  decoders report), the DFM GPS seconds and frame counter, the RS41 0x82
  hundredths of a second, the LMS6 east / north velocity and 24-bit serials.

The iMet-54 on the recording sends no CRC that matches the documented CRC-32
(nor any other variant): its frames are accepted when every codeword of the
decoded part is error-free (or the documented CRC passes), the status flags
look right and time and position are in range.

## Files

`decoder.cpp` (plugin, sonde state, publishing), `families.hpp`,
`sonde.hpp` / `sonde_util.cpp`, `fsk_rx.hpp` / `fsk_rx.cpp`, `rscode.hpp` /
`rscode.cpp`, `rs41.cpp`, `dfm.cpp`, `modem.cpp`, `imet4.cpp`, `imet54.cpp`,
`lms6.cpp`, `mrz.cpp`; `tools/sonde_gen.py` (test signal: an RS41-SG or an
M20 around the station), `tools/testsignals.json` (debug playback
`radiosonde-rs41-m20`).
