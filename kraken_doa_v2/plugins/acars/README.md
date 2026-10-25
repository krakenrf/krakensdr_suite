# ACARS (VHF, 2400 bit/s) decoder plugin

## Signal
ACARS (Aircraft Communications Addressing and Reporting System), the
ARINC 618 air / ground character link ("plain old ACARS" on VHF): AM voice
channels in the aeronautical band, 129-137 MHz (e.g. 131.550, 131.725,
131.825, 130.025, 129.125, 136.900 MHz - which ones depend on the region
and the network, ARINC or SITA; tune to the one used where you are). The
audio is MSK at 2400 bit/s, tones 1200 Hz (half a cycle per bit) and
2400 Hz (one cycle), on an AM carrier; the line code is NRZI (2400 Hz = the
bit repeats, 1200 Hz = it changes). Aircraft (downlinks) and ground stations
(uplinks) share the channel and take turns (CSMA). Unencrypted.

Block: pre-key (1 bits), `+` `*` SYN SYN SOH, mode, address (7 characters:
the aircraft registration right-aligned, `.` padded; NULs = every
aircraft), technical acknowledgement (NAK or the block ID being
acknowledged), label (2), block ID (downlink: a digit, uplink: a letter),
STX (ETX when there is no text), text (<= 220 characters; a downlink's
starts with the 4-character message number, e.g. `M01A`, and the 6-character
flight ID), ETX (the last block) or ETB (more blocks follow), BCS (16 bits),
DEL. 7-bit ASCII with odd parity, LSB first; the BCS has no parity.

## What is decoded
- Every block whose BCS matches - CRC-16/KERMIT (x^16 + x^12 + x^5 + 1
  reflected, init 0) over mode .. ETX / ETB, low byte first. The standard
  (ARINC 618) isn't public; the parameters were measured on a real
  recording (below).
- Direction, aircraft registration, flight ID and message number
  (downlinks), label (with a name for a few common ones: `_d` general
  response, `Q0` link test, `SQ` squitter, `SA` media advisory, `H1`
  message to / from a terminal), block ID, acknowledgement, the text.
- Messages of several blocks (ETB ... ETX) are put together and reported
  once more as a whole ("message of N blocks"); a retransmitted block is
  counted once.
- Ground station squitters (label `SQ`, text `0vX` + network `A` ARINC /
  `S` SITA + the station's IATA code): the station is named ("SITA ground
  station ZRH"). Squitter positions aren't parsed (their format isn't
  published); the plugin has no map points.
- Parity errors are repaired: up to 4 characters with a wrong parity, each
  by flipping one of its two least reliable bits (the coherent detector's
  soft values), and one error in the BCS (its three least reliable bits) -
  at most 16 BCS trials per block, so a wrong repair stays rare (~1 in
  4000 damaged blocks); the event says how many bits were repaired.

Events: one line per block, e.g. `↓ G-ABCD AB0042 · H1 message to /
from terminal · blk 4 · M03B: SECOND AND LAST BLOCK` (↓ downlink, ↑
uplink, `…` = more blocks follow); blocks without text (acknowledgements)
only with "Log all messages"; a retransmission once in 30 s. Facts: Blocks
(down / up), Repaired blocks, Aircraft heard, Last aircraft, Ground
station, Last label, Last text. Table: one row per aircraft (and ground
station) - flight, last label, blocks, last text; details: the last block's
direction / mode / block ID, its carrier offset, the last text. Raw frames
(decoder data log): `ACARS DL 2 .G-ABCD NAK H1 4 ETX M03BAB0042...`.

## ID separation (DoA per aircraft)
A downlink is the aircraft's own transmission: each one is a talker packet
(`kp::Talker::packet`) named by the registration, labelled with the flight
ID. An uplink is the ground station's transmission, and an uplink names
only the aircraft it is FOR - so ordinary uplinks get no talker (one would
mix every station on the channel); a squitter names its station, which
then gets a talker `GS <IATA>` (a 🗼 ground station in the 📡 DF panel,
aircraft are ✈). Samples: the block's last <= 0.24 s up to the end of its
BCS (kraken_doa takes packets up to 0.25 s; a block is 0.1-0.8 s), the
channel = the transmitter's own carrier offset, measured over the block
(aircraft and ground stations are often a kHz or more apart), 7 kHz wide;
averaged over 10 s, kept 30 min.

