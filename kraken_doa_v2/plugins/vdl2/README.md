# VDL Mode 2 decoder plugin

## Signal
VDL Mode 2 (VHF Digital Link), the aircraft datalink that carries most
ACARS traffic today and the ATN (CPDLC) messages: 25 kHz channels in the
VHF airband, mostly 136.6-136.975 MHz - 136.975 MHz is the common
signalling channel worldwide, others depend on the region and the network
(e.g. 136.725 / 136.775 / 136.825 / 136.875 MHz in Europe, 136.650 /
136.700 / 136.800 MHz in North America; tune to the ones used where you
are - the decoder knows no frequency list). D8PSK at 10 500 symbols/s =
31.5 kbit/s, raised cosine alpha 0.6 (about 17 kHz wide); aircraft and
ground stations take turns on one channel (CSMA), in bursts of a few ms to
a few hundred ms. Unencrypted (ATN payloads are binary, not decoded here).

Burst (ICAO Annex 10 Vol III, 6.4.2-6.4.3): ramp-up (5 symbols), the
16-symbol unique word, then scrambled (15-stage PN, x^15 + x + 1): the
reserved symbol, the transmission length (17 bits) and its (25, 20) header
FEC, and the data - AVLC frames (HDLC flags, bit stuffing, CRC-16 FCS) in
rows of 249 octets with RS(255, 249) check octets (a short row sends 2 or 4
of the 6; the others are erasures), interleaved column by column.

## What is decoded
- Every AVLC frame whose FCS matches, after the header FEC (one bit) and
  the Reed-Solomon code (errors and erasures) have repaired it.
- Addresses: each frame's source and destination - an aircraft by its ICAO
  24-bit address (the same hex as ADS-B: `3C6544`), a ground station
  (`GS 2D4918`), all aircraft.
- Frame types: I (with N(S) / N(R)), RR / RNR / REJ / SREJ, XID, UI, TEST,
  DM, DISC, UA, FRMR.
- ACARS over AVLC (I frames starting `FF FF 01`): registration, flight ID,
  message number, label, block ID, text - the same fields as the acars
  plugin (`plugins/lib/acars_msg.hpp`); the aircraft's registration and
  flight are remembered with its ICAO address.
- Ground station XID (station information): the airports it serves (ICAO
  codes) and its position (to 0.1 degree) - on the 🗺 Map ("Plot on map").
- X.25 / ATN packets (CPDLC, ADS-C, ...) are counted and shown with
  "Log all messages", not decoded.

Events: ACARS messages (`↓ D-AIBC (3C6544) LH0123 · ACARS H1 message to /
from terminal · blk 3 · M01A: POSITION REPORT ...`, `↑` = uplink) and
ground station information (`📡 ground station 2D4918 LSZH 47.5N 8.6E`,
once in 10 min); link control and X.25 frames with "Log all messages".
Facts: Bursts / frames, Aircraft heard, Ground stations, Last ACARS, Last
ground station, Last carrier offset, RS corrections, Frames failed FCS.
Table: one row per aircraft and ground station - kind, registration or
airports, flight, frames, last frame; details: last frame type, position.
Raw frames (decoder data log): `VDL2 SRC>DST TYPE <hex of the frame>`.

## ID separation (DoA per aircraft and ground station)
Every burst is one transmitter's, and every AVLC frame names its sender:
each burst whose frames passed their FCS is a talker packet
(`kp::Talker::packet`) of the frames' source address - an aircraft
(`3C6544`, labelled with its registration and flight once an ACARS message
named them) or a ground station (`GS 1A2B3C`, labelled with its airports;
a 🗼 in the 📡 DF panel, aircraft ✈). Unlike plain ACARS, uplinks name
their station too. Samples: the burst from its unique word, <= 0.24 s
(kraken_doa takes packets up to 0.25 s); the channel = the burst's carrier
offset, 17 kHz wide; averaged over 10 s, kept 30 min. A burst whose frames
name different sources (none seen) gets no talker.

Test generator: `tools/vdl2_gen.py OUT.cf32 [--offset HZ] [--clock-ppm P]
[--invert] --at T "aoa:A3C6544>G1A2B3C:down:D-AIBC:H1:3:M01ALH0123TEXT"
--at T "xid:G1A2B3C:NZAA:-36.8:174.9" --at T "rr:G1A2B3C>A3C6544" --at T
"data:A3C6544>G1A2B3C:180" ...` (frames in one burst joined by `+`). The
debug-playback entry `vdl2-three-aircraft` (`tools/testsignals.json`):
3C6544 at 60°, 7C1234 at 200°, A1B2C3 at 120°, the ground station 1A2B3C at
320° (its XID position near the station), each on its own carrier offset.

