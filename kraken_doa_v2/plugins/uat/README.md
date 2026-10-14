# uat - UAT (978 MHz Universal Access Transceiver)

The United States' second ADS-B link: general aviation below 18 000 ft
broadcasts its position on 978 MHz instead of (or as well as) 1090 MHz, and
the FAA's ground stations uplink weather / NOTAMs (FIS-B) and rebroadcast
the traffic radar and 1090 MHz ADS-B see (TIS-B, ADS-R). The plugin reads
all of it in the 2.4 MHz around 978 MHz and gives every aircraft and every
ground station its own bearing.

Pick "UAT (978 MHz)" as a VFO's digital decoder: like `adsb` it tunes the
VFO and its tuner to 978 MHz at 2.4 MHz (`kp::Info::fixed_freq_hz`), the VFO
a locked line; manual only. UAT is used in the US only (not in New Zealand,
where this was written: no live signal was available - see Tests).

## What it reads

- **Physical layer** (RTCA DO-282B, as publicly described): binary CPFSK,
  1.041667 Mbit/s, modulation index 0.6 (a 1 = +312.5 kHz) - 2.304 samples
  per bit at 2.4 MHz; a 36-bit sync, 0xEACDDA4E2 for ADS-B and its
  complement 0x153225B1D for the ground uplink.
- **FEC**: Reed-Solomon over GF(256), field polynomial 0x187, roots
  α^120.. (systematic, parity after the data): basic ADS-B RS(30,18) (6
  byte errors corrected), long ADS-B RS(48,34) (7), uplink 6 × RS(92,72)
  (10 each) byte-interleaved (on-air byte 6 i + b = byte i of block b).
  `rs.cpp` is the generic decoder of `plugins/radiosonde` (a copy).
- **ADS-B** (`msg.cpp`): header (payload type 0-10, address qualifier,
  24-bit address); state vector - latitude / longitude (23 / 24 bits, 360 /
  2^24°), barometric or geometric altitude (25 ft), NIC, air / ground
  state, N/S and E/W velocity (4 × on supersonic), vertical rate (64 ft/min,
  baro / geo), on the ground speed, track / heading and length / width,
  UTC coupling or the TIS-B site ID; mode status (payload types 1 and 3) -
  emitter category (A0-C7, as on 1090 MHz), 8-character callsign in base 40
  (or, CSID 0, the flight plan ID = squawk), emergency / priority, UAT
  version, SIL, NACp / NACv, NIC baro, CDTI / ACAS / RA / IDENT / ATC bits,
  true / magnetic heading; auxiliary state vector - the other altitude.
- **Address qualifier**: 0 ADS-B (ICAO address, id `A1B2C3`), 1 ADS-B
  self-assigned (`~A1B2C3`), 2 / 3 TIS-B (ICAO / track file), 4 surface
  vehicle, 5 fixed beacon, 6 ADS-R, 7 reserved.
- **Uplink** (432 bytes): the ground station's latitude / longitude
  (position-valid flag), UTC coupled, application data valid, slot ID (the
  ground segment slot it sends in), TIS-B site ID; then information frames
  (9-bit length, 4-bit type) holding FIS-B APDUs: product ID, time (hh:mm,
  + seconds / month and day per its time option), segmentation flag, data.
  Text products (413 and 20-27: METAR, TAF, PIREP, winds aloft) are DLAC
  decoded (6-bit characters, 4 in 3 bytes, tab runs, record separators) into
  records; graphical products (NEXRAD, icing, turbulence, lightning, cloud
  tops, SUA, NOTAM / TFR graphics) are named and counted, not drawn.

Demodulator (`decoder.cpp`): |x|² in 32-sample blocks against the noise
floor (the lowest 1024-sample mean, smoothed) gates the work - an empty band
costs little more than that; in a hot stretch the FM discriminator (a fast
atan2 of x[n] x*[n-1]) smoothed over two samples is sampled at fractional
bit centres and correlated with the sync (positive = ADS-B, negative =
uplink), the peak refined to 0.1 sample, the sync's own levels give the
slicing centre and deviation (≤ 4 sync bit errors), a Gardner loop follows
the bit clock (an uplink is 4.3 ms), RS decides: long ADS-B first (type
≠ 0), then basic (type 0) - as the standard's receivers do.

## Output

- Table (one row per aircraft / ground station): Address, Source (ADS-B,
  vehicle, beacon, TIS-B site n, ADS-R), Callsign, Category, Squawk,
  altitudes, V/S, GS, Track, Lat / Lon, distance / bearing from the
  station, Air / ground, Emergency, NIC / NACp, Signal, Messages; details
  (▸) with the mode status fields - and for a ground station its site,
  slot, position, the FIS-B products (count, product time) and the latest
  text records (METAR KSJC ...).
- 🗺 Map: aircraft (vehicles, beacons / obstacles as their kinds) incl.
  TIS-B / ADS-R targets, ground stations (kind station).
