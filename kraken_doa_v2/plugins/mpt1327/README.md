# MPT1327 decoder plugin

MPT1327 / MPT1343 analogue trunking signalling: 1200 bit/s FFSK (1 = one
cycle of 1200 Hz, 0 = one and a half of 1800 Hz) on NBFM. Control channel
slots of 128 bits: the control channel system codeword (CCSC, carrying the
system identity SYS; its BCH check bits are the SYNC 0xC4D7) and an address
codeword (Aloha, grants, acknowledgements, broadcasts ...). Traffic
channels: preamble + SYNT 0x3B28 + an address codeword (the radios' call
maintenance, the site's clear-down). Codewords: 48 bits + BCH(63,48) +
parity. The speech is analogue NBFM. Version 3.0 (2026-11-20) reports the
system, its channels and grants for trunk following, and on a traffic
channel the call and who talks. Details:
`kraken_doa_v2/docs/dev/digital-decoders.md`.

## What is decoded

Field positions from MPT 1327 (1997 issue) section 5, checked one by one
(2026-11-20; the GTC's were taken from a secondary source before and are
the same): bit n of a codeword = bit n-1 of the 48 information bits.

- CCSC: SYS. A CCSC counts only on the slot grid of another with the same
  SYS (128 bits apart, +-half a bit, up to 20 slots), so a control
  channel's first slot waits 107 ms for its second.
- GTC (bit 22 = 0): PFIX, IDENT1 (called), D (data), CHAN (10 bits), IDENT2
  (calling) -> event "Grant: ..." (with MHz when a plan gives them) and
  `grant()`.
- Aloha (FUNC, CHAN4 = the control channel number's low 4 bits), ACK family
  (ACKE marks the call as an emergency), AHY (POINT 0 marks the called party
  as a unit; E = emergency), AHYX / AHYP / AHYQ (status) / AHYC, MARK
  (CHAN4, SYS), MAINT (CHAN, OPER), CLEAR (CHAN, CONT, REVS), MOVE (CONT,
  (M)), BCAST SYSDEF 0 announce / 1 withdraw / 4 adjacent site / 5 vote now
  (CHAN, SYS, ADJSITE), 2 call maintenance parameters (PER, IVAL, PON, ID),
  3 registration; data headers (events only).
- Before 3.0 MAINT and MOVE read their channel from bits 16-25 and MARK was
  shown as a "Control channel" fact from them: wrong (they are at bits
  31-40 / CHAN4 + SYS).
- Codeword checks: on the control channel one bit error is corrected for
  facts, events, valid frames and site packets; after a SYNT only error-free
  address codewords count (with the correction a random word passes 1 in
  1000: a carrier's key-up produced one in a test), and everything that
  drives trunk following (grants, channels, control channel, calls) takes
  error-free codewords only. A GTC after a SYNT (a replacement channel on a
  traffic channel, a control channel used for traffic) counts only when the
  same codeword comes twice within 2 s (version 3.1, 2026-11-20: one
  error-free codeword after a false SYNT has 18 bits of protection - about
  one false grant per 20 h of noise).
- The discriminator's DC (the carrier offset) is taken off before the tone
  correlators (1.5 cycles of 1800 Hz don't reject it: a 2 kHz error lost
  most slots before) and reported as `freq_error` once a second while
  messages decode.

## Trunk following

`Info::trunking`. On a control channel:

- `trunk_system`: key = the SYS as 4 hex digits ("42E1") - the network part
  can't be known from the air outside MPT 1343 networks, so a multi-site
  network gets one key per site (one channel table each); option System key
  = MPT 1343 network part: "NET-n" (SYS bit 1 = 1: national network, bits
  2-3) or "OPID-n" (bit 1 = 0: regional, bits 2-8), site "SYS hhhh (NDD n,
  LAB n)". `control` = a CCSC within the last 30 s. At once when the
  identity is first heard (after every reset too) - never a grant before it
  - then every 5 s.
- `grant` per GTC: channel "CH <n>" / number n (the 10-bit field), target
  = IDENT1, source = IDENT2 as "PPP-IIII" (prefix-ident; "ALL" for ALLI,
  "PSTN" / "PABX" / "data network" / "PSTN short j" for gateways, "" for
  inter-prefix / dummy / TSC idents whose party is in another message),
  `data` = bit D (not followed), `emergency` = an ACKE or an AHY with E for
  either party within 20 s, `update` = the same GTC within 5 s (MPT1327 has
  no update flag: the TSC repeats GTCs, some systems re-send them during the
  call), `demod = "nfm"`, `bw_hz` 12500, decoder = this plugin. Group or
  unit is NOT in the signalling (units and groups share idents 1-8100):
  `group = false` when the called party was checked with an AHY (POINT 0 -
  only units answer), got an ACKV QUAL 0 ("called unit not in radio
  contact") or called before itself (IDENT2 of a GTC is always a unit or a
  gateway), true for ALLI; any other: the option Calls not known as unit
  calls (`undecided`, default "group"); a gateway is a unit. On air (NZ,
  below) no call was ever shown to be a group call and a multi-site unit
  call reaches the CALLING party's site without an AHY (the called unit is
  checked on its own site): there set "unit". The
  units heard are kept for the life of the VFO's decoder (4096, the one heard
  longest ago gives way; emptied on every reset, as are the emergency and
  repeated-GTC memories). A wrong guess costs the call's label and the
  host's policy (TG vs unit, `TRUNK_UNIT:0` skips unit calls) and splits
  its history (the host keys calls on group + target) - never the channel
  or the parties. Options for the traffic channel: `chan`, `sys`, `target`,
  `source`, `group`, `emergency` (+ `plan` / `network` / `link` when not the
  default).
