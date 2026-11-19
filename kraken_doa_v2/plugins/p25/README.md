# P25 decoder plugin

APCO P25 Phase 1 (TIA-102.BAAA, 12.5 kHz C4FM, CQPSK / LSM read through the
discriminator) and Phase 2 (two-slot TDMA, TIA-102.BBAC-A: the sites'
H-DQPSK and the radios' H-CPM) on one 12-24 kHz VFO. Version 3.0
(2026-11-18) added Phase 2. Both receivers run on the same 48 kHz input all
the time, so the two phases are told apart without a setting: a trunked
system's traffic channel can carry Phase 1 and Phase 2 calls in turn
(tested: a Phase 1 call, a Phase 2 call and a Phase 1 call on one channel,
all decoded). Version 3.1 (2026-11-20) reports trunked systems to kraken_doa's
trunk following (*Trunk following* below).

## Phase 1

The 4FSK receiver (`plugins/lib/dig_fsk4.cpp`: integrate-and-dump filtered
discriminator, frame sync correlation, levels / timing / carrier offset from
the sync) hands each frame to `p25.cpp` as soon as the frame's own length is
in (HDU 396 dibits, TDU 72, TDULC 216, LDU 864, a trunking block once its own
block is; before 2026-11-19 every frame waited for 864 dibits): a single
trunking block about 33 ms after its sync instead of 180 ms, terminators
100-165 ms earlier, frames in a recording's last 0.2 s decode; the frames'
sample positions are unchanged. CQPSK (simulcast LSM's fast phase steps
through the discriminator): the +-6 kHz per-sample limit on the
discriminator (there against noise clicks) cut its steps near the envelope
nulls (6 % of the on-air samples; the outer levels squeezed - scale 429
instead of 601 Hz per level, eye rms error 0.120 instead of 0.029 of the
level spacing - and more on one side as the carrier moved off centre, which
biased the centre fit: the sync has 14 negative and 10 positive symbols). A
second boxcar gets back what the limit took, modulo one turn (4800 Hz on
the boxcar); sync detection and the fit stay on the limited one, and a frame
is sliced from the restored one only when the limit squeezed its sync scale
below 0.85 of the exact one (CQPSK 0.65-0.78; C4FM and LSM 0.91-1.0 down to 4
dB SNR - restoring everywhere cost 0.11-0.16 dB on weak C4FM / LSM).

- NID (NAC + DUID, BCH(63,16) by nearest codeword, the last NAC tried first;
  `p25_nid_decode` in `plugins/lib/dig_fec`, one table shared with the
  receiver); option `nac` = only one NAC
- HDU (Golay(18,6) + RS(36,20,17)): talkgroup, algorithm, key
- LDU1 link control (Hamming(10,6) + RS(24,12,13)): group / unit-to-unit
  voice user (talkgroup, source unit ID, emergency, encrypted); with the SF
  bit (implicit MFID) the 8 octets after the LCO are the trunking-block body
  of the same message, decoded by the TSBK parser: adjacent site status
  (LCO 0x22 = TSBK 0x3C), RFSS status (0x23 = 0x3A), group voice channel
  update (0x02 = 0x02). LDU2 encryption sync (RS(24,16,9)); 9 IMBE frames
  each
- TDU, TDULC (Golay(24,12) + RS(24,12,13)) - the call's end; a TDULC's link
  control as LDU1's
- TSDU: trunking signalling blocks (rate-1/2 trellis + CRC-16) - grants
  (group 0x00, its updates 0x02 / explicit 0x03, unit-to-unit 0x04 / update
  0x06, telephone interconnect 0x08 / update 0x09, Motorola patch
  supergroups MFID 0x90 0x02 / 0x03), channel identifiers (channel numbers
  -> MHz), RFSS / network / adjacent site status, secondary control
  channels (implicit 0x39 and explicit 0x29), SNDCP data channel grants
  (0x14) and announcements (0x16), affiliations, registrations, location
  registrations (0x2B); PDU headers.
  Others are shown raw with Verbose. The trellis decoder's branch metric is
  each symbol's absolute error capped at 4 level units (2026-11-19; it was
  the squared error): the receiver's soft symbols have heavy tails near
  threshold (FM clicks, whole turns of phase through CQPSK), and one such
  symbol steered the path. On every control-channel recording with added
  noise the SNR at which half of the blocks decode is 0.3-0.6 dB lower
  (C4FM and simulcast LSM; up to 1.6 dB on weak recordings), clean
  recordings unchanged or better, CRC false accepts unchanged at 2^-16
  (16 million random blocks per input kind)
- Voice: IMBE, built in (port of mbelib, ISC). Played only once the call is
  known to be clear: ALGID 0x80 (HDU or LDU2 encryption sync) or a voice
  user link control without "protected" (on 13 real voice channels the bit
  agreed with the ALGID every time). Encrypted calls are muted, never
  decrypted. A call joined after its HDU at an LDU1 carrying another link
  control (adjacent site status, a channel update) is held: up to 2 LDUs
  wait for the next encryption sync and are then played in order (clear) or
  dropped (encrypted, or the call ended first) - before 2026-11-19 they
  played, and real encrypted calls joined late let 180 ms of noise through.
  The host's voice FIFO starts at 250 ms buffered, so two LDUs released
  together start playback at the same moment, with the same latency, as two
  LDUs 180 ms apart (a model of `push_voice` / `pull_voice`, 20 and 100 ms
  pulls): holding costs nothing, dropping would lose 180 ms
