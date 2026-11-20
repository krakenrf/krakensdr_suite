# 121.5 MHz beacons plugin

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in
`kraken_doa_v2/CLAUDE.md`). User-facing description, the signal table,
every test number and the sources: `plugins/beacon121/README.md`.

**121.5 / 243 MHz swept-tone beacons (`plugins/beacon121/`, 2026-11-18):**
- The homing signal of ELTs / EPIRBs / PLBs / MOB devices and of every 406
  MHz beacon's homer (the follow-up *406 MHz distress beacons* listed as
  not done). From ITU-R M.690-3, ETSI EN 302 152-1 8.2, UK CAA Spec 16,
  C/S T.001 4.5.3, 47 CFR 95.1402 (no ICAO Annex 10 / DO-183 text was
  available). A plugin of its own, not part of beacon406: another
  frequency, another VFO (32 kHz vs 100 kHz), nothing shared in the
  receiver; the two point at each other (a homer's pauses -> "run the 406
  MHz decoder")
- `sample_rate` = `min_vfo_rate` 32 kHz (2.4 MHz / 75: an efficient width,
  +-12.8 kHz flat), `fixed_freq_hz` 121.5 MHz, manual only, talkers,
  `squelch_hold_s` 1. 243 MHz: the user retunes the receiver (the locked
  VFO stays on the tuner centre); names come from `host.rf_hz()`. Distress
  = within 15 kHz of 121.5 / 25 kHz of 243 MHz (the whole VFO: +-50 ppm is
  the spec, old beacons drift further - never call a drifted distress
  beacon a training one); further away "training / test frequency?"
- `rx.cpp` Receiver: 4096-point finder FFT (7.8 Hz bins, 64 ms hop, ~0.5 s
  average, floor = median of +-12 kHz, also the noise variance every
  channel's C/N0 / presence thresholds use); a line >= 9 dB, narrow, 3
  spectra in a row, NOT a sideband (a stronger line within 3.5 kHz whose
  mirror frequency holds a similar level in the averaged spectrum - a line
  list check missed voice harmonics); <= 8 channels, each started on a 2.5
  s backlog from a 3 s input ring
- Channel: NCO + 31-tap LP -> 16 kHz; 20 ms block mean = the carrier
  (presence: > 12 x its noise and > 0.1 x its level; AFC; C/N0 = |c|^2 FS /
  sigma^2 minus the noise; pauses >= 13 blocks, reported only at >= 20 dB
  block SNR; a weak carrier's presence comes from the finder instead -
  blocks flicker); envelope 2 / 98 % points -> depth, share above the
  midpoint -> duty (the [1 2 1] smoothing first used cut a 1.6 kHz tone's
  depth by 10 %: removed)
- Sweep detector (`analyse`, every 0.5 s on 6 s): 256-point FFT / 8 ms on
  both sides; median-normalised, weaker side, clipped 13 dB; lag
  autocorrelation 22-100 frames, sub-multiples /2 /3 /4 (a 4 /s sweep =
  31.25 frames peaked at 3 periods, the lag nearest a whole multiple);
  fold refined +-1 frame in 0.05 steps; trace sub-bin from the UNCLIPPED
  fold's log (the clipped one is flat on top: a staircase / zigzag trace
  before); tests: lit >= 40 %, Spearman >= 0.8 over the best shift (or
  up-down segments), span >= 450 Hz, fill >= 60 % (two-tone sirens), jumps
  <= 1 + 10 %, 1.25-5.5 /s, both sides' own folds alike >= 60 % (added after
  the live playback gave AM voice a "confirming" talker: two same-rate
  beacons' one-sided sidebands crossing on its weaker side). 2 passes with
  one period (+-6 %) = a beacon. Debug `B121_DEBUG` (+ `B121_TRACE`),
  `B121_MORSE` - environment variables, not `host.verbose()`, which since
  the 2026-11-19 logging rework means "Show all messages" / Logging "Log
  All" (decoder-log.md) and would put diagnostics every 0.5 s into them
- Morse (`RunMorse`): runs of a key; a long run (>= 0.6 s) ends the group
  AT ONCE (first version waited for the run's end - the 48 s sweep after
  the letter); key-up lead required; the tone key's 16 ms smear subtracted;
  tone-keyed only <= 4 s after a pause (a false 'M' between two
  overlapping beacons before that rule); shown after 2 identical decodes
- `decoder.cpp`: beacons by carrier (id MHz to 100 Hz, re-attached within
  300 Hz), heard_s only while the carrier is there (the 6 s window kept a
  2.5 s test "sweeping" for 4 s more: it reached the 5 s ⚠ before that),
  confirmed = 5 s of sweep or 2 s over 15 s on the air; short (< 6 s on the
  air) -> "a beacon test?"; homer = >= 2 pause starts (a pause within 4 s
  after another - a carrier-keyed letter's last gap - left out) every
  8-150 s, <= 2.5 s; keyed = >= 5 pauses all within 20 % of a < 6 s
  interval. Talker packet per evaluation: 0.24 s of the latest carrier run,
  `freq_hz` = carrier, `bw_hz` 100 (kraken_doa's `narrow_cov` path at 32
  kHz). Valid spans = carrier-on runs of active beacons. Plots `sweep`,
  `strength`. Page: `RDF_PK_KIND` beacon121 -> `sar` (one entry). Merged
  with main 2026-11-20 (the redesigned page, logging per decoder, the
  removed Invert option): `host.rf_inverted()` dropped as in beacon406 /
  wildlife; the distress / ⚠ wording left as beacon406's (main didn't change
  it; plugin text is third-party text the page shows as it is, icons.md)
- Tests (scratch `tmp/beacon121/`: harness.py, t_sens / t_false / t_edge /
  t_depth / t_close / t_scene.py offline; kd/ = a TEST kraken_doa on 2818x /
  28179 / 28191-2 / 28147 + ws.py + t_pb.py for the playback): numbers in
  the plugin README - 28 dBHz 12 / 12, 0 false beacons in 300 s noise + 1200
  s voice + carriers / tones / sirens, `beacon121-four` offline 39.9 /
  149.9 / 259.9 / 100.2, end to end 39.8 / 149.7 / 259.7 / 100.0, squelch
  entry VFO DoA 69-71 (70) vs 241-251 without the squelch. Not run: a Pi,
  real recordings, the Windows / glibc syntax checks (no MinGW / zig here)
