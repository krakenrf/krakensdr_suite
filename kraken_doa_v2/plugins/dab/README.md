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
  handed to the audio layer - DAB (MPEG Layer II) audio built in, DAB+ (HE-AAC
  v2) audio through the user's libfdk-aac (see *The AAC library*), the dynamic
  label as "Text";
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
kept apart and taken over once, at the first CIF of the new configuration: the
earliest CIF after the announcing one whose CIF count's lower part equals the
occurrence change (clause 6.5 - a reconfiguration is announced up to 6 s = 250
CIFs ahead). The selected sub-channel follows it without a break: the new CU
allocation from that CIF r0, the new protection profile from logical frame r0
(r0 - 15 when the sub-channel shrinks, annex D); the de-interleaver keeps its
memory across the change. Services / sub-channels (current and next) the MCI
stops describing for 30 s are dropped. The database is bounded, whatever the
FIC carries: 64 services per configuration (the least recently signalled one
goes), 256 FIG 2 segment sets (incomplete ones dropped after 30 s).

A FIG 0/0 with another EId in 3 frames in a row means another ensemble on the
block (driving from one multiplex's area into the next): the old one's table
rows, map points and talkers are removed, one "Ensemble lost" event, then a cold
start of the ensemble with the sync kept. A single one (a corrupted FIB that
passed its CRC) is ignored.

### MSC (`dab_msc.cpp`)

Only the selected programme's primary audio component: its CUs of every CIF
(only those OFDM symbols are demodulated), time de-interleaving over 16 CIFs
(table 21; a missing CIF = erasures; a logical frame is decoded while code rate /
share of CIFs present <= 0.8, with at least 8 and at most 12 of its 16 CIFs
present - EEP 2-A 8, EEP 3-A / UEP 3 10 - else it is a lost frame for the audio
layer), de-puncturing from the UEP profile (table 15 via the table 8 index)
or EEP A / B, Viterbi, energy dispersal -> one logical frame (kbps x 3 bytes) per
CIF. It runs always (0.2-0.3 % CPU here): the dynamic label needs it; only the PCM
decoding depends on someone listening.

CIFs are numbered by the sample count while the frames follow each other (the
phase reference of this frame and of the previous one where the tracking expected
it, no new search); the FIG 0/0 CIF count gives the absolute number through an
offset. When the transmitted count steps with the frames contiguous (a looped
recording or debug playback, a multiplexer restart), the offset follows once the
new count holds for 3 frames in a row - nothing is lost or reset, a single
corrupted count is ignored; meanwhile those frames give no valid span or talker
packet. Across a gap in the input (the frame timing not contiguous) the count
says how many CIFs passed: CIFs skipped in an outage are erasures, so the audio
layer keeps its timeline (more than 250: the sub-channel starts over). A frame's
CIFs go to the de-interleaver one frame late (96 ms of audio latency): when the
next frame shows a jump in the input (the frame start >= 400 samples off -
samples the host dropped), they are erasures instead of garbage; a count that is
not +4 with the timing contiguous does not make them erasures. An input that
drops an exact number of whole frames can't be told from a count step: nothing
is counted as lost then, the audio layer's own checks reject the stitched frames.

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
sent only when the main eigenvalue holds >= 50 % of the raw trace and, after the
noise is removed, >= 80 % of the sum of the eigenvalues' magnitudes (the
subtracted matrix can be indefinite: all eigenvalues from a Jacobi solver); the
packet carries its positive semi-definite part (negative eigenvalues set to 0).
Packets only from frames whose null symbol is contiguous with the previous
frame. `start_s` / `end_s` = that null symbol's 1 ms window. Talkers
only while the FIC decodes (EId known, a good FIB in the last second). Through
`process()` (one antenna) the transmitters are listed in the facts, without
packets.

`t.lat` / `t.lon` are not filled: they serve the array calibration from known
directions (`array-calibration.md`), which takes 2-60 deg elevation and >= 5 km;
a ground transmitter is near 0 deg and its height is unknown (FIG 0/22 has none).
Its position goes to the map instead (`kind` station, id = the talker id), so the
DF panel shows the DF error against it.

