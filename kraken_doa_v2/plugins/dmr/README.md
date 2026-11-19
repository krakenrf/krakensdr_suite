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
  reported as "?" so its samples stay out of the DoA); hidden option `cc`
  (only bursts of that colour code - set by trunk following)

## Trunk following (Tier III)

`Info::trunking`. On a TSCC (trunked station control channel):

- System: the C_SYScode of C_ALOHA / C_BCAST, or of the CACH short LC
  (C_SYS_Parms on a TSCC, P_SYS_Parms on a payload channel - so a payload
  channel names its system too). `trunk_system` key = the NETWORK: model +
  network code (`Huge.1`, `Small.37`; the model sets the NET / SITE split,
  table 6.5), site = `site <SITE>` (raw value), `control` while TSCC PDUs
  were heard in the last 5 s. Sent on a change and every 5 s
- Grants: PV / TV / BTV / PD / TD / PV_DX / PD_DX / PD_MI / TD_MI (CSBKO
  0x30-0x38; 0x38 from an MS is BS_Dwn_Act) on a TSCC or a payload channel
  -> `grant`: channel `LCN <n>` / number n, slot = the LCN bit, group (TV,
  BTV, TD, TD_MI), emergency, `update` = Late_Entry (TV / BTV), `data` for
  the data grants, a duplex grant's Call Direction turned into called /
  calling, options `slot` = 1 / 2 and - with absolute parameters - `cc`.
  Logical channel 0xFFF = an MBC with the CG_AP block: downlink = its RX
  (the MS receive) frequency, uplink = its TX, the payload channel's colour
  code. The grant PDUs carry no privacy flag: `encrypted` comes only from
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
  `control_channel` `LCN <n>` when the rest channel number changes. Its
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

## Sources / limits

ETSI TS 102 361 (public); DSD / dsdcc / sdrtrunk as documentation only;
the AMBE frame interleave tables from DSD (ISC, notice in
`lib/mbe_tables.hpp`). A repeater INPUT with both slots busy can't be split
(MS bursts carry no slot); two radios in direct mode on both timeslots at
once: the receiver follows one burst sequence at a time.
