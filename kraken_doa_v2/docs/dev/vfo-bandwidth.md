# Arbitrary VFO bandwidth and the VFO decimator

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Arbitrary VFO bandwidth (the user's request 2026-10-21: "choose an arbitrary
bandwidth, while still keeping the decimation efficient"; config.hpp,
`src/signal_processing/shared_decimator.cpp`):**
- A VFO's bandwidth is ANY whole Hz, `VFO_BW_MIN_HZ` 1 kHz .. `VFO_BW_MAX_HZ`
  2.4 MHz: `DecimatorInstance::bandwidth_hz` (was `bandwidth_index`, an index
  into the 30 `BANDWIDTH_OPTIONS`), `DecimatorManager::setBandwidthHz` (clamps;
  `setBandwidthIndex` = an option's Hz). `clamp_bandwidth_hz` /
  `decimation_for_bandwidth` / `efficient_bandwidth_at_least` /
  `nearest_bandwidth_index` in config.hpp
- Decimation stays INTEGER (no resampler - every consumer works on
  2.4 MHz / D): D = the largest divisor of 2.4 MHz (2^8 3 5^5: 59 of them up
  to 2400 - `efficient_decimations()`) whose rate holds the width.
  `SharedDecimator::planStages(bw)` (static: the decimator,
  tools/decim_bench.cpp share it) splits D into stages of 2..8: an
  intermediate stage stops at its output rate - 0.4 bw (only what aliases
  onto the wanted +-0.4 bw must go), the LAST stage stops at 0.6 bw (pass
  0.4 bw, -6 dB at the bar's edge 0.5 bw) - a width narrower than its rate
  is cut AT THE WIDTH, so the VFO passes its bandwidth and no more; 1.2 -
  2.4 MHz = one stage of factor 1 (a full-rate filter). Kaiser 60 dB taps
  (`liquid_firdes_kaiser`; tap count 52 / (14.36 x transition)). Gain =
  each stage's 1 / (2 cutoff) = 2.4 MHz / bw for every width (= D for the
  efficient sizes, `bandwidth_is_efficient` - every old option is one: the
  int16 conversions downstream see the same levels)
- THE PLAN (2026-10-22, the user's pick of three measured proposals:
  "re-split D = 2-8 stages, fold the mixer into stage 1, halfband x2
  stages"): a cost model of the stage kernels (ns per output, fitted on the
  Pi 5 - job scratch explore/kern2.cpp + plan.py: regular 0.133 x window +
  1.13 + 0.14 x factor, folded 0.239 x window + 3.03 + 0.14 x factor,
  halfband = regular on its x phase at factor 1 + 0.3; per input sample the
  mixer 3.39, the mixer into a halfband's two phases 2.82, the copy a folded
  stage takes 0.52 - `R_TAP` ... in shared_decimator.cpp) picks the cheapest
  ORDERED split (memoised over the factor that remains: its input rate
  follows from it; ties = the larger factor first), per factor-2 stage
  regular or halfband, and for the first stage where the mixer goes.
  Measured against EVERY candidate plan of 28 widths (job scratch
  explore/allplans.cpp: 2-318 plans each, the real FirStage, VFO off
  centre): the model's pick is the fastest in 22, within 2 % in the other
  six (mean 0.1 %). It was largest factor first: D = 2-8 then ran ONE long
  sharp stage at 2.4 MHz (300 kHz: 145 taps at 8:1 - now 4:1 with 25 taps +
  a halfband 2:1 of 39). `Plan::describe()` = "8F:31 5:23 2h:39" (factor, F
  = the mixer folded in, h = halfband : taps); `Plan::cost_ns` = the model's
  estimate (within ~6 % of the measured on average)
- The stages: `SharedDecimator::FirStage`, a BLOCK FIR (2026-10-21; it
  replaced liquid's `firdecim_crcf`, itself the `cccf` with zero imaginary
  parts the decimator had before arbitrary widths - liquid copies EVERY
  input sample into its window with a call of its own, ~8.9 ns of the
  9.5-13 ns a sample cost, a multiply-accumulate ~0.14 ns): output j = the
  taps' dot product with the inputs ending at input j x M (liquid firdecim's
  alignment), straight from a buffer of history + the samples not used up
  yet (`SampleBuf`: grows without zeroing, drops from the front). The
  kernel (2026-10-22): the taps duplicated per I / Q float, padded to a
  multiple of 4 with leading zeros, FOUR outputs per pass sharing the tap
  loads, two outputs' sums combined by one shuffle into one store (GCC
  vector extensions - `__builtin_shufflevector` under clang - -> NEON / SSE;
  4 outputs per pass 10-20 % faster than 2). The mixer in front writes into
  the first stage's buffer, each stage's outputs go into the next one's; a
  block that isn't a multiple of a stage's factor leaves its tail in that
  stage. A block goes through the whole cascade in CHUNKS of `DECIM_CHUNK`
  1024 input samples (2026-10-22, decimateChannel), so every stage's buffer
  stays a chunk + its history long, in the core's L1 - see the
  measurements below for why. A regular first stage whose input isn't
  mixed in front (folded, or the VFO on the tuner centre - not a halfband,
  which splits its input) filters STRAIGHT FROM the input block
  (`FirStage::begin_input` / `pending_upto` / `produce_upto` /
  `end_input`, the user's go 2026-10-22): its history x gets only the
  block's first L - 1 samples, for the windows that reach back into it,
  every other window is computed from the block itself (across the chunks
  in place), and at the end x keeps what the next block's windows reach
  back to (<= L - 1 + M samples). Kinds:
  - regular: real taps
  - FOLDED (`Plan::fold`, the first stage only): the mixer in the taps -
    h[k] e^{+jwk} on the RAW input (copied in), each output turned by the
    mixer's phase at its newest sample (times e^{-jwM} per output,
    renormalised every 1024): the oscillator runs at the output rate.
    `FirStage::fold(w)` on every offset change (phase 1 at the next sample,
    as the mixer restarts; within 1 Hz of the centre: real taps); its
    history is raw, so it is mixed with the new offset at once (the mixer in
    front leaves the held samples mixed with the old). Pays where the first
    stage's taps per input are few: every width up to 240 kHz (factor 5-8
    first) - ~2 of the ~3.4 ns the mixer cost
  - HALFBAND (factor 2 cut at a quarter of its input rate: an intermediate
    factor 2, or the last when the width fills the rate): 4m+3 taps, those
    at even offsets from the centre set to exactly 0 (the sinc's zeros).
    The input kept as its two phases (`x` = the outputs' newest samples,
    `o` = the others), written so by its producer (`take()` -> `Dst` {a,
    b}: the mixer in front, the copy, or the previous stage's outputs -
    `put4` shuffles them apart): the x taps a plain FIR at the output rate +
    the centre tap times o[j] - ~40 % fewer multiply-adds (1.2 MHz with the
    mixer: 6.98 -> 5.86 ns)
- Checked (`tools/decim_bench.cpp`, Pi 5): every plan - the 59 efficient
  sizes + 38 arbitrary widths - fed blocks of odd lengths (1, 2, 3, 7, 333,
  12345 ...) vs a plain double-precision reference of the same plan (the
  mixer at the input rate, every stage a full convolution): max |diff| <=
  5.5e-7 of rms on the centre, <= 6.9e-5 off it (the float mixer's phase
  over 45k samples); response flat +-0.03 dB to 0.4 bw, -6.0..-6.2 dB at
  0.5 bw, -52..-72 dB from 0.6 bw, aliases <= -53 dB, the same with the VFO
  300 kHz off the centre (the folded path)
- `decimateMultiChannel(.., bandwidth_hz, offset)` (was the factor): the
  pipeline reads the width ONCE per block; the channel state rebuilds when
  the width changes (`ChannelDecimState::bandwidth_hz`), not only the factor.
  `MultiChannelDecimated::bandwidth_hz` = the block's width (<= its rate)
- The beamformed FFT (`process_beamformed_fft(.., bandwidth_hz)`) keeps only
  the bins within the width (a multiple of 8, `bandwidth_hz` = their span;
  the overlay message pools by powers of two that divide the count): with
  the filter's stopband in the rest - 48 % of the bins of a 1.25 MHz VFO at
  2.4 MS/s - its 10th-percentile noise floor fell 60 dB and the beamformed
  FFT squelch opened on noise
- Protocol: `SET_DECIMATOR_BW_HZ:id:hz` (clamped, `set_applied`; a bad value
  throws = nothing changes), `SET_DECIMATOR_BW:id:index` (older pages /
  scripts); the echo `{"decimator_bandwidth":{id, bandwidth_index (the
  nearest option), bandwidth_hz, decimation}}`; `decimator_info[]` carries
  `bandwidth_hz`, `bandwidth_index` (nearest), `bandwidth_mhz` (= hz / 1e6,
  was the option's), `decimation`; DEMOD_MODE's reply adds `bandwidth_hz`.
  The multi-DoA binary message keeps its u32 index (nearest; unused by the
  page). DECIMATORS snapshot: field 2 = the nearest index (an older
  kraken_doa reads it), a new LAST field (12) = the width in Hz; a file
  without it loads by the index. Exact Hz now also from: the web mapper's
  `vfo_bw_N` (both ways - it was snapped to an option), the discrete
  scanner's per-frequency kHz, the debug playback's `vfo_bw_khz` (every
  catalogue value is an old option: unchanged), the AI assistant's
  `set_vfo` / `add_vfo` `bandwidth_hz`; a decoder's minimum
  (`min_vfo_rate`) snaps UP to an efficient size
  (`efficient_bandwidth_at_least` / the page's `bwAtLeastEfficient`: ADS-B's
  2 MHz = the 2.4 MHz pass-through, not a 2 MHz full-rate filter)
- Page (kraken_doa.html, the block after `bwo`): `decBwHz(d)` / `decBwMhz(d)`
  = the ONE way to read a VFO's width (bandwidth_hz, else an older
  receiver's index); `setDecimatorBandwidthHz` (sends at most every 80 ms +
  the last value - a bar edge drag; `bwHeld` 1.5 s: the receiver's echoes of
  earlier steps don't pull the bar back, in `updateDecimatorList` and the
  `decimator_bandwidth` handler) / `setDecimatorBandwidthHzFromUI`; the old
  index functions wrap them; an older receiver (no `bandwidth_hz` in its
  decimator_info, `bwServerHz`) gets the nearest index. Bar edge drags (main
  display `pointerMove`, panes `paneMove`) give `bwUserHz` = 3 significant
  figures (a pixel is ~0.3 % of a bar) and keep the offset inside the band as
  the bar grows; the tooltip adds "(runs at 300 kS/s)" for a custom width.
  The card's BW row: ONE number box in kHz (the user's request 2026-10-21 -
  first it was a list of the 59 `BW_EFFICIENT` sizes + "(custom)" and a ✎
  text field): `<input type=number id=dec-bw-<id> min 1 max 2400 step any>`;
  its change event (Enter = blur, leaving it, the spinner / arrow keys - 1
  kHz steps) -> `bwBoxApply` (clamped; empty / not a number = back to the
  VFO's width), Escape = the value it had on focus; `bwBoxSync` keeps it on
  the VFO's width (not while focused, unless forced) and its tooltip
  (`bwTitle`: what rate the width runs at, x its width); locked VFOs (fixed
  / lock_vfo decoders) disable it. Saved frequencies: the editor's list =
  the efficient sizes + an entry's custom width (`sfBwSelectSet`), tuning
  applies the saved width exactly
- No snapping option: an "Efficient bandwidths only" tick (snap every width
  to `BW_EFFICIENT`) was built because the request asked for one "if
  significant efficiency is lost", then REMOVED the same day (the user's
  call): with the real-tap stages every width up to 1.2 MHz costs no more
  than the nearest old size did (below); only 1.2 - 2.4 MHz costs more (a
  full-rate filter: since the plans / kernel of 2026-10-22 +~9 % of a core
  with 5 antennas over the 2.4 MHz pass-through, +~30 over 1.2 MHz; it was
  +~15-20 with the first block FIR) - the BW box's tooltip says what a
  custom width runs at
