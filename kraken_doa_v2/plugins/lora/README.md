# LoRa / Meshtastic / LoRaWAN decoder (`lora`)

Decodes LoRa chirp-spread-spectrum packets in the whole 2.4 MHz the tuner
receives - in any country - reads the Meshtastic and LoRaWAN layers carried
on top, and gives every transmitter its own bearing in the 🗺 Map's 📡 DF
panel.

`sample_rate` = `min_vfo_rate` = 2.4 MHz, `lock_vfo` (the VFO sits on its
tuner's centre with the full bandwidth; tune the receiver to the band),
`manual_only`, `talkers`, `map`, `tunes`, `multichannel` (every antenna:
separation by code, below).

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
  more often - is not reported). Never reported either (2026-10-16): a
  preamble inside / next to (50 ms, an eighth of its bandwidth) a packet
  decoded - or being decoded on another channel - at the same time in the
  same band, and sync word 0x00 (both sync symbols on the preamble's own
  bin): ALIASES of a real packet, not other transmitters - a chirp with the
  same slope BW^2 / 2^SF (SF11 / 250 kHz dechirps like SF9 / 125 kHz and
  SF7 / 62.5 kHz) or a piece of one through the probe's channel edge. The
  playback lora-mesh-five showed five such "LoRa 921.733 SF9 0x00" talkers
  at the real nodes' bearings after ~14 min; the Search fact counts them as
  "aliases of decoded packets left out";
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

`avg_s` 10, `keep_s` 30 min. UI kind `node` (📶; list columns Node / Name ·
packet / Frames), with Dist / position columns while a node has sent a
POSITION (nearest first, the others by name). Until 2026-10-16 the 📡 DF
panel had no column headings for this kind: its list threw and stayed
empty (the nodes were only in the ID selector and the decoder tab).

### Separation by code (every antenna, since 2026-10-16)