Test generator: `tools/acars_gen.py OUT.cf32 [--offset HZ] [--depth 0.7]
--at T "down:N123AB:H1:2:M01AXY0123TEXT" ...` (real framing, parity, BCS,
NRZI MSK on AM; `down+` / `up+` = ETB, `up::SQ:A:02XSZRHLSZH` = a
squitter). The debug-playback entry `acars-three-aircraft`
(`tools/testsignals.json`): N123AB at 40°, G-ABCD at 165° (a message of
two blocks), VH-XYZ at 290°, the SITA ground station ZRH at 230° (an
acknowledgement and an addressed uplink - no talker - and two squitters),
each on its own carrier offset (+800 / -1200 / +300 / -400 Hz).

## Implementation
24 kHz complex input (10 samples per bit) -> 5.5 kHz channel filter ->
envelope (AM: independent of the carrier offset) -> minus its mean (the
carrier level) and divided by it -> mixed by 1800 Hz, 1.3 kHz low-pass (the
MSK at +-600 Hz) -> DPLL bit clock from the zero crossings of the phase
change over one bit (+90° = 2400 Hz, -90° = 1200 Hz) -> each bit boundary
interpolated and turned onto a 2400 Hz reference, where its phase is 0° or
180° = the bit (a 2400 Hz bit keeps the phase, a 1200 Hz bit turns it by
half a cycle) -> carrier phase from the squared samples (decision free,
followed without 180° jumps; the ambiguity is the sync's polarity) -> sign
= bit, magnitude = reliability. So the NRZI decoding is coherent: one
wrong decision costs one bit, not the rest of the block, as deciding the
tones one by one and integrating them would. Sync: the 40 bits `+` `*` SYN
SYN SOH, <= 4 bit errors, either polarity. Characters are collected until
a suffix (ETX / ETB, also one bit off) is followed by a matching BCS.

## Sources / specifications
- Block layout, character format, uplink / downlink field values,
  squitters: the WAVECOM decoder documentation for ACARS
  (wavecom.ch, "ACARS") and the ARINC 618 table of contents
- Tones, MSK, NRZI (2400 Hz = no change), the sync characters: Stanford
  EE179 Lab 8 (J. Pauly, "ACARS decoding")
- BCS polynomial / initial value / coverage / byte order: measured on the
  real recording below (no public text has them)
- Squitter prefix (`0vX` + A / S + IATA): RadioReference / acars-vdl2
  forum descriptions
- No ACARS decoder's source code was consulted (acarsdec etc. are GPL).

## Test results (Windows 11, x86-64, offline test mode)
| input | result |
|---|---|
| sigidwiki "ACARS IQ" recording (3.46 s, 135.5 kHz float WAV, 5 real blocks at 14-35 dB, carrier -1.0 to -1.4 kHz) | 5 / 5 blocks (4 downlinks, 1 uplink acknowledgement), the longest a 220-character ETB block; 0.4 % of real time. The recording isn't in the repo (real aircraft messages) |
| 20 synthetic downlinks of ~110 characters, carrier -2.2 kHz, noise (carrier / noise in 12.5 kHz) 30 / 15 / 12 dB | 20 / 20 each |
| the same at 10 / 9 / 8 / 7 / 6 / 5 dB | 19 / 19 / 19 / 17 / 16 / 5 (repaired: 0 / 0 / 1 / 8 / 11 / 5) |
| 120 s of Gaussian noise | 0 blocks, 0 events, 0.2 % of real time |
| synthetic: 2 aircraft, a message of two blocks, an acknowledgement, a squitter, an uplink, carrier +1.5 kHz | 6 / 6, the message put together, offset measured +1.50 kHz |
| `acars-three-aircraft` offline (`--channels 5 --uca 0.12`, 20 dB) | 11 / 11 blocks; talkers N123AB 40.1°, G-ABCD 165.0°, VH-XYZ 290.0°, GS ZRH 229.9°; no talker for the two uplinks |
| the same through kraken_doa (debug playback, a TEST build on moved ports) | the same events, talkers 40.0 / 165.0 / 290.0 / 230.0°, playback 8 % of a core |

## Limitations
- VHF ACARS only: no VDL Mode 2 (31.5 kbit/s D8PSK on 136.725-136.975
  MHz), no HFDL, no satellite ACARS.
- Message contents are shown as text; the airline / avionics formats
  inside (positions in label 16 / H1 reports, OOOI events, weather ...)
  aren't parsed, and there are no map points.
- Only a few labels have a name; the others are shown as their code.
- Two bit errors in one character (parity stays right) aren't repaired.