- Events: one "Voice: TG n <- unit" per transmission (the first link control
  that names a unit - a transmission's first LDU1 often names unit 0 -, or
  "TG n" alone after two that name none) and one "Call ended" (the
  terminator, a new HDU, 0.75 s without frames, another unit), no dedup, so
  a unit calling back within seconds gets its own; a terminator's link
  control in hang time (often "TG n <- 0") opens and ends nothing. They
  agree with the talker list (below); "Call termination (TDULC)" is LCO
  0x0F itself

## Phase 2 (two-slot TDMA)

6000 symbols/s, 30 ms timeslots of 180 symbols, 12-slot superframes (360
ms), 4-superframe ultraframes, two logical channels LCH 0 / LCH 1 per RF
channel.

Receiver (`p25p2_phy.cpp`, 8 samples per symbol, one 41-tap complex
low-pass, -6 dB at 5 kHz), one object for either direction:

- Outbound (a site, continuous H-DQPSK): differential detection at the
  symbol centres - kept over a discriminator integrated between the centres
  by measurement (symbol error rate 3.0e-3 against 5.2e-3 at 11 dB SNR).
  Acquired on the 20-symbol S-ISCH sync (0x575D57F7FF): two hits 180 or 540
  symbols apart with the same polarity and carrier, either spectrum
  polarity, the level scale measured, not assumed. Timing, carrier and scale
  are tracked once per slot; an S-ISCH or the S-OEMI sync confirms the grid,
  34 slots (about 1 s) without one give it up. A strong S-ISCH off the grid
  (the sample stream slipped - real captures drop samples) starts a new grid
  at once.
- Inbound (a radio, H-CPM bursts of 28 ms in 30 ms slots): the
  discriminator integrated between symbol centres is a duobinary-like
  partial response (489 Hz x (I[n] + I[n+1]) + tails of 11 Hz) and goes
  into a 16-state sequence detector (equal to the full 64-state one within
  measurement: symbol error rate 7.7e-3 against 7.6e-3 at 12 dB, a quarter
  of the work). Bursts with the 22-symbol sync are found by correlation and
  set a 30 ms grid; voice bursts (pilots only, no sync) are looked for on it
  with each radio's own track of timing, carrier and deviation (up to 4
  radios) - never one average over two radios.

MAC layer (`p25p2.cpp`), one slot at a time:

- I-ISCH (40,9,16) coset code by nearest codeword: superframe slot,
  ultraframe count, channel type; DUID (8,4,4); the burst bit maps
- Descrambling: PN44, x^44 + x^40 + x^35 + x^29 + x^24 + x^10 + 1, seed =
  WACN (20 bits) | system ID (12) | NAC (12), a 360-bit window per
  superframe slot. Voice bursts are scrambled too
- Reed-Solomon over GF(64): the (63,35,29) mother code shortened and
  punctured per burst type, the punctured symbols decoded as erasures
  (`rs64_decode_erasures` in `plugins/lib/dig_fec`), outbound also the
  least reliable hexbits as erasures when the plain decode fails; then
  CRC-12 or CRC-16 - nothing is used before both pass
