# wildlife - wildlife tracking transmitters (pulsed VHF / UHF collars / tags)

The classic direction-finding job, everywhere in the world: collars and
tags on deer, wolves, bears, kiwi, livestock, birds and bats send a short
CW pulse every second or two, and people walk with a Yagi to find them.
This plugin finds every such transmitter in the VFO, names it by its
frequency (+ code), reads its mode from its own behaviour, and gives each
one its own bearing on the 🗺 Map's 📡 DF panel (🐾 collar).

- **No country assumed** (profile `global`, the default): a collar is found
  anywhere in VHF / UHF, listed by its frequency in MHz (no channel
  numbers), its modes come from its OWN rate (below). Country / vendor
  specifics are selectable profiles that only ADD to that (`profiles.hpp`):
  channel numbers, a rate table, a mortality convention, a data-stream
  scheme, band names
- **Any VFO width** - the plugin takes the VFO's OWN rate
  (`kp::Info::sample_rate = 0`, new in the SDK with this plugin): a 2.4
  MHz VFO on the tuner's centre surveys a band at once, a narrow VFO (5 kHz
  and up) follows one collar. The same detector runs at every rate with
  ~200-300 Hz FFT bins. Manual only (not in Auto detect)
- **Band hints** (fact "Band", a collar's "Band" detail; never required -
  outside them a collar is found the same): a worldwide list of wildlife
  tracking allocations / common use (`band_hints()`, sources at the end):
  US FCC 90.248 40.66-40.70 and 216-220 MHz, North American agency collars
  148-152 / 159-161 / 164-168 MHz (also South Africa's 148-152 MHz wildlife
  telemetry), Germany 150.05-150.24 MHz, Finland 155.400-155.525 MHz (dog
  tracking collars), UK telemetry 173.2-173.35 and 173.7-174 MHz (bird
  tracking), Japan 142.93-142.99 / 146.93-146.99 MHz, Australia 48-49 MHz
  (aquatic animals), New Zealand 160.1-160.6 MHz (channels 0-47) /
  160.6-161.11 MHz (48-99), Motus 166.380 / 150.100 / 151.500 MHz. With a
  region profile its own bands are named first

## What it reads

- **Pulses** (`pulse.cpp`): FFT of N = 2^round(log2(rate / 250 Hz)) points
  (2.4 MHz: 8192 = 293 Hz bins; 48 kHz: 256 = 188 Hz; 10 kHz: 32), Hann, 50 %
  overlap; a noise floor per bin (gated mean, 1 s; under a steady carrier it
  creeps up 1 dB/s, so a birdie / CW interferer stops being a pulse within
  seconds). Two detectors per bin: one frame (> 19x the floor: a coded tag's
  few-ms pulses) and the mean of ~14 ms of frames (> 5x: a collar's 10-40 ms
  pulse, weak); a frame where a quarter of the band triggers at once is an
  impulse (ignition, power line) and blanked; a hit must be a narrow line.
  The finished pulse is measured on the raw samples: mixed to its bin,
  summed to ~32 kHz, a +-1 kHz Hamming FIR; the pulse = maximum-sum run of
  the 2 ms power over the channel's noise (known from the bin's floor),
  edges = maximum-sum run of the 0.3 ms power over the midpoint between
  noise and pulse; several pulses in one span (a coded burst) one after the
  other. Kept only if it looks keyed: 1-150 ms, down at the noise 1-3 ms
  outside its edges, a steady level inside (an FSK tone comes and goes with
  the bits) and one frequency in both halves (an FM line moves). Carrier
  from the phase slope (a few Hz at 40 dBHz), power, C/N0 (dBHz)