## Implementation
52.5 kHz complex input (5 samples per symbol) -> 33-tap low-pass (-6 dB at
8.5 kHz) -> unique word correlator on the one-symbol differential products
z[n] z*[n-5] (15 phase changes, |sum| / sum of magnitudes >= 0.8, >= 6 dB
above the noise floor, carrier within +-3 kHz; both spectrum orientations,
so an IQ-swapped input decodes too) -> timing from the correlation peak
(parabola), carrier frequency from its phase, carrier phase from the 16
unique word symbols -> two demodulators of the burst side by side, each
with a Gardner timing loop on cubic-interpolated samples: coherent
(decision-directed carrier loop) and differential (frequency loop only);
the one with more valid frames is reported -> descrambling -> header (one
bit corrected, reserved symbol 000) -> de-interleaving -> RS(255, 249)
errors-and-erasures decoder (`rs.cpp`) -> HDLC -> FCS -> AVLC. A burst
ends when its frames are complete, its power is gone (12 symbols below 3 dB
over the noise floor), or its decisions stop looking like D8PSK (mean phase
error > 0.3 rad, ~0.39 on noise).

Measured on a real recording where the standard leaves a choice: the PN
bit is the feedback bit (stage 1 xor stage 15) that is shifted in; an
octet's first bit is its most significant bit in the interleaver and its
least significant as an RS symbol (GF(256), x^8 + x^7 + x^2 + x + 1, roots
alpha^120..125), the first data octet the highest power of x, the virtual
fill trailing; the AVLC address bits are read as sent, most significant
first: 1 flag bit, 3 type bits (1 aircraft, 4 / 5 ground station), 24
address bits. The same recording's ground station decodes as the address,
airports and position a WAVECOM screenshot of that station shows.

## Sources / specifications
- ICAO Annex 10 Vol III, Part I, chapter 6 (2nd edition, amendment 88-A):
  6.4.2 modulation, Table 6-1 (phase changes), 6.4.3.1.1 training sequence
  and header FEC matrix, 6.4.3.1.2 RS code and block lengths, 6.4.3.1.3
  interleaver, 6.4.3.1.4 scrambler and Figure 6-2 - the copy published by
  the Swiss FOCA (bazl.admin.ch)
- ETSI EN 301 841-1 V1.4.1 (preview pages): burst structure
- WAVECOM "VDL-M2 Aeronautical Data Link" (W-CODE screenshots: AVLC
  address types, XID parameters of a ground station - airport coverage,
  location), SpenchWiki / sigidwiki (frequencies)
- The AVLC address bit order, the RS and PN conventions: measured (above)
- No VDL2 decoder's source code was consulted (dumpvdl2 etc. are GPL).

## Test results (Windows 11, x86-64, offline test mode)
| input | result |
|---|---|
| sigidwiki "VDL-M2 IQ" recording (47 s, 48 kHz, IQ swapped, Zurich: 5 aircraft, 3 ground stations) | 41 valid frames in 45 bursts (headers passed), 0.4 % of real time: ACARS from F-GCBG, HB-IJW, LN-RPA, TC-JRA, CN-ROF, two LSZH ground stations' XID with their position, link set-up (XID), X.25 / ATN data, RR / SREJ. Not in the repo (real messages) |
| 30 synthetic ACARS bursts, carrier -800 Hz, clock +30 ppm, noise (burst power / noise in 25 kHz) 15 / 13 / 12 / 11 / 10 dB | 30 / 27 / 24 / 8 / 1 |
| long bursts (1300 / 3000 octets: 6 / 13 RS rows), symbol clock +-200 ppm (the standard allows +-50) | 3 / 3 each |
| XID, AOA up / down, RR, two frames in one burst, 600 / 1300-byte X.25 frames, an IQ-swapped burst at -1.5 kHz | all decoded, offset reported +1504 Hz (the input's) |
| 120 s of Gaussian noise | 0 frames, 0 events |
| `vdl2-three-aircraft` offline (`--channels 5 --uca 0.12`, 25 dB) | 11 / 11 bursts; 3C6544 60.1°, 7C1234 200.1°, A1B2C3 120.0°, GS 1A2B3C 320.0° |
| the same through kraken_doa (debug playback, a TEST build on moved ports) | the same events, talkers 60.0 / 200.0 / 120.0 / 320.0° |
| RS decoder alone: 20 000 random rows (all three sizes) with errors + erasures within its capacity / 5 000 with 5 errors | all corrected / 94 % refused, the rest caught by the FCS |

The choice of two demodulators: on the real recording differential
detection decoded more (phase noise), in white noise coherent detection
is 2-3 dB better (12 dB: 17 / 30 against 0 / 30); together they decode
the most of both (real 41, 12 dB 24 / 30).

## Limitations
- One VDL2 channel per VFO (the VFO's centre): put a VFO on each frequency
  in use (they are 25 kHz apart; 136.975 MHz first).
- ATN / X.25 payloads (CPDLC, ADS-C, CM) aren't decoded; ACARS over AVLC
  contents are shown as text (as the acars plugin), positions inside them
  aren't parsed - no aircraft map points.
- Ground station positions are what their XID says: 0.1 degree (~11 km).
- No soft-decision RS decoding: about 11-12 dB SNR (in 25 kHz) is needed.
- The address field's flag bits (command / response, air / ground) are
  not shown: on the real recording they did not behave as the names in the
  available documentation suggest.