- MAC PDUs: MAC_PTT, MAC_END_PTT, MAC_ACTIVE, MAC_HANGTIME, MAC_IDLE,
  MAC_SIGNAL and their messages (voice channel user, grant updates,
  MAC_RELEASE, Motorola's 0x80 / 0x83); the Phase 1 TSBK bodies inside them
  (partition 01, the abbreviated forms whose message is the 9-octet TSBK
  body - network / RFSS / adjacent status, secondary control channels
  0x79, channel identifiers, grants 0x40 / 0x42 / 0x44 / 0x46, SNDCP grants
  0x54, acknowledge / deny, de-registration) go through the Phase 1
  parser, which shares its channel identifier table and network identity
  with Phase 2; the longer abbreviated forms (telephone grants 0x48 / 0x49,
  0x68, 0x6B) differ from their TSBKs and stay raw. Explicit / extended
  grants (0xC0, 0xC3, 0xC4, 0xC6) and the explicit multiple grant update
  0x25 are parsed since 3.1 (layouts inferred, *Sources / limits*); the
  rest of partition 11 and messages it doesn't parse are shown raw with
  Verbose
- A TDMA control channel (LCCH on one or both LCHs) as well as traffic
  channels; fact "Channel type"
- Voice: AMBE+2 3600x2450 frames (4 per 4V burst, 2 per 2V) in the same bit
  order as DMR's, the 4V 4V 4V 4V 2V cycle, one vocoder per logical channel,
  through the user's mbelib (root README, "Digital voice codecs"); ESS
  (ALGID / KID / MI) by RS(44,16,29) over the cycle, missing segments as
  erasures
- Encryption: identified (ESS or MAC_PTT ALGID, the voice channel user's
  "protected" service option) and muted, never decrypted. A call joined
  after its MAC_PTT stays muted until it is known to be clear (ALGID 0x80,
  or service options without "protected") - on real encrypted calls joined
  late the first voice bursts came before the ESS and played as noise. Its
  voice bursts until then are dropped, not held (unlike Phase 1's LDUs): a
  clear call joined late loses them

Network identity (the scrambling seed), in this order: the options (`wacn`
+ `sysid` + `nac`), a control channel's network status (a TDMA control
channel's NET_STS_BCST, or a Phase 1 control channel decoded on the same
VFO), blind recovery. Blind recovery needs no control channel: the
Reed-Solomon parity relations of a scrambled FACCH / SACCH burst are linear
in the 44 seed bits, so ONE clean burst gives the seed by linear algebra
over GF(2) (114-132 equations, rank 44; the surplus ones validate it). It
needs a nearly clean burst (at most about 2 wrong symbols); for weak signals
the three options give the seed at once. Facts WACN / System ID / NAC and
"Network ID from" (which source); from inbound bursts alone it says the
identity is assumed (the 243-step inbound offset and the NAC as the colour
code; that offset is never reported as confirmed by its own bursts). The
options also filter: a recovered or broadcast seed that contradicts a given
NAC / WACN / system ID is not taken up, so another system's scrambled bursts
(and MAC_END_PTTs naming another colour code) are dropped (as Phase 1's
`nac` drops frames). They take effect at once while the decoder runs: a
complete identity replaces the seed in use, a NAC alone drops a seed in use
that contradicts it. A recovered scrambler state of all zeros (a clear burst
whose DUID was miscorrected) is never taken for a seed.

Facts: Mode ("Phase 2 TDMA", "(inbound)"), Scrambling, Channel type, LCH 0 /
LCH 1 (or Inbound LCH n) state (Voice: TG n <- unit, Hangtime, Idle,
Control channel, Call ended), Encryption, Last source / talkgroup, the
control channel's system / site / neighbours / channel tables. Events: grant
updates, releases, the network identity, and per talker (as Phase 1's) one
"LCH k voice: TG n <- unit" when a transmission's unit is named (outbound:
the LCH's talker; inbound: the lane's radio) and one "LCH k call ended:"
when that talker ends (MAC_END_PTT / HANGTIME / IDLE, 0.5 s without bursts,
the call timing out, another unit, the same unit keying up again), no
dedup. A span nobody named ("?" in the talker list - on the real traffic
channels 0-30 ms before the unit was named) has no events; a talker named
only by its MAC_END_PTT gets "voice: unit n" and its end together (a
repeated MAC_END_PTT is the same transmission's). Before 2026-11-19 the
events followed the call, with 5 s / 2 s dedup: on 6 of 14 real traffic
channels starts or ends were missing (a unit keying up again, a hang-time
end within 2 s of the last end, a talker replaced inside one call) or an
unnamed span had its own start.
Encryption, Channel type and WACN / System ID / NAC are shared with Phase 1:
whichever phase decoded last shows its value.

## Options

- `nac` (NAC, 3 hex digits): Phase 1 only decodes that NAC; Phase 2 takes it
  as the colour code - with WACN and System ID it gives the scrambling seed,
  alone it only accepts a recovered identity with that NAC
- `wacn` (WACN, 5 hex digits), `sysid` (System ID, 3 hex digits): Phase 2
  only; empty = found from the signal
- `lch` (Timeslot: Both / LCH 0 / LCH 1): which Phase 2 logical channel is
  shown and played. Both = the first to talk is heard until it has been
  quiet for a second (a muted call doesn't hold the audio against a clear
  one). With LCH 0 or LCH 1 an inbound call whose logical channel is not
  known yet is not shown or played. The audio is tagged with its talker
  channel for Voice to Text: Phase 1 channel 0, Phase 2 outbound LCH + 1,
  inbound unknown (-1: inbound radios are packets, not transmissions)
- `phase` (hidden, 3.1): which receiver runs - `1` Phase 1 only, `2` Phase
  2 only, empty (default) both. Set by trunk following from the granted
  channel's identifier (an IDEN_UP_TDMA identifier with 2 or 4 slots = Phase
  2); it saves the other receiver's CPU. A change while running restarts
  the decoder (both receivers count samples from the same reset)

## Trunk following (3.1)

`Info::trunking`; the structured reports of SDK.md *Trunked systems*, from
`p25_trunk.cpp` (one `P25Trunk` shared by both phases). The events and
facts are unchanged.

- System: key = WACN.SYSID in hex ("BEE00.14C") from the network status
  broadcast (TSBK 0x3B, Phase 2 0x7B), name "P25 <key>", site "RFSS r site
  s" from the RFSS status (0x3A, also its link-control form on traffic
  channels); `control` = a trunking block or a TDMA LCCH burst passed its
  checks within the last 10 s. Before the network status the key is ""; a
  traffic channel without one takes the identity of the `wacn` / `sysid`
  options or of its Phase 2 scrambling (not the inbound-only guess). Sent
  when it changes and every 4 s, at most once per input block
- Channels (`trunk_channel`): the site's control channel (0x3A, 0x3B),
  neighbours' control channels (0x3C, same system ID only), secondary
  control channels (0x39, 0x29 with its uplink), SNDCP data channels (0x16);
  again when their identifier arrives later or a minute passed.
  `control_channel` when, on a control channel, the RFSS status names a
  control channel more than 4 kHz from the VFO's RF (at most every 30 s)
- Channel -> frequency: name "<identifier>-<number>" (as the events write
  it), number = the 12-bit channel number; downlink = base + number x
  spacing, for an IDEN_UP_TDMA identifier with N slots base + (number / N)
  x spacing on slot number % N (checked on air: a real update granted
  1-212 and 1-213 of a 6.25 kHz-step TDMA identifier to two talkgroups at
  once - one carrier, both slots); uplink = downlink + the identifier's
  transmit offset, or the explicit forms' channel R; NAN until the
  identifier is heard (the grant still goes out, name + number)
- Grants (every one heard, `update` for the update forms): TSBK 0x00, 0x02
  (two calls = two grants), 0x03 (channel T = the downlink), 0x04, 0x06,
  0x08 / 0x09 (telephone: target = the unit, no source), 0x14 (`data`),
  MFID 0x90 0x02 / 0x03 (target = the patch supergroup); link control 0x02
  on a traffic channel (= TSBK 0x02); Phase 2 0x40-0x46 (via the TSBK
  parser), 0x05 (up to three updates), 0x25, 0xC0, 0xC3, 0xC4, 0xC6.
  `encrypted` / `emergency` from the service options where the message has
  them (0x40 / 0x80). Each passed its FEC and CRC first: TSBK rate-1/2
  trellis + CRC-16, link control RS(24,12,13) after Hamming / Golay, Phase
  2 RS(GF 64) + CRC-12 / CRC-16. Multi-block (MBT) explicit grants are not
  decoded: the receiver hands a PDU over after its header block. P25 has
  no control-channel clear-down: no `grant_end`
