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
deque (`QUEUE_MAX` 4096; more are dropped and counted, `events_dropped`),
and posts one `loop->defer` drain when the queue goes non-empty - a grant
reaches the loop within a few ms, not at the next tick. Each event carries
the decoder's address: events of a decoder that is gone (VFO removed, id
reused) are dropped. A timer on the loop (`trunk::tick`, `TICK_MS` 50) drains
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
channel numbers lack a frequency, how often granted). Every granted channel
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
| `ended` | the user ended it (`TRUNK_END`) or removed its voice VFO: not rejoined while its grants go on (nor at its next overs within the RECENT time) |
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
the grant's `options` as `<decoder>.<key>`; options of the previous call that
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
- Removed by hand: simply gone - its call isn't rejoined (why `ended`).
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
  another target (not before 300 ms after the placement): re-labelled to
  that target if it may be followed (an existing waiting call of that target
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
`unlisted`, `no_decoder`, `no_signal` - not `off`, `paused`, `hold`, `ended`,
`duplicate` (the followed copy is there).

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
  `events_dropped`, `calls_dropped` (grants refused a row: 64 calls already),
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
MAC_END_PTT / HANGTIME -> ENDED, MAC_IDLE / MAC_RELEASE -> RELEASED per LCH;
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
`cc` descramble before); air-interface encryption hides the allocations
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

- `t1_life` 35 checks: the lifecycle with real timing (grant -> VFO on the
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
- `t3_policy` 34: VFO limit without / with priority (pre-empted), a moved
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
- `t5_events` 27: a scanner mid-call (paused, VFOs removed, resumed), a
  centre retune, the user retuning (taken back) / removing a trunk VFO (not
  rejoined), the control VFO retuned / removed, a mode change, the control
  channel lost (not receiving 5.2 s later; the call ends on its own
  hang time), CONTROL (the control VFO moved and saved, a second within 10 s
  waits).
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

AddressSanitizer + UBSan over t1-t3: ASAN_RESULT.
