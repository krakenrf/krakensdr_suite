# Analog FM IDs plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Analog FM IDs (`plugins/analog_id/`, README.md there; was `ctcss` "CTCSS / DCS" until 2026-10-22):**
- The user's request (2026-10-22): radio IDs on analog FM - MDC1200,
  FleetSync, 5-tone selcall (ZVEI / CCIR / EEA), DTMF, 1750 Hz - as part of
  the CTCSS plugin, renamed for "IDs on analog signals". id `analog_id`,
  name "Analog FM IDs (CTCSS / DCS, MDC1200, FleetSync, 5-tone, DTMF, 1750
  Hz)", v2.0. The old id is mapped (`dig::renamed_plugin`, now exported
  from dig_report.cpp: `ctcss` -> `analog_id`, like `sfn` -> `dtv`): saved
  `PLUGIN:ctcss` VFOs, `ctcss.*` options, AUTO_DETECT_OFF; `PluginRegistry::
  scan()` skips a renamed id's folder without sources (an old checkout's
  gitignored `plugins/ctcss/build/` would run next to analog_id). UI:
  `rdfKind` maps pl `analog_id` (and `ctcss`) to kind `tone`, `rdfIcon` 🆔
  MDC / FleetSync, 🔢 selcall / DTMF, 🔔 tone / code / burst; "Every analog
  transmitter"
- In-band signalling (`sig.hpp/.cpp`, `asig::Sig`) on the 12 kHz audio
  `rx.cpp` now also puts out (5 kHz low-pass, `audio_time(k)` = its RF
  time): FFSK by sliding one-bit complex correlators (1200 / 1800 Hz at 1200
  bit/s, 1200 / 2400 at 2400, on a 300 Hz high-pass), sync search at every
  sample with a BALANCED decision threshold (g = sqrt(r_b / r_a) from the
  crosstalk on the sync's own bits - scaling each tone by its level read a
  live MDC sync 20 / 39 wrong); MDC1200 (`mdc.hpp`: XOR-precoded MSK read
  COHERENTLY - the phase at bit i + 1's start = data bit i -, 7 x 16
  interleave, convolutional code's single errors by syndrome majority,
  CRC-16, double packets 0x35 / 0x55, an op table: PTT ID pre / post,
  emergency, status, message, call alert, radio check, inhibit ...); FleetSync
  (`fleetsync.hpp`, namespace `fleetsync` - `fsync` collided with POSIX
  fsync(): 64-bit blocks = MPT 1327 codewords - `block_ok(uint64_t)` is the
  BCH(63,48) 0xE815 + inverted last check bit + even parity check of the
  mpt1327 plugin's `cw_ok` (LICENCE: since 2026-10-23 - the first check
  restated the GPL fsync decoder's `_fsync_crc` too closely; never write it
  from Kaufman's code again: same results on 2.5 M words), fleet = byte +
  99, unit 12 bits + 999, FleetSync II = 4 BCH(15,7) nibbles per 16 bits
  (fields on air in swapped pairs, collected as 4 quarters `fs2_q`); read COHERENTLY first - a
  2-state Viterbi over the phase parity the bits before leave (1200 Hz at
  2400 bit/s / 1800 Hz at 1200 bit/s turn pi per bit), references from the
  sync's 32 bits - then by tone; 2-4 dB more sensitive, and the real 2400
  bit/s recording decodes at every sampling phase instead of 1 in 5);
  tones (512-point FFT of 20 ms every 5 ms): selcall (`selcall.hpp`: the
  Midian tables of ZVEI1-3 / PZVEI / DZVEI / PDZVEI / CCIR1-2 / PCCIR / EEA /
  EIA / NATEL / MODAT with priors - ZVEI and DZVEI share tones, auto = the
  common reading + the other as `alt`, option `selcall` picks one; runs of 4+
  steady tones, repeats as E, consecutive identical symbols refused), DTMF
  (talk-off protection: frequencies within 1.8 %, still within 0.6 %, >= 4
  frames, a lone digit >= 8, the pair >= 75 % of the band, voice harmonic
  families refused), steady bursts (1600-2550 Hz on the 50 Hz grid, >= 0.25
  s - 1750 Hz; other steady tones >= 0.5 s as "tone" events)
