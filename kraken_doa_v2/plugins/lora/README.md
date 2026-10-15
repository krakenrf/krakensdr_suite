# LoRa / Meshtastic / LoRaWAN decoder (`lora`)

Decodes LoRa chirp-spread-spectrum packets in the whole 2.4 MHz the tuner
receives, reads the Meshtastic and LoRaWAN layers carried on top, and gives
every transmitter its own bearing in the 🗺 Map's 📡 DF panel.

`sample_rate` = `min_vfo_rate` = 2.4 MHz, `lock_vfo` (the VFO sits on its
tuner's centre with the full bandwidth; tune the receiver to the band),
`manual_only`, `talkers`, `map`, `tunes`.

## What it decodes

**LoRa PHY** (SF7-12, BW 125 / 250 / 500 kHz, CR 4/5 - 4/8, explicit and
implicit header, low-data-rate optimisation): per watched channel the signal
is mixed down, resampled to 2x its bandwidth and run through a CSS receiver -
dechirped preamble detection (so weak, below-noise packets are still caught),
up/down-chirp sync (integer CFO / STO split + fractional CFO de-rotation),
symbol demodulation, then the coding chain: Gray mapping, diagonal
interleaving, Hamming FEC, de-whitening, header checksum and the payload
CRC16. Only frames that pass the header checksum and the CRC are reported.

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

**LoRaWAN**: the clear MAC header only - MHDR, DevAddr, FCtrl / FCnt / FPort
for data frames, DevEUI / JoinEUI / DevNonce for join requests. The
FRMPayload is encrypted with keys we do not have and the MIC cannot be
checked, so nothing is decrypted - headers are shown.

Meshtastic vs LoRaWAN is decided by whether the default-key decrypt yields a
valid `Data` protobuf (or the header is a broadcast with sane hop fields);
otherwise the frame is read as LoRaWAN, else as raw LoRa.

## ID separation (a DoA per transmitter)

Every decoded frame is a `kp::Talker` packet of the transmitter that actually
sent **these** samples, on its own channel (`freq_hz` / `bw_hz` = the LoRa
bandwidth), so nodes on different channels / SFs at the same moment keep their
own bearings:

- **Meshtastic**: the node id (`!xxxxxxxx` from `from`). When
  `hop_start != hop_limit` the frame was **relayed** - the transmitter heard
  is the relaying node, so the talker is `!relay-XX` (XX = the header's
  `relay_node` byte, the last byte of the relayer's id), not the originator
  (exactly as the flarm plugin drops relayed OGN packets). A private-channel
  frame still reports its sender's clear id and bearing.
- **LoRaWAN**: the DevAddr (data frames) or DevEUI (join requests).

`avg_s` 10 s, `keep_s` 30 min. UI kind `node` (📶), with Dist / position
columns for nodes that send POSITION.

## Region / channel plans

The watched Meshtastic channel is computed as the firmware does
(`RadioInterface`): `numChannels = floor((freqEnd - freqStart) / (spacing +
bw))`, `slot = djb2(channel name) % numChannels`, `f = freqStart + bw/2 +
slot x bw`. The default (unnamed) primary channel hashes the modem preset's
name, so **ANZ LongFast = slot 19 of 52 = 919.875 MHz** (MediumFast 926.125,
ShortFast 918.875, ShortSlow 920.625, LongSlow 918.312 ...); US LongFast
906.875, EU868 869.525 MHz. Options: `region` (anz 915-928 / us 902-928 /
eu868), `preset` (LongFast default: SF11 BW250), `channel` (a named primary
channel: its hash slot), `others` (default on: also the other presets'
default channels that fall inside the VFO - tune to 919.750 MHz and LongFast,
ShortFast and ShortSlow are all watched), `lorawan` (the region's 125 kHz
uplink grid: AU915 sub-band 2 916.8-918.2 MHz for anz), `sf=auto` (SF7-12 on
each channel, more CPU), `channels` (an explicit `"MHz:SF:BWkHz,..."` list
that overrides all of it). If no watched channel is inside the VFO and `tune`
is on, the plugin retunes the receiver once to the preset's channel.

A strong packet also decodes on a watched channel one bandwidth away (a
whole-bandwidth offset aliases to CFO 0 after dechirping). Frames with the
same payload ending within 0.15 s are one transmission: the channel where it
is strongest is kept (the real one), so no duplicate talker packet is ever
reported on the wrong channel.

## Tests

Generator `tools/lora_gen.py` builds the full transmit chain (mirrors
`coding.hpp` exactly) and modulates CSS at 2.4 MHz with continuous phase
(Meshtastic: 16-symbol preamble, sync word 0x2B; LoRaWAN: 8, 0x34) - several
transmitters at known bearings on the 5-element UCA, overlapping in time, on
different channels / SFs, a relayed packet, a private channel, noise; IDs in
the scene may be hex strings, random bytes come from the scene's seed.
`tools/scene_five.json` is the shipped scene; playback entry `lora-mesh-five`.

- **PHY round trip** (C++, scratch `t_phy`): SF7-12 x CR 4/5-4/8 at BW 125 /
  250 / 500, CFO 0 / +7 / -25 bins, timing offsets, noise: 71 / 72 bit-exact
  (the one miss: below). SF12 decodes down to about -12 dB per-sample SNR
  (well below 0 dB, as a LoRa demod should).
- **Byte-exact check of the whole path** (2.4 MHz scene -> channel resampler ->
  CSS receiver -> chain; scratch `verify.py` compares every CRC-valid frame
  with the generator's payload): **16 / 16 packets sent, 16 / 16 decoded
  exactly**, each on its own channel, no duplicates - incl. the LoRaWAN uplink
  on a channel +0.5 MHz off the VFO centre (DevAddr 26011F88).
- **Bearings** (`decoder --file scene5.cu8 --channels 5 --uca 0.12 --rf
  921.8e6`): `!a1b2c3d4` 60.0, `!11223344` 160.0, `!deadbeef` 300.0,
  `!000000aa` 100.0, DevAddr 26011F88 20.0, the relay `!relay-44` 230.0 deg -
  each exact. Noise produces no frames.

Bugs these tests found and fixed: the preamble residual was measured where
the b0 alignment put the start - one symbol BEFORE the preamble whenever the
first stable hop fell into the first preamble symbol, i.e. on noise: ~40 % of
packets failed their header checksum (now measured on the last preamble
symbol, 3 before the SFD); the SFD search looked only 12 symbols ahead
(Meshtastic's 16-symbol preamble would have been missed; now 40); a "wrong"
LoRaWAN DevAddr was the scene's own typo (638484360 = 0x260E7F88, not
0x26011F88) - the decoder had returned exactly the bytes sent.

CPU (Pi 5, offline; the host's own 2.4 MHz file read + conversion ~6 % of it):
one busy channel 15.6 % (was 39 %), five channels 43 % (was 120 %) - the
pending decode is retried only once the samples it waits for are there (it
was re-run every block while a packet's tail arrived), the channel mixer is a
phasor recursion, the decimating FIR runs over a contiguous double-buffered
history. Each watched channel / SF adds a resampler + dechirp receiver
(capped at 28).

## Limits / sources

- Several packets on the SAME channel overlapping in time cannot both be
  decoded (LoRa is half-duplex; one receiver per channel) - on the real band
  nodes back off, so this is rare.
- A carrier offset beyond about -20 % of the bandwidth (SF7 BW125: -24 kHz)
  sometimes costs a symbol (5 of 40 trials with noise, +20 % fine): the
  timing shift that cancels the integer CFO then puts a fifth of every window
  into the next symbol. LongFast (BW250) at 20 ppm is 7 % of its bandwidth.
- `sf=auto` across many channels is expensive; keep the watch list small.

Protocol knowledge is clean-room from public documentation: the LoRa PHY
reverse-engineering literature (Knight; Robyns et al.; Tapparel, "Complete
Reverse Engineering of LoRa PHY", EPFL; Semtech AN1200.22 / SX127x datasheets),
the Meshtastic protocol / encryption documentation (meshtastic.org,
meshtastic/meshtastic-sdk; the frequency slot formula from the firmware's
RadioInterface) and the LoRaWAN MAC specification. GPL projects (gr-lora,
gr-lora_sdr, rtl_433) were read only as documentation / test reference, never
copied.
