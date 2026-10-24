# LoRa / Meshtastic / LoRaWAN plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**LoRa / Meshtastic / LoRaWAN (`plugins/lora/`, README.md there):**
- `sample_rate` = `min_vfo_rate` 2.4 MHz, `lock_vfo`, `manual_only`,
  `talkers`, `map`, `tunes`. ONE FFT channelizer (`channelizer.hpp`:
  4608-point overlap-save, hop 3456, per channel its bins through a flat /
  raised-cosine response + an inverse FFT of 2 BW / 2.4 MHz x 4608 points ->
  EXACTLY 2x BW, zero delay, the off-grid residual mixed at the output rate)
  feeds every watched channel; per channel one sample ring (`lora_phy::Ring`,
  calloc'd, RING_SYMS 60 symbols of its largest SF: the sync / header stage
  only - after the header the payload is demodulated BLOCK BY BLOCK as it
  arrives, `LoraRx::continue_packet` / `Pk`; it was sized for the longest
  packet, 4 s, 16-32 MB per channel, 363 MB live) shared by a CSS
  receiver per SF and IQ orientation (`lora_phy.cpp`): cheap detection
  (`detect_bin`: one chip per sample, a window every DET_HOP 2 symbols, 3 on
  one bin), up/down-chirp sync (integer CFO/STO split by b0 alignment -
  which cancels the integer CFO in the data - + fractional-CFO de-rotation),
  symbol demod with the fractional offset TRACKED per symbol (TRACK_ALPHA
  0.2: sample-clock drift, ~0.4 bin over a 0.5 s SF10 packet), INVERTED IQ
  (downlinks) = the ring read conjugated, the sync word measured, then the
  coding chain (`coding.hpp`). SF5-12, BW 7.8-500 kHz, explicit + implicit
  header, LDRO. Frames with a CRC only when it passes; without one (LoRaWAN
  downlinks) when the header checksum + all 8 header codewords pass, with
  the payload codewords' parity count (`fec_bad`) and per-byte validity
  (`byte_ok`)
- THE RADIOS' CODING CHAIN (2026-10-15, settled on a real capture of AU915
  gateway downlinks by brute force against the header checksum + Hamming
  codewords, then consecutive FCnt): value = (bin - 1) relative to the
  preamble (reduced rate round((bin - 1) / 4)), Gray MAPPED v ^ v >> 1,
  `cw[k] bit (cw_len-1-i) = bit ((k - i) mod sf_app) of symbol i` (d0 first),
  4/5 = parity of all four bits, whitening LFSR 0xFF / (w << 1) |
  (w7^w5^w4^w3) over the payload only, CRC16 of the first n-2 bytes XOR the
  last two, the SX127x header checksum. The first version guessed each of
  these differently (self-consistent with its generator) and could not
  decode a real packet
- Meshtastic (`meshtastic.hpp`): the clear 16-byte PacketHeader; the payload
  decrypted ONLY with the PUBLISHED default channel key
  (`d4f1bb3a20290759f0bcffabcf4e6901`, AES-128-CTR, `aes.hpp`) - a private
  channel shows its clear header only, keys are never guessed. The Data
  protobuf -> portnum + TEXT / POSITION (map point) / NODEINFO / TELEMETRY /
  ROUTING. LoRaWAN (`lorawan.hpp`): clear MAC header only (MHDR, DevAddr,
  FCtrl/FCnt/FPort; join DevEUI/JoinEUI) - the FRMPayload is encrypted.
  Meshtastic vs LoRaWAN: a valid default-key-decrypted Data protobuf (or a
  broadcast header with sane hops) = Meshtastic (never with inverted IQ or
  sync 0x34 / 0x12), else LoRaWAN when its direction matches the IQ (uplinks
  normal, downlinks / join accept inverted), else raw LoRa (id "lora MHz/SF")
- ID separation: a `kp::Talker` packet per frame of the actual transmitter -
  the Meshtastic node (`!xxxxxxxx` from `from`), or the RELAYING node
  (`!relay-XX`, the header `relay_node` byte) when `hop_start != hop_limit`
  (like flarm's relay handling), or the LoRaWAN DevAddr / DevEUI of an
  UPLINK; a DOWNLINK is the GATEWAY's ("GW 927.5" by its channel - the frame
  names no gateway; never the DevAddr's device; its MAC header shown with a
  "?" when a codeword of bytes 0-8 failed parity, no event then); `freq_hz` /
  `bw_hz` = the LoRa channel so nodes on different channels/SFs keep their
  own bearings. `avg_s` 10, `keep_s` 30 min. UI kind `node` 📶 ("Node /
  Name · packet / Frames", nearest first by a Meshtastic POSITION, else by
  name)
