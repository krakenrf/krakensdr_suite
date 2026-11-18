# 121.5 / 243 MHz distress beacons (`beacon121`)

The legacy swept-tone homing signal on 121.5 MHz (military: 243.0 MHz) -
sent by ELTs (aircraft), EPIRBs (ships), PLBs (personal), man-overboard
devices, and by every 406 MHz beacon as its low-power homer. The signal
carries no ID: this plugin finds every such beacon in the VFO, describes it
and gives each one its own bearing on the 🗺 Map's 📡 DF panel - direction
finding is how these beacons are found.

**A heard DISTRESS beacon must be reported to the rescue coordination
centre** (or to air traffic services) with its time, frequency and bearing.
The decoder reports nothing to anyone itself, and one receiver cannot tell
a real distress from an inadvertent activation or a test. A 406 MHz
beacon's homer (see *Pauses*) also sends its ID on 406 MHz - run the
`beacon406` decoder on 406.0-406.1 MHz for it.

## The signal (what the decoder checks)

From ITU-R M.690-3, ETSI EN 302 152-1 clause 8.2 (the 121.5 MHz homing
device of a 406 MHz PLB), UK CAA Specification 16 (ADELT, consistent with
JTSO 2c91a / RTCA DO-183) and C/S T.001 4.5.3:

| | |
|---|---|
| Carrier | 121.5 MHz +-50 ppm (+-6.075 kHz; EN 302 152-1 8.2.2), 243 MHz +-0.005 % (CAA Spec 16) |
| Emission | AM, full carrier, both sidebands (A3X); >= 30 % of the power within +-30 Hz of the carrier (+-60 Hz at 243 MHz) |
| Modulation factor | (A - B) / (A + B) of the envelope >= 0.85 (EN: 0.85 - 1.0) |
| Audio | a tone sweeping over >= 700 Hz within 300 - 1600 Hz - downward (EN 302 152-1, CAA Spec 16), upward or downward (M.690-3) |
| Sweep repetition | 2 - 4 sweeps a second |
| Duty cycle | the positive half cycle at the envelope's half-amplitude points / the audio period: 33 - 55 % (EN), 33 - 66 % (CAA) |
| Keying | continuous, or a keyed carrier (CAA: e.g. 0.75 s on / 1.5 s off); optionally 2 s of unmodulated carrier every 8 s |
| 406 MHz beacons' homer | 25 - 100 mW; never at the same time as the 406 MHz burst (T.001) - interrupted for <= 2 s (EN), optionally followed by an identification letter in Morse (FCC 47 CFR 95.1402: a 406 MHz PLB sends "P") |
| Self-test | <= 3 sweeps or 1 s of homing (EN 302 152-1 4.7, on the label) |

## What it shows