- Grant options (all five every time, "" = the default): `nac` = the
  control channel's NAC (the system's on its traffic channels, checked on
  air), `wacn` / `sysid` (the Phase 2 scrambling identity: a TDMA traffic
  channel descrambles from its first burst), `lch` = slot + 1 for a TDMA
  identifier else 0, `phase` = 2 / 1 from the identifier, "" when it is not
  known yet. `bw` 12.5 kHz
- Calls (`call`, a traffic channel's own signalling): Phase 1 (slot -1)
  ACTIVE from the HDU (its talkgroup) and the voice user link control
  (target, source, emergency; encrypted = the muting's decision: ALGID /
  "protected"), repeated at most once a second; ENDED at the TDU / TDULC and
  0.75 s without frames; RELEASED at a TDULC with the call termination LCO
  0x0F, or when the channel carries trunking blocks. Phase 2 (slot = LCH;
  with `lch` only that one): ACTIVE from MAC_PTT and the voice channel user
  messages, ENDED at MAC_END_PTT, MAC_HANGTIME and the call timeout,
  RELEASED at MAC_IDLE and MAC_RELEASE. Inbound bursts are radios, not a
  traffic channel: no calls. Another known target on the same slot =
  RELEASED for the old call, then ACTIVE. A clear-down heard before any
  call of this instance releases nothing (it may be the previous call's on
  a channel just granted again)

## ID separation (DoA per radio)

Phase 1: a transmission = valid HDU / LDU1 / LDU2 frames less than 0.75 s
apart; its talker = the source unit ID of the LDU1 / TDULC link control
(label "TG n" / "unit call to n"), reported with the frames' sample
boundaries, ended by the TDU / TDULC, a new HDU or 0.75 s without frames.
Each unit ID gets its own DoA, history and heat map on the 🗺 Map's 📡 DF
panel. On a repeater output every unit's bearing is the repeater's.

Phase 2 outbound (a site): one transmission per logical channel, on talker
channel LCH + 1 (as DMR's timeslots); talker = the call's source unit, label
"TG n · LCH k · via site" - on a site's output every radio has the site's
bearing. The unit comes from MAC_PTT, from the voice channel user messages
on the SACCH of the ultraframe's superframes 0-2 only (superframe 3's names
the INBOUND talker - confirmed on real calls) and from Motorola's message
0x80, so a call joined after its MAC_PTT is named too. While both timeslots
carry different units the live host drops the frames that touch both spans
(as on a DMR repeater with both slots busy), so each talker's bearing comes
from the time it was alone.

Phase 2 inbound (a radio): a checked burst is a PACKET of a radio - symbols
6-173 of its slot, 12.5 kHz wide at the burst's own carrier offset, label
"TG n · inbound LCH k" - only on evidence that this radio sent it (a lost
packet costs a little averaging, a wrongly attributed one a wrong bearing):
- a MAC_PTT, voice channel user or MAC_END_PTT burst is the packet of the
  unit it names;
- a voice burst continues the lane's named radio when it follows that
  radio's last burst within 0.25 s at its carrier offset (within 60 Hz; the
  receiver measures each burst's offset to 2-13 Hz rms);
- voice bursts before the radio is named wait up to 1.2 s: a voice channel
  user / MAC_END_PTT takes the unbroken chain of them back from itself at
  its own carrier offset; a radio named by its MAC_PTT takes none (the
  MAC_PTT starts its transmission - what came before was another radio's);
- a signalling burst that names nobody is nobody's (a LISTENER may send on
  the talker's inbound SACCH in superframe 3).
Bursts that pass none of these are not reported (a radio whose signalling
was missed gets no talker at all, never the next radio's ID). Two radios
whose 28 ms bursts alternate every 30 ms (one per timeslot) get separate
bearings.

## Tests

Phase 1:
- Real recordings: the dsd-samples P25 discriminator files (FM-modulated to
  cf32) and a live trunked capture (iq_143.0.u8: two radios, unit 2010621 /
  3250, per-talker DoA 59.9 / 200.0 deg end to end) - see
  kraken_doa_v2/docs/dev/doa-per-talker.md.
- Generator `tools/p25_gen.py` (HDU, LDU1 / LDU2 with link control and
  encryption sync, placeholder IMBE frames with valid FEC - Golay(23,12),
  Hamming(15,11), the PN whitening; TDU / TDULC; status symbols; C4FM with
  the raised cosine x inverse sinc shaping filter, +-600 / +-1800 Hz;
  `--algid` / `--kid`: an encrypted call - ALGID and key in the HDU and
  every LDU2, the LC "protected" bit set; `--adj K`: LDU1 number 0, K, 2K,
  ... carries an adjacent site status link control instead of the voice
  user's, as real systems interleave them) -> debug playback entry `p25-three-units` (`tools/testsignals.json`): units
  2010621 (60 deg, twice), 3250 (200 deg, ends with a TDULC) and 1234567
  (300 deg) taking turns on TG 101, each at its own carrier offset and
  level. Offline (`decoder --file scene.cf32 --channels 5 --uca 0.12 --rf
  453612500`, 20 dB SNR): all 58 frames decoded, IMBE 9 / 9 frames clean,
  bearings 60.0 / 200.0 / 300.0 deg; end to end through the playback: see
  kraken_doa_v2/docs/dev/debug-playback.md. The generated symbol levels
  match the real recordings' (equally spaced, outer = 3 x inner).
- More real recordings (2026-10-16; verified against them, not in this
  repo): sigidwiki's P25 IF files (24 kHz IQ, the system named in the file
  name): `P25_C4FM-CC_BEE0014C` 54 s control channel - 723 valid frames,
  WACN BEE00 / system 14C / RFSS 5 site 9, channel tables, neighbours (=
  the file name's BEE00 + 14C); `P25_CQPSK-CC_920650D5` (a simulcast LSM /
  CQPSK control channel, read through the discriminator) - 1454 frames,
  WACN 92065 / system 0D5 (= the name's 92065 + 0D5); `P25-C4FM-VC` 8 s
  voice channel - NAC 0x293, a call TG 1 <- 1 from header to TDU. The
  generator has no independent decoder here: dsdcc (built in scratch for
  DMR / D-STAR / NXDN) has no P25, and OP25 / dsd / dsd-fme need GNU Radio
  out-of-tree builds, itpp, libsndfile, PulseAudio and mbelib
- Unchanged by 3.0 (2026-11-18): `p25-three-units` gives the same events
  and summary before and after (58 frames, 60.0 / 200.0 / 300.0 deg), and
  40,000 random TSBKs parse byte-identically through the old and the
  refactored TSBK parser.
- Out-of-sample test and fixes (2026-11-18 / 19): 13 further public
  recordings (37 channels) the decoder had never been run on - Phase 1
  control channels of six systems (97-100 % of frames, identities
  consistent between a system's channels and with what the posters state;
  on one system all 8 voice grants of a control channel were matched by a
  call on the granted frequency in the same capture), clear and encrypted
  Phase 1 voice (C4FM, CQPSK, wide-pulse), one more Phase 2 traffic channel
  (encrypted: 331 of 333 slots, identity recovered blind, AES-256 muted). No
  new Phase 2 clear-voice, TDMA control channel, simulcast Phase 2 or
  inbound recording could be found. Plugin 2.0 and 3.0 gave identical
  frames, events and audio on the Phase 1 files, so what they exposed were
  old defects, all fixed:
  - CQPSK frames lost to the discriminator limit (above): on the real CQPSK
    voice channel 90 of 90 frames at every carrier offset from -1000 to
    +1000 Hz (before 68-90), the SNR for half of the frames 16.9 -> 14.25
    dB (C4FM 5.2 dB and LSM 7.05 dB unchanged), IMBE frames 684 -> 702,
    unusable ones 39 -> 0. CQPSK through a discriminator stays about 9 dB
    weaker than C4FM in noise. A simulcast control channel's periodic
    "missing" frame (one about every 5.02 s) is not a decoder loss: a frame
    with a perfect NID whose block position (its 98 dibits and 3 status
    slots) holds one fixed waveform (constant and square-wave segments),
    identical every time, 29 bit errors from any rate-1/2 path where random
    data gives 22-32 and the neighbours 0 - nothing to decode, shown as
    nothing.
  - Encrypted audio before the encryption sync: on 3 encrypted calls joined
    late (AES-256, ADP) 0.18 s of noise each -> 0.00 s; the 7 clear voice
    recordings play exactly as long as before, sample-identical where not
    limited. Generator cases (`--algid`, `--adj 2`; clear and encrypted,
    whole and cut at an LDU2, a voice-user LDU1 and an adjacent-site LDU1):
    encrypted 0.00 s in every case (before 0.18 s when cut at the
    adjacent-site LDU1), clear as long as before and the held-then-released
    call sample-identical to the same call not held.
  - Call events: hang-time terminators ("TG n <- 0") opened and ended calls,
    every call got an extra end, and the 5 s / 2 s dedup swallowed the
    start of a unit calling back. Now on all real Phase 1 voice channels 41
    talkers, 41 starts, 41 ends naming the talker's unit (before 62 starts,
    90 ends).
  - Messages decoded (layouts from their parsed siblings, checked on the
    recordings): the link-control forms against the same site's control
    channel (adjacent sites 2261 of 2261, RFSS status 289 of 289, channel
    updates on one system 130 of 130 equal to its grants); 0x39: RFSS / site
    equal the site's own RFSS status on 9 of 9 sites of 5 systems, several
    of its channels announced as control channels by the system's
    adjacent-site lists; 0x29 (one system): uplink and downlink exactly the
    identifier's transmit offset apart; 0x14: every granted channel one 0x16
    announces, and the granted units are the data packets' LLIDs on that
    very channel (a data channel's recording: 5 of 5); 0x2B: RFSS / site
    the broadcasting site's 64 of 64, its groups talkgroups the same system
    grants. Still raw: 0x15 (one message seen), LCO 0x10, vendor LCs, the
    flag / option octets of 0x14 / 0x16 / 0x2B / 0x29 and the service
    classes (not shown)
  - Loud talkers no longer clipped (the output limiter, *Limits* below)
  - Regression (combined build, 2026-11-19): 63 recordings, none with fewer
    frames, talker lists and bearings unchanged; the three P25 scenes 40 /
    160 / 280, 120, 60 / 200 / 300 deg; DMR's scene the same output (180
    frames, 300 / 300 / 60 / 200 deg; its audio limited, *Limits*); 120 s of
    noise nothing; cost 1.03 -> 1.06 % of real time for the receiver change

Phase 2 (2026-11-18; SNR = signal power over the noise power in 12.5 kHz):
- Generator `tools/p25p2_gen.py` (numpy only; its docstring is the
  reference): outbound (a site's traffic channel or TDMA control channel,
  the call framing real sites send - MAC_ACTIVE, MAC_PTT twice, voice,
  MAC_HANGTIME, MAC_END_PTT from the system - and the measured transmit
  shaping) and inbound (one radio's bursts; `uca_scene.py` puts several at
  their bearings). Clear or encrypted calls, group or unit-to-unit, a
  chosen network identity; inbound radios whose signalling a receiver
  missed (`--omit ptt,sacch,end`: joined by voice alone) and listening
  radios (`--listener`: only superframe-3 SACCH bursts naming nobody);
  `--slots` writes the bit-level slots, `--selftest` checks the standard's
  test vectors and reproduces two real over-the-air bursts bit for bit.
- Generator signals through the plugin: outbound decodes fully down to 10
  dB and almost fully to 6 dB (identity recovered blindly: 276 of 285 slots
  and 97 % of the voice bursts at 6 dB; 144 slots at 5 dB; nothing at 4 dB;
  with the identity given as options 221 at 5 dB); a TDMA control channel
  158 of 166 slots at 6 dB; inbound, one radio, 40 bursts: all at 12 dB and
  above, 38 at 10 dB, 19 at 9 dB blind (with the identity given: 35 at 9
  dB, 19 at 8 dB). No false talker or wrong identity at any SNR.
- The receiver alone: outbound symbol error rate 3e-4 at 12 dB, 2e-2 at 8
  dB, lock down to about 7 dB; inbound 1e-2 at 12 dB, breaking down by 8
  dB; slot timing (`start`) within 2 samples.
- Debug playback entries (`tools/testsignals.json`):
  `p25p2-inbound-three-radios` - units 6001 (40 deg, TG 601, LCH 0) and
  6002 (160 deg, TG 602, encrypted, LCH 1) at the same time on the two
  timeslots, then 6003 (280 deg, LCH 0), the identity recovered from the
  bursts; `p25p2-site-two-timeslots` - a site at 120 deg with 2101 -> TG
  101 in the clear on LCH 0 and, overlapping, 2102 -> unit 2103 encrypted on
  LCH 1, then a Phase 1 call of 2104 on the same channel. Offline
  (`--channels 5 --uca 0.12`): 40.0 / 160.0 / 280.0 deg (errors <= 0.2 deg
  down to a scene SNR of 6 dB; with the second radio 10 dB weaker it is lost
  first, the others unaffected; no packets dropped for overlap), and 120.0
  deg for every talker of the site scene. End to end through a test
  kraken_doa's debug playback: 40.0 / 160.0 / 280.0 deg (718 packets, 716
  used, 2 dropped at the loop wrap, none for overlap), 120.0 deg for the
  site scene's three talkers, `p25-three-units` still 60 / 200 / 300 deg.
  The page's own Check button (headless browser) was not run.
- Inbound attribution (scratch scenes, 2026-11-18): a radio joined by its
  voice alone (70 deg, signalling left out, 25 of its voice bursts decoded
  in an open call) followed 0.35 s later on the same timeslot by another
  radio (250 deg, named by its MAC_PTT, or with the MAC_PTT left out by its
  SACCH): the first radio gets no talker at all and the second 40 (38)
  packets at 250.0 deg - also with both at the same carrier offset and at a
  scene SNR of 8 dB (before the fix the second radio took 11 of the first
  one's bursts). A listener's superframe-3 SACCH bursts on a talker's
  timeslot (200 deg) are nobody's packets; the talker stays at 30.0 deg
  with 85 packets (before: 88, three of them the listener's).
- Real recordings (public, not in this repo; all OUTBOUND - no inbound
  recording could be found anywhere): 21 files from 4 sources decode with
  no false events.
  - A 24 s IQ recording of a US statewide system's traffic channel during
    voice calls: 799 of 799 slots valid, WACN BEE00 / system 942 / NAC 941
    recovered blindly (= the identity its poster gives), 4 transmissions on
    both timeslots (two overlapping), 1172 of 1172 voice frames clean, 20 s
    of audio with no vocoder errors.
  - Wideband captures of an Australian state network's (encrypted) traffic
    channels: 93-99 % of slots valid on the good carriers (57 % at a band
    edge, 86-87 % on a capture with sample slips), AES-256 identified and
    muted on every call, NAC 0x161 (= the 353 of the capture's ground-truth
    list), 8 of 12 decoded talkgroup / unit pairs present in that list.
  - sigidwiki's discriminator recordings: a TDMA control channel 174 of 174
    slots (WACN 92715 / system 1F8 / NAC 02C from its network status and,
    independently, blindly); voice-channel recordings 96-100 %.
  - Idle traffic channels decode only their SACCH (16-21 % of the slots):
    their other bursts carry a DUID value the standard lists as reserved,
    content unknown.
- Settled on the real recordings: voice bursts ARE scrambled (0.2 % of
  18,398 frames have a clean Golay c0 as received, 95.7 % after
  descrambling); the voice frame order is DMR's (93.4 % of frames with no c0
  error); MAC_PTT is sent twice with the offset field 1 then 0, and the
  first voice burst follows exactly there (121 of 124); MAC_PTT's source /
  group agree with the voice channel user messages (212 of 216, on the SACCH
  of superframes 0-2); MAC_END_PTT comes after the hang time, from the
  system (source 0xFFFFFF), carrying the NAC; superframe 3's SACCH names
  another unit or 0 (the inbound talker); ESS: 840 decodes, ALGID 0x80 on
  clear calls, 0x84 on encrypted ones; the transmit filter is the raised
  cosine (alpha 1, -6 dB at 3.6 kHz) applied to impulses (measured spectrum
  -5.4 dB at 3.6 kHz, -22 dB at 6 kHz); Motorola (MFID 0x90) messages 0x80
  / 0x83 are 8 / 7 octets, not length-prefixed.
- Cost (x86_64, i7-11800H): the whole plugin 0.9-1.2 % of real time (Phase
  1 path 0.6 %, Phase 2 receiver + MAC 0.25-0.33 %; the Phase 1-only plugin
  was 0.7 %). Not measured on a Raspberry Pi.
- No false output: 300 s of noise, FM, a carrier and random 4FSK - nothing.
  Address / undefined-behaviour sanitizer runs over all recordings and
  generator cases clean.
- Call events per talker (2026-11-19): on the 14 real traffic channels
  (outbound) every named talker has one start and one end naming its unit,
  except 6 talkers still open when their recording ends (no end yet); the
  "?" spans (0-30 ms each, before the unit was named) have none. Inbound
  (the scenes above and 30 scratch variants of the three-radio scene, the
  second radio's level and the scene SNR varied): one start and one end per
  radio; a radio heard only through its MAC_END_PTT (once: the second radio
  10 dB weaker) now gets them too.
  Talker spans, packets, valid slots and audio identical before and after
  on every recording and scene.

Trunk following (3.1, 2026-11-20):
- Generator `tools/p25_gen.py --control` (TSDUs of 1 or 3 trunking blocks,
  the broadcasts, grants and updates on a timeline; `--release`: the call
  termination TDULCs after a call; `--u2u`). Its TSBK coding against real
  control channels: blocks demodulated from three sites of two public
  systems (an independent slicer, not the plugin), re-encoded with the
  generator's trellis / interleaver / CRC - 361 of 361 bit for bit; frame
  spacing 37.5 ms (1 block) / 75 ms (3 blocks) as on air. The NID's 64th bit
  is even parity on one system and inverted on every frame of the other
  (the decoder ignores it; the generator sends even parity)
- Scene `p25-trunk-three-calls` (`tools/testsignals.json`, 14 s, 336 MB):
  offline at the control channel's offset: system, the site's and a
  neighbour's channels, the three grants with 851.100000 / 851.600000 /
  851.350000 MHz (slot 1, `lch` 2, `phase` 2) and their uplinks 45 MHz
  below, 0.32-0.36 s before the first ACTIVE; each traffic channel at its
  offset with the grant's options: ACTIVE -> ENDED -> RELEASED (Phase 1 at
  the TDULCs 1 s after the call, Phase 2 at MAC_IDLE after the hang time),
  talkers 1001001 / 1001002 / 1001003 at 40.0 / 200.0 / 120.0 deg. Through
  `tools/digi_test` (the real engine): the same SYSTEM / CHANNEL / GRANT /
  CALL reports, every option delivered
- Control channel in noise (60 s, 24 grants, 120 grant messages; SNR in
  12.5 kHz): all at 12 dB and above, 119 at 8-9 dB, 108 at 7 dB, 61 at 5 dB
  (half), 8 at 4 dB, none at 3 dB; carrier +-1 kHz: 119 / 118 at 8 dB; the
  single-block form the same (60 at 5 dB). No wrong grant at any SNR or
  offset. Corruption (60 s each): 328 segments replaced by noise, 980
  bursts of 1-8 random symbols, 800 TSDUs with a valid NID and random
  blocks, 120 s of noise - no wrong grant, no event from the random parts.
  Every grant comes from a block that passed its FEC and CRC (TSBK: trellis
  + CRC-16; link control: Hamming / Golay + RS(24,12,13); Phase 2: RS +
  CRC-12 / CRC-16)
- Existing scenes and recordings unchanged: `p25-three-units`,
  `p25p2-inbound-three-radios`, `p25p2-site-two-timeslots` give identical
  events, facts, talkers and bearings before and after (plain and verbose);
  the generator writes them byte for byte as before
- Off air (2026-11-20, KrakenSDR recordings of two systems of one network,
  300 s each, 2.4 MHz at UHF 499.8 and VHF 142.3 MHz, not in this repo;
  all voice encrypted): 13 control channels, 14 traffic channels. Every
  control channel's and traffic channel's events and facts identical to
  3.0's. All 329 grants of the reference timeline (3.0's events) come as a
  `(grant)` with the same channel, frequency, talkgroup and source
  (UHF 118, VHF 211), with 10-11 messages per grant on average (the
  grant, then its updates); uplink offsets -4506.25 kHz
  (UHF) and -3000 kHz (VHF), from the identifiers (no uplink inside the
  recordings to check the sign against). The NAC of the control channel
  is the NAC of its system's traffic channels (0x233 / 0x226 for the two
  systems); with the grant's options the 14 traffic channels decode
  exactly as without. Calls: of 332 voice transmissions 331 got an ACTIVE
  of their talkgroup 0.07-0.08 s after the first frame (0.17-0.36 s when
  the header was missed), 328 an ENDED within 0.06 s of the last frame (up
  to 0.75 s when the terminator was missed: the gap); 292 were followed by
  a TDULC call termination 0.04-0.05 s after the terminator -> RELEASED (no
  hang time: these systems grant every transmission anew) - on one channel
  only 7 of 23. Grant -> first voice frame: median 0.22
  s, 10-90 % 0.14-0.28 s (UHF) / 0.02-0.27 s (VHF); 85 of the 319 matched
  ones start up to 77 ms BEFORE the grant's TSDU has been received (a
  transmission keyed while the channel was being granted). No TDMA grant
  on air (the second system announces TDMA identifiers, grants nothing)