- SEPARATION BY CODE (2026-10-16, the user's "implement separation by code":
  at 927.5 MHz the DF panel listed none of the decoder's transmitters - the
  SF10 BW500 gateway downlink is on air 60 % of the time, and TalkerDoa
  dropped every pair of packets overlapping on the "same channel"):
  `kp::Info::multichannel` - in coherent mode with DoA on every antenna
  (`process_multi`); antenna 0 decoded as before; `despread.hpp`
  `lora_ds::RawRing` keeps every antenna's input 2^21 samples (0.87 s, 8 bits
  per I / Q = the RTL's grid, 21 MB at 5 antennas; covers the probes' 0.8 s
  of history), indexed like the front end (`Channel::first_block`: channel
  sample a = input first_block x HOP + a x FS / rate_out). `LoraRx::desp`
  (`lora_phy::Despreader`, `ChanDesp` per channel): every symbol the receiver
  demodulates - the preamble's last <= 8 up-chirps (at its bin, a clear
  peak), the 2 sync symbols, the header block, the payload block by block -
  is correlated on every antenna with exactly the receiver's model of it
  (`despread_sym`: up-chirp from -BW/2 + (bin + de-rotation) BW/N, wrap at
  +BW/2, continuous phase, the channel's offset, conjugated for inverted IQ;
  phase recursion in 128-sample pieces from an exact phase) -> y per
  antenna; other LoRa (other SF / BW, the same SF at another timing / offset)
  and everything else in the 2.4 MHz stay spread. Held to
  `DESPREAD_BUDGET_S` 0.1 s of airtime per packet (every k-th symbol beyond).
  `Frame::sym_y` -> `lora_ds::code_cov` (power 0.25-4x the median, fit to the
  main eigenvector within 0.3 of the median, >= 4 symbols, main eigenvalue
  >= 40 % of the trace) -> `kp::Talker::cov`. With the antennas but no
  usable covariance the frame gives no talker packet (`set_cov`); with one
  antenna (process()) everything as before. Fact "Separation by code"
  (packets de-spread, mean symbols / coherence, without a bearing, CPU).
  At least `DESPREAD_MIN_SYMS` 12 symbols per packet (SF12 BW125)
- TWO TRANSMITTERS BEHIND ONE ID (`split.hpp`, the coded packets'
  signatures = main eigenvectors): IDs that name no transmitter (`GW <MHz>`,
  `LoRa <MHz> SF<n> ...`) split into `<id> #1` / `#2` when the last 30
  signatures form two clusters (>= 6 each, in both halves of the window,
  median fit >= 0.9 inside, centroids |c1^H c2|^2 <= 0.5 = >= ~30 deg apart
  on the live UCA, <= 20 % unclear); the merged talker is forgotten
  (`talker_forget`), a packet fitting neither gives none, 12 in a row = one
  ID again. #1 = the first heard since the decoder started (playback
  `lora-two-gateways` checks bearings only)
- Separation tests (2026-10-16, scratch lora4/): `scene_au915_busy` /
  playback `lora-au915-busy` (8 transmitters at 927.5 MHz, the gateway on
  air 60 %): before 138 packets, 109 with a bearing, 20 dropped as
  overlapping, an uplink inside the gateway's channel 26 deg off (7 / 8);
  after 124 / 124 coded, 0 dropped, 8 / 8 at 0.0 deg; `scene_two_gw` /
  `lora-two-gateways`: split after 12 packets, 60.0 / 220.0 (3 / 3); every
  other scene / entry unchanged frames, bearings within 0.1 deg; the real
  single-channel capture's frame dump byte-identical; dtv / cells / ism
  rebuilt with the new SDK host: identical output. CPU: +3.8 % (antennas'
  ring) +0.5-3 % (de-spreading) of a core, +21 MB; kraken_doa -45 MB (no
  wide ring for coded packets)
