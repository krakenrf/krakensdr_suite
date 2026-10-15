# LoRa / Meshtastic / LoRaWAN decoder (`lora`)

Decodes LoRa chirp-spread-spectrum packets in the whole 2.4 MHz the tuner
receives - in any country - reads the Meshtastic and LoRaWAN layers carried
on top, and gives every transmitter its own bearing in the 🗺 Map's 📡 DF
panel.

`sample_rate` = `min_vfo_rate` = 2.4 MHz, `lock_vfo` (the VFO sits on its
tuner's centre with the full bandwidth; tune the receiver to the band),
`manual_only`, `talkers`, `map`, `tunes`.

## Where it listens: region-neutral by default

Three layers, from cheap to general (option defaults in brackets):

1. **Plan channels** - `region` (Meshtastic, **Auto**) and `lorawan`
   (LoRaWAN plan, **Auto**): Auto = every Meshtastic region and every
   LoRaWAN regional plan whose band holds the tuned frequency, all their
   channels that fall inside the VFO (902-928 MHz: Meshtastic US + ANZ,
   LoRaWAN US915 + AU915 + AS923-1..4; 920-923 MHz also KR920 / JP / KR /
   TW / TH / MY_919 / SG_923 ...). A specific region / plan can be chosen
   instead (then only it), `0` / `off` switches a layer off. Receivers
   (channel x SF x IQ) are added by priority - explicit `channels`, learned
   channels, the primary Meshtastic preset's channel, LoRaWAN downlinks /
   RX2, class-B beacons, the other presets' default channels, LoRaWAN
   uplinks (nearest the VFO centre first) - within the `cpu` budget (a
   receiver costs ~ bw x SF, a channel ~ bw x 5.3 log2(2 bw / 520.8 Hz) -
   its inverse FFT); what doesn't fit is named in the Watching fact and left
   to the search.
2. **The plan-free search** (`search`, **on**): LoRa ANYWHERE in the VFO -
   any centre, SF5-12, both IQ orientations, BW 62.5 / 125 / 250 / 500 kHz
   (`all`: also 7.8 - 41.7 kHz, SF5-10). See *Plan-free search* below.
3. **Learned channels**: a transmitter the search decoded gets a receiver
   of its own on its exact frequency / SF / BW / IQ (strict no-CRC rule),
   kept 30 min after it was last heard (<= 8), so its next packets decode
   like a plan channel's - even below the search's energy threshold.

The old default (`region=anz`, AU915 only) is gone; a saved `anz` / `us` /
`eu868` region still works (Meshtastic only - LoRaWAN stays Auto).
`tune` retunes the receiver ONCE, only with a chosen Meshtastic region and
only when the receiver is outside that region's band - Auto never retunes
(a restart must not pull a receiver the user tuned away).

### Region tables (`regions.hpp`)

**Meshtastic** - the firmware's region table (`src/mesh/RadioInterface.cpp`,
spacing 0 everywhere), preset table (`src/mesh/MeshRadio.h`) and preset
display names (`src/DisplayFormatters.cpp` - the default channel hashes
the name; LONG_MODERATE is "LongMod", not "LongModerate" as the first
version had), checked 2026-10-16 against firmware master:

| Region | MHz | Region | MHz | Region | MHz |
|---|---|---|---|---|---|
| US | 902-928 | EU_433 | 433-434 | EU_868 | 869.4-869.65 |
| CN | 470-510 | JP | 920.5-923.5 | ANZ | 915-928 |
| ANZ_433 | 433.05-434.79 | RU | 868.7-869.2 | KR | 920-923 |
| TW | 920-925 | IN | 865-867 | NZ_865 | 864-868 |
| TH | 920-925 | UA_433 | 433-434.7 | UA_868 | 868-868.6 |
| MY_433 | 433-435 | MY_919 | 919-924 | SG_923 | 917-925 |
| PH_433 | 433-434.7 | PH_868 | 868-869.4 | PH_915 | 915-918 |
| KZ_433 | 433.075-434.775 | KZ_863 | 863-868 | NP_865 | 865-868 |
| BR_902 | 902-907.5 | (LORA_24 2400-2483.5: outside an RTL-SDR's range) | | | |

Presets: ShortTurbo SF7/500, ShortFast SF7/250, ShortSlow SF8/250,
MediumFast SF9/250, MediumSlow SF10/250, LongFast SF11/250, LongTurbo
SF11/500 (CR 4/8), LongModerate SF11/125 (4/8), LongSlow SF12/125 (4/8).
Channel = the firmware's slot: numChannels = floor((end - start) / bw),
slot = djb2(channel name) % numChannels, f = start + bw/2 + slot x bw - the
default channel's name is the preset's. E.g. LongFast: ANZ 919.875, US
906.875, EU_868 869.525, IN 865.875, EU_433 433.875, ANZ_433 434.425 MHz.
`channel` = a named primary channel (its hash slot), `others` (on) = every
other preset's default channel inside the VFO too, `sf` = SFs on the
primary channel. Sync word 0x2B, 16-symbol preamble.

**LoRaWAN** - LoRa Alliance Regional Parameters RP002-1.0.x, cross-checked
with the region headers of Semtech's LoRaMac-node (BSD):

| Plan | Uplink (+ RX1 where both) | Downlink / RX2 | Beacon |
|---|---|---|---|
| EU868 | 868.1 / 868.3 / 868.5 + 867.1-867.9, SF7-12 BW125; 868.3 SF7 BW250 | RX1 = uplink, RX2 869.525 | 869.525 SF9, 17 B |
| US915 | 902.3 + 0.2 n (64) SF7-10 BW125; 903.0 + 1.6 n SF8 BW500 | 923.3 + 0.6 n SF7-12 BW500 | 923.3 + 0.6 n SF12 BW500, 23 B |
| AU915 | 915.2 + 0.2 n (64) SF7-12 BW125; 915.9 + 1.6 n SF8 BW500 | 923.3 + 0.6 n SF7-12 BW500 | as US915 |
| AS923-1 | 922.0-923.4 (923.2 / 923.4 default) SF7-12; 922.1 SF7 BW250 | RX1 = uplink, RX2 923.2 | 923.4 SF9, 17 B |
| AS923-2 / -3 / -4 | AS923-1 - 1.8 / - 6.6 / - 5.9 MHz | | 921.6 / 916.8 / 917.5 |
| KR920 | 920.9-923.3 (922.1-922.5 default), SF7-12 | RX1 = uplink, RX2 921.9 | 923.1 SF9, 17 B |
| IN865 | 865.0625 / 865.4025 / 865.985 SF7-12 | RX2 866.55 | 866.55 SF8, 19 B |
| RU864 | 868.9 / 869.1 SF7-12 | RX2 869.1 | 869.1 SF9, 17 B |
| CN470 | every plan (legacy, 20 / 26 MHz A / B) on 470.3 + 0.2 n, SF7-12, both directions | | plan-dependent: not watched |
| EU433 | 433.175 / 433.375 / 433.575 SF7-12 | RX2 434.665 | 434.665 SF9, 17 B |
| CN779 | 779.5 / 779.7 / 779.9 SF7-12 | RX2 786.0 | 785.0 SF9, 17 B |

Downlinks use inverted IQ ("both" plans: every channel watched both
ways). Network-configured extra channels are covered by the common grids
above - anything else by the search.

**Class-B beacons** (`beacons`, on): implicit header (length known from
the plan), CR 4/5, no PHY CRC, preamble 10; layout RFU1 | Param | Time (4,
LSB first) | CRC1 | GwSpecific (InfoDesc + 6) | RFU2 | CRC2, both CRC-16
0x1021 init 0 stored LSB first (LoRaMac-node's parser). RFU1's length
differs per region: every split is tried and BOTH CRCs must hold. The talker
is the gateway's channel (`GW 869.5`); InfoDesc 0-2 = the gateway antenna's
position (shown, lat x 90 / 2^23, lon x 180 / 2^23). Beacons are sent by
every gateway of a network at the same instant: the bearing is the
strongest one's. Other implicit-header links (length unknown here) are
reported by the search as "preamble only" (below).

## Plan-free search

Stage 1 (`search.hpp`, ~2-3 % of a core): the channelizer's forward FFTs
(already computed: 520.8 Hz bins every 1.44 ms) -> power per bin over 11.5
ms groups, divided by a per-bin floor (slow, gated EMA: the RTL passband
shape, spurs and DC drop out); per bandwidth a window of 4-12 groups
(>= one SF10 symbol), centres every B/8: a candidate stands >= 9 sigma above
the noise, is FLAT inside (the four quarters within 2.5x - a chirp sweeps
the band evenly; CW / FSK tones fail), BAND-LIMITED (just outside < 30 % of
inside - so the B it fits is its bandwidth: a 125 kHz test inside a 500
kHz signal fails), a local maximum over the centres, reported once on its
rising edge, the clearest first.

Stage 2 (probes, `decoder.cpp`): the candidate's channel is cut out of the
last 0.8 s of FFT blocks (kept, 20 MB) - so the preamble that set it off is
there - with a CSS receiver for every SF / IQ orientation no channel there
already has, scanned at once; then:
- a frame decoded -> a LEARNED channel at the measured frequency (rounded
  to 1 kHz), the receiver that decoded stays (`🔎 LoRa found by the search:
  926.901 MHz SF7 BW250 sync 0x12 - now a watched channel`);
- a packet still arriving -> only that receiver keeps running;
- a preamble + SFD with a sync word but no valid header (implicit header,
  collision, a CR / LDRO it doesn't use) -> reported as an undecoded LoRa
  signal (frequency / SF / BW / IQ / sync word, a talker from the preamble's
  samples) once it was seen AGAIN at the same place within 10 min with most
  of the preamble window's energy in its bin (a one-off chance match of a
  non-LoRa burst - FSK data dechirps into a peak now and then, at SF5 / SF6
  more often - is not reported);
- nothing -> that place / bandwidth backs off 2 s doubling to 60 s.
No probe where a plan / learned channel of the same bandwidth sits (its
signal is being decoded). Cost control: <= 6 probes a second, <= 4 at once,
and the probes' CPU budget: the plugin's OWN thread CPU time
(CLOCK_THREAD_CPUTIME_ID) per second of INPUT, 12 % of a core (`cpu` low 4
% / high 25 %; <= 0.3 s banked). Over it the probes are SLOWED - at least
one every 2 s still runs - and the Search fact says "SLOWED to its CPU
budget (N candidates waited)". It used to time itself on the wall clock and
stop probing above the budget: on a busy Pi (load 3-5: the live stack + a
scratch kraken_doa) the preempted time counted as its own, it "paused (CPU
budget)" for good and the debug playback lora-private-search found 1 of its
5 links (now 5 / 5 under load 5-7). A probe costs ~5-30 ms of CPU (0.15-0.8
s of history x 16 receivers).
Frames found by the search without a payload CRC need (almost) every
payload codeword's parity to hold.

Sensitivity: stage 1 is an energy test - at 125 kHz a few dB below the
noise in the band, less for short or wide bursts; weaker transmitters are
decoded once learned, and on plan channels. The narrow bandwidths
(`search=all`) are best effort: at SF > 10 a symbol is longer than the
window, and the 0.8 s of history holds only a few symbols.

## What it decodes

**LoRa PHY**: SF5-12 (SF5 / SF6: SX126x), BW 7.8 / 10.4 / 15.6 / 20.8 /
31.25 / 41.7 / 62.5 / 125 / 250 / 500 kHz, CR 4/5 - 4/8, explicit and
implicit header, low-data-rate optimisation (auto: symbol >= 16 ms), normal
and **inverted IQ**: one FFT channelizer (`channelizer.hpp`: 4608-point
overlap-save of the 2.4 MHz, per channel the bins around it + an inverse
FFT -> exactly os x its bandwidth, os 2 - 7.8 kHz: 4, zero delay) feeds a
CSS receiver per SF and IQ orientation that share the channel's sample ring
- cheap dechirped preamble detection (one chip per sample, a window every 2
symbols, 3 on one bin - SF5 / SF6: 4, and a higher peak threshold), up /
down-chirp sync (integer CFO / STO split + fractional CFO de-rotation),
symbol demodulation with the fractional offset TRACKED per symbol, then the
coding chain (`coding.hpp`). Frames with a payload CRC are reported only
when it passes; frames without one (LoRaWAN downlinks) when the header
checksum and all eight header codewords pass - with the count of payload
codewords whose parity failed. **SF5 / SF6**: sf - 2 bits can't carry the
five header nibbles in a block, so the header block is read at the full
rate, with or without two extra symbols after the SFD (both tried; the
checksum + codewords decide) - this SX126x convention is NOT verified on a
real radio here (none at hand). Narrow bandwidths need a stable carrier: a
CFO beyond ~-20 % of the bandwidth (at 7.8 kHz: 1.5 kHz, ~2 ppm at 900
MHz) is outside what a channel at the nominal frequency sees - put the
channel on the measured frequency.

**The coding chain is the radios'** (since 2026-10-15, checked on real AU915
gateway downlinks): values are `(bin - 1)` relative to the preamble (reduced
rate: `round((bin - 1) / 4)`), Gray MAPPED (`v ^ v >> 1`), deinterleaved
`cw[k] bit (cw_len-1-i) = bit ((k - i) mod sf_app) of symbol i` with the
codeword's first bit d0, Hamming 4/5 = even parity of all four bits, the
whitening LFSR `0xFF, w' = (w << 1) | (w7^w5^w4^w3)` over the payload only,
the CRC = CRC16-CCITT of the first n-2 bytes XOR the last two, and the
SX127x header checksum.

