# APRS (AX.25 1200 bit/s) decoder plugin

## Signal
APRS (Automatic Packet Reporting System): amateur-radio AX.25 packet radio,
1200 bit/s Bell 202 AFSK (mark 1200 Hz, space 2200 Hz) on an NBFM carrier
(~±3-4.5 kHz deviation, ~12-13 kHz occupied). Typical frequencies: 144.390 MHz
(North America), 144.800 MHz (Europe), 144.575 MHz (New Zealand), 145.175 MHz
(Australia). Unencrypted by rule; position beacons, weather stations,
messages, objects, telemetry, relayed by digipeaters.

## What is decoded
- Every AX.25 frame whose FCS (CRC-16/X.25) is valid **and** whose address
  field is well-formed (callsign characters, extension bits, <= 8 digipeaters).
- Source callsign-SSID, destination, digipeater path (`*` = has been repeated).
- APRS positions (lat/lon in decimal degrees, symbol table/code):
  - uncompressed `!` `=` `/` `@` (with timestamp), incl. position ambiguity
  - base-91 compressed positions
  - Mic-E (`` ` `` / `'`), incl. speed (km/h) and course
  - objects `;` and items `)` (with name)
- Packet type and text for status `>`, messages `:` (addressee), weather `_`,
  telemetry `T`, raw GPS `$`, others shown raw (escaped with `kp::printable`).

Facts: Last station, Last path, Last packet type, Last position, Last info,
Frames, Stations heard. Events: one line per frame, e.g.
`ZL1ABC-9>APRS,WIDE1-1,WIDE2-1  position -36.85200, 174.76117 [/>]: comment`.
`host.freq_error` reports the discriminator DC (carrier offset) after a
valid frame. Map ("🗺 Plot on map" in the decoder tab): every station /
object / item with a position, marker from its symbol (car, person, ship,
aircraft, house / digipeater / weather station), course and speed from the
`ccc/sss` extension, kept 1 h; a killed object is taken off.
Raw frames (decoder data log, "Raw frames" ticked): each valid frame in
TNC2 form, `SRC>DEST,PATH:info` (non-printable bytes escaped).

## Implementation
19200 S/s complex input (16 samples/bit) -> 7 kHz FIR -> FM discriminator ->
DC removal -> 1200/2200 Hz quadrature correlators over one bit (running sums)
-> three slicers with space-tone weights 1.0 / 0.55 / 1.8 (flat, TX
pre-emphasis, de-emphasis) each with its own DPLL bit clock (interpolated
zero crossings), NRZI decoding, HDLC deframing (flags, bit de-stuffing, abort
on 7 ones) and FCS check. The same frame from several slicers is reported once.

## Sources / specifications
- AX.25 Link Access Protocol v2.2 (TAPR, 1998): address field, UI frames, FCS
- APRS Protocol Reference 1.0.1 (APRS Working Group, 2000), ch. 5-10, 12-14:
  data type identifiers, uncompressed / compressed / Mic-E position formats
- Bell 202 modem tones; general approach as in Dire Wolf / multimon-ng (no code copied)

## Test results (Raspberry Pi 5, offline test mode)
| input | result |
|---|---|
| `capture.cf32` (14.3 s recording from 144.575 MHz, 5 packets) | VALID FRAMES 5/5, 0.6 % of real time |
| same, mistuned by +2.4 kHz / -1.7 kHz (`--offset`) | 5/5, freq error reported ±2.05 kHz |
| same + added noise, SNR 10 / 6 / 3 / 0 dB (in 48 kHz) | 5 / 5 / 4 / 0 frames |
| 60 s of Gaussian noise | 0 frames, 0 events, 0.5 % of real time |
| synthetic test (7 frames: spec examples for uncompressed, timestamped, ambiguous, compressed, Mic-E, object, item; flat / pre- / de-emphasised tones; -800 Hz carrier) | 7/7, positions match the spec values (49°03.50'N 72°01.75'W, 49.5/-72.75, Mic-E 33°25.64'N 20 kn 251°) |

The live receiver was not running during development, so no off-air traffic
other than the recording above was tested.

## Limitations
- Hard-decision demodulation, no single-bit-error FCS repair: weak packets
  below ~8-9 dB SNR (in the signal bandwidth) are lost.
- 1200 bit/s AFSK only (no 300 bit/s HF packet, no 9600 bit/s G3RUH).
- Not parsed: NMEA `$GPRMC` positions, `!` positions later in the info field
  (X1J TNC beacons), Mic-E telemetry/altitude extensions, base-91 compressed
  course/speed/altitude bytes, weather fields (shown as raw text), third-party
  packet contents.
- The destination in Mic-E frames is shown as the raw (encoded) address.
