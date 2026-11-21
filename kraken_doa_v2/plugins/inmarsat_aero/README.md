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
- A VFO of 15 - 25 kHz on the channel frequency (min_vfo_rate 15 kHz). The
  carrier may be off by up to +-2.5 kHz (an RTL-SDR's 1 ppm is 1.5 kHz at
  1.5 GHz): the plugin finds it, tracks it, reports it (Carrier offset fact
  and to the receiver's AFC).
- Aircraft bursts (Channel "R / T bursts"): a VFO on the aircraft's
  transmit channel in 1626.5 - 1660.5 MHz; only aircraft in line of sight
  (near an airport) are strong enough. The DoA of a P channel or a
  to-aircraft C channel is the satellite's.

## Options

- **Channel**: Auto (default: the acquisition finds a P or C channel and
  its rate), P 600 / 1200 / 2400 / 4800 / 10500 bit/s or C channel 8400
  (search that rate only), R / T bursts from aircraft (every burst rate).

## What is decoded

Facts: Channel (rate, modulation; "syncing" / "searching (why)"), Carrier
offset, Signal units (passed / total, fill-in), SU types (counts per
message type), GES (ID), GES time (UTC broadcast), Satellite (ID,
longitude, P channel), GES 0x.. channels (its P / R channel advice), User
data (messages, ACARS, bad BCS), Aircraft heard, Last ACARS, Voice codec,
C channel frames.

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
  label = flight or registration, `start_s` at the unique word, at most
  0.24 s, `freq_hz` / `bw_hz` = the burst's carrier and width, avg 10 s,
  kept 30 min.
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

- `aero_acq.*`: rate and carrier from the squared signal's two spectral
  lines (2 fc +- R/2; A-BPSK and A-QPSK alike), FFT 16384, two averaged;
  a wide search at 24 kHz behind a +-7.5 kHz low-pass (4.8 / 8.4 / 10.5)
  and a narrow one at 12 kHz behind +-4 kHz (0.6 / 1.2 / 2.4); scored by
  the two lines' mean over the median (floored at 2 % of the mean),
  threshold 13.5 dB (noise: at most 10.3 dB in 220 searches of every
  rate). The lowest rate at one carrier wins (harmonics).
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
  4096-point one (the slow rates' long preambles) find the preamble's
  tone; the burst lasts while its band stays above the noise (or another
  preamble starts); then every rate, four starting timing phases each, the
  best UWs decoded until a CRC passes (R: one 19-octet SU; T: burst
  identifier, then SUs until one fails; 10.5 kbit/s T: the block size
  2 + 3n whose CRCs check).
- `aero_su.*`: SU decoding by type, user data reassembly (ISU / SSU
  sequence numbers, per Q / reference; per AES on T bursts; R channel SU
  sets by their sequence indicator), ACARS (`plugins/lib/acars_msg.hpp`),
  the aircraft table; `adsc.*` ADS-C.
- `tools/aero_gen.py`: test transmissions built from the standard (all
  of the above, synthetic content); `tools/testsignals.json`: the debug
  playback's entries.

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
| sigidwiki "10.5k-inmarsat" (receiver audio, MP3, 89.5 s) | P 10500, 175 frames, 4524 / 4576 SUs, 62 user data messages (41 ACARS, BCS good; 21 other user data) | 1.5 % |
| sigidwiki "Inmarsat AERO" (receiver audio, MP3, 24 s) | P 10500, 44 frames, 1144 / 1170 SUs, 3 ACARS | 1.3 % |
| 300 s of noise; the Inmarsat-C TDM recording | no frames, no events (Auto and bursts) | 0.6 % / 1.2 % |

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
Bursts (R channel log-ons, 20 per point): 600 / 1200 bit/s 13 / 20 at 2
dB, 20 / 20 from 4 dB; 10.5 kbit/s 13 / 20 at 2 dB, 18 / 20 at 4 - 6 dB,
20 / 20 at 8 dB.

Carrier offset: P 600 found and decoded from -2800 to +2800 Hz, 1200
from -2600 to +2600 Hz, 10500 from -2500 to +2500 Hz (not at +-3000);
lock after 1.4 - 2.7 s (the acquisition's FFTs) plus one frame.

Multi-aircraft scene (`plugins/sdk/tools/uca_scene.py`, 5-element UCA of
6 cm radius at 1645 MHz, three aircraft at 40 / 165 / 290 deg, R and T
bursts with ACARS and ADS-C): every burst decoded at 600, 1200 and 10500
bit/s (600 when the bursts don't overlap in time), talkers 4CA2B1 /
C0FFEE / A1B2C3 at 40.0 / 165.0 / 290.0 deg (10500: 39.9 deg), two
aircraft on the map. The four debug-playback entries generate and decode
offline (`make_testsignal.py`, then `decoder --file ... --channels 5 --uca
0.06`).

Voice: generated C channel frames (random voice bits - no AMBE encoder
exists to make speech) go through libaeroambe's codec built in job
scratch: 3.4 % CPU with the codec; "not installed" without it. **Not
verified on a real C channel: no recording was found** - the framing and
signalling follow the standard and JAERO's layout, intelligible speech is
unconfirmed.

## Limitations

- One continuous channel per VFO (the strongest); one burst at a time
  (bursts on two frequencies at once: the first is followed).
- The receiver's AFC (freq_error, every 2 s while synced) moves the input
  in steps; the plugin follows `rf_hz()` - not tried live.
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