- Measured on the Pi 5 (2026-10-21; `tools/decim_bench.cpp`, one core, ns
  per input sample per antenna, the cascade without the mixer): old sizes
  liquid cccf (the original) 10.7-25.5, liquid crcf 9.4-17.8, block FIR
  2.6-7.9 (means 13.17 / 10.74 / 3.97: the block FIR 3.3x the original,
  2.7x the crcf); the mixer ~2.9 more. Arbitrary widths (block FIR): 1 kHz -
  240 kHz 2.6-4.8, 250 kHz - 1.2 MHz 6.8-9.9 (D = 2-8: one long sharp
  stage - multiply-bound now; a re-split would help, see below), 1.2 - 2.4
  MHz 10.3-15.2 (the full-rate filter; liquid crcf 24-29) vs 0.33 (2.4 MHz
  pass-through) / 8.0 (1.2 MHz); within 0.86-1.2x the nearest old size up
  to 240 kHz. The rate is <= 1.2x the width from 300 kHz down, <= 1.35x to
  1.2 MHz, 1.2-1.9x above. Response: flat +-0.04 dB to 0.4 bw, -6.0 dB at
  0.5 bw, -52..-64 dB from 0.6 bw, aliases <= -53 dB, gain 0.00 dB re 2.4
  MHz / bw. Whole kraken_doa (test build, fake heimdall, one VFO, DoA on, 5
  antennas, % of a core; the original / liquid crcf / block FIR builds
  alternately, two runs each, the old index command): 240 kHz 50.1 / 47.0 /
  38.9, 300 kHz 49.0 / 45.2 / 41.6, 12 kHz 36.4 / 35.1 / 30.1, 16 kHz 38.5 /
  36.6 / 30.3, 1.2 MHz 87.4 / 77.0 / 65.6, 2.4 MHz 81.3 / 81.7 / 76.5 (no
  filter: the mixer's own copy is gone) - the block FIR build 6-25 % below
  the original at every old size. Custom widths (block FIR build): 250 kHz
  42.6 (300 kHz 41.5), 13 / 15 kHz 30.3 / 30.2 (12.5 kHz 30.2), 1.25 / 1.5
  / 2 MHz 100.4 / 96.8 / 95.3 (liquid crcf build 118.3 / 117.2 / 112.5;
  1.2 MHz 69.7, 2.4 MHz 80.9)
- Measured 2026-10-22 (Pi 5, the plans + folded / halfband stages + the
  4-output kernel vs the block FIR build above): the decimator
  (decimateChannel, one antenna, ns per input sample; job scratch
  explore/cmpbench.cpp, the same file built against both, two runs each)
  efficient sizes mean 4.35 -> 2.02 on the tuner centre, 6.67 -> 3.31 off
  it (the mixer included) - 2.0x; <= 240 kHz 2.7-4.9 -> 1.3-2.2 (off
  centre 5.0-7.3 -> 2.1-3.5), 250 kHz - 1.2 MHz 6.8-9.4 -> 2.3-4.7 (9.1-11.8
  -> 3.9-7.2), the full-rate widths 12.2-16.0 -> 5.4-7.3 (14.6-18.3 ->
  7.9-9.8); 2.4 MHz (the mixer alone) 2.97 -> 3.24 (the same loop compiled
  differently; `__restrict` didn't help and slowed the split mixer, not
  kept). Whole kraken_doa (as above, the block FIR / this build
  alternately, two runs each, VFO +100 kHz): 240 kHz 39.0 -> 35.2, 300 kHz
  41.5 -> 38.2, 12 kHz 30.4 -> 26.9, 16 kHz 30.6 -> 27.4, 1.2 MHz 67.1 ->
  60.4, 2.4 MHz 77.0 -> 76.6 % of a core; custom widths (one sweep each)
  250 kHz 43.2 -> 37.7, 13 / 15 kHz 29.5 / 30.0 -> 27.7 / 27.6, 1.25 / 1.5
  / 2 MHz 103.0 / 100.5 / 98.3 -> 90.6 / 90.2 / 90.2. The decimator is now
  a small part of a VFO's cost (MUSIC, the FFTs, the IQ conversion are the
  rest). clang 19 builds the kernels too (`__builtin_shufflevector`): the
  same checks, the same speed. Not done: a folded halfband first stage
  (complex taps on the x phase: the model gives ~0.2 ns at best - the
  mixer into the two phases is already cheaper than the interleaved one)
