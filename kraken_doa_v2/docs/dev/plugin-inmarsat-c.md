# Inmarsat-C plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`).

**Inmarsat-C / STD-C TDM (`plugins/inmarsat_c/`, README.md there; added
2026-11-22):** the user asked for "Inmarsat STD-C EGC ... and anything else
on Inmarsat that is decodable". The plugin decodes the NCS common channel
and LES TDMs (L-band downlink) up to the packets, and EGC SafetyNET /
FleetNET messages in full. 9600 Hz input (8 samples per symbol),
`min_vfo_rate` 9600, category Marine & distress, talkers (the satellite),
no map, one option (Repeated EGC messages), `squelch_hold_s` 9.5.

## How the facts were found
Inmarsat does not publish the System Definition Manual (SDM). The open
decoders of this signal are GPL (not opened - licensing rules). What was
used, and how each item was settled:

| Item | Source | Settled by |
|---|---|---|
| Frame 10368 symbols, 639 + 1 bytes, 64 x 162 matrix, UW in columns 0-1, row order i = 39 j mod 64, UW 07EACDDA4E2F28C2 (transmit order), G(1) 1011011 / G(2) 1111001, bit 1 (LSB) first, scrambling by 4-byte groups | SDM CD004 Vol. 1 Ch. 4 3.1.1 (unofficial copy, pdfcoffee.com) | the recording: UW column 64/64 in every frame; a brute-force search over all row orders (odd multipliers x offsets) and the four polynomial orientations found only i = 39 j (parity-check fraction 0.994, next 0.92) and G1 = 0x6D / G2 = 0x4F in kp's convention (0 symbol errors) |
| Both UW columns equal | SDM (as above) | first attempt saw column 1 differ - the Gardner loop had locked half a symbol off (three-level histogram); with the timing fixed both columns match 128/128 |
| Encoder starts and ends in state 0 | SDM Fig. 14 (garbled in the copy) | the generator reproduces every received symbol |
| Scrambling sequence: s[i] = s[i-4]^s[i-5]^s[i-6]^s[i-8], s[0..7] = 00000001 | not legible in the copy (a fragment "0 0 0 0 1 0 0 0 1" matches positions 3-11) | the idle fill at the end of frames: zero bytes come out as all-0 / all-1 groups (groups 65-159 visible); Berlekamp-Massey on those 95 bits gives linear complexity 8, the recurrence run backwards gives groups 0-64 |
| Sum check: Fletcher mod 256 over the whole packet, both sums zero | SDM: "All packets use the same 16 bit sum-check" (no algorithm) | candidate search on the bulletin boards (CRC-16 variants, Fletcher 255 / 256): only Fletcher-256 zero-sum, 8 / 8; then every packet of the recording |
| Length: type < 80h -> (T & 0F) + 1; 80h-BFh -> next byte + 2 | not public | walking the frames: all 65 packets pass; C0h+ (2-byte length) is a guess guarded by the sum check - never seen |
| Bulletin board 7D (14 bytes): network version, frame number (2 bytes), 3 bytes, origin ID, status, services (2), randomising interval | Nera service manual (field order) | frame numbers count up by 1 per frame; the origin byte decodes as 144 = NCS AOR-E (SDM Vol. 3 Ch. 3 7: 2-bit ocean region + 6-bit number, NCS x44-x63) |
| Signalling channel descriptor 6C (13 bytes): flags, uplink channel code, 28 x 2-bit slot markers | Nera service manual (field names) | the length is right for 1 + 1 + 2 + 7 + 2 bytes |
| EGC header: type, length, service, flags (bit 7 = C), message sequence number (16 bits), packet sequence number, presentation, LES ID, address (service low digit bytes), own sum check | field names from the SDM figures (web search snippets), "the second hex digit of the service code = address length" (SDM Vol. 2), "each packet, other than the last, has a continuation marker" (SDM Vol. 1 13) | the recorded SafetyNET message: header sum check at 12 bytes in all four packets, service 31h, address byte agrees with the area the message names, LES byte 4Eh = 114 agrees with the LES the message names, packet sequence 1 then 2, text continuous across the packets |
| Types 81 (19 bytes) and 92 (6-byte entries) name LESs and their TDM channel codes | not public | every LES ID always pairs with the same code; the codes give 1539.6-1540.0 MHz; type 92's entries repeat the pairs of type 81 |
| Station lists 92 / AB: [MES (3), 2 bytes] (92 only), network version, entry count, then {station, 3 status bytes, channel code} x count; entries may be NCSs; code FFFF = no TDM | not public | off air on the POR NCS (2026-11-22): the version byte equals the bulletin board's (222; 109 in the 2008 AOR-E recording), the count byte equals the number of entries in every packet, and tuning to each listed code found that very station's TDM (13 of 13, the RTL-SDR's -1.3 kHz aside) |
| Channel code -> 1510 MHz + code x 2.5 kHz | Furuno manual (NCS codes) + Wavecom / sigidwiki (NCS frequencies) | three points on one line |
| Pulse shape RRC, roll-off ~1 | not public | the recording's spectrum: -5 dB at 750 Hz, -22 dB at 1-1.5 kHz, as the alpha = 1 raised cosine |

| EGC priority = flag bits 6-5 (0 routine .. 3 distress) | the SafetyNET manual's priority per service | off air 2026-11-22: a shore-to-ship distress relay (service 14, always Distress) reads 3, a MET forecast (always Safety) 1; the 2008 METAREA V gale warning reads 2 (Urgency - not checkable). b5 as the high bit would make the forecast Urgency |
| Circular address: byte 1 = centre latitude, byte 2 = longitude, whole degrees | SafetyNET C3 form D1D2N(S)D3D4D5E(W)M1M2M3 | one real distress relay: 02, 54h = 84 against its text's 02 deg 01 min N, 084 deg 14 min E; bytes 3-4 (00 78) hold hemispheres and radius in some form - not settled |
| Service 31 address = the NAVAREA / METAREA number | as above | a second message: 0Ch = 12, a forecast for METAREA XII |
| Continued packets BD + BE | SDM: a packet overlapping the frame boundary is sent as two "continued" packets | off air: BD last in the frame, BE after the bulletin board / descriptors of the next; their payloads joined = the packet without its own last two bytes (sum check), 17 of 17 |

Not settled: bits 4-0 of the EGC flag byte, the hemisphere / radius bytes of
circular areas, rectangular areas, ITA2 packing, the meaning of the other
packet types. The decoder shows these raw and says so.

The recording is from 2008 (AOR-E NCS); the frame structure has not
changed since (the receivers in service still decode it).

## Design
- Demodulator (`demod.cpp`): acquisition first on 2048 samples (0.21 s; a
  carrier only if the squared-signal line is >= 20 dB over its noise - a
  strong carrier, as the NCS off air at 26-33 dB), then on 8192 samples
  (0.85 s, blocks overlapping by half): a zero-mean raised-cosine PSD template slid over
  +-3.6 kHz (coarse, ~1 Hz bins), then the squared signal's line within
  +-60 Hz of it, parabolic interpolation; a carrier only if the line is
  >= 11 dB over the squared spectrum 300-2400 Hz from it (noise: never in
  10 min). The block that acquired is fed through the tracker again, so
  nothing is lost. Tracking: NCO, 65-tap RRC (alpha 1), Catmull-Rom
  interpolation, Gardner (gain 0.02, clock integrator 2e-5, +-0.5 %),
  decision-directed Costas (kp 0.022, ki 2.5e-4 per symbol: ~10 Hz);
  back to acquisition after 5 s of lock quality < 0.15.
- Frame sync (`decoder.cpp`): ring of two frames of soft symbols; the UW
  correlation over its 128 symbols normalised by their energy, >= 0.5 to
  take a frame when hunting (peak within 4 symbols), >= 0.25 within +-4
  symbols of the expected place when locked, 2 misses -> hunting. The
  symbol count starts one frame "late" over zeros, so a first frame that
  began before the carrier was found decodes when at least half of it is
  there (the interleaver spreads the erasures over the code; first 60 %,
  since 2026-11-23 50 % - measured: on the real NCS (11 dB) every partial
  frame with >= 50 % decoded and none below, on generated 3-6 dB from 55 %;
  a failed partial frame is not counted as failed).
- Early recognition (`early.hpp`, 2026-11-23): for each of the 10368 frame
  phases the correlation of the unique-word rows received so far, kept up
  to date per symbol (the pair (n-1, n) is row j of the phase 162 j earlier:
  64 additions a symbol; a row's older occurrence is subtracted when the
  next frame's arrives). Every row (0.135 s): the best phase is taken when
  rows >= 16, |corr| >= 0.6 and |corr| x sqrt(2 rows) >= 7. On noise or a
  wrong signal the best of 2 x 10368 phases is the maximum of Gaussians
  (random data: of binomials); P(z >= 7) is ~1e-12 each, ~1e-3 an hour.
  Measured on everything else at hand (Aero P 600 / 1200 / 10500, C-channel
  calls, 17 carriers of the wide Aero capture, the CW line, empty offsets,
  10 min of noise; ~5500 evaluations): largest z 5.9, never a recognition.
  The floor for an honest recognition is ~22-25 rows (3 s): with 2 x 10368
  hypotheses even a perfect match of 2R hard symbols has to beat 2^-2R x
  2e4 x evaluations, which needs 2R >= ~45. Recognised -> locked on the frame
  in progress (tried if >= 50 % of it was received) or the next; the Sync
  fact counts down to it.
- Rows with no BPSK in them before the first frame (a row's (re^2 - im^2) /
  (re^2 + im^2) < 0.2: heimdall's calibration burst after a coherent retune,
  the loops re-locking) are zeroed (erasures) and restart the early search -
  else the burst's huge rows dragged every phase's correlation down until a
  frame later replaced them (recognition 9.0 s instead of 5.5 s, the same
  9.04 s live and in an offline copy with the burst put in).
- Frame decode (`frame.hpp`): de-interleave by a position table, `kp::
  viterbi_decode` (soft, terminated), re-encode to count corrected symbol
  errors, descramble, bytes LSB first.
- Packets (`packets.cpp`): the walk stops at the first zero type (idle) or
  the first packet that fails - its length can't be trusted then
  (counted: "frame(s) cut short"). No resynchronising search after a
  failure: on noise it would let 1 in ~65000 random byte runs through per
  try.
- EGC: messages keyed by LES-service-MSN (the message identity), parts
  [packet sequence][B1 / B2]; complete when the packet without C is there
  and every lower sequence number has both halves. A message is reported
  once complete, or after 180 s without a new packet as INCOMPLETE; a
  later repeat that completes it reports it again (IEC 61097-4 5.4.10).
  Memory 48 h / 1000 messages.
- Talkers: the satellite (see README). Packets rather than a transmission
  so kraken_doa filters the carrier's 3 kHz out of the VFO; 4 per frame
  from its last second (the host's 1.5 s packet ring); avg 60 s,
  keep 30 min. Considered and rejected: no talker (the VFO's DoA is the
  satellite's anyway) - but a named talker lists each satellite in the DF
  panel, which two VFOs on one satellite share, and lets the offline
  `--uca` test check the bearing.
- No map: the only geographic addresses whose layout is known are NAVAREA
  / METAREA numbers (areas without published polygons in the repo);
  circles and rectangles are not decoded (layout unknown). A circle
  centre would fit a map point; it waits for a recording that settles the
  layout.

## Measurements (2026-11-22, x86 test machine)
Real recording (sigidwiki "Inmarsat-C TDM EGC IQ", 49 s at 48 kHz, IQ
library `inmarsat/sigidwiki__Inmarsat-C_TDM_EGC`): 5 frames, 65 / 65
packets pass, 0 corrected channel symbol errors, 0.5 % of real time.
Carrier drifts +10.6 .. +21.7 Hz over the 49 s (0.25 Hz/s), sample clock
~200 ppm off (the symbol line at 1199.75 Hz): both followed.

The same recording with AWGN added (target Es/N0; 3 seeds; packets of 65):

| Es/N0 | 6 dB | 3 dB | 2 dB | 1 dB | 0.5 dB | 0 dB |
|---|---|---|---|---|---|---|
| packets passing | 65, 65, 65 | 65, 65, 65 | 65, 55, 57 | 57, 55, 55 | 55, 55, 55 | 42, 55, 53 |

(55 = all but the first, partial frame.)

Generator, AWGN, 8 files of 6 whole frames (48 frames, 400 packets),
carrier 300-650 Hz off; "ideal" = perfect carrier, timing and frame sync on
the symbols, the same Viterbi (24 frames, 408 packets):

| Es/N0 | -1 dB | -0.5 dB | 0 dB | 0.5 dB | 1 dB | 2 dB | 3 dB |
|---|---|---|---|---|---|---|---|
| frames with every packet | 12 / 48 | 24 / 48 | 39 / 48 | 48 / 48 | 48 / 48 | 48 / 48 | 48 / 48 |
| packets passing | 49 % | 71 % | 89 % | 100 % | 100 % | 100 % | 100 % |
| ideal receiver, packets | 54 % | 62 % | 83 % | | | | |

No measurable implementation loss against the ideal receiver; the
demodulator's corrected-symbol rate equals uncoded BPSK theory (8.1 %
measured at 0 dB, Q(sqrt 2) = 7.9 %; 1.6 % at 3.7 dB). Threshold Es/N0
~0.5 dB = Eb/N0 3.5 dB = C/N0 31.3 dB-Hz.

Acquisition: offsets -3600 .. +3600 Hz (every 600-1000 Hz tried, Es/N0 8
dB, drift 0.5 Hz/s): first frame decoded at its end; +-4000 Hz: nothing
(the signal leaves the 9.6 kHz input). Drift 2 Hz/s, -2500 Hz, inverted,
sample clock +-208 ppm: every frame. First frame (8.64 s after the signal
starts) decoded in 10 / 10 runs at Es/N0 0, 1, 2 dB (offsets -3000 ..
+2400 Hz).

Noise 10 min at 48 kHz and the Classic Aero 10500 baud recording: no
frame, no event; 1.4 % / 1.3 % of real time (acquisition FFTs every
0.43 s while searching).

Debug playback entries, generated with `make_testsignal.py` and decoded
offline with the playback's noise (`noise_rms`) added, 3 loops: NCS entry
11 of 12 frames (the last ends with the file), 3 / 3 EGC messages,
16 repeated packets suppressed, talker 70.0 deg (truth 70);
LES entry (Es/N0 3.7 dB) 11 of 12 frames, the group call, 200.0 deg (truth 200).
Debug playback end to end (2026-11-22, a TEST kraken_doa on ports
2918x / 2919x, no heimdall, driven over the WebSocket - `PLAYBACK_START`,
the DF status's talkers, the VFO's DoA plot (binary type 3), the decoder's
events and facts - 100 s per entry = 2.9 loops): every expect row met.

| Entry | Es/N0 / carrier (facts) | Frames | Packets | EGC | Talker / VFO DoA | Plugin CPU |
|---|---|---|---|---|---|---|
| `inmarsat-c-ncs-safetynet` | 9.6 dB, +700.0 Hz | 11 / 0 failed | 112 | 3 complete, 16 repeats suppressed | 70.0 / 70.0 (truth 70) | 0.7 % of a core |
| `inmarsat-c-les-tdm-weak` | 3.5 dB, -1500.1 Hz | 11 / 0 failed | 94 | the group call, 4 repeats suppressed | 200.0 / 200.0 (truth 200) | 0.7 % |

Two things the live path showed that the offline host can't:
- The first version reported the carrier with `host.freq_error()`: the
  host's AFC then moves the input by half the report every 300 ms - each
  step a frequency jump the ~10 Hz Costas loop can't follow; the carrier
  was lost after every frame and only 4 of 11 frames decoded (the rows
  still passed). Now no `freq_error`: the plugin follows +-3.6 kHz itself
  and gives the carrier's place to the talker packets (`freq_hz`).
- kraken_doa keeps the stream for packet talkers only 1.5 s (`RING_S`):
  of the first version's 8 packets spread over the frame only the last
  one was used. Now 4 packets from the frame's last second (44 of 44
  used). The Digital squelch's valid spans have the same limit: only the
  newest part of each frame's spans reaches the DoA.
- The decoder status flapped "Receiving" / "Signal lost" every frame (10 /
  11 times in 100 s): the engine's detection window is 4 s, frames come
  every 8.64 s. Fixed in the engine (`digital_decoder.cpp` housekeeping):
  a plugin stays detected for its `squelch_hold_s` when that is longer
  than the window (only inmarsat_c sets one that long), in the fixed-plugin
  mode and in Auto detect while no other plugin takes over. Tested with a
  build of the committed engine against one of the old engine: 0 "Signal
  lost" in 100 s in both modes; "Signal lost" 9.5 s (Auto detect 9.3 s)
  after the last frame of a signal that ends; aprs (no long hold) lost
  4.0 s after its last packet on both engines. Not exercised: another
  plugin taking over while inmarsat_c is held (*Digital decoders*).
- L band and the array: bearings are unambiguous only with neighbouring
  antennas under half a wavelength apart - five antennas at 1541 MHz: a
  radius under about 8.3 cm. Both playback entries use the default 12 cm
  scene (14 cm between antennas) and still give the true bearing for one
  clean source; a real 12 cm array can show false bearings beside it
  (README *How to receive*).

## Uplinks (ship / land terminals, 1626.5-1646.5 MHz)
The one Inmarsat-C transmitter worth a bearing. What the published SDM
Volume 1 gives: the signalling channel - slotted ALOHA bursts in 14 / 28
slots of an 8.64 s frame (600 / 1200 symbols/s), each a 64-symbol unique
word + 252 coded symbols (a 15-byte packet + 6 flush bits, the same K=7
code), the packet XORed with a 15-byte template (Fig. 13 - illegible in the
copy: "82 DA 9A AF 8B B0 F1 86 6F ..." in unknown order); the message
channel - a preamble (128 symbols of 1s, 64 of 0101...), the unique word
and the TDM's interleaving with 34 + 32 N columns, packets of 127 bytes.
Missing: the template order, the signalling packet layouts (Volume 4 Ch.
4: where the terminal's return ID sits), and any recording of an uplink
burst. A demodulator and frame decoder could be built from the TDM code
(same code, UW, interleaver), but without the packet layout no ID - so no
talker (a burst without an ID can't be told from another terminal's) -
and without a recording nothing could be tested. Not implemented.

## Survey: the other Inmarsat L-band services (2026-11)
Classic Aero is the other worker's plugin (`plugin-inmarsat-aero.md`).

| Service | On air? | Public spec / permissive reference | Clear? | Bandwidth, modulation | Verdict |
|---|---|---|---|---|---|
| Inmarsat-C / mini-C TDM (NCS, LES) | yes (GMDSS; no closure date found) | SDM Vol. 1-3 (unofficial copy), IMO SafetyNET manual | yes (EGC, signalling) | ~2.4 kHz, BPSK 1200 sym/s | **decoded here** |
| Inmarsat-C uplinks (signalling bursts, message channel) | yes | SDM Vol. 1 PHY; Vol. 4 layouts missing | yes | ~2.4 kHz, BPSK 1200 sym/s | not decoded: no packet layout, no recording (above) |
| Inmarsat-D / D+ / IsatM2M | IsatM2M withdrawn end 2021 (Polestar notice) | none | - | - | not decodable: gone, never published |
| IsatData Pro (now Viasat IoT Nano / ORBCOMM OGx) | yes | none (proprietary, SkyWave / ORBCOMM) | unknown | narrowband | not decodable without a spec; would need reverse engineering from recordings (none) |
| Inmarsat-A / B / M / mini-M | closed (B and mini-M 30 Dec 2016; A and M earlier) | - | - | - | nothing on air |
| Fleet 33 / 55 / 77, Swift 64 | closed (Fleet 33 / 55 2018, Fleet 77 1 Dec 2020; Swift 64 retired with the classic services - date not confirmed) | - | - | - | nothing on air |
| BGAN / FleetBroadband / SwiftBroadband (Family SL) | yes | ETSI TS 102 744 series (2015, free) | user traffic ciphered (UMTS-derived); system information in the clear | forward bearers up to ~200 kHz (QPSK / 16QAM, turbo code) | receivable within 2.4 MHz; the clear control / system information is decodable in principle from the ETSI spec, but it is a project of its own (thousands of lines: bearer structures, turbo decoder) with no recording here - not started. Ciphered traffic is never decoded |
| GSPS / IsatPhone (GMR-2 family) | yes (IsatPhone 2) | ETSI TS 101 377 (GMR-2) | voice / SMS ciphered (A5-GMR-2, broken in the literature - not decoded); broadcast control channels clear | narrow TDMA carriers | system information / paging decodable in principle from the ETSI spec, a further plugin; handset uplinks (1626.5-1660.5 MHz) would be DF targets by their burst timing, IDs mostly temporary. Not started, no recording |
| SBAS on Inmarsat satellites | EGNOS left Inmarsat-4 F2 (PRN 126); 2025 operational GEOs are SES-5, Eutelsat 5WB (Astra 5B in test) - no Inmarsat SBAS payload confirmed in service | ICAO SARPs; RTKLIB (BSD-2) decodes SBAS messages | clear | 1575.42 MHz, 1.023 Mchip/s BPSK, 250 bit/s K=7 | a GNSS-style correlator plugin (code search over Doppler, the same Viterbi, CRC-24Q) would fit 2.4 MHz - but it is no longer an Inmarsat signal; out of scope |
| TT&C, beacons | yes | - | - | TT&C in C-band (out of the tuner's range); no published L-band beacon content | nothing to decode |
| Fleet Safety (maritime safety over FleetBroadband, 2022) | yes | not public | rides on FBB bearers | as BGAN | not decodable |

Sources for the table: UK MCA MIN 617 (Fleet 77), Inmarsat legacy closure
notice (MacKay, 2016), satphonestore (Fleet EOL), Polestar (IsatM2M EOL),
Viasat IoT Nano news (2025-07), EGNOS service notice 33 (2025-08), GPS.gov
PRN assignments (2026-01), ETSI TS 102 744 / TS 101 377 catalogue pages,
the Ruhr University Bochum GMR cipher paper (2012). Statuses not
confirmed by an official notice are marked so.

## Off air on the roof Pi 5 (2026-11-22)
KrakenSDR CH0, a satellite patch with its LNA on the CH0 bias tee, gain 49.6
dB, coherent mode, the stack from the integration branch; the plugin built
there with gcc 14.2 / -mcpu=native without a warning. The README has the
numbers (46 min of the POR NCS 244: 249 frames, 3 failed - all at heimdall
calibration gaps; Es/N0 9-13 dB; every listed LES TDM decodes; no EGC packet
in that hour). What the live signal showed that the recordings had not:
- **Samples cut out.** heimdall's calibration bursts (noise source on, ~5
  min after a retune) are cut out of the stream, and a capture written to
  the SD card lost 0.14 s once (its reader fell behind). The frame grid then
  jumps (-164 and -8 symbols measured); the locked tracker looked only +-4
  symbols around the expected place and needed two misses plus a whole new
  frame (25 s without frames). Since 0ab82be the unique-word search runs
  while locked too and takes the next whole frame at once (8 s).
- **Two signalling channels per frame** (two 6C descriptors), a standby NCS
  (245: version 0, only the bulletin board and descriptor), and station lists
  that include NCSs - the fact now lists every station with its channel.
- **A CW line** at 1541.4139 MHz as received (1541.4152 corrected for the
  RTL-SDR's error): unmodulated (no sidebands above 1.5 Hz resolution within
  +-6 kHz), at the same RF with the receiver centred 200 kHz away (not a tuner
  spur); not identified - a satellite pilot or a fixed local emitter.
- The frame numbers' times agree with UTC; carriers drift with the RTL-SDR
  (-1356 -> -1302 Hz in an hour), which the plugin follows without help.
- **EGC on the air** (three messages after an hour without any): the
  priority bits, a circular area's centre degrees and the BD + BE continued
  packets came from them (table above); the first decoder version dropped
  every continued packet - 15 of the forecast's 32 halves - so the message
  never completed. Bits 4-0, the circle's hemispheres and radius,
  rectangles and ITA2 are still raw.

## Recognition time (G2, 2026-11-23)
The user saw the delay between tuning and the decoder recognising the
signal. Where the seconds went (before):

| Part | Time | Notes |
|---|---|---|
| receiver, independent mode | ~0.2 s | samples of the new frequency reach the plugin at once |
| receiver, coherent mode, after a retune | 3.6-4 s cooldown, then a ~0.5 s calibration burst with samples cut (-16 symbols measured) at ~4.7 s | heimdall's (not changed); only moving the VFO or switching the decoder on avoids it |
| plugin start / `reset()` | 0.0-0.3 s | the decoder switched on: plugin started within 0.3 s; `reset()` restarts acquisition and sync (nothing kept: a retune can't keep the carrier) |
| acquisition | 0.85 s (now 0.21 s for strong carriers) | the block is fed through the tracker again: no symbols lost |
| Costas / Gardner settling | < 0.1 s | invisible in the frame results |
| unique word | a whole frame before (the frame had to end) | now 3.4 s of symbols (25 rows) at 9-13 dB, ~3.5-4.5 s at 1-3 dB |
| the frame's end | U(0, 8.64 s) + a frame when < 60 % (now 50 %) received | the floor: 8.64 s on average |

Offline distributions (decoder started every 0.25 s; README table): first
frame mean 9.3 -> 8.3 s on the NCS, 9.5 -> 8.6 s on a second capture, 8.7
-> 8.3 s on a LES TDM (where 3 of 29 starts got none within 13 s before);
recognised now at 3.5 s, the first frame announced correctly.

Live (roof Pi, NCS 244 and LES 217, before / now, see the README for the
lines): independent - recognised 3.4-3.8 s, the countdown right to 0.1 s;
coherent retune - recognition after heimdall's burst, 7.3-9.4 s, but 36.7 /
39.7 s in 2 of 7 (1 of 5 before: nothing in 28 s) when heimdall's
recalibration slipped samples and re-acquired the lags (heimdall's log) -
nothing the decoder can shorten. Single
samples of the first frame vary by the frame phase (0-8.64 s); only the
offline distributions show the mean.

Kept: sensitivity (100 % of packets at Es/N0 >= 0.5 dB, 416 / 416), the
+-3.6 kHz acquisition, the resynchronisation after a gap (32 / 2 on the
capture with a calibration gap), every real capture's counts, both
playback entries offline (70.0 / 200.1 deg), nothing on noise / other signals.

An "all TDMs in the VFO" mode (the NCS and its LES TDMs side by side, as
the Aero plugin's all-channels mode): feasible, not built. What it would
take: `.sample_rate = 0`, a shared channelizer (the TDMs sit on the 2.5 kHz
raster; POR's eleven span 1539.50-1539.73 MHz, the NCS 1.7 MHz away - a
2.4 MHz VFO holds them all), one demodulator + frame sync + packet / EGC
state per carrier (the per-carrier state moved out of decoder.cpp into a
class; ~0.5 % of a Pi 5 core each), and the station lists (types 92 / AB)
give every TDM's exact channel code, so channels open without a search
once rf_hz is known (the -1.3 kHz RTL-SDR error then found once from the
NCS). Facts / table per channel are the larger part. A few hundred lines
plus a channelizer - one shared with the Aero plugin's would be the way.

## Test history
- 2026-11-22: first version. The research pass found Vol. 1-3 of the SDM
  (no Vol. 4); everything else measured (table above). Bugs met: Gardner
  error sign (locked half a symbol off - three-level eye, the second UW
  column looked like data); `fmt()` with an int for `%02X` (garbage in the
  table details); the first frame lost after acquisition (fixed by the
  erased-start frame). The talker per satellite added after the playback
  entries needed a bearing check offline.