The plugin is `multichannel`: in coherent mode with DoA on kraken_doa hands
it every antenna (`process_multi`; independent / wideband mode or DoA off:
one channel as before, no bearings). Antenna 0 is decoded exactly as before;
the antennas' input is kept 0.87 s in 8 bits per I / Q (`despread.hpp`
`RawRing`, 2^21 samples - the RTL's own resolution, 21 MB for 5 antennas;
long enough for the search's probes, which decode up to 0.8 s of history).
(A kraken_doa from before 2026-10-16 fed one antenna on a VFO where a
decoder that corrects its frequency - D-STAR, P25, ... - had run before,
e.g. this playback after another: the fact then says "one antenna" in
coherent mode with DoA on; fixed in kraken_doa's AFC mixer.)

Before, each frame was a packet whose covariance kraken_doa cut out of its
stream through the frame's channel filter - and it DROPPED both packets
when two talkers overlapped in time on the "same channel" (|df| < half the
wider bandwidth). In a busy band that is nearly every packet: the 927.5 MHz
SF10 BW500 gateway downlink is on air 60 % of the time and its 500 kHz
channel holds 927.4 / 927.6 MHz too; and what wasn't dropped mixed every
signal in its slice (a 500 kHz packet even the whole 2.4 MHz, until a
narrower one made kraken_doa keep the samples themselves).

LoRa is chirp spread spectrum, and a demodulated symbol IS its own code.
For every symbol the receiver demodulated (its window, the bin it decided,
its fractional de-rotation - the preamble's last up-chirps (checked: at the
preamble's bin, a clear peak), the two sync symbols, the header and the
payload) `lora_ds::despread_sym` correlates EVERY antenna's 2.4 MHz input
with exactly that chirp - the receiver's own model of the symbol (an
up-chirp from -BW/2 + u BW/N, wrapping at +BW/2 by BW, continuous phase, u
= bin + de-rotation; conjugated for inverted IQ), shifted to the channel's
offset, at the exact input times of the window (channel sample a = input
index first_block x HOP + a x 2.4 MHz / (os x BW): the channelizer has no
delay). y[k] = THIS transmitter's complex amplitude at antenna k; another
LoRa signal in the same samples - another SF / BW, or the same SF at
another timing / offset - stays spread (processing gain 2^SF), and so does
everything else in the 2.4 MHz. A model error (a fraction of a bin, the
receiver's CFO-for-timing window shift of a few chips) only lowers the
correlation: it is common to all antennas, so the direction is unbiased.
The two halves of a wrapped symbol are one coherent sum (no +-1 bin
bookkeeping). A packet's de-spreading is held to ~0.1 s of its airtime
(`DESPREAD_BUDGET_S`): longer packets take every k-th symbol (a 220-byte
SF10 downlink: 48 of its 240 symbols).

`lora_ds::code_cov` turns the packet's y vectors into its covariance:
symbols whose power is outside 0.25-4x the packet's median are left out (a
symbol decided wrong - the bin held another transmitter's energy, or none),
then those whose fit to the packet's main eigenvector is 0.3 below the
median fit; R = sum y y^H of the rest. It is sent with the packet
(`kp::Talker::cov`, wire TALKER fields 14 / 15) when >= 4 symbols are left
and the main eigenvalue holds >= 40 % of the trace ("coherence"). kraken_doa
then uses it as it is - no stream kept for the packet, no channel filter -,
never drops it for an overlap and runs MUSIC with ONE source on the talker
(`TalkerDoa::add_packet`). Talker IDs, labels, `avg_s`, `keep_s`, history
and the UI kind are unchanged. With the antennas but no usable covariance
(the samples already gone, too few clean symbols) the frame gives NO talker
packet: from the stream it would mix whatever else is in its channel - and
make kraken_doa keep 2.5 s of the whole 2.4 MHz (120 MB) for it. The
"Separation by code" fact counts both, with the mean symbols / coherence and
the CPU; the 📡 DF panel's packet line says "(all separated by code)". At
least `DESPREAD_MIN_SYMS` 12 symbols are taken whatever the budget (the
first version's 0.1 s left an SF12 BW125 uplink 3 symbols - no bearing).

**Two transmitters behind one ID** (`split.hpp`): an ID that names no
transmitter - `GW <MHz>` (two gateways of a network answering on one RX1
channel), `LoRa <MHz> SF<n> <sync>` - may hold several. Each coded packet's
spatial signature (the main eigenvector of its covariance) is kept for the
ID's last 30 packets; when they fall into two clusters - each >= 6 packets,
both in the older AND the newer half of the window (persistent and
interleaved, not one transmitter that moved), alike inside (median |u^H c|^2
>= 0.9; one transmitter's agree to ~0.99), apart from each other (|c1^H
c2|^2 <= 0.5 - on this 5-element UCA, r 0.12 m at 927 MHz, >= ~30 deg
apart), <= 20 % in between - the ID splits: `GW 927.5 #1` (the one heard
first since the decoder started) and `#2`, label "· transmitter n", the
merged talker forgotten (`talker_forget`); a packet fitting neither clearly
(>= 0.8 to one, 0.2 more than to the other) gives no talker, 12 such in a
row = one ID again. Event "📶 GW 927.5: two transmitters behind this ID";
the fact lists split IDs. Uplinks / Meshtastic nodes name themselves and are
never split (a moving node would cluster by its path).

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

**Real Meshtastic packets** (2026-10-16; verified against them, not in this
repo): no real Meshtastic IQ recording was found (the one public SigMF file
named in a GitLab project was never uploaded), so 2 min of Meshtastic's
public MQTT (`msh/ANZ/#`, read only) gave 21 real over-the-air packets
(ServiceEnvelope -> MeshPacket: from, to, id, hop limit / start, channel
hash, next hop, relay node + the ENCRYPTED payload as the radio sent it).
Reference: an independent decode (own protobuf walker, AES-128-CTR from the
`cryptography` library, the published default key): 14 decrypt to valid
Data protobufs (5 NODEINFO, 4 POSITION, 5 TELEMETRY), 7 are private
channels. Each packet = its real 16-byte header + real ciphertext
(`lora_gen.py` "payload_hex"), modulated on ANZ LongFast (919.875 MHz, SF11
/ 250 kHz, CR 4/5) and decoded by the plugin. Found and fixed:
- **AES-CTR counter**: bytes 12..15 were incremented LITTLE-endian (byte 12
  first) in `aes.hpp` AND in the generator; Meshtastic (CTR<AES128>,
  counter size 4) and standard CTR count big-endian from byte 15. So only
  the first 16 bytes of every payload decrypted - the round trip passed
  because the generator shared the error. Before: 11 / 21 as the
  reference (7 private headers + 4 whose protobuf survived; real NODEINFO
  names empty or the packet called "not the default channel"); after: 21 /
  21 - names, positions to 1e-7 deg, telemetry, the private ones header
  only; the generator's own encryption of the reference plaintexts = the
  real ciphertexts 21 / 21 (0 / 21 before)
- the "header only" event printed the channel hash through a double
  format (`ch#06` for every hash); now the real hashes (0x08 = LongFast,
  as the firmware's golden value)
- node names / texts are UTF-8 (`mesh::utf8_printable`; `kp::printable`
  showed "🐐Public MQTT Gate" as "\xF0\x9F\x90\x90Public MQTT Gate")
The radio PHY itself (Meshtastic's sync 0x2B, 16-symbol preamble) is not
checked on a real Meshtastic transmission (the PHY chain was, on real AU915
LoRaWAN gateway frames - sync 0x34 - and a 0x12 system).

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
`lora-au915-gateway`, `lora-au915-busy`, `lora-two-gateways`, `lora-eu868`,
`lora-private-search`.

**Separation by code** (2026-10-16, scratch lora4/; offline = `decoder
--file x.cu8 --channels 5 --uca 0.12 --rf F`, whose bearings now use the
packets' own covariances - "[n by code]"):

- `scene_au915_busy` (the user's band at 927.5 MHz, 8 s, plugin defaults):
  GW 927.5 SF10 BW500 downlinks every 0.8 s (on air 60 %, 140 deg), GW 926.9
  SF7 BW500 (250), the private SF7 BW250 0x12 system on 927.4 (30) and
  926.9 MHz (320, both found by the search), the SF8 BW125 0x12 sensor on
  927.0 (200), uplinks 260B1234 SF7 BW125 927.2 (100) and 26CC0042 SF9 BW125
  927.6 MHz (170 - inside the gateway's channel, 12 dB weaker), Meshtastic
  !c0ffee01 927.875 MHz SF10 BW250 (280). Before (cut from the stream):
  offline 13 of 64 packets dropped for overlaps, 26CC0042 at 144.0 (the
  gateway's direction), the 927.4 link peak/mean 5; debug playback (old
  kraken_doa + plugin) 138 packets in, 109 with a bearing, 20 dropped as
  overlapping, 26CC0042 -25.9 deg - 7 / 8. After: offline 64 / 64 de-spread
  (62 symbols each, coherence 1.00), every bearing within 0.6 deg; playback
  124 in, 124 with a bearing, 124 separated by code, 0 dropped - 8 / 8 at
  0.0 deg (26CC0042 -0.4 in a second run).
- `scene_two_gw` (two gateways on 927.5 MHz SF10 at 60 / 220 deg in turn
  every 0.6 s + the 927.0 sensor at 300): split after 12 packets (7 s) -
  before it `GW 927.5` read 54.4 deg (peak/mean 3: a bearing between them);
  then #1 60.0, #2 220.0, the sensor 300.0; playback 64 / 64 packets coded,
  3 / 3. No ID split in any other scene.
- Every other scene (au915, five, eu868, us915, as923, in865, 433, private,
  narrow): the same frames decoded as before, every talker's bearing within
  0.1 deg; in865 `!1d1d1d1d` keeps both packets (one was dropped for an
  overlap before). Playback (test build, ports 4708x): lora-mesh-five 6 / 6,
  lora-au915-gateway 6 / 6, lora-eu868 4 / 4, lora-private-search 5 / 5,
  every packet coded, settings file identical after Stop. (In a 5-entry
  session under load 6 lora-eu868 once missed its SF12 uplink: the plugin
  had fallen > 1 s behind and its input was dropped - the old plugin drops
  input there too.)
- Real single-channel capture (lora2/ch0_a.cu8, process() path): the
  per-frame dump is byte-identical to the old build (99 frames: GW 927.5
  47, GW 926.9 4, LoRa 927.000 12, the private system 18 + 18).
- Old plugin + new kraken_doa: works as before (no covariance sent: the
  stream path, 4 / 4 eu868, the busy scene's overlap drops as before).

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

Separation by code (every antenna, 2026-10-16): the antennas' input ring
3.8 % of a core (5 antennas x 2.4 MHz into 8 bits; -O3 / vector extensions
didn't speed it up), de-spreading 0.5-3 % (18 ns per input sample for all 5
antennas; the busy scene 2.8 %), +21 MB (the ring). Offline busy scene 63.8
-> 70.2 % of real time. Debug playback (plugin process, Pi 5 at load 4-6):
busy scene 46 -> 45 %, eu868 39 -> 43 % of a core, RSS 85-89 -> 107-111 MB;
kraken_doa 77 -> 74 % of a core and RSS 433 -> 388 MB (coded packets need
no stream kept: the 120 MB wide ring isn't allocated; the 48 MB/s of
SAMPLES_MC cost about what it saves).

## Limits / sources

- 2.4 GHz LoRa (SX128x, Meshtastic LORA_24) is outside the RTL-SDR's range.
- Encrypted payloads stay encrypted: Meshtastic only with the published
  default key, LoRaWAN FRMPayload / join accepts never (keys not public).
- Several packets on the SAME channel overlapping in time cannot both be
  decoded (one receiver per channel); collisions are lost.
- With every antenna, antenna 0 is decoded (as the CH selector's default; a
  weaker antenna 0 decodes less - combining the antennas for decoding, as
  ism does, isn't done). A frame decoded without enough clean de-spread
  symbols (the input already gone, a probe's oldest history) gives no
  bearing. An ID shared by transmitters < ~30 deg apart, or by three, is not
  split.
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
