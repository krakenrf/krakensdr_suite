# wildlife - VHF wildlife tracking transmitters (collars / tags)

The classic direction-finding job: kiwi, kakapo, deer, possum, bird and
bat transmitters send a short CW pulse every second or two, and people walk
with a Yagi to find them. This plugin finds every such transmitter in the
VFO, names it by its frequency (+ code), reads its mode, and gives each
one its own bearing on the 🗺 Map's 📡 DF panel (🐾 collar).

- **Any VFO width** - the plugin takes the VFO's OWN rate
  (`kp::Info::sample_rate = 0`, new in the SDK with this plugin): a 2.4
  MHz VFO on the tuner's centre surveys the band (e.g. all of NZ's
  160.12-160.59 MHz at once), a narrow VFO (5 kHz and up) follows one
  collar. The same detector runs at every rate with ~200-300 Hz FFT bins.
  Manual only (not in Auto detect).
- **Bands** (facts "Band" names them): NZ 160.12-160.59 MHz (general use,
  Telonics TR4 channels 0-47 = 160.120 + 0.010 n MHz; 48-99 = 160.60-161.11
  MHz by special licence - DOC Kiwi Best Practice Manual 2019, 6.4), US
  148-152 / 159-161 / 164-167 / 216-220 MHz, Motus 166.380 (Americas),
  150.100 (Europe), 151.500 MHz (Australia). The table shows the NZ channel
  and the carrier's offset from it.

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
- **Modes** (option `profile`): `nz` (default, the DOC convention: 30 ppm
  normal, 48 incubating - Egg Timer / Chick Timer -, mortality = double:
  60, kakapo 80) or `change` (only a change counts). A rate change x1.8-2.25
  or x2.4-2.9 = MORTALITY (⚠ event, "no movement for the transmitter's
  mortality time - dead, or the collar came off"), x1.45-1.75 from 30 =
  incubating; a collar first heard at the NZ mortality rate: "⚠
  MORTALITY?"
- **NZ data streams** (every ~10 min; runs of close pulses = R, single
  pulses = N; `train.cpp`):
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
- **Table** Collar, Name, MHz (4 decimals), Ch (NZ), ppm, Pulse, Mode, Code,
  C/N0, Level, Pulses; details: carrier to the Hz, NZ channel offset, period
  + jitter, previous rate, drift (Hz/min), the data stream's fields, the
  coded tag's gaps / interval, the C/N0 of the last 20 pulses (walking DF).
  Kept `keep_h`. **Plot** "Pulse strength": C/N0 per pulse over the last 2
  minutes of the `focus` collar (else the one heard last), the detection
  limit dashed - the signal-strength aid for walking. Facts: collars, the
  strongest, pulses (+ dropped data lines), detector (bin width, the
  per-pulse limit ~31-33 dBHz, impulse frames), band. Events: new collar /
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

`names` (`160.512=Kiwi Bob, 160.734=Kaka 3` - MHz within 1.5 kHz, or an
exact id; shown in table, talker label, events), `profile`, `doa_avg`,
`keep_h`, `chan_bw`, `sens` (-1 / 0 / +1: thresholds 2 dB up / down),
`focus` (the plotted collar).

## Tests

Generator `tools/collar_gen.py` (`tx`: one transmitter for uca_scene.py;
`scene`: every antenna streamed in 0.05 s pieces at any rate incl. 2.4 MHz,
noise, impulses / CW / FSK bursts / FM interference): trains with jitter,
drift, rate switch, the TSD / Chick Timer / Egg Timer streams, coded
bursts. Playback entries (`tools/testsignals.json`): `wildlife-five` (50 kHz
VFO), `wildlife-five-survey` (the same in a 2.4 MHz VFO),
`wildlife-one-collar` (10 kHz VFO on one collar). Results (scratch t9/):

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
  MHz (measured with an FFTW_MEASURE plan; ESTIMATE since 2026-10-15 -
  MEASURE planned 23 s at the start for a 77 -> 59 us FFT, ~1 % of a core),
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
  Fixed: FFTW_ESTIMATE (starts in ~10 ms, nothing dropped), the latest
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

## Not decoded / limits

- Lotek coded IDs: the gaps + burst interval identify a tag, but Lotek's
  code table (gaps -> Lotek ID) is confidential (Motus decodes it on its
  servers under an agreement): ids are the measured pattern. Lotek pulses
  are ~2.5 ms (the Motus docs say ~20 ms - both work); a burst needs all 4
  pulses (~40 dBHz for 2.5 ms pulses); partial bursts are not matched
- ATS coded collars, Sirtrack coded / CODA patterns, Telonics / ATS
  activity (variable-rate) collars: their encodings aren't public - shown as
  rate changes / "numbers ..." streams. CTT LifeTag / PowerTag / Motus 434
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

DOC Kiwi Best Practice Manual 2019 (section 6.4: frequency band, channels,
30 / 48 ppm, mortality = double after 24 h still, the time-since-death
stream, the Egg Timer / Chick Timer fields); Wildtech "User manual for
kakapo Egg Timer transmitters V6.3" (80 ppm mortality, 11 outputs, digit +
2 pulses, 3 s gap, 5 standard pulses); NZASE "Tracking with radio" (30 /
48 b/min, data stream of eight codes); Motus docs "How tags work", "Tag
aliasing", "Beacon tags" (4-pulse bursts, burst interval, tolerances,
frequencies). No GPL code used.