Picking the decoder tunes the VFO to 121.500 MHz at 32 kHz (`fixed_freq_hz`,
manual only): every carrier within +-12.5 kHz - the +-50 ppm of the
standard and old beacons drifted further. For 243 MHz tune the receiver
there (the VFO stays on its tuner's centre); the plugin names what it hears
by `host.rf_hz()`.

Per beacon (table row + details, one talker each):

- **Status**: `⚠ DISTRESS` (121.5 MHz) / `⚠ DISTRESS 243` once its sweep
  was heard for 5 s (a weak one heard now and then: 2 s of sweep over
  15 s), `confirming` before, `short (test?)` for a transmission that ended
  within ~6 s (a self-test), `weak, not confirmed`, and `training / test
  frequency?` for a beacon further than 15 / 25 kHz from 121.5 / 243 MHz
- **Carrier** to 10 Hz, its offset from 121.5 / 243 MHz in kHz and ppm
- **Sweep**: direction (↓ downward, ↑ upward, ↕ up and down), range (Hz),
  repetition (sweeps / s, period)
- **Modulation factor** (from C/N0 >= 60 dBHz, no other carrier within
  5 kHz) and **duty cycle** (from 48 dBHz, from the tone's low part < 700
  Hz, where its harmonics are inside the channel)
- **Against the standards**: what lies outside them (carrier > 50 ppm,
  rate outside 2 - 4 /s, sweep < 700 Hz wide or outside 300 - 1600 Hz,
  depth < 0.85, duty outside 33 - 55 %)
- **Pauses** of the carrier: count, length, interval - every 8 - 150 s and
  <= 2.5 s long = "the homer of a 406 MHz beacon"; regular every < 6 s =
  "keyed carrier, x s on / y s off". Only a carrier >= 20 dB over the noise
  in 20 ms (C/N0 >= ~37 dBHz) reports pauses - a weaker one's gaps may be
  noise
- **Unmodulated periods** (carrier without the tone for 1.2 - 10 s)
- **Morse**: a letter right after a pause, keyed in the tone or in the
  carrier - shown after it was heard twice ("'P' (.--.) ... the identifier
  406 MHz PLBs send on their homer")
- **C/N0** (dBHz, now and strongest), first / last heard (UTC), time on the
  air, DoA packets
- **Plots**: the sweep of the strongest beacon on the air (audio Hz over
  one sweep) and its C/N0 over the last 2 minutes (walking DF)
- **Events**: "⚠ DISTRESS BEACON on 121.5 MHz: ... - report it to the
  rescue coordination centre (time, frequency, bearing)" (again every 5
  min while it transmits), the 406 MHz homer's pauses, keying, CW periods,
  Morse, "not heard for 30 s" / "heard again", "Short swept-tone
  transmission (1.0 s) ... - a beacon test?", an off-frequency beacon
  ("a training / exercise beacon?")
- **Facts**: beacons heard (⚠ distress, on the air), the last distress
  beacon, the strongest, **carriers without a sweep** (voice, bare
  carriers, tones - explicitly not beacons), channels followed
- Raw (decoder data log): `B121 <id> <direction> <rate>/s <lo>-<hi> m<depth>
  d<duty> cn0 <dBHz>` every 10 s per beacon

## ID separation

There is no coded ID: a beacon is told apart by its **carrier**. Each one is
a `kp::Talker` packet every 0.5 s - 0.24 s of the carrier's latest stretch
on the air, `freq_hz` = the tracked carrier, `bw_hz` 100 Hz (the
sidebands start 300 Hz out, the carrier holds >= 30 % of the power within
+-30 Hz) - so kraken_doa filters each beacon's carrier out of every antenna
and beacons transmitting at the same moment get their own bearings, even
150 Hz apart (tested, below). `avg_s` = option `doa_avg` (default 2 s),
`keep_s` = `keep_h` (default 24 h). Id = the carrier in MHz to 100 Hz
("121.5034"; offline without an RF "+3.40k"), kept while the carrier
drifts; a beacon heard again within 300 Hz of a known one keeps its id. UI
kind `sar` 🆘 (as `beacon406`). The **Digital squelch** opens on the
carrier-on spans of confirmed beacons (`host.valid(start, end)` every 0.5
s, `squelch_hold_s` 1 s): the VFO's own DoA then comes from the beacons'
samples only.

Limits: two beacons within ~40 Hz are one carrier (one row, a mixed
bearing); a beacon 17 dB or more weaker than another within 1.6 kHz is
taken for that one's sideband and missed.

## Receiver (`rx.cpp`)

- **Carriers**: 4096-point FFT every 2048 samples at 32 kHz (7.8 Hz bins),
  averaged ~0.5 s, floor = the median bin of +-12 kHz; a carrier = a
  narrow local maximum >= 9 dB over it in 3 successive spectra, not a
  sideband of a stronger line (the spectrum at the mirror frequency within
  3.5 kHz holds a similar line - a steady tone's or a voice's lines). Up
  to 8 carriers are followed, each a **channel** started on the last 2.5 s
  of the input (a 1 s self-test is caught from its start); a channel
  without its carrier for 30 s closes
- **Channel**: NCO + 31-tap low-pass, 16 kHz; carrier AFC from the 20 ms
  block means (the block mean IS the carrier: the sidebands >= 300 Hz
  average out) - also the carrier's presence (pauses, keying, Morse) and
  C/N0 against the finder's noise floor; the envelope's 2 / 98 % points per
  block -> modulation factor and duty cycle
- **Sweep**: a 256-point FFT every 8 ms (62.5 Hz bins) on BOTH sides of the
  carrier (187.5 - 2000 Hz); every 0.5 s the last 6 s: each bin / side
  normalised by its median (a steady tone vanishes), a cell = the weaker
  side, clipped at 13 dB; the autocorrelation over 0.18 - 0.8 s lags gives
  the period (a peak at 2 / 3 / 4 periods falls back to the period),
  refined to 0.05 frames by the sharpest fold; the folded trace (the tone's
  frequency per phase of a sweep, sub-bin from the unclipped fold's log)
  must be lit in >= 40 % of the period, monotonic (|Spearman| >= 0.8 over
  the best circular shift) or up-then-down, >= 450 Hz wide, fill >= 60 %
  of its range (a two-tone siren doesn't), jump > 300 Hz at most once +
  10 %, run 1.25 - 5.5 sweeps / s, and appear on both sides alike (each
  side's own fold has the tone at the same frequency in >= 60 % of the lit
  phases - two neighbours' one-sided sidebands can cross on the weaker side
  of a third carrier). Two passes in a row with the same period (+-6 %)
  make it a beacon
- **Morse**: the carrier key (20 ms blocks) and the tone key (8 ms frames,
  the 16 ms window's smear taken off) as runs; 2+ marks of 30 - 600 ms
  between a key-up run of >= 0.6 s and a long run after; dot / dash by the
  marks' own lengths (unit 30 - 200 ms), letters by `nav::morse_char`
  (`plugins/lib/navaid`). A tone-keyed letter only within 4 s after a
  carrier pause (EN 302 152-1: "immediately following each pause")
- Debug (offline): `B121_TRACE=1` + `--verbose` prints every evaluation's
  folded trace, `B121_MORSE=1` the key runs that were decoded

## Tests

Generator `tools/beacon121_gen.py` (one transmitter per file, cf32 + .json
sidecar): sweeps down / up / up-down, linear / exponential, square wave
(band-limited, any duty) or sine, depth, carrier drift, sweep-rate wobble,
keyed carrier, CW periods, 406-style pauses with a Morse letter (keyed in
the tone or the carrier), `--at` / `--dur`, `--cn0` (noise for an exact
C/N0 of a level-0 carrier); and the guard channel's other signals: a bare
carrier, a steady AM tone, a two-tone siren, voice-like AM (pitch,
formants, syllables, pauses). Debug playback entries
(`tools/testsignals.json`): `beacon121-four` and `beacon121-squelch`.
Results (scratch tests, x86_64 i7-11800H; 2026-11-18):

- **In-spec beacon** (50 dBHz, +2.5 kHz): ⚠ at 7 s after it started,
  carrier 121.50250, ↓ 1581-322 Hz (truth 1600-300; the trace is the 16 ms
  window's mean - ~20-30 Hz inside the ends), 3.00 /s, C/N0 49.6 (truth 50)
- **Depth / duty** (3 seeds each, 10 s): depth 0.90 / duty 0.40 read 0.93 /
  0.39 at 48 dBHz, 0.94 / 0.40 at 53-56, 0.937 / 0.403 at 58, 0.915 / 0.403
  at 63; depth 0.85 / duty 0.50: 0.911 / 0.49 at 50, 0.886 / 0.50 at 59,
  0.861 / 0.50 at 64 - the envelope's noise biases the depth upward, hence
  shown only from 60 dBHz; at 70 dBHz 0.85 / 33 % -> 0.85 / 33 %, 1.0 / 55 %
  -> 0.98 / 55 %, sine 0.95 -> 0.95 / 50 %; C/N0 matches the truth for 50
  % duty (a 33 % square wave lowers the carrier itself by 3 dB: 66.5 at 70)
- **Edges of the specification** (45 dBHz unless noted): 2 /s 600-1100 Hz
  -> ↓ 1096-599, flagged "497 Hz wide (>= 700)"; 1.5 /s and 5 /s found and
  flagged; upward 400-1200 Hz 2 /s -> ↑ 390-1190; exponential 1600-300 3.5
  /s -> 1554-307; up-down 2.5 /s -> ↕ 297-1547; carrier +8 kHz (66 ppm) ->
  ⚠ with "carrier +66 ppm (> 50)"; depth 0.6 / duty 30 % at 65 dBHz -> 0.63
  / 30 %, both flagged; 243 MHz +10 kHz (`--rf 243e6`) -> "⚠ DISTRESS 243",
  +41 ppm; drift 200 Hz/min + sweep rate +-5 % over 60 s -> one beacon,
  id kept, carrier followed to +0.20 kHz
- **Sensitivity** (30 s, 4 seeds per row, the carrier's C/N0): in-spec 3 /s,
  2 /s 400-1100 Hz and upward 4 /s - **28 dBHz: 12 / 12 ⚠ within 9-11 s**,
  26 dBHz: 7 / 12 (16-24 s), 24 dBHz: 0 / 12. 30 dBHz: 12 / 12 in 7-9 s
  (with a 6 dB receiver noise figure 28 dBHz is about -140 dBm of carrier)
- **Homer with pauses** (50 dBHz, 0.5 s every 50 s, 130 s): "pauses every
  50.0 s for 0.50 s - the 121.5 MHz homer of a 406 MHz beacon" at 61 s, the
  Morse 'P' after it (tone-keyed 96 ms dot; carrier-keyed 100 ms), shown
  at 63 s (its second time); keyed carrier 0.75 / 1.5 s -> "keyed carrier,
  0.77 s on / 1.49 s off"; CW 2 s every 8 s -> "unmodulated carrier for 2.0
  s every 8.0 s"; 28 dBHz with pauses: no pause reported (too weak to be
  sure), the beacon ⚠ at 8 s
- **Self-tests**: 1.0 s (3 sweeps) -> "Short swept-tone transmission (1.0
  s) ... - a beacon test?", no ⚠; 2.5 s -> the same, "(2.5 s)"
- **False alarms** (no beacon, no talker, no valid frame in any): 300 s of
  noise; voice-like AM 5 seeds x 40 / 60 dBHz x 120 s; a 60 dBHz carrier;
  steady tones 400 / 1000 / 1500 Hz at 55 dBHz; two-tone sirens 700/1100 Hz
  2 /s, 500/1300 3 /s, 400/1500 2.5 /s - each listed only under "Carriers
  without a sweep" (before the mirror rule a 60 dBHz voice left 2-6 "carriers"
  at its pitch harmonics)
- **Several beacons at known bearings** (`uca_scene.py`, 5-element UCA r
  0.12 m, `decoder --file scene.cf32 --channels 5 --uca 0.12`):
  `beacon121-four` (ELT 121.4970 at 40 deg, 406 homer 121.4995 at 150 -6
  dB, upward 121.5042 at 260 -3 dB, an old one at 121.5105 at 100, AM voice
  121.5018 at 200, a carrier 121.4920 at 320) -> **39.9 / 149.9 / 259.9 /
  100.2 deg**, the homer's pauses + Morse 'P', voice and carrier not
  beacons; two beacons 1.2 kHz apart (30 / 200 deg) -> 30.2 / 199.9, the
  second 10 dB weaker -> 30.1 / 198.5; **150 Hz apart** -> 30.1 / 200.0;
  three 2.5 kHz apart (30 / 120 / 250) -> 29.7 / 120.0 / 250.0
- **End to end** (a TEST kraken_doa on moved ports, its debug playback, no
  heimdall): `beacon121-four` 4 / 4 talkers 39.8 / 149.7 / 259.7 / 100.0
  deg (two more runs 40.0 / 150.3 / 259.4 / 100.2 and 39.7 / 149.2 / 259.6
  / 100.2), every expected event.
  The first run also gave the AM voice a "confirming" talker (9 packets):
  the two same-rate beacons' one-sided sidebands crossed on its weaker
  side - fixed by the both-sides check (offline every scene unchanged).
  `beacon121-squelch` (ELT at 70 deg 0-12 / 20-30 s, AM voice from 250 deg
  between): with the Digital squelch the VFO's DoA 69-71 deg over the whole
  run, the talker 69.6; with the squelch off the VFO DoA swings to 241-251
  while the voice talks. doa_settings.json byte-identical after a playback
- **CPU**: offline 0.6 % of real time with one beacon, 3.2 % with the six
  carriers of `beacon121-four`; live in the playback 4.3 % of an x86 core
  (six channels). Not measured on a Pi
- Not tested: real recordings (none found with IQ), the Pi, the web UI
  (the VFO's tuning on pick is the generic `fixed_freq_hz` path)

## Not done / limits

- A real carrier within ~1 Hz of the tuner's centre is removed by
  kraken_doa's DC tracking (EMA, ~1 Hz corner): picking the decoder puts
  121.500 MHz exactly there. A beacon is rarely that close (+-50 ppm,
  receiver error), but if one is expected right on 121.500, move the
  receiver a few kHz off and use a manual VFO
- Morse: only right after a pause, only when the carrier is strong
  (>= ~37 dBHz); how a beacon keys its letter (tone or carrier) isn't in
  the texts read - both are decoded; not verified against a real beacon
- The modulation factor is biased upward below 60 dBHz (not shown there);
  the duty cycle needs the tone's harmonics: measured on tones < 700 Hz
- Second-generation 406 MHz beacons' homers behave the same; their 406
  MHz bursts are only detected by `beacon406`
- No position (the signal has none): the bearings are the location

## Sources

ITU-R M.690-3 (2015) and M.690-2 (2012) "Technical characteristics of
emergency position-indicating radio beacons operating on the carrier
frequencies of 121.5 MHz and 243 MHz" (itu.int); ETSI EN 302 152-1 V1.1.1
(2003) "Satellite Personal Locator Beacons (PLBs) operating in the 406.0
MHz to 406.1 MHz frequency band; Part 1" clauses 4.7, 8.2 (etsi.org);
UK CAA Specification 16 Issue 2 (1991) "Automatically Deployable
Emergency Locator Transmitter for Helicopters" 2.2 (caa.co.uk); C/S
T.001 Issue 4 Rev.7 (2021) 4.5.3 "Auxiliary Radio-Locating Device"
(cospas-sarsat.int); 47 CFR 95.1402 (Morse "P", continuous duty cycle
interrupted only during the 406 MHz transmission) and 2.1513 (EPIRB
modulation measurements). ICAO Annex 10 Vol. III and RTCA DO-183 / DO-204
were not available; the CAA specification states its consistency with
JTSO 2c91a / DO-183. No third-party decoder code used.
