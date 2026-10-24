# RDS / RBDS + SCA plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**RDS / RBDS + SCA (`plugins/rds/`, README.md there; the user's request 2026-10-22):**
- FM broadcast subcarriers: `sample_rate` 0 (the VFO's own rate: 240 kHz
  for RDS, 300-400 kHz for a 92 kHz SCA; > 480 kHz decimated, 300-480 kHz
  channel-filtered +-120 / 175 kHz by the plugin), `min_vfo_rate` 150 kHz,
  `voice`, `talkers`, `multichannel` (every antenna combined BEFORE the
  discriminator: main eigenvector of the covariance, 0.25 s, interpolated
  per sample, phase-aligned - `mpx.cpp` `Combiner`; option `array`, VFO <=
  480 kHz), NOT manual only (nothing without RDS)
- Discriminator (polynomial atan2) -> MPX in Hz -> the RDS band 54-79 kHz
  as ONE complex stream (66.5 kHz centre, a polyphase decimator to >= 48
  kHz: `Band` in decoder.cpp) for RDS and the RDS2 streams 66.5 / 71.25 /
  76 kHz; `rds_demod.hpp` `RdsDemod`: mix (table `Mixer` - the frequencies
  repeat within <= 8192 samples), msresamp to 19 kHz (8 per half-symbol;
  its delay MEASURED at init - `msresamp_crcf_get_delay` was ~1 ms off,
  `measured_delay`), RRC roll-off 1, Gardner (phase-free; the loop
  integrator's sign was wrong first: it ran to its clamp), Costas ~10 Hz,
  biphase pairing, differential decoding with soft values + the bit's MPX
  time. `rds_block.hpp` `BlockSync`: w mod g == offset (A 0FC, B 198, C
  168, C' 350, D 1B4 - checked against the EN 50067 parity matrix on 10000
  blocks), 3-pulse sync, burst <= 3 bits only among the 6 weakest bits,
  else 1-3 of the 5 weakest flipped; lost at 40 / 50, slip recovery;
  `generation()` per acquisition. `rds_station.cpp` `GroupDecoder`: every
  group type (0A/B PS TA MS DI AF, 1A ECC / language / TMC / EWS ids + PIN,
  2A/B RT, 3A ODA directory, 4A CT, 10A PTYN, 14A/B EON, 15A Long PS, 15B,
  ODAs RT+ 4BD7 / eRT 6552 / TMC / DAB 0093), per PI (`Station`, <= 64);
  RULES: a corrected block's data only with a second identical copy
  (`SegText::put`, `confirm`), a group with < 2 good blocks unused (random
  words pass an offset check 1 in 1024 - a lone block B in a gap between
  stations gave a false PTY), a new PI after 3 groups in a row (2 first)
  with one uncorrected, a PI-less group the station's only <= 0.25 s after
  one WITH its PI (a PI-less group extending that let the next station's
  first groups into the old one's data), PS complete = all four segments
  confirmed since one changed (in order was too strict for weak signals),
  dynamic PS = 3+ PSs a minute; `Stream::there()` = synced + 3 groups with
  3+ good blocks this sync generation (a false sync - a phantom RDS2
  stream - gives none). PTY names RDS / RBDS (option `pty`; auto = ECC US /
  CA / MX, else the station location in North America); RBDS call sign from
  the PI; tables `rds_tables.hpp` (EN 50067 / NRSC-4-B as compiled by
  redsea - Oona Räisänen, MIT, its files ISC: both notices there + a row /
  text I in THIRD_PARTY_NOTICES; `rbds_callsign()` follows redsea's
  getCallsignFromPI(), the block-sync conditions and the biphase pairing
  choice are redsea's ideas, credited in place - licence audit 2026-10-23:
  no GPL code, nothing else of redsea's)
- RDS2 streams: decoded (not via the spectrum - too weak) 4 s every 30 s
  and on a station change while RDS decodes, kept while synced; only
  counted (groups, block errors)
- Subcarriers (`mpx.cpp`): `MpxSpectrum` (0.8 s, every other frame), floor
  = 20th percentile +-16 kHz, regions >= 5 dB / >= 3 kHz / peak >= 7 dB in
  20 (stereo: 54) - 99 kHz minus pilot / 38 kHz / synced RDS / RDS2;
  twins (< 2.5 kHz gap, within 6 dB, <= 24 kHz) merged (wide FSK's two
  tones), a stronger neighbour's skirt (12 dB down within 2.5 kHz) dropped;
  matched to the tracked ones by band overlap; `probe_subcarrier` on 0.5 s
  of the MPX ring (new ones at once, then every 30 s): envelope CV -> FM /
  not; FM: clock line of the squared derivative (bit rate), kurtosis, a
  few lines holding > 50 % of the deviation's power = tones (audio - a
  tone's derivative has a line at 2f, which read as FSK), dev / carrier;
  else RDS2-shaped / DARC-like / AM. Gone after 10 s
- Audio (`audio` option): auto = the strongest FM-audio SCA else the main
  programme (mono, 75 us), sca / 67 / 92 / other (`sca_khz`); SCA 150 us
  (`deemph`), muted "(no carrier)" 1.5 s after its subcarrier left the
  spectrum. Voice to Text takes that audio. "🔊 Listening" again = back to
  the VFO's WBFM, bandwidth kept (*VFO audio*). kraken_doa's WBFM is mono
  (no stereo decoder anywhere)
- Talkers: a packet `PI xxxx` per RDS group with >= 3 good blocks of the
  confirmed station (exact 104 bits, ~0.2 ms - the first biphase bit's
  start), the carrier to the kHz, 200 kHz wide, avg 10 s, keep 30 min.
  UI kind `broadcast` 🎙 (`RDF_PK_KIND` rds, heads Station (PI) / Call sign
  · PS · MHz / Groups, by name, unit group, "Every broadcast station")
- Generator `tools/rds_gen.py` (real groups, stereo, SCA FM / FSK, RDS2,
  stations at bearings / times, 480 kHz x an integer, cf32 / cu8, pieces);
  playback `rds-two-stations` (`tools/scene_two.json`: RDS NZ station at 60
  deg, RBDS "KRKN" = PI 3DF5 at 200 deg, taking turns)
- Tests (job scratch 8e472af8, numbers in the README): every field; two
  stations 60.0 / 200.0 deg offline and END TO END (this PC's kraken_doa.exe
  from a scratch cwd with `KRAKEN_PLUGIN_DIR`, playback over the WebSocket:
  DF status exact, "5 combined", Listen -> SCA, no spurious events in 150
  s); 2.4 MHz input; RDS2; sensitivity one antenna full at 18 dB CNR, five
  combined at 10 dB (~7 dB gain); noise nothing; a REAL station (pysdr.org's
  99.5 MHz recording: PI 571D, RT, dynamic PS, a real 67 kHz SCA with
  speech-like audio); CPU 0.9 % (240 kHz) / 1.7 % (480 kHz) / 2.1 % (5
  antennas) of real time on this PC; Linux x86_64 / aarch64 syntax-clean
  (zig). Not done: TMC events, RDS2 groups, DARC / SCA data payloads, HD
  Radio, stereo decoding. Note for tests on Windows: kraken_doa.exe
  chdir's to its own folder when the cwd has no kraken_doa.html - copy the
  page into a scratch cwd or it runs in the checkout