- Watched channels, REGION-NEUTRAL (the user's rule 2026-10-15; reworked
  2026-10-16, `regions.hpp`): `region` (Meshtastic) and `lorawan` (plan)
  default **auto** = every Meshtastic region (the firmware's table: 25
  regions, spacing 0; presets incl. LongTurbo SF11/500; display names -
  LONG_MODERATE hashes "LongMod") and every RP002 LoRaWAN plan (EU868, US915,
  AU915, AS923-1..4, KR920, IN865, RU864, CN470 on its 470.3 + 0.2 n grid,
  EU433, CN779; grids `LwGrid` iq 0 up / 1 down / 2 both, RX2, class-B
  beacons `LwBeacon`) whose band overlaps the VFO; a key = only that one,
  off / 0 = none (old `lorawan=1` = auto, `region=eu868` = eu_868). The
  Meshtastic channel as the FIRMWARE computes it (`mesh_freq_hz`: numChannels
  = floor((end - start) / bw), slot = djb2(name) % numChannels, f = start +
  bw/2 + slot bw; the default channel hashes the preset's name - ANZ LongFast
  919.875, US 906.875, EU_868 869.525, IN 865.875, EU_433 433.875). Specs
  get a priority (`Prio`: explicit, learned, primary preset, LoRaWAN
  downlink / RX2, beacon, other presets, uplinks - then nearest the VFO
  centre, SF) and are added while the `cpu` budget holds (`budget_cost`:
  receiver ~ bw x SF, a NEW channel ~ bw x 5.3 log2(2 bw / 520.8 Hz) - its
  IFFT costs more than a one-SF receiver; normal 330 M ~ the AU915 window at
  927.5 MHz; MAX_RX 128); what's dropped is named in Watching. `tune` only
  with a chosen region, once, from outside its band. `channels`: explicit
  `MHz:SF:BWkHz[:inv|:both]` (SF5-12, any LoRa BW - `snap_bw`; channels with
  2 bw not a multiple of 4 bins run at os 4: `lora_chan::os_for`)
