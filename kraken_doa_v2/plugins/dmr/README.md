# DMR decoder plugin

DMR Tier II / III (ETSI TS 102 361-1 / -2 / -4), 12.5 kHz 4FSK, two
timeslots, on a 12-24 kHz VFO. The 4FSK receiver (`plugins/lib/dig_fsk4.cpp`:
RRC 0.2 filtered discriminator, the BS / MS / direct-mode sync words, voice
bursts B..F extrapolated from the last sync) hands each burst to `dmr.cpp`:

- CACH TACT (timeslot, Hamming(7,4)); slot type (colour code + data type,
  Golay(20,8), at most 2 corrections)
- voice LC header / terminator with LC (BPTC(196,96) + RS(12,9)): talkgroup
  or unit call, source radio ID, emergency / privacy flags
- voice superframes: EMB (QR(16,7)) + the embedded LC over bursts B..E
  (Hamming(16,11) + column parity + 5-bit checksum); GPS and talker alias LCs
- CSBK (grants, preambles, Tier III control-channel chatter with verbose),
  MBC (header + continuation blocks, the last block's CRC), PI and data
  headers, idle bursts; the CACH short LC (4 CACH bursts, Hamming(17,12)
  rows + column parity + CRC-8): C_SYS_Parms / P_SYS_Parms
- Tier III trunking (`dmr_trunk.cpp`, ETSI TS 102 361-4) for trunk
  following - see below; Motorola Capacity Plus: the rest channel
- Voice: AMBE+2 through a user-installed mbelib (README "Digital voice
  codecs"); privacy calls are muted, never decrypted. Option `slot` (talkers,
  calls, events and audio of that timeslot only; the other slot's radio is
  reported as "?" so its samples stay out of the DoA); hidden options set
  by trunk following: `cc` (only bursts of that colour code - also a CACH
  short LC counts only from a burst whose own slot type / EMB carried it,
  and none from a voice burst A, which has no colour code) and `traffic`
  (this VFO follows a payload channel: never `control = true`, never
  `control_channel`)

## Trunk following (Tier III)

`Info::trunking`. On a TSCC (trunked station control channel):

- System: the C_SYScode of C_ALOHA / C_BCAST, or of the CACH short LC
  (C_SYS_Parms on a TSCC, P_SYS_Parms on a payload channel - so a payload
  channel names its system too). `trunk_system` key = the NETWORK: model +
  network code (`Huge.1`, `Small.37`; the model sets the NET / SITE split,
  table 6.5), site = `site <SITE>` (raw value), `control` while TSCC PDUs
  were heard on a slot the instance follows in the last 5 s (C_SYS_Parms
  counts only without a `slot` option: it says one of the carrier's slots
  is a TSCC; never with `traffic`). Sent on a change - at once after a
  reset - and every 5 s
- Grants: PV / TV / BTV / PD / TD / PV_DX / PD_DX / PD_MI / TD_MI (CSBKO
  0x30-0x38; 0x38 from an MS is BS_Dwn_Act) on a TSCC or a payload channel
  -> `grant`: channel `LCN <n>` / number n, slot = the LCN bit, group (TV,
  BTV, TD, TD_MI), emergency, `update` = Late_Entry (TV / BTV), `data` for
  the data grants, a duplex grant's Call Direction turned into called /
  calling, options `slot` = 1 / 2, `traffic` = 1 and - with absolute
  parameters - `cc`. A single-slot instance reports only its own slot's
  grants.
  Logical channel 0xFFF = an MBC with the CG_AP block: downlink = its RX
  (the MS receive) frequency, uplink = its TX, the payload channel's colour
  code. An MBC's continuation must be the next burst of the header's slot:
  60 ms (+-3 symbols) after it; one 30 ms after it (the other slot's, its
  TACT misdecoded) is ignored, a later one breaks the MBC. The grant PDUs
  carry no privacy flag: `encrypted` comes only from
  the payload channel's own LC
- Channel numbers -> Hz only where the system says: C_BCAST Chan_Freq (MBC
  with BC_AP), CG_AP / MV_AP / VN_AP blocks; the plugin keeps them
  (`trunk_channel` with downlink and uplink) and uses them for later grants
  of that channel. The fixed / flexible channel plans of annex C are
  personalisation data, never on air: otherwise `freq_hz` NAN, and the
  user's channel table (or the host's learning) maps `LCN <n>`