- `decoder.cpp` (`AnalogIdDecoder`): ONE transmission at a time, opened by a
  tone / code (backdated) or an ID (from the carrier's rise in its 10 ms
  history), ended by the tone tracker / DCS / carrier (< 0.4 of its level 30
  ms); named in order sender ID (MDC / FleetSync, at once) > selcall / DTMF >
  tone / code > burst, waiting HOLD_SHORT 1.6 s - HOLD_LONG 4.2 s once IDs
  were heard on the channel (post-IDs come at the release; TalkerDoa keeps
  frames 6 s) - an ID up to LATE_ID 0.8 s after the end still belongs to it;
  a sender ID decoded later renames the spans not reported yet; another
  sender's ID inside one carrier (repeater output) starts a new
  transmission. Console commands name the radio they ADDRESS: no talker.
  Options `squelch` "ID / tone squelch" (tones / codes + `MDC 1029`,
  `FleetSync 100-1001`, `CCIR 12345`, `DTMF 123`, `Burst 1750`, kinds),
  `ids` (all / tone = the old naming), `selcall`, `min_dev`, `nonstd`.
  Table "ID / tone, Type, Details, With, Deviation, Tx, Last, Air time,
  Ending" + details, facts, events, raw. Offline env `CTCSS_DUMP`,
  `ANALOG_ID_DUMP`, `ANALOG_ID_AUDIO=<file>` (the 12 kHz audio)
- Generator `tools/analog_gen.py` (was `ctcss_gen.py`, old options kept) +
  `--mdc`, `--fleetsync` (`--fs-baud`, `--fs2`), `--selcall`, `--dtmf`,
  `--burst`, `--sig-at pre|post|both`, `--sig-preemph`, `--sig-baud-err`,
  `--audio-u8` (8 kHz for reference decoders); playback entries
  `ctcss-four-tones` and `analog-ids-six` (MDC 1234 / MDC 2002 on one CTCSS
  tone, FleetSync, CCIR, DTMF, 1750 Hz at six bearings)
- Tests (job scratch 9ee43aa0, numbers in the README): the generator against
  Kaufman's GPL MDC / FleetSync decoders (scratch oracle only, never in the
  repo); 40 / 40 synthetic cases (ids + spans); CTCSS / DCS A/B against the
  old plugin 46 / 46 identical; false alarms 2 in 50 min of voice, 0 on
  noise; scene analog-ids-six every talker exact (offline, 5 antennas);
  real recordings: Wikipedia MDC1200 1029 x 3 (= the reference), sigidwiki
  FleetSync 1200 + 2400 bit/s BOT / EOT 167-1002, ZVEI 12345 off air, CCIR
  04221, DTMF sequences, Whelen 461####333