- Profiled in the live pipeline (2026-10-22, the user's question "any other
  optimizations for the VFOs?"; perf - `linux-perf`, installed on the Pi
  with the user's go - on the test build + fake heimdall, job scratch
  e2e/t_prof.py: phases nothing decimated / one VFO without DoA / with DoA
  / 2 and 3 VFOs / 240 kHz / beamforming, total + per-thread CPU): a 12.5
  kHz VFO with DoA and 5 antennas cost ~19 % of a core EACH (1 / 2 / 3
  VFOs 28 / 47 / 65 %), its decimation 2.6 % of that in decim_bench - in
  the pipeline the first stage was MEMORY-bound: a memcpy (libc's 64-byte
  loop; the raw block copied into the folded first stage) was 30-40 % of
  the samples - 8 % of a core for one VFO, 19 % for two, ~1.5 GB/s (each
  VFO's 128 KB per antenna per block, written on other cores than the
  converter's, the VFOs contending) - and `produce` ran at ~5 ns per input
  sample against 2.2 in decim_bench (reading the 128 KB buffer back
  cold). Chunking (above) keeps both in L1: measured with 512 / 1024 / 2048
  / 4096 / 16384 (the old whole block) - 512 and 1024 the same, 2048 +1-2 %
  of a core, 4096 +2-7, the whole block +8-30. Whole kraken_doa, the
  previous build (5d3fe16 + 8f4e30c) / this one (chunked cascade + the fast
  dB, *FFT Processing* below) alternately, two runs each, % of a core:
  nothing decimated 9.5 -> 7.8 (the fast dB), one VFO without DoA 12.7 ->
  10.3, one VFO with DoA 28.5 -> 18.5, two 48.7 -> 26.2, three 66.8 ->
  32.3, 240 kHz 37.1 -> 26.4, DoA + beamforming 28.9 -> 19.1 - an extra
  narrow VFO ~7 % of a core instead of ~19. decim_bench (hot caches) mean
  1.69 -> 1.59 ns on centre; every check against the reference unchanged
  (<= 7.1e-5 of rms, blocks of 1 - 16384 samples across the chunk edges).
  What was left with two VFOs (perf, share of the samples / of a core):
  `produce` ~24 % / ~7.6 %, the copy into a folded first stage still ~19 %
  / ~6 %. Filtering straight from the block (above) removed the copy
  completely (perf: no memcpy left) but saved less than its share: the
  filter now pays the cold read of the block the copy paid (`run` 40 % /
  ~10.8 % of a core with two VFOs) - chunked / direct, two runs each, % of
  a core: one VFO 19.0 -> 18.1, two 26.5 -> 25.6, three 32.5 -> 30.7, 240
  kHz 26.6 -> 26.0, DoA + beamforming 19.4 -> 18.7; decim_bench (hot) mean
  1.59 -> 1.50 ns on centre, 2.82 -> 2.68 off it; every check against the
  reference unchanged (7.1e-5 of rms; block lengths now also 30, 31, 63 -
  65, 145, 146, 1023 - 1025: windows reaching back into the history, the
  chunk edges). Over the day (the same harness, separate runs): one VFO
  28.5 -> 18.1, three 66.8 -> 30.7 % of a core. What is left is mostly
  each VFO's read of every antenna's input (5 x 19.2 MB/s): VFOs sharing
  one pass over a chunk per antenna (every VFO's first stage while it is
  in L1) would cut that - the restructure below; then the spectrum FFT,
  the IQ conversion of the 5 antennas, the receiver thread
- Estimated before deciding (the user's question 2026-10-22; job scratch
  explore/share_exp.cpp: the production decimator, a converter writing
  fresh 5-antenna blocks, three shapes on the Pi): VFOs sharing one pass
  per chunk would save ~0.4-0.8 % of a core per extra VFO and nothing for
  one VFO - the live L2 / L3 refill counters (perf, `run` per VFO) show
  the extra VFOs already find the input in L3, the case the synthetic
  test measures; ~half of that (nearly all at 240 kHz) was from ONE
  thread pool for every VFO instead of one per VFO. First the shared pool
  (the user's go) - and live it saved NOTHING measurable:
  user-space cycles (`perf stat -e cycles:u`, frequency-independent - the
  Pi's ondemand governor made CPU time swing by up to 7 points between
  runs of one build; job scratch e2e/t_prof_cyc.py), 4 runs each, % of a
  core at 2.4 GHz, per-VFO pools / shared 4 / shared 5 threads: one 12.5
  kHz VFO 14.7 / 15.2 / 14.4, three 23.9 / 23.8 / 23.9, three at 240 kHz
  50.9 / 52.3 / 52.8 (each 47-55 between runs), one at 240 kHz 22.1 / 22.7
  / 21.9. Kept for the bounded thread count (51 -> 32 with three VFOs; 8
  more per VFO before), 5 threads (cores + 1) for the single-VFO figures.
  The raw buffer dropped no packet in any of the day's runs (175 reports)
- THE SHARED PASS (2026-10-22, the user's "do the pipeline restructure
  now"): `SharedDecimator::decimateVfos(VfoBlock*, n)` takes every VFO's
  block (its input pointers, the width and offset read once, where its
  `MultiChannelDecimated` goes): a task per distinct input ARRAY - coherent
  = an antenna with every VFO on it, independent = a tuner with its VFOs,
  FM only = the one channel - the first on the calling thread, the others
  on `decimation_pool()`; each runs its VFOs' `ChannelPass`es (decimateChannel
  split into begin / chunk(at) / finish, the per-channel state as before)
  chunk by chunk, every VFO on a 1024-sample chunk while it is in L1.
  `decimateMultiChannel` = decimateVfos with one VFO (the single-VFO work
  unchanged: the same calls in the same order). data_receiver.cpp: the
  inputs of every VFO, ONE decimateVfos, then per VFO its stream position,
  squelch, MUSIC and talker DoA (`run_pipeline(v)`, the old code after the
  decimation, moved as it was) - VFO 0 inline + the rest on the 3-thread
  VFO pool; FM only and independent / wideband (one channel each, MUSIC
  returns at once) all inline. A VFO's MUSIC no longer starts before every
  VFO is decimated (the pass is short). Checked: decim_bench section 3 -
  8 VFOs of every kind (folded, mixer in front, halfband first, the whole
  band, full-rate filter, two on one antenna alone) x 4 antennas in one
  pass vs each VFO alone, blocks of 1 - 16384: BIT FOR BIT equal (max diff
  0, Pi + x86); decim_bench's reference checks and single-channel timings
  unchanged (7.06e-5 of rms; mean 1.503 / 2.675 ns centre / off); t_bw 24 /
  24, t_bw_ui 24 / 24, job scratch e2e/t_multi.py 8 / 8 (five VFOs in one
  pass: 60.0 / 200.0 / 60.0 / 200.0 deg exact, the 1.5 MHz one running;
  back to one VFO; FM only with three VFOs heard; no decimation errors -
  the previous build the same). Measured (user-space cycles, % of a core
  at 2.4 GHz, e2e/t_prof_cyc2.py, the previous build / the shared pass
  alternately - TWO runs each: the A/B's monitor was stopped half way by a
  low-memory reap on the dev PC, not re-run): one 12.5 kHz VFO 14.3 / 14.6
  -> 14.8 / 14.7 (within that phase's 14.2-16.0 spread over the day's
  builds), three 24.3 / 23.9 -> 20.1 / 20.2 (the earlier builds' 12 runs
  22.9-24.6), five 30.4 / 30.2 -> 26.6 / 26.3, five at 240 kHz 90.9 /
  86.4 -> 85.8 / 84.7, one at 240 kHz 22.3 / 21.7 -> 21.9 / 22.2 - ~4
  points less with 3 or 5 narrow VFOs (more than the estimate's 0.8-1.6
  with three; why was not measured), one VFO the same. Thread count
  unchanged (29 / 32)
- Tests (job scratch 415cedbd e2e/, on the Pi: `fake_heimdall_bw.py` = a
  5-element UCA scene, tone A +100 kHz from 60 deg, tone B +125 kHz from
  200 deg; `t_bw.py` against a TEST build on 5808x / 5809x): 24 / 24 -
  command / echo / decimator_info, clamps, a bad value, the old index,
  bearings 60.0 / 200.0 in 26 / 37 / 41 / 10.5 kHz VFOs with the other tone
  25 kHz away filtered out, both tones in a 77 kHz VFO, the beamformed
  overlay 1.5000 / 0.2508 MHz, the beamformed squelch closed on noise in a
  1.25 MHz VFO and open on the tones, the snapshot's field 12, a restart, an
  older snapshot by index; `t_bw_ui.py` (headless Chromium, real mouse /
  keys): 24 / 24 - the BW number box (typed 26 / 12.5 / 1500 + Enter, the
  tooltip's rate, letters and Escape change nothing, arrow up = +1 kHz, 5000
  clamped to 2400), a main-display edge drag to an arbitrary width the
  receiver keeps (the box follows), no snapping tick, a second drag
  arbitrary too, independent mode's 2.4 MHz bar (both
  clipped edges grabbable, resize cursor, dragged in and back out, below),
  no page exceptions. Both again 24 / 24 / 24 / 24 on the plans / folded /
  halfband build (2026-10-22: the bearings through folded first stages at
  10 - 41 kHz, 100 kHz off centre, exact). A width change at the same rate
  clears MUSIC's accumulator (`last_input_bw_hz_`: the rebuilt filters'
  restart is no signal), as a rate change always did
