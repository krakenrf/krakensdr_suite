# rds - RDS / RBDS + SCA (FM broadcast)

What a wideband FM broadcast carries besides its programme: the **RDS**
data on 57 kHz (RBDS in North America - the same signal), the **RDS2**
data streams on 66.5 / 71.25 / 76 kHz, the 19 kHz stereo pilot and the
**SCA** subcarriers ("subsidiary communications": FM audio channels for
reading services and background music, FSK data, DARC). It decodes the
RDS groups, finds and classifies every subcarrier, plays an SCA's audio
(or the main programme in mono) on Listen, and gives **every station its
own DoA** by its programme identification code (PI).

Pick "RDS / RBDS + SCA (FM broadcast)" as the digital decoder of a VFO on
an FM station (Demod WBFM). VFO bandwidth: **240 kHz** for RDS, **300-400
kHz** for SCA subcarriers above ~80 kHz (a 92 kHz SCA needs the FM
signal's outer sidebands - the 240 kHz VFO keeps +-96 kHz); 150 kHz still
decodes RDS. Wider VFOs (up to 2.4 MHz) are filtered to the FM channel by
the plugin. It takes the VFO's own rate (`Info::sample_rate` 0). Auto
detect can pick it (it reports nothing without RDS).

## What it reads

EN 50067:1998 / IEC 62106 (2015 parts 1-2, the RDS2 additions of 2018),
NRSC-4-B (RBDS), the RDS Forum's ODA list and RT+ specification
(R06/040_1). Per station (PI):

- **PI** (hex), RDS coverage area / programme reference; **RBDS call sign**
  from the PI (K / W + 3 letters, the 3-letter calls, nationally linked
  networks - NRSC-4-B Annex D); **country** from the PI + the extended
  country code (1A variant 0); language (1A variant 3); TMC / EWS ids;
  programme item number (PIN)
- **PS** (0A / 0B) - complete once all four segments are confirmed since
  one last changed; a PS that changes 3+ times a minute is flagged
  **dynamic** (scrolling) and no longer logged per change
- **PTY** with the RDS or the RBDS name (they differ: PTY 9 is "Varied" in
  RDS, "Top 40" in RBDS) - option "Programme types": auto takes RBDS when
  the ECC says US / Canada / Mexico, else (no ECC yet) when the receiver's
  location is in North America; **PTYN** (10A); PTY 31 (Alarm /
  Emergency) is a "⚠" event
- **TP / TA** (a traffic announcement's start / end are events), **M/S**,
  **DI** (stereo, compressed, artificial head, dynamic PTY)
- **RadioText** (2A 64 / 2B 32 characters, A/B flag, end-of-text), **RT+**
  tags (ODA 4BD7: title, artist, ... "Now playing"), **Enhanced
  RadioText** (ODA 6552, UTF-8 / UCS-2), **Long PS** (15A, UTF-8)
- **Clock time** (4A) in UTC and local time (the station's offset)
- **Alternative frequencies** (0A: every AF code heard, LF / MF ones in
  kHz; the tuned frequency marked *; method B's regional variants are not
  told apart)
- **EON** (14A / 14B): other networks of the broadcaster - their PI, PS,
  AFs, PTY, TP / TA
- the ODA directory (3A) by name; RDS-TMC (counted, its location table
  number - events need the country's location tables), DAB cross-reference
  (ensemble + frequency + channel), EWS (9A: a "⚠" event), paging,
  transparent data, in-house groups (counted)
- the group mix, block errors, the RDS level (injection) and SNR

The RDS character set (EN 50067 Annex E) is converted to UTF-8. Raw frames
(decoder data log, "Raw frames"): one line per group in RDS Spy form,
`9201 0408 E2C8 5A4D` (`----` = a block that failed).

**Subcarriers** (facts, events): the averaged MPX spectrum (0.8 s) above
the programme (20-99 kHz; 54-99 kHz when the pilot says stereo) - every
band >= 5 dB over the floor around it, >= 3 kHz wide; a wide FSK's two
tones are joined, a strong subcarrier's outer sidebands left out. Each is
**probed** (0.5 s of the MPX, again every 30 s): constant envelope = FM
(its deviation; the carrier = the mean frequency, for FSK the middle of
its two tones), then FSK data (a clock line in the
squared derivative of the deviation = the bit rate, a two-level
deviation), a tone (a few spectral lines hold most of the deviation's
power - audio, not data), FM audio, or an idle carrier; envelope varying =
RDS2-shaped (66.5 / 71.25 / 76 kHz, narrow), DARC-like (76 kHz, wide) or AM
/ DSB. Injection = the subcarrier's deviation of the main carrier (% of 75
kHz). The pilot's level, and the RDS injection from the demodulator.

**RDS2 data streams** 1-3 are too weak for the spectrum search (2 kHz
deviation sits a few dB over the MPX noise there): while the station's RDS
decodes, each frequency is tried for 4 s every 30 s (and when the station
changes); a stream that synchronises is listed with its group count and
block errors. Their groups are not decoded (RDS2 group type C: logos,
files).

**Audio** (Listen = Demod Digital, 8 kHz): option "Audio" - auto (the
strongest SCA carrying FM audio, else the main programme), main programme
(mono L+R, 75 us de-emphasis), strongest SCA only, 67 / 92 kHz, or any
frequency ("SCA kHz"). SCA audio: FM demodulated, de-emphasis 150 us (the
SCA standard; option), 3.6 kHz. Muted ("no carrier") once its subcarrier
is gone. Voice to Text transcribes that audio (an SCA reading service, or
talk radio).

## Signal processing

- Front end: VFO > 480 kHz decimated (a wide-transition anti-alias stage),
  300-480 kHz channel-filtered (+-120 kHz pass, 175 kHz stop: adjacent
  stations), the FM discriminator (a polynomial atan2, ~1e-5 rad) -> MPX in
  Hz of deviation. Every antenna (coherent mode, DoA on, `multichannel`,
  VFO <= 480 kHz): combined blindly first - the main eigenvector of their
  covariance (0.25 s), interpolated sample by sample, phase-aligned to the
  previous update - before the discriminator, so the FM threshold sees the
  array's carrier-to-noise (option "Array combining")
- The RDS band 54-79 kHz as one complex stream (66.5 kHz centre, one
  polyphase decimator to >= 48 kHz) for RDS and the three RDS2 streams;
  each: mixed to 0 Hz, resampled to 19 kHz (8 samples per half-symbol),
  RRC roll-off 1 matched filter (H(f) = cos(pi f td / 4)), Gardner timing
  (phase-free), Costas carrier loop (~10 Hz), biphase pairing (the half
  pair with the larger difference), differential decoding with soft
  values (the weaker of the two symbols), the bit's MPX sample time
- Blocks (`rds_block.hpp`): w(x) mod g(x) == the offset word; sync = three
  offset words in order at 26-bit spacing within 6 blocks; per block the
  offset check, else a burst of up to 3 bits (1, 11, 101, 111 patterns: a
  symbol error is two adjacent bit errors) only where the soft bits were
  among the 6 weakest, else 1-3 of the 5 weakest bits flipped. Lost after
  40 of 50 blocks failed; a sequence at another bit position while every
  block fails = a slipped bit, moved to
- Data from a corrected block is used only when a second identical copy
  confirms it (each character, each value); a group with fewer than 2 good
  blocks is not used at all (random words pass one offset check in 1024);
  a stream counts as there after 3 groups with 3+ good blocks since its
  sync; a new PI takes over after 3 groups in a row (2 for the first), one
  of them uncorrected; a group without a PI is the station's only right
  after one with its PI

## ID separation (a DoA per station)

The PI names the programme / station. Every group of the station on air
with its PI confirmed (>= 3 good blocks) is a `kp::Talker` packet `PI xxxx`
of exactly its 104 bits (87.6 ms; times to ~0.2 ms - the start of a
biphase bit), on the FM channel (the carrier, 200 kHz wide), averaged over
10 s, kept 30 min; label call sign / PS / MHz; the decoder table's row key
is the talker id (a row click selects it on the map). So co-channel
stations that take turns - tropospheric DX fading in and out, driving from
one transmitter's area into another's on the same frequency - each get
their own bearing. FM's capture effect means one station at a time on a
frequency; the samples of each group are that station's. Not separated:
the transmitters of one network, which share the PI (a synchronised
network on one frequency is one talker). UI kind `broadcast` 🎙 (DF list
"Station (PI) / Call sign · PS · MHz / Groups", by name, "Every broadcast
station").

## Options

| key | | |
|---|---|---|
| `audio` | auto | auto / main / sca / 67 / 92 / other: what Listen plays |
| `sca_khz` | | the SCA frequency (kHz, 20-99) for `other` |
| `deemph` | 150 | SCA de-emphasis: 150 / 75 / 50 us, 0 = off |
| `pty` | auto | auto / rds / rbds: which PTY name list |
| `array` | 1 | combine every antenna before the discriminator |

## Tests

Generator `tools/rds_gen.py` (scene JSON: stations at bearings / times /
CNR on kraken_doa's UCA, every antenna, 480 kHz x an integer, cf32 / cu8,
0.1 s pieces): real RDS groups (check words + offset words, differential
+ biphase coding, the shaping filter, 57 kHz = 3 x the pilot), 0A / 1A /
2A / 3A / 4A / 10A / 11A RT+ / 12A eRT / 14A / 15A, RBDS PIs from a call
sign, stereo programme + pilot, SCA FM audio (tones, 150 us pre-emphasis)
and FSK data (Gaussian, any bit rate), RDS2 streams. Playback entry
`rds-two-stations` (`tools/scene_two.json`).

- **Check words**: the generator's (polynomial division) against the
  EN 50067 parity-check matrix (as in redsea's block_sync.cc): the
  standard's syndromes for A / B / C / C' / D on 10000 blocks, 0
  mismatches; the plugin uses the same polynomial
- **Every field** (one station, 35 dB, 20 s): PI, PS, PTY + PTYN, RT (two
  messages, A/B), RT+ title / artist, ECC -> New Zealand, language, PIN,
  CT 16:04 UTC+13 = 03:04 UTC, AF list (tuned marked), EON (PS, AF, PTY),
  ODA directory, TA on / off, Long PS with UTF-8 (en dash, check mark),
  eRT; 220 groups, 0 % block errors; the 67 kHz SCA as FM audio (its 440 /
  1000 Hz tones decoded on the 8 kHz audio, alternating per second as
  sent), the 92 kHz FSK at 1200 bit/s; 240 kHz VFO (decimated like
  kraken_doa's, +-96 kHz): the same, the FSK at a lower injection
- **Two stations taking turns** at 60 / 200 deg (RDS NZ, RBDS "KRKN" =
  PI 3DF5 with US ECC -> PTY "Talk"), 5 antennas: talkers `PI 9201`
  60.0 deg, `PI 3DF5` 200.0 deg (offline `--uca 0.12`); no group of one
  station in the other's data (a PI-less group in the gap between them
  used to be taken - fixed)
- **2.4 MHz input**, station 300 kHz off the centre (the decimating path):
  everything decoded, 300.0 deg, two RDS2 streams (66.5 / 76 kHz) 0 %
  block errors
- **Sensitivity** (CNR in 200 kHz, 10 s, groups of ~112): one antenna 14
  dB 18, 16 dB 95 (PS complete), 18 dB 112; **5 antennas combined** 8 dB
  60, 10 dB 111, 12+ dB 112 - about 7 dB of array gain. The channel filter
  width (70-120 kHz) made no difference
- **Noise only** 30 s, one / five antennas: no group, no subcarrier
- **Timing**: packets start 0.19 ms before the generator's group boundary
  (= the start of the first biphase bit); the resampler's delay is
  measured at start (liquid's `msresamp_crcf_get_delay` was ~1 ms off)
- **Real station** (pysdr.org's RDS chapter recording, 99.5 MHz in the
  US, 250 kHz, 4 s): PI 571D (area regional 4, programme 29 - as pysdr
  shows), RadioText "WayFM Uplifting. Upbeat. Real.", the dynamic PS
  ("WayFM" / "Upliftin"), PTY 9 (RDS "Varied", RBDS "Top 40" - no ECC in 4
  s: RBDS with a North American station location), 0 % block errors, the
  pilot at 8.8 %, and a real **67 kHz SCA** (FM, +-3.7-4.2 kHz, 3.3 %)
  whose audio is speech-like (energy 300-2000 Hz, 86 % of the envelope's
  modulation at the 2-8 Hz syllable rate)
- **End to end** (the debug playback `rds-two-stations` in kraken_doa,
  400 kHz VFO, 5 antennas, 150 s): DF status `PI 9201` 60.0 deg, `PI
  3DF5` 200.0 deg; "5 combined"; Listen -> "SCA 67.0 kHz", muted "(no
  carrier)" in the other station's slot, then the main programme; every
  expected event. Before the "2 good blocks" and "3 good groups" rules a
  spurious PTY and a phantom RDS2 stream appeared in the gaps between the
  stations; after them none in 150 s
- **CPU** (offline, this PC - the KrakenSDR's Ryzen 9 9950X3D, one core):
  240 kHz VFO 0.9 % of real time, 480 kHz 1.7 %, 480 kHz with 5 antennas
  combined 2.1 %; a 2.4 MHz VFO ~7 % (most of it the offline host's own
  mixing). Not measured on a Pi
- **Builds**: Windows (MinGW, -Wall -Wextra) clean; Linux x86_64 /
  aarch64 (zig's clang, -fsyntax-only -Wall -Wextra) no diagnostics in the
  plugin's files

Offline: `decoder --file x.cf32 [--rf HZ] [--audio out.wav] [--raw]
[--opt audio=sca]`; every antenna: `--channels 5 --uca 0.12`. Debug:
`RDS_DEBUG=1` logs the demodulator's amplitude / SNR / carrier / block
counts every second, `RDS_DUMP=file` writes every half-symbol strobe.

## Not done

RDS-TMC event / location decoding (the location tables are per country),
RDS2 group decoding (logos, files), DARC (16 kbit/s L-MSK) and the data
inside FSK SCAs (proprietary paging / DGPS / background-music control),
HD Radio / IBOC detection, stereo (L-R) decoding.

The tables (RDS character set, PTY names, countries, languages, ODA names,
RT+ types, RBDS call signs) follow EN 50067 / NRSC-4-B as compiled by
redsea (Oona Raisanen, ISC licence - the notice is in `rds_tables.hpp`);
the code is this plugin's own.
