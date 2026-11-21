# Inmarsat Classic Aero plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`).

**Inmarsat Classic Aero (`plugins/inmarsat_aero/`, README.md there; added
2026-11-22):**
- Written from ICAO Annex 10 Vol III Part I chapter 4 in the consolidated
  edition with amendment 77 (2002, scanned PDF; the 2007 edition cut the
  chapter down to a summary): channel formats 4.4, modulation 4.3,
  appendix 2 (SU formats), appendix 3 (field coding, message type list).
  libacars (MIT) as the description of ARINC 622 / 745, JAERO (MIT) to
  confirm the C channel layout, libaeroambe (MIT) for the voice frame's
  de-interleave table. No GPL decoder was opened.
- Input 24 kHz (`sample_rate`), `min_vfo_rate` 15000, category Aviation,
  `map`, `voice`, `talkers`; option `mode` (auto / p600 .. p10500 / c8400
  / burst). One plugin for every channel: the slow modes decimate inside
  (by 5 to 4.8 kHz for 0.6 / 1.2, by 2 for 2.4 kbit/s); CPU stays 1 - 1.5
  % of real time on x86 (3.4 % with the voice codec running).
- Acquisition from the squared signal: A-BPSK (+-90 deg per bit) and
  A-QPSK (offset QPSK) both give lines at 2 fc +- R/2, data-independent -
  rate and carrier in one FFT search, no timing or phase needed. Two
  searches (24 kHz behind +-7.5 kHz for the fast rates, 12 kHz behind +-4
  kHz for the slow), 2 x 16384-point FFTs averaged, the two lines' mean
  over the median; the median is floored at 2 % of the mean - MP3 receiver
  audio has no noise outside the signal and every ratio went to 150 dB.
  Threshold 13.5 dB from 300 s of noise (max 10.3 dB). A lower rate's
  harmonics match a higher rate's pair at the same carrier (a 600 bit/s
  signal also gives "1200" and "2400" lines): the lowest rate at a carrier
  wins. Range: +-2.8 kHz (600), +-2.6 (1200), +-2.5 kHz (10.5k; the
  low-pass and the 24 kHz rate end it).
- Demodulator (`aero_demod.cpp`): both modulations as one bit per bit
  period on a turning axis (j^n A-BPSK, j^(n mod 2) A-QPSK); Costas error
  sign(d) x quadrature, Gardner on the same axis. The Gardner sign was
  found on the real recording: (d[n-2] - d[n]) x mid for A-QPSK, (d[n-1] -
  d[n]) x mid for A-BPSK; the other sign locks OQPSK half a branch symbol
  off, where a 90 deg rotated lock is equally stable - the PLL then hopped
  between the two every few frames (58 changes in 87 frames). With the
  right sign: none, quality 0.998. Timing gain matters: a Python
  prototype with 1.6e-3 per bit had 1.7 % raw BER in the data (the dummy
  bits were clean) and the code's syndrome looked random; 1.25e-4 gave
  zero syndrome errors.
- The 10.5 kbit/s recording: the UW's polarity on each branch equals the
  data's (as written in the standard; earlier frames decoded "inverted"
  only because the prototype mixed branch signs); format ID 0001, frame
  numbers 0 .. 15 with the 1111 marker in frame 0, dummy 0001 repeated.
  The coded stream runs on across frames (no flush); each frame's 4992
  coded bits are a valid code segment and its 2496 decoded bits are its 26
  SUs exactly (scrambler reset at the information field) - JAERO's delay
  line is its own decoder latency. CRC: X.25 over 10 octets, the CRC's low
  octet first (bit 1 of each octet sent first).
- Message type 0x26 is broadcast every frame on the 10.5k recording
  (reserved in the standard; JAERO lists it as "Reserved_26"); 0x0A
  (system table index) too. Both only counted.
- ACARS in user data: FF FF, SOH, mode .. suffix (parity bits kept), BCS =
  CRC-16/KERMIT over mode .. suffix (as VHF ACARS), DEL - checked on all
  real messages. ADS-C groups are whole octets (the earth reference's 38
  bits in 5): reading them bit after bit misread every group after the
  first. The ARINC 622 CRC: CRC-16 0x1021 non-reflected, preset FFFF,
  complement appended high octet first, residue 0x1D0F (libacars' crc.c
  has a reflected-update function above the ARINC one - read the right
  one).
- UW search for weak signals: besides the hard match (<= 2 of 32 / 6 of
  64 errors), the soft UW correlation averaged per position over the
  frame period (0.5 / 0.5) - 1200 bit/s locked from 1 dB instead of 3 dB.
  A soft candidate is then checked with the tracking error limit.
- Restart bug fixed: on lock the demodulator runs over the 3 s of history
  the acquisition saw - the current block was in it and was processed a
  second time, a discontinuity that cost the first lock its frames.
- C channel 8.4 kbit/s (`aero_cchan.cpp`): different UWs on I and Q (52
  bits each); 2730 decoded bits per frame = 26 reference bits, 25 x 96
  voice, 24 x 12 sub-band, 16 dummy; the reference bits count as a check
  (a frame "clean" with a sub-band SU passing its CRC or <= 2 reference
  errors). Codec: `plugins/lib/aero_ambe.*` dlopens libaeroambe's mbelib
  (`mbe_processAmbe4800x3600Framef`; Qt-free, its CMake build makes
  libmbe.so.1, a superset of mbelib 1.3); the bit order goes through
  libaeroambe's 96-entry table (MIT notice in the file). No real C channel
  recording was found: the voice path is checked with random frames only.
- Bursts (`aero_burst.cpp`): the unmodulated preamble is a tone: a
  512-point FFT (one hit 18 dB, or two in a row at 14 dB, over the median
  and 10 dB over the bins 3 - 6 away - a modulated spectrum fails that)
  and a 4096-point one for 600 / 1200 bit/s (13 dB, two in a row): the
  short one alone missed 600 bit/s bursts below 8 dB, the long one gets
  them at 4 dB. The burst lasts while its band (+-0.7 or +-11.7 kHz) is
  above the pre-burst noise (smoothed; 8 quiet FFTs end it) or until
  another preamble at another frequency starts. Each rate, four starting
  timing phases; the demodulator keeps its 4x loop gains through the
  preamble (`gear_bits`) - with the default 256 bits they were spent on the
  noise before the burst and the 10.5k bursts didn't decode. A 10.5k T
  burst's block (2 + 3n columns) isn't signalled: every n is tried until
  the burst identifier and all SUs pass.
- Talkers: bursts only (packet per burst, AES = ICAO address, from the UW,
  <= 0.24 s); P / to-aircraft C channels have none (the satellite). A
  from-aircraft C channel (rf > 1600 MHz) gives a transmission talker.
- AFC: freq_error every 2 s while synced; the plugin shifts its mixer by
  each `rf_hz()` change (the host's correction moves the input). Not
  tried live.
- Real recordings used (IQ library): the sigidwiki 10.5 kbit/s IQ and
  three sigidwiki MP3s of receiver audio (catalogue
  `_tools/catalog/inmarsat_aero.json`; turned into IQ by a Hilbert
  transform for the test). Results and the sensitivity table: plugin
  README.
- Not done: CPDLC decoding, the 21 kbit/s C channel (no public 9.6 kbit/s
  LPC decoder), the other C rates, simultaneous bursts, verification of
  satellite / beam table fields (not on the recordings). Windows / Linux
  glibc syntax checks (MinGW, zig) not run - not installed on the job
  machine; the debug playback was run offline only (make_testsignal.py +
  `decoder --file`), not through a TEST kraken_doa.
