# UAT plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**UAT (`plugins/uat/`, 978 MHz, README.md there):**
- The US's second ADS-B link + its ground stations' uplink: `sample_rate`
  2.4 MHz, `min_vfo_rate` 2 MHz, `fixed_freq_hz` 978 MHz (like adsb),
  manual only, talkers, map. Not used in NZ: tested on synthetic signals
  and on dump978's sample data (real payloads) re-modulated
- CPFSK 1.041667 Mbit/s, h 0.6: 2.304 samples per bit. Gate = 32-sample
  block power > 2x the noise floor (lowest 1024-sample mean, smoothed); in
  a hot stretch the discriminator (fast atan2) smoothed over 2 samples,
  linearly interpolated at bit centres, correlated with the 36-bit sync
  (+ = ADS-B 0xEACDDA4E2, - = uplink, its complement); peak refined to 0.1
  sample, levels from the sync (<= 4 bit errors), Gardner timing loop (mu
  0.03 sample per transition; +-80 ppm clocks fine)
- RS (`rs.cpp` = radiosonde's generic GF(256) decoder; on-air byte i =
  x^(n-1-i), a "correction" into the shortened zeros = failure): long
  RS(48,34) with type != 0 first, then basic RS(30,18) type 0; uplink 6 x
  RS(92,72) de-interleaved (on-air 6 i + b). poly 0x187, fcr 120, prim 1
- `msg.cpp`: header / state vector / mode status / aux SV (dump978's field
  positions = DO-282B's), uplink header + information frames + FIS-B APDU
  header (product id, t_opt time), DLAC text -> records (`text_records`),
  product names. `UAT_DUMP=1` (offline) logs every message's fields
- Outputs: table (Address, Source, Callsign, Category, Squawk, altitudes,
  V/S, GS, Track, Lat / Lon, distance / bearing, Air / ground, Emergency,
  NIC / NACp, Signal, Messages; details: mode status / ground station
  products + latest text records), map (aircraft, vehicle, point, ground
  station kind station), events (new aircraft / TIS-B target / station,
  callsign, first position, products, ⚠ emergencies + unusual categories),
  raw = dump978's text format `-hex;rs=;ss=` / `+hex;...`. Options `tisb`
  (show TIS-B / ADS-R traffic), `fisb` (text records in the event log)
- ID separation: see *DoA per talker* (aircraft by address; TIS-B / ADS-R
  and uplinks = the ground station "GS site N")
- Generator `tools/uat_gen.py` (scene JSON: adsb / tisb / uplink sources or
  `replay` of a dump978 text file; real RS, CPFSK + Gaussian BT 0.5, clock
  ppm, carrier offset, 5-element UCA, 0.1 s pieces, cu8 / cf32);
  `tools/scene_five.json`
- Tests (scratch t4/): dump978 sample data (1143 real messages) re-modulated
  -> 1143 decoded byte for byte; the generator's RS = Karn's decoder with
  dump978's parameters on 9326 codewords; fields vs dump978's uat2text
  (built as a reference): 439 / 439 ADS-B identical (speed +-1 kt
  rounding), 704 / 704 uplinks, 87 / 88 text records. Sensitivity: ADS-B
  99 % from 6 dB per-sample SNR (2.4 MHz), uplinks 91-100 % from 6-8 dB;
  noise nothing. scene_five offline + end to end: 5 talkers (4 aircraft,
  the ground station incl. its TIS-B) 0.0 deg off; UI headless Chromium
  (✈ / 🗼, the station's details + METAR, DF error 0.0). CPU 2.1 % of a
  core live (end to end)