- Class-B beacons: an implicit-header receiver (`LoraRx` implicit, CR 4/5,
  no CRC, the plan's length); `beacon()` tries every RFU1 split, needs both
  CRC-16s (0x1021 init 0, LSB first, LoRaMac-node's parser) -> talker "GW
  <MHz>", InfoDesc 0-2 = the gateway's position
- PLAN-FREE SEARCH (`search` 1 = 62.5-500 kHz, all = 7.8-500, 0 = off):
  stage 1 `lora_search::Stage1` on the channelizer's forward FFTs (per-bin
  floor, 11.5 ms groups, per BW a window of 4-12 groups, centres every B/8:
  >= 9 sigma, flat quarters, band-limited, local max, rising edge); stage 2
  `probe()`: a channel at the candidate synthesized from the last HIST_S 0.8 s
  of FFT blocks (`xhist_`), receivers for every SF (5-12; narrow 5-10) / IQ
  no channel of that BW there has (none if a plan / learned channel of the
  BW sits within B/8), `strict_nocrc`, `det_log`; a receiver lives 14 of its
  symbols past the probe's start (or while pending). A frame -> `learn()`
  (a `Learned` channel at the measured frequency, rounded to 1 kHz, unless a
  plan channel already has that receiver; 30 min, <= 8); nothing -> a
  back-off (2 s doubling to 60 s); a preamble + SFD + sync word without a
  header -> `report_detections` once SEEN AGAIN (same place / SF / BW / IQ /
  sync within 10 min, preamble energy >= -6 dB in its bin) - the first run
  on the real capture reported 291 one-off SF5 / SF6 / narrow "detections"
  of FSK bursts before that rule. NOT reported (2026-10-16, `near_decoded`,
  counted in the Search fact "N aliases of decoded packets left out"): a
  preamble inside / next to (50 ms, BW / 8) a packet decoded at the same
  time in the same band - reported in the last 30 s (`heard_`), waiting in
  `pend_`, or in progress on another channel (`LoraRx::busy_from`; on probes
  only a decoded header, `decoding_from`) - and sync word 0x00 (both sync
  symbols on the preamble's own bin): aliases of a real packet (a chirp of
  the same slope BW^2 / 2^SF - SF11 / 250 kHz dechirps like SF9 / 125 and
  SF7 / 62.5 kHz - or a piece of it through the probe's edge). Playback
  lora-mesh-five listed five such "LoRa 921.733 SF9 0x00" talkers at the
  real nodes' bearings after ~14 min; offline 30 loops (180 s): 11 left
  out, 0 reported (before: 2 nodes in 120 s). Cost: <= 6 probes/s (token bucket), <= 4
  alive, a CPU budget in the plugin's own THREAD CPU time per stream second
  (`thread_cpu_s`, `probe_cpu_tok_`: 12 % normal, low 4 / high 25, <= 0.3 s
  banked) - over it the probes are slowed (>= one per PROBE_MIN_GAP_S 2 s),
  never stopped; the Search fact says "SLOWED". (The wall-clock version
  counted preemption on a busy Pi as its own time and paused the search for
  good: playback lora-private-search 1 / 5 at load 3-5)
- PHY additions (2026-10-16): SF5 / SF6 (header block at the full rate, with
  or without 2 extra symbols after the SFD - both tried; unverified on an
  SX126x), detection 4 windows + a higher peak threshold at SF <= 6
  (`detect_ratio`), no-CRC frames from the search / SF <= 6 need <= 1/8 bad
  payload codewords and CR != 4/7; `Detect` log; the dedupe (`same_frame`)
  also matches no-CRC copies with >= 80 % equal bytes (a KR920 923.3
  channel decoded the AS923 923.4 downlink with errors)
- Other LoRa's talker: channel + SF + sync word ("LoRa 926.901 SF7 0x12",
  `raw_tid`, the channel's nominal / learned frequency) - one sensor no
  longer splits into two IDs by its measured frequency
- Channel timestamps: `Channel::t0` = Host::time() at its creation (a
  rebuild mid-stream kept the old zero). A strong
  packet also decodes on the watched channel one BW away (a whole-BW offset
  aliases to CFO 0): same payload within 0.15 s on two channels = one
  transmission, the strongest kept (`queue` / `flush_pending`, 50 ms hold)
- `LoraRx`: the preamble residual (b_up) and the SNR are measured on the LAST
  preamble symbol (SFD - 3 symbols) - the b0 alignment can put the start one
  symbol before the preamble; the SFD is searched up to 40 symbols ahead
  (Meshtastic's preamble is 16); a pending packet is retried only once
  `need_abs_` samples are there (it was re-run every block)
- Generator `tools/lora_gen.py` (full chain mirroring coding.hpp, AES default
  key + protobuf, LoRaWAN frames, continuous-phase CSS at 2.4 MHz with
  Meshtastic's 16-symbol preamble / sync 0x2B and LoRaWAN's 8 / 0x34, several
  nodes + a relay on a UCA, hex IDs, seeded bytes; LoRaWAN `unconf_dn` /
  `conf_dn` = inverted IQ without CRC), `tools/scene_five.json` (playback
  `lora-mesh-five`, 6 talkers) and `tools/scene_au915.json` (playback
  `lora-au915-gateway`: 927.5 MHz default plan, GW 927.5 / GW 926.9 + uplinks
  SF7 / SF12 LDRO / SF9 / SF8-500; it used to write only the first second of
  a packet - SF12 / 125 kHz lasts 1.3 s)
- Real signal (2026-10-15, user's array at 927.500 MHz, 120 s channel-0
  capture, scratch lora2/): AU915 gateway downlinks on 927.5 MHz SF10 BW500
  (233-byte unconfirmed downs to DevAddr 00F6D6E1 FPort 204 every 0.8 s,
  FCnt consecutive) + SF7 BW500 ones there and on 926.9, and a sync-0x12
  sensor (2-byte frames, SF8 BW125, ~926.997 MHz): 63 frames decoded in 120 s
  (all CRCs pass; 13 downlinks without a parity error, the rest hit by the
  busy band - the Python reference sees the same). The "SF9 BW500 normal-IQ
  bursts" on 926.9 / 927.4 MHz were SF7 BW250 (the same chirp slope BW^2 /
  2^SF): a private system, sync 0x12, explicit header without CRC, 21 / 101 /
  231-byte frames starting 00 00 fd 1d - since 2026-10-16 the search learns
  both channels (63 + 45 frames in the 120 s, no false ones; scratch lora3/) Rework
  tests: scene_five 16 / 16 + bearings exact; scene_au915 six talkers
  within 0.6 deg. CPU live at 927.5 MHz 27-30 % of a core (incl. ~11 % for
  the input pipe; 90 % before the channelizer + cheap detector), 113 MB.
  Playback entries not run on the live kraken_doa (in use)
- Tests: C++ `t_phy` round trip SF7-12 x CR x BW incl. CFO / STO / noise
  (71 / 72; SF12 to ~-12 dB/sample); byte-exact check of the whole path
  (scratch verify.py: every CRC-valid frame = the generator's payload): 16 /
  16 sent / decoded, each on its own channel, no duplicates; bearings 60 /
  160 / 300 / 100 / 20 / 230 deg exact, noise silent. Found + fixed by them:
  the b_up-on-noise bug (~40 % of packets failed the header checksum), the
  12-symbol SFD search, adjacent-channel duplicates; the "wrong DevAddr" was
  a typo in the scene (638484360 = 0x260E7F88). CPU (Pi 5, offline, ~6 % of
  it the host's file read): one busy channel 15.6 % (was 39 %), five 43 %
  (was 120 %). Limits: two packets overlapping on one channel can't both
  decode (half-duplex); a CFO beyond ~-20 % of BW at SF7 sometimes costs a
  symbol; GPL projects used as documentation only. Live 10 min at 919.750
  MHz (ANZ default channels) + 25 s at 922.977: no frame (first version -
  its chain could not have decoded one). No real Meshtastic packet yet

