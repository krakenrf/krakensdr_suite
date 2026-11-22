# Inmarsat-C (STD-C) TDM / EGC decoder plugin

## Signal
Inmarsat-C is the store-and-forward data and GMDSS safety service of the
Inmarsat (Viasat) geostationary satellites. On the L-band downlink
(1525-1559 MHz; the Inmarsat-C carriers are at about 1530-1545 MHz) every
ocean region has one **NCS common channel** - the Network Coordination
Station's TDM that every terminal listens to when idle, carrying the
bulletin board, announcements and the **EGC** (Enhanced Group Call)
broadcasts: **SafetyNET** (navigational / meteorological warnings and
forecasts, SAR, shore-to-ship distress alerts) and **FleetNET** (commercial
group calls) - and each land earth station (LES) has one or more **LES
TDM** channels for its own traffic.

Every TDM is continuous BPSK at 1200 symbols/s (root-raised-cosine,
roll-off ~1, about 2.4 kHz wide), in frames of 10368 symbols = 8.64 s
(10000 frames a day, numbered from 00:00 UTC). A frame carries 639 bytes of
packets + a zero flush byte, scrambled, rate-1/2 convolutionally coded and
interleaved over the whole frame with a 64-bit unique word.

## What is decoded
- **Physical layer** (`demod.cpp`, `frame.hpp`): carrier found from the
  spectrum shape and the squared signal's line, anywhere within +-3.6 kHz
  of the VFO centre; RRC matched filter, Gardner timing, Costas loop;
  unique word (either polarity) -> de-interleaver -> soft-decision Viterbi
  (K=7) -> descrambler. A frame whose start came before the carrier was
  found is still tried when at least half of it is there (the missing
  symbols are erasures; the measured limit: 50 % at Es/N0 ~11 dB, 55 % at
  3-6 dB). Only packets that pass their 16-bit sum check are used.
- **Early recognition** (`early.hpp`): the unique word runs through the
  whole frame (two symbols at the start of each of its 64 rows), so the TDM
  and its frame boundary show long before a frame can be decoded. The
  decoder tab's **Sync** fact says what is known, honestly, as it becomes
  known: "a carrier at -1290 Hz ... looking for the Inmarsat-C unique word"
  (0.2 s after tuning to a strong carrier), then "Inmarsat-C TDM found
  (unique word, 25 rows, 0.99) ... frame-synchronised, the first frame
  completes in 7 s" (3.4 s after tuning; counted down), then
  "frame-synchronised" with the usual facts once a frame decoded. The TDM is
  declared only when at least 16 rows agree to 0.6 and correlation x
  sqrt(2 x rows) >= 7 against all 2 x 10368 frame phases - never seen on
  noise, the Aero carriers, the CW line or other carriers (largest 5.9).
  Nothing is reported as a frame (no `valid()`) before a packet passed its
  sum check.
- **Packets** (`packets.cpp`): every packet's type, length and sum check;
  all packets are counted by type (fact "Packet types").
- **Bulletin board** (type 7D, the first packet of every frame): whose
  channel it is (NCS common channel or LES TDM, the station ID and ocean
  region), the frame number and its time of day (UTC), network version,
  status, services and randomising interval (raw values).
- **Signalling channel descriptors** (6C, one per signalling channel - the
  POR NCS sends two per frame): each uplink channel code and its 28
  slot-state markers (counted by state).
- **The stations' TDMs**: the TDM channel of each LES (and of the other
  NCSs) from the station lists (types 92 and AB: network version, entry
  count, 6-byte entries {station, 3 status bytes, channel code}) and type 81
  - fact "TDMs of the stations (MHz)". Layout measured; off air every
  listed TDM was found where its code says.