**Meshtastic**: the clear 16-byte `PacketHeader` (to, from, packet id, flags =
hop limit / hop start / want-ack, channel hash, next hop, relay node). The
payload is decrypted **only with the published default channel key**
(`d4f1bb3a20290759f0bcffabcf4e6901`, the public "AQ==" / PSK 0x01 key the
Meshtastic project documents - anything sent with it is readable by anyone)
with AES-128-CTR (nonce = packet id little-endian + sender node + block
counter). The `Data` protobuf is parsed for `portnum` and the common ports:
TEXT_MESSAGE (public channel text), POSITION (map points), NODEINFO (long /
short name, hardware), TELEMETRY, ROUTING. A packet on any **other** channel
is **not** decrypted - only its clear header is shown. Private keys are never
guessed or brute-forced.

**LoRaWAN**: uplinks, gateway downlinks and class-B beacons - the clear
MAC header only: MHDR, DevAddr, FCtrl / FCnt / FPort for data frames,
DevEUI / JoinEUI / DevNonce for join requests, the MHDR of a join accept.
The FRMPayload is encrypted with keys we do not have and the MIC cannot be
checked, so nothing is decrypted - headers are shown. A frame's direction
must match its IQ (uplinks normal, downlinks inverted); on a channel that
is not a LoRaWAN plan channel (explicit / learned) it also needs the public
sync word 0x34. Downlinks carry no payload CRC, and with CR 4/5 a symbol
error can't be corrected: when a codeword of the first 9 bytes (MHDR ..
FPort) failed its parity the DevAddr is shown with a "?" and no event is
logged for it.