- Messages the decoder could not parse on those control channels: MFID
  0xD8 opcode 0x34 (279, constant payload), TSBK 0x2D (3). No MBT / PDU on
  any real control channel (the receiver hands a PDU over after its header
  block, so multi-block explicit grants are not decoded)

## Sources / limits

Phase 1: TIA-102.BAAA / .AABF / .AABC as published in the open-source P25
community's documentation (DSD, OP25 - used as documentation only); IMBE and
the frame interleave tables from mbelib / DSD (ISC, notice in
`lib/mbe_tables.hpp`; Phase 2's AMBE+2 frames use the same file's DMR
tables rW..rZ).

Phase 2 was written clean-room (the procedure of `docs/dev/licensing.md`): a
code-free fact sheet, then implementers who saw only that fact sheet, and a
separate author for the generator. Its sources:
- TIA-102.BBAC-A (the Phase 2 MAC layer standard): slot and burst
  structure, sync words, the ISCH and DUID codes, the Reed-Solomon codes,
  the bit maps.
- The Aeroflex / Viavi application note "Understanding and Testing P25
  Phase 2 TDMA" and M. Simon, "Standard APCO25 Physical Layer of the Radio
  Transmission Chain" (KTH thesis, 2013), for the two modulations.
- Measurements on the public recordings (above).
- **Facts taken from other projects** as documentation of facts - numbers
  and layouts, no code: from SDRTrunk (GPL-3.0) the MAC PDU opcode values,
  the MAC_PTT / MAC_END_PTT field layouts, the MAC message length table and
  the layouts of the voice channel user, grant update, MAC_RELEASE and
  paging messages (TIA-102.BBAD-A, the message standard, was not
  available); the CRC-12 polynomial 0x897, known from OP25; from
  GopherTrunk's (Apache-2.0) published PN44 notes the scrambler polynomial
  and seed layout, the inbound 243-step seed advance and the all-zero-seed
  rule. Confirmed on the real recordings since: the PN44 polynomial and
  seed layout (blind recovery gives the identity the recordings' posters
  give), CRC-12, the MAC_PTT / MAC_END_PTT / MAC_ACTIVE / MAC_HANGTIME
  opcodes (the call framing real sites send), the MAC_PTT and MAC_END_PTT
  layouts, the voice channel user layout (it agrees with MAC_PTT), 9 of the
  length table's entries. Not confirmed: the grant update, MAC_RELEASE and
  paging layouts, the 243-step advance, the all-zero-seed rule. No
  similarity audit against those projects' code was run.
