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
  libaeroambe's 96-entry table (MIT notice in the file). Checked off air
  on two real calls (below, "Off air"): framing, reference bits, sub-band
  SUs and the voice frames' arrangement (the codec's Golay words come out
  clean); speech itself not confirmed.
- Bursts (`aero_burst.cpp`): the unmodulated preamble is a tone: a
  512-point FFT (one hit 18 dB, or two in a row at 14 dB, over its bin's
  noise as over a median, and 10 dB over the bins 3 - 6 away - a
  modulated spectrum fails that) and a 4096-point one for 600 / 1200
  bit/s (13 dB, two in a row): the short one alone missed 600 bit/s
  bursts below 8 dB, the long one gets them at 4 dB.
  - The noise is per bin (a mean, ~0.2 s / ~0.4 s, each FFT's power
    clipped at 4x the noise so a tone doesn't lift its own bin before
    the second FFT confirms it; updated after the detection, not inside
    a burst's band). It was the median of the whole spectrum: the JAERO
    C-band burst sample is receiver audio with power in 88 of 512 bins,
    its median 92 dB under the noise where the bursts are, so noise peaks
    passed for preambles, held the one burst slot (still "present"
    against that floor) and the real bursts 160 Hz away were taken for
    its tone and never searched - 1 - 3 of the 6 decoded, depending on
    the mix-down frequency (0 / 1000 / 2000 / 2235 / 3000 / 4500 Hz: 3 /
    2 / 1 / 1 / 3 / 3; mirrored IQ: 5). Now 6 at each.
  - Up to four bursts at once (`Slot`), on different frequencies. A
    burst followed covers +-1.4 kHz (its modulated spectrum's peaks and
    its preamble's lines at R / 4, R / 2 would pass for tones; at R / 2
    differential detection even decodes the same bits), its whole wide
    band once a short tone (< 0.1 s as measured: 2400 / 10500 bit/s) is
    over; tones at 2625 / 5250 Hz (10.5k's R / 4, R / 2) from a burst
    started < 0.6 s before and tones 40 dB under a burst's carrier within
    4 kHz are not new bursts; a burst decoded twice (UW time within 20
    ms, same CRC) is reported once.
  - A burst lasts while its band (+-0.7 kHz; +-11.7 kHz too for a short
    tone) is over its noise from before it (the narrow band's: the median
    bin - the tone lifts its own bins a little before the 4096-point
    detector confirms it, ~0.2 s, and with their sum the 600 bit/s bursts
    ended early: 2 / 20 at 2 dB) and within 20 dB of its most (no noise
    at all: a playback file before the player adds it); 8 quiet FFTs end
    it, a new tone on its carrier after 0.15 s without (6 FFTs) starts the
    next burst there. At most 13 s (3 s for a short tone); one held that
    long sets its band's noise to what is there (a continuous carrier - the
    Inmarsat-C recording took 2 - 4.7 % of a core in burst mode, holding a
    burst open 13 s at a time; now 0.3 - 0.5 %), and each rate decodes no
    more than its longest burst (31 SUs) from the start. Each rate, four starting
  timing phases; the demodulator keeps its 4x loop gains through the
  preamble (`gear_bits`) - with the default 256 bits they were spent on the
  noise before the burst and the 10.5k bursts didn't decode. A 10.5k T
  burst's block (2 + 3n columns) isn't signalled: every n is tried until
  the burst identifier and all SUs pass.
- Talkers: bursts only (packet per burst, AES = ICAO address, from the UW,
  the burst's last <= 0.24 s - it was at the UW, and a 6 s 600 bit/s T
  burst's packet was out of kraken_doa's 1.5 s stream by the time it was
  decoded); P / to-aircraft C channels have none (the satellite). A
  from-aircraft C channel (rf > 1600 MHz) gives a transmission talker.
- No AFC (no `freq_error`): kraken_doa moves the input by half of each
  report every 300 ms; measured live on aero-p10500-ges (50 s, 2.5 loops),
  2262 / 2444 SUs with it (four frames lost to the steps) and 2366 / 2444
  without (the 78 lost: the first frame and one frame at each loop seam -
  as offline). The plugin tracks +-2.5 kHz itself; the earlier
  `follow_rf()` (mixer shifted by each `rf_hz()` change) went with it.
- Two passes per P frame (`PChannel::drain`): the released Viterbi bits
  arrive one frame late (TRACE 64 + CHUNK 64 pairs, the next frame's block
  has to be pushed): 2 s at 600 bit/s, beyond kraken_doa's 1.5 s stream
  (`talker_doa.hpp` RING_S), so the Digital squelch's DoA got no samples
  and the VFO's DoA stayed wrong (live: 271 / 127 / 311 deg for a truth of
  220). The early pass traces back from the best state the moment the
  frame is in (`Viterbi::peek`), emits the SUs that pass and reports the
  frame's span (`on_early` -> `valid_span`, clipped to the last 1.2 s);
  the final pass retries the failed SUs on the released bits (the
  look-ahead is worth ~0.5 dB on the last SU: 600 bit/s at 1 dB 89 % with
  the early pass alone, 93 % with both). Live after it: 220.0 deg steady.
  Bursts report their last 1.2 s too.
