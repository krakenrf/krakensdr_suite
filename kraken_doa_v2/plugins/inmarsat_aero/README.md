# Inmarsat Classic Aero decoder plugin

Inmarsat's Classic Aero service (the AMSS of ICAO Annex 10 Vol III, Part I
chapter 4): satellite data link and telephony for aircraft - ACARS, ADS-C
and CPDLC (FANS-1/A), log-ons, call set-up, cockpit / cabin voice. The
plugin decodes, in one VFO:

- the **P channel** (GES -> aircraft through the satellite, continuous TDM,
  L band 1.5 GHz) at every rate the standard defines - 0.6 / 1.2 / 2.4
  kbit/s A-BPSK and 4.8 / 10.5 kbit/s A-QPSK, rate and carrier found by
  itself;
- the **C channel** (voice, 8.4 kbit/s A-QPSK): frames, the sub-band
  signalling, and the AMBE voice through the user's own codec library;
- **R / T channel bursts from aircraft** (1.6 GHz, received directly from
  an aircraft in line of sight): every burst a talker of its aircraft - a
  DoA per aircraft - and its ACARS / ADS-C (positions on the map).

## Signal

| Channel | Direction | Rate (bit/s) | Modulation | Frame |
|---|---|---|---|---|
| P | GES -> aircraft, 1525 - 1559 MHz | 600, 1200, 2400 | A-BPSK | 2 / 1 / 0.5 s, 6 signal units (SUs) |
| P | GES -> aircraft | 4800, 10500 | A-QPSK | 0.5 s, 12 / 26 SUs |
| R (random access) | aircraft -> GES, 1626.5 - 1660.5 MHz | 600, 1200, 2400, 10500 | A-BPSK / A-QPSK | one burst = one 19-octet SU |
| T (reserved TDMA) | aircraft -> GES | 600, 1200, 2400, 10500 | A-BPSK / A-QPSK | burst identifier + 2 .. 31 SUs |
| C (voice) | both | 8400 (also 5250, 6000, 10500, 21000) | A-QPSK | 0.5 s: 25 voice frames + 3 SUs |

- **A-BPSK**: every bit turns the carrier by -90 deg ("0") or +90 deg ("1"),
  then a root-raised-cosine filter (decoders call it "MSK" or pi/2-BPSK).
  **A-QPSK**: offset QPSK, the bits alternately on I (0 / 180 deg) and Q
  (90 / 270 deg), Q one bit late.
- P channel frame: unique word (32 bits, 1110 0001 0101 1010 1110 1000 1001
  0011; A-QPSK: on I and Q), format ID 0001, superframe marker (frame
  number in the 8 s superframe), dummy bits (16 / 178 at 4.8 / 10.5
  kbit/s), then the information field: SUs of 96 bits (CRC-16/X-25 in the
  last two octets), scrambled (15-stage, 1 + X + X^15), coded (rate 1/2,
  K = 7, 133 / 171 octal; the encoder runs on across frames), interleaved
  (64 rows, rows permuted j = 27 i mod 64; 6 / 9 / 18 / 36 / 78 columns).
- C channel 8.4 kbit/s frame: a 52-bit unique word on I and another on Q,
  then 16 blocks of 64 x 4: the rate 2/3 code (G1 G2 G1 per two bits) of
  2730 scrambled bits - reference bits, 25 primary fields of 96 bits (one
  20 ms AMBE voice frame each) and 24 sub-band fields of 12 bits (three
  SUs).
- Bursts: preamble (unmodulated carrier, then alternating bits: 150 + 74
  bit periods at 600, 126 + 74 at 1200, 248 + 256 at 10500 bit/s), the
  unique word, one or more interleaver blocks (encoder and scrambler reset
  per burst).

