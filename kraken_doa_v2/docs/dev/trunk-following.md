# Trunk following

Written 2026-11-20 with the trunk manager (`src/trunk_follow.cpp`,
`include/trunk_follow.hpp`; the plugin API it consumes: `plugins/SDK.md`
*Trunked systems*, `include/digital/trunk_event.hpp`). *Italic* names are
other topic files in this folder.

## What it does

A trunked radio system (P25, DMR Tier III, NXDN, TETRA, MPT1327) hands out
its traffic channels call by call on a control channel. A decoder plugin on
a VFO tuned to the control channel reports each grant (`kp::Host::grant`);
the trunk manager puts a temporary **voice VFO** with the grant's decoder on
the granted channel - inside the receiver's span, never retuning the centre
or a tuner - for the length of the call, so the call is heard, decoded,
transcribed and direction-found (the voice VFO's own MUSIC / talker DoA).
The decoder on the traffic channel reports what it carries
(`kp::Host::call`), which drives the call's state.

- Off by default (`TRUNK_FOLLOW:0`): nothing changes for a user who doesn't
  turn it on. Off, the manager still tracks systems, grants and calls for
  display (calls wait with why `off`).
- The user's own VFOs are never touched, except a control VFO moved by a
  `CONTROL` event (below). Voice VFOs are never saved (`DECIMATORS:`).

## Terms

- **Control VFO**: a user's VFO whose decoder reports `SYSTEM` with
  `control = true`, or grants without any `SYSTEM` yet. One per site; several
  per system are normal (a real P25 network here: six control channels of
  two systems, three sites each, in one 2.4 MHz span).
- **System**: `<plugin id>:<key>` (`p25:BEE00.14C`). A VFO whose system key
  isn't known yet gets a temporary system `<plugin id>:@<vfo id>` (shown,
  never saved); when its key arrives the temporary one is folded into the
  keyed one (channels, talkgroups, units, counters, calls) and disappears; a
  temporary system with no control VFO left is dropped with its calls.
  Calls that waited in the temporary system (`no_freq` before the key was
  known) move with it and are resolved again against the keyed system's
  table and rules at once (seen in the NXDN scene: grants heard before the
  system identity became followable by the rule at the merge).
  Channel table, talkgroup names / modes, "listed only": per system, shared
  by its sites. Counters and "receives": per system and per control VFO.
- **Site**: the control VFO a grant came from.
- **Call**: system + site + group / unit + target. A wide-area talkgroup
  granted on two sites at once is two calls (two carriers of one
  conversation); two systems using the same talkgroup number are two calls.
  The call keeps its id (its row) over a conversation: each over
  (transmission trunking) revives the same call.
- **Voice VFO**: made by the manager (`DecimatorManager::addDecimator`, a
  `decimator_info` broadcast - not `ADD_DECIMATOR`, whose `decimator_added`
  resets every page's audio), set up through `ControlHandler::apply_command`
  on the uWS loop like a browser's commands. **Parked** between calls.

## Design and reasons

### Events
`trunk::on_plugin_event` (decoder worker threads) only queues: a bounded
deque (`QUEUE_MAX` 4096) and one `loop->defer` drain posted when the queue
goes non-empty - a grant reaches the loop within a few ms, not at the next
tick. When the queue is full every kind but RESET is dropped (SYSTEM,
CHANNEL, CONTROL, GRANT, GRANT_END, CALL; counted in the status'
`events_dropped`); a RESET is never dropped - one more is queued past the
bound unless one for the same VFO and decoder is pending already
(`resets_coalesced`): a lost RESET would keep stale calls or a stale control
VFO. Each event carries its decoder's **serial**
(`DecimatorInstance::digital_serial`, given when `ensureDigital` makes the
decoder, never repeated - an address can be reused by the next decoder made
for the same VFO id): events of a decoder that is gone (VFO removed, id
reused) are dropped. (`KRAKEN_TRUNK_QUEUE_MAX` shrinks the bound for tests.) A timer on the loop (`trunk::tick`, `TICK_MS` 50) drains
too and runs the state machine. Everything the manager owns lives on the
loop; `is_trunk_vfo()` / `vfo_info_json()` (the snapshot builder,
`decimator_info`, the saved-frequency scan) read a small mutex-guarded map.

### Resolving a grant's frequency
In this order: the grant's `freq_hz`; a **user** entry for the channel
name; a user **rule** for the channel number (`first..last: base_hz + (n -
first) * step_hz`, uplink = downlink + `up_off_hz`); an **announced** entry
(`CHANNEL` events, or the frequency an earlier grant carried); a
**learned** entry. The call's `fsrc` says which (`grant` | `user` | `rule` |
`announced` | `learned`). Announced / learned values never overwrite a user
entry (separate fields per channel). Unresolved grants make the call wait
with why `no_freq` and are listed per channel name (`unresolved`: which
channel numbers lack a frequency, how often granted); a channel leaves that
list as soon as anything gives it a frequency (a grant's own, an
announcement, a rule, learning, a user entry). Every granted channel
gets a table entry (its grant count). Uplinks are kept and shown; this
version follows the downlink only.

### Passive learning
An unresolved call (why `no_freq`) is matched against `CALL ACTIVE`
reports from ANOTHER VFO (one the user placed by hand) running the call's
decoder: same target and group, the same source when both give one, within
`LEARN_WINDOW_MS` 3 s of the grant, a VFO that didn't name another system.
That VFO's frequency is ONE observation; a second one from another grant of
the channel >= `LEARN_SEPARATE_MS` 5 s later on the same frequency (within
`LEARN_TOL_HZ` 1.5 kHz) makes it a **learned** entry (log line, the call is
followed at once). Refused (counted, `learn.refused`): another source, two
calls matching, two VFOs reporting the call at once on different
frequencies, a channel with a user entry, a frequency another channel of
the system already has. A learned entry whose calls receive nothing twice
(SETUP gives up, why `no_signal`) is dropped (`learn.dropped`). Conservative
on purpose: a wrong entry sends every later call to the wrong place.

### Following - the decision
A call is followed when all hold (else it waits with the first reason that
applies, `why`):

| why | when |
|---|---|
| `off` | following is off |
| `paused` | a scanner runs (below) |
| `stopped` | the user stopped it (`TRUNK_END`) or removed its voice VFO by hand: not rejoined (below) |
| `no_signal` | its SETUP gave up (nothing received): not followed again this over |
| `data` | a data grant - counted only, no call row (below) |
| `skip` | its talkgroup's mode is skip - counted only, no call row |
| `unlisted` | "listed talkgroups only" is on and the talkgroup has no user entry |
| `encrypted` | the grant / call is encrypted and `TRUNK_ENCRYPTED:0` |
| `unit` | unit-to-unit and `TRUNK_UNIT:0` |
| `hold` | a hold is on another talkgroup |
| `duplicate` | the same talkgroup / unit of the system is followed through another site already (`TRUNK_ALL_SITES:0`, the default) |
| `no_decoder` | the decoder is no built plugin, or one that holds its VFO (`fixed_freq_hz`, `lock_vfo`) |
| `no_freq` | the channel's frequency is not resolved |
| `out_of_span` | it can't be placed (below) |
| `no_vfo` | `TRUNK_MAX_VFOS` voice VFOs are busy with calls of equal or higher rank |
| `vfo_limit` | the receiver's 16 VFOs are all there |
| `settings` | (a moment) the VFO snapshot is being applied: voice VFOs were removed |

A waiting call is retried every tick (a VFO may be free, an entry added).
**Priority**: a priority talkgroup's call takes the voice VFO of the oldest
non-priority call (that one is over, `preempted`); equal rank never
pre-empts. **Duplicates**: the first site's copy of a conversation is
followed, the others wait (`duplicate`, counted per control VFO). A waiting
duplicate takes over only when the followed copy receives nothing (SETUP
gives up) or loses its control channel / VFO (`control_reset`,
`control_removed`, `retuned`, `mode`); when the followed copy ends normally
(`released`, `ended`, `reassigned`), its waiting duplicates end with it
(end `duplicate`, not `no_signal`) - they would get a VFO only after the
transmission had ended everywhere (seen on air: 4 of 4 `no_signal`s of a
P25 run were such late take-overs). A grant from site B never moves the
voice VFO that follows the call on site A (the site is in the call key).