- `trunk_channel`: BCAST announce control channel, adjacent site, vote now
  - when its SYS has this system's key. `control_channel`: MOVE with (M) = 0
  (every radio) to CONT. No `grant_end` on a control channel: MPT 1327 sends
  CLEAR on the traffic channel only (9.1.2.6); a control channel used for
  traffic (it signals with SYNT then) that hears a CLEAR for its own number
  sends `grant_end`.
- Channel plan option: "none" (default: `freq_hz` NAN - the receiver's
  channel table, "base + n x step") or "MPT 1343 Band III": 12.5 kHz,
  field 513-1015 = channels 58-560 of sub-band 1 (downlink 176.5 MHz + (n -
  1) x 12.5 kHz, uplink 8 MHz higher), field 1-503 = channels 58-560 of
  sub-band 2 (downlink 200.5 MHz + ..., uplink 8 MHz lower) (MPT 1343 4.1 /
  5.1); other fields NAN. Measured on air in NZ (Tests, below): 414.0000
  MHz + n x 12.5 kHz - enter it as the receiver's table.
- Example (the NZ network measured below, four sites): Channel plan "none"
  and the receiver's rule base 414.0000 MHz, step 12.5 kHz, numbers as the
  channel number; System key "MPT 1343 network part" (the sites' SYS codes
  0x42E1 / 0x42C1 / 0x4221 / 0x4201 are all MPT 1343's national form NET 0:
  one key "NET-0", one channel table - right, the plan is the same on every
  site); Calls not known as unit calls "unit".