- Events: new aircraft / TIS-B target / ground station, callsign, first
  position, the ground station's FIS-B products, ⚠ emergencies (emergency
  status or squawk 7500 / 7600 / 7700) and their end, ⚠ unusual categories
  (highly manoeuvrable, balloon, parachutist, ultralight, UAV, space).
  Option "FIS-B text in the log": every new text record as an event.
- Raw frames (decoder data log): dump978's text format, `-<hex>;rs=N;ss=dB`
  / `+<hex>;...` - the corrected payloads.
- Options: TIS-B / ADS-R traffic (show / hide), FIS-B text in the log.

## ID separation

- Every ADS-B message an aircraft / vehicle / beacon sends ITSELF (address
  qualifier 0, 1, 4, 5) is a `kp::Talker` packet of its address (= its table
  row and map point), span = sync to the last parity bit (265 / 403 µs), the
  whole band (the signal is 1.3 MHz wide), `avg_s` 5 (one message a second),
  `keep_s` 10 min; label "CALLSIGN · 4500 ft" like adsb. A message with an
  accurate position (NACp ≥ 8) carries it AT THE PACKET'S TIME (`lat` /
  `lon` / `alt_m`: UAT sends the position in the very message) - the array
  calibration can use it.
- TIS-B / ADS-R messages describe other aircraft but are TRANSMITTED by a
  ground station: they are packets of that station ("GS site 11" - the TIS-B
  site ID in the message), never of the aircraft they describe. Its uplink
  frames (4.3 ms) are packets of the same station ("GS site N" from the
  uplink's TIS-B site ID; "GS slot S" when it sends none), `avg_s` 20,
  `keep_s` 30 min, with the station's position from its uplink header. ADS-R
  / reserved qualifiers carry no site ID: no talker.
- UI: aircraft are kind ✈ like adsb's; ground stations kind `gs` 🗼
  ("Station / UAT ground station · slot / Frames", position columns -
  the DF error against the uplinked station position).

## Tests

All with `tools/uat_gen.py` (real framing, RS, CPFSK h = 0.6 with a
Gaussian BT 0.5 frequency pulse, sub-sample timing, clock / carrier offsets,
transmitters at known bearings on the 5-element UCA, 8-bit like the
receiver) - scratch scripts in the agent's job folder:

- **Real payloads**: dump978's sample data (1143 messages captured off the
  air in the US: 439 ADS-B incl. TIS-B, 704 uplinks; GPL repository, used as
  test data only, not in this repo) encoded and modulated by the generator:
  1143 / 1143 decoded with the payload identical byte for byte. The
  generator's RS parity checked against Phil Karn's RS decoder with
  dump978's parameters: 9326 / 9326 codewords clean, every 1..t-error copy
  corrected back (so the parity is the real one). Fields against dump978's
  `uat2text` (built in scratch as a reference) on the same messages: 439 /
  439 ADS-B messages identical in every field (ground speed ±1 kt: we
  round, it truncates), 704 / 704 uplinks (station, slot, site, every
  product ID), 87 / 88 text records identical (the 88th only cut by our
  dump's 300-character limit).
- **Sensitivity** (single antenna, SNR per sample over the 2.4 MHz;
  + 2.7 dB = in the signal's 1.3 MHz): ADS-B 45 % at 4 dB, 99-100 % from 6
  dB; uplinks 0 % at 4, 91-97 % at 6, 100 % from 8 dB - the same with +50 /
  -80 ppm bit clock and ±30-40 kHz carrier offset. 10 s of noise: nothing.
- **ID separation** (`tools/scene_five.json`, 8 s): two ICAO aircraft, a
  self-assigned one (squawk only), a surface vehicle and a ground station
  (uplinks + TIS-B of a third aircraft) at 60 / 150 / 250 / 320 / 200°:
  offline (`decoder --file five.cu8 --channels 5 --uca 0.12 --rf 978e6`)
  every talker 0.0° off, the TIS-B packets under "GS site 11"; end to end
  (fake heimdall streaming the scene into kraken_doa, DF status) 0.0° for
  all five (336 packets, 333 with a bearing). UI (headless Chromium): ✈ /
  🗼 rows, callsign names, the station's details with its METAR, DF error
  against the reported position 0.0°.
- **CPU** (Pi 5): plugin 2.1 % of a core in the end-to-end run (the 19 MB/s
  pipe included); offline 7 % of real time on noise (the offline host's own
  mixing / file reading is most of it).
- **Live** (Auckland, the real array on 978 MHz for 2.5 min): no UAT, as
  expected outside the US - and no false message.

## Limits

- No live signal here (New Zealand): only synthetic transmissions and real
  payloads re-modulated - the modulation of real transmitters (their
  pre-modulation filter, ramp-up) is the standard's, as described publicly.
- RS decoding is hard-decision (no erasures).
- FIS-B graphics (NEXRAD etc.) are not drawn; product 413's text is shown
  as records, not parsed further.
- dump978 (GPL) served as documentation and test reference only; no code of
  it is in this plugin.