### Placement (rf -> tuner, offset), never a retune
"Fits" = the whole channel (`max(bw_hz, the decoder's min_vfo_rate)`) plus
`PLACE_GUARD_HZ` 2 kHz each side inside the **usable band** = edge clip x
2.4 MHz around the tuner's centre (1.92 MHz at the default 0.8). The edge
clip is the margin: outside it the R820T's IF filter and the decimation roll
off (the same band the spectrum shows and the saved-frequency scan plans
with); the 2 kHz cover the VFO's AFC and the RTL-SDR's ppm error.
- coherent: inside the centre's (`coherent_tuner_hz()`) usable band.
- wideband scan: on the tuner nearest the frequency (as `SET_DECIMATOR_FREQ`
  picks it), inside its pane (half the tuner spacing each side, at most the
  usable half): a channel on a seam is `out_of_span`. The offset is from the
  spread's middle. (Measured: the receiver's float maths places a wideband
  VFO to about 30 Hz near 851 MHz - harmless for a real decoder's AFC.)
- independent: the control VFO's tuner first, then the others in order:
  the first whose usable band holds it (`SET_DECIMATOR_FREQ:id:kHz:tuner`).

An out-of-span call is counted and the status offers a **suggested
centre** per system (below) for the page's "centre on the system".

### The voice VFO
Set up, in this order, so the traffic decoder has the grant's options
before (or with) its first samples on the channel: the options directly
through `DecimatorManager::setDigitalOptions` (one call, no broadcast:
the grant's `options` as `<decoder>.<key>` - only keys the traffic plugin
declares in its `Info::options` (hidden ones included; the engine checks
only their form): others are dropped, counted (`options_dropped`) and
logged once a minute per plugin; options of the previous call that
this grant doesn't set go back to their default because the whole map is
replaced; Logging / Plot on map / Positions every / Show all messages copied
from the control VFO), then through `apply_command` (`trunk::dispatching()`
set: not saved, no VFO snapshot): `DIGITAL_MODE:v:PLUGIN:<decoder>` (only
when it isn't that plugin already - no process restart), `SET_DECIMATOR_DEMOD`
DIGITAL (or NBFM for `demod = "nfm"`), `SET_DECIMATOR_BW_HZ` = the narrowest
efficient bandwidth >= max(`bw_hz`, `min_vfo_rate`), `DEC_SQUELCH_METHOD`
DIGITAL (analogue: FFT with `DEC_SQUELCH_LEVEL` `ANALOG_SQ_DB` 10 dB),
`DEC_SQUELCH_ENABLE:1`, `VOICE2TEXT_VFO` as the control VFO has it, last
`SET_DECIMATOR_FREQ`. Heard per `TRUNK_AUDIO` while it carries a call
(muted when parked). Its own retune makes the decoder RESET: expected for
`RESET_EXPECT_MS` 2 s, not a loss.
- **Parked** after its call: muted, `enabled = false` (the pipeline doesn't
  decimate it: its decoder gets no samples, its plugin process idles -
  measured 0.0 % of a core), kept in the VFO list (no flicker), reused for
  the next grant - the one its call had last first (the same VFO for the
  conversation's next over), then one already running the call's decoder.
  Removed after `PARK_KEEP_MS` 30 s unused, when following stops, when
  `TRUNK_MAX_VFOS` is lowered, while a scanner runs.
- Never in the saved snapshot: `build_decimator_snapshot` leaves voice VFOs
  out and counts the heard list's positions without them; the user's own
  VFOs keep being saved while following runs. Before a snapshot is applied
  (the replay after a playback, `RESET_SETTINGS`) every voice VFO is removed
  (`trunk::release_all_vfos`): the snapshot works by list position.
- Removed by hand: simply gone - its call isn't rejoined (why `stopped`).
  Retuned / reconfigured by hand: taken back at the next grant.
- Placed at most every `PLACE_MIN_MS` 100 ms (a grant flood moving a call).

### States of a call
`wait` (not followed: `why`) -> `setup` (placed, nothing received) ->
`active` (valid frames of its decoder newer than 100 ms after the placement -
`frames_within` -, a `CALL ACTIVE` of its target, or for analogue the level
squelch open) -> `hang` (activity stopped, or `CALL ENDED`) -> `recent` (over:
listed for `RECENT_MS` 30 s with `end`, the next over's grant revives the
same call). A `hang` call goes back to `active` with new activity.
- SETUP gives up after `SETUP_GIVEUP_MS` 3 s without anything received -
  up to `SETUP_MAX_MS` 6 s while grant updates still arrive (DMR late entry:
  one real call keyed up 3.5 s after its grant) -> why `no_signal`, counted
  per system (`no_signal`: a wrong channel table shows up here).
- HANG ends after the hang time (`TRUNK_HANG`, default 2 s), unless a grant
  update came within `GRANT_FRESH_MS` 2 s AND there was activity within
  `MAX_SILENT_MS` 10 s: grant updates keep a call through pauses, at most 10
  s of silence.
- A not-followed call is over `WAIT_EXPIRE_MS` 3 s after its last grant.
- **Ends** (`end` in `calls` (recent) / `history`): `released` (`GRANT_END`
  for its channel + slot, or `CALL RELEASED`), `ended` (hang time
  elapsed), `reassigned` (its channel + slot granted to another target on
  that site: its VFO goes to the new call without a retune), `preempted`,
  `relabelled` / `wrong_call` (below), `duplicate`, `retuned` (its tuner's
  centre moved > 1 kHz), `mode` (operating mode changed), `control_reset`
  (its control VFO's decoder reset: retuned, decoder changed, plugin
  restarted), `control_removed`, `forgotten` (`TRUNK_SYS_DEL`), `no_system`,
  `playback` / `playback_end`; a not-followed call's end is its `why` (or
  `expired`).
- A RECENT call is revived by any FIRST grant (`update` false), and by an
  update unless it was followed (or a duplicate of a followed one), ended
  less than `REVIVE_UPDATE_MS` 2.5 s ago, and the update names the same
  channel + slot: the control channel's trailing repeats after a release
  don't restart it (NXDN: one more VCALL_ASSGN_DUP 0.26 s after the DISC;
  DMR late entry, P25 updates likewise). An update for a call the manager
  never followed (following switched on mid-call) always starts it.