On a traffic channel (the plugin runs beside the host's FM demodulator; it
can't hear speech, so `valid()` stays with real codewords):

- MAINT (CHAN = this channel when `chan` is set, else any; another
  channel's is breakthrough and ignored) from a unit: Pressel On (000) /
  Periodic (010) -> `call(ACTIVE)`, Pressel Off (001) / Disconnect (011) ->
  `call(ENDED)` (a Disconnect right after the same unit's Pressel Off adds
  nothing). The MAINT's PFIX/IDENT1 is the speaker - or, in group calls on
  systems that say so (BCAST SYSDEF 2, ID = 0), the group's address: equal
  to the grant's `target` in a group call = the speaker isn't named (source
  ""); in a unit call the called unit answering = the grant's pair stays
  (target the called unit, source the caller: the host keys a call on its
  target, so the other party as target would read as another call - who
  talks is the talker); the grant's `source` = that unit; any other unit
  while no clear-down has been seen = another party of the granted call
  (the TSC gives the channel to one call from GTC to CLEAR), after one =
  target unknown. `emergency` (the grant's option) marks the granted call's
  reports.
- CLEAR (REVS = 101010101010, CHAN = this channel) -> `call(RELEASED)`; one
  is enough (the TSC sends at least two, the rest within 2 s change
  nothing). A CCSC on the channel (back to control signalling) and a GTC
  moving the call elsewhere (9.2.3.4: `grant()` too, heard twice) release it
  as well.
- The granted channel's carrier (3.2): on an instance with the grant's
  `target`, a carrier for 0.2 s before any clear-down = `call(ACTIVE)` with
  the grant's parties (group: no source) - again when the carrier comes back
  after an ENDED. On air the site keyed the downlink 0.12-0.58 s BEFORE the
  GTC and held it to the clear-down, and the radios' call maintenance
  reached the downlink in 2 of 24 calls (Pressel On only): without this the
  call was RELEASED without ever being ACTIVE.
- Control channel signalling on a traffic channel's VFO (`chan` set) is
  another channel's unless its Aloha / MARK CHAN4 (the low 4 bits of the
  channel it is sent on, MPT 1327's own breakthrough guard) is this
  channel's: no system, no grants, no release from it (fact "Control
  channel heard"). On air (3.2): the site's control channel came through on
  5 of its traffic channels by intermodulation (CH 167 = 2 x CH 207 - CH
  247), up to 304 slots in 600 s - 3.1 made those VFOs control channels (7
  grants, 45 system reports on CH 167). 1 in 16 channels shares the control
  channel's CHAN4: no guard there.
- No Pressel Off: when the carrier goes (noise squelch on the
  discriminator: rms of its first difference over 10 ms, carrier < 2400 Hz,
  none > 3400 Hz - noise alone measured >= 4100, a carrier with speech at 6
  dB SNR median 1300) for 1 s, an ACTIVE call is ENDED. On a downlink with
  talk-through the site's carrier stays up until the clear-down.
- Talkers: one transmission per speech item, id = the MAINT's "PPP-IIII",
  from its first message to its last confirmed one (Pressel Off), label "TG
  <target>" / "to <other party>" + "CH n" + " · via site" - on the downlink
  (option Traffic channel side, default) the site repeats the radios, so the
  bearing is the site's; "up" = the radios' own.

## ID separation (DoA per site / per radio)
A control channel slot is the SITE's transmission and its CCSC names the
site: every slot with a good CCSC is a talker packet "SYS hhhh" - CCSC +
address codeword (107 ms), 2 ms inside both ends - in the 12.5 kHz channel,
averaged over 10 s, kept 30 min (site rows in the Map's DF panel). Sites
reusing a control channel frequency, heard in turn, keep their own bearings.
Traffic channels: talkers from the call maintenance (above). Not decoded:
the mobiles' return control channel (their requests).

## Tests

Generator `tools/mpt1327_gen.py` (MPT 1327 field layouts written from the
standard): a control channel (CCSC with the CCS that makes the check bits
the SYNC, Aloha idle slots; GTC timeline with AHY / ACKE before, repeats,
data calls; BCAST announce / adjacent / call maintenance; MOVE) and a
traffic channel's downlink (talk-through carrier, MAINT Pressel On /
Periodic / Pressel Off / Disconnect per speech item, made-up speech-like FM
audio - tone glides and filtered noise - between, two CLEARs) or its uplink
(a carrier per item). The old command line gives the same file bit for bit.

- Two sites (catalogue `mpt1327-two-sites`, SYS 42E1 at 100 deg, 42E7 at
  345 deg): 93 valid slots, 47 / 46 packets, 100.0 / 345.0 deg offline (`--channels
  5 --uca 0.12`) - the same as 2.1.
- Trunk scene (catalogue `mpt1327-trunk-two-calls`, 14 s, 2.4 MHz,
  wide_scene.py, SNR 20 dB): control channel CH 100 (202.4500 MHz, +300 kHz,
  300 deg) offline with `--opt plan=mpt1343`: (system) key "2C35" (3
  reports), (channel) CH 110 202.575000 / up 194.575000, 8 grants (5
  updates): CH 60 201.950000 / 193.950000 "042-0100" group from "042-1234"
  (1.17 .. 5.12 s, 4 messages), CH 132 202.850000 / 194.850000 "042-0555"
  unit from "042-2001" flags m (6.18 s, after AHY + ACKE), CH 75 202.137500
  flags d; site packets 300.0 deg. CH 60 (-200 kHz, 40 deg) with the
  grant's options: ACTIVE 1.39 s (042-1234), ENDED 3.59, ACTIVE 4.09
  (042-1300, target 042-0100), ENDED 5.69, RELEASED 6.29 (CLEARs at 6.2 s);
  talkers 042-1234 1.321-3.592 s and 042-1300 4.021-5.692 s, both 40.0 deg.
  CH 132 (+700 kHz, 200 deg, with the grant's options incl. `emergency=1`):
  calls target 042-0555 / source 042-2001 flags m throughout (3.0: source
  042-0555 while the called unit answered, no m), items 6.39-8.39,
  8.79-10.59, 10.89-11.89 (+ Disconnect), RELEASED 12.39; talkers 042-2001
  6.32-8.39 / 10.82-11.89 and 042-0555 8.72-10.59, 200.0 deg.
- `tools/digi_test` (the real engine): the control channel's SYSTEM /
  CHANNEL / GRANT and the traffic channel's CALL reports arrive as above,
  each GRANT with its 6 options.
- Reduced SNR (noise in 12.5 kHz, 4 seeds, the scene's control channel: 8
  GTCs, 131 slots; grants need error-free codewords): 8 / 8 grants at 12
  dB, 7-8 at 9-10 dB (128-131 slots), 5-7 at 8 dB (122-129), 3-7 at 7 dB
  (110-114), 0-2 at 6 dB (73-85), none at 5 dB (28-38 slots). Traffic channel CH 60 (8
  messages): all call lines (4 ACTIVE, 2 ENDED, 1 RELEASED) down to 9 dB,
  5-7 of 8 messages at 8 dB, 1-3 at 6 dB. No grant ever had a wrong
  channel, target or source; at 7-8 dB a lost AHY made the unit call a
  group.
- Carrier error +-1, +-1.5, +-2, +-2.5, +-3 kHz (20 dB): 8 / 8 grants,
  130-131 slots; traffic channel calls unchanged at +-1 and +-2 kHz. Before
  the DC removal -2 kHz gave 0 grants and 97 slots.
- False reports, 1 h each (2026-11-20): complex noise, and an FM carrier
  (2 kHz peak deviation, 20 dB SNR) modulated by recorded audio (speech and
  other sounds, ~13 min of clips at 4 speeds, scratch only): 0 valid frames,
  0 events, 0 grants, 0 calls. Before the CCSC grid check 7 (noise) / 8
  (speech) lone CCSCs passed, each a ghost "control channel" with its
  system reported for 30 s; 2.1 had 10 / 24 valid frames and one false
  grant ("channel 938") on the speech hour.
- 3.1 checks (each failed with 3.0): a traffic channel with ONE GTC to a
  replacement channel after its call - 3.0 grant + RELEASED, 3.1 nothing;
  with the GTC twice - 3.1 one grant + RELEASED. 4100 calls from 4100
  different units, then a call to the last of them: 3.0 "group" (its unit
  table stopped at 4096), 3.1 "unit". `tools/digi_test --retune` in the
  middle of nine GTCs: 3.0 sent the first two GRANTs after the RESET before
  the SYSTEM, 3.1 the SYSTEM first; an emergency unit call learned before
  the RESET is a plain group call after it (unit / emergency / repeat
  memories emptied - 3.0 as well).
- False trunk reports per VFO (2026-11-20), from the measured sync
  candidates and the bits each report needs: SYNT candidates 11 294 / h in
  noise, 17 068 / h in the FM audio hour (an instrumented copy). One
  error-free codeword after one = 2^-16; a GTC (bit 1, bit 22) 2^-18 =
  0.04 - 0.07 / h (380 - 570 a year: 3.0); twice the same 64 bits within 2
  s: < 1e-15 a year; CLEAR (bit 1, type 9, REVS 12) 2^-38 = 4e-4 - 5e-4 a
  year (x 2^-10 more with `chan`); MAINT -> call ACTIVE / ENDED + a talker
  (2^-27): 0.7 - 1.1 a year on a VFO without `chan`, 7e-4 - 1.1e-3 with it (a false
  ACTIVE on noise ends after 1 s: no carrier). Control channel reports need
  two CCSCs on one slot grid with the same SYS (7 lone CCSCs / h in noise,
  the second within 20 slots, +-20 samples, same 15-bit SYS: ~1e-10 / h).
  1 h of an idle control channel at 7 dB (85 % of 33 750 slots decoded,
  5091 lost): 0 grants, 0 wrong channels. 1 h noise / 1 h FM audio (3.1): 0
  valid frames, 0 events, 0 trunk reports.
- Uplink, Pressel Off lost: ENDED 1.0 s after the carrier went (4.02 s for
  a carrier ending at 3.02 s, 20 dB).
- Real signals (NZ, KrakenSDR captures at night, 2026-11-19: 416.5 MHz 600
  s, 418.0 MHz 300 s; the receiver read every carrier ~450 Hz low): control
  channels SYS 0x42E1 on 416.5875 MHz (5615 slots in 600 s = 99.8 %, none
  corrected), 0x42C1 on 416.6750 (5571, 102 corrected), 0x4221 on 416.7500
  (3474), 0x4201 on 417.7625 (193 in 300 s, weak). Every address codeword
  parsed: Aloha, BCAST (call maintenance parameters, registration
  parameters, vote now advice), ACK / ACKX for registrations - no GTC, MOVE
  or CLEAR, no call (a quiet night). Channel plan, measured: the
  sites name each other's control channels in their vote-now broadcasts
  (42E1 -> 42C1 214, 4201 301; 42C1 -> 42E1 207, 4221 220, 4201 301; 4221 ->
  42C1 214, 4201 301; 4201 -> 42C1 214) and give their own channel's low 4
  bits in the Aloha (15, 6, 12, 13 = 207, 214, 220, 301 mod 16): all fit
  414.0000 MHz + n x 12.5 kHz exactly (207 = 416.5875, 214 = 416.6750, 220
  = 416.7500, 301 = 417.7625). The SYS codes of one network differ per site
  (0x42E1, 0x42C1, 0x4221, 0x4201 ...): with the default key each site has
  its own channel table.
- Real calls, VERIFIED ON AIR (the same network, 600 s from 06:30 local,
  2026-11-20, one antenna, 416.5 MHz centre):
  - Control channels: 42E1 5624 slots, 58 GTCs (29 calls, each GTC twice
    in a row), 42C1 5138 slots, 16 GTCs (8 calls), 4221 weak (665 slots, no
    GTC). Types: Aloha, BCAST 2 / 3 / 5, ACK / ACKI / ACKV / ACKX, AHY,
    AHYX, GTC - none unparsed, no data codeword, no MOVE / CLEAR on a
    control channel, no GTC off the slot grid (the grid check passes every
    slot of a real control channel). GTC field positions and the channel
    plan confirmed: each granted channel (15 ... 309 = 414.1875 ... 417.8625
    MHz) keyed up 0.12-0.58 s (median 0.22 s) BEFORE the GTC's slot ended -
    23 of the 24 in-span calls, the 24th was on already - and its CLEARs
    name CONT 207 / 214 = the site's own control channel.
  - Traffic channels (24 in-span calls on CH 114, 115, 154, 167, 187, 227,
    247, 254, 267; 1339 s of calls): the downlink carrier is up from before
    the GTC to the clear-down without a break (the site's talk-through; one
    weak 42C1 channel faded once for ~2 s); the radios' call maintenance
    almost never reaches the downlink - 2 Pressel On (from the grant's
    calling unit, 5.3 and 14.7 s into the call), no Periodic, Pressel Off
    or Disconnect, no other base-station signalling during the calls. The
    end: 23 clear-downs (the 24th call ran past the capture), each 1-4
    CLEARs - typically 3 back to back (0.10-0.11 s apart) and a 4th after
    0.4-0.75 s; calls lasted 5.6-107 s, four 180-181 s (the system's
    call time limit). With the grant's options (as the host runs it), 3.2:
    ACTIVE in 24 / 24 calls (from the carrier; 3.1: 2 / 24, from the 2
    Pressel On), RELEASED at the first CLEAR in 23 / 23, one ENDED + ACTIVE
    again at the faded carrier; talkers only for the 2 Pressel On (0.07 s
    each). CLEARs of OTHER channels (breakthrough, 9 on 2 VFOs) ignored by
    the CHAN check; the control channel's intermodulation on 5 traffic
    channels (above) ignored by the CHAN4 check in 3.2.
  - Real speech: 4456 SYNT candidates in the 1339 s of calls (11 977 / h,
    as in noise), 83 error-free address codewords - every one a CLEAR or
    MAINT that was really sent (incl. the breakthrough CLEARs) - 0 others,
    0 false grants / calls.
  - Group or unit (37 first GTCs on both sites): 23 preceded by an AHY
    (POINT 0) to the called unit 0.4-0.75 s before, the other 14 by an ACKI
    (QUAL 1) to the caller only; reported unit 29, group 8 - of the 8, 3 were
    later callers themselves (units) and 5 undecided; no called ident had
    more than 2 callers, pairs called each other back and forth: a network
    of unit calls, no call shown to be a group call. 5 calls were granted on
    BOTH sites within 0.15 s (caller on one, called unit on the other: the
    AHY only on the called unit's site) - one network, the same idents on
    42E1 and 42C1. With `undecided=unit`: 29 / 29 unit on 42E1.
  - No emergency call (no ACKE, every AHY with E = 0).
- Decode time 0.6 % of real time at 48 kHz (2 % with the 1 h files on a
  busy machine).

Public recordings (2026-10-16): sigidwiki's "MPT1327-like bits" audio and
"Taxi trunk" IQ (504.625 MHz, 250 kHz, five carriers) are NOT MPT1327 - no
1200 / 1800 Hz FFSK on any carrier: the plugin reports nothing on them
(correct).

## Sources
MPT 1327 (January 1988, revised June 1997) and MPT 1343 (1997 issue),
Radiocommunications Agency - field layouts, ident table, procedures,
SYS structure, Band III channel plan. The pre-3.0 GTC positions came from
sdrtrunk's documentation; they agree with MPT 1327 5.4.