- **Channels / transmitters** (`decoder.cpp`): pulses within 700 Hz = one
  channel. A pulse 20 dB under an overlapping one within 6 kHz (10 dB within
  3 kHz) is its splatter; pulses 0.6-8.5 kHz apart at a similar level
  (10 dB) that start and end together, or come within 60 ms of each other,
  are one data burst's tones (dropped, counted - collars on neighbouring 10
  kHz channels are never such twins).
  Pulses closer than 160 ms form a group
  - **coded tag** (Lotek / Motus style): a group of exactly 4 pulses within
    0.4 s after >= 0.3 s quiet; tags by their 3 gaps (lead to lead, +-2 ms),
    confirmed after 3 bursts give the burst interval (the shortest interval
    every interval is a multiple of; +-30 ms + 5 ms per missed burst) - over
    the longest run of the latest bursts that agree: an older burst off that
    grid (a jump in the input - samples kraken_doa dropped don't advance
    Host::time() - or a false burst) is dropped. Two tags with the same gaps
    but another burst interval (Motus "aliasing") stay apart; a new
    candidate with a confirmed tag's gaps AND interval that wasn't heard
    since the candidate's first burst is that tag after a jump in the input
    (or back in range) and carries on under its own id (it used to come back
    as a second "...b" tag). Id `<MHz> <g1>/<g2>/<g3>@<BI>` ("160.367
    22.5/17.5/30.0@5.0")
  - **collar (pulse train)**: the period of the channel's single pulses
    (>= 4 ms wide) over 30 s - candidates from the pairwise intervals / 1..4,
    scored by the pulses on the grid minus half the empty slots (so P/2 and
    2P lose against P, missed pulses allowed) - confirmed with >= 4 pulses,
    rms jitter <= 2.5 ms + 0.1 % (a crystal keeps time), one pulse length
    (median absolute deviation <= 30 %) and a duty cycle < 10 %. Then each pulse is
    on its grid (within max(25 ms, 3.5 %)) or an extra (a data stream);
    fragments (8 dB weaker or < 40 % of its width) are dropped. A weak collar
    first found at twice its period halves it when two pulses come half way
    between. A rate change = a new period that fills its grid (>= 80 % of
    the slots, >= 6 pulses, >= 8 s) and beats the old one three times
    running. Id `<MHz>` to the kHz ("160.350"; offline without an RF:
    "+5.0k"); a second one on the same kHz gets "b"
- **Modes, generic** (every profile; `classify` in decoder.cpp) - from the
  collar's own behaviour, as no rate means the same everywhere:
  - **baseline**: the first steady rate it is heard at (shown in the
    details)
  - a sustained change (the rate-change rule above) to x1.4-3.1 of it =
    `faster xN`; held 2 minutes (`POSSIBLE_HOLD_S`) = **possible
    mortality** (⚠ event: most mortality sensors raise the rate after hours
    without movement - Kenward: it doubles after ~4 h still; Lotek: the
    rate rises, 4 h default; Telonics: the mortality time is set per
    collar). Slower = `slower xN`. Its baseline rate again = back to
    baseline ("moving again" after a mortality)
  - **activity**: >= 3 sustained changes within 30 min between rates it
    already had (the USFS collar: 40 resting / 60 active ppm) - not a
    mortality ("a mortality switch stays"); a NEW rate after such toggling
    is judged against the slowest of them (its resting rate: 40 -> 80 =
    x2)
  - a change of rate drops a data stream that was open: the new rate's
    pulses came closer than half the old period and read as "data"
- **Profiles** (option `profile`; `profiles.hpp`) - each adds to the above:

  | Profile | Channel scheme | Rate table / mortality convention | Data streams |
  |---|---|---|---|
  | `global` (default) | none (MHz) | the generic modes only | pulse groups shown |
  | `na` North America | none (vendors name collars by MHz) | a doubling (x1.8-2.25) over the previous / resting rate = **MORTALITY** at once (Kenward / Lotek; the USFS collar 40 / 60 / 80) | pulse groups shown |
  | `eu` Europe | none | doubling = MORTALITY (Kenward's convention) | pulse groups shown |
  | `au` Australia | none | doubling = MORTALITY | pulse groups shown |
  | `motus` Motus | "Motus 166.380 / 150.100 / 151.500" within 5 kHz | (coded tags have no modes) | - |
  | `nz` New Zealand (DOC / Wildtech) | NZ ch 0-99 = 160.120 + 0.010 n MHz (48-99 special licence; the carrier's offset from it) | 30 normal, 48 incubating, 60 / 80 MORTALITY (absolute; a collar first heard at 60: "⚠ MORTALITY?"), doubling / x2.4-2.9 = MORTALITY | Wildtech decoded |

  No public vendor rate tables were found for North America / Europe /
  Australia (ATS, Holohil, Sirtrack, Biotrack, Followit, Vectronic): those
  profiles use the documented doubling convention, not invented numbers.
  A saved `profile=change` (the old "rate changes only") loads as `global`.
  A profile change while running re-sends the columns and rows at once;
  the collars' modes follow at their next rate change
- **Data streams** (option `streams`; runs of close pulses = R, single
  pulses = N; `train.cpp`): `auto` = as the profile says (Wildtech with
  `nz`, else the pulse groups), `wildtech`, `raw` (pulse groups only), `off`.
  Without a vendor scheme a stream is shown as its **pulse groups** - each
  run's count, the single pulses between in brackets ("2 (3) 2 (5) 2 (11)
  2", "4 5 (5) 2 2 (5) ...") - nothing is guessed: the same pulses mean
  different things to different vendors, so the scheme is not auto-detected
  (Lotek's "Time Since Death", ATS / Sirtrack codes: no public format).
  **Wildtech scheme** (NZ kiwi / kakapo transmitters, every ~10 min):
  - time since death (DOC Manual 6.4.3): a double beep, (2 + digit) normal
    pulses, a double beep ... 3 digits = hours in mortality mode ("139 h in
    mortality mode (no movement for 163 h = 6 d 19 h)") - ⚠ event
  - Egg Timer / Chick Timer (Manual 6.4.4 fields; sent the way the Wildtech
    kakapo Egg Timer manual V6.3 describes it: per number a burst of (2 +
    tens) fast pulses, a 3 s gap, a burst of (2 + units), 5 normal pulses):
    8 numbers = the kiwi fields (days since incubation started / since
    hatching / since nest abandonment, hours since the last emergence,
    battery weeks, active 10-min blocks last night / 2 nights ago / 4-night
    mean), 11 = the kakapo Egg Timer's outputs; other counts = "numbers ..."
  - a stream heard again identically = "confirmed" (✓ in the Code column)
- **Table** Collar, Name, MHz (4 decimals), [Ch - only with a channel
  scheme: nz, motus], ppm, Pulse, Mode, Code, C/N0, Level, Pulses (the
  columns follow a profile change); details: carrier to the Hz, channel +
  offset, band, period + jitter, baseline, previous rate, mode (+ the rates
  an activity collar switches between), the profile's convention, drift
  (Hz/min), the data stream's fields, the coded tag's gaps / interval, the
  C/N0 of the last 20 pulses (walking DF).
  Kept `keep_h`. **Plot** "Pulse strength": C/N0 per pulse over the last 2
  minutes of the `focus` collar (else the one heard last), the detection
  limit dashed - the signal-strength aid for walking. Facts: collars, the
  strongest, pulses (+ dropped data lines), detector (bin width, the
  per-pulse limit ~31-33 dBHz, impulse frames), profile, band. Events: new collar /
  tag, mode / rate changes, data streams, lost (12 periods / 4 burst
  intervals, >= 30 s), heard again. Raw (decoder log): the streams' numbers

## ID separation

Each collar / tag is a transmitter: every pulse that belongs to it is a
`kp::Talker` packet - id = its id above, its exact span (edges +-0.1 ms
inside), `freq_hz` = the pulse's measured carrier, `bw_hz` = option
`chan_bw` (1 kHz), `avg_s` = `doa_avg` (10 s), `keep_s` = `keep_h` (12 h)
- so collars and tags in one VFO, also pulsing at the same moment on other
channels, each get their own lobe; coded tags on ONE frequency are told
apart by their codes. Label "name · ppm · mode" / "coded · BI". UI kind
`collar` 🐾 ("Collar (MHz) / Name · ppm · mode / Pulses", by name).

kraken_doa change with it: a packet channel far narrower than the VFO (<=
10 kHz where the 401-tap filter couldn't resolve it - a 1 kHz collar in a
2.4 MHz VFO) is mixed down and summed to ~16 x bw first, then filtered at
that rate (`narrow_cov`, talker_doa.cpp): the 401 taps at 2.4 MHz passed
~36 kHz, i.e. ~15 dB more noise and the neighbouring collars. The offline
host's `--uca` bearings use a CENTRED channel filter now (a causal one put
a 1 kHz channel's packets 2 ms early - before a short pulse).

The array is small at 160 MHz (r = 0.12 m = 0.064 wavelengths): a bearing
needs many pulses at low C/N0 - longer `doa_avg` when standing still.

## Options

`names` (`150.512=Elk 7, 160.734=Kaka 3` - MHz within 1.5 kHz, or an
exact id; shown in table, talker label, events), `profile` (global / na /
eu / au / motus / nz), `streams` (auto / wildtech / raw / off), `doa_avg`,
`keep_h`, `chan_bw`, `sens` (-1 / 0 / +1: thresholds 2 dB up / down),
`focus` (the plotted collar).

## Tests

Generator `tools/collar_gen.py` (`tx`: one transmitter for uca_scene.py;
`scene`: every antenna streamed in 0.05 s pieces at any rate incl. 2.4 MHz,
noise, impulses / CW / FSK bursts / FM interference): trains with jitter,
drift, a rate switch or a rate schedule (`rates`: activity toggling,
mortality and back), the Wildtech TSD / Chick Timer / Egg Timer streams,
coded bursts. Playback entries (`tools/testsignals.json`): `wildlife-five`
(50 kHz VFO, profile nz), `wildlife-five-survey` (the same in a 2.4 MHz
VFO, global), `wildlife-one-collar` (10 kHz VFO on one collar, global),
`wildlife-na-collars` (four collars at 150.985-151.015 MHz, profile na),
`wildlife-motus-tags` (four coded tags on 166.380 MHz, two on one carrier,
profile motus). Results (scratch t9/; measured before the region-neutral
rework, when NZ was the default - re-run with `--opt profile=nz` they are
identical, see *Region-neutral rework* below):

- **Five transmitters, 5 antennas** (48 kHz, 60 s; 160.340 / 160.345 /
  160.350 MHz collars at 40 / 34 / 38 dBHz, two coded tags sharing 160.367
  MHz at 45 dBHz): the collars confirmed after 4 pulses (5-7 s), the tags
  after 3 bursts, gaps to 0.1 ms, burst intervals exact; the 30 -> 60 ppm
  switch at 25 s -> "⚠ MORTALITY: rate 30 -> 60 ppm" at 30.1 s. Bearings
  (truth 40 / 170 / 130 / 300 / 260): **40.5 / 172.6 / 130.7 / 298.4 /
  259.8 deg** (the 34 dBHz collar and the 2.5 ms coded pulses are the least
  exact). One collar alone at 50 dBHz: 129.7, a tag at 55 dBHz: 260.6 - the
  small array (r / lambda 0.064) needs many pulses
- **Data streams** (240 s, three collars at 38 dBHz): time since death 139
  h (the repeat 100 s later 138 h: one pulse lost - before the 60 ms
  burst-piece rule both were 139 and "confirmed"), Chick Timer 23 0 0 14 40
  31 30 32, kakapo Egg Timer 63 12 40 41 39 45 38 44 50 42 27 - exact. (Every keyed-
  carrier check first cost a pulse here and there - one pulse = one wrong
  digit -, hence the edge test only above 9 dB in the channel)
- **Low SNR** (20 ms pulses): 36 / 35 / 34 dBHz collars found at their
  rates within 8 s, 32 dBHz after ~1 min, 30 / 28 not;
  a coded tag at 38 dBHz (2.5 ms) not (needs all 4 pulses of a burst)
- **False alarms**: 300 s of noise + impulses (8 / s) + a 55 dBHz carrier +
  two FSK data signals (2400 Bd 0.25 s bursts, 1200 Bd 60 ms bursts): no
  collar from them (data tones dropped as twins / unsteady level); a
  synthetic tone-FM signal with a strict 3.1 Hz modulation still gives one
  false collar (see limits). 10 s of 2.4 MHz noise: 6 single pulses, no
  collar
- **2.4 MHz band survey** (1 antenna, 20 s): transmitters at -800 / -235 /
  -15 / +125 / +905 kHz (34-46 dBHz) all found at their frequencies (NZ ch 0
  / 22 / 36 named), rates and modes; a 60 dBHz carrier, FSK bursts and
  impulses in the band: nothing
- **CPU** (Pi 5, other load): the detector alone 8-12 % of a core at 2.4
  MHz (measured with an FFTW_MEASURE plan; ESTIMATE from 2026-10-15 -
  MEASURE planned 23 s at the start for a 77 -> 59 us FFT, ~1 % of a core -
  and pffft since 2026-10-17, ~41 us for the 8192 points, no planning),
  0.2 % at 48 kHz, ~0 at 10
  kHz; the offline decode incl. the host's file conversion 15-24 % at 2.4
  MHz, 1.1 % at 48 kHz
- **Regression** (old vs new binaries after the SDK change): `--info` of
  all 25 plugins identical; the aprs playback scene decoded identically,
  beacon406's identically but one peak / mean ratio (660 -> 657, the
  centred offline filter)
- **End to end** through the LIVE kraken_doa's debug playback (settings
  sandboxed; `doa_settings.json` byte-identical after Stop):
  `wildlife-five-survey` (2.4 MHz VFO, the plugin at 2.4 MHz) 5 / 5 - 41.7 /
  165.1 / 131.0 / 300.9 / 260.1 deg (truth 40 / 170 / 130 / 300 / 260; the
  first run, before the last false-alarm rules: 39.1 / 166.8 / 129.1 / 302.0
  / 260.7); `wildlife-one-collar` (10 kHz VFO) 1 / 1 - 39.0;
  `wildlife-five` (50 kHz VFO) first 4 / 5 - the second coded tag (6.0 s
  bursts) never confirmed, while offline at 48 and 50 kHz both tags were
  found. Cause (the plugin's input captured live and replayed offline):
  the FFTW_MEASURE plan took 6 s at 50 kHz (23 s at 2.4 MHz) on the Pi 5
  before the plugin read its first sample, kraken_doa dropped 2.3 s of its
  input (> 1 s backlog) right between that tag's first two bursts, and the
  confirmation wanted EVERY burst on one grid (3.665 s then 6.0 s: never).
  Fixed: FFTW_ESTIMATE (starts in ~10 ms, nothing dropped; pffft since
  2026-10-17), the latest
  consistent run of bursts, and `resume_tag` (a 2.3 s jump cut into a
  scene after both tags were confirmed: both resynchronised under their
  ids - the old code made "...b" / new-gap duplicates and lost the
  originals). Re-run 2026-10-15 (cleanup): `wildlife-five` 5 / 5 - 39.0 /
  129.0 / 167.9 / 302.0 / 260.8, `wildlife-five-survey` 5 / 5 - 39.1 /
  130.4 / 169.6 / 297.4 / 262.3, `wildlife-one-collar` 39.0. The plugin
  live at 2.4 MHz: 9.3 % of a core (the pipe included); kraken_doa ~70 %
  while playing
- **Live** (2026-10-15 19:25-19:38, heimdall retuned to 160.355 MHz via 8092
  and back, both settings files byte-identical): 4 x 100 s of 160.115-160.595
  MHz (sigtool captures at 480 kHz, decoded offline): no wildlife
  transmitter. Periodic multi-tone DATA bursts near 160.40 MHz (every 2.5 s,
  ~5 kHz wide, tone pieces of 1-47 ms) and 160.32 / 160.53 MHz first came out
  as "collars" (24 / 12 / 8 ppm, jitter 8-16 ms, lengths 11-47 ms) - fixed by
  the burst-piece rule, the jitter limit and the one-length rule; the last
  capture re-decoded: 0 collars (1434 pieces dropped as data lines)

- **Region-neutral rework** (2026-10-15, scratch wl2/): every offline
  scene above re-run with the old binary and with `profile=nz`: the same
  collars, tags, rates, data-stream numbers and bearings (40.5 / 172.6 /
  130.7 / 298.4 / 259.8), texts reworded ("Wildtech Chick Timer data"); the
  noise scene's one false collar (the tone-FM signal) unchanged, but its
  back-and-forth "rate changes" read as activity now, not "⚠ MORTALITY".
  The same scenes with the default `global`: collars named by MHz only, no
  channel column, all "baseline", 160.350's 30 -> 60 ppm "faster x2.0" (60 s
  scene, < the 2 min hold), the streams shown as pulse groups ("2 (3) 2 (5)
  2 (11) 2" = 139 h). Found and fixed on the way: a rate slowed 60 -> 40
  ppm put every other pulse half way between the old slots and read as a
  DOUBLING (120 ppm, for 70 s): the halving rule now also needs the pulse
  half a period after the one before; a rate switch's transition read as
  data ("code - numbers 1 0" before, "pulse-coded data" now): a rate change
  drops an open stream
  - **modes** (1 antenna, 96 kHz, 300 s, 151 MHz, seven collars):
    global - 40 -> 80 ppm at 90 s "faster x2.0", ⚠ possible mortality at
    219 s; 30 / 60 toggling every 40 s "activity" from the 3rd change
    (141.8 s), no ⚠; 55 steady "baseline"; 45 -> 90 -> 45: possible
    mortality at 182 s, "back to its baseline rate: moving again" at 213 s;
    60 -> 30 "slower x0.50"; the USFS-style collar (40 / 60 toggling, then
    80) activity, then x2.0 of its resting rate -> possible mortality at
    262 s; 30 -> 60 for 60 s and back: no ⚠. na - every doubling MORTALITY
    at once (63-103 s), the USFS collar's 80 "MORTALITY: rate 40 -> 80 ppm
    (x2.0 its resting rate)" at 143 s, toggling still "activity". nz - the
    DOC table on top (55 / 60 ppm first heard: "⚠ MORTALITY?", 45
    "incubating")
  - **North America** (5 antennas, 48 kHz, 60 s, 150.985-151.015 MHz,
    profile na): 40 ppm 49.2 deg, the 40 / 60 activity collar 141.3 ("activity"
    at 49 s), 40 -> 80 ppm 220.7 ("⚠ MORTALITY" 3 s after the switch was
    confirmed), a weak 24 ppm 312.3 (truth 50 / 140 / 220 / 310); table
    without a channel column, band "148-152 MHz wildlife collars"
  - **Motus** (5 antennas, 48 kHz, 60 s, 166.380 MHz, profile motus): four
    coded tags, two on one carrier: gaps within 0.2 ms, burst intervals
    exact, all "Motus 166.380"; 31.5 / 110.5 / 200.6 / 320.0 deg (truth 30
    / 110 / 200 / 320)
  - **Debug playback on the LIVE kraken_doa** (settings sandboxed;
    doa_settings.json and heimdall_settings.conf byte-identical after),
    50 s each, offset from the truth: `wildlife-five` (nz) 5 / 5 -0.9 / 0.0
    / -1.2 / -2.6 / +2.4 deg ("NZ ch 22 / 23" in the table, "⚠ MORTALITY?"
    for 160.350); `wildlife-five-survey` (global, 2.4 MHz) 5 / 5 -2.3 /
    +0.7 / +3.3 / -2.6 / +2.3 (no channel column, all "baseline");
    `wildlife-one-collar` -2.3; `wildlife-na-collars` 4 / 4 +1.8 / -0.3 /
    -0.6 / -0.7; `wildlife-motus-tags` 4 / 4 +0.5 / -1.9 / +3.9 / +1.0
    ("Motus 166.380" for all four, the 10 s tag confirmed after ~30 s)
  - false alarms: the noise scene (300 s: impulses, a carrier, two FSK
    data signals, tone FM) - the same single tone-FM "collar" as before in
    every profile; no collar from the rest
- **Real recording** (2026-10-16; verified against it, not in this repo):
  the UAV-RT project's published flight data (Zenodo 5013572, "UAV
  wildlife radio telemetry: system and methods of localization", range
  test RNG005-ant_V - a small bird / bat VHF beacon received from a
  drone, 48 kHz IQ, 294 s; one member fetched out of the 3.5 GB zip by
  HTTP range requests). The reference: an independent numpy analysis
  (the carrier's channel, 1 ms power envelope, half-power widths, phase
  slopes). The tag: 16.2-16.4 ms pulses every 1292.5 ms (46.4 ppm), jitter
  < 1 ms, carrier -643 Hz drifting ~-10 Hz/min, and a CHIRP inside every
  pulse (the oscillator pulled by the keying: ~-90 Hz over the pulse with
  a +-50 Hz wobble repeating pulse to pulse), rise 1-4 ms; well above the
  noise only 86-140 s (19-66 dB in 1 kHz), the rest fading / truncated
  pulses, plus ~12 one-ms impulses a second (the drone).
  Plugin before: found at 87 s as 15.5 ppm (3x the period), then
  "baseline -> faster x3.0" - a false rate change. Two causes, both fixed:
  (1) a real pulse with an impulse within 0.16 s counted as a GROUP (a
  coded-burst candidate) and was left out of the train search - a
  collar-length pulse whose group holds only blips < 40 % of its width is
  single now; (2) with 40 % of the first pulses lost a 3x grid scored best
  - a whole-multiple period (x2..x4) corrected within max(30 s, 8 old
  periods) of the discovery, before any other change, is taken as "it was
  always this fast" (re-baselined, no event) WHEN pulses from before the
  discovery sit on the new grid between the old one's slots (after a real
  switch none do). Now: one collar, 46.4 ppm (1292.7 ms, jitter 0.2 ms),
  16.6 ms, baseline, 32 DoA packets 87-144 s, lost after the flight leg.
  Generator: `chirp_hz` added (the carrier moving linearly over each
  pulse); a scene of 46.4 ppm collars with chirps 0 / -90 / -300 Hz, a
  40 ms pulse with -150 Hz, a real 30 -> 60 ppm switch 10 s after its
  discovery and 12 impulses a second: all six found, the switch still
  "faster x2.0" (old and new binaries identical on it, on the five offline
  scenes and the modes scene under global / na / nz). No coded-tag (Lotek /
  Motus) recording found - those paths stay synthetic only.
  `WILDLIFE_DUMP=1` (offline) prints every train search with its pulses
## Not decoded / limits

- Lotek coded IDs: the gaps + burst interval identify a tag, but Lotek's
  code table (gaps -> Lotek ID) is confidential (Motus decodes it on its
  servers under an agreement): ids are the measured pattern. Lotek pulses
  are ~2.5 ms (the Motus docs say ~20 ms - both work); a burst needs all 4
  pulses (~40 dBHz for 2.5 ms pulses); partial bursts are not matched
- ATS coded collars, Sirtrack coded / CODA patterns, Lotek's Time Since
  Death, Telonics / ATS activity (variable-rate) collars: their encodings
  aren't public - shown as rate changes / activity / pulse groups. A collar
  whose rate varies continuously with movement (no steady rate) isn't
  confirmed as a train (a crystal-steady period is required). Vendor rate
  tables for North America / Europe / Australia: none public - their
  profiles use the doubling convention. The first switch of an activity
  collar reads as "faster" (a possible mortality if it holds 2 min) until
  it switches back CTT LifeTag / PowerTag / Motus 434
  MHz FSK digital tags: not done
- The kiwi Chick Timer's exact timing (fast pulse / double-beep spacing) is
  not public: the parser takes either form (runs of pulses closer than half
  the period, < 1 s); a missed pulse changes a digit (no checksum) - the
  repeat 10 min later confirms
- A strictly periodic modulated FM signal (the generator's tone FM with a
  3.1 Hz modulation) can still pass as one collar; impulses, CW carriers,
  FSK data bursts don't
- Collars below ~31-33 dBHz per 20 ms pulse are missed (the gate at a
  known collar's next slot is only ~1 dB more sensitive); at the limit a
  collar may be found at twice its period until it halves

## Sources

Per profile / band (no GPL code used):

- **Generic modes**: R. Kenward, "Wildlife Radio Tagging" (1987), via J.
  Field Ornithol. 70(4) 587-590 (https://sora.unm.edu/ - the rate doubles
  after ~4 h motionless); Lotek LifeCycle / coded VHF spec sheets (rate
  rises on mortality, 4 h default; up to 512 coded IDs per frequency);
  Telonics MOD-316 (https://telonics.com/products/vhfStandard/MOD-316.php:
  30-150 ppm, mortality time 8 s - 6 days set per collar); USFS sheep
  collar (https://www.fs.usda.gov/t-d/programs/im/fy00/livestock/sheepcollar.pdf:
  40 ppm resting, 60 active, 80 mortality after 2.5 h)
- **North America**: 47 CFR 90.248 (https://www.law.cornell.edu/cfr/text/47/90.248:
  40.66-40.70 / 216-220 MHz animal and ocean buoy tracking, <= 1 kHz); the
  148-152 / 159-161 / 164-168 MHz agency bands are common use (no
  regulator source found; Canada's ISED rules not found)
- **Europe**: Germany BNetzA Vfg 125/2023 "Radiomarkierung von Tieren"
  (150.05-150.24 MHz, 2 mW); Finland Traficom (155.400-155.525 MHz,
  tracking collars, mainly hunting dogs); UK telemetry 173.2-173.35 MHz
  (SI 1995/1081) and 173.7-174 MHz incl. airborne bird tracking (SI
  1999/930) - the current Ofcom IR 2030 was not checked
- **Australia**: ACMA LIPD Class Licence 2025 (48-49 MHz aquatic animal
  tracking only - VHF collars need an apparatus licence); Motus 151.500 MHz
- **Africa / Asia** (band hints only): ICASA band study (148-152 MHz
  wildlife telemetry tracking); Japan MIC animal detection notification
  system (142.93-142.99 / 146.93-146.99 MHz)
- **Motus**: docs.motus.org "Tag selection", "How tags work", "Tag
  aliasing", "Beacon tags" (166.380 Americas / 150.100 Europe / 151.500
  Australia; 4-pulse bursts, the Lotek ID in the intervals, burst
  interval, tolerances)
- **New Zealand**: DOC Kiwi Best Practice Manual 2019 (section 6.4:
  frequency band, channels, 30 / 48 ppm, mortality = double after 24 h
  still, the time-since-death stream, the Egg Timer / Chick Timer fields);
  DOC Banding Office newsletter Oct 2023 (160.1-160.6 MHz general user,
  channels 00-47; 160.6-161.11 MHz channels 48-99 by DOC sub-licence);
  Wildtech "User manual for kakapo Egg Timer transmitters V6.3" (30 / 48 /
  80 ppm, 11 outputs, digit + 2 pulses, 3 s gap, 5 standard pulses); NZASE
  "Tracking with radio" (30 / 48 b/min, data stream of eight codes)