- Real recordings used (IQ library, catalogue
  `_tools/catalog/inmarsat_aero.json`): the sigidwiki 10.5 kbit/s IQ,
  three sigidwiki MP3s and three of JAERO's published samples (10.5k,
  four 600 bit/s channels side by side, C-band 1200 bit/s bursts), all
  receiver audio turned into IQ by a Hilbert transform. Results and the
  sensitivity table: plugin README.
- No real C channel recording: searched JAERO's repository, releases,
  wiki and issues (#32 and #67 link Google Drive files that are gone),
  the Wayback Machine's copies of jontio.zapto.org (only the three
  samples above), the DecAero and OnAir READMEs (DecAero names an
  "8400bps_ambe_sample" without a link), InmarScope, rtl-sdr.com. Two
  calls were then captured off air (below).
- Debug playback through a TEST kraken_doa (2026-11-22, 2+ loops per
  entry): plugin README, Test results.
- Off air (2026-11-22, Raspberry Pi 5 + KrakenSDR CH0, patch antenna with
  an LNA on the bias tee, Inmarsat-4 F1): numbers in the plugin README,
  Test results, "Off air". Layout facts learned there:
  - Type 0x0B / 0x0C ("satellite identification") is a table, one SU per
    satellite of the system: octet 2 counts down by 4 per entry (0x18 ..
    0x00), octet 3's high nibble is the ID (0 - 7), octet 5 x 1.5 the
    longitude (deg E; 0: 99.0 W, 1: 54.0 W, 2: 142.5 E, 3: 84.0 E, 4:
    145.5 E, 5: 144.0 E, 6: 25.5 E, 7: 97.5 W), octets 6 - 9 two P channel
    codes. The satellite being received is the entry naming the received
    P channel (`SuDecoder::set_channel_hz`, nearest within 2.5 kHz): ID 2
    for GES 0x82 / 0x85's channels, ID 5 for GES 0x50's.
  - Real P channels: 600 bit/s on a 5 kHz raster, all the same level
    (+13 dB in 600 Hz here); the 10.5 kbit/s channels carry mostly types
    0x0A and 0x26 (about half each); the 600 bit/s ones types 0x19 (the
    commonest), 0x0C, 0x05, 0x18, 0x1A, 0x2B, 0x40, 0x41 - 0x18 - 0x1A and
    0x2B are not decoded. No type 0x09 (UTC) seen.
  - C channel calls: none in the first 31 minutes of P channel watching;
    then a 70-minute unattended watch (job scratch watch.py on the Pi:
    independent mode, tuner 0 at 1545.6 MHz, six VFOs on the P channels
    and a spare in c8400 mode listening, reading the decoders' events over
    the WebSocket; on a "C channel assignment" event the spare jumps to the
    to-aircraft frequency - tuner retuned when outside the span - and
    `sigtool.py capture --channel 0` records 160 s at 48 kHz; a 5 s look for
    8.4 kbit/s carriers every 5 minutes, a 1545 - 1556 MHz sweep every 25)
    caught two assignments, both on the 10.5 kbit/s channels (GES 0x82 /
    0x85 -> 1542.980 / 1542.990 MHz), followed within a second, call
    progress SUs on the C channel itself. Offline (Pi = x86): 20 / 41
    frames, 15 / 34 clean, sub-band SUs 23 / 42 and 65 / 102.
  - The voice frame arrangement, by the codec's own FEC: the 96 bits of
    each voice frame of frames with <= 1 reference error into
    `mbe_processAmbe4800x3600Framef` (ctypes, the plugin's W / X table):
    82 - 84 % of frames with no Golay correction (0.55 - 0.60 per frame);
    bits reversed 0 % (4.5 - 4.9), inverted 0 % (2.4 - 3.2: the extended
    Golay code holds the complement of a codeword, C1's PN differs), 1 bit
    late 0 % (4.1 - 4.9), 8 bits late 0 % (3.1 - 3.3), W / X shuffled 0 %
    (4.0 - 4.4), random bits 0 % (4.3). The generator's random voice bits
    stay (no encoder). Speech not confirmed: one call's clean frames are
    84 % one identical frame (decodes to silence); the other's decode with
    6 % voiced frames and no voiced run over one frame (random bits: 20 %).
  - The acquisition's line pair must be balanced (`Acquisition::search`:
    max / min <= 6): a non-Aero carrier with a strong unmodulated part
    (1545.476 MHz) has one x^2 line 20 - 25 dB over the rest.
  - A VFO move (`host.rf_hz()` changing by > 100 Hz) resets the decoder.
  - The 10.5 kbit/s channels are noise-limited with a patch (Eb/N0 ~0 dB)
    and fade by the minute (39 - 96 % of SUs over an hour); pll_bw 0.6 -
    2.5e-3, tim_gain 0.6 - 2.5e-4, roll-off 0.35 - 0.7 changed the
    capture's count by at most 2 points (none better than the defaults).
  - Live vs offline (the coordinator's open question - 75-87 % live
    against 93-96 % from captures): not the live path. Measured frame by
    frame: a test build deployed only on the Pi (commit 5632841, taken out
    again - a plugin writes no files, SDK rule 5) appended the live
    decoder's `AERO_DEBUG=1` frame lines ("frame t=<stream time> ... sus
    n/26") to a file named by AERO_DEBUG_FILE (kraken_doa keeps only a
    plugin's last 30 stderr lines, for its status), `sigtool.py capture`
    alongside records the same seconds from heimdall's 8091, the
    offline decode of it is aligned on frame times (a whole number of
    frames plus the grids' phase; job scratch compare.py): over ~190
    common frames live and offline agree within 0.1 - 0.7 % of SUs at VFO
    widths 15 / 20 / 25 / 30 / 48 kHz (SU counts per frame correlate 0.98 -
    1.00); the stream time kept the wall clock to 4e-5. The earlier gap
    was different seconds of a fading signal. A VFO emulated on the 2.4
    MS/s capture (decimation to 2.4 MHz / D, flat to 0.4 W, -6 dB at 0.5
    W) passes 2906 - 2926 of 3042 SUs from 15 to 48 kHz: `min_vfo_rate`
    stays 15 kHz.
- Not done: CPDLC decoding, the 21 kbit/s C channel (no public 9.6 kbit/s
  LPC decoder), the other C rates, bursts closer than ~1.4 kHz at once (or more than four), verification of
  satellite / beam table fields (not on the recordings). Windows / Linux
  glibc syntax checks (MinGW, zig) not run - not installed on the job
  machine.