On the 🗺 Map's 📡 DF panel the talkers are "DAB transmitters" 🗼 (list columns
TII / Ensemble / Nulls - the null symbols that gave a packet -, sorted by TII
code); the Dist and Δ pos columns appear only for a transmitter whose ensemble
sent its FIG 0/22 position, and take the Ensemble column's place (the label is
then in the row's tooltip).

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
The programme's facts (Sub-channel, Codec, Voice codec, Audio frames, RS
corrections, Text) are removed when the programme changes or no service matches
the option, and set again once the new programme provides them.

The services table: one row per service, an unchanged row sent again every 15 s
(the page's "Seen"), at most 32 rows a second.

Events: ensemble found / lost (no FIC for 3 s, or another EId on the block), the
services (one summary, then each new one), the programme, a service gone,
multiplex reconfiguration announced (with the time to it) / done / withdrawn,
announcements (FIG 0/19; alarms marked ⚠), each transmitter confirmed / no
longer received, the dynamic label (900 s dedup).

Raw frames (decoder log): one line per FIB that passed its CRC:
`FIB <CIF count>.<0..2> <30 data bytes in hex>`.

`host.valid(start, end)` for every 96 ms transmission frame with at least 2 FIBs
passing their CRC - one frame late, and only when the input was contiguous
through it (the frame's start and the next one's where the tracking expected
them, CIF count +4): a frame that straddles samples the host dropped gives none.

## Audio layer

`dab::SubchannelDecoder` (`dab_audio.cpp`) takes the selected sub-channel's
logical frames (24 ms, kbit/s x 3 bytes, after Viterbi and energy dispersal)
and gives 8 kHz mono audio, the dynamic label and the "Codec" / "Audio
frames" / "RS corrections" / "Text" facts.

**DAB: MPEG Layer II** (ETSI TS 103 466 V1.2.1, `audio_mp2.cpp`,
`audio_mpa.cpp`). 48 kHz (MPEG-1): one audio frame per logical frame; 24 kHz
(MPEG-2 LSF): one per two, starting in the logical frame that holds the
header (clause 5.5.2, annex B.1). A frame is played only when all three
checks pass: a header valid for DAB (clause 5.3.1.3: Layer II, CRC present,
48 / 24 kHz, no emphasis) with the sub-channel's bit rate; the header CRC
(annex B.2, over the header, bit allocation and ScFSI); the ScF-CRC (annex
B.3, CRC-8 over the scale factors' 3 MSBs in 2 or 4 groups, carried at the
end of the PREVIOUS frame). An encoder that writes no ScF-CRC: after 25
mismatches in a row with good header CRCs the check is switched off (logged)
until the next match. PAD: F-PAD in the last 2 bytes, the X-PAD in front of
the ScF-CRC in reversed byte order (EN 300 401 clause 7.4). PCM from
**minimp3** (vendored `minimp3.h`, github.com/lieff/minimp3 commit ea99364,
CC0), only for frames that passed; stereo is downmixed (L+R)/2, dual channel
plays the first channel.

**DAB+: HE-AAC v2** (ETSI TS 102 563 V2.1.1; RS in `audio_fec.cpp`). Five logical
frames = one super frame. Sync: the Fire code (clause 5.2), tried on the
received bytes and, failing that, after RS correction; lost after 3 unusable
super frames. RS(120,110) (shortened RS(255,245), GF(2^8) with 0x11D, roots
a^0..a^9) over the virtual interleaver (clause 6), up to 5 byte errors per
code word; a word is "uncorrectable" when the locator degree is above 5, its
roots are not distinct positions inside the word, or the syndromes are not
zero after the correction (measured on words with 6-12 errors: 99.97-99.98 %
flagged, 0.015-0.045 % miscorrected - the AU CRC catches those). The super
frame header gives dac_rate, sbr_flag, aac_channel_mode, ps_flag,
mpeg_surround_config and the AU starts (each bounds-checked); each AU's
CRC-16 decides ok / bad, and an AU that overlaps a logical frame the receiver
reported lost is never trusted. PAD from the AU's leading
data_stream_element (clause 5.4.3). PCM through the user's **libfdk-aac**
(below) with an AudioSpecificConfig built from the header (AAC-LC with
frameLengthFlag = 1 for 960 samples, SBR / PS signalled explicitly and
hierarchically; bit-identical to the ASC of Opendigitalradio's DAB+
encoder); a bad AU gets the library's concealment (TS 102 563 annex A). A
mono core (mono, PS) is decoded to mono by the library, which also skips the
PS synthesis; a stereo core is decoded to stereo and downmixed in the plugin
(the library's own stereo-to-mono downmix is L+R and clips).

**PAD and dynamic label** (EN 300 401 clause 7.4, TS 102 980 V2.1.2,
`audio_pad.cpp`): short X-PAD; variable-size X-PAD with up to 4 contents
indicators, length codes 4-48 and the end marker; continuation without a
contents indicator; interrupted data groups. DLS data groups (application
types 2 / 3), checked by their CRC; segments by toggle, first / last, SegNum
(up to 8 x 16 bytes); "clear display" reports an empty label (the receiver
removes the Text fact, no event). DL Plus, MOT
and user applications are skipped. Character sets (TS 101 756 table 1): 0
EBU Latin (annex C table C.2, checked against C.1 and against the receiver's
own table), 6 UCS-2, 15 UTF-8 (validated, bad sequences -> U+FFFD); 0x0A /
0x0B become spaces, 0x1F and other controls are dropped, spaces trimmed. A
label is reported only when complete and different from the last one (event,
900 s dedup).

**Output** (`audio_out.cpp`): 48 / 32 / 24 / 16 kHz mono -> Kaiser-windowed
FIR (pass band to 3.4 kHz, stop band from 4.0 kHz, 60 dB; 2kD+1 taps, so the
delay is whole output samples) -> integer decimation to 8 kHz. A bad or lost
frame becomes silence of its length while synced, with the last 4 ms held
back so the audio fades out before a gap and in after it (no clicks);
nothing without sync; samples clamped to +-1, NaN / inf -> 0. Without a
listener (`voice_wanted` false) only minimp3 / the AAC library and the
output stage are skipped - framing, RS, CRCs, PAD and labels always run.

### The AAC library (DAB+ audio)

Fraunhofer FDK AAC (libfdk-aac, the FDK AAC licence - not GPL) is installed
by the user and never shipped, vendored or linked: `audio_aaclib.cpp` loads
it with dlopen at first use, like the AMBE codec (mbelib). It looks for
`$KRAKEN_AAC_LIB` first (`none` / `off` disables it), then

- Linux / Raspberry Pi OS: `libfdk-aac.so.2`, `/usr/local/lib/libfdk-aac.so.2`, `libfdk-aac.so.1`;
- Windows: `libfdk-aac-2.dll`, `fdk-aac.dll`, `libfdk-aac.dll` (next to the program, in the working directory or on the PATH);
- macOS: `libfdk-aac.2.dylib`, `/opt/homebrew/lib/...`, `/usr/local/lib/...`.

Install:

| System | |
|---|---|
| Debian / Raspberry Pi OS | `sudo apt install libfdk-aac2` (Debian: the `non-free` component; Raspberry Pi OS normally has it). Older releases: `libfdk-aac1` (0.1.6) works too |
| Ubuntu | `sudo apt install libfdk-aac2` (multiverse) |
| Windows (MSYS2 UCRT64) | `pacman -S mingw-w64-ucrt-x86_64-fdk-aac`, then copy `/ucrt64/bin/libfdk-aac-2.dll` into the kraken_doa_v2 folder (or `KRAKEN_AAC_LIB=<path>`) |
| macOS | `brew install fdk-aac` |
| Docker | copy the host's `libfdk-aac.so.2` into `docker-data/codecs/` (the entrypoint sets `KRAKEN_AAC_LIB`) |

The "Voice codec" fact and the sidebar's Digital Decoders box show its state,
e.g. `libfdk-aac.so.2 (FDK AAC decoder 3.2.0)` - the decoder module's own
version (fdk-aac 2.0.2 reports 3.2.0, 0.1.6 2.5.17, 0.1.2 2.4.7) - or `not
installed (libfdk-aac2)`; then everything but the PCM still runs. Decoder
versions below 2.x and libraries that lack a function or report no version
are refused. Tested: fdk-aac 2.0.2 (the full test set), 0.1.6 and 0.1.2
(bit-identical DAB+ decoding after alignment); six hostile stand-in
libraries (missing functions, absurd stream info, error codes / NULL, broken
library info, extreme samples): no crash, no bad audio (ASan / UBSan clean).

## Test signal generator

`tools/dab_gen.cpp` is a complete DAB transmission mode I transmitter plus a
multi-transmitter, multi-antenna scene writer, written from EN 300 401
V2.2.1, TS 103 466 V1.2.1, TS 102 563 V2.1.1 and TS 101 756 V2.5.1 (FIG 0/22
from EN 300 401 V1.4.1 clause 8.1.9) independently of the receiver:

- FIC: FIG 0/0 (first in each frame, with the CIF count), 0/1 (short form
  for UEP, long form for EEP), 0/2 every frame; in a 5-frame cycle 1/0, 0/9,
  0/10, 1/1 per service, 0/17, 0/5, 2/0 (+ 2/1) in 2 UTF-8 segments, 0/22;
  end marker, padding, CRC, energy dispersal, the convolutional code, FIC
  puncturing, 4 code words per frame.
- MSC: per sub-channel energy dispersal, UEP (table 15 via the table 8
  index) or EEP 1-4 A / B, time interleaving over 16 CIFs, the CIF multiplex,
  PRBS padding of unused CUs.
- OFDM: frequency interleaving, pi/4-DQPSK from the phase reference symbol,
  2048-point IFFT at 2.048 MHz, guard and null symbols; resampled (Kaiser
  sinc, 48 taps, ~90 dB) to the output rate.
- TII: each transmitter's MainId / SubId carrier pairs (clause 14.8.1, table
  26) in the null symbol of the frames with CIF count mod 8 in 0..3 (every
  other frame). `"tii_pair_phase"` per transmitter: `"k"` (default, the
  standard: both carriers of a pair carry the first carrier's phase reference
  value) or `"k+1"` (the second carries its own, as several real transmitters
  do - see *TII*).
- Audio, a known tone per service: DAB mono MPEG-1 Layer II 48 kHz (32-192
  kbit/s) or MPEG-2 LSF 24 kHz (the frame split over 2 CIFs), the tone at a
  sub-band centre, header CRC, ScF-CRC, F-PAD + short X-PAD; DAB+ AAC-LC 960,
  48 kHz mono (real MDCT, quantisation, the AAC code books of
  `dab_gen_aac_tables.hpp`), PAD in a leading DSE, super frames with Fire
  code, AU CRCs and RS(120,110). A dynamic label per service in short X-PAD.
- Scene: N transmitters with the same ensemble but their own TII, bearing
  (array-relative, CCW from ANT0, as `sdk/tools/uca_scene.py`), delay (within
  the 246 us guard), level and carrier phase; a UCA (5 x 0.12 m by default,
  ANT0 on +x, clockwise); white noise at `snr_db`, CFO, sample clock offset,
  the block's offset from the file centre. A transmitter's FIG 0/22 position
  is `range_km` from the station (`"station"`, make_testsignal's
  `{station}`).
- Output: cu8 (composite rms 0.2 of full scale) or cf32, antennas
  interleaved per sample, + `OUT.json` (rate, channels, tuner centre, block,
  the ensemble, services and the truth per transmitter). The length is
  rounded to whole 0.96 s (40 CIFs: the LCM of the LSF frame, the super frame
  and the TII cycle) and built periodic over the loop, so the file loops
  without a decode error; at the seam the CIF count and the FIG 0/10 time
  step back.

```
cd kraken_doa_v2/plugins
g++ -std=c++20 -O2 -Idab -Isdk dab/tools/dab_gen.cpp -o dab_gen
./dab_gen --selftest
./dab_gen --scene dab/tools/scene_sfn3.json --out sfn3.cu8                        # 2.4 MHz, 5 antennas
./dab_gen --scene dab/tools/scene_sfn3.json --rate 2048000 --channels 1 --out ref.u8   # rtl_sdr style
./dab_gen --scene S.json --describe truth.json --dump DIR       # truth; DIR/<SId>.mp2 | .loas | .au
dab/build/decoder --file sfn3.cu8 --channels 5 --uca 0.12 --rf 227360000 [--opt service=Plus]
```

Options overriding the scene: `--seconds S | --frames N`, `--rate HZ`,
`--channels M`, `--radius M`, `--rf HZ`, `--snr DB`, `--seed N`, `--cfo HZ`,
`--sco PPM`, `--offset-khz K`, `--station LAT,LON`, `--format cu8|cf32`;
without `--scene` a built-in default (one Layer II and one DAB+ service, one
transmitter). The self-test checks the PRBS (table 12), the frequency
interleaver (table 25, a permutation), TII c=1 p=11 against figure 64, the
mother code, every UEP / EEP profile filling its sub-channel, the RS roots,
MDCT -> IMDCT reconstruction and the CRC check values. Talker ids are
`%04X-%02d-%02d` (EId-MainId-SubId).

Scene file (`tools/scene_sfn3.json`, `tools/scene_single.json`):

```
{"rf_hz": 227360000, "seconds": 4.8 (or "frames"), "rate": 2400000, "channels": 5, "radius_m": 0.12,
 "snr_db": 20, "seed": 1, "cfo_hz": 0, "sco_ppm": 0, "offset_khz": 0, "station": [lat, lon],
 "ensemble": {"eid": "10BC", "ecc": "E0", "label": "...", "short": "...", "ext_label": "UTF-8 (FIG 2/0)",
              "lto_half_hours": 2, "inter_table": 1, "utc": "2026-11-16T12:00:00", "cif0": 0},
 "services": [{"sid": "1001", "label": "<=16", "short": "<=8 of its characters", "ext_label": "",
              "type": "dab" | "dabplus", "kbps": 128, "rate_hz": 48000 | 24000 (dab),
              "protection": "1".."5" (UEP) | "1A".."4A" | "1B".."4B" (EEP),
              "pty": 14, "lang": 9, "tone_hz": 1125, "dls": "<=128 bytes", "subchid": auto, "start_cu": auto}],
 "transmitters": [{"name": "North", "main": 12 (0..69), "sub": 3 (1..23), "bearing": 40, "delay_us": 0,
              "level_db": 0, "phase_deg": 0, "range_km": 22, "tii_pair_phase": "k" | "k+1"}]}
```

Debug playback (`tools/testsignals.json`, 4.8 s looped, ~115 MB each):

| id | content | expect |
|---|---|---|
| `dab-sfn-three` | `scene_sfn3.json`: ensemble 10BC on 12C, 3 services (Layer II 128 UEP 3, 1125 Hz; DAB+ 64 EEP 3-A, 1750 Hz; DAB+ 32 EEP 1-B, 600 Hz); TX 12/3 @ 40 deg 0 us 0 dB, 12/7 @ 150 deg 37.5 us -4 dB (pair phase k+1), 31/15 @ 260 deg 112.25 us -8 dB; 20 dB S/N | the three talkers at their bearings; events "Ensemble found: Kraken SFN", "Transmitter 10BC-31-15", the first service's dynamic label |
| `dab-single-offset` | `scene_single.json`: ensemble 10C2, 6 services (Layer II 48 kHz and LSF 24 kHz, four DAB+ at 32-72 kbit/s EEP 1-A / 2-A / 4-A / 3-B); TX 5/1 @ 300 deg; block +24 kHz, CFO +370 Hz, SCO +12 ppm, 18 dB S/N; the CIF count wraps and the UTC date turns midnight in the loop, LTO -5 h | 10C2-05-01 at 300 deg; events "Ensemble found: Kraken Mux B", "6 services: Kraken One", the first service's dynamic label |

How the generator was checked (the reference receivers are GPL programs,
built in a scratch folder and run as black boxes - their sources were not
used): welle-cli (welle.io 512558d1) and eti-cmdline + dablin 1.16.1
decoded both scenes' ensemble (EId, ECC, labels incl. FIG 2), every service
(SId, sub-channel, start, size, bit rate, protection, PTy, language)
identical to the truth, the audio formats, every dynamic label, UTC across
the midnight wrap, LTO -05:00 and the frequency offset (24362 against 24370
Hz); every service played its tone (375, 1125, 1312.5 Hz Layer II; 500,
600, 1500, 1750, 2250, 3000 Hz AAC; median SINAD 66-70 dB Layer II, 86-88
dB AAC); the loop plays across every seam without a FIC CRC error. ffmpeg
decodes the `.mp2` dump at 1125.00 Hz (header CRCs pass `-err_detect
crccheck`) and the `.loas` at 1750.00 Hz with the 960-sample frame. TII:
single transmitters always right; from the 3-transmitter scene's antenna 0
welle-cli found all three. A beam scan with each code's carriers gave 39.9 /
149.8 / 259.8 deg (truth 40 / 150 / 260) and +20.0 / +16.1 / +12.2 dB
carrier power (0 / -4 / -8 dB at 20 dB S/N). FIG 0/22, which no reference
receiver decodes any more, parsed from welle-cli's FIC dump by a parser
written from V1.4.1: positions within one LSB, TD within rounding.

Not covered by the generator: stereo, joint stereo, SBR / PS, 32 kHz AAC,
variable-size X-PAD, DL Plus, MOT, charsets other than 0 in the labels,
FIGs 0/7, 0/8, 0/13, 0/18-0/21 (reconfiguration, announcements); the ScF-CRC
and FIG 0/22 are checked only against the standards' text (no black box
reads them).

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
- Audio layer: ETSI TS 103 466 V1.2.1 clauses 5.3, 5.4.2, 5.5, tables 4-6 and
  9-16, annex B.2 / B.3 (Layer II in DAB); TS 102 563 V2.1.1 clauses 5.2, 5.4,
  6, annex A (DAB+); EN 300 401 V2.2.1 clause 7.4 and annex E (PAD); TS 102
  980 V2.1.2 clause 7 (dynamic label); TS 101 756 table 1 and annex C
  (character sets); the ISO/IEC 14496-3 AudioSpecificConfig layout (AOT,
  sampling frequency index, frameLengthFlag), checked bit for bit against the
  ASC a DAB+ encoder emits.