- THE MIXER AS A TABLE (2026-10-26, performance pass; `mix_into`,
  `SharedDecimator::Oscillator`): the mixer in front of the first stage
  (widths above ~240 kHz off the tuner centre, the whole band, a halfband
  first stage) was one float phasor times e^{-jw} per sample - a
  loop-carried complex multiply, latency-bound (`MIX_NS` 3.39 ns on the Pi
  5). Now its phase is kept in double and per call (<= 1024 samples) the
  oscillator is made in a table in 8 interleaved lanes (sample i = sample
  i - 8 times e^{-8jw}: dependence distance 8, a plain loop both
  vectorizers handle), then x times the table. On a 2015 MacBook Air
  (i5-5350U, Apple clang 14; job scratch doa-dsp/mixbench.cpp, interleaved,
  median of 9): the mixer 4.6-4.8 -> 1.1-1.4 ns a sample (3.5-4x), into a
  halfband's two phases 4.1 -> 1.5 (2.7x); the pipeline's `decimateVfos`
  (doa-dsp/ab_decim: 5 antennas x 16384, process CPU, two runs of 7) 2.4
  MHz +250 kHz 6.0-6.3 -> 2.3-3.1 ns per antenna sample, 1.2 MHz 7.8-8.8
  -> 5.3-6.3, 1.5 MHz 12.7-15.4 -> 8.7-11.2; the folded plans (<= 240 kHz)
  unchanged, bit for bit. More accurate - the phase is exact at every call,
  the float phasor drifted (vs a double reference over 786k samples 1.4e-4
  -> 5.7e-6 of rms; `decim_bench`'s worst check 6.9e-5 -> 1.2e-5 of rms,
  its reference now the exact w for a mixer in front); the response table
  identical, decimateVfos still bit for bit. The absolute phase differs
  from the old mixer's by that drift (up to 3e-2 of rms after 786k samples
  of a 2.4 MHz VFO); every channel of a VFO runs the same arithmetic, so
  their relative phases are as before. Pi / Windows not measured (the old
  loop is latency-bound on the A76 as well). NOT changed: the plan's cost
  model (`MIX_NS` / `MIX_SPLIT_NS`, fitted to the old mixer) - with a cheap
  mixer every folded plan moves to the mixer in front, which on the
  MacBook was 5-15 % SLOWER at 12.5 / 240 kHz (the folded stage filtering
  straight from the block saves a pass): refit on the Pi before changing
  it. FMA (`-mno-fma` vs the build's own; Apple clang's `-ffast-math`
  contracts even with `-ffp-contract=off` after it - the same FMA count):
  the narrow plans' FIR kernels 1.3x faster with FMAs (12.5 / 240 kHz 4.9
  / 4.7 -> 3.5 / 3.6 ns), the mixer and the full band unchanged. GCC contracts
  `a*b+c` by default in C++ (its `-ffp-contract=off` default is for ISO C
  only - the GCC manual), so the Pi build should already have them in
  `fir_real` / `fir_cplx`; `objdump -d obj/shared_decimator.o | grep -c
  fmla` on the Pi confirms it (0 would make explicit FMAs there worth ~30 %
  of the narrow VFOs' decimation)

