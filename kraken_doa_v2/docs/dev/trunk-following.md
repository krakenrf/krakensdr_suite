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
the granted channel - inside the receiver's span; it retunes only when the
user allows it (an excursion outside the span in coherent mode,
`TRUNK_RETUNE`; voice tuners in independent mode, `TRUNK_TUNERS`, below) - for
the length of the call, so the call is heard, decoded,
transcribed and direction-found (the voice VFO's own MUSIC / talker DoA).
The decoder on the traffic channel reports what it carries
(`kp::Host::call`), which drives the call's state.

- Off by default (`TRUNK_FOLLOW:0`), and off means nothing changes for a
  user who doesn't turn it on: no VFO is made or moved (no CONTROL move, no
  scan restore), no command is dispatched, nothing is written to disk but
  what the user asked for with an edit command (`trunk_systems.json` then
  holds only the systems they edited). What it still does: track systems,
  grants and calls in memory for the box (calls wait with why `off`), within
  the bounds below; the status is built only on a change and once a second
  while a control VFO reports (not per tick, not per event; the counters
  ride along - a counter alone sends nothing).
- Events come only from plugins whose registry entry declares `trunking`
  (others are dropped: `untrunking_dropped`); a grant's traffic decoder must
  declare it too (else why `no_decoder`).
- The user's own VFOs are never touched, except - with following on - a
  control VFO moved by a `CONTROL` event and a control VFO put back after a
  scanner (below); neither is saved by itself (both go through the
  dispatching guard): the VFO set the user saves next carries the control VFO
  where it then is - the current control channel, deliberately. Voice VFOs
  are never saved (`DECIMATORS:`).

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
  A temporary system can't be edited (its entries wouldn't be its key's:
  the command is refused - "this system has no key yet"; forgetting it,
  `TRUNK_SYS_DEL`, works); its history rows are re-keyed at the merge.
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
`events_dropped`); a RESET is never dropped - it is queued past the bound
unless that VFO + decoder's LAST queued event is a RESET already
(`resets_coalesced`; a RESET followed by later events of the VFO doesn't
cover a newer one): a lost RESET would keep stale calls or a stale control
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
| `no_decoder` | the decoder is no built plugin, one that doesn't declare `trunking`, or one that holds its VFO (`fixed_freq_hz`, `lock_vfo`) |
| `no_freq` | the channel's frequency is not resolved |
| `out_of_span` | it can't be placed (below) |
| `leaving` | the receiver is going to its channel (an excursion, below): placed on arrival |
| `away` | the receiver is away for another call and its channel is not in the away span |
| `busy_here` | outside the span, but a followed call is in progress here and this one doesn't outrank it (priority / the held talkgroup do) |
| `retry` | outside the span, and an excursion for this call found nothing (or ran out of time) less than 30 s ago |
| `no_retune` | outside the span, and no centre in the tuner's range holds it |
| `tuning` | independent mode: a voice tuner is being retuned for it (placed once the stream shows the tuner there) |
| `retuning` | coherent mode, an excursion leaving / returning: the stream isn't at the new centre yet - placed at its next grant once it is (no placement against the old centre, no excursion started from the wrong one) |
| `no_tuner` | independent mode: outside every tuner's span and no voice tuner is free (each carries a call in progress) |
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
P25 run were such late take-overs). A copy that was followed first, lost its
VFO (e.g. `no_signal`) and waited while the other site's copy took over ends
`taken_over` (not `duplicate`): its history row says `followed` true and
`lost` why it lost the VFO. A grant from site B never moves the
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

### Following outside the span: an excursion (coherent mode, `TRUNK_RETUNE`)

Off by default. With it on, a grant that passes every rule but is
`out_of_span` in coherent mode makes the receiver leave: retune the centre
so the channel is in the usable band, calibrate at once, follow the call
there with a voice VFO as usual (DoA valid once the calibration has
converged), and when it is over go back to the centre it had ("home"),
calibrate at once again and pick the control channel up again. The price:
in coherent mode every VFO moves with the centre - the user's own VFOs are
carried along while the receiver is away (the page says so where the option
is switched on).

- **Where to.** A centre that puts the channel AND a control VFO of that
  system in the usable band (both at least 30 kHz off DC: the RTL-SDR's DC
  spike and LO leakage), the one nearest home - then that control VFO keeps
  receiving (its offset is moved so its RF stays) and no grant is missed
  (`how` "control"). Else the channel 250 kHz from the centre, on the side
  towards home (`how` "offset"): well clear of DC and its I/Q image's
  partner, inside the flat part of the band, the rest of the span towards
  home. The centre must be in the tuner's range (24-1766 MHz; the Wideband
  downconverter: its RF union range, the same `FREQ:` path - else
  `no_retune`).
- **The VFOs.** A "control VFO" here (the excursion and the voice tuners
  alike) is every VFO that is not a voice VFO and whose decoder is a
  trunking plugin - also one that has not reported a system yet, or whose
  entry a RESET erased until it reports again. Those that fit the away
  span keep receiving there (offset moved so the RF stays); the others are
  PARKED (disabled: no bogus SYSTEM / grants from whatever is on that
  frequency, no temporary systems; their entries in the systems' control
  lists say `parked` true and `rx` false - the page shows them as parked,
  not lost) and put back on the return. Each is remembered by its
  instance (`DecimatorInstance::serial`): a VFO removed while away and its
  id reused (by the user or a voice VFO) is left alone. In-span calls
  being followed when the receiver leaves end `excursion`.