The tables are in `dab_tables_gen.hpp`, machine-extracted from the documents'
text (pdftotext) and checked by `dab::self_test()` at start (1536 distinct
carriers from the frequency interleaver against table 25's entries, puncturing
vector weights 8 + PI and nesting, 70 distinct TII patterns of 4 bits and figure
64, every UEP profile and EEP size filling its sub-channel exactly, table 23
covering every carrier, EBU Latin spot checks). No GPL DAB implementation was
consulted.

Licences: the plugin is under kraken_doa_v2's licence. `minimp3.h` is
minimp3 (CC0) unchanged; `tools/dab_gen_aac_tables.hpp` holds VisualOn
vo-aacenc's AAC code book tables (Apache-2.0, notice in the file) and is used
only by the generator; libfdk-aac is the user's (never shipped). All three in
`kraken_doa_v2/THIRD_PARTY_NOTICES.md`.

## Limits

- Mode I only (the only one in V2.x). L-band works, but blocks are named only in
  Band III (TS 103 461 table E.1); elsewhere the frequency is shown.
- Antenna 0 is demodulated; the antennas are not combined for a better decode.
- No resampler: the sample clock offset's inter-carrier interference is not
  corrected - at 40 ppm the FIC symbols' MER is 29.7 dB at 54 dB SNR (60 dB at 0
  ppm); RTL-SDRs are within a few ppm.
