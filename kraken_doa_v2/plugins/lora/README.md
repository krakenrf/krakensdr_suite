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

`region` (default **anz** = Australia / New Zealand, 915-928 MHz) sets the
default Meshtastic channel frequency and the LoRaWAN uplink grid. `preset`
(default **LongFast** = SF11 BW250) is the Meshtastic modem preset
(ShortTurbo / ShortFast / MediumFast / LongModerate / LongSlow ...). With no
explicit `channels` the plugin watches the region's Meshtastic default
channel; `lorawan=1` adds the region's 125 kHz uplink channels (headers only).
`channels` = an explicit `"MHz:SF:BWkHz,..."` list overrides all of that;
`sf=auto` scans SF7-12 on each channel (more CPU). If no watched channel is
inside the VFO and `tune` is on, the plugin retunes the receiver once to the
region's Meshtastic frequency.

> The region default frequencies are the commonly used LongFast channel
> centres; a real mesh may use another channel name (the frequency comes from
> a hash of the channel name) - set `channels` or tune the receiver to match.

## Tests

Generator `tools/lora_gen.py` builds the full transmit chain (mirrors
`coding.hpp` exactly; verified bit-exact by round trip) and modulates CSS at
2.4 MHz with several transmitters at known bearings on the 5-element UCA,
overlapping in time, on different channels / SFs, with a relayed packet and
noise. `tools/scene_five.json` is the shipped scene; playback entry
`lora-mesh-five` (`tools/testsignals.json`).

- **PHY round trip** (C++ `t_phy`): every SF7-12 x CR 4/5-4/8 x BW
  125/250/500, clean and with CFO (to +-60 bins), timing offset and noise -
  payload bit-exact. SF12 decodes down to about -12 dB per-sample SNR (well
  below 0 dB, as a LoRa demod should).
- **Offline, multi-antenna** (`decoder --file scene5.cu8 --channels 5
  --uca 0.12 --rf 921.8e6`): Meshtastic text / position / nodeinfo / private
  and the LoRaWAN uplink decoded; per-talker bearings
  `!a1b2c3d4` 60.0, `!11223344` 160.0, `!deadbeef` 300.0, `!000000aa` 100.0,
  DevAddr 20.0, relay 230.0 deg - each exact.
- Noise produces no frames.

CPU (Pi 5, offline incl. the 2.4 MHz file read / conversion): ~30 % of a core
for one watched channel, ~80 % for three. The default (one Meshtastic
channel) stays modest; each extra channel / SF adds a resampler + dechirp
receiver (capped at 28).

## Limits / sources

- Several packets on the SAME channel overlapping in time cannot both be
  decoded (LoRa is half-duplex; one receiver per channel) - on the real band
  nodes back off, so this is rare.
- `sf=auto` across many channels is expensive; keep the watch list small.
- The channel-name-hash frequency is not computed; use the region default or
  set `channels`.

Protocol knowledge is clean-room from public documentation: the LoRa PHY
reverse-engineering literature (Knight; Robyns et al.; Tapparel, "Complete
Reverse Engineering of LoRa PHY", EPFL; Semtech AN1200.22 / SX127x datasheets)
and the Meshtastic (meshtastic.org, meshtastic/meshtastic-sdk) and LoRaWAN MAC
specifications. GPL projects (gr-lora, gr-lora_sdr, rtl_433) were read only as
documentation / test reference, never copied.
