# VDL Mode 2 plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`).

**VDL Mode 2 (`plugins/vdl2/`, README.md there; added 2026-10-25):**
- ICAO Annex 10 Vol III 6.4 (D8PSK 10 500 symbols/s, RS(255,249),
  interleaver, scrambler, (25,20) header FEC) - the full text is public as
  the Swiss FOCA's copy of Annex 10 Vol III (bazl.admin.ch); EN 301 841-1
  only summarises it, Doc 9776 (AVLC) isn't free. 52.5 kHz input (5
  samples per symbol), `min_vfo_rate` 25000, category Aviation, talkers,
  map (ground stations), no options. One channel per VFO; no frequency list
- Measured on the sigidwiki "VDL-M2 IQ" recording (Zurich, IQ swapped):
  the PN bit = stage 1 xor stage 15, shifted in, from the reserved symbol
  on (the unique word isn't scrambled); triplets X Y Z = bits 3n, 3n+1,
  3n+2, the unique word's triplets read as written; RS symbols = the
  interleaver octets bit-reversed (an octet's first bit is its msb in the
  interleaver, the lsb of the GF symbol), the first data octet the highest
  power, trailing virtual fill, roots alpha^120..125 (UAT's code: same
  field and roots); AVLC = HDLC (lsb first, CRC-16/X.25 FCS); address: the
  7 bits after each octet's extension bit, as sent, msb first = flag (1),
  type (3: 1 aircraft, 4 / 5 ground station), address (24). Checked against
  WAVECOM's screenshots of the same stations (2D4918 / 10981A / 47806D,
  airport coverage LSZH, location 47.5N 8.6E) and the aircraft's national
  ICAO blocks (F-GCBG 390826, LN-RPA 47806D, TC-JRA 4BAA41)
- The address flag bit: in the source it toggles with the frame type for
  one aircraft (I 0, RR 1), in the destination it is 1 towards aircraft, 0
  towards ground - not what "A/G in the source" would give; not shown
- Hunting: per sample the 15 unique word phase changes against z[n]
  z*[n-5] (both conj orientations), q = |sum| / sum |.| >= 0.8, sum > 15 x
  4 x noise floor, offset < 3 kHz; the best q within one symbol wins;
  parabola on |c| for the fraction. Noise floor: down fast, up 0.00005 /
  sample (0.002 in the first 0.1 s - the start-up false lock below), not
  up during a burst
- Burst: two `Demod`s from the same lock (coherent: decision-directed
  PLL KP 0.15 / KI 0.005; differential: frequency loop 0.01), each with a
  Gardner loop (KT 0.03, t += SPS - KT e; +KT diverges) on 4-point
  Lagrange interpolation; the burst is over when both are done / failed,
  the one with more FCS-valid frames is reported (tie: coherent). A demod
  fails on the header (one bit corrected by the syndrome = a column of H,
  reserved 000, length 104..131071 bits), on power (12 symbols < 2 x noise
  floor) or on EVM (mean |phase error| over ~32 symbols > 0.3 rad)
- Tuning history (scratch, real recording 45 bursts / 30 synthetic bursts
  at 12 dB): coherent alone 33-38 / 17, differential alone 39 / 0, both 41
  / 24; channel filter 33 taps 8.5 kHz best (49 taps 10.5 kHz: 35 / 9 -
  more noise; 49 taps 8.5 kHz: 37 / 2 - ISI); Gardner KT 0.1 cost 3 real
  bursts, 0.01-0.05 equal, 0 fails the +-200 ppm long bursts; a start-up
  false lock on noise (nf still ~0) with a passing header ran 0.7 s over
  the first real bursts - fixed by the fast start-up floor + the EVM abort
- Reporting: `host.valid` from the unique word to the last symbol; talker
  packet per burst = the frames' common source: aircraft `%06X` (as
  adsb), ground `GS %06X`; label registration + flight (from ACARS over
  AVLC, remembered per address) or the ground station's airports (XID 0xF0
  group, PI 0xC1); <= 0.24 s from the unique word, `freq_hz` = the lock's
  offset (sign for an inverted input flipped), `bw_hz` 17000, avg 10 s,
  keep 30 min. Page: `RDF_PK_KIND.vdl2 = 'aircraft'`, `GS ` ids `gs` (as
  uat / acars). Map point per ground station from XID PI 0xC8 (12-bit
  lat / lon, 0.1 degree, two's complement), ttl 1 h
- ACARS over AVLC: I frame, info `FF FF 01` + the ACARS block (mode ..
  ETX / ETB, BCS, DEL) - parsed with `plugins/lib/acars_msg.hpp`, shared
  with the acars plugin (*ACARS*)
- Tests (2026-10-25, scratch; README has the table): RS 20 000 rows within
  capacity all corrected; real 41 frames; synthetic SNR 15/13/12/11 dB:
  30/27/24/8 of 30; +-200 ppm 3/3; noise 120 s 0; `vdl2-three-aircraft`
  offline 60.1 / 200.1 / 120.0 / 320.0°, through a TEST kraken_doa (debug
  playback) 60.0 / 200.0 / 120.0 / 320.0°, every ACARS event. CPU 0.4 % of
  real time on x86-64 (not measured on a Pi)
- Not done: several channels in one wide VFO (a channelizer, as dme),
  X.25 / CLNP / ATN applications (CPDLC, ADS-C), soft-decision RS