- Only the selected programme is decoded; packet mode / data services, DMB and
  conditional access are named, not decoded.
- Audio: Layer II sample data has no CRC - under bit errors a frame that passed
  the header CRC and ScF-CRC can still carry damaged samples (muted only when a
  check fails, no sub-band concealment). The ScF-CRC was verified on the air
  only in its 4-group form (48 kHz >= 56 kbit/s per channel, and 24 kHz); the
  2-group form and the X-PAD before 2 CRC bytes follow the standard, tested
  with synthetic streams. MPEG Surround plays its core; PS plays the mono core
  (~2.9 dB louder than (L+R)/2 of the PS stereo); dual channel the first
  programme. DL Plus and MOT slideshow are not decoded. DAB+ audio needs the
  user's libfdk-aac.
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
  ("by code"). With 12/7 sending the k+1 pair phase (`"tii_pair_phase"`, the
  scene since 2026-11-16): 40.1 / 149.9 / 259.9 deg, -4.0 / -8.2 dB, +37.5 /
  +112.2 us, 12/7 tracked as "k/k+1", the others "k/k"; the same from the debug
  playback's generated file (`make_testsignal.py dab-sfn-three`). Own 3-transmitter scene (12/3 @ 40, 12/5 @ 150 +23.4 us -4 dB,
  7/11 @ 260 +61 us -9 dB): 39.9 / 149.9 / 260.2 deg, +23.4 / +61.0 us.
