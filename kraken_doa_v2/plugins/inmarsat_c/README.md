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
  found is still decoded when >= 60 % of it is there (the missing symbols
  are erasures). Only packets that pass their 16-bit sum check are used.
- **Packets** (`packets.cpp`): every packet's type, length and sum check;
  all packets are counted by type (fact "Packet types").
- **Bulletin board** (type 7D, the first packet of every frame): whose
  channel it is (NCS common channel or LES TDM, the station ID and ocean
  region), the frame number and its time of day (UTC), network version,
  status, services and randomising interval (raw values).
- **Signalling channel descriptor** (6C): the uplink channel code and the
  28 slot-state markers (counted by state).
- **LES TDMs**: the TDM channel of each LES, from packet types 81 and 92
  (layout measured, see below) - fact "LES TDMs (MHz)".
- **EGC** (types B1 / B2, the two halves of a double-header block):
  - header checked by its own sum check; service code with its name and
    SafetyNET / FleetNET / system family, LES, message sequence number,
    packet sequence number, presentation, address;
  - addresses: all ships, NAVAREA / METAREA number, coastal warning area
    (NAVAREA, station and subject letters), group (FleetNET ENID), MES
    (system messages, download group identity), chart-correction areas;
    rectangular and circular areas are shown as their raw bytes (their bit
    layout is not public and no recording had one - **not decoded, so not
    on the map**);
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
  - a shore-to-ship distress alert (service 14) starts with ⚠.
  - **Not decoded: the priority.** The header's flag byte (bit 7 = more
    packets follow) also carries the P bit, but how distress / urgency /
    safety / routine are coded is not public and the one recorded message
    (a METAREA warning, priority Safety by the SafetyNET manual's rules)
    cannot tell it. The flag byte is shown raw.
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
Channel, Frame number, Bulletin board, Signalling channel, LES TDMs (MHz),
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
- Tune a VFO to the channel; **VFO width 9.6 kHz** (the plugin's input is
  9600 Hz; it finds and follows the carrier within +-3.6 kHz of the VFO
  centre itself - it reports no frequency error to the host: kraken_doa's
  AFC moves the input in steps, which a coherent 1200-baud demodulator
  can't follow). In Auto detect a narrower VFO gets a 9.6 kHz
  stream of its own.
- NCS common channel frequencies, as published by decoder vendors and
  sigidwiki (checked 2026-11; not re-measured - check what you receive):
  AOR-W 1537.700 MHz, AOR-E 1541.450 MHz, POR 1541.450 MHz, IOR
  1537.100 MHz (channel codes 11080 / 12580 / 12580 / 10840: frequency =
  1510 MHz + code x 2.5 kHz). Since the move to the Inmarsat-4 / Alphasat
  satellites the region names may not match the satellite you see: the
  bulletin board tells which NCS it is. LES TDMs are near them
  (1537-1545 MHz); the "LES TDMs" fact lists the ones the NCS names.
- The decoder needs Es/N0 >= ~0.5 dB on a clean synthetic signal and
  ~3 dB on the real recording with noise added (C/N0 31-34 dB-Hz), see the
  test results; the "Carrier" fact shows the Es/N0 it measures.

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