Frequencies (information - check them for your satellite and region; the
decoder doesn't depend on them): the sigidwiki "Inmarsat Aero" page
(2026-11-22) gives the L-band P channels at 1545 - 1547 MHz, 600 / 1200
bit/s around 1545 MHz and 10.5 kbit/s around 1546 MHz. The GES's own
channel tables (the system table broadcasts the plugin decodes, below)
name its P / R channels: receive channels 1510 MHz + N x 2.5 kHz,
transmit channels 1611.5 MHz + N x 2.5 kHz.

## Receiving it

- An L-band antenna for 1.5 GHz with a clear view of the satellite
  (right-hand circular: a patch or a short helix) and an LNA at the
  antenna - it can be powered from the receiver: the antenna bias tees
  (top bar, `docs/dev/bias-tees.md`). The R820T tuners reach 1.76 GHz.
- With one antenna, decode in **independent mode** (top bar; the antenna's
  tuner on the band, the others idle): no calibration, so heimdall never
  cuts the noise-source bursts out of the stream - in coherent mode those
  gaps fall into frames and calls (off air, 2026-11-22: 0 gaps in 420 s
  independent, the same P channel counts as coherent, `sigtool.py capture
  --channel 0` records the tuner).
- A VFO of 15 - 25 kHz on the channel frequency (min_vfo_rate 15 kHz). The
  carrier may be off by up to +-2.5 kHz (an RTL-SDR's 1 ppm is 1.5 kHz at
  1.5 GHz): the plugin finds it, tracks it and shows it (Carrier offset
  fact). It doesn't hand it to the receiver's AFC: the steps that moves
  the input cost frames (measured, under Test results). Moving the VFO
  by up to 2 kHz while a channel is synced keeps it (the demodulator
  follows the move - nothing lost); a larger move, or a retune, starts the
  search over. Or the whole band at once: a wide VFO in **All channels
  in the VFO** (below).
- Where to look (off air, Inmarsat-4 F1 at 143.5 E, a patch antenna,
  2026-11-22): sixteen 600 bit/s P channels on a 5 kHz raster from
  1545.010 to 1545.210 MHz, a 1200 bit/s one at 1545.070, two 10.5 kbit/s
  ones at 1546.055 and 1546.070 MHz. The GESs' channel advice names the R
  channels (1646.5075 to 1646.6075 MHz). Other carriers nearby (1545.27 to
  1546.29, 1548.2 to 1548.4, 1549.4 to 1551.5 MHz) are other systems.
- Aircraft bursts (Channel "R / T bursts"): a VFO on the aircraft's
  transmit channel in 1626.5 - 1660.5 MHz; only aircraft in line of sight
  (near an airport) are strong enough. The DoA of a P channel or a
  to-aircraft C channel is the satellite's.
- **Array size for direction finding at L band.** A uniform circular
  array gives unambiguous bearings only while neighbouring elements are
  less than half a wavelength apart. With 5 elements the spacing is
  2 R sin(pi/5) = 1.176 R, so the radius must stay under 8.3 cm at 1545
  MHz (wavelength 19.4 cm) and under 7.7 cm at 1650 MHz (18.2 cm). The
  usual VHF / UHF radius of 12 cm (14.1 cm between neighbours: 0.73
  wavelengths at 1545 MHz) gives ambiguous bearings here. The test scenes
  (`tools/testsignals.json`) use 6 cm: 7.1 cm between neighbours, 0.36
  wavelengths at 1545 MHz and 0.39 at 1645 MHz. Set the array's radius in
  the receiver to the real one.

## Options

- **Channel**: Auto (default: the acquisition finds a P or C channel and
  its rate), P 600 / 1200 / 2400 / 4800 / 10500 bit/s or C channel 8400
  (search that rate only), R / T bursts from aircraft (every burst rate),
  **All channels in the VFO** (every P and C channel in a wide VFO at
  once; a VFO under 96 kHz holds one channel and runs Auto).
- **Calls (all channels)**: with All channels, several calls at once are
  **mixed** (default: every call, summed through a limiter so that two at
  once don't clip), or only **the newest** is heard - the call that began
  last, until it ends; then the one before it again if it still runs.

The plugin takes the VFO's own rate (`sample_rate` 0) and brings it to 24
kHz itself for the single-channel modes, with the receiver's own
resampler (liquid `msresamp`, 60 dB; nothing at 24 kHz): those modes get
exactly the samples they got before.

## All channels in the VFO

A VFO as wide as the band (up to 2.4 MHz, the tuner's whole span) with
Channel "All channels in the VFO": every Aero carrier in it is found,
decoded and listened to at once, in one plugin process.

- How: one FFT filter bank (6400 points every 3200 samples at 2.4 MHz -
  375 Hz bins) cuts a 24 kHz channel out for each carrier (flat to +-8.6
  kHz, gone at +-12 kHz; one 64-point inverse FFT per channel and block).
  Each channel is the single-channel decoder (Auto) unchanged - the same
  counts by construction (measured below). The bank's bins are also the
  survey's spectrum: every 0.25 s narrow peaks (4 dB over the local
  floor in 1.1 kHz: 600 / 1200 / 2400 bit/s) and flat-topped wide bumps
  (1.5 dB in 6.4 kHz: 4.8 / 8.4 / 10.5 kbit/s) not yet followed get a
  trial decoder (up to 24 narrow and 16 wide at a time; a strong newcomer
  replaces a trial that hasn't locked). A trial that never locks is
  closed after 4 s, one that locks but passes no CRC after 9 s, and its
  frequency is left alone for 90 s: the other systems' carriers between
  the Aero channels (1545.27 - 1546.29 MHz and others) never become
  channels. A C channel assignment on any channel naming a frequency in
  the VFO opens that C channel at once (it is often outside: then not
  followed - a retune would take the whole band away).
- The window: "Channels" (how many of each kind, how many carriers are
  being tried), one "Ch" line per channel - its nominal frequency (the
  received one corrected by the receiver's offset, measured from the
  locked P channels against the frequencies the signalling advertises;
  "~" before it while that isn't known), kind and rate, GES, signal units
  (C: clean frames), level over the floor, how long since its last good
  frame, its voice state -, "Receiver offset", "Signal units" over all
  channels, the satellite table and the GES advice once. Every event
  carries its channel's frequency first ("1545.1050 · Log-on confirm:
  ..."); a channel that goes quiet (40 s; C: 8 s) is dropped with an event
  of what it brought. The aircraft table is one table over all channels
  (an aircraft heard on two channels is one row, its messages summed).
- Listening: the decoded calls go to the VFO's audio (Demod Digital):
  mixed or the newest (option above); the voice state names the call(s)
  ("2 calls, mixed: 1545.7515 1544.8718"). Each C channel keeps its own
  codec state.
- Cost (Pi 5, live, below): ~1/3 of a core at 2.4 MHz with 22 channels;
  the bank alone is what an empty band costs.
- Not in it: R / T bursts (their own mode), following a C channel outside
  the VFO.

## What is decoded

Facts: Channel (rate, modulation; "syncing" / "searching (why)"), Carrier
offset, Signal units (passed / total, fill-in), SU types (counts per
message type), GES (ID), GES time (UTC broadcast), Satellites (system
table: every satellite's ID and longitude, broadcast one SU per
satellite), Satellite (the entry whose P channel is the one received -
the nearest within 2.5 kHz - and how far off the receiver hears it: the
RTL-SDR's error), GES 0x.. channels (its P / R channel advice), User data
(messages, ACARS, bad BCS), Aircraft heard, Last ACARS, Voice codec, C
channel frames, Voice codec frames (mbelib's Golay corrections per 20 ms
frame: near 0 for a cleanly received voice frame, ~4.3 for random bits).

Events (signal units that passed their CRC, by type; appendix 3's message
type list):
- log-on confirm / reject / interrogation / prompt / log-off, data channel
  reassignment, log-on / log-off acknowledgements (AES = the aircraft's
  24-bit ICAO address, GES ID, satellite / beam, the AES's voice codec);
- call announcements and call progress (connect, channel release, cause),
  **C channel assignments** (AES, GES, priority, the to- and from-aircraft
  frequencies);
- user data (ISU 0x71 + SSUs, 3 / 4-octet LSUs), reassembled: ACARS blocks
  (0xFF 0xFF, SOH, mode .. suffix, BCS checked, as `plugins/acars`):
  registration, flight, label, block, text; messages of several blocks
  joined; other user data with its length and first octets (X.25 packet
  type where the header is one);
- **ADS-C** (ARINC 622 framing, CRC checked; ARINC 745 groups): basic /
  emergency / event reports (position, altitude, time, figure of merit),
  flight ID, earth / air reference (track, ground speed, vertical rate),
  meteo, predicted route, projections, airframe ID - the aircraft on the
  🗺 Map (popup: everything decoded). CPDLC (AT1 / CR1 / CC1 / DR1) and
  ADS-C contract requests are recognised and counted, not decoded;
- with "Show all messages": system table broadcasts' contents, P/R and T
  channel control, T channel reservations, requests for acknowledgement /
  acknowledgements, signal units of other types;
- R channel (bursts): log-on requests (AES class), telephone / data access
  requests, acknowledgements, call progress, user data; T bursts: their
  SUs as above.

Table: one row per aircraft (AES ID): registration, flight, last label,
messages, last text; the 🗺 Map and the 📡 DF panel use the same key.

Voice (C channel; Listen, demod Digital): the 25 frames of every 500 ms
frame through the codec, 8 kHz, the output limiter of
`plugins/lib/dig_vocoder` - see *Digital voice codec* below. Dialled
numbers in the call information SUs are not shown.

Raw frames (Logging "Log All"): every SU in hex ("P SU ..", "C SU ..", "R
SU .."), T bursts, ACARS blocks.

## ID separation (DoA per aircraft)

- P channel and the to-aircraft C channel: everything comes from the
  satellite - no talker; the VFO's own DoA (Digital squelch: from the
  frames that passed their checks) is the satellite's bearing.
- Aircraft bursts: each R / T burst that passed its CRC names its sender -
  the AES ID (R SUs carry it, a T burst's identifier does): a talker
  packet with that ID (the same key as the table row and the map point),
  label = flight or registration, the burst's last 0.24 s (kraken_doa
  keeps the stream ~1.5 s, and a long 600 bit/s T burst is decoded ~6 s
  after its unique word), `freq_hz` / `bw_hz` = the burst's carrier and
  width, avg 10 s, kept 30 min.
- A from-aircraft C channel (VFO above 1600 MHz): the call's AES (from the
  sub-band signalling) as a transmission talker.

## Digital voice codec (C channel)

The Aero C channel's codec is AMBE (4.8 kbit/s, 96-bit frames). No codec
code is in the suite; the plugin loads the user's library at run time
(`plugins/lib/aero_ambe.*`): libaeroambe's mbelib
(github.com/jontio/libaeroambe, directory `mbelib-master`: mbelib 1.3 plus
`mbe_processAmbe4800x3600Framef`; MIT / ISC). It needs no Qt - only that
directory, built with CMake:

```
git clone https://github.com/jontio/libaeroambe
cmake -S libaeroambe/mbelib-master -B build-aeroambe -DCMAKE_BUILD_TYPE=Release
cmake --build build-aeroambe
# either install it as the system's mbelib (a superset of mbelib 1.3 - DMR /
# D-STAR / NXDN keep working):  sudo cmake --install build-aeroambe && sudo ldconfig
# or point the receiver at it:  export KRAKEN_AERO_AMBE=$PWD/build-aeroambe/libmbe.so.1
```

Lookup: `$KRAKEN_AERO_AMBE` (a path; `none` / `off` disables), then
`$KRAKEN_MBELIB`, then the system's libmbe.so.1 - the first with the Aero
function. The Voice codec fact says which ("AMBE (Aero 4800): <file>
(mbelib 1.3.0, with the 4800x3600 codec)") or "not installed (libmbe.so.1
lacks the Aero AMBE 4800x3600 functions - see README...)". Windows: the
same CMake build in MSYS2 makes libmbe.dll (next to kraken_doa, or
`KRAKEN_AERO_AMBE`). The 96 received bits go to mbelib's code-vector
layout through libaeroambe's de-interleave table (MIT, notice in
`plugins/lib/aero_ambe.cpp`).

The older 21 kbit/s C channel (9.6 kbit/s LPC voice, Aero-H) is not
decoded: no public decoder of its codec is known.

## Implementation

- `decoder.cpp`: the plugin - the VFO's rate to 24 kHz for the
  single-channel modes, or the All channels mode; `aero_channel.hpp`: one
  channel's decoder (acquisition, demodulator, P / C channel, bursts, SUs)
  - the single-channel modes, and every channel of the All channels mode.
- `aero_bank.*`: the All channels mode's overlap-save FFT filter bank
  (N ~ rate / 375 Hz points every N/2 samples; a channel = 64 bins, a
  flat-topped weighting, a 64-point inverse FFT, the middle half kept, the
  block's phase; 24 kHz out at 2.4 / 1.2 MHz, 600 / 300 kHz, other rates
  through a resampler per channel). A tone through it: the right
  frequency, level 1.000, phase continuous across blocks (worst step error
  0.0004 rad). `aero_all.*`: the survey, trials, channel list, merged
  table, audio mixer.
- `aero_acq.*`: rate and carrier from the squared signal's two spectral
  lines (2 fc +- R/2; A-BPSK and A-QPSK alike), FFT 16384, a search after
  every half-overlapping FFT on the last 1 - 3 summed; a wide search at
  24 kHz behind a +-7.5 kHz low-pass (4.8 / 8.4 / 10.5) and a narrow one
  at 12 kHz behind +-4 kHz (0.6 / 1.2 / 2.4); scored by the two lines'
  mean over the median (floored at 2 % of the mean), threshold by the
  number summed: 16.5 / 13.5 / 12.5 dB (noise: at most 13.5 / 10.4 / 9.9
  dB in 70 / 70 / 1435 searches). The lowest rate at one carrier wins
  (harmonics); the wide rates only after the narrow search's first result
  unless 4 dB over the threshold (A-BPSK gives 4800 bit/s lines up to 16
  dB). A demodulator stuck on a strong P carrier (quality < 0.93 after 1
  s, no SU) starts again half a bit later; a C channel without a clean
  frame for 1.5 s, up to three times.
- `aero_demod.*`: one coherent demodulator for both: the bit-rate samples
  on a turning axis (j^n for A-BPSK, j^(n mod 2) for A-QPSK), decision-
  directed Costas loop (2nd order, ~5 Hz for A-BPSK, ~12 Hz for 10.5
  kbit/s), Gardner timing (A-QPSK: a branch's samples two bits apart and
  the sample between), cubic interpolation, RRC matched filter, 4x loop
  gains for the first bits; the loop frequency is moved into the mixer.
  A-BPSK: differential detection after the coherent one (a "1" keeps the
  sign) - the UW fixes the polarity.
- `aero_pchan.*`: UW search (hard: <= 2 / 6 errors; or the soft UW
  correlation averaged over the frame period - weak signals), the UW
  fixes A-QPSK's branch polarities and pairing, then a flywheel (one frame
  later, +-1 bit, more errors allowed; 3 missed = lost); de-interleaving,
  a streaming soft Viterbi (64-bit trace back) over the continuous code,
  descrambling per frame, the SUs' CRC.
- `aero_cchan.*`: the C channel the same way (I / Q unique words, the 2/3
  code depunctured, 2730 bits, the 26 reference bits counted as a check).
- `aero_burst.*`: bursts: a 512-point FFT (every 256 samples) and a
  4096-point one (the slow rates' long preambles) find preamble tones
  against each bin's own noise (a slow mean outside the bursts); up to
  four bursts on different frequencies are followed at once, each while
  its band stays above its noise from before it; then every rate, four
  starting timing phases each, the best UWs decoded until a CRC passes (R:
  one 19-octet SU; T: burst identifier, then SUs until one fails; 10.5
  kbit/s T: the block size 2 + 3n whose CRCs check). The same burst
  decoded twice (same UW time and CRC) is reported once.
- `aero_su.*`: SU decoding by type, user data reassembly (ISU / SSU
  sequence numbers, per Q / reference; per AES on T bursts; R channel SU
  sets by their sequence indicator), ACARS (`plugins/lib/acars_msg.hpp`),
  the aircraft table; `adsc.*` ADS-C.
- `tools/aero_gen.py`: test transmissions built from the standard (all
  of the above, synthetic content); `tools/aero_band.py`: a 2.4 MHz band
  of them (several P channels, a C channel opened by an assignment,
  carriers of other systems; 5 antennas, cu8); `tools/testsignals.json`:
  the debug playback's entries.

## Sources / specifications

- ICAO Annex 10 Volume III, Part I, chapter 4 (AMSS), its appendices 1 - 3
  (filter masks, signal unit formats, field coding, message type list), in
  the consolidated edition with amendment 77 (2002) - the edition before
  the chapter was cut down (2007): channel formats, modulation, unique
  words, scrambler, code, interleaver, burst formats, SU formats.
- ARINC 618 block format and the ACARS BCS: as `plugins/acars` (measured).
- ADS-C (ARINC 745) group layouts and scaling and the ARINC 622 framing
  and CRC: as documented by libacars (MIT, github.com/szpajder/libacars,
  `adsc.c`, `arinc.c`, `crc.c`; read as a description, not copied).
- JAERO (MIT, github.com/jontio/JAERO, `aerol.cpp`): consulted to confirm
  the C channel's field layout (offsets of the primary / sub-band fields,
  bit packing of a voice frame) and that ACARS sits in the user data as
  FF FF SOH ...; the implementation is written from the standard.
- libaeroambe (MIT, github.com/jontio/libaeroambe): the voice frame's
  de-interleave table (with its notice) and the codec library users build.
- Measured on the recordings below: the unique word's polarity against the
  data (A-QPSK: as written), the CRC's bit order, the Gardner sign, the
  ACARS framing in user data, the BCS.

## Test results (Linux x86-64, offline test mode, 2026-11-22)

Real recordings (local IQ library; none is in the repo):

| Recording | Result | CPU |
|---|---|---|
| sigidwiki "Inmarsat Aero 10500 Bd OQPSK" (IQ, 43.6 s) | P 10500 found at -10.7 Hz, 84 frames, 2158 / 2184 SUs pass the CRC (the 26 lost: the first frame, while the loops converged), 9 ACARS messages (BCS good), log-on interrogations / acknowledgements, T channel assignment, RQA / ACKs | 1.1 % |
| sigidwiki "600bps Aero" (receiver audio, MP3, 120 s; turned into IQ) | P 600 found, 58 frames, 348 / 348 SUs, 3 ACARS | 1.1 % |
| sigidwiki "10.5k-inmarsat" (receiver audio, MP3, 89.5 s) | P 10500, 176 frames, 4550 / 4602 SUs (4524 / 4576 before the faster acquisition of 2026-11-23: it locks a frame sooner), 62 user data messages (41 ACARS, BCS good; 21 other user data) | 1.5 % |
| sigidwiki "Inmarsat AERO" (receiver audio, MP3, 24 s) | P 10500, 44 frames, 1144 / 1170 SUs, 3 ACARS | 1.3 % |
| JAERO sample "10.5k" (receiver audio, 240.8 s) | P 10500, 12454 / 12480 SUs (12428 / 12454 before 2026-11-23), 193 user data messages (143 ACARS, BCS good) | 1.2 % |
| JAERO sample "dish 4 x 600 aligned" (receiver audio, 488 s, four 600 bit/s channels side by side) | the strongest channel: 1458 / 1458 SUs, 4 user data messages (3 ACARS) | 0.8 % |
| JAERO sample "1200 C-band burst" (receiver audio, 14.8 s, C-band 1200 bit/s bursts at ~2.24 kHz audio) | 6 bursts of 6 (two aircraft, R channel SUs, CRC good), the same however the audio is mixed down (by 0, 1000, 2000, 2235, 3000 or 4500 Hz) and fed straight in as IQ (the spectrum mirrored, each burst at +f and -f: 6, each once) | 0.5 - 1 % |
| 300 s of noise; the Inmarsat-C TDM recording | no frames, no events (Auto and bursts) | Auto 0.6 %; bursts 0.3 - 0.5 % (the Inmarsat-C carrier cost 2 - 4.7 % before: it held a "burst" open for 13 s at a time, each decoded at every rate) |

Generated (`tools/aero_gen.py`, AWGN; Eb/N0 per channel bit; SUs passing
their CRC, after lock):

| Signal | 0 dB | 1 dB | 2 dB | 3 dB |
|---|---|---|---|---|
| P 10500 | 99.2 % | 99.9 % | 100 % | 100 % |
| P 1200 | 48 % | 91 % | 98.8 % | 100 % |
| P 600 | 55 % | 93 % | 99.6 % | 100 % |
| C 8400 (frames with clean sub-band / reference bits) | 95 % | 98 % | 100 % | 100 % |

The standard asks for a BER of 1e-5 after decoding at 5.4 dB (A-QPSK) /
7.3 dB (A-BPSK) per channel bit (table 4-8); a 96-bit SU then fails about
once in 1000. Measured: A-QPSK below 1 % at 0 dB (coherent BPSK with this
code: BER ~1e-4 at 3 dB per information bit - ~0.3 dB implementation
loss), A-BPSK at 2 dB (its differential detection costs ~2 dB).
Acquisition locks down to 0 dB (10.5 kbit/s) and 1 dB (600 / 1200 bit/s).
Bursts (R channel log-ons, 20 per point): 600 / 1200 bit/s 15 / 20 at 2
dB, 20 / 20 from 4 dB; 2400 bit/s 19 / 20 at 4 dB, 20 / 20 at 8 dB; 10.5
kbit/s 13 / 20 at 2 dB, 18 / 20 at 4 - 6 dB, 20 / 20 at 8 dB.

Bursts at the same moment on different frequencies (R log-ons, ten per
aircraft, starting 20 ms apart; each file with its own noise at 12 dB,
so ~5 dB less in the sum): 1200 bit/s, two aircraft 2.5 kHz apart 20 /
20, three (-2200 / +300 / +2700 Hz) 30 / 30; 600 bit/s, 1.8 kHz apart 20
/ 20 (one burst at a time before: 9 / 20, 9 / 30, 3 / 20). Closer than
~1.4 kHz the second isn't separated (800 Hz apart: 8 / 20). A burst
mirrored in frequency (I / Q swapped) still decodes: A-BPSK's +-90 deg
steps change sign, which the unique word's polarity takes care of.

Carrier offset: P 600 found and decoded from -2800 to +2800 Hz, 1200
from -2600 to +2600 Hz, 10500 from -2500 to +2500 Hz (not at +-3000);
lock after 0.7 - 1.4 s (the acquisition's FFTs; 1.4 - 2.7 s before
2026-11-23) plus one frame. The faster acquisition (half-overlapping FFTs,
a search after each) re-measured: carrier ranges unchanged; sensitivity
within a frame of before (P 600 at 0 dB 74 / 156 SUs, was 67 / 144; 1 dB
142 / 156 both; P 1200 at 1 dB 310 / 336, was 307 / 336; 2 dB 334 / 336,
was 338 / 342; P 10500 unchanged); every row of the table above the same
or one frame more (the lock a frame sooner); noise: no frames, no events.

Multi-aircraft scene (`plugins/sdk/tools/uca_scene.py`, 5-element UCA of
6 cm radius at 1645 MHz, three aircraft at 40 / 165 / 290 deg, R and T
bursts with ACARS and ADS-C): every burst decoded at 1200 and 10500
bit/s, 4 of 5 at 600 (there the long T bursts overlap, 600 - 800 Hz
apart), talkers 4CA2B1 /
C0FFEE / A1B2C3 at 40.0 / 165.0 / 290.0 deg (10500: 39.9 deg), two
aircraft on the map. The four debug-playback entries generate and decode
offline (`make_testsignal.py`, then `decoder --file ... --channels 5 --uca
0.06`).

Debug playback through kraken_doa (a test instance, 2026-11-22; each
entry played 2.2 - 2.7 loops, 6 cm array):

| Entry | Signal units (live) | DoA / talkers | "Receiving" / "Signal lost" | CPU |
|---|---|---|---|---|
| aero-p10500-ges | 2366 / 2444 (the first frame, and one frame at each loop seam) | VFO 75.0 deg steady | 1 / 0 | 1.8 % |
| aero-p600-ges | 210 / 210 after lock | VFO 220.0 deg steady | 1 / 0 | 1.0 % |
| aero-bursts-four-aircraft | every burst decoded; 14 of the 15 heard gave a talker bearing (1200 bit/s: 12 of 13 - 4CA2B1 4 of 5 -, the long 600 bit/s T burst: 2 of 2) | 4CA2B1 40.0, C0FFEE 165.0, A1B2C3 290.0, 7C0123 330.0 deg | 7 / 6 (between bursts) | 1.5 % |
| aero-c8400-voice | 186 / 192 (a frame at each loop seam) | VFO 130.0 deg | 1 / 0 | 1.7 % without the codec, 3.7 % with it |

Every `expect` row was met. The C channel listened to (Demod Digital,
speaker on): with the codec, the voice state "voice · AES 4CA2B1" and
502 of 551 Opus packets carrying sound; with `KRAKEN_AERO_AMBE=none`, the
fact "disabled (KRAKEN_AERO_AMBE=none)", the voice state "AMBE codec not
installed" and only silence packets. Two fixes came from these runs: the
plugin no longer feeds the receiver's AFC (with it, aero-p10500-ges lost
four more frames to the frequency steps: 2262 / 2444), and a P frame's
span is reported the moment the frame is in (at 600 bit/s the released
bits came 2 s late, past the receiver's 1.5 s stream, and the VFO's DoA
read 127 - 311 deg instead of 220).

### Off air (Raspberry Pi 5, KrakenSDR CH0, patch antenna + LNA, 2026-11-22)

Inmarsat-4 F1 (143.5 E), received with an L-band patch antenna and an LNA
on CH0's bias tee, gain 49.6 dB (by measurement: the 10.5 kbit/s channel
passed 92 % of its SUs at 49.6 dB, 75 / 66 / 74 % at 44.5 / 40.2 / 33.8
dB; no clipping, -34 dBFS rms). The receiver hears the carriers 1.49 kHz
low (-0.96 ppm); a carrier drifted +40 Hz in an hour and +-2 Hz within 5 minutes - tracked without the AFC.

| P channel | Live (kraken_doa on the Pi) | Offline, a capture of it (Pi = x86) | Lock | CPU (Pi 5, live) |
|---|---|---|---|---|
| 600 bit/s (16 seen, 1545.010 - 1545.210 MHz) | 476 / 480 SUs in 170 s; 148 / 150 | each channel of a 60 s wide capture: 156 - 168, all passed; 70 s narrow: 198 / 198 | 4 - 10 s | 1.2 - 1.3 % |
| 1200 bit/s (1545.070 MHz) | 1278 / 1290; with the VFO 2 kHz off: 594 / 594, 462 / 462 | 342 / 342 (wide); 558 / 558 (narrow, 2 kHz off) | 3 - 11 s | 1.3 - 1.6 % |
| 10.5 kbit/s (1546.055 / 1546.070 MHz) | 7485 / 8736, 4805 / 5512, 12907 / 15314, 10300 / 13806 (75 - 87 %); the other channel 12516 / 13936 (90 %) | 2927 / 3042 (96 %, wide), 4456 / 4810 (93 %) | 0.3 - 1.6 s | 1.9 - 2.0 % |

(The Lock column is of 2026-11-22; since 2026-11-23 the 600 / 1200 bit/s
channels lock in 1.5 - 1.7 s and the 10.5 kbit/s ones in ~1 s - Time to
recognition, below.) No samples dropped. Offline on the Pi 5 every count equals x86 (the
acceptance set and 24 off-air cuts); 0.2 - 1.2 % of real time, 8 - 34
Mcycles per second of signal (600 bit/s 16.7, 10.5 kbit/s 27.7).

What the real signal showed (fixed): the satellite identification SUs
are a table of every satellite of the system (IDs 0 - 7, one SU each),
not this channel's - now shown whole, and the satellite named by the
received P channel's frequency (ID 2, 142.5 E - and ID 5, 144.0 E for
GES 0x50's channel); a carrier of another system with one strong
unmodulated line (1545.476 MHz) made the acquisition pair that line with
noise and sync falsely - the two lines must now be alike (this also made
the 600 / 1200 bit/s channels lock a frame sooner); a VFO moved under a
running decoder kept its state and locked 900 Hz beside a 1200 bit/s
carrier for a minute (408 / 960 SUs) - a move now restarts the search
(since 2026-11-23 except a move of <= 2 kHz while synced: the carrier is
known, the demodulator follows it).
The 10.5 kbit/s channels are weak here (Eb/N0 near 0 dB, demodulator
quality 0.85, failures scattered) and their level changes by the
minute: a fixed capture of one channel passed 86, 74, 54, 71, 63, 66, 39,
43, 56 % of its SUs in nine 100 s runs within 25 minutes. The live path
loses nothing: frame by frame against an offline decode of a capture of
the same seconds (taken alongside from heimdall's stream), live 3598 /
offline 3601, 3185 / 3187, 2119 / 2114, 1973 / 1963, 2109 / 2077 SUs at
VFO widths 25, 20, 30, 15, 48 kHz (190 frames each; 15 kHz with the VFO
on the carrier: 54.2 / 55.5 % over the run); the plugin's stream kept
time with the wall clock to 4e-5, nothing dropped. The width doesn't
matter from 15 to 48 kHz (a VFO emulated on the 2.4 MS/s capture: 2906 -
2926 of 3042 SUs at 15 - 48 kHz). Loop bandwidths and roll-off changed
nothing by more than 2 points.

C channel calls: none in the first 31 minutes; in a 70-minute watch (six
P channels at once, independent mode) two C channel assignments came, on
the two 10.5 kbit/s channels (GES 0x82 and 0x85, to-aircraft 1542.980
and 1542.990 MHz - outside the 1545 - 1546 MHz P channel band), each
followed within a second (VFO in c8400 mode, tuner retuned, a capture) -
see Voice below. Not seen: the UTC broadcast (type 0x09) on any of these
channels; ADS-C reports (on P channels only contract requests travel - the
reports go up on R / T channels); aircraft bursts (8 minutes on the R
channels the GESs advertise, 1646.5075 - 1646.5475 MHz: nothing - the
floor there is 3.7 dB under 1545 MHz, the patch / LNA hardly pass 1.6
GHz). Unknown SU types counted, not decoded: 0x18 / 0x19 / 0x1A / 0x2B
(600 bit/s channels), 0x0A / 0x26 (half of every 10.5 kbit/s channel).

Voice: generated C channel frames (random voice bits - no AMBE encoder
exists to make speech) go through libaeroambe's codec built in job
scratch: 3.4 % CPU with the codec (2.6 % of real time on the Pi 5,
where the same codec was built); "not installed" without it; the codec's
own error count for random bits: 4.3 corrections per frame.

**Off air, two real calls (to-aircraft C channels, 2026-11-22)** -
checked against the real signal for the first time:
- framing: unique words found (0 - 10 errors of 104 while the carrier was
  good; 0 - 16 in the faded parts), reference bits 0 - 3 / 26 wrong, 15 + 34 clean frames of 20 +
  41;
- sub-band signal units: 23 / 42 and 65 / 102 passed their CRC (the rest
  from frames where the carrier had faded), with call progress units
  decoded;
- the voice frames' position, bit order, polarity and de-interleave, with
  the codec's FEC as the oracle: on the voice frames of frames with <= 1
  reference bit error mbelib corrected 0.55 - 0.60 Golay bits per 20 ms
  frame and 82 - 84 % of the frames needed none; the same bits arranged
  wrongly (reversed, inverted, 1 or 8 bits late, the de-interleave table
  shuffled) give 0 % of frames without corrections and 2.4 - 4.9 per
  frame, random bits 4.3. The arrangement is right; nothing was changed;
- speech: not confirmed. In one call 567 of the 675 clean voice frames
  are one and the same frame, which the codec turns into silence; in the
  other the clean frames decode to sound with little voicing (voiced 6 %
  of active frames, no voiced run longer than one frame; random bits:
  20 %, runs of up to 4), and the carrier was good for only ~10 s. No
  audio was kept or listened to; statistics only.
Both calls are private captures in the local IQ library.

### Time to recognition (2026-11-23)

"Recognised" = the Channel fact names the channel (the lock), "first
frame" = the first checked frame (CRC). Offline: the decoder started every
0.5 s over 20 s of each capture (40 starts); before = 93be402.

| Signal | Recognised, before -> now (median / max) | First frame, before -> now (median / 90 % / max) |
|---|---|---|
| P 600, off air | 2.73 / 2.73 -> 1.36 / 1.36 s | 3.46 / 6.82 / 16.96 -> 3.21 / 3.96 / 3.96 s (bounded by the 2 s frame) |
| P 1200, off air | 2.73 / 2.73 -> 1.36 / 1.36 s | 2.73 / 6.82 / 8.19 -> 1.51 / 1.51 / 2.01 s |
| P 10500, off air (weak, ~4 dB) | 1.36 / 1.36 -> 0.68 / 1.02 s | 2.34 / 6.84 / 26.34 -> 1.84 / 5.84 / 10.34 s |
| P 10500, JAERO sample (strong) | 1.36 -> 0.68 s | 1.36 / 1.69 / 2.19 -> 0.69 / 1.24 / 1.69 s |
| C 8400, call 2 (20 starts every 1 s) | 1.02 / 1.36 s (unchanged) | 2.16 / 5.81 / 16.66 (2 starts never) -> 2.34 / 4.48 / 8.66 s (all 20) |

Through the receiver (Pi 5, kraken_doa, VFO 15 kHz, independent mode;
before = the 93be402 plugin swapped in): recognised / first frame, medians
of 2 - 4 trials each way:

| Way | P 600 before -> now | P 1200 before -> now | P 10500 before -> now |
|---|---|---|---|
| decoder switched on | 7.0 / 9.6 -> 1.5 / 4.0 s | 6.9 / 8.0 -> 1.65 / 2.2 s | 1.6 / 1.6 -> 0.96 / 1.0 s |
| VFO moved to the channel | 3.0 / 4.5 -> 1.6 / 3.8 s | 7.0 / 8.0 -> 1.5 / 3.2 s | 1.5 / 3.5 -> 0.96 / 1.9 s |
| receiver retuned (+-50 kHz, the VFO kept on the channel) | (not run) -> 1.6 / 4.1 s | (not run) -> 1.65 / 2.0 s | (not run) -> 1.0 / 2.0 s |

Coherent mode the same (now: 1.5 - 1.7 s / 2.5 - 4.0 s for 600 and 1200,
0.8 - 1.1 / 1.0 - 1.9 s for 10500), except while a calibration runs (the
noise source replaces the antenna: a trial during one took 12 s).

What was done: the acquisition searches after every half-overlapping FFT
(0.68 s at 24 kHz, 1.37 s at 12 kHz) on the last 1 - 3 summed, with
thresholds by count from noise; 4800 / 8400 bit/s hits wait for the
narrow search (a strong 1200 bit/s channel scored 20.4 dB as 4800 bit/s
off air and was locked falsely for 5 s - the live 1200 bit/s times of up
to 6.8 s before); a demodulator stuck on a strong carrier starts again
half a bit later (P) or after 1.5 s without a clean frame (C). The
receiver adds nothing measurable: offline and live times agree. A VFO move
of up to 2 kHz while synced keeps the lock (the demodulator follows the
carrier); larger moves restart - the new carrier isn't known, and the
acquisition takes the same time as a fresh start.

The weak 10.5 kbit/s channels' counts depend on the samples' timing phase
(the same capture from a start half a 24 kHz sample later: 2119 instead
of 2694 of ~2900 SUs) - the spread between any two decodes of them.

### All channels in the VFO (2026-11-23)

Offline (x86), the 60 s 2.4 MS/s off-air capture (`aero_wide_1545.300MHz_
2.4Msps_60s`, Inmarsat-4 F1): 22 channels found - 16 x 600, 1 x 1200 and
5 x 10.5 kbit/s (three weak 10.5 kbit/s channels at 1545.315, 1545.3275
and 1545.3675 MHz, 2 - 3 dB over the floor, besides the two at 1546.055 /
.070 that the first survey of this capture had found; they pass 85 - 90
% of their CRCs) - and nothing else: the carriers of other systems
between them, the dongle's DC spike and the band edges were tried and
dropped. Every channel against a single-channel decode of it from the
same file and the same start: 15 of the 17 narrow ones equal SU for SU,
two with one frame more in All channels (168 / 156); the five 10.5 kbit/s
-9 .. +5 % (sum -1.6 %), within the timing-phase spread above. The 600 /
1200 bit/s channels' trials open at 0.25 - 0.5 s, the 10.5 kbit/s ones at
0.5 - 1.75 s; 20 of 22 pass their first CRC by 4.0 s, all by 7 s.
15 % of real time (file reading included).

| Input | Channels | Events | CPU (x86) |
|---|---|---|---|
| the off-air capture (60 s) | 22 (above) | 249 | 15 % |
| the capture with the two real calls put in (+450 kHz from 6 s, -430 kHz from 10 s, each's noise floor matched; 40 s) | 22 P + both calls: trials opened 0.75 / 1.0 s after each call began, first clean frames 3.0 / 4.2 s; the P channels as without them (17 equal or one frame more); voice state "2 calls, mixed", 17.7 s of mixed audio for ~17.5 s of calls | - | 16 % |
| generated band (`tools/aero_band.py`, 5 antennas, 12 s) | all 7: P 600 x 2 (5 kHz apart), 1200, 2400, 4800, 10500, and the C channel opened by the assignment at once (validated 1.5 s after it began); the unmodulated carrier and the other system's carrier never | 4 (log-on, ACARS, assignment, call progress) | 7 % |
| 60 s of noise at 2.4 MS/s | 0, no trial | 0 | 2.3 % |
| Inmarsat-C NCS (60 s) / LES TDM (20 s) captures at 2.4 MS/s | 0 (5 / 45 trials, all dropped) | 0 | 2.6 / 11 % |

The composite costs the calls ~0.5 dB (8-bit requantisation, the floor
match), so they are compared with single-channel decodes of the same
composite: call 1 7 - 10 clean frames (single 9), call 2 31 (single 33).

Pi 5 (live, independent mode, the band at 1545.6 MHz): 22 channels at
2.4 MHz, 13 at 1.2 MHz (8 x 600, 5 x 10.5k), 13 at 300 kHz (12 x 600, 1200
- centred at 1545.15 MHz); no samples dropped. Side by side with four
single-channel VFOs on the same channels: 432 / 432 vs 432, 882 vs 888,
7670 / 7670 vs 7696 / 7774, 432 vs 444 SUs over 150 s (the trials open a
few seconds after the singles). CPU of the All channels plugin: 32 % of a
core at 2.4 MHz (22 channels), 20 % at 1.2 MHz (13), 14 % at 300 kHz (13);
kraken_doa 75 - 77 % with it and 75 % with one 15 kHz VFO instead. Offline
on the Pi 5 (perf, cycles): the bank alone 10.0 % of a 2.4 GHz core at
2.4 MHz (+0.07 % per channel; an empty band a quarter of it after 2 s),
4.9 % at 1.2 MHz, 1.3 % at 300 kHz; a channel's decoder 0.8 % (600 / 1200
bit/s), 1.3 % (10.5 kbit/s), a searching trial 0.7 %.

## Limitations

- One continuous channel per VFO (the strongest) - except in All
  channels in the VFO, which doesn't follow a C channel outside the VFO
  (that would need a second tuner, or the band given up for the call)
  and doesn't do bursts. Bursts: up to four at
  once on different frequencies; one that starts within ~1.4 kHz of a
  burst being followed (within its whole band for 2400 / 10500 bit/s) is
  missed, and so is a fifth at once.
- Not decoded: CPDLC contents (ASN.1 PER), ADS-C contract requests'
  contents, Data-3 beyond the packet header, the 21 kbit/s C channel
  (and its 9.6 kbit/s LPC voice), the 5.25 / 6.0 / 10.5 kbit/s C
  channels, R / T bursts at 2400 bit/s and 4800 bit/s (implemented as the
  standard says, not generated or tested), several system table
  broadcasts' contents (index, beam ID, beam support: counted). Message
  type 0x26 (reserved in the standard, broadcast on the real 10.5 kbit/s
  recording) is counted only.
- Satellite ID / channel fields of the system tables are laid out per
  appendix 2 but were not on the recordings - unverified.
