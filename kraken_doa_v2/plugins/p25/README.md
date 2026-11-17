# P25 decoder plugin

APCO P25 Phase 1 (TIA-102.BAAA, 12.5 kHz C4FM, CQPSK / LSM read through the
discriminator) and Phase 2 (two-slot TDMA, TIA-102.BBAC-A: the sites'
H-DQPSK and the radios' H-CPM) on one 12-24 kHz VFO. Version 3.0
(2026-11-18) added Phase 2. Both receivers run on the same 48 kHz input all
the time, so the two phases are told apart without a setting: a trunked
system's traffic channel can carry Phase 1 and Phase 2 calls in turn
(tested: a Phase 1 call, a Phase 2 call and a Phase 1 call on one channel,
all decoded).

## Phase 1

The 4FSK receiver (`plugins/lib/dig_fsk4.cpp`: integrate-and-dump filtered
discriminator, frame sync correlation, levels / timing / carrier offset from
the sync) hands each frame to `p25.cpp`:

- NID (NAC + DUID, BCH(63,16)); option `nac` = only one NAC
- HDU (Golay(18,6) + RS(36,20,17)): talkgroup, algorithm, key
- LDU1 link control (Hamming(10,6) + RS(24,12,13)): group / unit-to-unit
  voice user (talkgroup, source unit ID, emergency, encrypted); LDU2
  encryption sync (RS(24,16,9)); 9 IMBE frames each
- TDU, TDULC (Golay(24,12) + RS(24,12,13)) - the call's end
- TSDU: trunking signalling blocks (rate-1/2 trellis + CRC-16) - grants,
  channel identifiers (channel numbers -> MHz), RFSS / network / adjacent
  site status, affiliations, registrations; PDU headers
- Voice: IMBE, built in (port of mbelib, ISC); encrypted calls (ALGID not
  0x80 or the LC "protected" bit) are muted, never decrypted

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
  (partition 01: network / RFSS / adjacent status, channel identifiers,
  grants) go through the Phase 1 parser, which shares its channel identifier
  table and network identity with Phase 2. Messages it doesn't parse are
  shown raw with Verbose
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
  late the first voice bursts came before the ESS and played as noise

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
control channel's system / site / neighbours / channel tables. Events: a
call's start and end per LCH, grant updates, releases, the network identity.
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
  the raised cosine x inverse sinc shaping filter, +-600 / +-1800 Hz) ->
  debug playback entry `p25-three-units` (`tools/testsignals.json`): units
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
  all-zero-seed rule, the explicit / extended message forms (shown raw with
  Verbose), the inbound control channel.
- Blind identity recovery needs a nearly clean burst; give the identity as
  options for weak signals.
- One unit's audio in the best recording was about 8 dB louder than the
  other's and clips with the shared AMBE output scaling.
- Not run on this change: the Windows (MinGW) build and the Linux-glibc
  cross-check (zig); gcc with the Makefile's flags is warning-free.
- Not decoded: encrypted content.

## Files

`decoder.cpp` (the plugin: both receivers on one input, options);
`p25.cpp` / `.hpp` (Phase 1 protocol; the TSBK parser, channel identifiers
and network identity Phase 2 shares); `p25p2_phy.cpp` / `.hpp` (Phase 2
receiver); `p25p2_slot.hpp` (the slot interface between receiver and MAC
layer); `p25p2.cpp` / `.hpp` (Phase 2 MAC layer); `tools/p25_gen.py`,
`tools/p25p2_gen.py` (generators), `tools/testsignals.json`.
