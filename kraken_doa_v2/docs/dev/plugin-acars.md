# ACARS plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`).

**ACARS, VHF (`plugins/acars/`, README.md there; added 2026-10-25):**
- ARINC 618 "plain old ACARS": AM, 2400 bit/s MSK (1200 / 2400 Hz), NRZI
  (2400 Hz = the bit repeats). 24 kHz input (10 samples per bit),
  `min_vfo_rate` 12500, category Aviation, talkers, no map, no options.
  Region-neutral: no frequency list, the user tunes the channel
- Measured facts the public sources don't give (sigidwiki "ACARS IQ",
  5 real blocks): BCS = CRC-16/KERMIT (reflected 0x1021, init 0, no
  xorout) over mode .. ETX / ETB (SOH excluded), low byte first - the CRC
  over the block + BCS is 0. Characters go on air with their odd parity
  bit: ETX 0x83, ETB 0x97 (0x17 has four 1s - the first version looked for
  0x17 and lost every multi-block message), sync `+` 0xAB, `*` 0x2A, SYN
  0x16, SOH 0x01
- Demodulator: 49-tap 5.5 kHz channel filter -> |x| -> minus / divided by
  its mean (carrier level, ~10 ms) -> x e^-j2pi1800t, 33-tap 1.3 kHz
  low-pass -> DPLL from the zero crossings of Im(z[n] z*[n-10]) (gain 0.5
  hunting, 0.15 in a block; crossings half a bit after a boundary) -> the
  boundary interpolated, x e^-j2pi600t (= the 2400 Hz reference, both
  tables 40 samples) -> phase from the squared samples (1/16 hunting, 1/32
  in a block; the half angle followed continuously) -> bit = sign, soft =
  |Re|. Coherent NRZI: one error = one bit (tone-by-tone decisions would
  invert the rest of the block). Sync: 40 bits `+*` SYN SYN SOH, <= 4
  errors, either polarity; abort after 6 parity errors or 240 characters
- Block check: at every character whose position allows a suffix (>= 12
  after SOH) and that is <= 1 bit from ETX / ETB, the BCS two characters
  later is tried. Repair: <= 4 bad-parity characters, each flips one of its
  two least reliable bits (<= 16 trials), else (no parity error) one of the
  BCS's three least reliable bits. CRC-16/KERMIT is linear with init 0: an
  all-zero block would pass, but 0x00 fails odd parity
- Reporting: `host.valid` from `+` to the end of the BCS; downlink (digit
  block ID) = packet talker id = registration (leading `.` stripped),
  label = flight ID; ordinary uplinks no talker (an uplink names its
  recipient, not its station); squitter (label SQ, `\d\dX[AS][A-Z]{3}`) =
  talker `GS <IATA>` (page: kind `gs`, like UAT's ground stations;
  `RDF_PK_KIND.acars = 'aircraft'`). Packet = the last <= 0.24 s up to
  the BCS end, `freq_hz` = the block's carrier offset (arg of the ~40 ms
  average of y[n] y*[n-1] - the carrier dominates, the sidebands cancel),
  `bw_hz` 7000, avg 10 s, keep 30 min. No `freq_error` (the envelope
  doesn't need AFC, and aircraft / ground carriers differ)
- Multi-block: key direction + address + label; ETB blocks appended (a
  repeated block ID once), the ETX block emits "message of N blocks";
  pending messages dropped after 120 s without a block
- The block's fields (`acars::Block`, registration, label names, MSN /
  flight split, squitter) live in `plugins/lib/acars_msg.hpp` (header
  only), shared with the vdl2 plugin's ACARS over AVLC (*VDL Mode 2*);
  the refactor was re-tested with every test below (identical results)
- Tests (2026-10-25, scratch; README has the table): the real recording 5 /
  5; 20 synthetic downlinks 20 / 20 down to 12 dB C/N in 12.5 kHz, 16 at
  6 dB; 120 s noise 0 events; `acars-three-aircraft` offline 40.1 / 165.0
  / 290.0 / 229.9° and through a TEST kraken_doa (debug playback, ports
  2808x / 2809x, cache made by make_testsignal.py) 40.0 / 165.0 / 290.0 /
  230.0°, every event incl. the two-block message. CPU 0.2-0.4 % of real
  time on x86-64 (not measured on a Pi)
- Not done: HFDL, message-content parsing (positions in label 16 / H1,
  OOOI), squitter positions (format not published). VDL Mode 2: the vdl2
  plugin (*VDL Mode 2*)