- A grant of the same target on another channel (or slot): the call MOVES
  (its VFO retunes, `moves`). A `CALL ACTIVE` from the voice VFO naming
  another target (not before 300 ms after the placement and its decoder's
  RESET for it - or 2 s: reports of the previous channel may still be in
  the pipe): re-labelled to that target if it may be followed (an existing waiting call of that target
  takes the VFO; the old one ends `relabelled`), else the VFO is freed
  (`wrong_call`). Only ACTIVE counts: an ENDED / RELEASED of another target
  is the previous call's, late. An EMPTY target means "not known yet" (DMR
  voice before any LC, MPT1327 MAINT): the grant's stays. A CALL naming
  another slot (`call_slot` >= 0 and not the call's slot) is not this
  call's: two calls on one carrier (P25 Phase 2 "2-56" / "2-57", DMR "LCN
  81" slot 0 / 1, TETRA timeslots of the control carrier itself) each get a
  voice VFO on the same frequency with their own slot option, and the
  release of one slot never ends the other (a `GRANT_END` with slot -1 ends
  the whole carrier's calls).

### What interrupts following
- **Scanners**: while the discrete, continuous or saved-frequency scan runs
  the manager is **paused** (`paused`, `paused_why`): its voice VFOs are
  removed, its calls wait (`paused`), the setting stays; it resumes when the
  scan stops. (The saved-frequency scan's free tuners don't count voice
  VFOs.) A scanner's retune also resets the control VFOs' decoders: their
  calls end `control_reset`.
  The scanners move the user's VFOs, control VFOs included (their decoders
  reset). Where every control VFO is (RF, bandwidth, decoder) is noted each
  tick while not paused; 1 s after the scan stops, each one the scan moved is
  put back with normal commands (saved: the user's VFO) when its RF fits the
  span now - retried every 0.5 s for 30 s (e.g. until the receiver is tuned
  back by hand). Moved by the user meanwhile: left as it is. Each outcome is
  in `scan_restore` (`restored` + `note`: "put back on ... MHz after the
  saved-frequency scan", "... outside the receiver's span: tune back ...",
  "... then set by hand - left as it is") and a log line. The scanners'
  own behaviour is unchanged.
- **Centre / tuner retuned**: the calls on that tuner end (`retuned`, or
  `control_reset` when the control VFO's RESET comes first - in coherent mode
  the control VFO moves with the centre).
- **Operating mode changed**: every followed call ends (`mode`).
- **Control VFO** removed (`control_removed`) / retuned / decoder changed /
  plugin restarted (`RESET` not caused by the manager: `control_reset`): its
  calls end; its system loses that control VFO until it reports again.
- **Control channel lost**: a control VFO with no report for
  `CONTROL_LOST_MS` 5 s is shown not receiving (`rx` false). Calls in
  progress go on with their own signalling (CALL ENDED / RELEASED) and timers:
  without grant updates HANG ends after the hang time.
- **CONTROL** (the control channel is now elsewhere, e.g. a Capacity Plus
  rest channel): the control VFO is moved there with a normal
  `SET_DECIMATOR_FREQ` (saved: it is the user's VFO, where the control
  channel is now) when the frequency resolves and fits its tuner, at most
  every `CONTROL_MOVE_MIN_MS` 10 s, with a log line; otherwise the control
  entry's `note` says why (no frequency / outside the span / waiting / in a
  playback).

### Playback
Debug playback and recording playback work as live (that is how scenes are
tested end to end): nothing reaches heimdall (`DataReceiver` drops it), the
settings store is sandboxed, and the manager keeps a copy of its systems at
the playback's start and puts it back at its end - what the playback added
(systems, entries, edits, learned channels, counters) is gone; the systems
file is never written during a playback. At both transitions every call and
voice VFO goes and the events still in the pipe (1.5 s) are dropped.

### Data / skip, rows and history
Data grants and talkgroups in mode skip are **counters only**: per system
(`counts.not.data` / `.skip`), per control VFO (`counts.data` / `.skip`), per
channel (`chans[].data` in the lists message); once per grant sequence
(repeats within 3 s are the same one). They make no call rows and no history
(on a DMR Tier III network 30 of 33 grants in 100 s were data). `history`
keeps the followed calls plus the not-followed ones the user can act on:
`no_freq`, `out_of_span`, `no_vfo`, `vfo_limit`, `encrypted`, `unit`,
`unlisted`, `no_decoder`, `no_signal` - not `off`, `paused`, `hold`, `stopped`,
`duplicate` (the followed copy is there).

**Stopped by the user** (`TRUNK_END:<id>`, or its voice VFO removed by
hand): "this call" is the call row - system + site + target - for as long
as it is listed: while its grants go on, and at every further over of the
conversation that comes while it is RECENT (30 s after its last over ended;
each new over restarts that 30 s). Its overs wait with why `stopped`. It is
followed again once the row has expired (30 s with no grant), and only that
call is affected: the same talkgroup through another site, a hold, another
`TRUNK_END` change nothing else.

### Decoder log
Each history entry also goes to the decoder data log (*decoder-log*) as one
record of type `call` in the control VFO's decoder folder, when logging is
allowed and that control VFO's Logging is Log / Log All; never during a
playback: `{"t","vfo" (control VFO),"mhz" (its frequency),"dec","type":"call",
"sys","target","target_name"?,"group","source","sources":[..],"ch","call_mhz",
"slot","enc","emerg","followed","voice_vfo","dur_s","end","why"}`.

### The suggested centre
Per system, recomputed every 2 s: the centre (whole kHz) that keeps every
control VFO in span and covers the most known channel frequencies (table
entries and channels resolved by a rule), each weighted by its grant count
(at least 1). If the control VFOs don't fit one span together (their spread
> usable band - the margins above), the best partial centre: the most control
VFOs first, then the most weight - `controls` < `controls_of` says so.
Ties: the one nearest the current centre. `null` = nothing known.

## Commands (plain text, through `apply_command`; the AI assistant and the settings replay use the same path)

All start with `TRUNK_` (`is_query_command`: not echoed; every one is
answered with the status to every page; a refusal also with
`{"trunk_error":{"cmd":<first 64 bytes>,"error":<why>}}` and
`CommandRejected`). Settings are saved by the handler (schema
`trunk_*`, replayed after `OPERATING_MODE`, sandboxed in a playback):

| command | value | default |
|---|---|---|
| `TRUNK_FOLLOW:0\|1` | following on / off | 0 |
| `TRUNK_MAX_VFOS:<n>` | voice VFOs at most, clamped 1-8 | 2 |
| `TRUNK_ENCRYPTED:0\|1` | follow encrypted calls (the decoders mute them; the IDs and DoA are still wanted) | 1 |
| `TRUNK_UNIT:0\|1` | follow unit-to-unit calls | 1 |
| `TRUNK_AUDIO:0\|1` | hear followed calls | 1 |
| `TRUNK_HANG:<s>` | hang time, clamped 0.5-10, 0.1 s steps | 2.0 |
| `TRUNK_ALL_SITES:0\|1` | follow a talkgroup through every site that grants it (1) / once (0) | 0 |

Not saved:
- `TRUNK_END:<call id>` - stop following that call now; not rejoined.
- `TRUNK_HOLD:{"sys":<id>,"tg":<talkgroup>}` - only that talkgroup is
  followed (others: `hold`) until `TRUNK_HOLD:` (empty) releases it.
- `GET_TRUNK` - the status; `GET_TRUNK_SYS:<system id>` - its lists message.

