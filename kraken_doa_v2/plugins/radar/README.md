# radar - pulse radar analyser (L-band ATC / surveillance)

Every pulsed radar in the 2.4 MHz the tuner receives - airport and en-route
primary surveillance radars (1215-1400 MHz), wind profilers, any other pulse
radar the receiver can tune to. There is no protocol and no content: the
plugin measures what a radar's signal says about the radar itself, like an
ELINT receiver - its pulses, their timing, its antenna - and gives every
radar its own bearing.

Pick "Pulse radar (L-band ATC / surveillance)" as a VFO's digital decoder and
tune the receiver to the radar. Like `ism`, the VFO is locked on its tuner's
centre with the full 2.4 MHz (`kp::Info::lock_vfo`); manual only, 2.4 MHz.

## What it measures

- **Pulses**: width (at half power), centre frequency, linear FM (chirp:
  rate in kHz/µs and the sweep over the pulse, up or down), level (dBFS and
  dB over the noise), clipping of the 8-bit ADC. A radar's pulse group is
  linked - several kinds of pulse at a fixed delay after each other
  (frequency diversity, a short "fill" pulse + a long chirp of a solid-state
  radar) when they are all inside the 2.4 MHz.
- **PRI / PRF**: the pulse repetition intervals and how they are used -
  constant (+ jitter), stagger (a fixed sequence, pulse by pulse), blocks
  (each PRI for a block of pulses - the coherent processing intervals of an
  MTD radar), jittered, and PRI sets the radar switches between (per scan).
- **Antenna**: scan period (rpm) from the main beam sweeping past once per
  turn, the main beam's -3 dB width (degrees, from the level profile of a
  pass - only when the pass doesn't clip), where it points now, the pulse
  level over one turn (plot: main beam, sidelobes, reflections).

## Outputs

- Table, a row per radar: Radar (its id), MHz, Pulse, PRF Hz, PRI, Scan s,
  RPM, Beam °, Peak dBFS, SNR dB, Pulses/s, Main beam (how long ago). A row's
  details (▸): every pulse kind (width, frequency, chirp, delay in the
  group, count), the PRI pattern + how much of the timing it explains, PRF,
  duty cycle, the PRI sets seen, scan period (and from how many passes),
  where the antenna points now, main beam width and level, the strongest
  pulse, the DoA pulses reported.
- Facts: pulses per second (+ clipped share), noise floor, carriers (runs > 2
  ms: CW / interference), the radars, and the strongest one's pulse, PRF,
  PRI, scan, level.
- Events: a new radar (with its parameters and band), another pulse kind of
  a radar, a new PRI set, its scan period (and a change of it), "⚠ main
  beam clips the receiver", not heard for 60 s / heard again.
- Plot (`kp::Host::plot`): the strongest radar's pulse level over one turn
  of its antenna, 0° = its main beam pointing here, the DoA level dashed and
  a "main beam" marker naming its talker (shown in the 📡 DF panel too;
  before the scan period is known: the level over the last 20 s).
- Raw frames (decoder data log, "Raw frames"): one line per main-beam pass.

## ID separation

A radar has no coded ID: it is identified by its **signature** - its pulse
kind(s) (carrier, width, chirp) and its PRI pattern. Each radar is a talker,
id = its frequency to 0.1 MHz ("1355.0 MHz"; a second radar within 100 kHz
gets "#2"), label "PRF 643 Hz · 59 µs LFM · 4.88 s". Its DoA comes from
packets (`kp::Talker::packet`) of exactly its pulses' samples (± 10 µs: whole
16-sample covariance chunks), with the pulse's own channel (`freq_hz` /
`bw_hz` = centre and sweep + 2 / width), averaged over `doa_avg` (30 s), kept
`keep` (1 h) - so several radars in the band get their own bearings.

Only **main-beam pulses** are used: when the radar's antenna points at the
receiver the direct signal is 20-50 dB over everything else. A pulse is
reported when it is unclipped (clipping bends the phases), >= 15 dB over the
noise, within `doa_db` (15 dB) of the scan's peak, and in a burst that stays
near the peak about as long as the main beam's passes do: a reflector the
main beam lights elsewhere in the turn arrives 10-20 dB weaker from the
reflector's direction - and when the main beam clips the receiver (its true
peak hidden) only its longer dwell near the clip level tells it apart.
Pieces of weak chirps, echoes and ghosts (below) never go into the bearing.
Reported only after two main-beam passes (or 30 s for a radar that doesn't
rotate); at most 60 packets a second per radar.