**Other LoRa** (private links, proprietary systems): the PHY payload as
hex in the raw log, length, CRC / parity state, sync word.

Meshtastic vs LoRaWAN is decided by whether the default-key decrypt yields a
valid `Data` protobuf (or the header is a broadcast with sane hop fields);
otherwise the frame is read as LoRaWAN, else as raw LoRa.

## ID separation (a DoA per transmitter)

Every decoded frame is a `kp::Talker` packet of the transmitter that actually
sent **these** samples, on its own channel (`freq_hz` / `bw_hz` = the LoRa
bandwidth), so transmitters on different channels / SFs at the same moment
keep their own bearings:

- **Meshtastic**: the node id (`!xxxxxxxx` from `from`). When
  `hop_start != hop_limit` the frame was **relayed** - the transmitter heard
  is the relaying node, so the talker is `!relay-XX` (XX = the header's
  `relay_node` byte, the last byte of the relayer's id), not the originator.
- **LoRaWAN uplinks**: the DevAddr (data frames) or DevEUI (join requests).
- **LoRaWAN downlinks and beacons** are sent by the GATEWAY: talker named by
  its channel (`GW 927.5`), never the DevAddr's device.
- **Other LoRa** (no transmitter ID in the frame): its channel + SF + sync
  word (+ " inv"), e.g. `LoRa 926.901 SF7 0x12` - the channel's nominal /
  learned frequency, so one transmitter keeps one ID (the first version
  split one sensor into `lora 926.997/8` and `lora 926.998/8`). Several
  devices sharing that channel / SF / sync word share the ID.
- **Undecoded LoRa found by the search**: the same form, from the preamble's
  samples (row "preamble only - header not decoded").

`avg_s` 10, `keep_s` 30 min. UI kind `node` (📶), with Dist / position
columns for nodes that send POSITION.

A strong packet also decodes on a watched channel one bandwidth away (a
whole-bandwidth offset aliases to CFO 0 after dechirping) or 100 kHz away (a
neighbour channel of another plan): frames ending within 0.15 s with the
same payload (no CRC: the same length and >= 80 % equal bytes) are one
transmission; the strongest copy is kept, a plan / learned channel's over a
probe's.

## Tests

**Real signal** (2026-10-15, the user's array at 927.500 MHz, a 120 s
channel-0 capture from heimdall's 8091, scratch lora2/): AU915 gateway
downlinks on 927.5 MHz (SF10 / SF7 BW500, inverted IQ, sync 0x34, DevAddr
00F6D6E1 FPort 204 every 0.8 s) and 926.9 MHz, and a sync-0x12 sensor (2-byte
frames with CRC, SF8 BW125, ~926.997 MHz). With the defaults (Auto) the
plugin decodes the same 63 frames as before (51 downlinks: GW 927.5 47, GW
926.9 4; LoRa 927.000 SF8 0x12 12) PLUS what the search found:

- **The "SF9 BW500 normal-IQ bursts" on 926.9 / 927.4 MHz** (headers never
  validated) are **SF7 BW250** - the same chirp slope (BW^2 / 2^SF: 250^2 /
  128 = 500^2 / 512 kHz^2), so an SF9 / 500 kHz receiver dechirped them
  cleanly but read garbage symbols. A private LoRa system: sync 0x12, explicit
  header WITHOUT payload CRC (every codeword's parity holds), 21 / 101 / 231-
  byte frames that all start `00 00 fd 1d` (the 21-byte one `... 55 55 00 40
  00 .. aa aa 02 57` - a beacon / sync frame), alternating 926.900 / 927.400
  MHz ~0.5 s apart, every ~15 s. Not LoRaWAN (no 0x34, no CRC), not
  Meshtastic (0x12). The search learned both channels at their first
  decodable packets: `LoRa 926.899 SF7 0x12` and `LoRa 927.401 SF7 0x12`,
  41 more frames in the 120 s (104 in all; in the slice 49-52 s all 6
  bursts). When the search first decodes one depends on its probe budget in
  the busy band (926.9 at 3 s, 927.4 at 33-50 s in different runs).
- no false frames, no undecoded-LoRa reports (FSK / OOK bursts of the busy
  band were probed and backed off).

**Synthetic scenes** (`tools/lora_gen.py`, 5-element UCA r 0.12 m, every
antenna, `decoder --file x.cu8 --channels 5 --uca 0.12 --rf F`, defaults =
Auto + search), every talker's bearing vs the truth:

| Scene | RF | Plans Auto picked | Talkers (truth deg) | Offline bearings | CPU |
|---|---|---|---|---|---|
| scene_au915 | 927.5 | Meshtastic US, ANZ; LoRaWAN US915, AU915, AS923-x | 6 (GW 927.5 140, GW 926.9 250, 4 DevAddr 40 / 300 / 200 / 80) | all exact, GW 926.9 250.6 | 54 % |
| scene_five (explicit channels) | 921.8 | - | 6 incl. a relay, a private channel | all exact | 37 % |
| scene_eu868 | 868.8 | EU_868 + 6 other Meshtastic regions; EU868, RU864 | mesh 869.525 50, DevAddr SF12 130 / SF7 210, GW 869.5 (RX2 downlink + beacon) 290 | all exact; beacon CRCs + position | 62 % |
| scene_us915 | 906.0 | US, BR_902; US915 | mesh 906.875 40, DevAddr SF10 120 / SF7 200 | all exact | 52 % |
| scene_as923 | 923.0 | 8 Meshtastic; US915, AU915, AS923-1..4, KR920 | DevAddr 60, GW 923.4 (RX1 + beacon) 150, DR6 SF7 BW250 240, mesh KR 922.875 320 | all exact (the DR6 channel learned by the search) | 55 % |
| scene_in865 | 865.8 | IN, NZ_865, KZ_863, NP_865; EU868, IN865, RU864 | DevAddr SF12 30 / SF8 100, GW 866.5 (RX2 + 19-byte beacon) 170, mesh 865.875 250 | all exact | 49 % |
| scene_433 | 433.9 | EU_433, ANZ_433, UA / MY / PH / KZ_433; EU433 | mesh EU_433 433.875 70, ANZ_433 434.425 160, DevAddr 260 | all exact | 61 % |
| scene_private (no plan at 446 MHz) | 446.0 | none | SF9/125 0x12 45, SF5/125 300, SF6/250 330, SF10/62.5 sync 0x56 inverted 135, SF7/500 225 | all 5 found + learned, all exact, 27 of 32 packets (the first of each is the one found; the low SFs' learned frequency is ~2 FFT bins off: 445.504 / 445.805 / 446.508) | 21 % |
| scene_narrow, explicit channels | 446.0 | - | SF8 BW15.6 100, SF7 BW7.8 200, SF9 BW41.7 300 | all exact (13 / 13 frames); `search=all` instead: only the 7.8 kHz one found | 13 % |

CPU = offline decode time / real time on the Pi 5 (incl. the host's file
read and the per-talker bearing work; live ~0.65x of it, see below).

Playback entries (`tools/testsignals.json`): `lora-mesh-five`,
`lora-au915-gateway`, `lora-eu868`, `lora-private-search`.

**First-version tests** (still valid for the parts they cover): PHY round
trip SF7-12 x CR x BW incl. CFO / STO / noise (71 / 72; SF12 to ~-12
dB/sample); byte-exact check of the whole path (16 / 16); bearings exact.

## CPU (Pi 5)

Offline on the real 927.5 MHz capture: 44 % of real time before (12
channels, 55 receivers), now (the same plan channels + 2 beacon
receivers + the search ~5-9 % + 2 learned channels: ~64 %); live the plugin runs
at ~0.65x its offline figure plus the input pipe. `cpu=low` drops plan
receivers past ~120 M (bw x SF units) and holds the search to 4 %. Live
(2026-10-16, the user's stack around 924.5 MHz, 14 plan channels + a learned
one, 30 receivers over the budget): ~40 % of a core incl. the input pipe;
RSS 363-375 MB with rings sized for the longest packet (32 MB per 500 kHz
channel) and 20 MB of float FFT history; now ~110 MB (peak 117 MB, CPU unchanged ~40-45 %). A channel's ring now
holds only the sync / header stage (60 symbols of its largest SF: detection
lookback, a 16-symbol preamble, sync, SFD, header); the payload is
demodulated block by block as it arrives (`LoraRx::continue_packet`). Rings
are calloc'd (a probe's ring is committed only as far as it is written), the
FFT history is int16 with a scale per block (10 MB), <= 8 learned channels, and the mmap threshold is fixed at 256 KB (glibc raised it to the first freed ring, after which rings came from the heap, zeroed and kept).
The detection hop grid keeps its phase across packets (a first version of
the block-by-block demod moved it and the real capture lost 3 of its 63
frames - their CFO / timing split depends on it). Live it decoded the gateway
downlinks on 923.9 / 924.5 / 925.1 MHz, the private SF7 BW250 0x12 system on
923.875 / 924.875 MHz, a sync-0x12 SF8 sensor on 924.000 and learned a
further SF7 BW250 0x12 channel on 924.401 MHz. Known: a frame whose sync
word is mis-measured (e.g. 0xB2 for 0x12) still makes its own talker.

## Limits / sources

- 2.4 GHz LoRa (SX128x, Meshtastic LORA_24) is outside the RTL-SDR's range.
- Encrypted payloads stay encrypted: Meshtastic only with the published
  default key, LoRaWAN FRMPayload / join accepts never (keys not public).
- Several packets on the SAME channel overlapping in time cannot both be
  decoded (one receiver per channel); collisions are lost.
- Implicit-header frames decode only where the length is known (LoRaWAN
  beacons); other implicit links show up as undecoded LoRa.
- SF5 / SF6 header convention not verified on a real SX126x.
- A carrier offset beyond about -20 % of the bandwidth sometimes costs a
  symbol.
- CN470's beacon channel depends on the plan in use: not watched.

Protocol knowledge is clean-room from public documentation: the LoRa PHY
reverse-engineering literature (Knight; Robyns et al.; Tapparel, "Complete
Reverse Engineering of LoRa PHY", EPFL; Semtech AN1200.22 / SX127x / SX126x
datasheets), the Meshtastic firmware's region / preset tables and protocol
documentation (meshtastic.org), the LoRaWAN MAC specification and the LoRa
Alliance Regional Parameters RP002-1.0.x, Semtech's LoRaMac-node region
headers and class-B beacon parser (BSD, facts only). GPL projects (gr-lora,
gr-lora_sdr, rtl_433, the Meshtastic firmware) and LoRaPHY were read only as
documentation / test reference, never copied.