- `scene_single.json` (block +24 kHz, CFO +370 Hz, SCO +12 ppm, CIF count and
  midnight wrap, LTO -5 h): CFO +24370 Hz, clock +11.9 ppm, 6 services, 5/1 @ 300
  -> 299.9 deg (also from `make_testsignal.py dab-single-offset`). The block is
  shown as 227.384 MHz: 24 kHz off the raster, so not named 12C.
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

**With the audio layer**: the generator's Layer II service plays its 1125 Hz
tone (measured 1125.0 Hz in the `--audio` WAV), 121 / 121 audio frames ok, its
dynamic label shown; its DAB+ service reaches super frame sync, 144 / 144 AUs ok
- "not installed" without the AAC library, with libfdk-aac 2.0.2 its 1750 Hz tone
(1749.7 Hz in the WAV). UK 12B, BBC Radio 1 (Layer II joint stereo): 38.5 s of
audio, 1557 frames ok, 9 bad (the recording's gap), the dynamic label shown.

**The audio layer on its own** (an offline harness, 2026-11-16):
- Layer II decoder choice: minimp3 against ffmpeg on all 96 DAB combinations
  (48 kHz 32-384 kbit/s mono / stereo / joint / dual as TS 103 466 table 12
  allows, 24 kHz 8-160 kbit/s), each frame on its own: at most 3e-5 apart
  (float rounding).
- Synthetic DAB streams (twolame with CRC; PAD, DLS and ScF-CRC written by an
  independent generator): all 96 every frame ok; 7 / 7 labels on each (EBU
  Latin specials, UTF-8 Cyrillic / CJK / an emoji across segments, UCS-2,
  control codes, a 128-byte label, clear, a label after the clear); PCM against
  ffmpeg's decode: corr 1.000000, SNR 61-67 dB from 64 kbit/s up and on
  unmodified streams (the lowest rates 0.97-0.999: the test generator's
  overwritten sample bits, decoded differently).
- Synthetic DAB+ streams (Opendigitalradio's fdk-aac DAB+ encoder, RS parity
  added independently): 9 configurations (HE-AAC v2 48 / 32 kHz, HE-AAC stereo
  / mono, AAC-LC 48 kHz 128 / 192, AAC-LC 32 kHz mono), every AU ok, 7 / 7
  labels, PCM against a direct fdk decode corr 1.000000, SNR 65.5 dB (PS streams
  0.9993-0.9996: mono core against the averaged PS stereo).
- Errors: DAB+ AAC-LC 128 at BER 1e-4 996 / 996 AUs (262 RS words corrected, 0
  uncorrectable), 1e-3 991 / 996 (1626 / 1), 3e-3 538 / 996 (4 of 7 labels), 10 %
  lost frames 756 / 996; DAB 128 at BER 1e-4 807 / 834, 1e-3 610 / 834, 1 % lost
  826 / 834. Random input: no frame, no label, no PCM. No clicks at gaps (the
  largest sample step equals the clean stream's).
- Real broadcasts (ETI files, every audio sub-channel): DAB+ Bayern 11D (14
  sub-channels, 99.5-100 % AUs ok), Sydney 9A (33, 99.3-100 %; the failures in
  the same super frames on every channel = the recording's reception gaps),
  Melbourne 9A (29, ~3 of 40 super frames unusable per channel, as the Fire-code
  count on the ETI file itself) - AAC-LC, HE-AAC, HE-AAC v2 at 32 / 48 kHz;
  Layer II BBC National (15 min, 8 sub-channels incl. 128 joint stereo, 192
  stereo, 80 / 64 mono and a 24 kHz LSF test channel: every frame ok), two more
  ensembles 4 x 5034 / 5034 and 2499 / 2500; the ScF-CRC matched on every frame
  of every encoder; PCM against ffmpeg corr 1.000000, SNR 66 dB.
- CPU per second of audio (one core here; a Pi 5 ~3x): Layer II 48 kHz 2.2 ms
  (0.03 ms without a listener), 24 kHz 0.9 ms; AAC-LC 144 kbit/s stereo 4.6 ms
  (0.6), HE-AAC 96 kbit/s 5.4 ms (0.4), HE-AAC v2 32 kbit/s 4.0 ms (0.13).
- ASan + UBSan: the error matrix, a structured fuzzer (random PAD / X-PAD, all
  charsets, valid Layer II headers with random bodies at every rate, DAB+ super
  frames with a correct Fire code and random AUs with valid CRCs fed to the
  library, odd sizes, `want_audio` toggling) and the hostile-library tests: 0
  reports.

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