UI: kind `radar` in kraken_doa.html (📡, list "Radar / PRF · pulse · scan /
Pulses", by name, no position columns; "Every radar"), the DF panel's plot
section labelled for it.

## How

- `pulse.cpp` (`Detector`): per sample |x|^2, a 3-sample sum (short pulses)
  and a 24-sample sum (10 µs: a 60 µs chirp in a radar's sidelobes is ~10 dB
  per sample and fades across its sweep) against the noise (median of
  64-sample means: robust to pulses, CW and the 8-bit quantisation), the
  long threshold 6 dB under the short one (`sens`, 10 dB). A segment's
  pulses: runs where the 3-sample mean is over the short threshold or the
  9-sample mean over the long one, apart by > 4 µs; edges = the OUTERMOST
  -6 dB crossings of the peak (raw power for strong short pulses, 3- or
  9-sample means), corrected for the smoothing (W / 4 per side: a
  rectangular pulse's width comes out exact). Frequency from the phase
  steps; LFM from 6-sample pieces' frequencies fitted against time
  (unwrapped). Clipped = an I / Q sample >= 0.96 of full scale (the input is
  (u8 - 127.5) / 127.5). A ring of 2^15 samples (13.7 ms, in the L2 cache);
  pulses up to 2 ms.
- `decoder.cpp`: pulses -> **modes** (width, frequency, chirp within
  tolerance; medians of the last 256). A mode with >= 20 pulses in 4 s and a
  PRI structure becomes a **radar**. Folded into an existing radar instead
  (the same radar measured otherwise): **pieces** of its weak chirps (the
  same sweep rate, a shorter pulse inside its sweep - placed in the chirp by
  its frequency, one pulse per chirp), modes **on its PRI grid** (a pulse
  merged with its echo is longer and its sweep bent, but comes one PRI after
  the radar's previous pulse), modes with its PRIs on its frequency,
  **echoes** at a fixed delay >= 10 dB down, **ghosts** with its PRIs >= 20 dB
  down (the RTL's image of a strong radar outside the band), and radars with
  its scan period on its frequency. A weaker copy of a pulse within 0.3 PRI
  is an echo and dropped. A mode at a fixed delay (±1.5 µs, comparable
  power) to a radar's pulses is another pulse of its group.
- `pri.cpp`: consecutive differences (100 µs .. 30 ms) clustered ±max(1.5
  µs, 0.12 %); clusters that are sums of 2-3 smaller ones (with fewer
  members) are missed pulses; the rest are the PRIs. Their labels' runs ->
  blocks (median run >= 3; the cycle of levels), else a period -> stagger,
  else "no fixed order"; no levels but a peak -> jitter. A window whose
  levels change half way (a set switching) isn't taken for the set list.
- `scan.cpp`: levels in 5 ms bins over 41 s; bursts within 6 dB of the
  strongest that dwell there >= half as long as the longest = main-beam
  passes (a pass recorded in the first turn, before the real main beam came,
  is dropped when it no longer qualifies); scan period = the spacing most
  passes repeat at, refined by a fit over the chain; -3 dB width from a
  robust parabola (in dB) over the unclipped pulses of the lobe's top 10 dB.

## Generator

`tools/radar_gen.py SCENE.json OUT.cu8|OUT.cf32` - synthetic radars at known
bearings on kraken_doa's UCA (every antenna, sample by sample, written in 0.1
s pieces; the scene format is in the script): pulse groups (frequency, width,
LFM sweep, delay, level), PRIs (constant + jitter, stagger, blocks with
alternating sets per turn), a rotating antenna (scan, -3 dB beam width,
sidelobes with ripple), an optional reflector lit at an azimuth (its echo
from another bearing), noise in 8-bit LSB, cu8 clipped like the receiver's
ADC. `OUT.json` = rate, channels, rf_hz, radius_m, truth. The cu8 plays
through a fake heimdall into kraken_doa (8091 format) and goes straight into
`decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf HZ`.
`tools/scene_three.json`: three radars in one band (below). Debugging:
`RADAR_DUMP=1 decoder --file ...` logs every pulse (time, width, level,
frequency, chirp, clip, mode), every second's modes and every main-beam
pass.

## Tests (Pi 5, 2026-10-15)

Synthetic (`tools/scene_three.json`, 5 antennas, 12 s, UCA r 0.12 m):

| radar | truth | measured |
|---|---|---|
| ATC-SS | 50 µs LFM -0.8 MHz at +400 kHz + 1 µs at -150 kHz 80 µs later; PRI 1712 / 1395 and 1748 / 1424 µs in blocks of 8, sets alternating per turn; scan 4.0 s, beam 1.4°; 60°; a reflector lit at 120° echoing from 150° (-12 dB) | 49 µs LFM -0.78 MHz + 1.0 µs (+80 µs, at -152 kHz); "1748 / 1424 us, blocks of 8; also 1395 / 1712 us"; 4.000 s, 1.43°; **60.0°** |
| MAG-STAG | 2 µs at -800 kHz, stagger 1100 / 1250 / 1180 µs, scan 2.5 s, beam 2.5°, 200° | 2.0 µs, "stagger 3: 1250 / 1180 / 1100 us", 2.500 s, 2.55°; **200.0°** |
| JIT | 1 µs at -450 kHz, PRI 2500 µs ± 5 % uniform, scan 3 s, beam 2°, 300° | 1.1 µs, "~2484 us ± 2.8 % (jittered)", 3.000 s, 2.01°; **300.0°** |

Offline bearings (`--channels 5 --uca 0.12`) and end to end through the real
kraken_doa (scratch cwd, fake heimdall streaming the cu8, DF status `doa`)
both exact: 60.0 / 200.0 / 300.0°; end to end 1415 of 1430 packets used
(15 fell where kraken_doa wasn't keeping the stream), report lag 0.33 s. More scenes: noise
only (8 s, 2 LSB): 0 pulses; weak (22 dB / 15 dB main beams): both radars
found with the right PRIs, LFM -0.99 MHz, scan 2.50 s; a staring radar (no
rotation) + an up-chirp (+0.6 MHz, stagger 2000 / 2300): both measured
right; a main beam 52 dB over the noise (clipping the 8-bit ADC by ~10 dB)
with a reflector lit at 90° echoing from 250° at -14 dB: bearing **140.0°**
(truth 140; before the dwell rule it read the reflector's 253.6°). UI
(headless Chromium, scratch t_ui_radar.py): DF list (📡, columns, "Every
radar (3)", sorted by name), one radar's line, the DF panel's plot section,
the decoder tab's table + details + plot.

Real, Auckland (5-element UCA r 0.12 m): an L-band surveillance radar on
**1355.0 MHz** - **59 µs pulses, linear FM -1.17 MHz (down-chirp, -19.8
kHz/µs)**, **PRF 643 Hz: PRI 1712 / 1395 µs in blocks of 8 pulses, in the
next turn 1748 / 1424 µs** (the sets alternate per turn), **scan 4.88 s
(12.3 rpm)**, main beam **1.5° at -3 dB** (20.9 ms, at 12 dB gain; at 30 /
43 dB it clips the ADC), duty cycle 3.8 %; the same radar also sends a short
pulse (1.2 µs) on **1350.07 MHz** with the same PRIs and main-beam times
(frequency diversity: a separate VFO 4.9 MHz away). **Bearing 116-119°**
in the array frame (~255° compass with the station's heading) in every run:
offline 118.7° (20 s at 30 dB gain), 117.1° (43.4 dB), 121.3° (the 1350 MHz
pulse); live 117.0° / 116.8° (43.4 dB, the user's gain, 90-100 s) and
116.3° (12 dB). Live it found the radar within ~4 s and the scan within ~15 s.

CPU (Pi 5): the plugin **2.2-2.9 % of a core** live (2.4 MHz, 640-730 pulses
/ s); the detector alone ~2-3 %. kraken_doa itself ~80-90 % with a 2.4 MHz
VFO and DoA on (as with ism / adsb).

## Limits

- One 2.4 MHz slice: a radar's pulses on another frequency (the 1350.07 MHz
  short pulse above) need a VFO of their own (they are another "radar" there);
  chirps wider than the slice are measured in part.
- Two co-channel radars with the same pulses and PRIs merge into one;
  pulse-to-pulse frequency-agile radars and PRFs over 10 kHz are not
  analysed; non-linear FM / phase codes show as plain or the nearest LFM.
- The 8-bit receiver: a nearby radar's main beam clips at normal gains - its
  peak level and beam width can't be measured then, and its bearing comes
  from the beam's flanks (fewer pulses; a lower gain gives more).
- Pulses shorter than ~0.4 µs (one sample) are dropped as noise.

## Sources

Public radar / ELINT literature as background (pulse descriptor words,
PRI deinterleaving by difference histograms, stagger / CPI PRF switching,
antenna scan analysis): Skolnik, *Radar Handbook*; Wiley, *ELINT: The
Interception and Analysis of Radar Signals*; Mardia, *New techniques for the
deinterleaving of repetitive sequences* (IEE Proc. F, 1989). No code from
elsewhere.