- (Below: the CTCSS / DCS part, unchanged from the `ctcss` plugin.) The
  sub-audible signalling under analog FM (ETSI TS 103 236: tones,
  DCS codeword, polarity, the 83 codes' bit patterns): `sample_rate` 24
  kHz (= the 24 kHz VFO, no resampling), `min_vfo_rate` 12 kHz, talkers,
  NOT manual only (in Auto detect an analog channel with a tone or an ID
  shows as this plugin; without one it reports nothing), no voice
- `rx.cpp`: channel filter +-7.5 kHz, discriminator, 24 kHz -> 2400 -> 1200
  Hz; a 45 Hz high-passed stream (CTCSS), a 130-290 Hz low-passed one (DCS)
  and the carrier power aligned with them. CTCSS: 2048-point FFT of the last
  0.3 s every 50 ms (only what follows a carrier's onset in the window),
  candidates = strongest 60-257 Hz peak >= 13 dB over the band median, >= 55
  % of the band's power, "Weakest tone" .. 1.5 kHz deviation, not a line of
  a periodic family (f +- f/k within 15 dB, k 2..10: a TDMA data carrier's
  33.33 Hz lines, seen live); lock = 3 candidates (2
  strong) agreeing in frequency (0.12-0.8 Hz by SNR) and amplitude (30 %),
  phase linear over 0.2 s; off-list +1 candidate and >= 70 % purity, only
  62-252 Hz; 67.2 Hz (DCS 1010 runs) +5; 133-135.8 Hz never; a tone NOT at a
  carrier onset (voice harmonic? capture / late entry) only after 0.8 s.
  Classification: a listed tone within 0.85 % + 2 sigma of the lock's
  measurement (<= 1.4 %), else non-standard by its measured value (an id
  within 0.5 % reused, `ns_seen_`). Tracker: mixed to the tone, 97-tap 35
  Hz low-pass every 10 ms - frequency ("Measured" = the whole phase advance
  over the ticks with the tone there / their time, a reverse burst's step
  rolled back, vs the median of the ticks' frequencies), end = amplitude through half / the carrier
  gone / another tone dominating 3 hops / a phase step > 100 deg confirmed
  by the end within 0.5 s (reverse burst, at the step); an end after a
  stronger carrier's rise is put at the rise (capture). Start = the
  carrier's onset or the amplitude's half crossing back in the stream.
  DCS: zero-crossing clock + rate loop (leaks to 134.4 while searching),
  decision-directed levels, hard bits; lock = 46 bits 23-periodic + Golay
  codeword, or 69 bits' 3-word majority a codeword with <= 5 errors; no DCS
  search while a CTCSS tone is locked; end = > 6 of 23 wrong, at the last
  clean bit; turn-off = a 134.4 Hz tone within 0.8 s after (NOTE event)
- `tones.hpp`: the 51 tones, Golay g = 0xC75 (checked on all 83 ETSI
  patterns), `dcs_name` (ETSI code N > I > the 104-list N > I > any; the
  other polarity's equivalent)
- `decoder.cpp`: talker / valid spans CONFIRM_LAG 0.3 s behind the
  receiver (an end is known only after it: capture, tone loss), the end
  exact minus END_GUARD 20 ms; valid() every hop for the squelch;
  `.squelch_hold_s` 0.35. Option `squelch` (tone squelch: tokens 88.5 /
  023N / D023N / 047I, comma list) - other tones: no talker, no valid
  frames, "(tone squelch: ignored)" in the log. Table (row key = talker id)
  + details, facts, events (new tone once, every transmission's end)
- Host changes with it: `kp::Info::squelch_hold_s` (`--info`,
  `PluginInfo`, `Engine::hold_ms_by_`, `DigitalDecoder::squelch_open`),
  `DigitalDecoder::takes_voice()` / `lead_talker()` (Voice to Text's analog
  fallback + naming, see *Voice to Text*), TalkerInfo `dur_s` / `air_s`
  (status `dur` / `air`), UI kind `tone` (see *DoA per talker*)
- Generator `tools/analog_gen.py` (was `ctcss_gen.py`; one transmitter: keyed spans, real or
  synthetic speech through a TX audio chain, CTCSS + reverse burst / DCS +
  turn-off, tolerances, narrow, de-emphasis, offset, noise); scenes with
  `plugins/sdk/tools/uca_scene.py`
- Tests (scratch t5/, numbers in the README): all 51 tones and all 104 DCS
  codes N / I named right; lock median 190 ms (CTCSS) / 360 ms (DCS),
  start / end within a few ms; false alarms 2 in 40 min of voice-only /
  noise (synthetic voice, harsh cases; real speech none); 10 / 10
  transmissions from 2-4 dB CNR; scene of 4 transmitters (2 overlapping)
  offline + end to end: 60.0 / 200.0 / 300.0 deg exact, the capturing one
  146 vs 140 (its first 2 s overlap the other carrier); tone squelch e2e:
  only 123.0 talked, squelch open 26 % (its duty 26 %), VFO DoA 200.0 only;
  Voice to Text e2e: analog transcripts named after the tones, split at
  tone changes, the digital path (voicetest plugin) unchanged. CPU 0.6-0.7
  % of a core. Live (Auckland, ~1.5 h on 2 m / 150-174 / 70 cm / PMR446 /
  PRS 476 MHz): no CTCSS / DCS user caught; two false alarms seen live and
  fixed (256 Hz at 5.7 kHz deviation; a data carrier's 200 Hz line among
  33.33 Hz multiples read as 199.5 - the deviation limit and the periodic-
  family rule)