- **What is saved while away** is the home state: a VFO snapshot recorded
  then (the user's own edit of another VFO) holds every control VFO at
  its HOME offset (`trunk::home_offset`), never the away one; the centre is
  saved only by the user's own `FREQ:`. The Wideband downconverter's mixer
  side, if the excursion's centre made it switch, is not saved and is put
  back on the return.
- **Immediate calibration** with heimdall as it is (`heimdall_v2/docs/dev/
  calibration.md`, "Calibration hold"): `hold_calibration` 3000 ms ->
  `FREQ:` -> `hold_calibration` 0 in one go. heimdall retunes and enters the
  cooldown inside `set_frequency`, so the release finds the hold in effect
  and backdates the cooldown: the calibration starts at the lag driver's
  next tick (at most 1 s) instead of 3 s later. While it calibrates the
  noise source replaces the antennas: nothing is received (the start of
  the call on arrival, the control channel on the return); the voice VFO's
  SETUP clock waits meanwhile (no `no_signal` from the calibration).
  `TRUNK_RETUNE_CAL:0` ("only listening"): the hold is kept for the whole
  excursion (lease 3 s renewed every second - a receiver that stops
  renewing lets it lapse) and released at the return - no gap in the call,
  no DoA away. The calibration after the RETURN is always immediate.
  KerberosSDR manual mode (header flag 0x100): no calibration requests; the
  excursion still works, the calibration is STALE after it as after any
  retune there.
- **When it leaves.** Only when no followed call is in progress in the
  span - a priority talkgroup's or the held talkgroup's grant leaves an
  ordinary call (the pre-emption ranking; else why `busy_here`). Never in
  a playback (`out_of_span` as before), while a scanner runs (tested
  directly, not the once-a-tick pause: a grant right after a scan starts
  doesn't leave), with following off, while heimdall sends nothing (no
  packet for 2 s: `no_retune`), or while an earlier excursion never
  arrived: `not_arrived` blocks every excursion for 30 s, doubling with
  each further one up to 8 min (`no_retune` meanwhile). The calibration
  hold's lease is renewed only while heimdall's packets arrive.
- **When it returns** (`end`): `call_over` - no followed call in progress
  for the hang time (`TRUNK_HANG`) after the last one ended (on systems
  that grant every transmission anew the next over usually comes on the
  same channel within it: the receiver stays for it, and calls that fit
  the away span are followed there meanwhile); `no_signal` - the SETUP
  time-out (nothing received) - then that call can't send it out again for
  30 s (why `retry`); `limit` - `TRUNK_RETUNE_MAX` (a wrong table entry or
  a continuous carrier can't strand the receiver; also 30 s retry);
  `command` (`TRUNK_RETURN`); `off` (following or the option switched
  off); `mode` / `scanner` / `playback` - the command that changes the mode
  or starts a scanner or a playback goes home first, then runs
  (`trunk::before_command` in `apply_command` - also when that command is
  then refused); `not_arrived` - the stream didn't reach the new centre
  within 5 s; `shutdown` - kraken_doa stopping while away goes home first
  (one command round on the loop before the last save - best effort: if
  heimdall doesn't take it, heimdall stays at the away centre and keeps
  that until kraken_doa's next start puts it home). A mode switch from
  heimdall's side (not a command) ends the excursion without a retune.
  The voice VFOs' SETUP clocks wait for the calibration at most 5 s after
  the arrival (a calibration that never converges can't keep a call from
  timing out). `user_retuned`: the user retunes
  the centre while away (`FREQ:` / `WIDEBAND_FREQ:` from anyone but the
  manager) - the excursion is over WITHOUT a return (he took the
  receiver; the control VFOs back on their offsets). The user's own
  retune / decoder change of a control VFO is never taken for the
  excursion's (its expected-reset window is cleared by that command): its
  calls end `control_reset` as at home.
- **A control VFO's report without a key** after a RESET the manager
  expected (an excursion's retune, a CONTROL move): the system it had
  stands (decoders report the system before they have decoded its key
  again - P25 does, and this ended the call that sent the receiver out,
  8 of 8 on air, before 2026-11-20). An unexpected RESET (the user retuned
  it, its decoder changed) forgets the VFO's system first, so a keyless
  report then starts a temporary system as before: nothing of the old
  system is carried to another network's control channel. After the return the
  receiver waits for a control VFO to report again (`returning`, at most
  20 s), then carries on; a grant update of a call still in progress is
  heard as late entry and may send it out again - intended.
- **Nothing of it is the user's tuning.** Every `FREQ:` / VFO move goes
  through the dispatching guard (not saved; the scanners' "retuned by hand"
  stops don't trip); the holds go to heimdall directly. A restart while
  away comes back home (home is what is saved). A playback can't start
  away: its start command returns first.
- Wideband scan mode: no excursions (`out_of_span` as before).

**Measured on the KrakenSDR** (2026-11-20, the 3-site P25 system, home
498.2 MHz with two control channels in the span and every voice channel
outside it; two clean runs of 6.1 min - 11 excursions before the review
fixes, 6 after (the system was quieter) - every one `how` "control": one
control VFO stayed in the away span, the other was parked; heimdall's own
status checked every 30 s - mode, centre and gain only ours):
- A plain retune (the user's `FREQ:`, no hold; 4 of them): noise source
  on 3.04-3.09 s after it, CONVERGED 3.41-3.54 s after it.
- An excursion: retune -> the stream at the new centre 20-82 ms; noise
  source on 0.02-0.46 s after the retune (the lag driver's next tick),
  CONVERGED 0.26-0.88 s after it (arrival -> CONVERGED 400-850 ms). The
  calibration costs a reception gap of 0.32-0.49 s (noise on -> off), on
  arrival and on the return alike.
- The call's first valid frames (ACTIVE) 0.18-1.28 s after the grant made
  the receiver leave (the gap lies in it when the calibration comes first).
- The return: CONVERGED 0.41-0.90 s after the retune home, a control VFO
  reporting again 0.34-0.85 s after it.
- `TRUNK_RETUNE_CAL:0` (3.1 min, 7 excursions): no calibration while away
  (no gap in the call, no DoA there); the one at the return: noise on
  0.04-0.08 s after the retune home (the hold released with it), gap
  0.38-0.44 s, CONVERGED 0.44-0.47 s after it.
- A busy system: 1.8 excursions a minute, half the time away; 26 calls
  started while away, 0 missed because of it (the control VFO in the away
  span heard their grants; calls that fit were followed there); 35 overs
  ended `released`, 1 `ended`, no `no_signal`. The control VFO that stayed
  in span decoded 2.67 grant messages / s away (1.21 / s before - the
  system's load, not a loss).
- After the review fixes: 6 excursions, 14 overs `released`, no other end;
  9 calls started while away, 1 missed (why `away`).
- Not exercised on air: the DMR Tier III system (4 min, home 456.2 MHz)
  granted only inside the span - no excursion; MPT1327 (busy by day) not
  run; the Wideband downconverter (none here).
- Found on air and fixed: after the retune the P25 decoder's first SYSTEM
  reports carry no key, which the manager took for another system and
  ended the call that sent the receiver out (`control_reset`, 8 of 8 in the
  first run): a keyless report now keeps the VFO's system.
In the fake heimdall's calibration model (the scratch tests below: 3 s
cooldown, a once-a-second lag tick, 0.6 s calibration) the same sequence
gives arrival -> CONVERGED 0.4-1.5 s - MODEL numbers, not the hardware's.

### Independent mode: voice tuners (`TRUNK_TUNERS`)

In independent mode every tuner has its own frequency (`TUNER_FREQ`, no
calibration, no DoA). The user names the tuners the manager may retune for
voice channels ("voice tuners", e.g. `TRUNK_TUNERS:1,2,3,4`; empty = as
before). A tuner carrying a control VFO (the control tuner) is never
retuned, listed or not. For a grant:
1. a tuner (any) whose usable band holds the channel: the voice VFO there,
   no retune (the control VFO's tuner first) - as before; a voice tuner
   carrying a call takes another whose channel fits its span;
2. else a voice tuner with no followed call in progress (one the manager
   holds already first): retuned (`TUNER_FREQ` through the dispatching
   guard) so the channel is in its usable band, at least 30 kHz off DC,
   at the centre that also holds most of the system's known channels
   (weighted by their grants; ties: the channel 250 kHz off the centre) -
   the next grants of the system then often need no retune. The call
   waits (`tuning`) until the stream shows the tuner there, then is
   placed. The voice tuner takes the control tuner's gain while the
   manager uses it (the same band);
3. else `no_tuner` - or a priority talkgroup's call takes a voice tuner
   whose EVERY call ranks lower (the oldest of them `preempted`, the others
   `returned`; nothing is ended before the new centre is known). With no
   listed tuner able to carry voice at all (each a control tuner): as
   before, `out_of_span`.
Up to four calls outside every span at once with five tuners; with two
generic RTL-SDRs (heimdall's generic mode, flag 0x1000) one control tuner
and one voice tuner. A voice tuner is the manager's: the user's own VFOs on
it move with it (the page says so). Nothing of it is saved (the `TUNERS:`
record honours the guard, and every saved `TUNERS:` - the user's change of
another tuner too - holds a taken tuner's HOME frequency and gain,
`trunk::tuner_home`). Each voice tuner goes back to the frequency and
gain it had when the manager first took it when following or the list is
switched off (or the tuner leaves the list / gets a control VFO), and
before a mode change, a scanner or a playback starts (`before_command`:
back first, then the command; a mode switch from heimdall's side or a
playback: no command, but the remembered home values are put back into the
tuning kraken_doa replays at the next switch); a voice tuner whose frequency
or gain the USER changes is his again (no restore), as are all after his
`FREQ:` (every tuner) or `TUNERS:`. A tuner whose gain was never set (-999)
keeps the gain it had while the manager used it (the control tuner's): there
is no gain of its own to go back to.
The saved-frequency scan also uses "free tuners" in this mode: the
manager gives its voice tuners back before the scan starts and pauses
while it runs, so the two never hold a tuner at once.

Measured on the KrakenSDR (2026-11-20, P25, every tuner at 498.2 MHz, the
control VFOs on tuner 0, `TRUNK_TUNERS:1,2,3,4`, 5.1 min, two clean runs):
the first out-of-span grant took tuner 1 to 499.47 MHz (the centre holding
most of the system's known channels), retune -> the stream there 48 ms; every
later voice channel of the system (499.04-499.86 MHz) fell into that tuner's
span: no further retune, 28 overs `released`, 1 `ended`. Four calls on four
voice tuners at once did not happen on air (this system's voice channels fit
one tuner) - only in the tests. The control tuner kept decoding while the
voice tuner was in use: 1.66 / 1.69 grant messages a second on its two
control VFOs (0.77 / 1.93 before - the system's load).

Not done: a single-tuner trunk tracker (independent mode, no voice tuner
free or configured: an excursion of the control VFO's own tuner) - with no
voice tuner a call outside every span stays `out_of_span`.
Wideband scan mode: no retunes (`out_of_span` as before).

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
- A voice VFO that can't be removed because it is the receiver's last VFO
  (the user removed theirs) is handed back to the user as a plain VFO:
  enabled, NBFM, its decoder off, muted, where it was - never a hidden
  disabled one.
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
  `relabelled` / `wrong_call` (below), `duplicate`, `taken_over` (above),
  `retuned` (its tuner's
  centre moved > 1 kHz), `excursion` (the receiver left for a priority /
  held call outside the span), `returned` (still in progress when the
  receiver went home), `mode` (operating mode changed), `control_reset`
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
- Reports of the previous channel after a placement (what the voice VFO's
  decoder said before it was moved, still in the pipe). The engine resets a
  decoder at a retune > 100 Hz or another mode / plugin, and sends the
  manager a RESET then only if the decoder delivered reports since its last
  one (`DigitalDecoder::trunk_reported()`, or reports of it still in the
  queue). So at the placement it is known whether a RESET will come:
  - a placement that resets a decoder which has reported (a parked VFO moved
    to another channel): its CALL reports count only after that RESET (or 2 s
    if it never comes) - a late release of the old channel can't end the
    moved call;
  - a fresh VFO, or one whose decoder reported nothing: at once;
  - the same frequency and plugin again (a parked VFO back on the channel of
    its previous over - most overs of a conversation here): at once, but an
    ENDED / RELEASED only once the call was active on it since this
    placement (CALL ACTIVE, valid frames, the analogue squelch): a trailing
    release of the previous over can't end the new one (P25 and NXDN release
    only a call their instance reported, too).
  (2026-11-20: the first version waited for a RESET after every placement -
  on a real P25 replay 9 of 37 short overs lost their RELEASED, which came
  within 2 s of a placement that sends no RESET.)
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
  reset). With following on, where every control VFO is (RF, bandwidth,
  decoder) is noted each tick while not paused; 1 s after the scan stops,
  each one the scan moved is put back through the dispatching guard (not
  saved by itself) when its RF fits the span now - retried every 0.5 s for 30 s (e.g. until the receiver is tuned
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
  rest channel): with following on and not paused, the control VFO is moved
  there with `SET_DECIMATOR_FREQ` through the dispatching guard (the move is
  not saved by itself; above) when the frequency resolves and fits its
  tuner, at most every `CONTROL_MOVE_MIN_MS` 10 s, with a log line; otherwise
  the control entry's `note` says why (following is off / paused / no
  frequency / outside the span / waiting / in a playback).
- **A control VFO that names another system** (a keyed key changed): the old
  system's calls of that site end (`control_reset`).

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

## Control channels: the search, the memory, click to follow

**The search** (`TRUNK_SEARCH`) sweeps frequency ranges for control
channels. Coherent mode: the centre steps (the Wideband downconverter
through its `FREQ:` path, in its RF range). Independent mode (also generic
RTL-SDRs, one dongle too): ONE tuner steps (`TUNER_FREQ`, no calibration,
no hold) - the tuner of the user's control VFO if there is one, else tuner
0; the probes go on it and it goes back to its frequency at the end; the
other tuners keep what they do. Wideband scan mode: refused ("switch to
coherent or independent first" - its tuners are spread around one centre,
a step would move all five and the probes' placement across the panes is
not worth it for a search). The centre steps by the
usable band (edge clip x 2.4 MHz) less a 50 kHz overlap; heimdall's
calibration is HELD for the whole sweep (lease 3 s renewed every second, no
noise-source bursts) and released at the end (the calibration at home at
once). Every wait in a step is counted in DATA, not wall time (a loaded
receiver - a Pi 4 - averages late): the spectrum frames are counted per
channel at the RF they were taken at (`fft_frame_hz` / `fft_frames_here`);
at the first frame at the new centre the average is reset (frames of the old
centre still queued can't land in it), the first look comes once enough
frames there have refilled it (95 %, `srch_refill_frames`), the second look
half that many frames later. A CANDIDATE: the spectrum smoothed over ~1 kHz,
every bin >= `threshold_db` above the LOCAL background (the 20th percentile
within +-50 kHz - on air the band's background sat 11-14 dB above the
receiver's noise floor), strongest first: its width and centre from the run
within 10 dB of its own peak (a narrow spike on top: the -20 dB run's), the
run within 20 dB taken out for the weaker peaks (a peak in a 15 dB stronger
one's skirt within 20 kHz is no carrier); 4-32 kHz wide (wider: broadcast /
LTE / DAB - not tried), in both looks (control channels are continuous).
Each is probed once per run - a carrier probed already (half a raster) or in
a band an earlier step looked at (the overlap; the last step's clamp) is
skipped. PROBING: a temporary probe VFO per (candidate, trunking plugin) -
every built plugin that declares `trunking`, a remembered control channel's
plugin first - at most half the cores the process may run on (its affinity:
2 on a Pi 4 or a receiver pinned to 2 cores; 2-12) at once and never past
the receiver's 16 VFOs; placed on the raster when the measurement is within
a quarter raster of it, else at the measurement. A probe ends when its
plugin reports `SYSTEM` with `control = true` (and its key, waited up to 2.5
s when the flag came first), after 2.5 s of stream without a valid frame,
or after 5 s of stream (a wall-clock 30 s guards a dead stream); a carrier
whose plugin decoded valid frames but no control is a traffic channel of
that protocol: no other plugin tries it; the other plugins of a candidate
found are not tried; two probes within 15 kHz confirming the same system
are one control channel - the stronger carrier stands (the weaker one heard
it off its centre). Probe VFOs are
the search's (`decimator_info` `trunk.role` "probe"; never saved, never
heard, removed after each batch - their plugin processes with them); while
it runs no other VFO's report counts (the user's control VFOs are parked).
The nominal frequency: the signalling's own control channel (a CONTROL /
CHANNEL report within 15 kHz of the carrier), else the measured carrier
centre snapped to `raster_hz` (6250 default; 5000, 12500, 0 = none - the
measured value is kept beside it: this receiver reads ~ +1.35 ppm, 0.67 kHz
at 499 MHz, well inside half a raster).

It is a fourth scanner: refused while another scanner or a playback runs
(and they are refused while it runs: "a control-channel search runs - stop it
first"); trunk following stands down as for a scanner (an excursion home
first, voice tuners back, calls freed, paused why "control-channel search").
Nothing of it is saved (the dispatching guard). It ends: `done`, `stopped`
(`TRUNK_SEARCH_STOP`), `follow` (a click to follow), `mode` (an
`OPERATING_MODE:` / `WIDEBAND_MODE:` - home first), `shutdown` (home first),
`no_vfo` (no free VFO for its probes for 10 s - home first),
`heimdall` (the stream never came), `playback`; then the receiver is home
(the centre it had; its VFOs carried back with it, the control VFOs enabled
again) and following resumes. `user_retuned`: the user's own `FREQ:` ends it
WITHOUT going back. A restart mid-sweep comes up at home (home is what is
saved). One gain for the whole sweep: a front end that clips at some
frequency hides control channels there - the page should say so (the
search does not change the gain).

**The memory** (`trunk_control_channels.json`, cwd-relative, written through
a temp file 5 s after a change and at shutdown, never during a playback;
loaded at start; a file that doesn't read is kept as
`trunk_control_channels.json.bad-<YYYYmmdd-HHMMSS>[-n]` and `trunk_cc.file`
says so). At most 1000 entries: a new one makes room by dropping the entry
the air brought longest ago that the user never named nor followed; full of
the user's: not added. An entry is fed by the search, by any control VFO in
normal use reporting `control = true` (`src` "heard", at most once a minute
per VFO, at its RF snapped to 6.25 kHz) and by the page (`TRUNK_CC_SET`).
The same plugin within half a raster (at least 3 kHz) is the same entry:
updated (last seen, how often, level, options, gain); a different system key
there replaces the system fields (the user's name stays). Entries are never
dropped because a channel is gone - `last` says when it was seen.

**Click to follow** (`TRUNK_CC_FOLLOW`): ends a running search; if the
channel is not in the usable band now, retunes so that it is - THE USER'S
tuning (saved, a normal `FREQ:` / in independent mode `TUNER_FREQ` of the
control tuner): the system's suggested centre when that holds the channel,
else the channel 250 kHz off the centre; puts a control VFO on it with the
remembered plugin, its options (`DIGITAL_OPT`) and the bandwidth the plugin
needs (12.5-50 kHz) - a VFO of the user's (saved); switches following on. A
second click REPLACES the VFO the previous click made (the user's own VFOs
are never taken); `"add":true` adds another one instead, refused when the
new channel doesn't fit the span with them. Refused during a playback, in
wideband scan mode, while a scanner runs. The entry's `followed` is set.

**After the review (2026-11-22), the acceptance on the KrakenSDR**: 496-502
MHz at 33.8 dB, coherent, the receiver and its plugins pinned to 2 cores
(`taskset -c 0,1` - about a Pi 4), six sweeps - three with a P25 control VFO
on 498.7625 and following on beforehand, three with following off: all six
surveyed control channels found in every sweep (before the fixes: 4 of 6 -
498.9375 never a candidate - in the orchestrator's runs and mine). The
candidates of the step at 498.805 MHz (13 each time) held 498.7625 /
498.9375 / 499.1875 / 499.3250 every time, the step at 500.67 MHz (5-6)
499.7625 / 500.5875 every time, each within 1.2 kHz of the nominal (the
receiver's +1.35 ppm). Time: 60-66 s for the first sweep (16-17 new entries,
72-75 probes, 2 at once), 16-25 s for the next (remembered plugin first,
traffic channels left after one plugin). Found besides the surveyed six: 11
more P25 control channels of the same two systems (their keys; other site
numbers where reported) - not checked one by one; no entry from a carrier
the decoders didn't claim. The cause of the misses (finding 1 of the
review): the threshold was measured over the receiver's global noise floor,
and the band's background around 499 MHz sits 11-14 dB above it - a weak
control channel (26 dB over the floor, 13 dB over the background) had its
-20 dB run reach into the neighbours' skirts and was dropped as "wide"; the
wall-clock waits were the second problem on a loaded receiver (frames from
before the retune). Measured in the real capture of 499.8 MHz replayed (2
cores): its three control channels candidates in each of three sweeps.

**Measured on the KrakenSDR** (2026-11-22, coherent mode, threshold 9 dB,
raster 6.25 kHz, one gain per range, the five trunking plugins built, at most
8 probes at once on this 16-core machine; heimdall's own status checked
before / after - only ours):

| range | gain | steps | carriers | probes | found | time | per MHz | CPU (kraken_doa + plugins) |
|---|---|---|---|---|---|---|---|---|
| 496-502 MHz | 33.8 dB | 4 | 55 | 272 | 13 P25 | 56.8 s | 9.5 s | 79 % mean, 97 % max of a core |
| 453-460 MHz | 40.2 dB | 4 | 5 | 21 | 4 DMR | 10.1 s | 1.4 s | 104 % |
| 410-420 MHz | 29.7 dB | 6 | 9 | 40 | 5 DMR + 2 MPT1327 | 9.5 s | 1.0 s | 72 % |
| 138-146 MHz | 49.6 dB | 5 | 26 | 129 | 7 P25 | 32.1 s | 4.0 s | 68 % mean, 76 % max |

Against the survey of 2026-11-20: every listed control channel of P25 UHF
(6 of 6), P25 VHF (7 of 7) and DMR Tier III (4 of 4) found at its nominal
frequency, the measured centre within 2.4 kHz of it; in 410-420 MHz the Tait
DMR 414.7750 / 418.5125 and the other network's 418.2750 and MPT1327
416.5875 / 416.6750 found, Tait 415.6375 and MPT1327 416.7500 / 417.7625 not
(not on air during the sweep or under the threshold - not checked further);
not in the survey but reported as control channels by the decoders (their
control signalling, the system keys of the surveyed systems - probably
further sites heard here; not checked one by one): 7 P25 in 496-502 MHz,
DMR 414.3250 and 415.7125. No entry from a carrier the decoders didn't
claim. Click to follow, once per plugin (P25 499.3250, DMR 456.50625,
MPT1327 416.5875): the receiver retuned (saved) and the new control VFO
received its system within 25 s each.
A busy UHF step (14 carriers, 4-5 plugins each) takes ~14 s - over the
~10 s target: one VFO per (candidate, plugin) starts 60-70 plugin processes
per step in batches of 8. An engine probe set (one VFO running the plugin
list, delivering every runner's trunk reports) would cut the VFO setup and
the decimators to one per candidate, roughly halving it; not built. On a
Pi 4 (4 cores: 2 probes at once) expect a busy step at about four times
this. The DMR keys of the two DMR networks around 415 / 456 MHz came out
the same (`dmr:` + its key without a Network tag option): the memory keeps
them apart by frequency, but a system shared by key merges them in the
systems list - follow such an entry with its Network tag set.

**Rules from the third review (2026-11-22):**
- Probe VFOs are the manager's like voice VFOs: never in a saved VFO
  snapshot (a user's edit during the probing saves his VFOs only), not
  control VFOs, not the saved-frequency scan's tuners.
- A start with no free VFO is refused ("no free VFO for the probes (16 in
  use)"); a sweep that has no room for its probes for 10 s (the user added
  VFOs) ends `no_vfo` (home, hold released).
- heimdall lost mid-sweep (`heimdall`) and a mode change heimdall reports
  without a command (`mode`) end it with the home state ASKED for: the
  receiver is home when the stream is back.
- Step centres stay inside the tuner's range (the downconverter: its RF
  range); the band covers the edge from inside. The search's own retune
  that the receiver refused is never "the user retuned" (the step is
  compared with what the receiver took).
- Step centres sit half a raster off the raster: a channel on the raster is
  never at a centre's DC (within 3 bins of it a carrier can't be measured).
  With `raster_hz` 0 a carrier within ~0.44 kHz of a step centre is not
  seen - documented, not worked around.
- `user_retuned`: nothing goes back - the control VFOs keep their offsets as
  on any retune (no scan restore).
- A probe whose plugin set the control flag is always recorded, with or
  without its key; the key is waited for 2.5 s of stream time.
- With several FFT workers a frame of the previous step may still be
  averaged in just after the reset (frames are counted by their RF, the
  average isn't filtered): one frame among the refill count - the two looks
  and the threshold over the local background make it harmless.
- A duplicate erased as "the weaker sighting" is never one the user named
  or followed.
- Click to follow checks every refusal (not built / doesn't declare
  `trunking`, wideband scan mode, outside the tuner range, an add that
  doesn't fit, 16 VFOs in use) before it changes anything; it places the
  control VFO against where the receiver WILL be (a running search or an
  excursion goes home first, voice tuners back; never the stream's stale
  header); a replaced channel's plugin options go back to the defaults (only
  the entry's are set); options the receiver refuses are named in the
  answer ("followed on D<n>, but not applied: ...").
- Only decoders that declare `trunking`: refused at `TRUNK_CC_SET` ("plugin:
  a decoder that declares trunking"), dropped from a loaded file (counted in
  `file.note`), refused at follow ("its decoder (<id>) does not declare
  trunking"). Options: what `DIGITAL_OPT` takes (its key rule, <= 100 bytes,
  no control characters) - checked at SET / load by the same rule.
- `cc_followed`: a clicked VFO that is gone, runs another plugin or was
  moved more than 3 kHz off its entry is dropped; after a restart a control
  VFO with the plugin and frequency of an entry followed before is the
  followed one again (the most recently followed entry) - the highlight and
  "replace" work across restarts.
- The file: its size is checked before it is read; a newer `version` is kept
  aside untouched (`file.state` "not_loaded", the note says "newer
  version"); ids are renumbered 1.. on load; entries over 1000 are kept aside
  (`trunk_control_channels.json.full-<ts>`, the note says so) before a write
  could lose them.
- Messages: `TRUNK_CC_SET` and `GET_TRUNK_CC` are answered with the list
  message (to every page, at most once a second), not the full status (an
  import of 1000 entries was 1004 statuses); there is no reply-to-the-asker
  path in the command handler, so "the asker only" is not done - the rate
  limit is global. A heard control VFO's once-a-minute refresh that changes
  only `last` / `n` sends the list and saves the file at most every 5 min.

Commands (all `TRUNK_` / `GET_TRUNK_CC`, answered with the status, errors as
`trunk_error`):
- `TRUNK_SEARCH:{"ranges":[[lo_hz,hi_hz],...] (1-16, inside the tuner's
  range, <= 2000 MHz in all), "threshold_db":9 (3-40), "raster_hz":6250
  (0 | 5000 | 6250 | 12500)}`; `TRUNK_SEARCH_STOP`.
- `GET_TRUNK_CC` -> the list message; `TRUNK_CC_NAME:{"id":n,"name":".."}`,
  `TRUNK_CC_DEL:{"id":n}`, `TRUNK_CC_CLEAR`, `TRUNK_CC_SET:{entry}` (the
  fields below; with an existing `id` it replaces that entry, else it adds /
  updates the one at that plugin + frequency - the import path: one per
  entry; times clamped to 0 or 2020-01-01 .. now, `n` <= 1e9, `level_db`
  0-120, `gain_db` -1-60, `meas_hz` within 50 kHz of `hz`, `src` one of the
  three else "user" - the same when the file is loaded),
  `TRUNK_CC_FOLLOW:{"id":n,"add":false}` (the entry already followed on its
  VFO - same plugin, same frequency: a no-op; with `add`: refused "already
  followed on D<n>").

Messages:
- `{"trunk_cc":{file:{ok, state ("ok" | "not_loaded" - the file at start
  could not be read, kept as `kept_as`, the memory started empty |
  "not_written" - a save failed, the entries are kept in memory and saved
  again in 5 s), note, kept_as}, max:1000, entries:[{id, hz
  (nominal), meas_hz (null = none), plugin, sys ("<plugin>:<key>", "" =
  no key heard), sys_name, site, name (the user's), src ("search" | "heard" |
  "user"), level_db (above the floor when last found, null), gain_db (the
  receiver's gain then), first, last, followed (epoch ms, 0 = never), n,
  opts: {<key>: <value>} (plugin options without the prefix)}]}}` - on
  change (at most once a second) and on `GET_TRUNK_CC`; not in the status.
  The file holds `{"version":1,"entries":[<the same objects>]}`.
- In the status: `search`: null, or `{since, ranges, mode ("coherent" |
  "independent"), tuner (the stepping tuner, -1 = coherent), threshold_db,
  raster_hz, home_hz, centre_hz, step, steps, pct, eta_s, phase ("tuning" |
  "settling" | "looking" | "probing"), carriers (candidates this run),
  probing (probe VFOs now), probed (probes so far), found_new, found_known,
  elapsed_ms}`; `search_last`: null or `{end, carriers, probed, found_new,
  found_known, steps, steps_done, dur_ms, t_end}`; `cc_followed`: `[{vfo,
  id}]` - the control VFOs clicks made and the entries they follow.

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
| `TRUNK_RETUNE:0\|1` | coherent mode: follow a call outside the span by an excursion (retune, calibrate, follow, return) | 0 |
| `TRUNK_RETUNE_CAL:0\|1` | ... calibrate on arrival (1); 0 = the calibration held for the whole excursion (no gap in the call, no DoA there) | 1 |
| `TRUNK_RETUNE_MAX:<s>` | ... the longest time away, 10-3600 s, 0 = no limit | 120 |
| `TRUNK_TUNERS:<list>` | independent mode: the tuners the manager may retune for voice channels, e.g. `1,2,3,4` (0-7; empty = none) | (empty) |

Not saved:
- `TRUNK_END:<call id>` - stop following that call now; not rejoined.
- `TRUNK_HOLD:{"sys":<id>,"tg":<talkgroup>}` - only that talkgroup is
  followed (others: `hold`) until `TRUNK_HOLD:` (empty) releases it - or
  its system is deleted (a held system is never evicted) or the talkgroup is deleted
  (`TRUNK_TG_DEL`): the hold is released then too. A temporary system's
  talkgroup can't be held (no key yet).
- `TRUNK_RETURN` - an excursion: back home now; its call is stopped (why
  `stopped`, not rejoined). Refused ("the receiver is home") when not away.
- `GET_TRUNK` - the status; `GET_TRUNK_SYS:<system id>` - its lists message.

Systems (JSON in the saved-frequency store's style; strings are cleaned:
control characters dropped, cut at a character boundary; saved to the
systems file at once). `sys` is an existing id - or, for the four `_SET`
commands, a well-formed `<plugin id>:<key>` (`[a-z0-9_-]{1,32}` :
`[A-Za-z0-9._-]{1,48}`; the plugin needn't be installed) that doesn't exist
yet: the system's record is made (`heard` false until its control channel is
heard; a playback's: in memory only) - only when the whole command is
valid: a refused command leaves nothing behind. A temporary id (`:@`) is
refused by every edit but `TRUNK_SYS_DEL`. `TRUNK_END` takes an integer id
1 .. 2^53. A catalogue entry's `commands` can
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

- `systems_file`: `{ok, note ("" or what went wrong: not loaded - unreadable
  / larger than 32 MB; not written - over 32 MB), kept_as (the dated name an
  unreadable file was moved to)}`; `sys_refused` (new air systems not made:
  the table full of the user's).
- `follow`, `all_sites`, `max_vfos`, `encrypted`, `unit`, `audio`, `hang_s`,
  `retune`, `retune_cal`, `retune_max_s`: the settings.
- `excursion`: null at home, else `{state ("leaving" - retuned, the stream
  not there yet | "away" | "returning" - home again, waiting for the
  calibration and the control channel, at most 20 s), since (epoch ms it
  left), away_ms, call (its id), sys, target, ch, hz (its channel), centre_hz
  (where it went), home_hz, how ("control" - a control VFO of the system is
  in the away span | "offset" - the channel 250 kHz from the centre),
  calibration ("running" | "done" | "held" - TRUNK_RETUNE_CAL:0 while away |
  "manual" - KerberosSDR, no calibration requests), end (why it returned:
  set in "returning"), limit_ms (time left before TRUNK_RETUNE_MAX, -1 = no
  limit / not away), ctl: [{vfo, parked}] (every control VFO: parked = it
  is outside the away span, disabled until the return)}`.
- `tuners`: the `TRUNK_TUNERS` list; `voice_tuners`: one entry per listed
  (or still taken) tuner: `{tuner, control (a control VFO is on it: never
  retuned), taken (the manager has retuned it), hz (where it is now),
  home_hz / home_gain (where the manager found it; null while not taken),
  calls (followed calls in progress on it), tuning_for (the call it is
  being retuned for, 0 = none), tune_ms (the latest retune -> the stream
  there, -1 = none yet)}`.
- `excursions`: `{n, away_ms (total), ends: {<why>: n} - call_over |
  no_signal | limit | command | off | mode | scanner | playback |
  user_retuned | not_arrived, calls_away (calls that started while away),
  calls_missed (calls not followed because the receiver was away: why
  `away`), last: {arrive_ms (retune -> the stream at the new centre),
  cal_ms (arrival -> CONVERGED, noise off), first_frames_ms (left -> the
  call ACTIVE), back_arrive_ms, back_cal_ms, ctl_back_ms (return -> a
  control VFO reporting again); -1 = not seen}}`.
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
  last 5 s), parked (an excursion parked it: rx false meanwhile, not lost), site, note (a CONTROL that couldn't be done, "" = none),
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
  (epoch ms), `followed`, and `lost` (why a followed over lost its voice VFO
  before its end; only when it did).
- `learn`: `{learned, refused, dropped}`; `lat`: `{place_ms, place_max_ms,
  active_ms, active_max_ms, n}` (median / max over the last 50: a grant's
  arrival on the loop -> its VFO placed / -> ACTIVE; `place_*` counts only
  calls placed on the grant's arrival - one that waited for a free VFO, a
  pre-emption or `PLACE_MIN_MS` measures the wait and is left out; `active_*`
  is from the latest grant, so a call that waited includes it); `events`,
  `events_dropped` (queue full: every kind but RESET), `resets_coalesced`,
  `options_dropped` (grant options the traffic plugin doesn't declare),
  `untrunking_dropped` (events of plugins that don't declare `trunking`),
  `stale_reports` (voice-VFO CALL reports not taken: before the awaited
  RESET, or an ENDED / RELEASED before the call was active on a reused
  channel),
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
playback; a user's edit at once, new announced / learned entries within 30
s, counters every 10 min - with following OFF only an edit writes (the air
writes nothing). At shutdown `trunk::save_now` has the uWS loop write the
current state once (main waits up to 2 s; a loop that doesn't answer: its
last copy, at most 1 s old), and no write follows it - both writers take the
same lock. Not touched by `RESET_SETTINGS` (as the saved-frequency store).

What is written (what must survive a restart): the systems the user edited,
and - while following is on - the keyed ones heard; of each its names,
"listed only", rules, the talkgroups the user listed / named / gave a mode,
and the channels that have a frequency (user, announced, learned - with
their grant count and last time as the evidence). Not: talkgroups only
heard, channels never resolved, units, counters of the systems. A text over
`SYSTEMS_FILE_MAX` 32 MB is never written (the old file stays; the status
says so) - the user's entries alone can reach 32 x 4000 x ~180 B = 23 MB, so
what is written always reads back. A file that doesn't load (unreadable or
over 32 MB) is kept as `trunk_systems.json.bad-<YYYYmmdd-HHMMSS>[-n]` (never
overwritten) and the status' `systems_file` says `ok` false, why and where
it went; the manager starts empty.

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

Every container the air fills is bounded on every path that inserts (the
merge of a temporary system included - one insertion function per
container); when one is full the oldest tenth of what may go goes in one
pass (amortised O(log n) per insert, never a scan per event), and an entry
of the user's never goes. Queue 4096 events (a drain takes at most 2000, the
rest on the loop's next turn - commands and sockets run in between); 32
systems - when full a system nobody edited, no control VFO reports and no
hold names goes (a temporary one first, then the one heard longest ago); a
system the user edited is never evicted by the air: full of those, a new air
system is not made (`sys_refused`) - the user deletes systems themselves;
per system 2000 talkgroups (unlisted ones go, least recently heard first),
2000 channel entries (ones without a user frequency go, least recently
granted first), 32 rules, 1000 units (memory only), 200 unresolved names, 4096
data / skip grant sequences remembered (the map is cleared past it);
passive learning 512 observations, each kept 2 min; 64 calls in progress +
recent (the oldest recent one makes room, else the grant gets no row:
`calls_dropped`); 50 history entries; 8 sources per call; voice VFOs 1-8,
never past the receiver's 16 VFOs, each at most 50 kHz wide (twice TETRA's
25 kHz, the widest of the five; P25 / DMR / NXDN / MPT1327 12.5 kHz); the
systems file 32 MB.

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
Since the fix round (plugin 2.2): every grant also carries the hidden option
`traffic=1` - an instance with it never reports `control = true` and never
sends `control_channel` (Tier III or Capacity Plus); options are `slot`,
`traffic`, and `cc` only when the grant names a colour code. The control
rule is per slot: only TSCC PDUs on a slot the instance follows count;
C_SYS_Parms from the CACH counts only when no `slot` option is set; a
single-slot instance reports only its own slot's grants (the real network's
control carrier slot 2 carries an occasional C_ALOHA - 2 per 600 s - hence
`traffic` on top of the slot rule). A CACH short-LC fragment counts only once
the burst behind it passed its own check (slot-type Golay + CRC / FEC, with
the `cc` filter its colour code; voice bursts B-F by their EMB; a voice burst
A's only when no filter is set): a co-channel site's CACH cannot name the
system of a filtered instance. A multi-block (MBC) continuation must be the
next burst of its header's slot, 60 ms +- 3 symbols after the previous block.
Capacity Plus: `control_channel` only after 3 status CSBKs in a row name the
same new rest channel, at most once per 5 s (a move can take up to ~6 s on an
idle system).
Two unrelated Tier III networks can send the same network code - private
networks choose their own, and Tier III puts nothing else network-wide on
air. Here the 455 MHz network (plan 455.30625 MHz + LCN x 12.5 kHz) and a
Tait network at 414-418 MHz (plan 414.0000 MHz + LCN x 12.5 kHz) are both
`Huge.1`. The dmr plugin's option `net_tag` (Network tag, set by the user on
the control-channel VFOs of one network) makes the key `<model>.<NET>.<tag>`
(e.g. `dmr:Huge.1.tait`), so that network's channel table, learnt channels
and talkgroups are kept apart. Every grant passes `opt.net_tag`, and the
traffic instance reports the same key; Capacity Plus keys (`CapPlus.CC<n>`)
take the tag too. Grants and calls name a gateway / special address by its
alias (target `PSTNI` = a telephone call: unit, voice, followable; `ALLMSID`
etc. = all-call talkgroups). Grant options are now `slot`, `traffic=1`,
`net_tag` when set and `cc` with absolute parameters. A message-trunked
private call (PV_GRANT) can take about 1.4 s from grant to first voice.

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
generator and specification only, not seen on air. Since the fix round:
`trunk_system` every 5 s, at once after a reset, and ahead of any grant that
would otherwise precede it (the engine can drop a plugin's reports without
resetting the plugin - Auto detect locking elsewhere and back); in a
dual-format CAC, SITE_INFO / SRV_INFO are handled before a grant in the same
CAC. A DISC reports RELEASED only for a call this instance reported since its
reset (a voice VFO that arrives during the previous call's three DISC repeats
no longer ends the new call of the same talkgroup); an instance that arrives
mid-call and never decodes a VCALL reports nothing, also at the DISC - the
manager ends that call on its own no-frames timer. A SACCH message (VCALL ->
call state, grants) is assembled only from the four fragments of four
consecutive frames in order.

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
wanted call just stops being heard); after a loss of slot sync (9 slots
without a training sequence) the timeslot numbers are unknown and nothing
timeslot-dependent is reported (no grant from a traffic instance, no
`call()`, talker or voice) until the next BSCH or the carrier back on the
old slot grid within 10 s - a fade of more than 1 s ENDs the call, the
host's hang time then decides (plugin README *Loss of slot sync*);
air-interface encryption hides the allocations
(nothing can be followed; the plugin's "Trunk following" fact says so);
downlink bearings are the base station's; the control VFO still plays the
first traffic timeslot (listening to it too hears the call twice). Verified
on air: grants of a group call to timeslot 2 of the main carrier against the
traffic / talker change there (public recording: 8 of 8 allocations).
Release, other carriers, individual calls, fragment reassembly, extended
numbering: generated signals only.

**MPT1327** (plugin mpt1327, 3.2; analogue FM traffic, `demod = "nfm"`,
12.5 kHz). Every GTC on the control channel (bit 22 = 0) is a `grant`:
"CH n" (10-bit CHAN), target IDENT1, source IDENT2 as "PPP-IIII"; only
error-free codewords on a CCSC-confirmed slot grid. `update` = the same GTC
within 5 s; `data` = bit D (not followed); `emergency` = an ACKE / AHY with E
before the GTC. Group or unit: not in the GTC - from an AHY (POINT 0), an
ACKV QUAL 0 or an earlier call by that ident, else the plugin's `undecided`
option (default group; "unit" for networks of unit calls - the NZ one: 23 of
37 calls had the AHY, 9 more were shown to be units, none a group call; a
multi-site unit call reaches the caller's site without the AHY). Frequencies: only MPT 1343 Band III
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
stays). Talker: the MAINT's unit, "via site" on the downlink. Since 3.2 a
granted instance reports ACTIVE from the granted channel's carrier (0.2 s,
before any clear-down; the TSC gives the channel to one call from GTC to
CLEAR), and control-channel signalling on a traffic VFO counts only when
its Aloha CHAN4 is that channel's (intermodulation brought the site's
control channel onto 5 of its traffic channels on air). Verified on air
(NZ, 2026-11-20, 600 s at 06:30 local): GTC field positions and the plan
(37 calls on two sites; every granted channel keyed 0.12-0.58 s before its
GTC and its CLEARs name the site's control channel), the clear-down (23 of
23: 1-4 CLEARs, mostly 3 at 0.107 s + a 4th after 0.4-0.75 s, RELEASED at
the first), the downlink (carrier from before the GTC to the CLEAR; the
radios' MAINT almost never on it: 2 Pressel On in 24 calls - so no per-over
ENDED / talkers there), ACTIVE from the carrier in 24 of 24 calls, the
multi-site calls (5 granted on both sites within 0.15 s), real-speech false
codewords (4456 SYNT candidates in 1339 s of calls, 0 implausible
codewords, 0 false reports), the four sites as one network (`network=mpt1343`
-> "NET-0", one table). Not verified on air: Pressel Off / Periodic /
Disconnect on a downlink, an uplink, MOVE, BCAST announce / withdraw, data
calls, emergency calls, group calls (none was seen), the MPT 1343 Band III
plan. False reports: 0 over 1 h of noise and 1 h of FM speech.
Since the fix round (plugin 3.1): a replacement GTC on a traffic channel (a
GTC after SYNT) counts only when the same codeword is heard twice within 2 s
(one error-free codeword after an unconfirmed sync was 2^-18: 380-570 false
grants per VFO-year measured from 11,294 / 17,068 SYNT candidates per hour of
noise / FM audio; now < 1e-15); on a control channel a GTC must sit on the
confirmed CCSC slot grid (as before). A single CLEAR still releases (2^-38,
the standard's own acceptance check); a single MAINT still sets ACTIVE /
ENDED (about one false report per VFO-year on noise without the `chan`
option, 1e-3 with it; a false ACTIVE on noise ends after 1 s - no carrier). A
unit call's `call()` keeps the grant's target and source (who is speaking is
the talker); the traffic calls take the grant's emergency flag through the
hidden `emergency` option; `trunk_system()` goes out the moment a confirmed
CCSC is heard and always before a grant after a reset.

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
  hang time), CONTROL (the control VFO moved, the move itself not saved, a
  second within 10 s waits); a scanner's moved control VFO put back by the manager (scan_restore).
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
  line), the declared ones set. (The flood's waiting calls push rows
  through the 50-row history within a few hundred ms, so the check takes the
  `control_reset` row or the call gone with its voice VFO parked.)
- `t12_review` 17 (the review round, 2026-11-20): edits on a temporary
  system refused; three refused `_SET`s leave no system (status, file); five
  out-of-range `TRUNK_END` ids refused; a 2 MHz grant's VFO 50 kHz; a grant
  for a plugin without `trunking` `no_decoder`, that plugin's own reports
  dropped (a copy of trunktest that doesn't declare it); a control VFO naming
  another keyed system ends the old one's call `control_reset`; a systems file
  with 1e30 / -5 numbers reads them as defaults, its out-of-range rule
  dropped. 10 of the 17 fail on the build before the round. The reviewer's
  reproducers (copied): following off moves / saves / writes nothing (7), the
  user's system never evicted by 40 air systems (3), the last VFO handed back
  plain (4), shutdown saves the current state (1), a 60 s merge flood keeps
  the channel table at 2000 and the file at 348 B (was 150500 entries, 19
  MB), its file reloads (2), GET_TRUNK's worst answer through a flood of 400 /
  4000 / 40000 events per 100 ms block: 0.24 / 0.28 / 0.001 s (was 0.32 / 9.9
  / 12.6 s; at 40000 the old build dropped 98 % of 22 M events, the new one
  none of 6.5 M).
- `t13_place` 5 (2026-11-20, after the replay regression): one voice VFO,
  overs of 1.2 s ending in RELEASED - a fresh VFO and a same-frequency reuse
  end `released` (on the build that waited for a RESET after every
  placement: `ended`, 3.6-3.7 s - the RELEASED came 1.7 s after the
  placement and was dropped); a parked VFO moved to another channel for the
  same talkgroup's next over, its grant swept over -0.45 .. +0.3 s around the
  old channel's RELEASED (17 timings): every moved over ends with its own
  RELEASED; 1-3 of the old decoder's late reports per run were not taken
  (`stale_reports`). The window is a few ms wide and not hit on every run
  (on the build without any such guard the 17 timings passed too): the
  test shows the guard doesn't cost a release, not that each late report is
  caught. Real P25 replay (the 3-site UHF capture, 110 s): 51 overs
  followed, history 47 `released`, 1 `ended`, 0 `no_signal`, 0 stale
  reports (the round before, the orchestrator's 80 s replay: 28 of 37
  `released`).
- The fake heimdall (round 2) also models the calibration (`cal_model`:
  a retune -> 3 s cooldown, the lag driver's tick once a second, hold /
  release / backdating as heimdall, the noise source in the packets),
  KerberosSDR manual mode, generic RTL-SDRs (flag 0x1000) and a heimdall
  that doesn't retune (`freeze`).
- `t14_exc` 36: an excursion with a control VFO in the away span (hold ->
  retune -> release -> backdated -> calibration <= 1 s later; the control VFO
  keeps receiving; two overs in one excursion; back home with the same
  immediate calibration; the control channel regained; nothing saved), one
  without (the offset centre, the control VFO parked, no bogus system),
  `no_signal` then `retry`, the limit, `TRUNK_RETURN` (the call stopped),
  the option off, the user's retune (no return, his FREQ saved), `busy_here`
  and a priority call leaving an in-span one, a kraken_doa restart while
  away (home first at the shutdown, home after it), `TRUNK_RETUNE_CAL:0`
  (held while away, renewed, one calibration at once after the return),
  a scanner / a mode change while away (home first), KerberosSDR manual
  mode (no holds), `not_arrived` and the growing block.
- `t15_tuners` 12 (5 tuners) / 10 (two generic RTL-SDRs): in the control
  tuner's span no retune, a voice tuner retuned (the control tuner's gain,
  its home noted), four calls on four voice tuners and a fifth `no_tuner`,
  a call that fits a busy voice tuner's span, every voice tuner back to its
  frequency and gain when following goes off, nothing saved, back first
  before a mode change; the control tuner alone listed -> `out_of_span`.
- The review of round 2 (scratch `review2/`): `r1` 8 (a VFO snapshot while
  away holds the control VFO's home offset; a parked VFO's reused id left
  alone; the user's retune stands), `r2` 3 (the user's change of another
  tuner saves a voice tuner's home frequency and gain) - 6 / 8 and 2 / 3 on
  the build before the fixes.
- `t16_search` 30 (2026-11-22; the fake's carriers can now be band-limited
  noise of a given width): six control channels of two systems (10 kHz
  blocks + the system's pilot) over two ranges - one on a step seam, one 6.25
  kHz off the 12.5 kHz raster, one 12 dB over the floor, two 12.5 kHz apart -
  beside a tone, a 200 kHz block, a block without a pilot and an intermittent
  one: all six and only those, once each, nominal frequency, system and site
  right, 7.0 s for 5.6 MHz; the calibration held and released, home, nothing
  saved, the probe VFOs and their processes gone, following resumed, the file
  written; a second sweep updates (4 known, 0 new, no duplicate); stop, the
  user's retune (no return), a scanner and a playback refused while it runs,
  a shutdown mid-sweep (home first), the restart (home, no search), the file
  reloaded, a corrupt one kept aside; five bad commands refused, an import, a
  hostile name cleaned; click to follow out of the span (retuned and saved,
  the system followed), add, replace, an add that doesn't fit refused;
  independent mode: only tuner 0 steps, back at the end. One trunking test
  plugin kind (trunktest) - the two-protocol case is the hardware sweep's.
- `t17_idle` 6 (2026-11-22): an idle receiver (no control VFO, no call)
  answers GET_TRUNK and a TRUNK_ command that changes nothing, for two pages
  connecting one after the other, and GET_TRUNK_SYS after a restart (4 of
  the 6 failed before: from the first review round on, an unchanged status
  body was not sent again - a page that connected later to an idle receiver
  never got `{"trunk"}` and kept the box hidden).
- `t16_search` also pinned to two cores (`TK_PIN=0,1` - the receiver and its
  plugins on 2 cores): 30 / 30. `t18_search2` 8: with following OFF the
  status comes at least once a second while it searches and shows the
  probing phase; a heard entry carries its system and site; a memory file
  that can't be written is told (`file.state` "not_written"), warned once,
  written again when it can. `t19_ccimport` 6: a hostile imported entry
  bounded (times, `n`, levels, `meas_hz`, `src`), `file.state` "ok"; a
  repeated click to follow re-applies nothing, add of the followed one is
  refused; a sighting without a site keeps the site the entry has. `t17_idle` also: `DIGITAL_HISTORY` of a VFO that is gone sends no
  frame (an empty one used to reach every page).
- 308 checks, all passing on the final build (2026-11-20) but one
  load-sensitive check of t7 (the idle manager's CPU, 10.7 / 12.3 % and 18.3
  / 12.8 % of a core at load average 17-25 - it passed at load 3-7). Found on the way: a
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

MPT1327, the NZ capture of 06:30 local replayed as heimdall (two control
VFOs, 42E1 / 42C1, the rule 414.0000 MHz + n x 12.5 kHz for both systems,
the first 320 s): 23 grants, 12 followed as analogue FM on the granted
channels - all 12 received (0 `no_signal`), all 12 `released` by the traffic
channel's clear-down after 16-34 s; 10 outside the span (channels below
415.5 / above 417.5 MHz), 1 with no free voice VFO.

Live on the KrakenSDR, 2026-11-20 about 05:00 local - P25 (three control
VFOs on three sites of one system): 150 s, 22 grants, 13 calls followed, all
ended by the network's release, 9 cross-site duplicates, none missed, ACTIVE
a median 112 ms after the grant; DMR Tier III (three sites, the one-line
rule): 300 s, 50 voice grants, 32 followed (31 released by P_CLEAR), 14
duplicates, 92 data grants counted, 6 out of span, none missed; bearings of
the voice VFOs cluster per transmitting site.

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
- **The systems file not loaded** (`systems_file.ok` false): a dialog once
  per kept file in this browser (`localStorage` `kraken_tk_sysfile`) - the
  receiver's reason and the dated name it kept the file under, as a
  selectable line, and that the user's entries are in it, not lost - and a
  note at the top of the box for as long as the status says so.
- **Systems refused** (`sys_refused` > 0): "N systems heard but not added: 32
  systems of yours are kept" (what to do in its tooltip: Forget one). An
  edit refused for the same reason shows the receiver's `trunk_error` text
  plus "(a system's Edit, then Forget this system)".
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
  control VFO's `note` (any reason the receiver gives: not moved because
  following is off / paused, no frequency, outside the span, waiting, in a
  playback); DMR only: "N calls on channels over 10 MHz from its control
  channel" - calls whose frequency came from the table / rules (`fsrc` not
  `grant`) more than 10 MHz from every control VFO of the system (a site's
  traffic channels are within a few MHz of its control channel; the two
  networks with one code here are ~40 MHz apart) - pointing
  at the decoder's Network tag (two networks with one network code sharing
  one key and one plan). The editor's System pane has the same in a `?` next
  to a DMR system's key. Counts come from the receiver's `calls` (they stay
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
- Recent: `history`, newest first, made once each (keyed by call id, end
  time and system: a row the receiver re-keys when it folds a temporary
  system is made again, so its menu acts on the keyed system): time of the
  end, the label, marks, VFO, ⋯; duration and how it ended (`TK_END`: the
  receiver's end reasons in words - `taken_over` "Taken over by another
  site"; with `lost` the reason the over lost its voice VFO follows in
  brackets, "(lost its VFO: Nothing received)", the long form in the
  tooltip) or "Not followed: <why>".
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
quiet `uiAlert`. Every `TRUNK_*` command the box and the editor send goes
through `tkSend`, which records it for that match (the receiver refuses,
e.g., an edit or a hold on a temporary system - "this system has no key
yet" - which the page doesn't offer anyway, and a 33rd system).

**Time between statuses.** The status may come once a second, or less when
nothing changes. What depends on time is computed from the last status'
ages plus the time since it arrived (`tkStAt`), and a 1 s timer draws the
calls and the notes again when no status came for 0.9 s: an ended call's
6 s dimmed row, the notes' 20 s hold and the 15 s hover cap run on without
statuses.

**Following outside the span (an excursion) and voice tuners.** Shown
only by a receiver that sends `retune` / `tuners` (an older one: none of
it - checked against the first-pass build).
- Settings (the ⚙ fold), where they apply: coherent mode "Outside the
  span" (`TRUNK_RETUNE`) with, while it is on, "Calibrate there"
  (`TRUNK_RETUNE_CAL`) and "Longest away" (`TRUNK_RETUNE_MAX`, s, 0 = no
  limit), and the counters under them ("Away N times, m:ss in all · N calls
  followed away · N missed"; the ends by reason in the tooltip). Switching
  "Outside the span" on asks every time (`uiConfirm` naming the costs: the
  control channel not heard while away unless it fits, grants missed, the
  user's VFOs move with the centre, a calibration blanks reception for
  about half a second - the KrakenSDR's 0.32-0.49 s); off goes at once. Independent mode "Voice tuners" (`TRUNK_TUNERS`): a toggle
  per tuner (lit = the manager may retune it), a tuner with a control VFO
  "Tn Control" and disabled, "Others" = every tuner without a control VFO;
  handing tuners over when the list was empty asks (the user's VFOs on them
  move, they go back when following stops); changes after that and giving
  tuners back don't.
- Away (`excursion`): a block at the top of the box (amber) - "Away for
  <call>" (or "Leaving for"), its frequency, the time away and "back within
  m:ss" while `TRUNK_RETUNE_MAX` runs (both counting on between statuses),
  the centre with the user's own frequency ("yours: 851.000"), the
  calibration (calibrating / calibrated / calibration held: no bearing /
  not calibrated (KerberosSDR)), which control VFOs are kept or parked, and
  **Return now** (`TRUNK_RETURN`, no question: the call is not followed
  further). The same in the top bar: a banner beside the frequency ("AWAY
  FOR A CALL", the call, its frequency, "yours ..." and the time, Return),
  in the playback banner's place and look but amber, and the frequency
  digits outlined amber - the centre they show is the manager's, not the
  user's. "returning": "Back on <home> MHz: waiting for the control
  channel" and why it came back; then for 10 s "Back home (<why>) ·
  control channel regained after N s" (`excursions.last.ctl_back_ms`). A
  parked control VFO (the control entry's own `parked`, else the
  excursion's `ctl` list): its card "Control · ... · parked while away"
  (dimmed) and the system row's chip "Dn parked" (its frequency there means
  nothing) - never "not receiving".
- Voice tuners (`voice_tuners`): a block at the top of the box, a row per
  voice tuner - what it carries ("TG 9602", "tuning for TG ...", "free
  (held for voice)" / "free", or "yours again (you tuned it)" for a listed
  tuner the manager had taken and the user retuned or changed the gain of -
  the page sees it as `taken` going false with the list unchanged), where
  it is and "yours <home>" while taken;
  the tuner's pane: a "Trunk: <call> · yours <home>" tag in its header and
  the frequency digits outlined amber.
- Reasons and ends: `leaving` "Receiver leaving", `away` "Receiver away",
  `busy_here` "Call here", `retry` "Retry later", `no_retune` "Can't go
  there" (its tooltip names every case: no centre in range, heimdall silent
  for 2 s, an excursion that didn't arrive and the 30 s - 8 min wait after
  it), `retuning` "Receiver retuning", `tuning` "Tuner retuning",
  `no_tuner` "No free tuner" (each with what to do in its tooltip); ends `excursion` "Receiver left for another
  call", `returned` "Receiver went back". Notes: "N calls outside the span"
  gets a ⚙ button to the option that follows them (coherent, option off;
  independent, no listed tuner that can carry voice - none, or only
  control tuners), next to Centre; "N calls waiting for a free
  voice tuner" with a ⚙ to Voice tuners.

**Control channels: the search, the memory, click to follow.** At the
bottom of the box (under Recent: the calls don't move when it grows),
`#tk-cc-sec`, shown only by a receiver whose status has `cc_followed`
(the page then sends `GET_TRUNK_CC` once per connection - `tkCcAsked` is
reset when the socket opens, before the first status; `trunk_cc` comes to
every page on each change). The sub-heading says "saved automatically"
(its tooltip: the file on the receiver, saved by itself, loaded at start,
Export for a copy) - or, while a save fails (`trunk_cc.file.state`
"not_written"), "not saved" in amber with what that means - and the count.
- **Search bands…** folds out the form: band buttons (several at once:
  "Around here" ±10 MHz around the receiver's frequency - in independent
  mode around the tuner the receiver will step (the lowest tuner with a
  control VFO as the receiver counts them - a VFO of the user's running a
  decoder that declares trunking, heard or not -, else T0; its tip names it), VHF 136-174, UHF
  380-470, UHF 450-520, 700 MHz 763-776, 800 MHz 851-870, 900 MHz 935-941,
  "Whole range" = the tuner's range from `FREQ_MIN`/`FREQ_MAX`; frequency
  ranges only, nothing regional; made once, then only their state, small
  line and tip change - a button made again would lose the keyboard focus),
  typed ranges ("451-458, 935.5-940" MHz; a decimal comma is taken when that
  is the only reading that works - "451,5-458" - else the error says to
  write a point; a range under 10 kHz: "Too narrow"),
  an Advanced fold (threshold 3-40 dB over the local background around a
  carrier - it only decides whether a carrier is tried; raster 6.25 / 12.5
  / 5 kHz / none): shown at the receiver's defaults (6 dB, 6.25 kHz) and
  sent only when the user changed them (`tkSrchSel.thr` / `ras` null = the
  receiver's default; a selection saved by an earlier page, which always
  sent 9 dB, keeps its bands and ranges only). Ranges are merged, clipped to the tuner's range, at most 16 and
  2000 MHz (said in the form, Start disabled otherwise). The estimate under
  them, one line: the ranges, the steps (the receiver's own stepping:
  usable band from the edge clip, 50 kHz overlap), the time as a span -
  `TK_SRCH_S_PER_MHZ` 1.5 s per MHz on a quiet band to 6 s on a busy one,
  what a user sees on a PC (the receiver's first sweeps at 6 dB on the
  KrakenSDR: 496-502 MHz 31 s, 453-460 23 s, 410-420 15 s, 138-146 49 s; a
  band already in the memory goes faster; the tip says a Raspberry Pi
  takes several times longer; the low end at least 1.5 s a step) - amber
  when its slow
  end is over 5 min, and "away
  from <home>"; its `?` tip has the cost in words (the receiver leaves
  <home> MHz, the user's VFOs move, following pauses, it comes back at the
  end or on Stop, tuning ends it there, what it finds is kept as it goes).
  Start sends `TRUNK_SEARCH` at once when the busy end is 5 min or less,
  else (and always for Whole range) after a `uiConfirm` with the MHz, the
  steps and the span. While it runs the receiver's `eta_s` is shown. The selection
  is remembered in this browser (`kraken_tk_srch`). The receiver's refusal
  shows in the form ("Not started: ..." + what to do: switch the mode, the
  scanner's Stop, stop the playback). The mode is not checked by the page
  (the receiver decides: independent mode may search too).
- **While it runs** (`search`): the form folds away; an amber block where
  the form was (`#tk-srch`, made once in the markup: only its texts, bar,
  icon and which button shows change - a Stop made again under a press or
  the keyboard focus lost them) - "Searching", Stop, a progress bar, "<centre> MHz · step n
  of N · p % · about T left", the phase in words (tuning, waiting for the
  spectrum, looking for carriers, "trying n carriers with the decoders"),
  "range i of n (lo-hi)" with several ranges, "Found n new, m known · c
  carriers, p tries", "Back to <home> MHz when it ends". The top bar: the
  excursion's banner ("SEARCHING CONTROL CHANNELS", centre, %, time left,
  found, "yours <home>", its button Stop) and the frequency digits outlined
  - the frequency shown is the search's. In independent mode (one tuner
  steps: `search.tuner`) the block says "Tn steps", the estimate "one tuner
  (the one with a control VFO, else T0) leaves its frequency", and that
  tuner's pane gets the voice tuners' tag and outline ("Searching · yours
  <home>"). The box's state and badge say "Searching". **Probe VFOs**
  (`trunk.role` "probe") are dropped from decimator_info where it arrives
  (`tkProbeFilter`): no card, no bar, no DoA plot, no decoder history asked
  for, their decoder messages dropped (an id leaves the probe set when a VFO
  of the page's gets it) - only the count in the block. The list fills as the receiver
  sends `trunk_cc`; entries first found by this search are marked "new".
- **How it ended** (`search_last`, until closed or the next search): "Finished
  / Stopped by you / Ended to follow a control channel / Ended by a mode
  change / Ended: you retuned the receiver (it stays where you tuned it) /
  Ended: no free VFO for its probes - remove a VFO, then search again
  (`no_vfo`) / ... <time>" - a reason the page doesn't know as the receiver
  names it ("Ended (some reason)"), none as "Ended", "n new, m known control channels · steps done of N in T ·
  c carriers, p tries"; a finished search that found nothing adds one line
  of what to try (another band, a lower threshold, the gain), the reasons in
  its tooltip (one gain for the whole sweep, clipping hides channels).
- **The list** (`#tk-cc-list`, keyed rows in a 300 px scroller): the
  frequency, the name (the user's in bold, else the system's name, else its
  key, else "System not identified"), the level ("+18 dB", gain in the
  tooltip), a ⋯ menu; second line: "Following on Dn" / "Starting…" / why it
  can't be followed, "new", the decoder, the system name under a user's
  name, the site, "found / heard / seen 3 min ago". Sort (by frequency, last
  seen, strongest, by name; remembered, `kraken_tk_cc_sort`) and, from 7
  entries on, a filter row: text (MHz, names, site, key, decoder), decoder,
  seen within 1 h / 24 h / 7 days. **Rows don't move under the pointer:**
  while the pointer is on the list (or focus, a menu of it open, or 4 s
  after a tap) the order is kept - new entries go to the end, re-sorted
  when the pointer leaves - and a rAF watch scrolls the sidebar by as much
  as anything above the list (notes, a system appearing, a VFO card) moved
  it. Keyboard: Tab to a row, Enter / Space follow, arrows move, Shift+F10
  the menu.
- **One click follows** (`TRUNK_CC_FOLLOW`, no question; taken on the
  release of a press on the row - the press's row counts, so a redraw that
  moves the list between press and release can't lose it; a press that
  moved or a finger that scrolled is not a click): the row says
  "Starting…" at once and "Following on Dn" when `cc_followed` has it; a
  click on one followed already selects its VFO; a second click while one
  is on its way sends nothing (`tkCcPend`). A finger or a pen follows on
  `click` instead: the page's press-and-hold help eats the click after a
  hold, so a hold to read the row's tip never follows (in help mode the
  page eats the whole press). The held list's correction divides by the
  sidebar's zoom (`currentCSSZoom`, else `--sb-zoom`): a scroll of 1 moves
  the zoomed content by the zoom on the screen. The rows' key leaves the
  centre frequency out: at each step of a search only the tooltips of the
  rows that go in or out of the band change, and an open Details block is
  made again only when what it says changed (a selection in it stays). Said without sending (the
  row's second line and a note): outside the tuner's range, its decoder not
  on this receiver, a playback, wideband scan mode, a scanner running. The
  receiver's refusals come as a note under the search with what to do
  ("Follow it alone" when an added one doesn't fit the span; remove a VFO;
  build the plugin; a decoder that "does not declare trunking": tune a VFO
  to the channel and pick its decoder yourself; a search refused with "no
  free VFO for the probes": remove a VFO). A follow that is done but with an
  option the decoder refused (the receiver's `trunk_error` "followed on Dn,
  but not applied: ...") is no failed click: the row goes to "Following on
  Dn" with "... not applied" after it (`tkCcPartial`, while it is followed)
  and the note says "following, but ... not applied". Entry ids are the
  receiver's per start: on every new connection (`tkCcNewConn`) the clicks
  waiting, the open Details and menu, the held order, the latest note (its
  buttons name ids) and an import in flight ("broken off", the list shows
  what arrived) go, and the list is drawn again from the receiver's (what is
  followed too - a control VFO on a followed entry is followed again after
  a restart). The row's tooltip says when the receiver will be
  retuned (coherent: outside the usable band now) and that it replaces the
  last click's control channel. The menu: Follow, **Follow it too**
  (`add:true`; only when something is followed by a click), Name… (`TRUNK_CC_NAME`,
  empty = the system's name), Details (inline: frequency and measured,
  decoder and key, system, site, the user's name, source, level and gain,
  first / last seen, times seen, last followed, options), Forget
  (`TRUNK_CC_DEL`; asks only for a named one).
- **The ⋯ menu of the list**: Export… (the `trunk_cc` entries as
  `{"version":1,"entries":[...]}`, `trunk_control_channels.json`, made in
  the page - no receiver route), Import… (a file input: JSON of that form or
  a bare array, at most 8 MB / 2000 entries; each entry checked as the
  receiver checks it - frequency, plugin id, system key of that plugin,
  strings cut to 64, options as the receiver takes them (keys `[a-z0-9_]`,
  values up to 100 bytes, no control characters), a decoder this receiver
  has that doesn't declare trunking refused as the receiver would, times
  after now set to now, `n` up to 1e9, a level outside 0-120 dB dropped -
  and sent without its id as one `TRUNK_CC_SET` every 50 ms (an accepted
  one is answered only by the next list, at most once a second; the end is
  the first list more than 1.1 s after the last one sent, else 4 s). A
  refusal (`trunk_error`, at once) is matched to its command by the
  command's first 64 bytes (the receiver's echo), not through `tkSent` (the
  last 40 commands); a socket that is not open stops
  it ("Import broken off - the connection to the receiver was lost", n not
  sent); entries past the first 2000 are said apart ("n not imported (more
  than 2000 entries in the file)"), not as refused. A note counts
  "Importing: n of N sent" with Stop (the notes are keyed: the Stop stays
  the same element under a press and the focus), then "Imported <file>: a
  added, u updated, r refused (the reasons)" and old air entries dropped to
  make room), Clear all… (`TRUNK_CC_CLEAR`, asks: how many, how many named,
  export first). The memory file (`trunk_cc.file.state`; a receiver without
  it: `kept_as` set = not loaded): "not_loaded" - a dialog once per kept file
  in this browser (`kraken_tk_ccfile`) with the kept name as a selectable
  line (written by a newer version: said so), and a note with the
  receiver's own words while it lasts; "ok" with a `note` (entries of
  decoders without trunking dropped, those over 1000 kept aside in a file
  it names) - a note with the receiver's words, the list as loaded, the
  heading still "saved automatically"; "not_written" - "could not be saved -
  kept in the receiver's memory, tried again every 5 s" and the heading
  "not saved" until a save succeeds. Whatever an older receiver holds is
  shown safely: a time in the future "at an unknown time" (never "new",
  sorted as now), no "Invalid Date" (`tkCcDate`), a negative level "-12 dB"
  (`tkCcLv`).
- Notes there also say: a playback (following waits), wideband scan mode.
  The empty list says one line ("Nothing remembered yet - Search bands…
  finds them."), the rest in its tooltip.

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
- After the review round (2026-11-20, the receiver with `systems_file`,
  `sys_refused`, refusals on temporary systems, notes when following is
  off, events only from `trunking` plugins): the whole pass again plus
  `t_review` (16 checks: the systems file not loaded - a dialog naming
  `trunk_systems.json.bad-<date>` once per kept file in this browser, also
  not again after a reload, and a note; 3 systems refused - a note; the DMR
  Network tag hint for a call 41 MHz from its control channel; an ended
  call's row leaving after its 6 s with no status meanwhile; the real
  receiver refusing a bad rule and a 33rd system - "too many systems (32,
  all edited or in use): delete one first (a system's Edit, then Forget this
  system)" shown, 32 kept). All suites pass (states 9, fixtures 18, review
  16, actions 15, stability 9 - 60 s: 6 calls, 44 changes, 0 re-made, 0
  pushed down; pointed at: nothing moved -, scale 12 - median 2.1 ms per
  status with 64 calls + 50 history, lists 48-49 ms -, touch 8, the five
  scenes 10 each, misc 13, the DMR rule 9, the editor 26 and 29). Found and
  fixed on the way: a Recent row made while its call was in a temporary
  system kept that system's id after the receiver re-keyed it at the merge
  (its menu would have acted on the temporary id - refused); rows are keyed
  by the system too now. The NXDN scene's first call (TG 100) is followed
  now: the receiver resolves waiting calls again at the merge. One run of
  the desktop editor suite stalled (no output, killed after 15 min; the
  390 px run of the same steps passed in the same pass, and a rerun of the
  desktop one passed: not reproduced). The new page against the first-pass
  receiver: 11 checks.
- Following outside the span and voice tuners (2026-11-20, the receiver
  with `TRUNK_RETUNE` / `TRUNK_TUNERS`; the host worker's fake heimdall with
  its retune / calibration model and the scripted `trunktest` plugin;
  the TEST stack on 28779 / 28780-1 / 28791-2). `t_pexc` (31 checks at
  390 px, 30 at 1500): an out-of-span call with the option off - the note's
  ⚙ opens the settings at "Outside the span"; switching on asks (Cancel
  keeps it off, Switch on - a second page sees it); away - the box's block
  and the top-bar banner (the call, 853.6125 MHz, "yours 851.000", the time
  running, "back within 1:59"), calibrating then calibrated, the parked
  control VFO's card and its system chip, a hostile talkgroup name shown
  literally, the second page and a reload while away in the same state, the
  phone's top bar with the banner inside the window; Return now ->
  "waiting for the control channel" -> "Back home (Return) · control
  channel regained after 0.5 s" (gone after 10 s); the call's row "Stopped
  by you"; Longest away 15 s typed, Calibrate there off -> "calibration
  held: no bearing", "back within 0:15" counting, back by the limit and said
  so; the counters. `t_ptun` (14 checks at 1500 and 390 px): independent
  mode, "T0 Control" disabled, the note pointing at Voice tuners, T1 asks,
  Others doesn't, a call outside every span on T1 (the box: "T1 TG 9602
  853.3483 · yours 851.000", the pane's tag and outline), four more calls
  -> "1 call waiting for a free voice tuner", giving the tuners back (no
  pane marked). The first-pass receiver (rebuilt on these ports): none of
  it shown, 12 checks. The whole pass again on the merged build: states 9,
  fixtures 18, review 16, actions 15, stability 9 (60 s of the P25 capture:
  5 calls, 44 changes, 0 re-made, 0 pushed down; pointed at: nothing
  moved), scale 12 (median 2.0 ms), touch 8, misc 13, the DMR rule 9, the
  editor 27 and 29, the five scenes 10 each (their test signals generated
  again first, 21-27 s each: the cache had lost them); palette check OK; no
  console errors.
- Control channels (2026-11-22, the receiver with `TRUNK_SEARCH` /
  `trunk_cc`; the host worker's fake heimdall with a band of control
  channels - six 10 kHz blocks with the `trunktest` pilots of two systems,
  a tone, a 200 kHz block and a block without a pilot as decoys - on the
  TEST stack 28779 / 28780-1 / 28791-2). `t_cc` (55 checks at 1500 px, 55
  at 390): the form (a band ticked: steps, time and the cost; nothing
  ticked or a bad typed range: Start disabled, said), typed ranges
  "849-853, 855-856.6" -> the form folds, the block (step, %, time left,
  "trying 8 carriers with the decoders", found, home), the top-bar banner
  and outlined digits, a second page the same, no probe VFO card or bar
  while probes ran (13 looks), the list growing 1 -> 7 while it ran,
  "Finished: 6 new" with the six marked new, home at 851.000; the 800 MHz
  preset stopped from the banner ("Stopped by you", back home; at 390 px
  with the sidebar closed); Whole range asks first (Cancel: nothing
  started); the receiver's refusal in the form; a reload mid-search (block,
  banner, list back) and the block's Stop; click to follow in the span (the
  row under the pointer stays the same row; "Following on D1"), a second
  click replacing, Follow it too (two), out of the span (the receiver
  retuned to 855.75, followed), Follow it too where it doesn't fit (the
  refusal with "Follow it alone", which follows); a hostile name (markup,
  quotes, RTL override) literal on both pages; details; forgetting a named
  one asks; sort by level; export -> Clear all (asks) -> import: the same
  entries (ids aside); an import with 8 bad entries of 10 (no frequency,
  bad plugin id, another plugin's key, 40 options, a string, null, an
  array, 1 THz) and control characters / markup / a 500-character name:
  "2 added, 8 refused (the reasons)", the receiver's cleaning, a name cut to
  64; a file that isn't JSON: said, nothing sent. `t_cc_scale` (17 at
  both widths): 500 entries (five decoders, four bands, three weeks of
  ages) - listed in 67-76 ms from empty, 5-7 ms again, typing a filter
  12-18 ms a key, sorts 31-38 ms, decoder / age filters right; with the
  pointer on a row: a rename from page 2 that sorts first, an entry added
  and a note appearing above - the same row stays under the pointer;
  re-sorted when the pointer leaves; click to follow among 500; export ->
  clear -> import of 502 in 31-32 s ("502 added"), identical, page 2 in
  sync. `t_cc_more` (13 at both widths): an idle receiver (no control
  VFO) shows the box and the memory to a page that connects later (found
  on the way: the receiver answered `GET_TRUNK` only when the status had
  changed - fixed in the receiver); a memory file that doesn't read - the
  dialog once per kept file (not after a reload) and the note; independent
  mode: a search from the page (the stepping tuner's pane tagged
  "Searching · yours 851.000", "T0 steps"), finished, one click follows
  there; a touch tap follows and the list isn't held afterwards. Found and
  fixed on the way: the page asked `DIGITAL_HISTORY` for probe VFOs, which
  were gone by the answer - the receiver then broadcast an empty frame
  (a JSON parse error on every page; reported) - probes are filtered before
  the decoder code now; a click on a row was lost about one run in eight
  when the list moved between the test's pointer arriving and its press -
  the test moves the pointer onto the row first, as a hand does (the list
  is held from then on), and a row follows on the release of a press on it
  (then t_cc 9 runs in a row green at both widths);
  the import paced by the receiver's answers waited 1.5 s per entry (an
  unchanged status wasn't sent again): one every 50 ms now. The whole
  earlier pass again on the committed receiver (8039b88 merged): states 9,
  fixtures 18, review 16, actions 15, stability 9, scale 12, touch 8, the
  five scenes 10 each, misc 13, the DMR rule 9, the editor 27 and 29; the
  first-pass receiver 13 (now also: no control channel part, no
  `GET_TRUNK_CC` sent); palette check OK; no console errors.
- After the independent review of the control-channel page (2026-11-22,
  13 findings; the receiver at 88bfa58: `trunk_cc.file.state`, a repeated
  follow a no-op / "already followed", bounded numbers, no empty frames).
  `t_cc_rev` (49 checks at 1500 px, 46 at 390 - the zoom part is
  desktop-only): `GET_TRUNK_CC` once per connection and once again after a
  reconnect; the estimate one line (28 px) with the cost in its `?` tip;
  every preset but Whole range "4-36 min" and asked first, Cancel starts
  nothing, 849-853 under 5 min at its busy end; the Bands tip 1-10 s per
  MHz; "451-451.005" too narrow, "451,5-458" / "451,5-458, 460-462" /
  "451,5-458,5" with a decimal comma, "450-460,470-480" two ranges, "451 x
  458" says to write a point; a band button by Enter and Space keeps the
  focus; the search block's Stop keeps the focus and stays the same element
  through statuses, pressed 0.1 / 0.45 / 1 s it stops each time (sent
  once); a double follow sends one; "Follow it too" on the one followed
  shows the receiver's "already followed on D1"; at sidebar zoom 0.8 / 1 /
  1.5 a four-line note appearing above moves the row under the pointer by
  0.1-0.2 px; an older receiver's future time / negative level shows "at an
  unknown time" / "-12 dB", no "Invalid Date"; imported future times set to
  now, a negative level dropped; 2100 entries: "100 not imported (more than
  2000 ...)", not refused; the socket closed mid-import: "Import broken off
  - the connection ... lost", n not sent; 1000 named entries, then 60 more
  refused by the full memory - all 60 counted (past `tkSent`'s 40); the
  import's Stop keeps the focus and stops on presses of 0.1 / 0.45 / 1 s;
  a failed write ("not saved" in amber, "could not be saved") and the save
  succeeding again ("saved automatically", the note gone); the probe
  filter switched off during a search: no JSON parse error (the receiver
  sends no empty frame now); independent mode with the control VFO on T1 at
  460 MHz: Around here 450-470, the tip and the estimate name T1, the
  receiver steps T1; at 390 px with touch: holds of 0.75 and 1.2 s on a row
  in and out of the band (the tip shown) follow nothing, help mode follows
  nothing, a short tap follows once. `t_cc_scale` 19 at both widths (new: a
  search over 500 rows - 111 list mutations in 10 s, status handling median
  7.8 ms, max 22 ms, an open Details block and a text selection in it kept
  through the steps). Found on the way: the rows' key had `isNew` as 0 /
  false, so every row was made again when a search started and ended; the
  age is in the key as its words now (a row changes when its text does);
  a row drawn before the plugin list came kept the plugin id for its name.
  The whole pass again at both widths on the receiver at ffe1247: t_cc 55 /
  55, t_cc_rev 49 / 46, t_cc_scale 19 / 19 (its round trip compares the
  scene's live trunktest channels without their sightings - last, n, level:
  the receiver hears them between the export and the import), t_cc_more 13 / 13, states 9, fixtures 18, review
  16, actions 15, stability 9, scale 12, touch 8, the five scenes 10 each,
  misc 13, the DMR rule 9, the editor 27 and 29, the first-pass receiver
  13; palette check OK; no console errors.