- **EGC** (types B1 / B2, the two halves of a double-header block):
  - header checked by its own sum check; service code with its name and
    SafetyNET / FleetNET / system family, LES, message sequence number,
    packet sequence number, presentation, address;
  - addresses: all ships, NAVAREA / METAREA number, coastal warning area
    (NAVAREA, station and subject letters), group (FleetNET ENID), MES
    (system messages, download group identity), chart-correction areas;
    a circular area's first two bytes are the centre's whole degrees of
    latitude and longitude (one real distress relay, its text naming the
    position); its hemisphere bits and radius (bytes 3-4) have not been seen
    coded yet, nor any rectangular area - so **areas are not put on the
    map** (a point needs the hemispheres);
  - messages reassembled from their packets in sequence-number order; the
    last packet is the one without the continuation flag; missing packets
    show as `[...]`; a message whose packets stop coming is reported as
    INCOMPLETE after 3 minutes;
  - repeats (the same LES + service + sequence number, the message
    identity of IEC 61097-4 / the SafetyNET manual) are suppressed, but
    fill gaps: a message completed by a repeat is reported then (as IEC
    61097-4 5.4.10 asks: print again when a later reception is correct);
  - text: IA5 with odd parity (a parity error becomes `_`, as IEC 61097-4
    asks), ITA2 (one character per byte - unverified, no recording), data
    as hex;
  - **priority** from bits 6-5 of the flag byte (bit 7 = more packets
    follow): 0 Routine, 1 Safety, 2 Urgency, 3 Distress - measured off air on
    a shore-to-ship distress relay (3; the SafetyNET manual: service 14 is
    always Distress) and a MET forecast (1; forecasts are always Safety);
    the 2008 METAREA V gale warning reads 2, Urgency. Bits 4-0 are not
    decoded (shown in the details). An urgency / distress message or a
    service-14 alert starts with ⚠;
  - **continued packets**: a packet that does not fit the frame comes as a
    BD part at the end of the frame and a BE part at the start of the next,
    each with its own sum check; joined (without the inner packet's own sum
    check, which is not sent) they are the original packet - measured off
    air, 17 of 17 (EGC halves, a station list).
- Other packet types (announcements, acknowledgements, polls, logical
  channel assignments, LES TDM message packets ...): counted by type; with
  "Show all messages" each one is an event with its first bytes in hex.
  Their layouts (SDM Volume 4) are not public, and no field is guessed.

