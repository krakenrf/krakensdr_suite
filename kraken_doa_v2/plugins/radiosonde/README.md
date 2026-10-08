# Radiosonde decoder plugin

Weather balloon sondes on 400-406 MHz. Every supported type runs side by
side on the VFO's signal; the one whose frames pass their checks is shown.

| Type | Modulation | Checks | Position | Weather |
|---|---|---|---|---|
| Vaisala RS41-SG / -SGP / -SGM | GFSK 4800 Bd, whitened | Reed-Solomon (255,231) x2 + CRC-16 per block | ECEF + velocity (incl. the newer 0x82 GNSS block) | temperature, humidity (calibrated, sensor temperature), pressure (-SGP); -SGM: encrypted, shown as such |
| Graw DFM-06 / -09 / -09P / -17 / -17P | FSK 2500 Bd Manchester | Hamming(8,4), max. 1 bit error per codeword | lat / lon / alt + speed, direction, climb | temperature (NTC) |
| Meteomodem M10 / M10+ | FSK 9615 Bd Manchester, differential | 16-bit checksum | lat / lon / alt + velocity | temperature, humidity (approximate model), battery |
| Meteomodem M20 | FSK 9600 Bd Manchester, differential | 16-bit checksum | lat / lon / alt + velocity | temperature, humidity, sensor temperature, pressure, battery |
| InterMet iMet-4 / iMet-1-RS | Bell 202 AFSK 1200 Bd, 8N1 | CRC-16 per packet | lat / lon / alt (+ velocity, enhanced GPS) | pressure, temperature, humidity, sensor temperatures, battery, ozone instrument data |
| InterMet iMet-54 / iMet-50 | FSK 4800 Bd, 8N1, interleaved | Hamming(8,4) + CRC-32 | lat / lon / alt (velocity derived) | temperature, humidity, sensor temperature |
| Lockheed Martin LMS6-403 (LMSX-403) | FSK 4800 Bd, convolutional K=7 r=1/2 | Viterbi + Reed-Solomon (255,223) + CRC-16 | lat / lon / alt + velocity | (none sent) |
| Meteo-Radiy MRZ | FSK 2400 Bd Manchester | CRC-16 | ECEF or lat / lon + velocity | temperature, humidity |

## What it shows

- **Facts** (the decoder tab under the waterfall): the latest sonde's type
  and serial, frame, time, position, altitude (and the maximum reached),
  climb rate, flight phase, temperature, humidity, dew point, pressure
  (measured, or "from altitude" = the ICAO standard atmosphere at the GPS
  altitude), wind (the balloon's horizontal motion: "from 270° at 43 km/h"),
  sensor temperatures, battery, satellites, burst / kill timers, the
  configured transmit frequency (RS41), distance and bearing from the
  station, and type-specific extras (RS41 mainboard + firmware, calibration
  progress, aux / ozone instrument data).
- **Table**: one row per sonde heard (altitude, climb, temperature,
  humidity, pressure, wind, distance).
- **Events**: a new sonde; every standard pressure level passed on the way
  up (1000, 925, 850, 700, 500, 400, 300, 250, 200, 150, 100, 70, 50, 30,
  20, 10 ... hPa) with height, temperature, humidity, dew point and wind -
  a sounding, as weather services report it; "⚠ burst at ..."; "🪂 landed at
  ...".
- **Map** ("🗺 Plot on map" in the decoder tab): a balloon marker per sonde
  with every value above in its popup; the track of the whole flight.
- Types without a velocity field (iMet-54, iMet-4's basic GPS packet) get
  speed, direction and climb from the fix 3-10 s earlier (not from fixes
  with fewer than 5 satellites: a receiver still converging moves tens of
  metres).

## Use

A VFO of 24-50 kHz on the sonde's frequency (400-406 MHz; local launch sites
publish theirs, or watch the waterfall around launch times - most stations
launch at about 23:15 and 11:15 UTC), Digital decoder "Radiosonde" or Auto
detect. RS41 sondes also report the frequency they're set to ("TX
frequency"). The plugin reports the carrier offset, so kraken_doa keeps a
drifting sonde centred. DFM shows a sonde once its serial number has been
received (a few seconds); RS41 temperature / humidity need the calibration
table, which arrives in 51 fragments over about a minute ("Calibration
n/51").

## Not supported

RS92 (raw GPS - needs downloaded ephemeris), the 1680 MHz sondes
(LMS6-1680 / Mk2a, RS92-NGP), Meisei iMS-100 / RS-11G, MTS01, Weathex,
Chinese types. RS41-SGM military sondes encrypt their payload. The M10
humidity model is approximate; the iMet-4 sends no serial number
(shown as "iMet"); LMSX-403 and DFM-06 follow the protocol descriptions but
had no test recording.

## Sources

Independent implementation from the protocol descriptions published by the
open-source radiosonde community: rs1729/RS (decoders for every type above,
GPL-3.0 - used as documentation and, in testing, as the reference to compare
against; no code taken), DF9DQ (RS41 calibration model), InterMet's
"Binary Radiosonde Packet Definition" (iMet-1-RS / iMet-4).

## Tests

The radiosonde_auto_rx test recordings
(https://rfhead.net/sondes/sonde_samples.tar.gz, 96 kHz complex float,
`decoder --file x.bin --rate 96000`), compared frame by frame with the
rs1729 decoders (`SONDE_DUMP=1` prints every frame):

| Recording | Frames (reference) | Agreement |
|---|---|---|
| RS41-SG N3920808 | 119 (118) | position, temperature, humidity (advanced model), battery identical |
| DFM-09 637797 | 109 (93) | position, temperature, battery identical |
| M10 803-2-10732 | 120 (120) | temperature, battery identical (no GPS fix in the recording) |
| M20 911-2-00269 | 120 (120) | position, temperature, humidity, pressure, battery within print rounding |
| iMet-4 | 119 (119) | PTU within print rounding; fixes with 4+ satellites identical |
| iMet-54 55064062 | 241 (240) | position, temperature, humidity identical |
| LMS6-403 8097164 | 120 (121) | position and velocity within print rounding |
| MRZ 5667-39155 | 49 (20 dated) | position, temperature, humidity within 0.1 |
| MTS01 (unsupported), 60 s of noise | 0 | no false frames |

Each recording decodes only as its own type. CPU: 2.6 % of real time on a
Raspberry Pi 5 (3.8 % with LMS6's Viterbi decoding running).