- Trunk following (3.1): the TSBK grant / update / telephone layouts from
  TIA-102.AABC (0x03's channel T = the downlink, as established on real
  0x29s); [GPL-fact OP25 / SDRTrunk] Motorola MFID 0x90 opcodes 0x02
  (patch voice channel grant, GRP_V_CH_GRANT's layout) and 0x03 (its
  update, GRP_V_CH_GRANT_UPDT's) - 0x03 seen once on a public Motorola
  control channel, naming the supergroup the same site's regroup command
  had just formed on a channel of its tables; 0x02 not seen. Phase 2
  explicit / extended grants 0xC0 / 0xC3 / 0xC4 / 0xC6 / 0x25 [INFERRED]
  from the length table [GPL-fact] and the explicit siblings' fields,
  every length adds up; not seen on air. The TDMA channel arithmetic
  (number / slots, slot = number % slots) is the standard's; checked on a
  public recording's grant update giving the two slots of one carrier to
  two talkgroups at once

Limits:
- The whole INBOUND path - H-CPM parameters and timing, the inbound sync and
  bit maps, the inbound scrambling (the seed advanced 243 steps, one 360-bit
  window per slot), the inbound MAC contents - is checked only against the
  plugin's own generator: no inbound recording exists publicly. The
  receiver and the generator were written by separate people from the same
  fact sheet, so they check each other's reading of it, not the fact sheet.
  The decoder therefore solves the inbound scrambler state from the bursts
  rather than relying on the 243-step rule, and shows a network identity
  from inbound bursts as assumed.
- Not seen on air: unit-to-unit and emergency calls, channel grants on a
  TDMA control channel, dual control channels, MAC_RELEASE, the
  all-zero-seed rule, the explicit / extended message forms, the inbound
  control channel; for trunk following also no unit-to-unit, telephone or
  data grant, no TDMA traffic channel granted from a control channel, no
  control channel move (`control_channel`), no multi-block grant.
- Blind identity recovery needs a nearly clean burst; give the identity as
  options for weak signals.
- Loud talkers: the vocoders run at mbelib's fixed gain (x7 into 16 bits,
  no gain control), and one unit of the statewide recording was loud at the
  source - its coded gain 12-18 dB above the other timeslot's talker, 149
  of its 486 error-free AMBE+2 frames up to +18 dB over full scale, 5 % of
  its samples clipped (mbelib's own 16-bit output clips the same 5 %). The
  shared output limiter (`VoiceLimiter`, `plugins/lib/dig_vocoder`,
  2026-11-19; also DMR, D-STAR, NXDN, YSF) now takes such frames to -1 dBFS
  instead (that call: rms -10.0 -> -18.4 dBFS, 0 % clipped; the CQPSK voice
  channel 0.09 % -> 0); frames within full scale pass bit for bit. The
  host's AGC then levels it.
- Not run on this change (3.0, 3.1): the Windows (MinGW) build and the
  Linux-glibc cross-check (zig); gcc with the Makefile's flags is
  warning-free.
- Not decoded: encrypted content.

## Files

`decoder.cpp` (the plugin: both receivers on one input, options);
`p25_trunk.cpp` / `.hpp` (trunk following: system, channels, grants, calls);
`p25.cpp` / `.hpp` (Phase 1 protocol; the TSBK parser, channel identifiers
and network identity Phase 2 shares); `p25p2_phy.cpp` / `.hpp` (Phase 2
receiver); `p25p2_slot.hpp` (the slot interface between receiver and MAC
layer); `p25p2.cpp` / `.hpp` (Phase 2 MAC layer); `tools/p25_gen.py`,
`tools/p25p2_gen.py` (generators), `tools/testsignals.json`.
