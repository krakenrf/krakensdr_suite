# LoRa / Meshtastic / LoRaWAN decoder (`lora`)

Decodes LoRa chirp-spread-spectrum packets in the whole 2.4 MHz the tuner
receives, reads the Meshtastic and LoRaWAN layers carried on top, and gives
every transmitter its own bearing in the 🗺 Map's 📡 DF panel.

`sample_rate` = `min_vfo_rate` = 2.4 MHz, `lock_vfo` (the VFO sits on its
tuner's centre with the full bandwidth; tune the receiver to the band),
`manual_only`, `talkers`, `map`, `tunes`.

## What it decodes

**LoRa PHY** (SF7-12, BW 125 / 250 / 500 kHz, CR 4/5 - 4/8, explicit and
implicit header, low-data-rate optimisation, normal and **inverted IQ**): one
FFT channelizer (`channelizer.hpp`: 4608-point overlap-save of the 2.4 MHz,
per channel the bins around it + an inverse FFT -> exactly 2x its bandwidth,
zero delay) feeds a CSS receiver per SF and IQ orientation that share the
channel's sample ring - cheap dechirped preamble detection (one chip per
sample, a window every 2 symbols, 3 on one bin), up/down-chirp sync (integer
CFO / STO split + fractional CFO de-rotation), symbol demodulation with the
fractional offset TRACKED per symbol (the sample clocks drift ~0.4 bin over a
0.5 s SF10 packet at 2 ppm), then the coding chain (`coding.hpp`). Frames
with a payload CRC are reported only when it passes; frames without one
(LoRaWAN downlinks) when the header checksum and all eight header codewords
pass - with the count of payload codewords whose parity failed.

**The coding chain is the radios'** (since 2026-10-15, checked on real AU915
gateway downlinks): values are `(bin - 1)` relative to the preamble (reduced
rate: `round((bin - 1) / 4)`), Gray MAPPED (`v ^ v >> 1`), deinterleaved
`cw[k] bit (cw_len-1-i) = bit ((k - i) mod sf_app) of symbol i` with the
codeword's first bit d0, Hamming 4/5 = even parity of all four bits, the
whitening LFSR `0xFF, w' = (w << 1) | (w7^w5^w4^w3)` over the payload only,
the CRC = CRC16-CCITT of the first n-2 bytes XOR the last two, and the
SX127x header checksum. The first version (checked only against its own
generator) had a self-consistent guess for EACH of these - whitening,
diagonal direction, Gray direction, codeword bit order, 4/5 parity, header
checksum, CRC - so it could never decode a real packet.

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

**LoRaWAN** (on by default): uplinks AND gateway downlinks - the clear MAC
header only: MHDR, DevAddr, FCtrl / FCnt / FPort for data frames, DevEUI /
JoinEUI / DevNonce for join requests, the MHDR of a join accept. The
FRMPayload is encrypted with keys we do not have and the MIC cannot be
checked, so nothing is decrypted - headers are shown. A frame's direction
must match its IQ (uplinks normal, downlinks inverted). Downlinks carry no
payload CRC (LoRaWAN), and with CR 4/5 a symbol error can't be corrected:
when a codeword of the first 9 bytes (MHDR .. FPort) failed its parity the
DevAddr is shown with a "?" and no event is logged for it. The sync word is
measured per frame (0x34 public LoRaWAN, 0x12 private, 0x2B Meshtastic) and
shown in the row details.

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
- **LoRaWAN uplinks**: the DevAddr (data frames) or DevEUI (join requests).
- **LoRaWAN downlinks** are sent by the GATEWAY, not by the device whose
  DevAddr they carry: their talker is the gateway, named by its channel
  (`GW 927.5`; the frame names no gateway), its row lists the latest DevAddr
  it addressed. Never attributed to the DevAddr's device.

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
ShortFast and ShortSlow are all watched), `lorawan` (default ON: every channel
of the region's LoRaWAN plan inside the VFO at every data rate's SF - anz =
AU915: uplinks 915.2 + 0.2 n MHz (125 kHz, SF7-12) and 915.9 + 1.6 n (500 kHz,
SF8), downlinks 923.3 + 0.6 n (500 kHz, SF7-12, inverted IQ); us = US915
(902.3 / 903.0 / 923.3 MHz grids); eu868: 868.1-868.5 + 867.1-867.9 MHz both
directions + RX2 869.525 - at 927.5 MHz that is 7 + 1 uplink and 2 downlink
channels, 55 receivers), `sf=auto` (SF7-12 on the Meshtastic channels),
`channels` (an explicit `"MHz:SF:BWkHz[:inv|:both],..."` list, SF 0 = SF7-12,
that overrides all of it). `tune` (default on) retunes the receiver ONCE to
the preset's Meshtastic channel, and only when the receiver is OUTSIDE the
region's LoRa band (anz 915-928 MHz) - inside it the user picked the
frequency, and a plugin restart (a rebuild, a crash) must not pull the
receiver away (the first version retuned whenever nothing it watched was
inside the VFO - e.g. at 927.5 MHz with LoRaWAN off).