- C_BCAST: call timers, adjacent sites and vote-now advice (facts +
  `trunk_channel` of the neighbour's TSCC), announce / withdraw TSCC,
  local time, general site parameters, mass registration (verbose)
- C_MOVE with Mask 0 (every radio) -> `control_channel`; a C_MOVE for some
  radios only is an event
- Payload channel, per timeslot: `call(ACTIVE)` from the voice LC header /
  embedded LC (late entry: a voice burst without an LC yet gives an ACTIVE
  with no target) - group or unit from the FLCO, encrypted = the privacy
  bit of the LC's service options or the EMB's PI; `ENDED` at the
  terminator with LC (its hang-time repeats every 60 ms are skipped) or
  1 s without voice; `RELEASED` at P_CLEAR (repeated P_CLEARs: one per 2 s).
  P_PROTECT is an event. With `slot` set only that timeslot is reported
- Motorola Capacity Plus (no public standard): CSBKO 0x3E FID 0x10 on the
  rest channel -> `trunk_system` key `CapPlus.CC<colour code>` (nothing
  better is on air: two systems on one colour code in reach share it),
  `control_channel` `LCN <n>` when the rest channel number changes - only
  after 3 status CSBKs in a row name the new one (both slots count: up to
  ~6 s on an idle rest channel's beacons) and at most once per 5 s. Its
  active-channel / group list is not decoded (no recording with traffic),
  so there are no Capacity Plus grants. Connect Plus (FID 0x06) and Hytera
  XPT: only named

## ID separation (DoA per radio)

Every timeslot is a talker channel (0 = MS / direct without a slot, 1, 2):
a transmission = its confirmed bursts less than 0.5 s apart, talker = the
full / embedded LC source ID ("TG n · slot N", " · via repeater" for
BS-sourced bursts; "?" for a busy slot whose radio isn't named yet). Two
radios talking at once on the two timeslots keep their own spans; on a
repeater output every radio's bearing is the repeater's, in direct mode
(TDMA direct-mode syncs) each radio's own.

## Tests

- Real recordings: dsd-samples dmr_1 / dmr_2 (MS) and dsdcc dmr_it_8 (BS
  slot 2), FM-modulated to cf32 - see kraken_doa_v2/docs/dev/doa-per-talker.md.
- Generator `tools/dmr_gen.py` (BS: continuous CACH + bursts, both
  timeslots, idle bursts; DMO: one timeslot, ramped bursts; voice LC header,
  superframes with EMB + embedded LC, terminator; placeholder AMBE+2 frames
  with valid FEC - Golay(24,12) / Golay(23,12) + PN; RRC 0.2, +-648 / +-1944
  Hz) -> debug playback entry `dmr-repeater-and-direct`: a repeater at 300
  deg carrying 3100001 (slot 1) and, overlapping it, 3100002 (slot 2), then
  3100011 (direct, TS1, 60 deg) and 3100012 (direct, TS2, 200 deg). Offline
  (20 dB SNR): every burst decoded (burst A carries no checked signalling,
  so it isn't counted as a valid frame), bearings 300.0 / 300.0 / 60.0 /
  200.0 deg; end to end: kraken_doa_v2/docs/dev/debug-playback.md.
- Generator through an independent decoder (2026-10-16): dsdcc's
  `dsdccx` (f4exb, GPL - built in scratch, run only), the generator's
  output FM-demodulated to 48 kHz at 25 dB SNR: the repeater (BS) call
  pair - `BS S1: *01 VOX 03100001>G00000001 S2: *01 VOX 03100002>G00000002`,
  colour code 1, voice LC header / superframes / idle bursts on both slots
  as generated. The direct-mode (DMO) transmissions can't be checked there:
  dsdcc handles only the BS / MS syncs, not the TDMA direct-mode TS1 / TS2
  ones (its output on them is noise)
- Found with the generator (2026-10-16): random voice payload correlates >=
  0.8 with a sync word now and then. A false VOICE sync off the 30 ms grid
  of a confirmed burst sequence moved the extrapolation of bursts B..F (now
  ignored - `Fsk4Receiver::on_sync`), and a false data sync of another kind
  (a direct-mode pattern inside a repeater's burst) set the kind + slot the
  next extrapolated burst was read with even though its slot type failed
  (now only checked bursts set it - `DmrProto::decode`). On the real
  recordings: dmr_1 263 -> 269 valid bursts, dmr_it_8 604 -> 605, dmr_2
  unchanged, the talkers unchanged.

- Trunking, generator (2026-11-20): `dmr_gen.py --mode tscc` (C_ALOHA,
  C_BCAST, Chan_Freq MBC, grants twice then late entry once a second,
  C_MOVE, C_SYS_Parms in the CACH) and `--mode bs --sys ... --clear S`
  payload channels -> catalogue entry `dmr-tier3-trunk-three-calls` (14 s,
  2.4 MHz: TSCC at +300 kHz, channel 301 at -500 kHz with one call per
  timeslot from two transmitters, channel 302 at -100 kHz from a grant
  with absolute parameters, colour code 2). Offline: the TSCC gives the
  system (Small.37 site 5), 3 channels (301 / 302 with downlink and
  uplink exact, 320 without), 15 grants (9 late entry) of 3 calls with
  the frequencies, slots and the `cc` option as generated; each payload
  channel with the grant's options (`--opt slot=1|2 [--opt cc=2]`):
  ACTIVE at the call's first burst, ENDED at the terminator, RELEASED at
  the P_CLEAR (+0.6 / +1.0 s), talkers 3100001 40.0, 3100003 160.0,
  3100002 220.0 deg (each the other slot's talker as "?"; with the two
  calls overlapping the offline estimator - which takes a talker's whole
  span - mixed them: 43.8 / 138.3 deg, so the entry has them one after
  the other). `tools/digi_test` delivers the same SYSTEM / CHANNEL /
  GRANT / CALL reports through the engine
- Robustness (TSCC, 12 s, 15 grant messages, SNR in 12.5 kHz, 3 noise
  seeds each): all 15 down to 8 dB, 10-12 at 6 dB, none at 4 dB; +-1 kHz
  carrier offset: all 15 (offset measured -1001 / +1002 Hz). Damaged
  grants (114: 1..14 flipped BPTC bits, a wrong CRC mask, an MBC header
  without / with a broken continuation or a CG_AP of another CSBKO, a
  burst cut short): 0 false grants; BPTC repaired 6/6 with up to 5 flips,
  2-5/6 with 6-14, every other kind none. Each grant form is guarded by
  BPTC(196,96) + CRC-CCITT (mask 0xA5A5; MBC header 0xAAAA, the last
  block's own CRC over the continuation, its CSBKO must repeat the
  header's), Cdeftype 0 and 50-999 MHz for absolute parameters
- Real Tier III network (2026-11-20, 600 s off air, 2.4 MHz of UHF, one
  antenna): three TSCCs (sites 1-3 of one network) and seven payload
  carriers named by their CACH
  P_SYS_Parms (sites 1-3). Only C_BCAST Adjacent_Site is sent (no
  Chan_Freq, no MBC, no absolute parameters): grants carry logical
  channels only. From the grants and which carrier carried the granted
  talkgroup: one linear plan, f = base + LCN x 12.5 kHz, for all 10
  channels seen (the base is personalisation data); uplinks are not on air.
  13 voice grants (TV_GRANT, sent twice 60 ms apart, then as late-entry
  grants 4 at a time every 360 ms until the clear-down): 12 matched a call
  of the same talkgroup on the granted carrier and slot 0.34-0.58 s later
  (median 0.50; one weak carrier 3.5 s: its first LCs lost), 1 never keyed
  up (P_CLEAR to the talkgroup 2.0 s after). A transmission ends with a
  terminator (repeated every 60 ms through the hang time), a new one
  inside the hang time is the same call; 1.98 s after the last terminator
  the slot is cleared with 3 P_CLEARs 60 ms apart, addressed to the
  talkgroup (data calls: ALLMSI). The TSCC carriers' slot 2 carries a
  Motorola-FID CSBKO 0x19 (aloha-like, its own system code, about 300 per
  600 s) and some data; data grants (PD_GRANT / PD_GRANT_MI, 102 at site
  1) go to the payload channels and the TSCC's own slot 2 (composite).
  Not parsed (shown with Show all messages): FID 0x10 CSBKO 0x19, C_ACKD
  / C_AHOY (standard, no trunking content). A 30 s probe of another
  Tier III network showed call timers and vote-now advice too
- Capacity Plus: a 30 s rest-channel recording (no traffic): the rest
  channel's CSBKO 0x3E on both slots, its timeslot bit matching each
  (system `CapPlus.CC1`, rest channel 1); nothing to check grants against
- Review fixes (2026-11-20; plugin 2.2; each test fails with 2.1 and passes
  with 2.2): a CC 2 payload channel with a CC 1 TSCC 20 dB weaker on
  its frequency, `slot=1 cc=2`: was `control=1` (C_SYS_Parms from the
  CACH) every 5 s before its own carrier keyed up, now only its own
  `control=0`; a composite TSCC whose slot 2 carries a call, `slot=2`: was
  `control=1` and slot 1's 6 grants, now `control=0`, no grants, the call
  as before; 24 pairs of interleaved MBC grants (slot 1 + slot 2, the same
  CSBKO) with 18 of the slot-2 continuations' TACTs corrupted to slot 1:
  was 18 grants with one call's target on the other's channel, now 0 (24
  slot-1 grants right, the 6 intact slot-2 ones right); Capacity Plus with
  one glitching status CSBK and two real moves 1 s apart: was 4 moves (to
  the glitch and back), now 2 (the second 5 s after the first).
  `tools/digi_test --retune` from one network (Chan_Freq for channel 301)
  to another: after the RESET the new network's SYSTEM comes before its
  first GRANT, and its channel 301 has no frequency (the old table is gone)
- Every earlier test unchanged (dmr_1 269, dmr_2 294 valid bursts and their
  talkers, the `dmr-repeater-and-direct` entry's talkers and bearings),
  except: the GopherTrunk voice-terminator fixture's hang-time terminator
  train is 1 talker instead of 23 of 27 ms; opcode names 0x2E / 0x2F

## Sources / limits

ETSI TS 102 361 (public; trunking: -4 V1.12.1 clauses 6.3.2.2.1, 7.1.1.1,
7.1.1.3, 7.1.2, 7.2.19, 7.2.42, annexes A.4 / B.1 / C; MBC and short LC:
-1 V2.6.1 7.4, 7.1.4, B.2.3, B.3.3, B.3.7, B.3.12); DSD / dsdcc / sdrtrunk
as documentation only; the AMBE frame interleave tables from DSD (ISC,
notice in `lib/mbe_tables.hpp`). Capacity Plus (`capplus()` in
dmr_trunk.cpp): the CSBKO 0x3E octet-2 layout is a [GPL-fact] from
DSD-FME / SDRTrunk, its timeslot bit confirmed on a recording, the rest
channel number [UNVERIFIED]. A repeater INPUT with both slots busy can't be split
(MS bursts carry no slot); two radios in direct mode on both timeslots at
once: the receiver follows one burst sequence at a time.
