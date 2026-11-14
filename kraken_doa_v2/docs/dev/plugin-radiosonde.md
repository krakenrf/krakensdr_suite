# Radiosonde plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Radiosonde (`plugins/radiosonde/`, weather balloons; README.md there) - v2.0, a CLEAN-ROOM rewrite (2026-10-22):**
- Why: v1.x held code translated from rs1729/RS (GPL-3.0: rs41.cpp's
  temperature / humidity / pressure and more). A spec writer read v1.x +
  permitted sources (ra-firmware BSD-3, NOAA's iMet-1-RSB protocol,
  standards) and wrote a no-code spec; a separate implementer wrote v2.0 from
  it alone; a similarity audit (winnowing, k 12) against v1.x, rs1729/RS and
  ra-firmware: no rs1729 expression (<= 4.7 % of fingerprints, all formula
  lines / plain assignments - v1.x's translated rs41.cpp: 5.8 %). rs41.cpp's
  PTU and modem.cpp's M10 / M20 sensor code follow ra-firmware's metrology
  closely: they carry its BSD-3 notice (+ THIRD_PARTY_NOTICES.md text H).
  NEVER consult rs1729/RS, radiosonde_auto_rx, dxlAPRS or another GPL
  decoder's code for this plugin
- Kept on purpose (facts the rs1729 project first published, no code;
  README *How it was written*): the DFM Steinhart-Hart fit + model rules
  (ra-firmware's beta model is 2-7 K off from -20 to -80 degC), the M10
  humidity model, the M20 main air temperature via the M10 network, the M20
  serial printing (= other decoders' / SondeHub's), iMet-50, LMS6-403-2, LMSX
- Same public interface as v1.1 (Info, options, facts, table, events, map,
  talkers, raw, SONDE_DUMP, generator CLI) except the version. Recordings:
  RS41 119, iMet-4 119, iMet-54 241, LMS6 120, M10 120, M20 120 = v1.1, raw
  frames byte-identical; DFM 104 (shown from 16.9 s, once the serial is
  known; v1.1 111 from 9.8 s), MRZ 71 (v1.1 49); MTS01 / noise 0; the scene
  24 / 24 at 40.0 / 250.0. Values from ra-firmware's maths differ a little
  (RS41 RH -0.8 %RH, TU 31.0 vs 31.3, M10 / M20 battery +0.05 V, M20 TU
  20.2 vs 19.8, iMet-4 time +18 s = GMT as sent) - README test table
- Files: decoder.cpp (plugin, per-sonde state, events, publishing at most
  every 0.75 s - every record of a sonde sending one per >= 0.8 s),
  families.hpp, sonde.hpp / sonde_util.cpp (record, geodesy, ISA, humidity
  helpers), fsk_rx.* (the binary FSK frame receiver), rscode.* (RS
  GF(256): RS41 + LMS6 CCSDS), rs41.cpp, dfm.cpp, modem.cpp (M10 + M20),
  imet4.cpp (AFSK), imet54.cpp, lms6.cpp (+ LMSX), mrz.cpp
- 48 kHz in, min_vfo_rate 24 kHz; per sonde (type + serial) the latest value
  of every field: facts (one sonde: the latest), table row, map point kind
  "balloon" (ttl 3 h; the page
  keeps balloon trails 4 h / 4000 points), events: new sonde, standard
  pressure levels passed on the way up (pressure measured or ISA at the GPS
  altitude, "(est.)"), burst (300 m below the maximum > 3 km, descending),
  landed (30 s still after burst). Velocity for types without it from the
  fix 3-10 s back (5+ satellites). freq_error = the frame's DC (AFC)
- Tested on the radiosonde_auto_rx recordings (rfhead.net
  sonde_samples.tar.gz, 96 kHz cf32; results in the README) against v1.1's
  output; `SONDE_DUMP=1 decoder --file ...` prints every frame. The flight
  logic was tested with a simulated flight (15 levels, burst, landing)
- Not supported: RS92 (needs ephemeris), 1680 MHz types, Meisei, MTS01
- DoA per sonde (ID separation): every frame a
  packet of type + serial, its first <= 0.24 s - see *DoA per talker*
- Generator `tools/sonde_gen.py` (rewritten with the plugin; the same
  command line): RS41-SG (GFSK, whitening, the 320-byte block frame with
  CRCs + the two RS(255,231) codewords, keyed 0.6 s a second) and M20
  (biphase-mark 9600 Bd, the 70-byte frame + checks, 0.17 s a second);
  positions around the station given at generation time -> playback
  `radiosonde-rs41-m20` (RS41 U2410853 at 40 deg, M20 512-2-14077 at 250
  deg on 403.0 MHz): 24 / 24 frames, bearings 40.0 / 250.0, with the new
  decoder and with v1.1's