A strong packet also decodes on a watched channel one bandwidth away (a
whole-bandwidth offset aliases to CFO 0 after dechirping). Frames with the
same payload ending within 0.15 s are one transmission: the channel where it
is strongest is kept (the real one), so no duplicate talker packet is ever
reported on the wrong channel.

## Tests

**Real signal** (2026-10-15, the user's array tuned to 927.500 MHz, a 120 s
channel-0 capture from heimdall's 8091): the band is busy - 840 bursts, most
of them short non-LoRa (FSK) bursts on the 200 kHz grid. LoRa found in it: a
LoRaWAN gateway's downlinks on 927.5 MHz (AU915 downlink 7), SF10 BW500,
inverted IQ, sync 0x34, 233-byte unconfirmed downlinks to DevAddr 00F6D6E1
FPort 204 every 0.8 s (FCnt 2613, 2614, ... consecutive - a bulk / multicast
session), plus SF7 BW500 downlinks there and on 926.9 MHz to 0096EB28,
00EA3A76, 00CD8354, 0004DBC5, 01C294FD (FPort 1; NetID-0 private-network
DevAddrs); and a private-sync-word (0x12) sensor sending 2-byte frames with
CRC at SF8 BW125 on ~926.997 MHz. Offline the plugin decodes 63 frames in the
120 s (44 SF10 + 7 SF7 downlinks, 12 SF8 frames - every CRC passing); 13
downlinks with no parity error at all, the others hit by the other devices
transmitting inside the 500 kHz channel (the scratch Python reference
demodulator - scratch lora2/ - sees the same symbol errors). Not decoded:
SF9 BW500 normal-IQ bursts on 926.9 / 927.4 MHz (their header never
validated - perhaps implicit header or another framing). Why the old
version decoded nothing there: (1) the coding chain (above) was wrong for
every real packet, (2) no inverted IQ, (3) LoRaWAN was off by default and
watched only AU915 sub-band 2 at one SF, (4) no-CRC frames were never
reported, (5) at 927.5 MHz nothing it watched was inside the VFO. Live after
the rebuild: ~1 downlink / s reported, talkers GW 927.5 / GW 926.9 and the
sensors with bearings in the 📡 DF panel.

Generator `tools/lora_gen.py` builds the full transmit chain (mirrors
`coding.hpp` - the radios' chain since 2026-10-15; a LoRaWAN `unconf_dn` /
`conf_dn` source is sent with inverted IQ and no CRC, `"inverted"` overrides)
and modulates CSS at 2.4 MHz with continuous phase
(Meshtastic: 16-symbol preamble, sync word 0x2B; LoRaWAN: 8, 0x34) - several
transmitters at known bearings on the 5-element UCA, overlapping in time, on
different channels / SFs, a relayed packet, a private channel, noise; IDs in
the scene may be hex strings, random bytes come from the scene's seed.
Scenes: `tools/scene_five.json` (playback `lora-mesh-five`) and
`tools/scene_au915.json` (playback `lora-au915-gateway`: 927.5 MHz, the
default plan, two gateway downlinks SF10 / SF7 BW500 + uplinks SF7, SF12 (LDRO),
SF9 BW125 and SF8 BW500). Fixed with it: the generator wrote only the first
second of a packet (an SF12 / 125 kHz uplink lasts 1.3 s).

- **2026-10-15 rework** (real chain, channelizer, inverted IQ): scene_five
  16 / 16 frames, every bearing exact (60.0 / 160.0 / 300.0 / 100.0 / 20.0 /
  230.0); scene_au915 16 frames, six talkers GW 927.5 140.0, GW 926.9 250.6,
  260B1234 40.0, 260C5678 300.0, 26BB9ABC 200.0, 26AA0001 80.0 deg (truth
  140 / 250 / 40 / 300 / 200 / 80; two 260B1234 uplinks lost where they
  overlap the 500 kHz downlink channel, as on the air). The playback entries
  were NOT run through the live kraken_doa (the user was using it).
  The tests below are from the first version (its own chain):

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

Live with the first version (2026-10-15): 10 min at 919.750 MHz (the ANZ
LongFast / ShortFast / ShortSlow default channels) + 25 s at 922.977 MHz: no
LoRa frame - with its guessed chain it could not have decoded one (see
*Real signal* above). No real Meshtastic packet has been decoded yet (none
heard here); its layer is unchanged and the PHY is now the radios'.

Bugs these tests found and fixed: the preamble residual was measured where
the b0 alignment put the start - one symbol BEFORE the preamble whenever the
first stable hop fell into the first preamble symbol, i.e. on noise: ~40 % of
packets failed their header checksum (now measured on the last preamble
symbol, 3 before the SFD); the SFD search looked only 12 symbols ahead
(Meshtastic's 16-symbol preamble would have been missed; now 40); a "wrong"
LoRaWAN DevAddr was the scene's own typo (638484360 = 0x260E7F88, not
0x26011F88) - the decoder had returned exactly the bytes sent.

CPU (Pi 5): the AU915 window at 927.5 MHz (12 channels, 55 receivers) live
27-30 % of a core incl. the ~11 % every 2.4 MHz plugin spends on its input
pipe (offline 44 % of real time with the file read); 113 MB RSS (the channels'
rings, sized for the longest packet of their largest SF, <= 4 s). Before the
channelizer and the cheap detector the same window took 90 %. First version
(offline; the host's own 2.4 MHz file read + conversion ~6 % of it):
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
- LoRaWAN downlinks have no CRC: payload bytes with parity errors are
  reported as received (only the MAC header gets the "?" check); CR 4/5
  corrects nothing.
- The receiver cap is 72 (channel x SF x IQ); the Watching fact says when it
  is reached.

Protocol knowledge is clean-room from public documentation: the LoRa PHY
reverse-engineering literature (Knight; Robyns et al.; Tapparel, "Complete
Reverse Engineering of LoRa PHY", EPFL; Semtech AN1200.22 / SX127x datasheets),
the Meshtastic protocol / encryption documentation (meshtastic.org,
meshtastic/meshtastic-sdk; the frequency slot formula from the firmware's
RadioInterface), the LoRaWAN MAC specification and the LoRaWAN Regional
Parameters (RP002: AU915 / US915 / EU868 channel plans and data rates). GPL
projects (gr-lora, gr-lora_sdr, rtl_433) and LoRaPHY were read only as
documentation / test reference, never copied; the coding conventions were
settled on the real capture (a brute force over the open choices against the
header checksum + Hamming codewords, then CRC / consecutive FCnt).
