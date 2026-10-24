# Noise floor (the spectrum's 0 dB)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Noise floor = the spectrum's 0 dB (`src/signal_processing/noise_floor.cpp`, `nfloor::`):**
- Every tuner channel's spectrum is drawn, FFT-squelched (`check_squelch`,
  `get_range_peak_db` -> `check_squelch_in_range`) and scanned (continuous
  scanner) relative to ONE floor per channel: `FFTProcessor::
  channel_noise_floor(ch, data)`. It was each caller's own 10th percentile
  of the centre 80 % of the bins (`calculate_noise_floor`) - a signal wider than the 2.4 MHz
  (DVB-T, LTE) fills those bins, so the percentile landed on it: drawn at
  ~0-10 dB (only its edge showed its height) and an FFT squelch inside it
  never opened (the user's report, 2026-10-14)
- `nfloor::on_frame` (FFT worker, under fft_mutex - after the EMA; worked
  out at most every `EVAL_MS` 50 per channel and on every retune: a
  percentile of 13k bins is ~0.1-0.2 ms on the Pi 5 and frames come ~146 / s
  - the squelch checks, which each took their own percentile per packet,
  read the cached floor now): f = the averaged spectrum's percentile (today's floor), r = the RAW
  frame's percentile + 10 log10(fft size) (independent of the averaging and
  the FFT size). The receiver's own floor is remembered per tuner channel and
  gain (`"ch|gain"`, from the 8091 header; AGC (-1) and calibration
  (`doa_is_calibrating`) = the frame's floor, nothing learned):
  follows a lower r within ~0.5 s, a higher one 1 dB/s, nothing learned
  `SETTLE_MS` 600 after a retune; a tuner with none takes the others' at
  that gain. HELD (the band's floor is a signal) -> floor = f - (r - ref):
  the averaged spectrum's percentile stands as far above its noise as the
  frame's (a band-filling signal fluctuates like noise). Not held = f,
  exactly as before
- When is it held? Coherent mode: the COHERENCE TEST - receiver noise is
  independent per antenna, a signal isn't. `measure_coherence` (decimation
  thread, the FFT channel + any other antenna of the same packet, every
  `MSC_PERIOD_MS` 250; the receiver converts a second channel for it when
  DoA is off - `coherence_due()`): Welch 256-point Hann segments (64 per
  16384-sample packet), magnitude-squared coherence per bin, the mean over
  the quietest quarter of the centre 80 % (no DC) -> >= `MSC_SIGNAL` 0.2 =
  signal (noise ~1/64; a signal at SNR s (s/(1+s))^2), EMA 0.5, valid 2 s
  and only after the last retune. Signal: held once r > ref + 1 dB, ref
  never rises (a day on DVB-T doesn't wear it away); noise: not held, ref
  follows (a real rise - active antenna, interference - is learned at once).
  Independent mode or no fresh verdict: held when r > ref + 6 dB (released
  < 4), ref creeps up 1 dB a minute while held (a real +10 dB rise shows as
  ~10 dB noise for minutes there)
- WIDEBAND SCAN MODE (the stitched spectrum; 2026-10-20/21, the user's
  reports "wideband signals seem to disappear over the boundaries", then
  "noise floors don't align between tuners, some show a much higher signal
  level, a tuner's level glitches up and down before settling"): the stitch
  (`FFTProcessor::stitch_wideband_fft`) normalised every tuner to its own
  per-frame percentile, so a DVB-T / LTE over several tuners was drawn at
  full height in the partly covered tuners and at ~0 dB in the ones it
  filled, and a whole-array second normalisation (`calculate_noise_floor` on
  the stitched array: the first 13k bins only) shifted everything down when
  tuner 0 held a signal. Now the stitch is one grid of FFT bins (each
  frequency from the tuner whose centre is nearest, boundaries half way
  between the centres; no sort of 65k pairs per tick), each tuner's AVERAGED
  spectrum minus the common shape (`nfloor::shape_db`) minus
  `channel_noise_floor(t)` (the floor its VFOs' squelch reads too), built
  from `fft_averaged` - every frame averaged as in coherent mode; the old
  second EMA at the 20 Hz tick, its globals (`wideband_fft_magnitudes /
  _averaged / _mutex`, `wideband_last_reset_gen`,
  `wideband_reference_noise_floor`) and the second normalisation are gone;
  while `fft_data_valid` is false the tick sends nothing (the pages keep the
  last frame - it used to flash the single-tuner display)
- What the live KrakenSDR showed (a read-only 8091 capture, 593-601 MHz,
  a DVB-T mux over tuners 3-4): the dongles' noise floors differ by up to
  4.4 dB at the same gain; every tuner's response has the SAME shape (a
  ~3.5 dB tilt across the usable band, a slow roll-off below -1 MHz, a steep
  one above +1 MHz - three quiet tuners within ~0.3 dB); the DVB-T fades ~13
  dB across a tuner (multipath). A first version (commit bfaa5bf) derived a
  filled tuner's floor from p10s of 2 % edge windows at the boundaries - on
  the hardware those sit in the roll-off, 2.4-3.3 dB apart per boundary, so
  chains of them drew tuners several dB too high - and used the remembered
  floors (refs, per tuner + gain, frequency-blind: 7-19 dB off there) with
  the 6 dB rule, which held tuners and drew their noise up to +19 dB. The
  same capture replayed through it: noise of a tuner jumping 1.9 -> 7.9 dB,
  the DVB-T 25.0 / 29.6 dB in tuners 3 / 4, up to 7.9 dB of wander
- Now (`wideband_frame`, `update_step`, `wideband_pass`, under `mu`; no refs
  used or learned in wideband scan mode - the coherent / independent path is
  main's, byte for byte): (1) the common SHAPE (`WB_SHAPE_DEFAULT`, 256
  points over 2.4 MHz, measured on that KrakenSDR = the median of three
  quiet tuners, 5-point smoothed; GENERATED - shape_default.txt in the job
  scratch; a hand-typed copy once differed by 3 dB) is taken off every
  tuner; each tuner's floor f = p10 of its flattened centre 80 %. (2) GAIN
  STEPS from the OVERLAPS: neighbours (each 2.4 MHz, spaced by edge clip x
  2.4) share 0.48 MHz; at the same frequencies their flattened averaged
  spectra differ only by gain, whatever the band holds - the median of the
  difference over the bins where both responses are > `WB_OVERLAP_MIN_DB`
  -6 (>= 64 bins; ~2000 at 16384), smoothed `WB_STEP_TAU_S` 1 s. Live:
  +4.06 / +0.02 between quiet tuners whose floors differ +4.36 / -0.08. A
  floor raised in one tuner only (overload, its antenna) is in the overlap
  too: a gain step, so that tuner stays aligned. The median (not a quantile
  difference) because it compares identical frequencies (DVB-T ripple) and
  tolerates a station aliasing into one side. A step is the two dongles'
  gain difference: kept PER PAIR of tuners (`Wb::step[j]`) through retunes,
  replaced by the next measurement after a move > `WB_STEP_RESET_HZ` 2 MHz.
  (3) Tuners adjacent by frequency with a step for the pair are linked
  (also while they don't overlap - see the drag below); along a chain common
  = min(f - g) = each tuner's receiver floor + g; over = f - g - common, and
  the floor BLENDS from f (over <= `WB_BLEND_LO_DB` 1: quiet tuners' noise
  lines up exactly) to common + g (over >= `WB_BLEND_HI_DB` 3: filled by a
  signal), linearly between - no switching, no state (`Wb::blend`). (4) The
  shape learned from quiet tuners (blend 0, flattened p50 - p10 < noise's +
  1.5 dB - `noise_spread_db()`: 8.2 dB for one frame, x sqrt(alpha / (2 -
  alpha)), 2.27 at 0.15 - every 1 s, points within 3 dB, tau 60 s; FFTs >=
  4096; not persisted) - a 2 dB-steeper tilt is learned within 60 s. (5)
  After a retune (> 1 kHz) only the step and shape MEASUREMENTS wait, until
  the frames from before it weigh < 5 % in the average (`WB_SETTLE_OLD`:
  ln 0.05 / ln(1 - alpha) frames x the measured frame interval = 3 time
  constants, 125 ms at alpha 0.15; 30 ms - 4 s); the floors follow at once.
  It was 5 time constants and at least 300 ms (the user's "speed up the
  equalization", 2026-10-21): after a jump to where the dongles' gains differ
  otherwise, the old steps held the tuners apart that long. A retune is when
  the AVERAGED spectrum moved: `on_frame` gets the RF its centre holds (the
  shift's `placed_hz`) on every frame (`Place`: before the `EVAL_MS` return),
  and the steps compare the neighbours at those - a tuner's header frequency
  (`tuner_frequencies`) changes before its FFT worker moves the average, and
  a step measured in that moment came from two different bands (with the
  shorter wait steps are measured during fast drags, and the DVB-T's edges
  were drawn 0.1-0.5 MHz off in 5 % of the frames). (6)
  `FFTProcessor::process_channel_fft` shifts a retuned tuner's average by
  the move (wideband scan mode; whole bins, the rest carried; bins
  the old band didn't hold keep their old values; a move past the band: no
  shift) - the 8091 header frequency of the packet rides `RawDataPacket::
  freq_hz` -> `FFTWorkItem::freq_hz` / `seq` (an older packet processed late
  by another FFT worker doesn't move it back)
- DRAGS (2026-10-21, the user's "dragging is laggy and glitchy, it takes
  time to equalize after a retune"): heimdall's wideband scan retune
  (`setup_wideband_frequencies`) is the full `rtlsdr_set_center_freq` per
  tuner - not the coherent drag fast path -, one by one on Windows (~110 ms
  for 5), each tuner's 8091 header frequency flipping as it finishes, no
  retuning flag, no cooldown; 8092 coalesces queued `set_frequency`s. The
  first version froze the fill decisions for the settle time after every
  retune (renewed by every drag step) and dropped them after 1 MHz: a
  DVB-T drawn at 0 dB for most of a drag, 0.4-3.6 s to recover. Steps were
  dropped when a neighbour went out of reach: in a fast drag (~0.5 MHz per
  sweep) a moved tuner and the next unmoved one are > 2.4 MHz apart for a
  moment - a chain of tuners all inside the signal, no quiet one, the
  signal at 0 dB for that frame. And the average kept the old band shifted
  with the tuner until it decayed
- Limits: a signal wider than all the tuners (or a rise on every tuner)
  leaves no quiet tuner - drawn as the floor (nothing remembered in this
  mode); a tuner's step to a neighbour it never overlapped quietly is still
  measured (any content works); a response unlike the default shape gives
  wrong steps until learned (~1 min, partial blends meanwhile); the
  squelch reads the flattened floor against the unflattened spectrum
  (<= ~2 dB at the band edges); a few frames after a retune carry old
  samples under the new header frequency (heimdall's pipeline) - averaged
  in at the new place with weight alpha each. heimdall's wideband scan
  retunes take the drag fast path since 2026-10-21 (Windows 114 -> 74 ms per
  step on the KrakenSDR - heimdall_v2/CLAUDE.md *Drag retunes*). Not done:
  the page panning locally while dragging (it waits for frames: the display
  follows the receiver - 140-210 ms behind in the drag test, modelled on the
  full path's ~110 ms sweep; the fast path's 40 ms less per step not measured
  end to end)
- `noise_floors.json` (cwd, gitignored; `{"version":1,"floors":{"0|49.6":
  -27.49,...}}`): loaded at start (`nfloor::load`), written by the FFT
  worker at most every 5 min when a floor moved >= 0.5 dB
  (`maybe_save`) and at shutdown (`save_now`), so a start on DVB-T is right
  at once. Not touched by RESET_SETTINGS
- Not covered: the beamformed-FFT squelch (beamforming on: its own narrow
  spectrum's percentile, `check_squelch_beamformed`)
- Tests: unit (scratch t_nfloor.cpp, `-DNFLOOR_TEST_CLOCK` = a test clock,
  minutes in no time) - noise = the old floor exactly; coherent +20 dB
  band-filling shown at +20 for 10 min; a +10 dB uncorrelated rise learned
  at once; independent: +20 held, creeping to +10 in 10 min; retune onto the
  signal right within 0.5 s; AGC / calibration = old; FFT 8192; another
  tuner taking tuner 0's floor; a strong carrier in noise (verdict noise);
  the file round trip. End to end (scratch e2e/fake_heimdall_floor.py +
  t_floor.py, a test build on ports 4708x): 5 coherent channels, phases
  noise / a band-filling signal 20 dB over it, the same in every antenna /
  noise 10 dB up, independent per antenna / noise / the signal again, each a
  retune; VFO 0 FFT squelch 10 dB. The band's median as drawn (noise's own:
  2.1-2.4 dB): before 2.2 dB on the signal, squelch never open; now 21.6-
  21.9 dB, squelch open 100 % (DoA off for 12 s of it - the second channel
  converted for the test), the +10 dB noise at 0 dB after ~1 s (no verdict
  for the first frames after the retune), squelch closed. kraken_doa CPU the
  same (50-55 vs 52 % of a core)
- Tests, wideband scan (job scratch 0d0a7cca, not in the repo): REPLAY
  (t_wbreplay.cpp: a live 8091 capture, cap8091.py, through the client's
  own process_channel_fft + nfloor + stitch at its packet rate, alpha 0.15,
  60 s): noise 2.15 / 2.18 / 2.10 / 2.35 dB in tuners 0-3 (the last one's
  only noise beside the DVB-T), the DVB-T 22.4 dB in tuner 3, 22.1 just past
  the 3 | 4 boundary (tuner 4's 24.3 is the mux's own multipath shape), each
  level wandering <= 1 dB (alpha 0.15's own noise); the first version on the
  same replay: see above. UNIT (t_wbfloor.cpp, synthetic 5-tuner spectra
  with the default shape + per-dongle deviations of +-0.3 dB, the live
  gains, alpha 0.15 noise; 13 scenarios): noise aligned within 0.05 dB and
  flat; DVB-T +20 / +5 dB over tuners 1-3 equal in every tuner (<= 0.3 dB),
  the same after 30 min; a signal ending exactly at a boundary (the quiet
  tuner not lifted); +6 dB of antenna noise on one tuner; +12 dB on every
  tuner; carriers on boundaries; an FM-like band (200 kHz stations at +25-45
  dB over half of it) for 10 min, then gone - still flat; a gain change; a
  start on the signal; a pan of +1 MHz and back; a 2 dB-steeper response
  learned in 60 s; coherent mode = the old floor. END TO END (fake_wb.py =
  heimdall in wideband scan mode on 4809x with the measured shape + gains,
  t_wb_e2e.py reading type 7 frames in exact-bin windows from a test build
  on 48080): 20 / 20 - noise 2.35 dB in every tuner, DVB-T 21.2-22.1 dB in
  all five, ~20 dB over the noise, after a pan too, no tuner's level moving
  more than 0.47 dB (one display step) over 60 s, carriers on the 4
  boundaries; the main build 4.24 dB in tuners 1-3. kraken_doa CPU 7.7 vs
  9.9 s over 91 s (the stitch's sort gone). DRAG (fake_wbd.py = heimdall in
  wideband scan mode following each retune like the Windows one - tuners
  one by one, 22 ms each, header per tuner, data 8 ms after it, latest
  command wins; 16384-sample packets; a 7.6 MHz block 20 dB over the noise
  fixed in RF; t_wb_drag.py plays drags as WIDEBAND_FREQ steps every 50 ms -
  600 -> 604 MHz in 50 kHz, back in 100 kHz, a 6 MHz jump, 606 -> 596 MHz in
  250 kHz = 5 MHz/s - and reads every type 7 frame of the whole band): the
  block over the noise during drags (median / worst frame) - main 0 / -2.8
  dB (never shown), the first version 0 / -1.4 dB (gone from 1 MHz into a
  drag), the blend 19.3 / 17.4 dB but the fast drag -0.5 dB (the chain
  break), now 19.3-19.8 / 16.5 dB; the tuners inside it within 1.4 dB of
  each other (p95; fast 1.9; the first version 18 dB); settled (3-frame
  median within 1 dB) 0 s after a drag, 0.25 s after the jump (first
  version 0.4-3.6 s); its edges drawn within 1.3 kHz of the truth in 95 %
  of the frames during drags (without the shift 100-250 kHz: the old band
  shifted); the display at the new frequency 140-210 ms after the last
  command in every build (the tuners' sweep + the 50 ms frame tick). JUMP
  (t_wb_jump.py + ana_jump.py: fake_wbd.py with the dongles' gains drifting
  apart 0.2-0.5 dB per 100 MHz; lined up = every tuner's noise and the
  signal parts within 1.5 dB, 3-frame median): the shorter settle wait
  (5 % rule) at alpha 0.15 - 293 MHz 0.14-0.31 s (5 time constants / >= 300
  ms: 0.45), back 300 MHz 0.25-0.61 s, most 0.30 (0.50), 6 / 3 MHz 0.20-0.25
  s (0.19-0.27); at 0.05 back 300 MHz 0.55-0.59 s (0.91), the rest the same
  (there the averaging itself is slower than the wait). Drags with it: the
  first version (header frequencies) drew the DVB-T's edges 139-459 kHz off
  in 5 % of the fast drag's frames (p95, 3 runs; 300 ms wait 1.3-59 kHz);
  with the averages' own frequencies 1.2-1.3 kHz, the rest as before (block
  16.9-18.8 dB over the noise at worst, spread p95 <= 1.9 dB)
