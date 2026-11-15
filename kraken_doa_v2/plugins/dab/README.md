# DAB / DAB+ digital radio (`dab`)

Eureka-147 DAB ensembles (ETSI EN 300 401, transmission mode I - the only
mode left in V2.x) in a 2.4 MHz VFO (`.lock_vfo`: the VFO is the tuner's whole
band; tune the receiver to the block, e.g. 12C 227.360 MHz). One decoder shows

- the **ensemble**: label (FIG 1/0, the extended FIG 2/0 label when sent), EId,
  country (ECC + the EId's country id), block, SNR, frequency offset, FIC
  quality, date / time with the local time offset, number of services;
- every **service** as a table row (label, SId, DAB / DAB+ / data, bit rate,
  protection, sub-channel, programme type, language) with the rest in the row's
  details (short label, components with start CU / size / protection / packet
  address / user applications, announcement support, clusters, CA);
- the **selected programme** (option `service`): its sub-channel out of the MSC,
  handed to the audio layer (audio, the dynamic label as "Text");
- the **transmitters** of the single frequency network from their TII codes,
  with their level and delay against the strongest - and, with every antenna
  (coherent mode, DoA on), **a bearing per transmitter** (talker
  `EId-MainId-SubId`, e.g. `10BC-12-03`);
- transmitter positions from FIG 0/22 (the TII database of EN 300 401 V1.4.1,
  removed in V2.x but cheap) as map points with the talker's id.

Everything shown passed a check: FIB CRC-16 for every FIG, TII consistency over
several null symbols. Text from the air (labels, dynamic label) is converted to
UTF-8 (EBU Latin, UCS-2, UTF-8) and passed through a UTF-8-aware variant of
`kp::printable` (control characters and invalid bytes escaped, characters kept).

## How it works

```
2.4 MHz (antenna 0) ── dab_ofdm: null symbol search, cyclic prefix CFO, phase reference
                       (whole-kHz CFO, impulse response timing), 2400-point FFTs,
                       D-QPSK soft bits
                         ├─ FIC (symbols 2-4) ── dab_fic: de-puncture, Viterbi, PRBS, CRC,
                         │                       FIGs -> ensemble database
                         └─ MSC (only the selected sub-channel's symbols) ── dab_msc: time
                            de-interleaving, UEP / EEP de-puncturing, Viterbi, PRBS
                            -> logical frames -> dab_audio (audio layer)
every antenna, null symbol ── dab_tii: codes, level, delay, per-transmitter covariance
```

### OFDM front end without a resampler (`dab_ofdm.cpp`)

At 2.4 MHz a mode I useful symbol (Tu = 1 ms) is exactly 2400 samples, so the
1 kHz carrier spacing is one bin of a 2400-point FFT (2400 = 2^5 3 5^2: pffft
does it directly). The guard interval (590.625), the symbol (2990.625) and the
null symbol (3112.5 samples) are not whole samples, but the 96 ms frame is
(230400). Each symbol's FFT window starts at a whole sample 48 samples before
the first path's useful part; the window's sub-sample offset from the ideal
start - different for every symbol - is a linear phase over the carriers,
taken out before the differential demodulation (an `exp(-j 2 pi k delta / 2400)`
recurrence per symbol). The tracked sample clock offset scales the symbol
positions. No resampling of 2.4 Msps x 5 antennas on a Raspberry Pi.

Carrier frequency offset, entirely in the plugin (no `host.freq_error()`: an AFC
correction would take the other antennas away): the fractional part by
derotating each window's samples with the phase running on the sample index
from the frame start (so the symbol-to-symbol phase is right); the whole-kHz
part N as a bin shift plus the common phase `2 pi f (w - frame start) / fs`,
which also removes the `2 pi N Tg / Tu` turn between symbols.

Synchronisation:
1. Null symbol: a 2880-sample power dip below 0.65 of the mean, twice 96 ms apart.
2. Fractional CFO: cyclic prefix correlation of all 76 symbols.
3. Whole-kHz CFO (+-410 kHz): the phase reference symbol's differential
   correlation at every bin shift. It repeats for shifts that keep the h pattern
   of table 24 (multiples of 32 carriers: 0.92 next to 0.995 measured), so the 12
   best shifts are told apart by the coherent impulse response (peak / mean 1488
   against 177).
4. Timing: the impulse response from the phase reference (Hann window over the
   carriers); the frame timing is the first path within 15 dB of the strongest,
   with a parabolic sub-sample estimate.

Tracking per frame: CFO from the frame's cyclic prefixes (loop gain 0.5), timing
from the phase reference (proportional 0.5 + integral on the sample clock),
whole-kHz moves after 3 votes, sync lost after 2 bad phase references (a gap in
the input: search again at once). Soft bits: `Re / Im (y_l conj(y_l-1))`, scaled
per symbol (the carrier's power weights it), frequency de-interleaved.

The null symbol is transformed (on every antenna) at frame start + 680 samples:
as late as the first path's phase reference allows, so the previous frame's last
symbol from transmitters up to ~280 us later than the first path stays out (on
the NPO recording this raised the TII SNR from 12 to 18 dB and found a fourth
transmitter).

### FIC and ensemble database (`dab_fic.cpp`)

4 codewords of 2304 bits per frame: de-puncturing (21 x PI 16, 3 x PI 15, tail),
a dedicated soft-decision Viterbi (64 states, butterflies, int32 metrics,
vectorised; `dab_fec.cpp`), energy dispersal, 3 FIBs each with the CRC-16. The
re-encoded bits of good FIB groups give the channel bit error rate ("FIC
quality"). FIGs: 0/0 (EId, change flags, alarm, CIF count, occurrence change),
0/1 (short and long form), 0/2, 0/3, 0/5, 0/7, 0/8, 0/9 (ECC, LTO, international
table, extended field), 0/10 (short / long UTC), 0/13, 0/14, 0/17 (also the V1.x
language / complementary code bytes), 0/18, 0/19 (V1.x region byte; alarms only
when FIG 0/0 enables them), 0/22, 1/0, 1/1, 1/4, 1/5, 2/0, 2/1, 2/4, 2/5 (Ver 1,
segments, UTF-8 / UTF-16). Every FIG and field length is checked against its
FIB; a field that doesn't fit ends that FIG. Next-configuration MCI (C/N = 1) is
kept apart and taken over at the occurrence change. Services / sub-channels the
MCI stops describing for 30 s are dropped.

### MSC (`dab_msc.cpp`)

Only the selected programme's primary audio component: its CUs of every CIF
(only those OFDM symbols are demodulated), time de-interleaving over 16 CIFs
(table 21; a missing CIF = erasures, more than 4 missing = a lost frame for the
audio layer), de-puncturing from the UEP profile (table 15 via the table 8 index)
or EEP A / B, Viterbi, energy dispersal -> one logical frame (kbps x 3 bytes) per
CIF. It runs always (0.2-0.3 % CPU here): the dynamic label needs it; only the PCM
decoding depends on someone listening.

### TII (`dab_tii.cpp`)

Every null symbol: the power of every (comb c, pair b) slot over its 8 carriers
(4 quarters x 2), summed over the antennas, against the null symbol's median
carrier power. A code = a comb with exactly 4 slots above the threshold (or 4
standing 6 dB above the other 4) that passes a structure check: both carriers of
every pair carry power, 3 of the 4 quarters do, and it is not a copy of a stronger
code with the same pattern on a nearby comb (13 dB stronger on the next comb, 25 dB
within 4 combs - inter-carrier interference from a strong transmitter lands on
exactly those carriers: seen with a 40 ppm sample clock offset at 54 dB SNR). The
TII has no CRC: a code counts after 5 detections in the last 20 null symbols that
carried TII (TII is sent in every other frame), and is dropped 20 s after its last.

Level: the smoothed per-carrier SNR, relative to the strongest.

Delay against the strongest: the pairs' phase step `Y(k+1) conj(Y(k))` gives the
delay modulo 1 ms (coarse); a delay profile over the 32 carriers (non-coherently
averaged, 1-sample grid, parabolic peak) gives it finely but repeats every 50
samples (the 48-carrier spacing). The coarse value picks the period; the delay is
shown only when both agree within 20 samples and the pair steps are coherent
(>= 0.5). EN 300 401 puts phi_k of the pair's first carrier on both carriers; some
transmitters on the air (BBC National DAB, DAB Melbourne 1, DAB+ Sydney 1, RAS
12D) put phi_k+1 on the second - both conventions are tracked per transmitter.

## ID separation

A DAB network is a single frequency network: every transmitter sends the same
signal, so the VFO's ordinary DoA is a mix of all of them. In the null symbol
every transmitter has 32 carriers of its own. In `process_multi()` (coherent
mode, DoA on) each confirmed transmitter detected in a null symbol gets a talker
packet (`kp::Talker`, `packet`, `avg_s` 10, `keep_s` 30 min, id `%04X-%02d-%02d` =
EId-MainId-SubId, label = the ensemble label + the distance when FIG 0/22 and the
station position are known) with its own spatial covariance (`t.m` / `t.cov`):
the sum of `v v^H` over its carriers that no other code detected in the same null
symbol uses, minus the noise covariance of the empty carriers of that symbol;
sent only when the main eigenvalue holds >= 50 % of the raw trace and >= 80 % after
the noise is removed. `start_s` / `end_s` = that null symbol's 1 ms window. Talkers
only while the FIC decodes (EId known, a good FIB in the last second). Through
`process()` (one antenna) the transmitters are listed in the facts, without
packets.

`t.lat` / `t.lon` are not filled: they serve the array calibration from known
directions (`array-calibration.md`), which takes 2-60 deg elevation and >= 5 km;
a ground transmitter is near 0 deg and its height is unknown (FIG 0/22 has none).
Its position goes to the map instead (`kind` station, id = the talker id), so the
DF panel shows the DF error against it.

## Options

| key | default | |
|---|---|---|
| `service` | "" | the programme to decode: "" = the first audio service (lowest SId); a SId in hex (`C221`) or a case-insensitive part of the label |

## Facts, events, raw frames

Facts: Sync, SNR (+ MER of the FIC symbols), Frequency offset (+ sample clock),
Block, FIC quality (share of FIBs with a good CRC over 10 s + channel bit error
rate), Ensemble, Country, Date / time, Services, Programme, Sub-channel (logical
frames, lost, channel bit errors), Codec / Voice codec / Audio frames / RS
corrections / Text (from the audio layer), Transmitters (TII), Antennas;
"CPU" with "Log all messages" (the plugin's time per stage, % of real time).

Events: ensemble found / lost (no FIC for 3 s), the services (one summary, then
each new one), the programme, a service gone, multiplex reconfiguration
announced / done, announcements (FIG 0/19; alarms marked ⚠), each transmitter
confirmed / no longer received, the dynamic label (900 s dedup).

Raw frames (decoder log): one line per FIB that passed its CRC:
`FIB <CIF count>.<0..2> <30 data bytes in hex>`.

`host.valid(start, end)` for every 96 ms transmission frame with at least 2 FIBs
passing their CRC.

## Audio layer

> PLACEHOLDER (audio layer): `dab_audio.cpp` - MPEG Layer II, DAB+ super frames,
> PAD / dynamic label, the AAC library. To be written by its author.

## Test signal generator

> PLACEHOLDER (generator): `tools/dab_gen.cpp`, scenes, `tools/testsignals.json`.
> To be written by its author.

## Standards used

- ETSI EN 300 401 V2.2.1 (2026-06): clause 5.2 (FIB, FIG types 0 / 1 / 2), 6
  (MCI: FIG 0/0, 0/1 + table 8, 0/2, 0/3, 0/7, 0/8, 0/13, 0/14; reconfiguration
  6.5), 8.1 (SI: 0/5, 0/9, 0/10, 0/17, 0/18, 0/19, labels 8.1.13 / 8.1.14), 10
  (energy dispersal), 11 (convolutional code, table 13 puncturing, tables 14-20
  UEP / EEP), 12 (time interleaving, table 21), 13 (CIF), 14 (OFDM: table 22,
  phase reference tables 23 / 24, block partitioning, QPSK mapping, frequency
  interleaving, D-QPSK, TII 14.8 + table 26), annex E (CRC).
- ETSI EN 300 401 V1.4.1 (2006-06): clause 8.1.9 (FIG 0/22, TII database), the
  V1.x layouts of FIG 0/17 and 0/19.
- ETSI TS 101 756 V2.5.1: tables 1, 2a / 2b, 3-7 (countries), 9 / 10
  (languages), 11-13 (programme types), 14 (announcements), 16 (user
  applications), annex C (EBU Latin repertoire).
- ETSI TS 103 461 V1.2.1: annex E (Band III block frequencies).
- ETSI TS 102 563 V2.1.1 clause 6 (RS / virtual interleaving) - only to check
  decoded DAB+ super frames in the tests.

The tables are in `dab_tables_gen.hpp`, machine-extracted from the documents'
text (pdftotext) and checked by `dab::self_test()` at start (1536 distinct
carriers from the frequency interleaver against table 25's entries, puncturing
vector weights 8 + PI and nesting, 70 distinct TII patterns of 4 bits and figure
64, every UEP profile and EEP size filling its sub-channel exactly, table 23
covering every carrier, EBU Latin spot checks). No GPL DAB implementation was
consulted.

## Limits

- Mode I only (the only one in V2.x). L-band works, but blocks are named only in
  Band III (TS 103 461 table E.1); elsewhere the frequency is shown.
- Antenna 0 is demodulated; the antennas are not combined for a better decode.
- No resampler: the sample clock offset's inter-carrier interference is not
  corrected - at 40 ppm the FIC symbols' MER is 29.7 dB at 54 dB SNR (60 dB at 0
  ppm); RTL-SDRs are within a few ppm.
- Only the selected programme is decoded; packet mode / data services, DMB and
  conditional access are named, not decoded.
- A reconfiguration resets the selected sub-channel's de-interleaver (15 CIFs of
  audio). Reconfiguration and announcements are not verified (no test signal has
  them).
- The country is looked up from the EId's country id + ECC as transmitted: DAB
  Melbourne 1 (EId F003, ECC F0) reads "Malaysia" by table 7.
- TII delay: shown only when the coarse and fine estimates agree; a strong
  multipath channel moves the coarse one (power-weighted mean delay).
- FIG 0/22 is deprecated; ensembles rarely send it now.

## Tests

All offline (`decoder --file ...`), on this machine (i7-11800H). "CPU" = the
plugin's own time (the "CPU" fact); the summary's "decode time" also holds the
offline host's file reading and, for 2.048 MHz files, its resampling to 2.4 MHz.

**Generator** (`tools/dab_gen.cpp`, written independently from the standards):
- Default scene, 1 antenna, 20 dB: 49 / 50 frames valid (the first is the
  acquisition), 100 % FIB CRC, SNR 20.0 dB, MER 20.0 dB, 2 services, TII 10BC-12-03.
- `scene_sfn3.json` (3 transmitters, 5 antennas, UCA 0.12 m): truth 12/3 @ 40 deg
  0 us 0 dB, 12/7 @ 150 deg 37.5 us -4 dB, 31/15 @ 260 deg 112.25 us -8 dB ->
  40.1 / 150.3 / 259.8 deg, -4.0 / -8.2 dB, +37.5 / +112.2 us, 21 packets each
  ("by code"). Own 3-transmitter scene (12/3 @ 40, 12/5 @ 150 +23.4 us -4 dB,
  7/11 @ 260 +61 us -9 dB): 39.9 / 149.9 / 260.2 deg, +23.4 / +61.0 us.
- `scene_single.json` (block +24 kHz, CFO +370 Hz, SCO +12 ppm, CIF count and
  midnight wrap, LTO -5 h): CFO +24370 Hz, clock +11.9 ppm, 12C named, 6 services,
  5/1 @ 300 -> 299.9 deg.
- Logical frames vs the generator's dump: Layer II 128 kbit/s UEP 3 and 64 kbit/s
  UEP 2 (LSF) bit-identical (181 / 181 frames at 12 dB; 380 / 381 at 8 dB, one
  residual error); DAB+ super frames with every RS(120,110) code word clean: EEP
  1-A, 2-A, 3-A, 3-B, 1-B 36 / 36, 4-A 35 / 36 at 12 dB.
- Offsets: block at +-380 kHz, CFO +3456 / -480 / +470 Hz, SCO -35..+40 ppm: all
  100 % FIB CRC.
- SNR sweep (1 antenna): 10 dB 100 % FIBs (0.7 % channel bit errors), 5 dB 100 %
  (9.3 %), 4 dB 98 %, 3 dB 75 %, 2 dB 17 %, 1 dB none. TII confirmed down to 5 dB.
- MER floor: 60.0 dB at 60 dB SNR (2.4 MHz file), 55.7 dB from a 2.048 MHz file
  through the host's resampler; at 10 dB SNR 10.0 / 10.1 dB - the no-resampler
  front end adds no measurable loss.

**Real recordings** (published off-air captures, 2.048 / 2.5 Msps, through the
offline host's resampler):

| recording | ensemble | FIB CRC | services | TII | sub-channel check |
|---|---|---|---|---|---|
| 11D, DE | Bayern 10A5 | 100 % | 14 | 04-01, 04-20 (-9 dB, +79.7 us) | DAB+ 96k: 213 / 213 super frames RS-clean |
| 9A, AU | DAB+ Sydney 1 1004 | 100 % | 33 | 22-02 | DAB+ 64k: 605 / 605 |
| 12D, IT | RASDAB12D 512D | 100 % | 8 | 04-04 | Layer II 160k: 697 / 697 frames start with a Layer II header |
| 10B, BE | DAB+ West-Vlaanderen 0064 | 98 % (SNR 6 dB) | 15 | 05-12 | DAB+ 72k: 15 % channel bit errors, 0 RS-clean |
| 12C, NL | NPO 8001 | 100 % | 10 | 03-13, 03-06 (+52.1 us), 02-10 (+91.5 us), 02-02 (-35.6 us) | DAB+ 128k: 409 / 409 |
| 9A, AU (2.5 Msps) | DAB Melbourne 1 F003 | 100 % | 29 | 31-02, 31-12 (-71.4 us) | DAB+ 64k: 55 / 55 |
| 12B, UK | BBC National DAB CE15 | 100 % | 12 | 11-01, 08-13 (+31.1 us), 01-07 (+62.4 us), 11-20 (+7.4 us) | Layer II 128k: 1634 / 1635 (the gap) |
| 11C, UK | Devon C19F | 100 % | 8 | 14-09 | Layer II 128k: 2613 / 2613 |

Labels, PTy, languages, LTO and UTC were plausible throughout (e.g. Bayern
2024-10-11 16:49 UTC, local 18:49). The UK 12B recording has a gap at 1.3 s: lost
and re-acquired.

**With the audio layer** (receiver side only): the generator's Layer II service
plays its 1125 Hz tone (measured 1125.0 Hz in the `--audio` WAV), 121 / 121 audio
frames ok, its dynamic label shown; its DAB+ service reaches super frame sync, 144
AUs ok ("not installed" without the AAC library). UK 12B, BBC Radio 1 (Layer II
joint stereo): 38.5 s of audio, 1557 frames ok, 9 bad (the recording's gap), the
dynamic label shown.

**Wrong signals** (10 s each): white noise, a tone, LTE-like OFDM (15 kHz), a
2048-carrier OFDM, a 4 kHz-spaced OFDM with periodic nulls, and a DAB-sized OFDM
(2400-point FFT, 1536 carriers, same guard and null, random phase reference -
it passes the null and cyclic prefix steps): no valid frame, no event, no talker.

**Robustness**: a sanitizer build (address + undefined behaviour) on the
generator scenes, a recording, noise and the DAB-like OFDM: clean. 2 million
random / malformed FIBs through the FIG parser + the label converters on random
bytes: clean. Switching `process()` <-> `process_multi()` every 0.37 s, 10 ms gaps
every second, 50 ms gaps, a reset: clean, re-acquired each time.

**CPU** (the plugin's own time, i7-11800H): 1.0-1.1 % of real time with one
antenna (front end 0.6-0.7, FIC 0.1, MSC 0.2-0.3, TII 0.0-0.1), 1.5-1.7 % with
five; 1.3 % on the UK recording with Layer II audio decoded. Not measured on a
Pi 5 (expect ~3x).
