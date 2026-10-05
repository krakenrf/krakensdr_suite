# v23_telemetry - V.23 AFSK 1200 bit/s async telemetry

Decoder for a SCADA / telemetry style radio-modem link found at
457.2765 MHz by the AI Signal Lab (session 20261004-201248-457276kHz). The
system / vendor is **unknown**; everything below was measured from the
recording, not taken from a published specification.

## Signal

| layer | parameters |
|---|---|
| RF | NBFM, ~12.6 kHz occupied bandwidth, peak deviation ~4.5 kHz (one transmitter ~2 kHz) |
| modem | AFSK on the FM audio: mark 1300 Hz, space 2100 Hz (the ITU-T V.23 tone pair), 1200 baud, continuous phase |
| lead-in | 0.45-0.6 s of steady mark tone (key-up / squelch time) |
| characters | asynchronous UART: start bit 0, 8 data bits LSB first, **odd parity**, 1 stop bit (one station uses 2) |
| packet | `FF x N` (byte sync, N = 4..15) - `00` - `L` - payload - `CRC16` |
| length | `L` = number of bytes from `L` itself through the CRC |
| CRC | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0) over `L` + payload, high byte first |

The CRC and length rules were found by brute-force search over CRC-16 variants;
they verify on every packet in the test recording. The payload format is
proprietary: the decoder shows it as hex.

## What the plugin reports

- event per packet that passed UART framing, parity and CRC:
  `packet L=39 8O2 dev 2.0 kHz CRC ok: 27 00 7A 50 ...` (L + payload, CRC
  omitted; 48 bytes at most unless "Log all messages" is on).
  `(1 char repaired)` marks a packet that needed the single-error repair.
- facts: packet counter, repaired packets, character format (8O1 / 8O2 /
  8E1 / 8N1), deviation of the last packet (lets you tell the transmitters
  apart), last packet, a histogram of packet lengths, carrier offset
  (`freq_error`)
- with "Log all messages": packets whose CRC failed go to the plugin log
  (stderr) only - never to events or facts

Nothing is reported for data that failed a check.

## Demodulator

FM discriminator (19.2 kHz, 16 samples/bit) -> DC removal -> sliding
quadrature correlators at 1300 and 2100 Hz over one bit -> three slicers with
mark/space weights 1.0 / 0.6 / 1.7 (pre- / de-emphasis tilt) -> per-slicer
DPLL bit clock -> UART deframers for 8O1, 8E1 and 8N1 in parallel -> packet
assembler -> CRC. Duplicates from several slicers/formats are merged within
50 ms and reported with the strictest format that passed (parity-checked
before 8N1). The option *Character format* restricts the deframers.

Single-error repair (parity formats only): a character with one bit error
fails its parity check, which locates it. One damaged character per packet
(including a stop bit read as 0) is tolerated; on completion the 8 single-bit
flips and the unchanged value are checked against the CRC. If the damaged
character is the length byte, its 9 candidate lengths are each checked when
that many bytes have arrived. At most 9 CRC trials per packet, so the CRC
still leaves a false-accept probability below 1.4e-4 for a packet that has
already passed sync, length and parity on every other character.

## Test results

`build/decoder --file ...` on the session recordings (Raspberry Pi 5):

| recording | result | CPU |
|---|---|---|
| capture.cf32 (10 s, 3 complete packets + 1 truncated at the start) | 3 valid packets (L = 6, 27, 39) | 0.5% of real time |
| long60.cf32 (60 s live, 17 bursts, ~11 dB above the noise floor) | 16 valid packets (L = 6..86, 2 transmitter styles 8O1/8O2); the 17th burst is cut by the end of the recording. One weak L = 6 packet at 35.28 s (bit errors) was only found after adding the damaged-character handling; before that 15 | 0.5% |
| capture + AWGN, 14 / 10 / 8 / 6 dB SNR (vs. burst power, 30 kHz) | 3 / 3 / 3 / 3 valid packets | 0.5% |
| capture + AWGN, 4 dB SNR | 2 valid packets (1 repaired); the 2 kHz-deviation one is lost | 0.5% |
| empty20.cf32 (20 s live, 456.900 MHz) | 0 packets, 0 events | 0.4% |
| 60 s Gaussian noise | 0 packets, 0 events | 0.5% |
| 300 s synthetic V.23 AFSK, random 8O1 characters (FF/00 rich), 15 dB SNR | 0 packets, 0 events | 0.4% |

## Limitations

- Only the framing seen on one link was verified (`FF.. 00 L .. CRC`). Other
  V.23 modems using the same tones with another framing (e.g. Modbus RTU,
  CRC-16/MODBUS, no length byte) are not decoded.
- The leading `00` might be the high byte of a 16-bit length; packets longer
  than 255 bytes are not supported.
- Payload meaning (addresses, commands, values) is unknown; shown as hex.
- 1200 baud only; MPT1327 (1200/1800 Hz) and Bell 202 / APRS (1200/2200 Hz)
  have their own plugins.