Events: "Receiving NCS common channel, NCS 144 (AOR-E), carrier +15 Hz"
when a channel is first decoded; one line per EGC message ("EGC SafetyNET
MET / NAVAREA warning or MET forecast, NAVAREA / METAREA 5, LES 114, msg
4101 (2 packets, 362 bytes): METAREA V / SYNTHETIC TEST WARNING ..." - from
the generator's debug playback entry).
Table: one row per EGC message (service, address, LES, message number,
packets received / total, state, the start of the text); its details hold
every header field and the whole text. Facts: Carrier (offset, Es/N0),
Channel, Frame number, Bulletin board, Signalling channels, TDMs of the stations (MHz),
EGC (messages, complete, repeated packets suppressed), Frames (decoded,
failed, channel symbol errors the Viterbi decoder corrected), Packets,
Packet types. Raw frames (decoder data log, "Log All"): every packet in
hex. Option: Repeated EGC messages (Suppress / Show again).

## Talkers (DoA)
Everything on a TDM comes from one transmitter: the ocean region's
satellite (the NCS or LES only feeds it from the ground). Each decoded
frame gives 4 talker packets of 0.24 s from its last second (kraken_doa
keeps the stream for packets only 1.5 s, and a frame is known good only at
its end), id `Inmarsat
<ocean region>` (e.g. `Inmarsat AOR-E`), label the NCS / LES, the
carrier's own 3 kHz filtered out of the VFO, averaged over 60 s - the
bearing (and, with a vertical array model, the elevation) of the
satellite, a fixed reference. Two VFOs on carriers of the same satellite
give the same talker. The Digital squelch opens on decoded frames and holds
9.5 s (a frame lasts 8.64 s). kraken_doa keeps a plugin detected for its
squelch hold when that is longer than its 4 s detection window, so the
decoder status stays "Receiving" between frames and says "Signal lost" 9.5 s
after the last one (tested in both decoder modes, 2026-11-22; before that
change it said "Signal lost" after every frame).

Ship and land terminals (the transmitters worth a bearing) transmit on
1626.5-1646.5 MHz; their bursts are not decoded - see
`docs/dev/plugin-inmarsat-c.md` *Uplinks* for what is missing.

## How to receive
- An L-band RHCP antenna aimed at the satellite (a patch or helix sold for
  Inmarsat / Iridium reception), an L-band LNA close to it powered from
  the antenna bias tee (top bar, `docs/dev/bias-tees.md`), and a filter
  against strong nearby signals if needed. Each KrakenSDR channel needs
  its own antenna for direction finding; for decoding one is enough.
- For a bearing the array must be small enough for L band: a circular
  array is unambiguous only with neighbouring antennas under half a
  wavelength apart (9.7 cm at 1541 MHz) - five antennas: a radius under
  about 8.3 cm (spacing = 2 x radius x sin 36 deg). The 12 cm radius of a
  VHF / UHF array has them 14 cm apart: the true bearing can come with
  false ones. Set the array's real radius in the DoA settings.
- With one antenna (decoding only), use **independent mode** (top bar):
  in coherent mode heimdall cuts samples out of the stream each time it
  recalibrates, and every frame that failed in the off-air test (5 of 373)
  sat at such a gap. Independent mode has no calibration; on the same
  receiver its stream ran 7 minutes without a gap (measured with the
  Inmarsat Aero decoder; this decoder was not re-run in that mode).
- Tune a VFO to the channel; **VFO width 9.6 kHz** (the plugin's input is
  9600 Hz; it finds and follows the carrier within +-3.6 kHz of the VFO
  centre itself - it reports no frequency error to the host: kraken_doa's
  AFC moves the input in steps, which a coherent 1200-baud demodulator
  can't follow). In Auto detect a narrower VFO gets a 9.6 kHz
  stream of its own.
- NCS common channel frequencies, as published by decoder vendors and
  sigidwiki (checked 2026-11): AOR-W 1537.700 MHz, AOR-E 1541.450 MHz,
  POR 1541.450 MHz, IOR 1537.100 MHz (channel codes 11080 / 12580 / 12580
  / 10840: frequency = 1510 MHz + code x 2.5 kHz). **Measured 2026-11-22
  (roof Pi 5, patch antenna, POR satellite):** NCS 244 on 1541.450 MHz, a
  standby NCS 245 on 1539.575 MHz, LES TDMs 201, 202, 203, 204, 206, 211,
  212, 217, 221, 230 between 1539.505 and 1539.725 MHz - all exactly on the
  codes the NCS lists. The bulletin board tells which NCS it is; the "TDMs of
  the stations" fact lists the LES TDMs to tune to next.
- The decoder needs Es/N0 >= ~0.5 dB on a clean synthetic signal and
  ~3 dB on the real recording with noise added (C/N0 31-34 dB-Hz), see the
  test results; the "Carrier" fact shows the Es/N0 it measures. Off air with
  a satellite patch antenna and its LNA on a KrakenSDR at gain 49.6 dB the
  POR NCS read Es/N0 9-13 dB and the LES TDMs 8-10 dB: 6-10 dB of margin.

## Sources
No GPL decoder of this signal (Scytale-C, stdcdec, SatDump, ...) was
opened; nothing here is translated from one.
- Inmarsat-C System Definition Manual, release CD004 (Inmarsat, 2004),
  Volumes 1-3 - **an unofficial copy on pdfcoffee.com**; Inmarsat does not
  publish the manual. Volume 1 Ch. 4 3.1.1 (TDM frame: 10368 symbols, 639
  + 1 bytes, 160 groups of 4 bytes each inverted or not by a fixed
  sequence, bit 1 first, G(1) = 1011011 / G(2) = 1111001, the 64 x 162
  matrix with the unique word in columns 0-1, the row order i = 39 j mod
  64, the unique word 07EA CDDA 4E2F 28C2 in transmit order), 3.1.2 /
  3.1.3 (message and signalling channels), 3.2 (bit and byte order), 13
  (EGC: single / double header, the continuation marker); Volume 2 Part 1
  Ch. 3-4 (the service code's low digit = the address length); Volume 3
  Ch. 2 9 (C-codes, service codes, address formats) and Ch. 3 7 (station ID
  byte: 2-bit ocean region, 6-bit number, NCS x44-x63). Volume 4 (packet
  formats) is not in that copy and was not used. The field names of the
  EGC header (service code, C, P, repetition, message sequence number,
  packet sequence number, presentation, LES ID, address) are as the
  manual's figures name them (quoted in web search results).
- IMO MSC.1/Circ.1364, Revised International SafetyNET Manual (2010):
  C-codes, priorities per service, message identity.
- IEC 61097-4:2007 (Inmarsat-C SES / EGC receiver) - duplicate handling
  and the `_` for a character with a parity error.
- Nera Inmarsat-C service manual (manualslib.com) - the bulletin board's
  field order.
- Furuno FELCOM manuals (NCS channel codes 11080, 12580, 10840),
  Wavecom documentation and sigidwiki (NCS frequencies) - together they
  give the channel raster.
- Measured on the real recording (sigidwiki "Inmarsat-C TDM EGC IQ", AOR-E
  NCS common channel, 49 s at 48 kHz) and not in any public text used:
  the scrambling sequence (s[i] = s[i-4] + s[i-5] + s[i-6] + s[i-8] from
  0000 0001 - Berlekamp-Massey on the idle fill), that both unique-word
  columns hold the same bit and the matrix row order (both also found by
  a brute-force search), the encoder order and start / end state, the sum
  check (Fletcher modulo 256, both running sums zero), the packet length
  rules, the EGC header layout and its own sum check, the layouts of types
  81 and 92, the pulse shape's roll-off. The generator rebuilds each of
  the recording's frames from its 639 decoded bytes symbol for symbol
  (41472 of 41472).

## Recognition time (2026-11-23)
From tuning to the decoder knowing the channel, on real captures (the
decoder started every 0.25 s over two frame periods; times from the start):

| | carrier | TDM recognised | first decoded frame (mean / median / max) |
|---|---|---|---|
| before (NCS 244, 81 starts) | ~0.85 s (not shown) | - | 9.3 / 9.0 / 13.8 s |
| now (NCS 244, 81 starts) | 0.21 s | 3.5 s | 8.3 / 7.9 / 12.9 s |
| before / now (NCS, a second capture, 71 starts) | | - / 3.5 s | 9.5 / 8.6 s |
| before / now (LES 203, Es/N0 8 dB, 29 starts) | | - / 3.7 s | 8.7 (3 of 29 none in 13 s) / 8.3 s |
| now, generated Es/N0 3 dB / 1 dB | 1.0 s | 4.4 / 4.7 s | 9.7 / 9.2 s |

The first frame cannot come before the end of the frame in progress, and
that frame decodes only if at least half of it was received (a rate-1/2
code cannot do with less); for a tune at a random moment that is 8.64 s
on average however fast everything else is - the decoder is there now.
What it can say earlier, it says: the carrier in 0.2 s, the TDM and when
its first frame will complete in 3.5 s (25 rows of the unique word - the
fewest that keep false recognitions out).

Through the real receiver (roof Pi 5, one sample per line; first frames
vary with the frame phase): independent mode - carrier 0.2-0.5 s,
recognised 3.4-3.8 s, first frame 5.5-12.2 s, the announced time right to
0.1 s; coherent mode - the same after moving the VFO or switching the
decoder on, but after a retune heimdall's cooldown (3.6-4 s) and its
calibration burst (~0.5 s of noise source and missing samples, ~4.7 s
after the retune) come first: recognised 7.3-9.4 s, first frame 11.9-18.7 s
(before: 11.9 / 18.6 s, and once nothing within 28 s). The burst rows are
now taken out (erasures, search restarted) instead of weighing on the
search for a whole frame. In 2 of 7 coherent retunes (and 1 of 5 before)
heimdall's calibration ran much longer - its log shows sample slips during
the phase recalibration and the lags re-acquired - and no BPSK reached the
decoder for ~30 s (recognised 36.7 / 39.7 s); that is heimdall's, on this
array with one antenna. The LES TDM 217 live: the same pattern (independent
recognised 3.5-3.6 s, first frame 5.1-12.3 s; before 7.8-11.5 s).

## Off air (2026-11-22, roof Pi 5, KrakenSDR CH0, satellite patch + LNA on the CH0 bias tee)
- **NCS 244 (POR)**, gain 49.6 dB, VFO 9.6 kHz on 1541.450 MHz: 55 minutes
  live over 8 turns - 368 frames decoded, 5 failed (each one at a heimdall
  calibration gap, where samples are cut out), 0 dropped samples; Es/N0
  9.1-12.8 dB (mostly 10.5-12); 0-0.27 % channel symbol errors corrected;
  carrier -1356 Hz at the start, -1265 Hz two hours later (the RTL-SDR
  warming up); frame numbers advance by one per frame, their time of day
  agrees with UTC within ~2 s; the status stays "Receiving" between frames
  (the engine's squelch-hold change), "Signal lost" only at the calibration
  gaps (8 s with the resynchronisation of 0ab82be, 25 s before it).
- **EGC**: none for the first hour (04:02-05:01 UTC), then (05:15-05:57 UTC)
  four SafetyNET messages - a shore-to-ship distress relay (3 packets,
  Distress, circular area), a 32-packet MET forecast for METAREA XII
  (Safety; 17 of its packets crossed frames as BD + BE), a 3-packet SAR
  coordination to a circular area (Urgency) with the new build live, and
  one 2-packet message a run caught only partly. Every header passed its own sum
  check, every text byte its parity; repeats: none came within the turns
  (their intervals are hours). Packet types seen on the NCS: 6C 7D 81 92
  93 99 A2 A3 A8 AB AC B1 B2 BD BE.
- **Time to the first frame**: 10-15 s after a retune (heimdall's
  calibration, then the first whole frame); 5-7 s after moving only the VFO
  (the partly received first frame decodes). With the VFO put off on purpose
  - carrier at -3624, -3324, +1677 Hz from the VFO centre - every frame
  decoded.
- **LES TDMs** (centre 1539.600 MHz, 52 s each): all eight the NCS listed
  decode (202, 203, 204, 206, 211, 212, 217, 221; 5-9 frames each, 1 failed
  frame in all), Es/N0 8.0-10.4 dB; a 20 s capture showed LES 201, 230 and the
  standby NCS 245 as well. LES TDM packet types: 08 27 2A 5E 83 85 91 99 9A A0
  AA AC AD BD BE (header fields not decoded; counted).
- **CPU on the Pi 5**: the plugin live 0.55 % of one core (`/proc` over 60 s);
  offline 15.7 M cycles per second of signal on a 48 kHz capture (0.65 % of a
  2.4 GHz core, the host's resampler included), 29 M cycles/s while searching
  noise. The offline harness at 2.4 MS/s spends ~15 % of real time in its own
  resampler (not the plugin).
- Recordings of all this are in the IQ library (`inmarsat/offair_roof_2026-11-22/`,
  local only).

## Test results (2026-11-22, x86 test machine)
- **Real recording** (sigidwiki, 49 s): 5 frames (the first 73 % present),
  65 packets, every one passes its sum check; 0 channel symbol errors
  (Es/N0 ~21 dB); NCS 144 (AOR-E), frame numbers in sequence; 11 packet
  types; one EGC SafetyNET message (service 31, LES 114) - 2 of its
  packets, both halves each, header sum checks pass, 512 text bytes with
  no parity error. 0.5 % of real time.
- **The real recording with added noise** (3 seeds each): all 65 packets
  down to Es/N0 3 dB; 55-65 at 2 dB; 55-57 at 1 dB; 42-55 at 0 dB.
- **Generator, AWGN** (8 files x 6 frames each, carrier 300-650 Hz off):
  see `docs/dev/plugin-inmarsat-c.md` for the table - every frame with
  every packet down to Es/N0 0.5 dB (Eb/N0 3.5 dB); the demodulator's
  symbol error rate equals uncoded BPSK theory (8.1 % at 0 dB, theory
  7.9 %).
- **Acquisition**: carrier offsets -3600 .. +3600 Hz decode (beyond +-4 kHz
  the signal leaves the 9.6 kHz input); drift 2 Hz/s, sample clock
  +-208 ppm, inverted polarity: decoded. The first frame is decoded at
  the end of the first frame the signal covers (8.64 s) down to Es/N0 0 dB.
- **Noise** (10 min) and **Classic Aero 10500 baud** (the other worker's
  recording): no frame, no event; 1.4 % of real time while searching.
- **Debug playback end to end** (a TEST kraken_doa, the playback's
  interpolation to 2.4 MHz, live noise, decimator, plugin, Digital squelch,
  talker DoA; 100 s = 2.9 loops each): `inmarsat-c-ncs-safetynet` - Es/N0
  9.6 dB, carrier +700.0 Hz, 11 frames decoded / 0 failed, 112 packets, 3
  EGC messages, 16 repeated packets suppressed, talker 70.0 deg, VFO DoA
  70.0; `inmarsat-c-les-tdm-weak` - Es/N0 3.5 dB, -1500.1 Hz, 11 / 0, 94
  packets, the group call, talker 200.0 deg, VFO DoA 200.0. Every expect
  row met; plugin 0.7 % of one core.
- **Debug playback entries offline** (`tools/testsignals.json`), built with
  `make_testsignal.py` and decoded offline with the noise the playback
  adds, 3 loops each: `inmarsat-c-ncs-safetynet` - 3 EGC messages
  complete, 16 repeated packets suppressed, talker `Inmarsat AOR-E` at
  70.0 deg (truth 70); `inmarsat-c-les-tdm-weak` (Es/N0 3.7 dB, -1.5 kHz)
  - the group call, talker at 200.0 deg (truth 200).

## Files
`decoder.cpp` (plugin, reports), `demod.hpp/.cpp` (BPSK demodulator),
`frame.hpp` (unique word, de-interleaver, Viterbi, descrambler),
`packets.hpp/.cpp` (packets, sum check, bulletin board, EGC reassembly),
`tools/inmc_gen.py` (generator), `tools/testsignals.json` (debug playback).