Systems (JSON in the saved-frequency store's style; strings are cleaned:
control characters dropped, cut at a character boundary; saved to the
systems file at once). `sys` is an existing id - or, for the four `_SET`
commands, a well-formed `<plugin id>:<key>` (`[a-z0-9_-]{1,32}` :
`[A-Za-z0-9._-]{1,48}`; the plugin needn't be installed) that doesn't exist
yet: the system's record is made (`heard` false until its control channel is
heard; a playback's: in memory only). A catalogue entry's `commands` can
thus enter a plan before the decoder has heard anything.

| command | JSON | rules |
|---|---|---|
| `TRUNK_SYS_SET:` | `{"sys","name"?,"listed_only"?}` | name <= 64 bytes (`""` clears); listed_only bool |
| `TRUNK_SYS_DEL:` | `{"sys"}` | forget the whole system (its calls end `forgotten`; a control VFO still on it starts it over, empty) |
| `TRUNK_TG_SET:` | `{"sys","tg","name"?,"mode"?}` | tg 1-32 bytes, no control characters (as the system writes it, "1201", "32-1201"); name <= 32; mode `normal` \| `priority` \| `skip`; makes the talkgroup "listed" |
| `TRUNK_TG_DEL:` | `{"sys","tg"}` | |
| `TRUNK_CH_SET:` | `{"sys","ch","hz","up_hz"?,"num"?}` | the user entry: ch 1-32 bytes (the system's channel name, "1-0412", "LCN 54", "CH 32"); hz (0, 6e9]; up_hz the same or null; num integer >= 0 or null |
| `TRUNK_CH_DEL:` | `{"sys","ch","src"?}` | src `user` (default) \| `announced` \| `learned` \| `all` (the entry) |
| `TRUNK_RULE_SET:` | `{"sys","first","last","base_hz","step_hz","up_off_hz"?,"i"?}` | integers 0 <= first <= last <= 1e9; base_hz (0, 6e9]; step_hz != 0, abs <= 10 MHz; the last channel inside (0, 6 GHz]; up_off_hz abs <= 1e9 or null; `i` = replace rule i, absent = add (32 rules at most) |
| `TRUNK_RULE_DEL:` | `{"sys","i"}` | |

Examples: `TRUNK_RULE_SET:{"sys":"dmr:Huge.1","first":1,"last":4094,"base_hz":455318750,"step_hz":12500}` (the DMR
network here: 455.30625 MHz + LCN x 12.5 kHz),
`TRUNK_RULE_SET:{"sys":"nxdn:global.123","first":1,"last":1023,"base_hz":451012500,"step_hz":12500}`
(the NXDN scene's CH n = 451.0 MHz + n x 12.5 kHz).

## Status `{"trunk":{...}}`

Broadcast on change (at most every 250 ms) and at least every second while
anything is active (a call, a voice VFO, a control VFO), and on every
`TRUNK_*` / `GET_TRUNK`. Pages ignore unknown keys; fields are added, never
renamed.

- `follow`, `all_sites`, `max_vfos`, `encrypted`, `unit`, `audio`, `hang_s`:
  the settings.
- `paused` (bool), `paused_why` (`"saved-frequency scan"` | `"discrete
  scanner"` | `"continuous scanner"` | `""`), `playback` (bool), `mode`
  (`coherent` | `wideband` | `independent`), `hold`: `{"sys","tg"}` | null.
- `scan_restore`: the control VFOs a scanner moved, put back (or not) when
  it stopped, the last 60 s: `[{vfo, sys, hz (where it was), restored,
  note}]` (below).
- `vfos`: the voice VFOs, `[{vfo, call (0 = parked), state (parked | setup |
  active | hang), hz (the frequency placed on, null), plugin, tuner, heard,
  enabled, + while it has a call: sys, ctl, label ("TG 1201 Fire" /
  "Unit 700001"), target, group, ch, slot}]`. **A page takes a voice VFO's
  state, heard and parked from here**: `decimator_info` is broadcast only
  when a voice VFO is made, removed or (re)placed - not at each over (184
  `decimator_info` in 80 s of a real P25 run before; now one per placement).
- `systems`: `[{id, plugin, key, name (reported), user_name, site
  (reported, the latest), saved (keyed: in the file), heard (false = the
  user's record only), listed_only, control: [{vfo, hz, rx (a report in the
  last 5 s), site, note (a CONTROL that couldn't be done, "" = none),
  last_ms (since its last report), counts: {grant_msgs, grants, followed,
  duplicate, data, skip}}], counts: {grant_msgs (every grant message),
  grants (calls / overs granted), followed, no_signal, moves, preempted,
  not: {<why>: n (calls not followed, by their first reason; data / skip:
  grant sequences)}}, unresolved: [{ch, num, n (grants), last (epoch ms)}]
  (the 50 most granted), n_unresolved, centre: {hz, weight, total,
  now_weight (covered at the current centre), channels, of, controls,
  controls_of} | null, n_tgs, n_chans, n_rules, n_units}]`.
- `calls`: the calls in progress and the recent ones, `[{id, sys, target,
  target_name, group, source (the latest), sources (<= 8), ch, num, hz,
  up_hz (null = unknown), slot (-1 = whole carrier), fsrc, enc, emerg, data,
  prio, decoder, demod, state (wait | setup | active | hang | recent), vfo
  (-1 = none), ctl_vfo (its site), site (the site's name), why (wait: the
  reason, "" = followed), moves, t0 (epoch ms of this over's first grant),
  overs (overs before this one), end (recent: how it ended), age_ms,
  state_ms, grant_ms (since the last grant)}]`. A page shows a conversation
  as one row by `id`; `recent` rows can be shown dimmed.
- `history`: the last 50 finished overs worth showing (rule above), oldest
  first: the call fields without the ages, plus `end`, `dur_ms`, `t_end`
  (epoch ms), `followed`.
- `learn`: `{learned, refused, dropped}`; `lat`: `{place_ms, place_max_ms,
  active_ms, active_max_ms, n}` (median / max over the last 50: a grant's
  arrival on the loop -> its VFO placed / -> ACTIVE); `events`,
  `events_dropped` (queue full: every kind but RESET), `resets_coalesced`,
  `options_dropped` (grant options the traffic plugin doesn't declare),
  `calls_dropped` (grants refused a row: 64 calls already),
  `now` (epoch ms).

## Lists `{"trunk_sys":{...}}`

Per system, on `GET_TRUNK_SYS:<id>`, within 1 s after what is in them
changed (an entry added / edited / removed) and every 10 s while only
counters / last-heard times change: `{id, user_name, name, listed_only,
tgs: [{id, name, mode, listed, last (epoch ms, 0 = never), calls}], chans:
[{ch, num, hz, up_hz, src (the one resolving it now; "" = unresolved),
user_hz, user_up, ann_hz, ann_up, learned_hz, grants, data, last}], rules:
[{i, first, last, base_hz, step_hz, up_off_hz}], units: [{id, last,
calls}]}`. Every number that isn't known is null.

## The systems file `trunk_systems.json`

cwd-relative (Docker: in the app's data folder like the other runtime files;
it is written there, not served, so no `COPY` / `link_files` entry);
written through `fs_replace` (temp file, renamed over), never during a
playback; a user's edit at once, new announced / learned / heard entries
within 30 s, counters and last-heard times every 10 min, the last unsaved
change at shutdown (`trunk::save_now`). Not touched by `RESET_SETTINGS` (as
the saved-frequency store). An unreadable file is kept as `.bad` and the
manager starts empty. Only keyed systems; units heard are not saved.

```json
{"version":1,"systems":[
 {"id":"p25:BEE00.14C","plugin":"p25","key":"BEE00.14C","name":"County","reported_name":"P25 BEE00.14C",
  "listed_only":false,"heard":true,
  "tgs":[{"id":"1201","name":"Fire","mode":"priority","listed":true,"last":1795090923221,"calls":12}],
  "chans":[{"ch":"1-0412","num":412,"user_hz":null,"user_up":null,"ann_hz":851162500,"ann_up":806162500,
            "learned_hz":null,"grants":9,"last":1795090923221}],
  "rules":[{"first":1,"last":400,"base_hz":851000000,"step_hz":12500.000,"up_off_hz":-45000000}]}
]}
```

## Limits

Queue 4096 events; 32 systems (a temporary one goes first); per system
2000 talkgroups (the least recently heard unlisted one goes), 2000 channel
entries (the least recently granted without a user frequency goes), 32
rules, 1000 units (memory only), 200 unresolved names; 64 calls in progress
+ recent (the oldest recent one makes room, else the grant gets no row:
`calls_dropped`); 50 history entries; 8 sources per call; voice VFOs 1-8 and
never past the receiver's 16 VFOs; the systems file 8 MB.

## Protocols

What each plugin reports (the plugin workers' notes, 2026-11-20; their
README has the rest).

**P25** (plugin p25, 3.1). Grants: every grant form on a control channel -
updates (`update` set): TSBK 0x02, 0x03 (explicit: the downlink is channel
T), 0x06, 0x09, MFID 0x90 0x03, P2 0x05 / 0x25 / 0xC3 / 0xC6; first grants:
TSBK 0x00, 0x04, 0x08, 0x14 (data), MFID 0x90 0x02, P2 0x40 / 0x44 / 0xC0 /
0xC4; link-control 0x02 grant updates heard on traffic channels too. MBT
explicit grants are not decoded. No control-channel clear-down: no
`grant_end`. Channel name "identifier-number" ("1-412"), `number` the
12-bit channel number (the same number under different identifiers: a user
plan needs the identifier too). Frequency from the identifier broadcasts
(IDEN_UP / _VU / _TDMA); the user's table is needed only before they are
heard. TDMA: carrier = n / slots, slot = n % slots, the grant passes `lch` =
slot + 1; two calls on one TDMA carrier come as two names ("2-56" / "2-57"),
each wants its own VFO with its own `lch`. Uplink = downlink + the
identifier's signed transmit offset. Options on every grant: `nac` (the
system's, verified identical on its traffic channels), `wacn` / `sysid`
(Phase 2 descrambling from the first burst), `lch`, `phase` (1 / 2); `bw_hz`
12500. Call end: Phase 1 TDU / TDULC or 0.75 s without frames -> ENDED, a
TDULC LCO 0x0F -> RELEASED (on the recorded systems 40-50 ms after almost
every transmission: no hang time, each transmission granted anew; on one
channel most ended without it - the host's hang timer is needed); Phase 2
MAC_END_PTT / HANGTIME / MAC_RELEASE (one unit's release or a preemption,
not the channel's) -> ENDED, MAC_IDLE -> RELEASED per LCH;
another talkgroup on the slot -> RELEASED for the old call, then ACTIVE.
Timing on air: grant a median 0.22 s before the first voice frame (10-90 %
0.14-0.28 s UHF, 0.02-0.27 s VHF); in 85 of 319 matched grants voice started
up to 77 ms before the grant's TSDU was complete (the host usually misses
the HDU); ACTIVE 0.07-0.08 s after the first frame. Verified on air
(2026-11-20, two systems of one WACN, 13 control / 14 traffic channels): 329
grants with the reference channel / frequency / talkgroup / source; 332
transmissions: 331 ACTIVE with the right talkgroup, 328 ENDED within 0.06 s
of the last frame, 292 followed by a TDULC termination. Unparsed on air:
MFID 0xD8 opcode 0x34 (279), TSBK 0x2D (3). Not verified on air: TDMA
traffic grants, Phase 2 calls via trunking, control-channel moves, the
uplink sign, unit-to-unit / telephone / data grants, the Phase 2 explicit
forms, MAC_RELEASE. Outside the standard: [GPL-fact OP25 / SDRTrunk]
Motorola MFID 0x90 opcodes 0x02 (patch grant; UNVERIFIED) and 0x03 (its
update; partly verified on a public recording); [INFERRED] Phase 2 0xC0 /
0xC3 / 0xC4 / 0xC6 / 0x25 layouts (UNVERIFIED).

**DMR Tier III** (plugin dmr, 2.1). Grants: CSBKs 0x30-0x38 on the control
channel (TSCC), repeated on the payload channel (P_GRANT): PV / TV / BTV /
PV_DX voice, PD / TD / PD_DX / PD_MI / TD_MI data (`data`). Voice grants
twice 60 ms apart, then late-entry grants (4 every 360 ms, `update`) until
the clear-down - the keep-alive. Channel `LCN n` (number n), `slot` = the
timeslot, options `slot=1|2`, plus `cc` (only bursts of that colour code)
only when the grant names one (absolute parameters). No privacy bit in
grants: `encrypted` comes from the payload channel (LC privacy / PI).
Frequencies only from the signalling: channel 0xFFF = a multi-block grant
with absolute parameters (CG_AP: downlink, uplink, payload colour code), and
Chan_Freq / MV_AP / VN_AP announcements (an LCN -> Hz table, reported as
`trunk_channel`); the fixed / flexible plans of TS 102 361-4 annex C are
programmed into radios, never sent: otherwise `freq_hz` NAN and the user's
table decides. System key model.NET (`Huge.1`, `Small.37`), site `site <n>`,
from C_ALOHA / C_BCAST and the CACH short LC (C_SYS_Parms = control,
P_SYS_Parms = payload: a payload channel reports its system with control =
false - the manager never makes a system or control VFO of a voice VFO's
SYSTEM); `control` while TSCC PDUs were heard in the last 5 s. Every C_BCAST
type decoded; adjacent sites / vote-now give `trunk_channel` for the
neighbour's control channel; C_MOVE to all (mask 0) gives `control_channel`.
Payload channel per timeslot: ACTIVE from the voice LC header or embedded LC
(a voice burst before any LC: ACTIVE with an empty target), ENDED at the
terminator or after 1 s without voice, RELEASED at P_CLEAR (at most one per
2 s; with no call heard the P_CLEAR names the talkgroup); the `slot` option
also limits talkers. Measured on the real network here (2026-11-20; three
sites, colour code 4, key `Huge.1`): control channels 455.75625 / 456.50625 /
457.68125 MHz; only Adjacent_Site announcements (no Chan_Freq, no absolute
parameters); the user's table is ONE rule, f = 455.30625 MHz + LCN x 12.5 kHz
(all 10 channel numbers seen); uplinks not on air; the composite control
channel's slot 2 carries data grants and P_CLEAR. Grant -> first voice burst
0.34-0.58 s (median 0.50 s; 12 of 13 voice grants matched the talkgroup on
the granted carrier and slot; one weak carrier 3.5 s by late entry; one never
keyed up and was cleared 2.0 s later). A call ends with the terminator
(repeated through the hang time; a new transmission inside it is the same
call); 1.98 s after the last terminator 3 P_CLEARs 60 ms apart; data calls
cleared to ALLMSI. Not parsed: Motorola-FID CSBKO 0x19, C_ACKD / C_AHOY.
Capacity Plus: rest channel only - key `CapPlus.CC<colour code>` (weak: two
systems on one colour code share it), `control_channel` `LCN n` when the
rest channel changes, NO grants; [GPL-fact DSD-FME / SDRTrunk] CSBKO 0x3E
octet 2, the rest-channel number [UNVERIFIED]. Connect Plus and Hytera XPT:
detected by name only. Verified on air: system identity, grants, the LCN
plan, call start / end / release. Not verified: absolute parameters
(generator only), C_MOVE, Capacity Plus rest-channel numbering.

**NXDN** (plugin nxdn, 2.1; Type-C trunking from TS 1-A / 1-C 1.3). System
key = Location ID category + system code (`global.123`, `regional.4000`,
`local.70001`), site `site <site code>`, `control` on an RCCH or composite
RTCH_C; from SITE_INFO / SRV_INFO, sent every 10 s. Grants: VCALL_ASSGN, then
VCALL_ASSGN_DUP every Tr_dup (0.5 s) as `update` for as long as the call
lasts; DCALL_ASSGN / _DUP data. From the control channel's CAC, or FACCH1 /
FACCH2 / SACCH of a traffic or composite channel (late entry). Channels
`CH <n>` (1-1023), never a frequency: **the user's table (or a rule, or
learning) is always needed**; no uplink. `trunk_channel` for the 1st / 2nd
control channel, CCH_INFO and adjacent sites' control channels;
`control_channel` on CCH_INFO "New". No encryption flag in grants (the
traffic VCALL gives it); `emergency` from the CC Option; options `ran`,
`rate` (4800 / 9600). Traffic: ACTIVE from VCALL; ENDED at TX_REL, TX_REL_EX
(hang time, Tr_hold 1 s groups / 7 s individual) or a 1 s gap; RELEASED at
DISC. No `grant_end`. Not supported: DFA, Type-D, EFR voice. Verification:
generator and specification only, not seen on air.

**TETRA** (plugin tetra, 2.2). The site runs through the MCCH on timeslot 1
of the main carrier; secondary control channels on timeslots 2-4 are
decoded by the same instance. A channel allocation in a MAC-RESOURCE /
MAC-END becomes a `grant` when its TM-SDU is a CMCE call PDU (D-SETUP,
D-CONNECT, D-CONNECT ACK, D-TX GRANTED, D-CALL RESTORE, D-INFO, D-ALERT,
D-CALL PROCEEDING); with an SNDCP PDU a data grant; none without a call PDU
or with timeslot bit map 0000. Name "<carrier>/<timeslot 1-4>", `number` =
carrier, `slot` = timeslot - 1; downlink = band x 100 MHz + carrier x 25 kHz
+ offset (band / offset from SYSINFO or extended numbering: no user table
once SYSINFO is heard, NAN before); uplink from TS 100 392-15's duplex
table. target = the D-SETUP's addressee, source = the calling party or
D-TX GRANTED's transmitting party; `encrypted` from the end-to-end flag or
D-TX GRANTED's encryption control; `update` = a repeat on the same channel.
Options `timeslot`, `mcc` / `mnc` / `cc`, `call`, `group`. Calls often sit
on another timeslot of the control carrier: the voice VFO then sits on the
control VFO's frequency, its `timeslot` option keeps it to that slot. End:
D-RELEASE gives `grant_end` on the control VFO (only when heard there - same
carrier); on the traffic VFO D-RELEASE or two "unallocated" AACHs give
RELEASED, D-TX CEASED / hang time / 1 s without traffic ENDED. A TETRA
carrier is continuous: the traffic instance reports valid frames only from
its timeslot while assigned; the manager relies on `call()` (CALL ENDED
moves an active call to hang even while frames go on). Limits: timeslot
numbers only after the carrier's first BSCH (up to 1.02 s; `mcc` / `mnc` /
`cc` descramble before, and with them the traffic instance accepts that
cell only - a stronger co-channel cell's calls are never reported, the
wanted call just stops being heard); air-interface encryption hides the allocations
(nothing can be followed; the plugin's "Trunk following" fact says so);
downlink bearings are the base station's; the control VFO still plays the
first traffic timeslot (listening to it too hears the call twice). Verified
on air: grants of a group call to timeslot 2 of the main carrier against the
traffic / talker change there (public recording: 8 of 8 allocations).
Release, other carriers, individual calls, fragment reassembly, extended
numbering: generated signals only.

**MPT1327** (plugin mpt1327, 3.0; analogue FM traffic, `demod = "nfm"`,
12.5 kHz). Every GTC on the control channel (bit 22 = 0) is a `grant`:
"CH n" (10-bit CHAN), target IDENT1, source IDENT2 as "PPP-IIII"; only
error-free codewords on a CCSC-confirmed slot grid. `update` = the same GTC
within 5 s; `data` = bit D (not followed); `emergency` = an ACKE / AHY with E
before the GTC. Group or unit: not in the GTC - from an AHY (POINT 0) or an
earlier call by that ident, else group. Frequencies: only MPT 1343 Band III
is standard (option `plan=mpt1343`, with uplinks); elsewhere NAN and the
user's rule decides (the NZ network measured here: 414.0000 MHz + n x 12.5
kHz, 5 control channels cross-checked). System key = the whole SYS by
default (one table per site); `network=mpt1343` uses OPID / NET (the NZ
network's four sites share "NET-0"). BCAST announce / adjacent / vote-now
give `trunk_channel`; MOVE with (M) = 0 gives `control_channel`. Options:
chan, sys, target, source, group (+ plan / network / link when not default).
Call end on the traffic channel only: MAINT Pressel Off = ENDED, the TSC's
CLEAR = RELEASED; no clear-down on the control channel; no Pressel Off: ENDED
1 s after the carrier drops (talk-through: up until the CLEAR). The
traffic-channel CALL may name only the unit (empty target: the grant's
stays). Talker: the MAINT's unit, "via site" on the downlink. Verified on
air (2026-11-20, night): control-channel decoding of four control channels,
the BCAST channel numbers and the plan. Not verified on air: any GTC or
traffic channel. False reports: 0 over 1 h of noise and 1 h of FM speech.

## Tests (2026-11-20; scratch, not in the repo)

TEST build (ports 28180 / 28181 / 28191 / 28192, MCP 28179, exe
`kraken_doa_trunktest`), a fake heimdall (`fh.py`: a 5-antenna UCA scene of
CW carriers switched on and off at scripted wall-clock times, coherent /
wideband-scan / independent headers and commands), a scratch decoder plugin
`trunktest` driven by a script (a pilot tone = a control channel of a given
system / site, a tone per grant -> SYSTEM / CHANNEL / CONTROL / GRANT /
GRANT_END, a tone per call on a traffic channel -> valid frames, a talker,
CALL ACTIVE / ENDED / RELEASED, audio), and a WebSocket client asserting on
the broadcasts:

- `t1_life` 36 checks: the lifecycle with real timing (grant -> VFO on the
  frequency, decoder / demod / 12.5 kHz / options / Digital squelch / heard ->
  ACTIVE 0.2-0.3 s after the grant -> HANG -> released at GRANT_END ->
  parked, muted, not decimated, still listed); the parked VFO reused for the
  next call; trailing grant updates up to 1.1 s after a RELEASED don't
  restart it; two calls at once; decimator_info only per placement.
  Latency (wall clock, measured by the plugins): the control plugin emits
  the grant -> the first samples of the new channel in the traffic decoder:
  19 ms with a PARKED voice VFO reused (retune + options, no process start),
  134-170 ms with a new VFO (its plugin process starts) - hence the parking;
  the grant's arrival on the loop -> VFO placed 0-4 ms; -> ACTIVE 207-449 ms
  (the test call's tone starts 250 ms after its grant).
- `t2_sites` 29: two control VFOs of one system granting one talkgroup on
  two channels at once (one followed, the other `duplicate`, per-site
  counters, no move between sites); the followed copy released -> the
  waiting duplicate ends with it, its trailing updates don't bring it back,
  no `no_signal`; takeover when the followed site's channel carries nothing
  (no_signal after 3 s -> the other site ACTIVE); two systems with one
  talkgroup number (two calls); `TRUNK_ALL_SITES:1`; the partial centre
  when two control VFOs don't fit (edge clip 0.3).
- `t3_policy` 36: VFO limit without / with priority (pre-empted), a moved
  call (same VFO), a re-assigned channel (VFO handed over, no retune), skip
  and data counted only (no rows, no history), encrypted / unit / unlisted,
  out of span + the suggested centre, unresolved -> user entry -> followed,
  a rule (channel 10 -> 850.1125 MHz, uplink -45 MHz), hold, end, a system
  record made by an edit (heard false, saved, deleted), malformed commands
  (22 + 5 refused with trunk_error), clamping, cut / escaped names.
- `t4_persist` 26: settings and the systems file across a restart; the
  snapshot with a heard trunk VFO present = the user's 2 VFOs and heard list
  `1` only, the user's change during following saved, the user's VFOs back
  exactly after the restart; RESET_SETTINGS leaves the systems file.
- `t5_events` 29: a scanner mid-call (paused, VFOs removed, resumed), a
  centre retune, the user retuning (taken back) / removing a trunk VFO (not
  rejoined), the control VFO retuned / removed, a mode change, the control
  channel lost (not receiving 5.2 s later; the call ends on its own
  hang time), CONTROL (the control VFO moved and saved, a second within 10 s
  waits); a scanner's moved control VFO put back by the manager (scan_restore).
- `t6_learn` 13: learning from a user's VFO (two observations, then
  followed), the refusals, a learned entry dropped after two no_signals.
- `t7_stress` 16: a flood of 3000 grants / s for 5 s (15340 events, none
  dropped, 64 call rows, GET_TRUNK answered in <= 2 ms, 14 % of a core),
  garbage strings (valid JSON, no control characters, keys sanitised), the
  16-VFO limit; CPU: the idle manager and a parked VFO cost nothing
  measurable (kraken_doa 9-11 % of a core either way here), a parked VFO's
  plugin process 0.0 %.
- `t8_playback` 10: a recording of two calls played back with following on:
  both followed; nothing to heimdall; the systems file and the settings file
  untouched; the playback's edits and entries gone after it.
- `t9_modes` 12: independent (a call on tuner 1, one on the control VFO's
  tuner 0, out of span past every tuner, no tuner retuned) and wideband scan
  (calls on tuners 3 and 1 at the right offsets, out of span on a seam and
  past the spread, no retune).
- `t10_log` 2: one `call` record in the decoder log with the control VFO at
  Log, none at Off.
- `t11_queue` 8: the queue bound shrunk to 64, VFO 0 flooding 3000 grants / s:
  events dropped, VFO 1 (another system's control VFO, following a call)
  retuned in the middle - its RESET arrives, its call ends `control_reset`; a
  grant option the traffic plugin doesn't declare dropped (counted, one log
  line), the declared ones set.
- 217 checks, all passing on the final build. Found on the way: a
  temporary system merged into its keyed one erased the KEYED one (merge_temp
  took the Track's system id by reference and rewrote it) - the keyed system
  came back empty and the temporary one stayed as a ghost with a stale
  control entry, as the first real P25 run showed; fixed. (The test scene:
  strong carriers summed into uint8 clipped and made intermodulation tones on
  other channels - the scratch plugin's tone detector took them for calls;
  the scene's carriers are kept small since.)

On real captures and the plugins' scenes (the orchestrator's runs on the
merged tree, 2026-11-20): an 80 s off-air P25 capture replayed as heimdall,
three control VFOs on three sites of one system, `TRUNK_MAX_VFOS:4`: 476
grant messages, 41 grants, 43 calls followed on the reference frequencies
and times, voice VFOs parked and reused, 9 duplicates not double-followed,
963 events none dropped, place 0 ms (max 357), ACTIVE 87 ms (max 1457) -
the 4 `no_signal`s were late duplicate take-overs (fixed since: they end with
the followed copy). A 100 s DMR Tier III capture, three sites, the one-line
rule: 443 grant messages, 33 grants, the voice call on LCN 81 slot 0
resolved by the rule, ACTIVE 0.3 s after the grant, four overs, released by
P_CLEAR after 15.7 s; its copy on site 3 not followed; 30 data grants (now
counters only). Debug playback of every plugin's trunk scene:
p25-trunk-three-calls 8 / 8 followed (the Phase 2 call on slot 1),
dmr-tier3-trunk-three-calls 7 / 7 (two timeslots of LCN 301 and the
absolute-parameter channel), tetra-trunk-two-calls 5 / 5 (one on a timeslot
of the control carrier), mpt1327-trunk-two-calls 5 as analogue FM (2 data
grants not followed), nxdn-trunk-two-calls (with the rule in its
`commands`); every expected talker at its bearing, every call `released`.

The same two captures on the final build (2026-11-20, the TEST build, the
first 80 / 100 s): P25 - 486 grant messages, 49 grants (overs), 39 followed,
9 duplicates not double-followed, 2 `no_freq` (before the identifier tables
were heard), 0 `no_signal`, ONE system listed (no temporary ghost), 38
history entries (37 `released`, 1 `ended`), 945 events none dropped, place
median 0 ms (max 102), ACTIVE median 103 ms (max 296), 45 `decimator_info`
in 82 s (was 184 in 80 s). DMR Tier III with the rule sent at once (the
system record made before its control channel was heard): 110 grant
messages, the voice call followed (ACTIVE 34 ms after the grant reached the
loop), its site-3 copy `duplicate`, 8 data grant sequences counted only (no
rows), 6 `decimator_info` in 102 s; the suggested centre at the default edge
clip 0.8 keeps 2 of the 3 control VFOs (455.756 - 457.681 MHz don't fit
1.92 MHz together): the partial one, `controls` 2 / `controls_of` 3.

AddressSanitizer + UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`,
the whole TEST build and the scratch plugin) over t1 / t2 / t3 / t5 / t8: no
report. Under the sanitizers (3-5 x slower plugins) t1-t3 and t8 pass; t5's
CONTROL part misses its 0.6 s grant tone (5 of 27, timing only).

## Page

`kraken_doa.html`, script section "📡 TRUNK FOLLOWING" (`tk…`), the box
`data-sb="trunk"` after Saved Frequencies (all operating modes), the editor
`#tk-dlg`. The page shows the receiver's state and sends its commands; no
optimistic updates - every page redraws from the broadcasts (a second tab
follows at once). `tkSt` (the last status) and `tkLists` (lists by system id)
sit in the globals block: the VFO cards and `drawBox` read them.

**Compatibility.** The page sends `GET_TRUNK` on connect. The box is
`display:none` until a status arrives, so a receiver without trunk following
leaves the page as it was (its decimator_info has no `trunk`, its plugin
list no `trunking`: no card line, no tag). Fields a receiver doesn't send
are left out of the view (no `heard`: no "Add a system"; no `scan_restore`:
no restore notes; why `ended` from a receiver before `stopped` reads "Stopped
by you" too; a parked voice VFO is `trunk.call` 0, or `trunk.state` parked
from a receiver before). Each system on the air gets one `GET_TRUNK_SYS` per
connection (talkgroup names and modes, the channel table); the receiver
broadcasts the lists again when they change.

**The box**, top to bottom:
- *Follow calls* (a switch, `TRUNK_FOLLOW`), the state (Off / Following /
  N followed / Paused / Playback) and ⚙, which folds out the rarely used
  settings (the saved-frequency scan's pattern): Voice VFOs, Hang time,
  Encrypted calls, Unit-to-unit calls, Hear calls, Every site, and "System
  ahead: Add…". The header's badge: "N calls" followed, or "Paused".
- The hold line ("Holding on TG … · system", Release) while `hold` is set.
- Notes - why nothing happens, each with the action that fixes it: Paused
  (which scanner); a control VFO a scan moved, put back or not (the
  receiver's `scan_restore` note, for its 60 s); Playback; no control
  channel (what to do, the decoders with `trunking`; a VFO running such a
  decoder with no system yet: "no control channel heard yet"); "N channels
  without a frequency (…)" + Enter (the editor's Channels); "N calls outside
  the span" + Centre (coherent mode, not during a playback, only when the
  suggested centre covers more than the current one: `uiConfirm` naming the
  frequency, what it covers and - with `controls` < `controls_of` - how many
  control channels stay in, then the normal `FREQ:` through
  `sendFreqChange`); "N calls waiting for a voice VFO" + "+1 VFO"
  (`TRUNK_MAX_VFOS:n+1`); 16 VFOs in use; nothing received on N calls +
  Channels; the call's decoder not built; "No speech: the X codec is not
  installed" + Decoders (Setup Settings, from `{"codecs"}`: p25 IMBE, dmr /
  nxdn AMBE, tetra ACELP; the existing codec warnings stay as they are); a
  control VFO's `note`. Counts come from the receiver's `calls` (they stay
  30 s after their end), so a note does not flash with each call; a note
  about the traffic stays 20 s after its last cause (`TK_NOTE_HOLD_MS`), and
  while the pointer is on what lies below the notes (systems, calls,
  recent) notes may change their text but not come or go - at most 15 s
  (`TK_HOVER_MAX_MS`: a new note is not held back for good).
- Systems: one row each (on the air first): the name (the user's, else the
  reported one, else the key; "Unidentified (Dn)" + "Waiting for the
  system's identity" - or "Its control channel is not received" - for a
  temporary one), the decoder unless the name starts with it, the site; the
  control VFOs as chips (Dn, MHz, a green / red dot = received / not for 5
  s; a click selects the VFO); "N calls · N followed · N not · N TGs" (the
  reasons, data and skip included, in its tooltip); Edit. "Not heard yet"
  for a record with `heard` false (entered ahead of time), "No control
  channel now" for one heard before. A temporary system whose control VFOs a
  keyed system has too is not shown (the receiver folds them since
  2026-11-20; kept as a guard for one from before). A temporary system's
  Edit and its calls' talkgroup actions (hold, name, mode, System…) are
  disabled, with the reason in their tooltip: what is entered for a system
  before it has identified itself (usually seconds) would not carry over to
  the keyed system it becomes - Stop following works.
- Calls (in progress): the receiver's `calls` (data and skip grants make
  none), plus `recent` ones for `TK_LINGER_MS` (6 s, dimmed "Ended"), in
  first-seen order: new calls at the bottom, and a conversation's next over
  (the receiver keeps it as the same call) lights its own row again. Rows
  are keyed (`tkSyncRows`): made once, only their text changes. While the
  pointer is on the list or a row's menu is open (`tkFreeze`) no row leaves.
  Each row is two fixed-height lines: state (Waiting / Setup / Active / Hang
  / Ended, a coloured edge), the talkgroup's name or "TG n" / "Unit n",
  marks (lock = encrypted, warn = emergency, P = priority), the voice VFO
  (click: select it), stop and ⋯; then the reason it is not followed first
  (words a user can act on, `TK_WHY`, the long form in the tooltip; "Set its
  frequency" opens the channel table), the talker, the channel / slot and
  MHz, the site.
- Recent: `history`, newest first, made once each: time of the end, the
  label, marks, VFO, ⋯; duration and how it ended (`TK_END`) or "Not
  followed: <why>".
- **During a playback** the receiver keeps the user's systems (without
  control VFOs) and the history from before, to put them back at its end;
  the box shows only what is the playback's: systems a control VFO hears
  now or a call belongs to, and recent calls of those systems that ended
  after the page first saw the playback (`tkPbSince`, the receiver's `now`;
  the user's calls its start ended are not the scene's either; the Recent
  count's tooltip says how many from before are not shown). The Playback note says
  so. A page opened mid-playback hides the scene's calls that ended before
  it connected.

**Stop following** (the row's stop button, or the menu): `TRUNK_END:<id>`.
On the page it means *this call row*: the receiver frees its voice VFO and
does not follow the call's further transmissions on that site while the row
is listed (until 30 s without a grant; why `stopped`, "Stopped by you" in
the row, its stop button gone - there is no undo). The same talkgroup on
another site, other talkgroups and the settings are not affected; after the
row has expired the talkgroup is followed again. A voice VFO removed with ×
on its card does the same. To keep a talkgroup from being followed for good,
the menu's Skip.

**Per call / talkgroup** (⋯, a menu in the View popover's dismissal
pattern - `popDismiss`; keyboard Up / Down, Esc back to the button): Stop
following this call, Hold on this talkgroup / Release the hold
(`TRUNK_HOLD`), Name… (`uiPrompt` -> `TRUNK_TG_SET` name - which also lists
it), Priority / Normal / Skip (the current one ticked, from the lists),
System… (the editor). A unit call has Stop and System only.

**A system ahead of time** (⚙ → System ahead: Add…, shown when the receiver
reports `heard` - one from before would refuse an unheard id): a `uiPrompt`
with the decoder (one of the plugins with `trunking`, checked), the key and
an optional name. The key field's placeholder is the decoder's key format
(from the Protocols above and the plugins' READMEs: p25 WACN.SYSID in hex
"BEE00.14C", dmr model.NET "Small.37", nxdn category.system code
"global.123", tetra MCC-MNC "530-1", mpt1327 SYS in 4 hex digits "42E1" or
NET-n), checked against the receiver's key rule (`[A-Za-z0-9._-]{1,48}`).
`TRUNK_SYS_SET` makes the record; the editor opens on its Channels as soon
as the status lists it. It is listed "Not heard yet" until its control
channel is heard; a wrong key makes a second, separate system (the help
says so).

**The editor** (`tkDlgOpen(id, tab)`; the Setup Settings dialog's look,
sections as a strip across the top, full screen below 640 px; Esc / × /
the backdrop close it). Lists are drawn from `trunk_sys`, never while the
user is typing in them (drawn when the field is left); at most 200 rows,
search for the rest.
- System: Name (Enter / leaving saves, `TRUNK_SYS_SET`), Listed talkgroups
  only (switch), what is known on the air (decoder, key, reported name,
  site, control VFOs), the counters with every not-followed reason in words,
  Forget this system (`uiConfirm`, `TRUNK_SYS_DEL`).
- Talkgroups: add (number, name, mode), search, a table (number - heard-only
  ones dimmed -, name field, mode select, last heard, calls, delete - a
  named or non-normal one asks first).
- Channels: first the missing channels (the status' `unresolved` less those
  the table resolves by now, plus granted entries without a frequency):
  each with a MHz field + Set (`TRUNK_CH_SET`, its number kept), then the
  rule form "Channel numbers to frequencies" - First, Last (empty: up to
  4096 channels inside 0-6 GHz), Base (MHz), Step (kHz, negative allowed),
  Uplink offset - with a live preview of what the missing channels get
  ("LCN 81 → 456.31875 MHz · …", or the rule's first channels and a warning
  when none of the missing ones is in it), errors in place under the
  fields; the rules (edit fills the form, delete asks); the channel table:
  missing first, the frequency with its source tag (user / rule / announced
  / learned), uplink, calls, last (uplink and last left out below 640 px);
  edit (`uiPrompt`, downlink + optional uplink -> a user entry), clear the
  user entry, forget a learned one, delete the entry.
- Units: heard units (not saved), searchable.
A refusal (`trunk_error`) is shown only by the page that sent that command
(the first 64 bytes matched, 10 s): in the editor's error line, else a
quiet `uiAlert`.

**Trunk VFOs elsewhere.** The VFO card gets a line under its header
(`tkRenderCards`, called from `digRenderCards`): a voice VFO "TG 1201 Fire
· Active · 1-412" (its state and call from the status' `vfos` / `calls` -
decimator_info comes only when a voice VFO is made, removed or placed) or
"Trunk VFO · parked" (the card dimmed); a control VFO "Control · system ·
site" (red with "no signal"); a traffic VFO "Traffic · system". **What is
editable on a voice VFO:** the receiver sets frequency, tuner, bandwidth,
demod, decoder, squelch and Transcribe for each call and takes back a
change at the next grant, so on the card they are disabled (the frequency
digits locked like a fixed-frequency decoder's) and on the spectrum,
waterfall and independent panes its bar is drawn without grab handles, not
hit-tested, dashed while not active, with "Dn label" under the other VFOs'
labels; a parked one is not drawn. A click on the spectrum, the VFO's
digits, a pane, or a saved frequency does not retune a selected voice VFO
(`vfoNoTune`; a saved frequency says "Dn is a voice VFO of trunk following
- select another VFO"). The speaker, Save and × (stops following its call,
as above) work, and its decoder tab shows its decoding. The plugin list
(Setup Settings → Decoders) tags the plugins with `trunking`.

**Text from the air** (system, site, talkgroup, channel and unit names,
notes, reasons): `digEsc` / `textContent` everywhere, `unicode-bidi:
isolate` on names and `<bdi>` around each one inside a sentence (a
right-to-left override can't turn the rest of the line), cut to 40
characters inside notes.

### Page tests (2026-11-20; scratch `tmp/page/`, not in the repo)

A TEST build on 2848x / 2849x, headless Chromium with real mouse, key and
touch events; the real P25 (300 s, 499.8 MHz) and DMR Tier III (600 s,
456.8 MHz) captures replayed as heimdall, the five catalogue scenes through
the debug playback (Play only).
- Step 1, the real P25 capture, 60 s (first build): 200 status messages
  (3.3 / s, 14 KB), 18 lists, 125 decimator_info; up to 5 calls per status.
  The final receiver, 90 s: 296 status, 17 lists, 32 decimator_info.
- The whole pass on the final receiver: 15 suites, 194 checks, 0 failures
  (load average 15-30 from other jobs on the 16-core machine):
  - row stability, 60 s of the P25 capture (7 calls, 31 state / VFO
    changes): 0 rows re-made, 0 pushed down by a new call; with the pointer
    on the list nothing re-made or moved in 14 s and, later, 30 s (the list
    itself held still for the first 15 s); status handling median 1.0 ms,
    max 30 ms;
  - real states: paused by the continuous scanner (no voice VFO while
    paused), resumed, the moved control VFOs put back and said so (2 of 2,
    "D1: put back on 498.937500 MHz after the continuous scanner"); Voice
    VFOs 1 with Every site on: "2 calls waiting for a voice VFO" after 1 s,
    "+1 VFO" raised it;
  - fixtures (out of span + Centre: Cancel sends nothing, Retune sends
    `FREQ:499.512`; no decoder; codec missing; a control note; a lost
    control channel) and an older receiver (every trunk message and field
    dropped: box hidden, no card line, no tag): 18 checks;
  - actions with a second page checking (Name with a hostile name, Priority,
    Hold / Release, Stop following -> why `stopped`): 15;
  - 64 calls + 50 history rows with hostile strings in every field: 99 ms
    for the first status, then median 2.8 ms; 500 / 2000 talkgroups + 2000
    channels + 300 units in the editor 63-90 ms; nothing wider than the box,
    no script run, no element made;
  - the five scenes with nothing but Play (p25, dmr, nxdn - its rule from
    its own `commands` -, tetra, mpt1327), played right after the live P25
    system was followed: the keyed system recognised, only it listed, only
    its calls in Recent, a call followed, the voice VFO's card "TG … ·
    Active · <channel>", its system gone at the end; 9 checks each, 10 in
    a second run after Recent left out the user's calls the playback's start
    ended (NXDN:
    the first grant, TG 100 at 1.5 s, comes before the plugin names its
    system, so it waits `no_freq` in the temporary system; TG 200 is
    followed, TG 100 on the loop's next pass.)
  - the missing-channel workflow on the real DMR capture (the note names LCN
    207 / 81; Enter; LCN 81 / 207 / 54 on top; rule First 1, Base
    455.31875, Step 12.5 previewed "LCN 81 → 456.31875 MHz · …"; Add rule ->
    no missing channel; a call followed on its rule frequency): 9;
  - the editor at 1500 and 390 px (rename with RTL override + emoji +
    markup, talkgroups add / mode / rename / search / delete, listed only,
    a channel's user entry and its clearing, a rule's edit, a bad rule's
    errors in place, a system added ahead of time - `dmr:Test.99`, the key
    field's placeholder "model.NET, e.g. Small.37", the editor opening on
    its Channels, its rule kept, "Not heard yet", `heard` false -, Forget):
    27 and 29;
  - touch at 390 px (help mode explains Follow calls without flipping it,
    40 px menu items): 8; reload mid-call (back in 0.8 s), the sidebar
    zoomed to 420 px, hover help, Drive mode in / out, the saved-frequency
    scan pausing following, the decoder tabs: 13.
- The new page against the first-pass receiver (no `heard`, no
  `scan_restore`, why `ended`, `trunk.state` in decimator_info): box,
  systems, calls, voice VFO cards, menu, Stop following ("Stopped by you"),
  editor, no "Add a system", no errors: 11 checks.
- `tools/theme_palette.py --check` OK; no console errors in any run.
