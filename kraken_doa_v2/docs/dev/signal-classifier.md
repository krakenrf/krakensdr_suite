# Signal ID: the signal classifier (`SIGCLASS:`, `{"sigclass"}`)

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`).

With **Signal ID** on, kraken_doa labels every signal in the live spectrum
with its type ("DMR", "POCSAG", "NFM voice", "LoRa", "ADS-B", "DAB" ... the
classes of the model) at a few percent of one core. The core (detection,
features, a small MLP per view, the tracker) is `include/sigclass/sigclass.hpp`
+ `src/sigclass/`; the runtime around it is `src/sigclass_service.cpp`
(`include/sigclass_service.hpp`, namespace `sigsvc`); the page's part is
kraken_doa.html "SIGNAL ID".

## Core

`include/sigclass/sigclass.hpp` (the only public header), `src/sigclass/`:
`analyzer.cpp` (the pipeline), `channelizer.cpp` (front end + views),
`detector.cpp`, `features.cpp`, `model.cpp`, `tracker.cpp`. C++20 + pffft,
no other dependency; the same code runs in kraken_doa and in
`tools/sigclass_tool.cpp` (training features = runtime features).

- **Front end**: one 0.5 s snapshot (1.2 M samples, uint8 -> (u - 127.5) /
  127.5, mean removed). Overlap-save blocks of LONG_N = 120000 (50 ms,
  2^6 x 3 x 5^4) with 2000 samples of reach each side; their Hann-windowed
  FFTs give the fine PSD (20 Hz bins) and are reused by the views. A short
  spectrogram (1024-point frames, hop 512 = `Config::spectrogram_hop`,
  427 us) for duty, max-hold and bursts
- **Detector** (`detector.cpp`, its header comment has the full rule list):
  base bins of 160 Hz; a frequency-dependent noise floor (low percentile per
  30-bin cell, bridged across signal cells, level-corrected); continuous
  detection at 1 + Z sigma (Z 6) over box widths 1-64 bins, regions split at
  deep valleys, AM / comb / FSK-tone merges, a narrow strong peak on a wide
  region split off; bursts the mean misses from the short spectrogram (box
  widths 1-256 short bins, Gamma thresholds, family-wise false-alarm rate
  0.01); then (2026-11-20, from the training data and the debug playback):
  - pieces of one burst signal: a bursty detection inside a bursty one >= 4x
    wider and within 3 dB of its max-hold is dropped (ADS-B / DME / UAT
    pulse spectra found piecewise)
  - burst splatter: a detection >= 6 dB weaker (max-hold), <= half as wide,
    within 3 widths (30-150 kHz) of a burst's band, whose short-frame power
    correlates > 0.6 with it - or a "continuous" one under 1 dB SNR and >= 20
    dB under the burst - is part of the burst (an OOK device's keying
    sidebands: ism-six-devices went from 16 to 11 tracks, the keying pieces
    of the Acurite / EV1527 devices)
  - wide signals: under a detection >= 0.6 fs or a band-filling one, narrow
    continuous lines go when there are >= 3 of them or they are < 6 dB, wider
    continuous bumps < 3 dB go; under a band-filling one also anything < 10
    dB and bursty lines (DVB-T continual pilots fade with SFN echoes; the
    echoes ripple the spectrum by 6-8 dB). Before: a DVB-T capture gave 32
    pilot detections of 60 Hz and NO wide one (the band-fill detection has
    peak 0 dB and the max_candidates cut dropped it - now it sorts first)
  - band-fill test (when nothing covers >= 0.6 fs): autocorrelation of the
    PSD with the detections replaced by the floor, lags 25 us - 2.5 ms (was 8
    us: an uneven real floor correlates smoothly at 8-13 us), corr >
    `band_fill_corr` 0.012 AND a peak >= `band_fill_prominence` 4 x the
    median of the lags within +-25 % of it. Real captures: a 2.4 MS/s
    HackRF / heimdall FM capture had 6 / 1 false fills in 6 / 8 snapshots
    (corr 0.013-0.019 at lag 20-29, prominence 1.5-3.4), now 0; real DVB-T
    (522 MHz) corr 0.045 at 2150 samples, prominence 30. Also a fill: >= 10
    narrow lines (>= 6 dB) spread over >= 0.94 x half the band, or on one
    regular spacing >= 5 kHz (a TV signal's line-rate comb; then every
    piece narrower than 50 kHz under it goes: analog TV from 27 tracks to 1)
  - (phase 3, from the real recordings) a burst hump around a continuous
    detection >= 3 dB stronger and at most 10x narrower than the hump is
    dropped: a 2.7 dB pedestal beside a real DMR channel came out as a 42 kHz
    "burst" that the tracker then merged the channel into ("AIS")
  - IQ image (2026-11-21, after the failure analysis): the tuner's gain /
    phase imbalance puts eps x conj(x(-f)) at f, 25-40 dB under the signal at
    -f; the image carries that signal's modulation and the model named it by
    its class (in the training data the largest group of confident wrong
    labels). A detection >= 20 kHz from the centre whose MIRRORED band (-f, its
    own width) holds >= 15 dB more power than its own band in the fine PSD is
    a candidate (was: the mirror of a detection >= 15 dB stronger in mean SNR,
    +-0.5 x its bandwidth - that missed the images of a strong signal's pilot
    lines, denser than its mean, and took a real P25 control channel 1.5 kHz
    off the mirror of a voice channel 27.7 dB stronger). The analyzer then
    confirms it: the candidate's view at f and the view at exactly -f (mirrored
    fine bins, same view), the samples where the mirrored side is on (>= 0.1 x
    its 90th-percentile power), the projection of the candidate's view onto
    conj(the mirrored view) = the image's energy; an image when that holds >=
    `Config::image_share` 0.1 of the view's energy over its noise (on the
    composer's IQ imbalance up to 0.5 dB / 3 deg: images median share ~1.0;
    independent signals at a mirror 0.002-0.01; the real control channel
    0.000 - kept, p25p1 1.00 with 8 hits). Candidates in views 2-3 (wider than
    43 kHz) go without the test, as before: a wide view holds other signals,
    whose energy hides the image's share. Image candidates do not count
    against `max_candidates` (the analyzer drops the confirmed ones, then
    cuts: otherwise busy captures lost real detections to image slots).
    Synthetic regression set (below): detections at a truth's mirror band
    1357 -> 820, false labels there 61 -> 30; `SIGCLASS_DEBUG_IMAGE=1` prints
    each candidate's share
  - an irregular comb of >= 10 lines forces the band-fill test only on its
    prominence (>= 4), no longer unconditionally; a regular comb (a TV line
    comb) still forces it. A real trunked-radio site (own heimdall capture,
    30-38 carriers / spurs over 2.25 MHz, prominence 1.4-2.5) had become one
    band-filling "DVB-T2" track that swallowed its P25 channels; synthetic
    OFDM combs that need the forced path have prominence 9-16
  - (phase 4) >= 16 bursty pieces <= 60 kHz wide spread over >= 0.6 fs
    (and nothing wide) run the band-fill test with the pieces' power KEPT
    (they are the signal) and a prominence >= 5: a lightly loaded LTE / NR
    carrier (own 768 / 875 MHz captures: 29-114 pieces a snapshot, before
    nothing but `none` pieces) becomes one band-filling detection
    (prominence 5-10 at the OFDM symbol lag); an SSR 1030 band reached 4.5,
    the busy LoRa band 2.8
  - limits: a crowded HF band merges into 100-500 kHz regions; a real LTE
    carrier at low load (own 768 MHz capture) comes out as a 930 kHz burst
    plus dozens of 16-23 kHz resource-block pieces and no fill (the
    floor-replaced residual holds too little of it); with sub-LSB noise
    (own captures, 0.6 LSB) RTL spur lines 1-6 dB near the band edge are
    detected (`none` for the model)
- **Views**: 0..3 at fs / 100, 25, 5, 1 (24 k / 96 k / 480 k / 2.4 MS/s),
  the smallest with rate >= bw / 0.45. Overlap-save channelizer: per long
  block the view's M = LONG_N / D bins x a zero-phase Kaiser low-pass (flat
  to 0.4 x the view rate, >= 60 dB from 0.5 x), an M-point inverse FFT,
  the block's mixer phase restored. Measured by the core's author: passband
  +0.01 dB to 0.39 x the view rate, out-of-view tones -89 / -106 / -132 dB
  (views 0 / 1 / 2). View 3 (no decimation) mixes by a block of exact table
  phasors x the block's start phasor (vectorised; was a table lookup per
  sample: 1.85 -> 1.25 ms per wide signal)
- **Second look in view 0** (`analyzer.cpp`, 2026-11-21): the trainer keeps
  a class in a view only with >= 100 examples there, so view 1's model (96
  kHz) has no 12.5 kHz class at all (DMR, P25, P25 Phase 2, NXDN, YSF,
  D-STAR, MPT1327, NFM voice: 46-96 view-1 examples each); a narrow channel
  measured a little wider than view 0's 10.8 kHz (a shoulder of its own
  transmitter, a neighbour 12.5 kHz away, a strong signal's skirt, a low-SNR
  width) cannot be named there. A view-1 detection at most
  `Config::second_view_max_hz` 40 kHz wide is therefore also cut and
  classified in view 0, its width clamped to view 0's 10.8 kHz (as view 0
  would have seen it: the meta features and the symbol group's channel filter
  take the channel's core), unless view 1 is already >= `second_view_skip_p`
  0.99 sure. View 0's answer is taken when its class is one view 1's model
  does not have (a class native to view 1 - TETRA, AIS, VDL2, pagers, wide
  FSK - cannot be taken from it, and the generic classes exist in both), >=
  `second_view_min_p` 0.5 and more probable than view 1's best. The
  detection then reports view 0 and view 0's probabilities (the tracker's
  evidence per view and the service's `class_info(view, best)` stay
  consistent) and keeps its band. Off when the caller takes the features
  (training rows stay those of the detection's own view) and for models
  without views 0 and 1. Synthetic regression set: +23 signals right (p25p2
  +4, nxdn, ysf, aprs, acars, am, ssb, wildlife +2 each ...), no class lost;
  cost: a second pass is a view-0 feature pass (~1.5 ms here): a busy own
  capture (142.3 MHz, 32 signals) 88-90 -> 98 ms a snapshot (5 second
  passes), one narrow or one wide signal unchanged (21.6 / 29.7 ms);
  40 kHz beat 20 kHz (+12), the clamp +3; on its crowded scenes 19.5 % of
  the detections (31 % before the 0.99 skip, which lost no takes) take the
  second pass, 0.27 % take its answer. `Analyzer::Timing::second_view` / `second_view_taken`;
  `sigclass_tool classify` adds `"second_view": [passes, taken]` per
  snapshot; `SIGCLASS_DEBUG_SV=1` prints each second look
- **Features** (`features.cpp`, layout v2, hash da962d2a45310d8f, D = 1082;
  `FEATURE_LAYOUT_VERSION` 2): the v1 groups - meta 16, psd 128, env_hist
  32, env_mom 12, env_spec 128, env_spec_lo 32, if_hist 64, if_mom 8,
  if_spec 128, if_spec_lo 32, sq 40, quad 40, acf_c 40, acf_e 40, acf_f 40,
  lagprod 12, tf 12 - plus (phase 3):
  - `sym` 134, symbol-synchronous: the symbol rate = the strongest cyclic
    line of |dIF| (FSK) or of the envelope (linear modulation) against its
    neighbourhood's mean, searched over fs_v / 200 .. fs_v / 2.2 and by the
    detection's width (|dIF|: >= bw / 40 - slow wide-deviation paging; the
    envelope: >= bw / 4; both <= 1.5 bw); when the two lines disagree a clear
    (>= 12 dB) |dIF| line wins (an FSK signal's envelope lines are the
    recording's - AGC, 200 / 567 Hz lines in real YSF / DMR files); parabolic
    refinement; per block of <= 2048 symbols (<= 4096 in all, blocks evenly
    picked) the timing phase of 8 whose IF samples cluster most; then the IF
    at the symbol centres on a fixed Hz scale (64 bins over +-0.25 fs_v:
    187.5 Hz at 24 kHz), |IF| / its 90th percentile (24 bins: the
    inner / outer level ratio), the phase step over one symbol (32 bins, the
    carrier offset's turn removed: CQPSK / pi/4-DQPSK at +-45 / +-135
    degrees), 14 scalars (line strengths, rate / fs_v, log rate, rate / bw,
    harmonic and sub-harmonic lines, concentrations, level quantiles). On the
    bank: rate 4800.0 +- 0.3 Bd for DMR / P25 / NXDN96 / YSF / D-STAR, 512 for
    POCSAG, 18000 for TETRA, 2400 for NXDN48; the real files the same (after
    the envelope rule)
  - `frame_c` / `frame_e` / `frame_f` 48 each: |autocorrelation| of the
    complex view / envelope / gated IF on a LINEAR lag grid - views 0-1: 5 ms
    bins centred at 5..240 ms from the whole-view box sums, views 2-3: 22 us
    bins 0..1.056 ms from the sample-level correlations - max-pooled, minus
    the grid's median, x 8. Real files: DMR 60 / 120 / 240 ms, NXDN 80 / 160,
    YSF 100 / 200, a P25 control channel 75 / 150 / 225 (its TSDUs) - the
    bank's generators the same, except that the bank had no P25 control
    channel (180 ms LDUs only). The wide grid was also meant to split DVB-T
    (896 us) from ISDB-T (1008 us): not enough (below)
  - (layout v3, phase 4) `symseq_meta` 4 + `symseq` 2 x 1024, the LAST
    group: the symbol sequence for the learned front end - 1024 symbols at
    the `sym` group's rate and timing, the on-air ones in time order from
    the first (bursts: the on-air part), zero-padded; channel 0 = the IF at
    the symbol centre / its q90 (deviation tolerance drops out; the absolute
    scale stays in the `sym` scalars), clipped +-1.5, / 1.5; channel 1 = the
    phase step over the symbol / pi; scalars: symbols / 1024, the scale /
    fs_v x 4, the share with a phase step. On the bank the sequence's own
    autocorrelation peaks at the frame period (DMR 144 / 288 symbols, NXDN
    192 / 384, TETRA 255, YSF 480); real files the same (DMR 288 / 576 at
    0.66, a P25 control channel 360, TETRA 255)
  - the `sym` group now works on a channel-filtered copy (windowed sinc,
    cutoff 0.8 x the detection's width, per span; only for detections <
    0.5 fs_v): a neighbour channel inside the view or the view's wider noise
    corrupted the discriminator of weak real signals. Symbol-rate hit rate
    on the real files (653 detections at confirmed / own 4-FSK, TETRA,
    pager, AIS, sonde signals, within 1 % of the class's nominal rate): 62 %
    -> 65 % (DMR 0.50 -> 0.62, P25 0.52 -> 0.63, YSF 0.87 -> 0.96, TETRA
    1.00, radiosonde 0.86 -> 0.81, FLEX 0.81 -> 0.29 - its 800 Hz
    sub-harmonic now wins -, POCSAG 0, AIS 0); most misses are the own
    trunked-site captures' weak channels
  Long views (2-3) are analysed in 8 chunks of 4096 (was 16), quantiles of
  the whole-view box powers on <= 16384 of them. `sigclass_tool layout`
  prints each group's meaning; every value finite and O(1) (clamped); a
  model of another hash is refused. `SIGCLASS_DEBUG_SYM=1` prints the two
  lines per detection (debug output to stderr, any of the tools / the app:
  also `SIGCLASS_DEBUG_FILL` - the band-fill test -, `SIGCLASS_DEBUG_IMAGE`
  - each IQ-image candidate's share - and `SIGCLASS_DEBUG_SV` - each
  second look in view 0)
- **RF plausibility mask** (phase 5; model format 3): the receiver knows
  where it is tuned, and many services exist only in their worldwide
  allocations. `Analyzer::analyze(..., double centre_hz = 0.0)` (0 =
  unknown: no mask): each detection's probabilities are masked by its
  absolute band [centre + offset -+ bw / 2] before the tracker sees them -
  a class with `rf_hz` ranges is plausible only if the band overlaps one;
  a detection whose centre lies in an exclusive band (`bands`) cannot be a
  class outside that band's `only` list; generic classes (the unknown
  families, `none`, `carrier`: `generic: true`) are plausible anywhere; the
  rest is renormalised (everything masked: the generic classes keep their
  share; nothing left: unmasked, never a division by 0). `Model::rf_mask` /
  `has_rf_table`; the service passes the capture's centre (its one call);
  `sigclass_tool classify --rf HZ`, `forward --rf F --offset BW`. No JSON in
  the core and no protocol code: the table is data (`classes.json`, carried
  in the model file). The table and its basis (ITU Radio Regulations
  Article 5 worldwide allocations, the plugins' `fixed_freq_hz`;
  conservative: a class that is used outside a range anywhere in the world
  stays unrestricted):

  | class | rf_hz (MHz) | basis |
  |---|---|---|
  | ils_loc | 74.8-75.2, 108-112, 328.6-335.4 | aeronautical radionavigation: localizer 108-112 (marker beacons 75, glide path 328.6-335.4: the ILS allocations, kept for safety) |
  | vor | 108-117.975 | aeronautical radionavigation |
  | acars, vdl2 | 117.975-137 | aeronautical mobile (R) VHF (HF / satellite ACARS are other modulations) |
  | ais | 156-162.6 | maritime mobile VHF (AIS 1 / 2 at 161.975 / 162.025; the ais plugin's fixed frequency 162.0) |
  | beacon406 | 406.0-406.1 | mobile-satellite, distress beacons only (plugin 406.05) |
  | radiosonde | 400.15-406, 1668-1700 | meteorological aids |
  | wfm | 64-108.5 | broadcasting: OIRT 65.8-74, Japan 76-95, 87.5-108 |
  | dab | 174-240, 1452-1492 | broadcasting band III, L-band (Canada) |
  | dvbt (+ dvbt2, isdbt), atsc, atsc3, dtmb, analogtv | 44-88, 170-230, 470-890 | broadcasting bands I, III (no DAB part above 230), IV / V |
  | lte, nr | >= 400 | the 3GPP bands (450 MHz upward) |
  | uat | 975-981 | UAT 978 (plugin 978) |
  | adsb | 1087-1093 | 1090 MHz (plugin 1090) |
  | ssr1030 | 1027-1033 | 1030 MHz (plugin 1030) |
  | dme | 960-1215 | aeronautical radionavigation DME / TACAN |
  | all others | anywhere | PMR / digital voice, pagers, APRS, LoRa, ISM, FLARM, wildlife, radar, V.23, MPT1327, analog voice, CW, SSB, D-STAR, TETRA ... |

  Exclusive bands: 88-108 only `wfm`; 108-117.975 only `vor`, `ils_loc`
  (GBAS VDB D8PSK: generic PSK); 117.975-137 only `am_voice`, `acars`,
  `vdl2`; 406.0-406.1 only `beacon406`; 960-1215 only `dme`, `adsb`,
  `ssr1030`, `uat` (GNSS L5 / E5, JTIDS: generic noise / pulses). Every
  debug-playback entry keeps its class at its RF (checked: POCSAG at 157.95
  in AIS's range is fine - a range is not exclusive -, wildlife at 151 /
  160 / 166, LoRa at 446 / 868 / 915 / 921, the radar at 1300, DAB at
  227.36 inside both DAB's and DTV's band III)
- **Model file** format 4 (2026-11-22): an ensemble per view - a view may have
  up to `MEMBERS_MAX` 16 entries (fits of the same data), every one with the
  same classes (ids and names, in order; a member with other classes is
  refused, and so is a repeated view in formats 1-3); the view's probabilities
  are the mean of its members' (each with its own temperature, normalisation,
  front end and layers), before the RF mask and the tracker. `model_io`:
  `{"members": [vm, ...]}` per view, written as one entry per member (format 4
  chosen by itself), read back the same way, `forward()` averages - numpy vs
  C++ (`sigclass_tool forward`) on 3-member ensembles of the shipped model and
  perturbed copies, all four views, NaN / inf inputs: max |dp| 4.7e-6 (inputs of
  -3e38 differ, float32 against float64, in the format-3 code before as well -
  real features are clamped to O(1)). The fuzzer (`tmp/retrain/fuzz/`, ASan +
  UBSan) got a format-4 model (view 0 with three members - a Conv1D front end,
  a plain one, a "sequence not read" one -, view 3 with two, the RF table):
  every truncation, 400 000 random byte changes, hostile values in every u32
  field - no fault; every probability finite and summing to 1. Cost: one
  member 0.05 ms a signal; 5 members 4.4x one (measured 0.98 against 0.22 ms a
  signal at load ~22, i.e. ~0.25 ms at the usual load) - nothing ships one yet
- **Model file** format 3 (phase 5): format 2 + the RF table after the
  views (classes with an entry <= 4096: id, generic flag, <= 64 [lo, hi]
  f64 ranges; <= 256 exclusive bands: f64 lo, hi, <= 256 class ids;
  finite, 0 <= lo < hi, every count bounded before use); formats 1 and 2
  still load (no mask). `model_io.write_model` takes the table from
  meta["classes"] (rf_hz, generic) and meta["bands"]; `rf_mask()` is the
  numpy twin (parity at 8 frequencies x 4 views: 2.9e-7); the fuzzer got a
  format-3 model and checks every masked result sums to 1 and is finite
- **Model file** format 2 (phase 4; format 1 files still load): per view
  an optional front end after the input standardisation - 0 none, 1 Conv1D
  (sequence slice offset / length / channels, K filters x L taps, stride
  1, ReLU, max- and mean-pooled over time, the pooled values standardised;
  the MLP then reads the other features followed by them), 2 "the
  sequence is not read" (views without a front end: no zero weights for
  2048 inputs). The loader bounds C <= 8, S <= 65536, K <= 256, L <= 256,
  L <= S, the slice inside D, every product in 64 bits before allocation;
  the evaluator is a plain per-filter loop over time with the taps outside
  (vectorised). Parity numpy vs C++ (`model_io.forward` vs `sigclass_tool
  forward`, random models: formats 1 and 2, with and without front end,
  NaN / inf inputs): max |dp| 1.7e-7. The review's fuzzer
  (`tmp/review/fuzz_model.cpp`, ASan + UBSan) got a format-2 model (a
  front-end view and a not-read view): every truncation, 400 000 random
  byte changes, hostile values in every u32 field - no fault.
- **Model file** (`model.cpp` header): "KSIGCLS\0", format 1, layout
  version + hash + D, meta JSON, per view: class ids / names, temperature
  (1e-3 .. 1e3), input mean / std, Linear + ReLU layers, linear output;
  CRC-32 trailer. Python parity (`model_io.forward`) 1.7e-7 (core author)
- **Tracker** (`tracker.cpp`): association by band overlap (IoU >= 0.3) or
  close centres (narrow, width ratio <= 4); a band-filling detection joins
  the band-filling track; a detection lying mostly (`merge_overlap` 0.5) in
  a track's band envelope without matching it keeps that track alive but
  changes neither band nor class evidence. Band envelope `lo_hz` / `hi_hz`
  (public since 2026-11-20): widens at once to a detection's band, narrows
  20 % a hit toward it; a band-filling track's is the capture +-0.47 x 2.4
  MHz. After each update tracks are merged (one signal = one track): a
  track whose envelope lies >= 50 % inside one >= 2x wider is absorbed by
  it (not by a band-filling one unless the piece is bursty, < 10 dB or seen
  less than half as often; not when the wide one is >= 20x wider and >= 200
  kHz and the
  narrow one strong and continuous - a carrier on a DAB block); a wide
  transient (bursty, less than half the hits) over a lasting narrow track
  is dropped instead (a key-up click's splatter); similar widths overlapping
  >= 25 % = one channel (the one with fewer hits goes, the survivor takes
  the union band). `expire(t)` (no snapshot) and `Track::fills_band` added
  for the service. Class evidence: per view the mean of the log-probabilities
  of the first 1 / alpha looks, then an EWMA with weight alpha (phase 5:
  alpha 0.1, was a plain EWMA 0.3), the track's view = the one most of its
  detections had. Why: the debug playback's DMR entry (a repeater with both
  timeslots busy, then two radios in direct mode, 12 s looped) ended "P25
  Phase 1 50 %" live and "DMR 97 %" offline - not a difference of the paths
  but of where the evidence ended: per snapshot the repeater part reads P25
  in 3 of 8 snapshots (0.54-0.99), the direct-mode part DMR 0.95-0.99; with
  0.3 the last three looks decided, and the live run's 40 s end at 4 s into
  the loop, inside the repeater part
  - (2026-11-21, after the failure analysis: a correctly labelled channel was
    absorbed into a wide weak track - the recording's plateau, a key-up
    click's splatter, a packet's splatter, a receiver hump - at the first
    snapshot both had 1 hit, so the transient rule never fired, and later the
    channel's detections only kept the wide track alive as "inside pieces")
    four rules, every one with its reason:
    - **a channel's core**: a detection / track >= `min_channel_hz` 2 kHz wide
      (a modulated channel, not a carrier line - lines stay pieces of the
      signal around them: an NFM carrier, a VOR) inside a track >= 2x wider
      that holds at most `core_energy_ratio` 1.5 x its energy (energy =
      10^(SNR / 10) x width, x duty when bursty: the excess power over the
      floor) is that track's core: neither absorbed into it nor counted as
      its piece. A skirt, a receiver plateau or a key-up transient adds little
      energy to the channel inside it; a second signal adds its own (two FSK
      tones: ~2x - still one signal). Not between two bursts (pieces of one
      pulsed signal - DME, radar - found at two widths: without this DME lost
      3 of 123 synthetic signals) and not under a band-filling track
    - **a burst seen once** (1 hit) does not take a continuous narrow track
      with >= 2 hits: it waits until it repeats; the transient rule (a burst
      with less than half the channel's hits goes) then drops it
    - **a faint pedestal**: a continuous wide track under `pedestal_db` 3 dB
      of mean SNR does not take a track >= `pedestal_contrast_db` 10 dB
      stronger (its strongest look, hits and pieces, forgotten at 1 dB a
      snapshot); a modulated signal's own sidebands around its carrier are far
      denser than a faint hump. The VOR own capture (114.5 MHz: its width flips
      0.1 / 2 kHz, a ~1 dB receiver hump beside it) now ends "VOR 0.95, 3
      hits" instead of a 1-hit track
    - **looks kept**: a lasting narrow track absorbed into a continuous wide
      one seen for the first time (the same signal measured wider) gives it
      its looks - the evidence per view weighted by the looks, its hits (not
      into a burst or a band-filling track: a band-wide burst took a pager's
      evidence and DTMB / DME / FLARM lost 2-4 points)
    Each rule turned off on the synthetic regression set (below): core -52
    signals (top-1 of 6255; final code), burst seen once -19, pedestal -1,
    looks kept -1 (these three measured on the code just before the last
    two changes - the image candidacy, burst seen once only over a continuous
    channel - with env toggles since removed); `TrackerConfig::min_channel_hz`,
    `core_energy_ratio`, `pedestal_db`, `pedestal_contrast_db`;
    `sigclass_tool classify --min-channel HZ --core-ratio X` (and
    `--second-view-max HZ`, `--image-share X` for the analyzer rules)
  - **the synthetic regression set** these rules were validated on (not on
    the recordings that suggested them): 2000 composed captures of 2 s (4
    snapshots), composer seed 31 (no training set used it), 6255 signals,
    `classify --track`, scored on the last snapshot's tracks (job scratch
    `fix_core/run_syn.py`, `score_syn.py`). Shipped core -> the three rules:
    track label right 0.631 -> 0.648 (drawn 0.610 -> 0.626); false labels
    (drawn tracks with >= 2 hits overlapping no signal) 176 -> 140 per 2000
    captures, at a signal's mirror band 61 -> 30; narrow signals with two or
    more drawn tracks 3.4 -> 3.8 %; no class loses more than one signal
    (CW, FLEX, AM voice, wildlife, radar, FLARM, ACARS one each); APRS +11,
    NXDN +7.8, P25 Phase 2 +6.6, NFM voice +5.7, D-STAR +5.3, YSF +4.1
    points. The analysis' synthetic twins (6 seeds at 30 / 45 / 55 dB): NXDN
    6 / 4 / 2 -> 6 / 6 / 6, P25 6 / 5 / 5 -> 6 / 6 / 6, DMR 6 / 6 / 5 ->
    6 / 6 / 6. Detector acceptance set unchanged byte for byte
  - **wide-shift FSK tones** (2026-11-22, `merge_tone_pairs`,
    `Config::tone_merge`): with a modulation index above ~5 (ISM sensors and
    keyfobs with 50 us - 2 ms bits and tones 20-200 kHz apart, FLEX, DFM) the two
    tones are detected apart and each alone is an on / off keyed carrier (46 of
    90 real ISM FSK rows partly or wholly, 17 ended "ISM device (OOK)"). Any two
    detections - not only neighbours in frequency: pieces lie between the tones -
    that look alike (<= 400 kHz from one's lower edge to the other's upper, not
    overlapping, widths within 16x, tone power = max-hold x width within 10 dB)
    are one transmitter when (a) at burst level they share its timing: per packet
    (the short frames where either is on, gaps <= 2 frames bridged) both are on
    in it and start and end with it (within 2 frames or 10 %), for >= 80 % of the
    packet frames - two devices that merely overlap in time, two radios taking
    turns on adjacent channels do not; and (b) at sample level their powers are
    complementary: each band's |x|^2 cut from the long blocks' spectra (its bins,
    10 % raised-cosine edges, one inverse FFT of LONG_N / D per block, D the
    largest of 100 .. 2 with fs / D >= 1.25 x the wider tone - bits shorter than
    the 427 us frame are resolved, where both tones are on in every frame),
    summed over the tones' resolution, gated to the packets: the mean-normalised
    powers correlate <= -0.5 (z <= -6), each is keyed (CV >= 0.3) and their sum
    is steadier (CV <= 0.6 x either's). Two independent signals correlate ~0; a
    keyed carrier and its keying sidelobes, DSB sidebands, a Manchester code's
    two lobes correlate positively. The pair becomes one detection over the
    union band (centre: the tones' power centroid; SNR: their power over the
    union's width - not re-measured from the PSD, where a spur inside the band
    took the 98 % band); pieces inside it no stronger than the tones (+3 dB) and
    on only while it is (or continuous like it) go. At most 48 pair tests and 4
    long blocks a test per snapshot. `SIGCLASS_DEBUG_TONE=1` prints every test,
    `sigclass_tool --tone-merge 0` turns it off. Measured: the 94 ISM FSK twins
    (synthetic copies of the measured rtl_433 devices, 108 snapshots with a
    packet), snapshots where one detection holds both tones and no other piece
    23 -> 47 (+ 35 / 36 where it is also much wider: high-SNR splatter), one-tone
    pieces 31 -> 7; 150 synthetic wide-range FSK captures (`synth.py ism_fsk2`
    seeds outside the bank, 8-40 dB) whole 42 -> 105, pieces 64 -> 3; 75 pairs of
    independent devices 12.5-150 kHz apart: one detection over both 46 -> 52 (the
    6 new ones: the second device inside or at the edge of the FSK's own band).
    The v4 branch's neighbour-only, frame-level test made 0 of 26 fragments whole
  - **keying sidelobes** (2026-11-22, the burst-splatter rule widened,
    `Config::sidelobe_widths` 8): a carrier keyed with pulses of length Tp has the
    spectrum envelope 1 / (pi f Tp)^2: its sidelobes are 1 / Tp wide (half the
    2 / Tp main lobe; a group of joined ones as wide as the main lobe or wider)
    and fall 6 dB per octave of offset without end; a band d1..d2 off the carrier
    holds a mean density of 1 / (pi^2 d1 d2 Tp^2) of the main lobe's. With the
    burst's width as its main lobe (Tp = 2 / bw), a piece centred outside the
    burst's band, at most 4 x its width, within 8 widths (30-300 kHz), no
    stronger (max-hold) than that envelope + 6 dB (-10 log10(4 pi^2 d1 d2 / bw^2)
    + 6) and >= 6 dB under the burst, that comes and goes with it (short-frame
    correlation > 0.6) or is a faint "continuous" average (< 1 dB, >= 20 dB
    under it) is a sidelobe too. The old rule (at most half as wide, within 3
    widths, 30-150 kHz) missed the `ism-six-devices` playback's stray "Unknown
    OFDM 52 %" (a 13 kHz piece 57 kHz below a 16-21 kHz OOK burst, ~32 dB under
    it) and 25-45 kHz skirts 20-50 kHz off the same burst: on that scene (6 s,
    looped to 42 s, snapshots every 0.97-1.31 s) 14-22 tracks -> 9-12, every one
    of the 6 devices kept (the stray itself did not occur offline).
    `sigclass_tool --lobe-widths 0` turns it off
  - both rules on the synthetic regression set (below) with the shipped weights:
    track label right 0.649 -> 0.648 (4057 -> 4056 of 6255), false labels 140 ->
    127; FLEX -2 signals, ISM FSK and unknown FSK -1 each (every one traced: the
    tones now merged correctly into one detection, which the old weights had not
    seen whole), the sidelobe rule alone +3 signals, false labels 125; detector
    acceptance set byte-identical; `selftest` passes
  - not attempted (from the same analysis): a max-hold "burst" region that
    bridges and swallows continuous detections (a 1.4-2.2 MHz bursty region
    over a busy site's strong carriers: 416.5 MHz DMR x4, the MPT1327
    control channels of the intermodulation capture) and a band-wide level
    flutter of about +-1 dB taken for a burst
- **Cost** (`sigclass_tool bench`, i7-11800H, load ~1.5, layout v1 tool of
  commit 8b7c707 and v2 run alternately on the same three files,
  2026-11-20): fixed per snapshot 21 ms both (front end 11 ms, detector 8-12
  ms; the 34-41 ms of phase 2 were measured under load 18-30 - nothing in
  the fixed path had grown); per signal, narrow (view 0) features 1.09 ->
  1.25 ms, a typical mix of 21 signals channelize 0.29 -> 0.24 and features
  1.49 -> 1.59 ms, wide signals (views 2-3) channelize 1.85 -> 1.25 and
  features 4.5 -> 3.8 ms (8 chunks instead of 16; the rest of a view-3
  signal's cost is the whole-view pass over 1.2 M samples: box sums 1.0 ms,
  on / off runs 0.4 ms); the `sym` group 0.36 ms (view 0) / 0.46 ms (view
  3), the frame groups < 0.05 ms; model (1082-192-96-C) 0.05 ms. A Pi 5 is
  an ESTIMATE, not measured: 3-4x this machine per core -> ~70-90 ms fixed,
  ~4-6 ms per narrow signal, ~15 ms per wide one
- **Tool** `tools/sigclass_tool.cpp`: `layout`, `features`, `classify
  [--track]` (tracks with lo_hz / hi_hz), `bench`, `view`, `forward`,
  `selftest`; `make sigclass-tool` or the one-line g++ in its header.
  `SIGCLASS_DEBUG_FILL=1` prints the band-fill test's numbers to stderr
- **Acceptance** (the core author's sets, re-run after every detector
  change, 2026-11-20): Pd 1.00 unchanged for every type and SNR (carriers,
  2- / 4-FSK, 25 k - 1.5 MHz noise blocks, 1 ms bursts, ADS-B-like pulses,
  OFDM fill, two channels); extra detections per capture down (ADS-B 10 dB
  0.17 -> 0, OFDM fill 20 dB 0.17 -> 0, 1 ms bursts 6 dB 2.50 -> 2.17);
  noise-only 1 false (narrow, -3 dB) in 40 captures as before; the debug
  playback's table-driven noise (replicated in Python) and numpy noise at
  0.7-25 LSB: 0 detections; `selftest` passes. Unchanged after the phase 3
  rules (burst hump, image, comb prominence). Not run: the MinGW / zig
  syntax checks (neither is installed on the dev machine); the changes are
  standard C++20 without platform code

## Training and evaluation

Everything in `tools/sigclass/` (numpy + scipy only; commands in its
`README.md`); bulk data outside the repo (`--work`, `$KRAKEN_SIGCLASS_WORK`).

- **Class table** `classes.json`: 51 classes = 45 signal types + 5 unknown
  families (`unk_fsk`, `unk_psk`, `unk_ofdm`, `unk_pulse`, `unk_noise`: the
  open-set sink) + `none` (`hidden: true`: a detection that is no signal -
  never shown). `merged_into`: DVB-T2 and ISDB-T are trained as `dvbt`
  ("DVB-T / T2 / ISDB-T") - with the v1 features 28 % of DVB-T went to
  DVB-T2 and 35 % of ISDB-T to DVB-T. Re-tested with the v2 linear lag grid
  (phase 3, trained apart, `train.py fit --no-merge`): still no separation -
  val / test ISDB-T 0.48 / 0.54 (0.27 / 0.35 -> DVB-T), DVB-T 0.83 / 0.76
  (0.11 -> DVB-T2), DVB-T2 0.93 / 0.66 (0.25 -> ATSC 3.0 on test); so the
  merge stays. The dtv plugin tells the standards apart itself. TEMPEST is
  no class.
  Per class: plugin, demod / VFO width (click-to-tune), occupied-bandwidth
  range (the SNR bandwidth's clamp, and the view plausibility rule below),
  burst, family, fills_band
- **Bank** `bank.py` (39 GB, 1582 clips, 4.6 h of signal after phase 3): clean,
  single-transmitter complex baseband per class from the plugins' own
  generators (run as separate programs - the GPL ism / flarm generators
  too, never copied) and `synth.py` (AM 25 / 8.33 kHz, SSB, CW, carriers,
  the unknown families kept +-5-6 % away from the known symbol rates /
  subcarrier spacings; since phase 2 also a broadcast-FM model with music /
  speech, pre-emphasis, limiter, stereo pilot and an RDS-like subcarrier,
  and generic OOK / 2-FSK ISM devices - real FM was "LoRa" for every model
  before). Seeded IDs, payloads, modes, speeds, timing. Phase 3: dmr_gen,
  dstar_gen, nxdn_gen, p25_gen and tetra_gen took an optional `--seed N`
  (without it their output is bit-identical - all 13 catalogue steps of the
  five, checksums before / after); the bank seeds every clip and sends the
  vocoder-frame voice (`ambe` / `imbe`: what radios send) instead of 30 %
  random bits; new physical-layer variants in `synth.py` - a P25 control
  channel in C4FM (p25_gen's frame sync + NID, three random TSBKs, status
  symbols: 360 symbols = 75 ms), P25 CQPSK / LSM (p25_gen's voice calls or
  control channel, the phase step of each C4FM symbol, RRC roll-off 0.2-1.0:
  on-air LSM occupies ~9 kHz), NXDN48 (2400 Bd, +-350 / +-1050 Hz,
  nxdn_gen's filter and FSW + random payload symbols - nxdn_gen frames
  NXDN96 only; a physical-layer approximation whose 80 ms periodicity is
  weaker than a real one's), VOR at measured depths (30 Hz 8-35 %, the 9960
  Hz subcarrier 18-32 %, ident 5-15 % or none: ils_gen has the nominal 30 /
  30 % only); p25p1 48 clips (24 / 12 / 12), nxdn 32 (12 NXDN48), dmr 40
  (phase 4: + 8 mobile-to-repeater clips, `synth.py dmr_ms` - dmr_gen's
  TDMA bursts with the MS-sourced voice / data sync words of TS 102 361-1;
  the bank had BS and direct-mode syncs only, and a sync pattern the bank
  never shows cannot be learned; the dmr plugin decodes them), dstar 24,
  tetra 24, vor 24; LTE / NR half lightly loaded (0.02-0.2), 40 /
  32 clips; LoRa: 2 of 5 clips sparse (24 s, a packet every 1.5-8 s). Limits:
  sfn_gen has no `--seed`; dab_gen's audio is a tone per service; analogtv
  one test card; tetra_gen's payload is the empty SCH/F every frame without
  `--traffic` (the bank uses it since 2026-11-22) and its
  output does not decode for 18 of 64 colour codes (2, 3, 5, 8, 10, 17-20,
  27, 28, 34, 38-40, 46, 52, 54: the bank uses the others); radiosondes are
  RS41 and M20 only (no generator for DFM / iMet / LMS6 / M10 / MRZ / MTS01:
  left to unknown FSK)
- **Bank additions of 2026-11-22** (after the ISM / TETRA / P25 / FM failure
  analyses; parametric, no per-device code; the earlier clips of each class
  unchanged): `synth.py ism_fsk2` (ism_fsk clips 80-239): 2-FSK with the bit
  time 10 us - 2 ms and the tone spacing 10-200 kHz drawn independently
  (log-uniform, modulation index kept to 0.5-60), NRZ / Manchester / NRZI /
  PWM-FSK (the information in the length of one tone), Gaussian-filtered (BT
  0.3-1) or hard keyed, packets of 2 ms - 1 s (preamble, sync word, payload)
  sent 1-6 times 1-50 ms apart, groups 0.1-1.5 s apart, key-up pull (+-30 kHz,
  50 us - 5 ms) and chirp (+-20 kHz/s); `synth.py ism_ook2` (ism_ook clips
  104-203): hard-keyed long pulses (te 0.4-2 ms), very short pulses (10-80 us),
  shallow keying (6-20 dB) and ASK with a residual carrier (6-12 dB), PWM /
  PPM / Manchester; TETRA all 36 clips with `tetra_gen.py --traffic` (random
  SCH/F and AACH bits in every burst - before, every normal burst was the same
  empty one: |autocorrelation| at the slot / frame lag 0.92 / 0.69, now 0.006 /
  0.004; without the switch the generator's output is sha256-identical) and a
  slow carrier drift (bank.py's `drift`: an Ornstein-Uhlenbeck frequency, rms 1-15
  Hz, 0.2-2 s); `synth.py wfm_loud` (wfm clips 48-63: a slow AGC and a hard
  limiter, 70-80 kHz deviation - real stations are detected 100-130 kHz wide).
  Tried and removed: P25 Phase 1 C4FM at 0.8-1.6 x its deviation in 35 % of the
  draws (a "wide pulse" variant seen in one public recording) - the 4-FSK systems
  differ mainly in their outer deviation (P25 +-1.8 kHz, DMR 1.08 x, NXDN96 1.33 x,
  YSF 1.5 x), so it laid P25 over the other three in the property that separates
  them best; on the synthetic val clips the confusion among the four at nominal
  deviation was 21-30 (median 27) detections over 7 fits with it, 19-27 (23)
  without (the shipped model 19); the composer is back to the +-10 % every
  constant-envelope class has (generation B of the Results below: the same
  caches with those rows dropped, `tmp/retrain/bin/drop_devwide.py`). classes.json: ISM FSK
  occupies 10-500 kHz, OOK 3-300 kHz (was 20-160 / 5-120: the SNR bandwidth's
  clamp and the evaluation's band-limited feed). Checks: the IQ measurement of
  the failure analysis (OOK vs FSK from the envelope / IF) finds 160 of 160 new
  FSK clips FSK and 95 of 100 new OOK clips keyed as meant (all 100 with noise
  added: the 5 are shallow / ASK clips without noise); the tetra decoder decodes
  composed clips (3 of 3: network, colour code, location area); 77 of the 98
  measured real FSK devices now have a bank clip within 25 % of both their bit
  time and tone spacing (was 35), 139 of 142 OOK devices (was 112). Not added:
  radiosondes without a generator (DFM, MRZ, iMet-4), dPMR
- **Composer** `scenes.py`: 0.5 s 2.4 MHz cu8 captures, 0-8 signals (+
  spurs) placed in the frequency domain (one inverse FFT): 25 % at one
  channel spacing from a neighbour, 12 % of later narrow ones ON an earlier
  one's channel +-1.5 kHz (half of them the same class: radios taking
  turns - the playback scenes have 2-5 transmitters per channel), ~11 %
  near DC, ~11 % at the band edge; SNR 55 % U(5, 25), 30 % U(-3, 45), 15 %
  U(25, 55) dB (phase 3: real narrow recordings are often 30-60 dB; the bank
  of phase 2 had a median of 15 and a 90th percentile of 25 dB per
  detection), broadcast FM 40 % at 2-12 dB, <= 40 dB between signals;
  constant-envelope FM / FSK classes their deviation x U(0.9, 1.1) and in
  30 % a 4th-order channel filter (a transmitter's tolerances: DMR's and
  P25's level sets now overlap, as on the air); bursts fully in / cut /
  absent; per signal sample-rate error,
  flat fading (static / Rician / Rayleigh, Doppler <= 100 Hz), phase noise,
  soft keying edges, 15 % multipath; receiver noise 3-40 LSB and (since the
  own off-air captures showed 0.6-1 LSB at full gain) 30 % 0.3-3 LSB, droop,
  spurs (4 % a comb every 100-300 kHz), (phase 4) in 12 % one or two
  noise-like humps 8-25 kHz wide, 3-15 dB, at 1.04-1.12 MHz from the centre
  (70 % above it: an RTL-SDR's band-edge artefacts at high gain - the own
  1290 MHz empty-band capture showed one at +1096 kHz as "Unknown OFDM";
  in no truth: `none`), IQ imbalance, DC, 8-bit quantisation after the
  noise, gain pulled down near clipping (6 % let clip)
- **Labels** (`train.py ingest`, per batch through `sigclass_tool
  features`): a detection is matched to the truths it overlaps (cover of
  the detection / of the truth's 99 % band); one dominant truth (6 dB) or
  several of ONE class labels it, else dropped; a detection near no signal
  is `none` (a detection near a wide / pulsed signal's band edge is dropped,
  not `none`: skirts, lobes, edge pilots); a piece of a wide signal in a
  view too narrow for that class (occupied-bandwidth minimum > 2 x the
  view's widest) becomes `none` - so no "DAB" on a 0.4 kHz track. Phase 3:
  a detection outside a strong truth's band (>= 20 dB) within max(2 x its
  width, 25 kHz) and >= 15 dB weaker - or at its IQ image - is dropped, not
  `none` (`near_strong`; applied to the caches on loading too). With the
  stronger composer the val set's confident wrong labels were nearly all
  such "none" detections (images, skirts) named by the strong signal's
  class: `min_confidence` could not reach 95 % precision (0.98 at 38 %
  shown); with the rule and the detector's image rule 0.81 at 76 % shown
- **Splits**: by bank clip, per class (every 10th clip test, the next val,
  the rest train: test payloads / IDs / modes never trained on); the test
  scenes also come from another composer seed
- **Trainer** (`train.py fit`, its own numpy MLP: Adam with decoupled
  weight decay 1e-4, dropout 0.2, label smoothing 0.05, class-weighted
  cross-entropy (weights (N / n_c / C)^0.75; `none` x 2.5 since phase 3), cosine LR from 2e-3, batch
  512, early stop on val macro recall; `gradcheck`: 1.7e-7). Per view the
  classes with >= 100 training examples there; one temperature per view
  (val NLL); `min_confidence` = the lowest p at which >= 95 % of the shown
  (not `none`) val labels are right
- **Classes kept out of a view** (2026-11-22, `fit --exclude-view-classes`,
  default `1:dmr,p25p1,p25p2,nxdn,ysf,dstar,mpt1327,nfm_voice,aprs`): with 59 000
  captures of the new composer YSF, APRS and NFM voice reached the 100 examples
  in view 1 (96 kHz) and the trainer took them in - but the analyzer's second
  look in view 0 takes view 0's answer only for a class view 1's model lacks,
  and that rule was validated with the 12.5 kHz channel classes absent there.
  They are kept out (their view-1 rows are not trained, as before)
- **Ensembles** (2026-11-22, model format 4): several fits of the same data
  (seeds only) averaged per view; `tmp/retrain/bin/make_ens.py` builds one from
  fits and refits `min_confidence` on the synthetic val split as the trainer
  does (averaged probabilities are softer: 0.70-0.74 against 0.80-0.83 for one
  fit), `thresholds.py` then works on it unchanged
- **Per-class thresholds** (`thresholds.py`, phases 4-5): per shown class
  the lowest p >= `min_confidence` at which its synthetic val labels are
  >= 95 % right, raised above every wrong label of it on HALF A of the own
  native captures (crc32 of the capture id even) - wrong at an identified
  signal or drawn (p >= 0.5, not hidden) where none is (`eval_real.py`'s
  `false_tracks`: own captures list every identified signal in their
  `expect` rows); 1.01 = never sure. The eval of half A runs WITH the RF
  mask (the receiver's default). Writes `min_p` into `meta["classes"]` and
  the RF table (format 3)
- **The learned front end** (phase 4, `fit --conv-views 0,1 --conv-k 32
  --conv-l 24`): per view a `ConvMLP` - Conv1D over the int8 symbol
  sequence (the caches keep it apart as `Q`, int8 x 127: 2 KB a detection;
  the other 1086 features `X` float32), im2col + GEMM, ReLU, max + mean
  over time, appended to the standardised features, the MLP; back-propagated
  end to end (`gradcheck`: conv 2.2e-8, MLP 1.7e-7). 0.25 s a batch of 512:
  ~25 min for views 0-1 of 59 000 captures. Views without it export front
  end 2 (the slice not read). Ablation options: `--drop-groups` (feature
  groups left out), `--drop-edge-none`
- **Variance** (phase 4): the real-data score of a class moves a lot with
  the training set, not only with the features: the same layout, the same
  recipe, 43 000 vs 59 000 captures gave real ISM OOK 29-33 vs 95 of 149
  (the phase 3 model: 79); dropping the `sym` groups or the band-edge humps
  changed nothing (29, 30) - more data did. Single-class real deltas of a
  few signals are within that noise

### Diagnosis of the synthetic-to-real gap (phase 3, 2026-11-20)

The phase 2 model (layout v1) against the real library, per failing class:
the real detections' features compared per feature group with the true
class's training distribution (mean |z| over the group, same view; the
held-out synthetic examples sit at 0.4-0.9), plus plain facts (detected
width, view, SNR, duty) against the bank. Four kinds of cause:

- **(a) the evaluation path** (fixed in `eval_real.py`, see its README):
  the recording receiver's DC spike inside the signal (public SDR# DMR /
  NXDN files, a P25 file whose first two snapshots hold only the spike,
  YSF: 26-35 dB lines -> "carrier 0.88"); squelch-gated files (26-82 % exact
  zeros; mean removal made a gated DC tone) and files that open with
  silence (the first 4 s: 5 distinct sample values); recordings with more
  SNR than 8 bits hold (60-100 dB) quantised with the noise under 0.5 LSB
  -> a whole-band "burst" (unknown noise / radar). Also the core: a 2.7 dB
  pedestal beside a real DMR channel made a 42 kHz burst detection the
  tracker merged the channel into ("AIS") -> the detector's burst-hump rule
- **(b) bank realism**: real narrow recordings at 30-60 dB SNR (the bank's
  detections: median 15, 90th percentile 25 dB) - TETRA at 41 dB had its
  psd / envelope / x^2 / x^4 / lag-product groups 1.9-2.6 sigma off; real
  P25 control channels framed every 75 ms (TSDUs), the bank had voice calls
  only (180 ms LDUs)
- **(c) generator artefacts**: none found - the frame lags of real DMR (60 /
  120 / 240 ms), NXDN (80 / 160), YSF (100 / 200) equal the generators';
  the fixed payload seeds don't show in a 0.5 s snapshot (payload bits are
  random per frame anyway)
- **(d) missing variants**: P25 CQPSK / LSM (all 5 such recordings ->
  P25 Phase 2: if_hist 2.4, acf_e 2.2, acf_f 2.2 sigma), the P25 control
  channel, NXDN48 (-> unknown FSK / VDL2), radiosonde DFM / iMet / LMS6 /
  M10 / MRZ / MTS01 (no generator; DFM09 sat closer to POCSAG in the low
  spectra and lags)
- and the features: a clean P25 control channel was as near NXDN as P25 in
  every v1 group (largest: envelope spectrum 2.0, IF spectrum 1.7): the raw
  IF histogram blurred the 1800 / 2400 Hz outer levels and the log lag grid
  could not see a frame period -> layout v2 (`sym`, `frame_*`, above)

### Not merged: the symbol-rate estimator v4 experiment (2026-11-21)

Branch `worktree-signal-classifier-symv4` (one commit on top of f045b13;
its copy of this file has the full tables). Read this before working on
the `sym` group or retraining for the 4-FSK family again.

- The request: the `sym` estimator finds the right rate in only ~0.65 of
  the 4-FSK detections - measured on the IMPAIRED synthetic test set (DMR
  0.67, P25 0.65), which matches real recordings (0.62, 0.63); on the clean
  bank clips it is ~1.0, so the composer's channel model already reproduces
  the failure. Misses at SNR >= 10 dB: the voice classes pick an unrelated
  weak line, the pagers a harmonic (x2, half, x3), TETRA a strong wrong line
- The estimator built there (layout v4; `tools/sigclass/sym_bench.py`
  scores it on synthetic captures with known rates, fit seed 101, test
  seed 7): coherent cyclic spectra over the whole view of the envelope,
  |dIF| and even lag products, candidates with harmonic moves and a
  run-length statistic, a fitted score and confidence with "no rate" below
  it, interpolated timing; the detector merges complementary FSK tone
  pairs. Synthetic, 4-FSK / TETRA at >= 10 dB: hit 0.88, confident-wrong
  0.41 (all target classes) -> 0.03. Real recordings: rate right 0.62 ->
  0.73, wrong 0.38 -> 0.13 (DMR 0.77, P25 0.80). Cost of the block +0.5 ms
  per narrow signal, -0.3..0.5 ms per wide one. Still poor: POCSAG (0.59 /
  0.71; 26 % of its detections are fragments - one tone of the wide-shift
  pair), FLEX, AIS / VDL2 (short bursts: mostly "no rate"), DFM (bit rate
  reported, not the chip rate)
- **Why it is not merged**: a model retrained on those features (59,000
  captures; plain and with the learned Conv1D front end reading `symseq`;
  two seeds each) is no better on real data. Real DMR 3 of 5, P25 9 of 19,
  NXDN 2 of 5 in EVERY fit - the same counts as this branch's model;
  library top-1 143 / 156 / 164 / 174 of 357 across the four fits against
  177 here; own captures 65 of 110 against 68; debug playback 27 and 26 of
  28 (DVB-T read as ATSC 3.0) against 28 and 27; sure-label precision on
  the library 0.80 against 0.91; the empty band shows one unsure label
- What it taught: (1) symbol timing was NOT the bottleneck of the real
  4-FSK rows - they did not move with a far better estimator or with the
  front end, so those failures belong to the particular recordings (look
  at them one by one before changing features again); (2) the trainer seed
  moves the real-data score more than a feature change does (ISM OOK 66-91
  of 149 between seeds, equal synthetic validation): never judge a change
  by one fit - several seeds, or an average of them, and a real benchmark
  far larger than 5 DMR / 19 P25 / 5 NXDN recordings (the local trunked
  systems, labelled by the decoders); (3) the one real POCSAG IQ recording
  is a poor witness: its channel alternates POCSAG 1200 and FLEX 1600 and
  the file carries narrow periodic artefact lines (one measurement saw a
  comb every 512 Hz in the envelope, a second an 800 Hz grid in the
  spectrum - not settled); its channels are at about -10.25 and +14.6 kHz,
  not where the index first had them

### Why the real recordings fail: the per-recording analysis (2026-11-21)

Every failing real signal of the shipped model was traced one by one
(truth from the project's decoders; how it was fed; detection; features
against the synthetic rows of its class; per-snapshot output; tracker;
synthetic twins; an experiment per cause): 38 failing rows of the public
narrowband digital recordings, 47 of the own native captures. **The
real-recording tables above understate the classifier: most failures are
in the evaluation and around the model, not in the features or the
training.** The evaluation, the tracker, the view fallback and the image
rule were fixed the same day (the next section); the rest is the work list.

- **Evaluation and truth, 44 of the 85 rows** (`eval_real.py`, the
  library's index): the 5-channel capture was read as one interleaved
  stream (0 of 8 right; antenna 0 alone 7 of 8); 14 rows of bursty voice /
  payload channels are not on air in the 4 s window that is scored; wrong
  truth rows (P25 Phase 2 voice listed as Phase 1, channels at other
  offsets, a carrier that does not exist, two tones of one FSK device as
  two OOK devices); a narrow recording placed into the 2.4 MHz band keeps
  its own floor ~1 dB above the white fill, and the detector reports that
  band as a 70-100 kHz signal (9 rows; replacing the floor outside the
  signal with flat noise makes all 9 right at 0.86-1.00); two IF-filtered
  files whose passband noise became a "noise-like signal"
- **The tracker absorbs a correctly labelled channel into a wide weak
  track** (~17 public rows end this way; also at runtime: key-up clicks,
  packet splatter, band-wide floor flutter): a narrow track inside one at
  least 2x wider is absorbed while both have one hit, and from then on the
  channel's detections only keep the wide track alive. In 12 of the 15
  DMR / P25 / NXDN rows that fail in every fit the model's own output on
  the channel is right at 0.84-1.00 per snapshot. Experiment (a channel
  >= 2 kHz wide and >= 6 dB stronger is neither absorbed nor counted as a
  piece): library confirmed top-1 177 -> 186, shown-correct 117 -> 126, own
  captures unchanged; the 2 kHz floor is needed (without it NFM carriers
  split off); side effect to check: a dPMR recording then reads NXDN 0.98
- **The 96 kHz view has no 12.5 kHz class** (no P25, P25 Phase 2, DMR,
  NXDN, YSF, D-STAR, MPT1327, NFM voice): training keeps a class in a view
  only with >= 100 examples and the composer made 46-96. A real channel
  detected wider than 10.8 kHz (a shoulder, a neighbour, three adjacent
  simulcast channels, ghost copies) is sent to a model that cannot name
  it - this, not the features, is why those rows did not move in any fit.
  Experiment (a view-1 detection 10.8-40 kHz wide is also classified in
  view 0, the more probable known class wins) plus a tighter IQ-image
  rule: channels on air at the busy local trunked sites 66 -> 77 of 81
  right; no regression on 44 synthetic single signals (not yet run on the
  impaired synthetic set or the public library)
- Detector: the IQ-image rule dropped a real control channel 1.5 kHz off
  the exact mirror of a 27.7 dB stronger one (it allows half a bandwidth);
  a max-hold "burst" region 1.2-2.2 MHz wide bridges continuous
  detections and swallows channels; a +-1 dB band-wide level flutter is
  taken for a burst; wide-shift FSK still arrives as its tone humps (FLEX
  6400 / 4, DFM, ISM FSK); the VOR's detected width flips 0.1 / 2 kHz and
  the tracker then absorbs it into a ~1 dB receiver hump
- Bank: the TETRA generator repeats an identical frame, so `frame_c` shows
  peaks of 2-4 at the slot and frame lags where real TETRA shows ~0
  (replacing that one group by the class mean turns unk_psk into tetra
  0.98-1.00); radiosonde types without a generator (DFM, MRZ, iMet-4: 6
  rows); a "wide pulse" P25 at ~1.5x the C4FM deviation (the bank allows
  +-10 %); weak broadcast FM is detected 100-130 kHz wide, the synthetic
  rows 60 kHz
- Receiver and recordings: one crowded local capture holds 34 third-order
  intermodulation products (the product of three strong channels
  correlates 0.46-0.97 with each; the DMR decoder itself syncs on some); a
  public 2.4 MS/s capture has copies of every strong carrier at +-12.5 /
  -25 kHz; LoRa packets at -7..-2 dB are below what an energy detector
  sees; NR against other OFDM at 875 MHz is ambiguous in 0.5 s
- Training noise explains 2 of the 85 rows
- What the shipped model really does on local signals: of the 81 channels
  on air in the scored windows of the busy trunked-site captures 66 are
  right (81 %); on 27 further channels identified with the decoders 23
  (P25 14 of 16, DMR 6 of 8, MPT1327 3 of 3)

### Results (2026-11-21): a fair evaluation, core fixes without retraining, global thresholds

The same weights as phase 5 (layout v3, no retraining); what changed is how
the real recordings are scored, three rules around the model (Core: the
tracker's absorption rules, the second look in view 0, the IQ-image test)
and the thresholds in the model's metadata.

- **The evaluation** (`tools/sigclass/eval_real.py`; its docstring has
  every rule; `--selfcheck` feeds synthetic signals four ways and a silent
  and a noise file: 10 of 10, and the OLD feed fails both IF-filtered
  cases): multi-channel files one antenna at a time; a row is scored only
  while on air (the library's `on_air` / `on_air_s`: an intermittent row
  needs >= 2 s on air in a 4 s window or one whole transmission; several
  windows per long capture, each row once); `classes` on a shared channel;
  a row takes the strongest overlapping track (within 10 dB of the
  strongest, then best overlap; a band-wide row only tracks >= 25 % of its
  width); narrow recordings are band-limited to the signal band plus a
  margin and filled at the level beside the signal (the recording's own
  floor and band edges are not presented as a signal), IF-filtered files
  are filled from inside their passband, leading zeros are skipped,
  `notch_dc` attenuates instead of injecting noise; confirmed / probe /
  unconfirmed per row from what the library states; false labels = drawn
  tracks on no row, per capture. The feed may remove what the recording
  device added, never clean the signal itself
- Rows scored / top-1 / family - (1) old evaluation + the core of f045b13,
  (2) new evaluation + that core, (3) new evaluation + this core:

  | set | (1) | (2) | (3) |
  |---|---|---|---|
  | own native captures | 110 / 68 / 69 | 123 / 103 / 104 | 123 / 113 / 112 |
  | ... the busy trunked sites | 57 / 30 / 33 | 83 / 67 / 69 | 83 / 77 / 77 |
  | public, decoder-confirmed | 357 / 177 / 227 | 343 / 187 / 228 | 343 / 188 / 229 |
  | ... without ISM OOK / FSK | 110 / 59 / 78 | 96 / 67 / 77 | 96 / 69 / 79 |
  | discriminator audio | 29 / 11 / 16 | 29 / 13 / 18 | 29 / 13 / 18 |
  | open set (right) | 42 of 51 | 43 of 53 | 42 of 53 |

  Confirmed per class (1) -> (3): DMR 3 of 5 -> 5 of 6, P25 Phase 1 9 of
  19 -> 12 of 15, P25 Phase 2 1 of 2 -> 2 of 5, NXDN 2 of 5 -> 5 of 5, YSF
  1 of 3 -> 3 of 3, TETRA 2 of 3 -> 2 of 3, AIS 0 of 2 -> 2 of 2,
  radiosonde 3 of 14 -> 6 of 14, ISM FSK 21 -> 22 of 98, ISM OOK 97 of 149
  (unchanged), WFM 10 of 13, DAB 9 of 9, VOR 3 of 3. **ISM is now the
  public set's weak spot (128 of its 155 wrong rows) and has not been
  analysed row by row**
- Sure labels (p >= the class's `min_p`, else `min_confidence`), right /
  wrong: own captures 61 / 0 -> 96 / 0 -> 104 / 0 (sure share 0.56 -> 0.85);
  public confirmed 78 / 8 -> 84 / 8 -> 86 / 8. False labels on single-service
  own captures 0 sure / 2 unsure throughout; the busy sites 22 / 33 (mostly
  real channels nobody identified), the intermodulation capture 4 sure
- Synthetic capture-level regression set (2,000 composed captures of 2 s,
  composer seed 31, 6,255 signals, track labels): right 0.631 -> 0.648,
  false labels 176 -> 140 (at a signal's mirror band 61 -> 30); no class
  loses more than one signal; NXDN / P25 / DMR twins at 30 / 45 / 55 dB now
  6 of 6 each (NXDN was 6 / 4 / 2). Detector acceptance byte-identical
- Strict debug playback through a TEST build, two runs: 27 and 28 of 28
  (f045b13: 28 and 27); the miss is one stray "Unknown OFDM 52 %" among
  the 14 tracks of `ism-six-devices`; DMR (73-78 %) and DVB-T (72-73 %) are
  right but unsure
- Cost per snapshot (i7-11800H, load ~3): one narrow signal 21.6 ms, one
  wide 29.7 ms (both unchanged); a busy capture with 31 signals 89 -> 98 ms
  (the second look: 19.5 % of detections on synthetic scenes take it,
  0.27 % take its answer)
- **Thresholds, now fitted without any own capture** (`thresholds.py
  --real-group confirmed`: synthetic validation + the public confirmed rows
  with crc32(entry id) even): the phase 5 values plus radar 0.863, unk_fsk
  0.992 and ism_ook 1.006 (never sure); `min_confidence` 0.82. Against the
  old recipe (which refits to exactly the phase 5 values): own captures 104
  / 0 either way; the held-out public half 44 / 5 (0.90) -> 35 / 1 (0.97),
  sure share 0.28 -> 0.21. The price: an "ISM device (OOK)" label is always
  drawn with "?" - one confidently wrong label in the fitting half does
  that (the rule is zero-tolerance per class; a precision target would be
  less blunt), and the OOK / FSK truth of the rtl_433 files is itself
  unchecked
- Still wrong on the own captures (10 of 123): 6 channels inside 1.4-2 MHz
  max-hold "burst" tracks at one crowded site (the bridging rule was not
  attempted), 1 P25 voice channel merged with the receiver's ~251 kHz hump
  comb, 1 weak DMR channel merged with a neighbour, a LoRa packet below
  the noise, a weak FM station. A dPMR recording (no class) reads "NXDN
  0.98": the model's own output on that channel - dPMR is 4-FSK at 2400
  symbols/s like NXDN48 - which the old tracker bug had hidden

### Why the ISM recordings fail (2026-11-21, the core and model of 59e465e)

The 128 failing ISM rows of the public set (52 OOK, 76 FSK; nearly all
files of the rtl_433 test corpus: one device's burst(s), 0.1-2 s, mostly
250 kS/s, rows without an offset) traced one by one, with OOK / FSK decided
from the IQ itself (envelope keyed at one frequency, or constant envelope
with two frequency levels - the measure finds 183 of the bank's 184 ISM
clips right) and synthetic twins built from each file's measured bit time,
tone spacing, packet times and SNR. Not fixed yet.

- **FSK is a real gap in the training bank: 37 of the 76 FSK rows.** Clean
  twins fail like the recordings (26 of 94 right against 22 of 98; the
  wrong twins read LoRa, unknown FSK, unknown pulses). The pass rate
  follows the bank: bits of 45-65 us (its WH24 / TPMS kinds) 14 of 25
  right, bits >= 130 us 1 of 26, modulation index above 10 3 of 24, packets
  >= 100 ms 0 of 19. 50 of the bank's 80 FSK clips are those two kinds and
  the generic ones never exceed an index of ~5.8; 40 of the 94 measured
  devices are above 5 - slow keyfobs, TPMS, tank gauges with 250-1000 us
  bits and tones 40-120 kHz apart (index 20-60). The 2.4 MHz view has no
  ism_fsk class at all (75 training rows, 100 needed): 0 of 22 real rows
  and 0 of 36 twins right there
- Fragments are the symptom of the same gap: with an index >= 5 the tones
  are detected apart (46 of 90 FSK rows partly or wholly; 12 of 16 above
  20) and each tone alone is an on / off keyed carrier - 17 FSK rows end
  "ISM device (OOK)". The tone-pair merge of the v4 branch did nothing
  because it tests only neighbours in frequency (there are pieces between
  wide-shift tones) and its 427 us frames are longer than most bits, so the
  tones are on together in a frame. Testing all pairs makes 26 fragments
  whole, but only 5 then read ISM FSK - the model does not know that signal
- **The evaluation, 38 rows**: for a row without an offset the track with
  the most hits and the largest overlap with the 250 kHz recording band is
  often a band-edge piece or splatter (rescored against the measured band:
  OOK +17 / -4, FSK +7 / -3); files under 0.5 s are one short snapshot (62
  of those 99 rows fail), files of 0.5-1 s lose their tail
- **Truth: 9 rows have the wrong class** (5 indexed FSK are keyed PWM: three
  KeeLoq, two Fineoffset; 4 indexed OOK are FSK), 14 are undetermined (no
  sample rate, clipped, too short), 5 "OOK" rows are ASK with a residual
  carrier. The one confidently wrong ISM OOK label that made the whole
  class never sure (`min_p` 1.006) is one of them: the label was right.
  With that row corrected the same threshold rule gives ism_ook 0.98 and
  ISM sure labels on the public rows go from 6 right / 1 wrong to 29 / 2
- OOK, ~16 rows: real hard-keyed devices with 0.4-2 ms pulses are detected
  wide (57 % of real OOK tracks in views 2-3, 11 % of the training rows)
  and their envelope and complex autocorrelation decay over ~1 ms where the
  bank's decay within ~0.1 ms; replacing the `frame_*` groups by the class
  mean turns 16 of 49 right, no other group more than 10. Also missing:
  pulses of 10-80 us (meter transmitters), keying depths of only 6-20 dB
- One label or two: as scored 119 of 247; with OOK <-> FSK swaps counted
  right 140 (measured truth and band: 137 -> 160 of 240); sure labels 69 ->
  80. A merge loses only the modulation hint (both start the `ism` plugin);
  it does not touch the 107 rows that end outside ISM (none 33, LoRa 19,
  unknown FSK 16, unknown pulses 15)
- These labels are fragile: another noise-fill seed flips 26 OOK rows,
  showing a burst in a full 0.5 s snapshot instead of a short one 58. Rows
  never right in any of five fits: 35 OOK, 61 FSK
- The stray "Unknown OFDM 52 %" of the `ism-six-devices` playback is a
  keying sidelobe 54-64 kHz below one device's burst: the splatter rule
  wants the piece at most half the burst's width and reaches 48-56 kHz
- The work list this gives: generic FSK devices in the bank with the bit
  time (10 us - 2 ms) and the tone spacing (10-200 kHz) drawn independently
  (index up to ~60), packets of 2 ms - 1 s with repeats, enough of them for
  the 2.4 MHz view (the 94 twins are the synthetic check: 26 right today);
  OOK with long hard-keyed pulses, very short pulses and shallow keying;
  the library's rows with their measured class, offset, width and on-air
  times; then the all-pairs tone merge and a wider splatter rule. The bank
  changes need a retrain, judged over several seeds

### Results (2026-11-22): corrected ISM truth, two detector rules, a wider bank, ensembles - the weights NOT replaced

What changed: the library's ISM truth (9 class changes measured from the IQ,
14 rows `truth: "undetermined"` - out of every score -, 236 rtl_433 rows with
their measured offset, band, SNR and burst times; `eval_real.py` honours
`truth`), the detector's FSK tone merge and keying-sidelobe rule (Core), the
bank additions (Training), all training data regenerated (Z1-Z4, 59 000
captures, test ZT 4 000, with the new detector), seven fits a generation,
averaged ensembles (model format 4). Two generations: **A** with the P25
deviation widening of the brief (0.8-1.6 x in 35 %), **B** without it (the
same caches with those rows dropped - see Training: it laid P25 over DMR /
NXDN / YSF). Every table below on the CORRECTED library; "shipped" = the model
and tool of a7f3708 re-run on it; "final tree" = the shipped weights with this
round's detector (what `sigclass.model` + the code are now). Track level; the
evaluation of 2026-11-21. Exact (family).

| set | shipped | final tree (shipped weights) | A: 7 single fits min / median / max | A: ensemble 1,2,5,6,7 | B: 7 single fits | B: ensemble 1,2,5,6,7 |
|---|---|---|---|---|---|---|
| public decoder-confirmed (336) | 193 (244) | 196 (253) | 245 / 253 / 263 | 263 (296) | 242 / 252 / 258 | 262 (290) |
| ... without the ISM classes (96) | 69 (79) | 69 | 68 / 69 / 70 | 68 | 67 / 69 / 70 | 69 |
| ... ISM (240): two classes | 124 | 127 | 176 / 183 / 195 | 195 | 173 / 182 / 189 | 193 |
| ... ISM: OOK <-> FSK swaps right | 153 | 144 | 198 / 201 / 208 | 216 | 194 / 201 / 209 | 212 |
| ... ISM: family | 165 | 175 | 201 / 206 / 211 | 215 | 194 / 204 / 211 | 208 |
| ... ISM OOK (146) / FSK (94) | 94 / 30 | 94 / 33 | 101-118 / 70-77 | 114 / 81 | 100-116 / 73-78 | 115 / 78 |
| ... half A (165, the tuning half) | 98 | 99 | 117 / 124 / 127 | 131 | 121 / 125 / 127 | 128 |
| ... half B (171, test) | 95 | 97 | 124 / 129 / 136 | 132 | 121 / 128 / 131 | 134 |
| own captures (122 of known class) | 112 | 112 | 104 / 108 / 111 | 110 | 105 / 110 / 113 | 108 |
| ... busy trunked sites (83) / single (35) / IM (4) | 77 / 33 / 2 | 77 / 33 / 2 | 70-76 / 32-33 / 2 | 75 / 33 / 2 | 71-77 / 32-34 / 2 | 73 / 33 / 2 |
| own: sure right / sure wrong | 104 / 0 | 104 / 0 | 73-103 / 0-2 | 101 / 1 | 90-104 / 0-4 | 102 / 0 |
| public confirmed: sure right / wrong | 70 / 0 | 69 / 0 | 55-68 / 0-1 | 116 / 0 | 61-95 / 0-4 | 62 / 1 |
| ISM FSK twins (94) / OOK twins (146) | 26 / 131 | 24 / 133 | 62-76 / 130-136 | 71 / 137 | 60-72 / 127-136 | 71 / 139 |
| synthetic regression set: track right / false labels | 0.649 / 140 | 0.648 / 127 | 0.640-0.645 / 114-131 | 0.648 / 102 | 0.640-0.647 / 101-140 | 0.650 / 109 |
| synthetic val / test top-1 | - | - | 0.889-0.899 / 0.866-0.875 | 0.907 / 0.883 | 0.895-0.902 / 0.870-0.883 | 0.910 / 0.886 |

- **Averaging** (item 12, judged on the tuning data only): generation A, half
  A singles 117-127 (median 124, spread 10), five 3-member ensembles 127-132
  (spread 5), three 5-member ensembles 128-131 (3); synthetic val 0.889-0.899 /
  0.9035-0.9062 / 0.9059-0.9068. Higher than the median single fit and a
  smaller spread: implemented as model format 4 (Core). The public sure share
  rose most (A: 116 right / 0 wrong against 55-68 for one fit)
- **Choice** (synthetic val + public half A only): among the 5-member ensembles
  the highest half A, then val: A 1,2,5,6,7 (131, 0.9068); B 1,2,5,6,7 (128,
  0.9097). Thresholds `thresholds.py --real-group confirmed --prec 0.95`: A
  gives `ism_ook` 0.97 and `ism_fsk` 0.99 (sure possible - a confidently wrong
  half-A ISM row no longer forces "never"), B gives both 1.01 (a half-A ISM OOK
  row wrong at p 1.00): B's ISM labels are never sure. Unknown families (B):
  unk_fsk 1.01, unk_noise 0.87, unk_ofdm 0.82, unk_pulse 0.943, unk_psk the
  global min_confidence 0.70
- **Why not shipped** (the acceptance stated before generation B's numbers
  were seen: own captures at most one row below with no sure-and-wrong label,
  non-ISM public at most one below, playback 28 / 28 in one of two runs and >= 27
  in the other, ISM / twins / public sure labels better, false labels not
  higher): A's ensemble 110 own (-2) with one sure wrong, playback 27 and 27
  (DVB-T read "ATSC 3.0" 71-74 % in both); B's ensemble 108 own (-4) and fewer
  public sure labels (62 / 1 against 70 / 0). The own-capture rows lost:
  P25 channels of the 499.8 MHz site and of the 5-antenna 142.3 MHz capture
  (to DMR, unknown PSK, none - in 1-5 of 7 fits each, both generations) and
  the 526.5 MHz DVB-T band-edge capture ("Unknown OFDM 0.65-0.82" in all 14
  fits; the shipped weights on the final tree still read DVB-T, so it is the
  training, not the detector). Not tuned on: they are test data
- **The final tree** keeps the shipped weights: with the new detector they
  give own 112 (unchanged), non-ISM 69, public confirmed 196 (+3), ISM family
  175 (+10) but OOK <-> FSK-swap score 144 (-9: whole FSK detections the old
  weights never saw), FSK twins 24 (-2), false labels on single-service public
  recordings 9 -> 6 sure, 67 -> 56 unsure, on the synthetic regression set 140
  -> 127. Strict playback through a TEST build of the final tree, two runs: 28
  and 28 of 28 (25 / 24 sure; POCSAG, DVB-T 55-69 %, wildlife or DMR /
  radiosonde unsure); `ism-six-devices` without a stray label (6 / 14 tracks,
  all ISM). Generation A's ensemble: 27 and 27. `eval_real.py --selfcheck` 10 of
  10 (shipped weights and A's ensemble), `selftest` passes, detector
  acceptance set byte-identical, `make kraken_doa sigclass-tool` without a
  warning
- **What remains weak**: P25 against DMR on the weak channels of the two P25
  sites (the new fits; the shipped weights get them); DVB-T read as ATSC 3.0 in
  the playback (both generations; known since phase 3) and the 526.5 MHz edge
  capture; the ISM rows still wrong (~45 of 240) are mostly short files read in
  one short snapshot; the public sure labels of generation B. Not in this
  round: generators for DFM, MRZ, iMet-4, dPMR
- Reproduce: `tmp/retrain/bin/` - `stage_lib.py` (the library staging),
  `frag_twins.py` / `frag_syn.py` (the fragment rates), `gen.sh` (the data),
  `truth_regen.sh` + `drop_devwide.py` (generation B), `fit.sh` / `fit2.sh`,
  `make_all_ens2.sh` (`train.py ensemble`), `evalm.sh` / `evalm2.sh`
  (eval_real + thresholds + `report.py` + twins + the regression set),
  `summary.py`, `syn_eval.py`, `fsk4_val.py`; caches kept in
  `~/.cache/kraken_sigclass/ds_2026-11-22/`; the best new models in
  `tmp/retrain/ev/e12567_thr.model` (A) and `ev/f12567_thr.model` (B)

### Experiment (2026-11-22): other classifiers on the same features - boosted trees and a tree + MLP stack are better; nothing shipped

The question: does another CLASSIFIER on the same 1,086 features and the same
synthetic rows (generation B above) do better than the per-view MLP - more
accurate, steadier across seeds, less prone to one class's data moving
another's? Left out on the user's instruction: decoder confirmation and deep
networks on raw samples / spectrograms. Nothing in the tree changed.

**Harness** (only the classifier is swapped): a scratch build of the core
dumps every detection of every evaluated capture with its features, plus the
view-0 second look of every detection that qualifies; Python applies the
second-look rule and the RF mask to any method's per-view probabilities; a
tracker-only replay mode of the tool turns them into tracks; scoring is
`eval_real.py`'s own functions and `thresholds.py`'s recipe. Validated before
use: numpy against the dumped C++ max |dp| < 1e-6 with every second-look
decision equal; with the shipped weights the library table equals
`eval_real.py`'s in every column (checked again against the table of the
round above: 663 of 663 rows the same label; the averaged model 662 of 663),
the regression set gives 0.648 / 127 and the playback emulation 28 of 28; the
in-harness MLP trainer reproduces a fit of the round above to 1.6e-6. One
method costs ~15 s to evaluate. The playback column is that offline emulation
(each entry looped 40 s at the live cadence, the service's publish rule), not
a live run.

Choices (the LightGBM configuration, stack weights, thresholds) used the
synthetic validation split and public half A only; half B and the own
captures were looked at once per finished method. Cells are min / median / max
over seeds or draws (count in the header). LGB = LightGBM 4.7.0, one model
per view, 7 leaves, learning rate 0.05, early stop on val macro recall, the
trainer's class weights; the stack averages probabilities with the weight
picked by val NLL (0.3 MLP / 0.7 LGB).

| | shipped weights | MLP single (7) | MLP 5-average (6) | LGB single (5) | LGB 3-average (5) | MLP 5-avg + LGB (5) | 1 MLP + 1 LGB (5) |
|---|---|---|---|---|---|---|---|
| synthetic val / test top-1 | .895 / .866 | .900 / .876 | .910 / .885 | .914 / .881 | .915 / .880 | .920 / .889 | .917 / .888 |
| val cross-confusions 4-FSK / OFDM / ISM | 38 / 66 / 31 | 43 / 87 / 22 | 36 / 56 / 20 | 29 / 31 / 22 | 29 / 27 / 21 | 28 / 21 / 19 | 30 / 26 / 20 |
| public decoder-confirmed (336) | 196 | 242 / 252 / 258 | 257 / 258 / 262 | 270 / 278 / 281 | 274 / 278 / 280 | 279 / 279 / 281 | 270 / 279 / 280 |
| ... without ISM (96) | 69 | 67 / 69 / 70 | 68 / 69 / 69 | 69 / 71 / 72 | 69 / 70 / 71 | 70 / 71 / 71 | 70 / 71 / 72 |
| ... ISM, two classes (240) | 127 | 173 / 182 / 189 | 188 / 189 / 193 | 201 / 207 / 209 | 204 / 208 / 209 | 208 / 209 / 210 | 200 / 208 / 209 |
| ... half A (choice) / half B (test), median | 99 / 97 | 125 / 128 | 127 / 132 | 136 / 140 | 138 / 140 | 138 / 142 | 136 / 141 |
| own captures (122) | 112 | 105 / 110 / 113 | 108 / 109.5 / 112 | 109 / 111 / 112 | 111 / 111 / 111 | 111 / 111 / 111 | 111 / 111 / 112 |
| ... P25 channels of the busy sites (50) | 49 | 43 / 47 / 49 | 45 / 46.5 / 49 | 47 / 49 / 49 | 49 / 49 / 49 | 49 / 49 / 49 | 49 / 49 / 49 |
| own: sure right / sure wrong (median; range of wrong) | 104 / 0 | 95 / 0 (0-4) | 103 / 0 (0) | 108 / 2 (1-4) | 108 / 3 (2-3) | 109 / 2 (2-3) | 109 / 2 (0-3) |
| public half B: sure right / wrong (median) | 35 / 0 | 31 / 0 | 31 / 0 | 30 / 2 | 30 / 2 | 44 / 3 | 44 / 4 |
| FSK twins (94) / OOK twins (146), median | 24 / 133 | 67 / 134 | 68.5 / 138 | 73 / 137 | 75 / 136 | 75 / 141 | 73 / 140 |
| synthetic regression set: right / false labels, median | .648 / 127 | .645 / 120 | .648 / 101.5 | .652 / 90 | .654 / 85 | .658 / 65 | .656 / 80 |
| playback emulation (28): right, sure (median) | 28, 26 | 27, 25 | 27, 25 | 28, 27 | 28, 27 | 28, 28 | 28, 28 |
| global `min_confidence` the recipe gives (median) | 0.82 | 0.81 | 0.70 | 0.73 | 0.72 | 0.64 | 0.64 |

The others (medians): extra-trees public 275, non-ISM 68, own 105,
regression set .610 / 62, 255 MB; multinomial logistic regression 225 / 68 /
108 / .633 / 124; class prototypes in the MLP's last hidden layer 252 / 70 /
107 / .637 (many rows end hidden); a family stage then a specialist per
family, MLP stages 238 (227-258) / 70 / 111 / .630, LGB stages 268 / 71 / 110
/ .639; the flat model with specialists only inside the confusable groups
(4-FSK, OFDM, ISM OOK / FSK), MLP 251 / 69 / 110 / .643, LGB 269 / 71 / 111 /
.654. DMR 5 of 6, P25 12 of 15, P25 Phase 2 2 of 5, NXDN 5 of 5 and YSF 3 of
3 on the public rows with EVERY method: those rows are not the classifier's
to win.

- **LightGBM is better than the MLP by more than the seed spread**: public
  278 against 252 (single) and 258 (five averaged), the regression set's
  false labels 85-90 against 101-120, the playback emulation 28 of 28 in every
  fit with DVB-T right, half as much seed churn (below). Almost all of the
  gain is ISM (+18 to +20 of 240); the rest of the public rows gain 2 of 96
  (radiosonde, FLEX). The own captures do not gain (111 against 108-112): the
  busy sites' P25 channels come back to the shipped weights' 49 of 50, the
  DVB-T band-edge capture reads "Unknown OFDM" in every new fit of every
  method, 5G NR reads LTE (unsure, 0.52-0.61) in the stacks
- **The stack is the best and the steadiest** (public spread 2, own 111 in
  all five draws, regression set .658 / 65, playback 28 right and 28 sure);
  one MLP + one LGB keeps nearly all of it
- **The open cost is trust**: the tree-based methods are sure and wrong on
  2-3 own rows (the averaged MLP on none) and on 2-4 public half-B rows.
  The own rows are in every fit the two MPT1327 control channels of the
  intermodulation-dominated 416.5 MHz capture, read "carrier" at 0.84-0.86;
  the public ones sit at p 0.65-0.72 - above the 0.64 the threshold recipe
  gives the stack, below the shipped 0.82: the recipe trades a larger sure
  share (half B 0.28 against 0.18) for them. A stricter precision target was
  not tried
- **The isolation test** (each method trained with and without the new ISM
  kinds, the bank's ism_fsk clips 80-239 and ism_ook 104-203; decisions
  changed on NON-ISM signals: 40,182 synthetic val rows / 96 public / own):

  | | seed alone: val / public / own | data change: val / public / own | public ISM, full -> without |
  |---|---|---|---|
  | MLP single | 2603 / 15 / 5 | 2626 / 14 / 8 | 182 -> 124 |
  | LGB single | 1246 / 3 / 5 | 1447 / 3 / 6 | 207 -> 131 |
  | family stage + specialists, MLP | 2398 / 9 / 7 | 1900 / 7 / 8 | 168 -> 121 |
  | family stage + specialists, LGB | 1022 / 7 / 6 | 1008 / 3 / 6 | 197 -> 122 |
  | MLP 5-average (against a disjoint five) | 1115 / 10 / 5 | 1331 / 9 / 6 | 189 -> 139 |
  | MLP 5-avg + LGB 5-avg | 465 / 3 / 1 | 728 / 2 / 2 | 208 -> 137 |

  For the flat MLP, taking the new ISM data away changes as many non-ISM
  decisions as a different seed does: the "unrelated decisions moved" of the
  round above (P25 to DMR, DVB-T to ATSC 3.0) was seed noise, not one
  family's data disturbing another's - and the shipped weights' own-capture
  score sits at the top of the single-fit range. An ISM specialist bolted
  onto the model trained without the new kinds is perfectly isolated (0
  public non-ISM decisions change) and recovers 5-9 of the ~58 ISM rows: the
  gain comes from the shared model routing the new kinds INTO the ISM family
  (away from unknown FSK / LoRa / none), not from the OOK / FSK split. No
  method keeps ISM's gain and leaves the rest where it was; what reduces the
  churn is averaging and trees
- **Not worth pursuing**: the hierarchy (worse on ISM, the twins and the
  regression set), specialists where needed (no gain), extra-trees, logistic
  regression, prototypes. Logistic regression still gets 108 own and 68
  non-ISM: the features do most of the work outside ISM
- **Cost**: MLP 0.05 ms per detection (3.7 MB, ~2 min to train); one LGB
  0.31-0.48 ms in LightGBM's own C++ predictor on one core of the loaded dev
  machine (view 0 is 6-8 thousand trees; 1.9-3.2 MB; 10-19 min to train),
  estimated 0.7-1.2 ms on a Pi 5 - not measured there, and against the ~0.5 ms
  the model was budgeted; 1 MLP + 1 LGB ~0.35 ms, 5 MLP + 5 LGB 1.7-2.9 ms.
  Shipping it would need a tree evaluator in `model.cpp` (a member type of
  model format 4, bounded loading, fuzzing, parity against LightGBM),
  LightGBM (MIT) as a training-only dependency, and thresholds that deal with
  the "carrier" case
- **Not tested**: XGBoost, a random forest, one-vs-rest, feature-group
  dropout; the LGB grid was small (5 configurations on view 1, 2 full
  candidates); the LGB and stack validation numbers are optimistic (early
  stopping and the stack weight used val); timings under load
- **Decision (Carl, 2026-11-24): the fast per-view MLP stays; no tree model
  or stack is built.** The gain was almost all ISM devices, and those are
  not critical - the band a signal is in already says "ISM" (the page's
  Bands bar names it). Do not reopen this without a new reason; the shipped
  weights stay as they are
- Kept in `~/.cache/kraken_sigclass/arch_2026-11-22/` (`bin/` the harness and
  the methods - paths point at the job scratch -, `src/` the dump / replay
  tool, `sets/` the cached dumps with the pinned library index, `tables/`
  the full 79-row table and the churn table, `models/` the LGB, MLP,
  averaged and stacked models) and `ds_2026-11-22/genB/` (the generation-B
  rows)

### Investigation (2026-11-24): can a look be made faster? Yes - prototypes on branch `sigid-speed`, nothing adopted

The question (Carl): "Is there any way to further optimize the
classification speed?", with the MLP kept as it is. Two meanings: the CPU
of one look (0.5 s of one tuner: detector -> views -> features -> model ->
tracker), and the time until a signal has its label - the service looks
every `clamp(look / 0.04, 1 s, 10 s)`, so a cheaper look is an earlier next
look once a look costs more than 40 ms (a busy band here; every band on a
Pi 5). All numbers: i7-11800H, one pinned core, thread CPU time, the
minimum over alternating rounds on a shared machine (no `perf` here, so no
cycle counts). The Pi 5 was NOT measured.

**Where a look's time goes** (the shipped core, ms):

| stage | 0 signals | 1 narrow | 1 band-filling | 8 (ISM) | 16 narrow | 25 mixed | 32 narrow (P25) |
|---|---|---|---|---|---|---|---|
| whole look | 21.9 | 23.9 | 33.8 | 62.8 | 51.9 | 102.3 | 92.8 |
| convert, long FFTs, short spectrogram | 13.7 | 13.3 | 13.4 | 13.7 | 14.1 | 13.7 | 12.4 |
| detector: noise floor, burst search, band-fill | 9.8 | 9.5 | 9.7 | 9.9 | 9.9 | 9.5 | 8.6 |
| detector: FSK tone-pair test | 0 | 0 | 0 | 9.3 | 2.2 | 9.4 | 6.1 |
| detector: per-detection measurements | 0 | 0.1 | 5.3 | 2.1 | 1.5 | 2.5 | 3.2 |
| channelizer | 0 | 0.06 | 1.4 | 5.3 | 0.9 | 6.0 | 2.2 |
| features | 0 | 1.6 | 5.3 | 23.7 | 24.7 | 66.7 | 69.2 |
| model | 0 | 0.06 | 0.06 | 0.7 | 0.8 | 1.8 | 2.0 |
| tracker | <0.01 | <0.01 | <0.01 | <0.01 | 0.01 | 0.02 | 0.04 |

A fixed ~22 ms (transforms and the detector's searches) plus 1.5-2.5 ms of
features per detection (a full-rate view-3 signal ~8 ms). The model is 2 %
of a busy look. The model reads 1,056-1,081 of the 3,134 values; the unread
ones (`symseq`, constants) cost < 0.01 ms to compute - not worth removing.

**The candidates** (commits on `sigid-speed`, base 9751af3; each its own
commit):

| | change | saving | the answers |
|---|---|---|---|
| P1, P5 | burst search: four frames' prefix sums side by side; a branch-free "any box over threshold" test per scale first (2230845, cc58392) | ~5 ms of every look (7.7 -> 2.5 ms) | bit-identical |
| P2 | tone-pair test: band powers cached per round, packet frames counted in one pass (0280c64) | 1.3-2.2 ms of a busy look | bit-identical |
| P4 | the symbol group's short Hann window from the window cache (187e076) | ~0.05 ms a detection | bit-identical |
| P3 | a branch-free `fast_atan2` and two-loop discriminators that vectorise (a204743) | 0.1-0.2 ms a detection | NOT identical: `-ffast-math` reorders the sums, the last bits of the IF move and the symbol group's rate / timing choice can flip |
| P1-P5 | together | 4.0 / 4.2 / 9.3 / 7.5 / 12-14 ms at 0 / 1 / 8 / 16 / ~30 signals = 13-18 % | as P3 |
| S1 | a track that is settled (>= 3 hits) and sure is not classified again in every look - its detection is only tracked; it is still classified in one look of four (core: an opt-in list of known bands for `analyze()`, 5926b61; service: d98a3c9) | on top of P1-P5, steady state: busy bands a further 24-36 % (27-51 % of the detections skipped) | changed, slightly: playback emulation 28 of 28, every published label the same; six own captures replayed 150 s each: 99.3 % of the published labels the same, 0.5 % different, 0.2 % missing |
| S1b | the same for every track with >= 3 hits, sure or not | a further 10-20 % | rejected: 5 % of the published labels differ, the playback loses a sure label |

Not prototyped (estimates): three quantiles from one copy in the
per-detection statistics, no reorder pass in the spectrogram's FFT, the
uint8 conversion fused into the first transform - about 2 ms together, same
answers; a spectrogram hop of 1024 instead of 512 - 3-4 ms, changes the
burst statistics. Not worth it: skipping unread features, a faster model,
the display's spectrum for the detector (wrong resolution, and the views
need the long FFTs anyway), shorter snapshots / fewer views / a lower cap on
detections (they change the model's inputs).

**Checked by the orchestrator** (an own set: 154 captures of the synthetic
regression set and the 21 own 2.4 MS/s captures, 164,047 detections;
`classify --track` and `features` compared byte for byte; scripts in
`~/.cache/kraken_sigclass/speed_2026-11-24/`): the core at P1 + P2 is
BYTE-IDENTICAL to the shipped one (features and output) and ~4 % faster on
that set; the core at P1-P5 is ~14 % faster (the minimum of five alternating
rounds) and differs in 64 of 164,047 detection labels (0.04 %; every one a
guess below p 0.6), 74 of 245,216 track rows, 134 detections cross 0.82 in
either direction, max |dp| 0.24 - that is P3; P4 and P5 alone were
bit-identical on the worker's set (1,280 snapshots) and are not separated
in the orchestrator's. The regression set scores the same with P1-P5
(0.648, 127 false labels). S1 was not re-measured; its service half was
never run in a receiver (syntax-checked only), and it touches the look loop
the multi-tuner rules of the same day changed.

**What it would mean on a Pi 5** (an ESTIMATE from the documented 3.5 x
factor, not a measurement): a look on an empty band 77 -> 63 ms; on a busy
band ~360 -> ~310 ms (P1-P5) -> ~205 ms (with S1), so the period there 8.9
-> 7.7 -> 5.1 s; with five tuners on busy bands a round 44 -> 26 s.
A one-command benchmark for a Pi (builds the shipped core and the branch's,
runs both alternately on 11 snapshots, prints the stage tables) is in
`~/.cache/kraken_sigclass/speed_2026-11-24/pkg/` (`./run_pi.sh`; eight of
its snapshots are cut from own captures: local use only).

**Open decision.** Recommended by the worker and the orchestrator: adopt
P1, P2, P4, P5 (same answers); P3 with them if 0.04 % of low-confidence
guesses moving is accepted (a different compiler moves more: the shipped
core built with clang 14 instead of GCC 11 changes 1 of 1,150 labels); S1
only on Carl's word, after a run in a receiver and on top of the
multi-tuner rules.

### Results (2026-11-20, phase 5, the weights still shipped)

The phase 4 weights (no retraining), now model format 3 with the RF table,
per-class thresholds refitted (`thresholds.py --prec 0.95` on the synthetic
val split + half A of the own captures, evaluated WITH the RF mask, false
labels included: 16 classes with `min_p`), the tracker's evidence over ~10
looks (alpha 0.1), the one-hit rule and the page's per-track sure level.
Before = commit 6d51b53's model (layout v2, no mask possible in its core) /
the phase 4 model (as shipped then: unmasked, old tracker).

- **(a) debug playback, strict**: 6d51b53 27 / 28, phase 4 26 / 28, now
  28 / 28 in the second full run (26 sure; DMR 80 % and DVB-T 71 % unsure)
  and 27 / 28 in the first (APRS: no track). `dmr-repeater-and-direct` three times live: DMR 0.92,
  0.94, 0.92 (phase 4 live "P25 Phase 1 50 %"; the cause and the fix are in
  Core / Tracker); FLARM right (its one-hit "ISM device (FSK) 52 %" piece
  is no longer published); DVB-T right (72-81 %, unsure). APRS: in 2 of 7
  live runs of the entry no track at all at the end (offline 0.99, the
  other live runs 0.98-0.99): not explained - see "still weak"
- **(b) own native captures** (110 signals): top-1 65 / 67 -> 68 (62 %;
  unmasked 66), family 70 / 70 -> 69, shown correct 52 / 60 -> 61. The mask
  fixed the FM station at -600 kHz (FM 3 -> 5 of 6: "LoRa" is implausible
  in 88-108 MHz) and the false ILS label on the DME capture's band-edge spur
  (+1128 kHz, ils_loc 0.88 SURE unmasked with the phase 4 thresholds);
  the 1290 MHz empty band shows no label (the same spur cannot be ILS at
  1.29 GHz). False labels (drawn, p >= 0.5, at no identified signal):
  single-service captures 0 sure / 2 unsure (a one-hit ISM FSK piece at
  433.88 MHz; an "Unknown OFDM 0.58" band-edge artefact at +1097 kHz on the
  DME capture); the busy trunked-site captures (`mixed/`: P25 / DMR /
  MPT1327 sites) 40 sure / 49 unsure on channels their `expect` rows don't
  list - mostly real transmitters the decoders didn't identify, reported
  apart and not used to fit thresholds
- **(c) library, decoder-confirmed** (357; 201 of 338 entries have a
  centre frequency): top-1 157 / 170 -> 177 (50 %; unmasked 171), family
  220 / 219 -> 227 (unmasked 218), shown correct 102 / 108 -> 117. Masked vs
  unmasked rows: FM 8 -> 10 of 13, ISM FSK 19 -> 21, LoRa 2 -> 3, P25
  family 12 -> 13; DMR 3, P25 9, NXDN 2, YSF 1, TETRA 2, radiosonde 3, ISM
  OOK 97 of 149
- **(d) synthetic** (XT, unmasked): unchanged from phase 4 - top-1 0.855,
  family 0.874
- **Trust** (sure = p >= the class's `min_p` or `min_confidence`; track
  level; own half B and the library are test data):

  | | sure right | sure wrong | precision | sure share | false labels (single-service captures) |
  |---|---|---|---|---|---|
  | 6d51b53, own half B | 31 | 6 | 0.84 | 53 % | not scored then |
  | phase 4, own half B | 38 | 0 | 1.00 | 54 % | (unmasked) 1 sure (ILS on a spur), 1 unsure |
  | now, own half B | 39 | 0 | 1.00 | 56 % | 0 sure, 1 unsure |
  | 6d51b53, library | 102 | 41 | 0.71 | 40 % | - |
  | phase 4, library | 69 | 8 | 0.90 | 22 % | - |
  | now, library | 78 | 8 | 0.91 | 24 % | - |

  The library's 8 sure-and-wrong now: ISM FSK as ISM OOK (current cost,
  fineoffset, Honda remote, two KeeLoq), as unknown OFDM (Jansite TPMS), as
  LoRa (Insteon 915 MHz); ISM OOK as unknown FSK (ELV WS2000)
- **Checks**: selftest passes; detector acceptance unchanged; parity
  formats 1-2 1.9e-7, the RF mask 2.9e-7 (8 frequencies x 4 views); the
  fuzzer with a format-3 model (every truncation, 400 000 byte changes,
  hostile u32 fields; every masked result finite and summing to 1): no
  fault
- **Still weak**: the APRS entry's intermittent live miss; real ISM FSK
  (21 / 98) and the ISM FSK <-> OOK swaps that the thresholds cannot catch
  (they are confident); the busy-site captures' unidentified channels (the
  library lacks the truth to score them); LoRa, SSR and NR confidence on
  the own captures; the learned front end (off) - all as in phase 4

### Results (2026-11-20, phase 4)

Layout v3 (hash cf9d37c1ca7226ea, D = 3134 incl. the 2048-value symbol
sequence), model format 2, 1086(+seq not read)-192-96-C per view, 3.8 MB,
`min_confidence` 0.82 plus per-class `min_p` for 16 classes
(`thresholds.py --prec 0.95`: am_voice 0.99, atsc3 0.87, beacon406 0.98,
carrier 0.95, cw / unk_pulse / wildlife 1.01 = never sure, ism_fsk / ism_ook
/ unk_fsk 0.98, lora 0.92, unk_noise 0.97, unk_ofdm 0.93, unk_psk 0.96,
vdl2 0.99, wfm 0.87). Trained on 43 000 captures (X1 24 k, X2 16 k, X3 3 k
wide), test XT 4 000. "Before" = the committed 6d51b53 model (layout v2)
with its own tool on the same library state; real tables at track level.

**The learned front end does NOT ship.** Trained with it (Conv1D 2 x 24 ->
32 filters, max + mean pooled, views 0-1; `fit --conv-views 0,1`) the model
was no better on real data than the same data without it: native 65 vs 67
of 110, confirmed 158 vs 170 of 357; the 4-FSK rows identical (DMR 3 / 5,
P25 9 / 19, NXDN 2 / 5, YSF 2 / 3, native P25 16 / 34, DMR 12 / 21);
synthetic 0.841 vs 0.855; playback 26 / 28 both (it got DMR right and DVB-T
wrong, the shipped one the reverse). Its code stays (layout v3 sequence,
format 2 front end, trainer) - sound, tested, and off in the shipped model
(every view: front end 2, the sequence not read). What it learned is real:
on bank clips and real recordings the most excited filters fire once per
frame - DMR every 288 symbols (and 144: the 30 ms slots; real DMR control
channel: 288 / 144 exactly), P25 voice 396, a real P25 control channel 360
(75 ms TSDUs) on the same filter (f25) that fires on the bank's P25,
NXDN 192, TETRA 255. Why it doesn't help yet: the real 4-FSK signals the
model misses are mostly the ones whose sequence is unusable - the symbol
rate is found in 62-65 % of the real 4-FSK detections only (own trunked-site
captures: weak channels, neighbours), and with a wrong rate the sequence
is noise; where the rate is right the plain features already separate the
systems. Next step there: timing recovery that holds at low SNR (a
matched-filter / Gardner loop on the channel-filtered signal instead of the
|dIF| line), then retrain with the front end.

- **(a) debug playback, scored strictly** (`pb_strict.py`: the entry's own
  class or the class it is merged into - the old table accepted any class of
  the plugin and counted "ATSC 3.0" on the DVB-T entry right): before 27 / 28
  (DVB-T as ATSC 3.0 68 %; sure 25), after 26 / 28, sure 23, unsure 3.
  Fixed: DVB-T (DVB-T / T2 / ISDB-T 78 %), radiosonde 35 -> 85 %, wildlife
  66 -> 85 %. Wrong: DMR (the repeater + direct-mode entry ends "P25 Phase 1
  50 %", DMR 36 %; the offline emulation of the same entry: DMR 97 %),
  FLARM (a one-hit "ISM device (FSK) 52 %" piece next to the three FLARM
  tracks, which are right). Target 28 / 28 not reached
- **(b) own native captures** (110 signals): top-1 65 -> 67 (59 -> 61 %),
  family 70 -> 70, shown correct 52 -> 60. Fixed: LTE 768 MHz (none 0.99 ->
  LTE 0.95: the burst-comb band fill), NR 875 (72 none tracks -> one NR track,
  but p 0.23: hidden), the empty 1290 MHz band shows nothing, DME 0.53 ->
  0.65. Not fixed: SSR 1030 (none 0.43), LoRa 921.8 (now a hidden
  band-filling track, p 0.19), FM 98 MHz -600 kHz still "LoRa 0.62", VOR
  (unknown OFDM 0.46), ISM 433 (FSK, 0 / 3)
- **(c) library, decoder-confirmed** (357): top-1 157 -> 170 (44 -> 48 %),
  family 220 -> 219, shown correct 102 -> 108. Rows exact (family): DMR 3
  (5) -> 3 (3), P25 9 (14) -> 9 (12), NXDN 2 (3) -> 2 (3), YSF 2 (3) -> 2
  (3), TETRA 3 -> 2 of 3, POCSAG 1 -> 1, FLEX 2 -> 1 of 2, AIS 0 -> 0 (family
  2), ACARS 1 -> 1, VDL2 0 -> 0, radiosonde 3 (11) -> 3 (11) of 14, ISM OOK
  79 (103) -> 95 (110) of 149, ISM FSK 23 (39) -> 19 (33) of 98 (still
  worse than phase 2's 28), ISDB-T 0 (none) -> 0 (none), LTE 1 -> 3 of 5,
  NR 0 -> 1, LoRa 1 -> 2 of 4, FM 9 -> 8 of 13, DAB 9 / 9. Probe-only 23 ->
  27 %, disc_audio 31 -> 31 %, open set 82 -> 80 %
- **(d) synthetic** (XT, test clips): top-1 0.855, top-3 0.942, family
  0.874; per view 0.888 / 0.824 / 0.868 / 0.734 (not comparable 1:1 with
  phase 3's 0.875 on VT: other composer seed contents - the band-edge
  humps, the MS-sourced DMR)
- **(C) trust: how often a SURE label is right on real data** (track level;
  sure = p >= the class's `min_p` or `min_confidence`; unsure = 0.5 .. that;
  hidden = < 0.5, `none` or no track; thresholds set on the synthetic val
  split + half A of the own captures - crc32 of the capture id even -, half
  B and the public library untouched):

  | | sure right | sure wrong | precision | unsure right / wrong | hidden | sure share |
  |---|---|---|---|---|---|---|
  | before, own half A | 21 | 0 | 1.00 | 0 / 1 | 16 | 55 % |
  | before, own half B | 31 | 6 | 0.84 | 5 / 3 | 25 | 53 % |
  | before, library | 102 | 41 | 0.71 | 32 / 45 | 135 | 40 % |
  | after, own half A | 22 | 0 | 1.00 | 1 / 1 | 14 | 58 % |
  | after, own half B (test) | 38 | 0 | 1.00 | 1 / 7 | 24 | 54 % |
  | after, library (test) | 69 | 8 | 0.90 | 76 / 59 | 143 | 22 % |

  Before, the sure-and-wrong own captures were all 4-FSK called YSF (5 x
  P25, 1 x DMR) and one P25 called unknown FSK; on the library 41, 30 of
  them ISM (OOK / FSK swapped, OOK as unknown pulses / OFDM). After (the
  library's 8): APRS as noise-like, ISM FSK as unknown OFDM (Jansite TPMS,
  oil watchman), ISM OOK as unknown OFDM (Acurite 3-in-1), a P25 site as FM
  broadcast (an off-channel 2.4 MS/s capture), a P25 Phase 2 as unknown PSK,
  an M10 radiosonde as FM broadcast, a wildlife collar as unknown pulses.
  Without the per-class thresholds (min_confidence only) the library's sure
  labels were 0.80 right at 38 % sure: the per-class `min_p` buys 0.90 at
  22 % (the ISM classes need 0.98)
- **Cost** (load ~1.7, the 6d51b53 tool alternated with this one, 2026-11-20):
  fixed per snapshot 21 ms both; features per narrow signal 1.25 -> 1.44 ms
  (the channel filter + the sequence), typical mix 1.61 -> 1.84, wide 3.84
  -> 3.99 ms; model 0.05 ms per signal (the front end would be 0.15-0.17 ms:
  1.5 M multiply-adds); model file 3.7 -> 3.8 MB. Pi 5 (an ESTIMATE, not
  measured: 3-4x this machine per core): ~70-90 ms fixed, ~5-7 ms per
  narrow signal, ~15 ms per wide one
- **Checks**: `sigclass_tool selftest` passes; detector acceptance
  unchanged after every phase 4 rule (Pd 1.00, extras as before, noise-only
  1 in 40); parity numpy vs C++ on random format 1 / 2 models 1.7e-7 and on
  the trained models over 2 400 real cache rows 4.9e-7 (front end, not-read
  and plain views); the fuzzer (ASan + UBSan, now also a format-2 model with
  a front-end view and a not-read view): ALL DONE, no fault
- **Still weak, and the next steps**: (1) symbol timing at low SNR (above),
  then the front end again with the MS-sourced DMR and P25 control-channel
  clips it now has; (2) ISM FSK on real files (rtl_433 corpus 19 / 98: the
  bank's two FSK device kinds + the generic ones don't cover the real
  deviations / bit rates - more generic FSK devices from published device
  parameters); (3) LoRa: real packets come out as a band-filling or `none`
  track - the burst-comb fill now also fires on a busy LoRa band; (4) NR /
  SSR / VOR confidences on the own captures; (5) a 1-hit transient track
  can show a "?" label (FLARM): the service could publish a track only from
  its 2nd hit; (6) the real-data variance between training sets (ISM OOK 29
  - 95 of 149 for equal recipes): an ensemble or averaging several seeds
  would make the shipped model's real behaviour less of a draw
- Commands: `train.py gen` X1 / X2 / X3 / XT as in the README; `train.py
  fit --data X1,X2,X3 --test XT --hidden 192,96 --epochs 40 --none-weight
  2.5 --conv-views none`; `thresholds.py ... --prec 0.95`; `eval_real.py`;
  scratch `trust.py`, `pb_strict.py`, `rs_hit.py`, `filt_probe.py`

### Results (2026-11-20, phase 3: the model of commit 6d51b53)

Model 1082-192-96-C per view, 3.7 MB (was 2.84), `min_confidence` 0.81
(95 % of the shown val labels right at 76 % shown); views (classes) 24 kHz
32, 96 kHz 22, 480 kHz 17, 2.4 MHz 22. Trained on 59 000 captures (V1-V4,
0.44 M detections; 0.26 M of train clips), tested on 4 000 of another seed
(VT, test clips only). Real tracks: the tracks of the last 0.5 s snapshot
of 4 s (track level). "Before" = the phase 2 model with the v1 tool of
commit 8b7c707 on the SAME (new) evaluation path and library state.

- **(a) debug playback through the real TEST pipeline** (28 entries, 40 s
  each): 28 / 28 right (25 with the label at the transmitter, the 3 whose
  expected frequency is the VFO centre between channels - AIS, ISM, FLARM -
  have every channel track right); 28 sure, 4 unsure, 0 wrong (phase 2: 32
  sure, 0 unsure). Unsure: radiosonde 35 % (both sondes), wildlife 66 %,
  DVB-T labelled ATSC 3.0 68 % (the table accepts any DTV class; with the
  v2 model trained unmerged it was DVB-T 86 %). One-track-per-transmitter
  check (`frag_test.py --pb`, 30 entries): 1.02 tracks per channel, 22
  exactly right (phase 2: 1.05, 23)
- **(b) own off-air captures, native bytes** (110 expected signals - the
  library now has own DMR / P25 / MPT1327 / NFM captures): top-1 57 -> 65
  (52 -> 59 %), family 70 -> 70, shown correct 47 -> 52. DVB-T 18 -> 19 /
  19, DMR 12 -> 12 / 21 (8 not detected: weak), P25 10 -> 16 / 34 (wrong:
  YSF 5; 9 not detected - in the 5-antenna 142.3 MHz capture the expected
  channels show only as 1.2-1.4 kHz lines; its phase 2 "detections" were
  the false band-filling track), MPT1327 5 / 7, NFM 4 / 7, FM broadcast 3 /
  6 (3 weak ones "LoRa"), ISM 433 0 / 3 (ISM FSK), LoRa / LTE / NR / SSR /
  VOR none (hidden)
- **(c) library, decoder-confirmed** (357 signals): top-1 135 -> 157 (38 ->
  44 %), top-3 54 -> 61 %, family 196 -> 220 (55 -> 62 %), shown correct 75
  -> 102. Rows (exact / family, before -> after): DMR 1 / 5 -> 3 / 5, P25
  4 / 11 -> 9 / 14 of 19, NXDN 0 / 1 -> 2 / 3 of 5, YSF 1 / 3 -> 2 / 3 of
  3, TETRA 1 / 1 -> 3 / 3 of 3, POCSAG 0 / 1 -> 1 / 1, FLEX 1 / 2 -> 2 / 2,
  AIS 0 / 2 -> 0 / 2 (ISM FSK), ACARS 1 -> 1, VDL2 0 -> 0 (of 1), radiosonde
  3 / 9 -> 3 / 11 of 14 (the bank's types only), ISM OOK 65 / 77 -> 79 /
  103 of 149, ISM FSK 28 / 51 -> 23 / 39 of 98 (WORSE: to ISM OOK 17, none
  16, unknown FSK 13), FM 9 -> 9 of 13, DAB 8 -> 9 of 9, ISDB-T 1 -> 0 (none).
  With the phase 2 path the same model had DMR 0, P25 6, NXDN 1. Probe-only
  13 -> 23 %, unconfirmed 7 -> 7 %, disc_audio 17 -> 31 %, open set 84 ->
  82 % labelled unknown / none / below `min_confidence`
- **(d) synthetic test** (VT: new composer - more strong signals, IQ
  images, deviation tolerance - so not comparable 1:1 with phase 2's B2
  0.867): top-1 0.875, top-3 0.950, family 0.891, macro recall 0.819 /
  precision 0.791 (49 classes); per view 0.873 / 0.898 / 0.881 / 0.760
  (family 0.886 / 0.908 / 0.897 / 0.830); by family exact / family: fsk
  0.80 / 0.85, pulse 0.84 / 0.89, am 0.83 / 0.87, ofdm 0.83 / 0.84, psk
  0.89, fm 0.93, none 0.91; top-1 vs SNR 0.59 (< 0 dB), 0.78 (0-5), 0.86
  (5-10), 0.89-0.95 (>= 10). Weak: radar 0.38 (-> SSR 1030), unknown FSK
  0.26 (-> APRS), am_voice 0.40, cw 0.59. The first v2 fit without the
  near-strong / image rules: top-1 0.840, `min_confidence` 0.98 at 38 %
  shown
- **Unknown vs ISM** (item of the brief): ISM OOK -> unknown pulses and ISM
  FSK -> unknown FSK stay errors in the exact score; the family score counts
  them right (OOK and pulses are one family, FSK one) - the honest answer
  for a device the bank lacks. No class definition changed
- Where it stands, and why: P25 / DMR / NXDN on real files are about half
  right now (DMR 3 / 5, P25 9 / 19, NXDN 2 / 5; native P25 16 / 34 and
  DMR 12 / 21) - the rest go to YSF / unknown FSK (deviation tolerance makes
  the 4-FSK systems overlap in level; the frame lag is the separating cue
  and it is weak in 0.5 s); radiosondes other than RS41 / M20 have no
  generator; real ISM FSK got worse (19 % from 29 %); LTE / NR at low load
  and LoRa / SSR in the own captures are still hidden as none
- Commands: `tools/sigclass/README.md` ("Train the model"): `train.py gen`
  V1 (24 000, seed 11), V2 (16 000, 12), V3 (3 000, 13, `--classes` of the
  wide ones), V4 (16 000, 14), VT (4 000, 2: test); `train.py fit --data
  V1,V2,V3,V4 --test VT --hidden 192,96 --none-weight 2.5`; `eval_real.py`
  (before: `KRAKEN_SIGCLASS_TOOL=<the v1 build> eval_real.py --model <phase
  2 model>`)

## Runtime (`sigclass_service.cpp`)

- **Tap** (`sigsvc::on_packet`, data receiver thread): right after the AI
  assistant's capture hook in the packet parser - every packet passes there,
  heimdall's and a debug / recording playback's alike, retuning and
  noise-source ones included. Cost while no capture is armed: one relaxed
  atomic load per packet (`sigsvc::armed()`); armed, a mutex nobody else
  holds and a `memcpy` of one channel's bytes (32 KB a 16384-sample packet)
  into a buffer sized once (2 x `sigclass::NOMINAL_SNAPSHOT_SAMPLES` = 2.4
  MB, allocated on the first capture). When it has the 1.2 M samples (0.5 s)
  it waits `TRAIL_PACKETS` (3) more clean packets - heimdall's noise flag
  trails the samples by up to 2 packets (*bearing-spectrum.md*) - then hands
  the snapshot to the worker. Aborts (the worker decides what next): a
  noise-source packet (also stamps the time: parked), a retuning packet or a
  change of that channel's header frequency (> 1 Hz: a retune - 500 ms
  `SETTLE_MS` before the next capture), a change of the channel count, the
  channel gone. A stream gap restarts the snapshot (the data receiver's
  `stream_gap`: a byte resync on a bad magic / header, a reconnect, a switch
  to / from a playback). Packets heimdall drops for a slow client are not
  visible in the stream (it has no sequence number)
- **Worker thread** `sigclass` (`sigsvc::start()` from main, joined by
  `sigsvc::stop()`; idle on a condition variable while off): pick the tuner
  and its centre, arm, wait (timeout `CAPTURE_TIMEOUT_MS` 4 s: "no data from
  the receiver"), `Analyzer::analyze` (uint8 snapshot, 2.4 MHz), the
  tuner's tracker `update`, publish, rest. Every path that takes no snapshot
  (settling after a retune, a capture ended by a retune or a channel-count
  change, no data) still runs the trackers' expiry, the band filter and a
  publish (`age_out`) - a stream of retunes closer than a capture (~1 s:
  the saved-frequency scan's 0.5 s dwell, the discrete scanner, a long drag)
  used to freeze the list with the old band's tracks in it. Which tuner:
  coherent mode - the CH selector's antenna (`active_channel`) at
  `coherent_tuner_hz()`; wideband scan, independent mode and generic
  RTL-SDRs (one channel too) - the BANDS in turn at `exact_tuner_hz(ch)`
  (*Tuners in turn* below: tuners within 1 kHz of each other are one band,
  a band's first look comes first, a retuned tuner settles on its own).
  The centre is the exact request when
  the snapshot's header frequency (float32, ~64 Hz exact near 1 GHz) is
  within 2 kHz of it, else the header's. Labels are absolute Hz, so they land
  on the stitched wideband-scan spectrum and on each pane by themselves
- **CPU budget**: `CPU_SHARE` 4 % - the cycle period (capture + analysis +
  rest) is the analysis time (exponential mean, weight 0.3) / 0.04, clamped
  to `MIN_PERIOD_S` 1 .. `MAX_PERIOD_S` 10 s (`rules::Budget`; the rest
  is taken before the next look, once its band is known). With tuners in
  turn a band's FIRST look may skip the rest (a debt, at most
  `FIRST_LOOK_DEBT_LOOKS` 8 looks' analysis, paid back by the next periods,
  each at most doubled) - see *Tuners in turn*. Coherent mode never takes a
  look early; its periods are the old rule's except while a debt brought
  from a multi-tuner mode is paid back (each at most doubled). The debt and
  the budget's clock stay over a disable and a mode change: toggling Signal
  ID buys no analysis beyond the share + the cap (INV-G). The state carries
  `analyze_ms`, `period_ms` (the planned period, a debt included), `duty`
  and `looks` (snapshots per tuner since the switch-on / mode change; the
  page ignores it). No thread priority change: the code
  base has no portable per-thread helper (`kproc::lower_own_priority` lowers
  the whole process on Windows), so the budget is the only limit
- **Trackers**: one per band (`TunerTracks`; coherent mode uses [0] only;
  tuners in turn: in the slot of the tuner standing for the band - the
  lowest-numbered one on it -, and it MOVES with the band
  (`rules::regroup`): when a lower tuner joins or the lead leaves, the band
  keeps its tracks and labels; a tuner leaving for a band nobody was on keeps
  its own tracker, the old band's tracks out of sight, unless another tuner
  continues that band; the trackers of tuners beyond a lowered channel count
  are cleared),
  `TRACK_EXPIRE_S` 45 s (a pager or a repeater between overs keeps its
  label). A track's age counts against the looks at ITS band: each tracker
  has its own clock, which moves only with a snapshot of its band - by the
  wall time since its previous one, at most `EXPIRE_STEP_S` (the expiry /
  `MIN_MISSED_LOOKS` 6, + 10 ms). So a track goes once it was missed for
  45 s AND in at least 6 snapshots of its band (2 until 2026-11-24: an
  intermittent signal's track went before the signal came back - see
  *Tuners in turn*) - and in every mode at the latest `TRACK_CEILING_S`
  180 s of wall time after it was last seen, looked at or not (see the
  table there). Before (one tracker on the wall clock), 8
  tuners in turn at a 10 s period (analysis >= 225 ms; 5 tuners: >= 360 ms
  - inside the Pi 5 estimate) revisited a tuner every 80 s and 3 of 8
  tuners had no labels at any time. In coherent mode (one tuner; every
  snapshot is a look) the clock also runs between snapshots (no data,
  retunes): there it is the wall clock. Track ids sent = the tracker's id x
  8 + the tuner. Cleared on an operating-mode change, on disable, and when
  parked for the noise source; kept over a retune (absolute Hz: the ones
  still in band go on). Only what the pages show is sent: a track whose
  centre lies inside the EDGE-CLIPPED part of its tuner's band now - the
  tuner centre +- `EDGE_CLIP` x 1.2 MHz (`current_edge_clip`, the setting
  the spectrum, the wideband-scan stitch and the scanners use; 0.80 =
  +- 0.96 MHz; read at every publish, so a change of the setting is followed
  with the next one). On real captures every remaining stray label sat on
  band-edge artefacts beyond ~+-1.05 MHz (15 kHz humps at +1080 / +1096 kHz
  labelled "Unknown OFDM" 0.5-0.67, a spur at +1128 kHz). A wide or
  band-filling track whose centre is inside is sent with `lo` / `hi`
  limited to the clipped band (its bracket stays on the plot). After a
  retune - or a switch between playback entries - the old band's tracks
  leave the list at once. Tracks outside are kept in the tracker (they may
  drift in after a retune, and come back if the receiver returns before
  they expire), just not sent. Two overlapping tuners (wideband scan's
  edges, independent tuners on neighbouring frequencies) seeing one
  signal: sent once (`rules::drop_overlap_duplicates`: best view first -
  fresh, then one with a class, then the higher SNR - each tuner has its
  own antenna, live one control channel read 15 dB on CH0 and 3.6 dB on
  CH1 -, then the newest, the most hits -, each taken unless a view
  of the same signal - centres within half the wider width + 500 Hz,
  widths within 3x, another tuner - was taken). Classes marked
  `"hidden": true` (`none`: a false detection) are never sent, nor offered
  as an alternative
- **Ages**: `age` is 0 while the track's band's latest snapshot had it and
  that snapshot is not overdue (a round of the bands + 5 s) - with tuners
  in turn a round can take 80 s, and the page's fade needs no knowledge of
  it; otherwise the wall time since it was seen (no data, retunes: the
  labels fade). The CPU budget is spent per snapshot (the 1 s floor applies
  per snapshot, so N tuners take N periods a round) - not changed
- **Parked** (no captures, the reason in `parked`): "calibrating (noise
  source on)" - `doa_is_calibrating()` or a noise packet seen by the tap
  within `NOISE_HOLD_MS` 1.5 s (tracks cleared: the noise source would be a
  band-filling "signal"); "settling after a retune" (500 ms, tracks kept;
  with tuners in turn only when EVERY band is settling - a retuned tuner
  settles on its own while the others are looked at);
  "no data from the receiver". Calibration state (phase states, KerberosSDR
  stale) doesn't matter otherwise: one antenna's samples need no array
  calibration
- **Tuners in turn** (wideband scan, independent mode, generic RTL-SDRs;
  2026-11-24, the user's report "in wideband and independent mode the
  classifier ... is not classifying on some signals ... those work in
  coherent mode"). The rules are pure functions in
  `include/sigclass_rules.hpp`, tested by `tools/sigclass_rules_test.cpp`
  (randomized invariants, mutants it must catch):
  - **Bands** (`band_reps`): tuners within `SAME_BAND_HZ` 1 kHz of each
    other are one band - one look serves them all, one tracker (the
    lowest-numbered tuner's). The user's independent layout had three
    tuners on 416.815 MHz: three of five looks went to one band. The
    price: the band is seen through the representative's antenna only (CH1
    for CH1-CH3 there) - live the channels differ by several dB on one
    signal, so a weak one another of those antennas would catch can be
    missed
  - **Which band** (`update_bands`, `pick_band`, `note_look`): two passes.
    (1) A band nobody has looked at since its start (switch-on, mode change,
    its tuner's retune) is taken AHEAD of its turn - first come first served
    - while its tuner holds the token: a tuner that took a look ahead of the
    turn gets the next one only after every other ready band has had a look
    since (the token is the tuner's own, never copied with a band: a tuner
    hopping onto another's band and off again took that band's token along
    and overtook twice in the model). (2) Else the ready band whose turn is
    the oldest (`turn_seq`: its latest look; a new band's turn counts from
    its start, behind every band looked before it); ties in cursor order;
    with nothing retuned this is the old round robin exactly. Until
    2026-11-24 a first look always won and moved the cursor: one tuner
    retuned every 2.8 s (the saved-frequency scanner's dwell, a user
    stepping a pane, the assistant) took every other look - 120 s, 5
    tuners: looks [1, 42, 41, 8, 1] (the reviewer's harness), a band's
    longest gap 230 s at Pi periods. Invariants (tested, R = the tuners
    standing for a ready band in the stretch): INV-A a band ready and not
    retuned through 2R consecutive looks gets one; INV-B a single retune,
    nothing else changing, gets its first look before any other band's
    next; INV-C a band waiting for its first look is overtaken at most once
    by each band that began waiting later; INV-D without retunes the
    cursor's round robin. Several scanned tuners share the first looks: 4
    tuners retuned every 2 s at 300 ms analysis, 600 s (the reviewer's
    model on these rules): looks 6 / 5 / 5 / 4 and 60 for the steady one
    (the scanned ones are settling or retuned mid-capture most of the time;
    before: 20 / 0 / 0 / 0). A retune of tuner N (its `exact_tuner_hz`
    changed - seen at every pick, not only by a capture of that tuner)
    holds tuner N for `SETTLE_MS` 500 ms; the others go on. Before, a
    retune of a tuner that was not being captured went unnoticed and its
    next snapshot could start on samples of the old band (heimdall's
    pipeline carries them under the new header for some 10-100 ms)
  - **First looks** (`Budget`): a band's first look may start without the
    budget's rest, as long as the analysis not waited for (the debt) stays
    within 8 looks' worth; the following periods pay it back (each at most
    doubled; a period at the 10 s clamp cannot pay - as before, an
    analysis over 0.4 s exceeds the share). So the first labels of every
    band come within a round of CAPTURES (about 0.6 s + the analysis per
    band), not a round of periods
  - **A band's lead changes** (INV-E): the band's tracker and its band
    fields (looks, turn) move to the tuner now standing for it
    (`rules::regroup`); before, a lower tuner joining cleared the old lead's
    tracker and a lead leaving took the band's tracks with it - 8 labels
    gone for 4-5.5 s, all back only after 29-43 s (the reviewer's harness)
  - **Expiry**: a track is kept until missed for 45 s AND in 6 looks of its
    band (`MIN_MISSED_LOOKS`), at the latest `TRACK_CEILING_S` 180 s of wall
    time after it was last seen, in every mode, looked at or not (INV-F;
    `Tracker::erase` in the core, the ceiling also applied at publish). How
    long a signal that went off the air stays labelled, about max(45 s, 6
    rounds), the ceiling above that (a round = bands x period):

    | bands \ period | 1 s | 2.5 s | 4 s | 7.5 s | 10 s |
    |---|---|---|---|---|---|
    | 1 (coherent) | 45 s | 45 s | 45 s | 45 s | 45 s |
    | 2 | 45 s | 45 s | 48 s | 90 s | 120 s |
    | 3 | 45 s | 45 s | 72 s | 135 s | 180 s |
    | 5 | 45 s | 75 s | 120 s | 180 s | 180 s |
    | 8 | 48 s | 120 s | 180 s | 180 s | 180 s |

    A band that gets no looks at all (no data, starved, a tuner moved away)
    loses them after 180 s (the calibration pause clears them at once)
  - **A mode switch** (every mode): the worker's rest and capture wait end
    when `operating_mode_index()` changes (checked every 100 ms) and the
    cleared list goes out at once (`publish(true)`) - before, the old
    mode's tracks stayed on the new mode's spectrum for up to a period.
    Known edge: a mode change DURING an analysis publishes that look under
    the old mode for a few ms, until `publish(true)` replaces it
  - **Fixed**: the overlap rule's compaction (`pub[w++] =
    std::move(pub[i])`) also ran for w == i - a self-move-assignment, which
    leaves a `std::string` empty: class, name, family, plugin, demod and the
    alternatives of every track in front of the first dropped duplicate.
    The rule ran only with more than one tuner, and the page draws only
    tracks with a class and a name - so in wideband scan (no duplicates)
    NO label showed at all, and in independent mode with tuners on one band
    only the tracks above the first duplicate. The cause of the report
- **Model**: `sigclass.model` in the working directory (cwd-relative like
  every runtime file), `$KRAKEN_SIGCLASS_MODEL` overrides it. Loaded by the
  worker on the first enable (the settings replay enables it at startup);
  missing or refused (another feature layout) - `available:false`, the
  loader's reason in `reason`, ONE line to stderr ("Signal ID unavailable:
  cannot open sigclass.model"), no retries; the next enable after a disable
  tries the file again. The model's metadata `"classes"` (the objects of
  `tools/sigclass/classes.json`: id, name, family, plugin, demod, bw_hz,
  burst, unknown, fills_band, hidden) is parsed once (`parse_table`,
  json_find per object), and its `"min_confidence"` (the trainer's "show a
  label only at p >= this", 0 < x <= 1) passed to the pages; the C++ has no
  class names or protocol knowledge - it passes them through. The loader
  (`model.cpp`) refuses a softmax temperature outside 1e-3 .. 1e3 (1e-39
  passed the old "> 0" test and every probability came out NaN), clamps the
  scaled logits to +-1e4, and sizes a layer's weights (in x out, up to 2^40)
  in 64 bits against the bytes left (a 32-bit build - armv7l - wrapped);
  `tools/sigclass/model_io.py` writes / computes the same (its forward agrees
  with the C++ within 2e-4 on the real model, extreme inputs included)
- **Docker**: the image's `sigclass.model` is linked into `/data/kraken_doa`
  only when there is no file or link of the user's there
  (`link_if_absent` in `docker/entrypoint.sh`; no dangling link when the
  image has none) - a user's own model there wins. Another place:
  `KRAKEN_SIGCLASS_MODEL` (e.g. `-e KRAKEN_SIGCLASS_MODEL=/data/my.model`)
- **The KrakenSDR Wideband downconverter variant** (`--wideband`): nothing
  to do. heimdall negates Q while the mixer is on the high side
  (`sdr_pipeline.cpp`, `spectral_inversion`) BEFORE it re-quantises the
  phase-compensated samples into the 8091 stream (`tcp_data_server.cpp`), and
  the header / `coherent_tuner_hz()` carry the true RF. The bytes the tap
  copies are therefore already the true spectrum at RF - no conjugation and
  no offset mirroring here (the same conclusion as MUSIC's and the bearing
  spectrum's, *wideband-variant.md*). Only coherent mode exists there
- **Read-only**: nothing is ever sent to heimdall (tested: no control
  command in 35 s with it on); no files written; the on / off setting goes
  through the settings store, sandboxed during a playback like any other

## Protocol

- `SIGCLASS:0|1` (`control_handler.cpp`; `CommandRejected` otherwise):
  persisted (schema `sigclass`, default off, replayed after `BIN_DOA:`),
  echoed as sync_cmd and in `is_replayed_command` (the page ignores the
  echo - it draws from the state). Allowed during a playback (sandboxed)
- `{"sigclass":{enabled, available, reason, parked, analyze_ms, period_ms,
  duty, snapshots, looks, min_confidence, tracks:[{id, hz, bw, lo, hi, tuner, snr,
  duty, burst, wide, cls, name, family, p, alt:[[cls, p, name], ...], plugin,
  demod, vbw, min_p, age, hits, unknown}]}}` on the control topic (`WebSocketServer::broadcast_json_message`):
  on enable / disable at once; after each analysis and on a park change when
  the track list's signature (ids, classes, p in 2 % steps, hz in 200 Hz
  steps, bw on a log scale, the state) changed, at most every 500 ms
  (`SEND_MIN_MS`; a held change goes out from the worker's next wake-up),
  else every 5 s (`SEND_REFRESH_MS`: ages, analysis numbers). A page that
  connects gets the current state in the connect sync
  (`get_connect_sync_messages`, ages computed then). Tracks: sorted by
  frequency, at most 200 (the most recently seen); none while off. `cls`,
  `name`, `family`, `plugin`, `demod` are null without one; `vbw` = the
  class's `bw_hz` (the VFO width a click sets; null = none); `min_p`
  (phase 4) = the class's own "sure" threshold from the model's metadata
  (`meta.classes[i].min_p`, 0..1.01; null = use `min_confidence`; 1.01 =
  never sure) - the page's `sigSure(t)` uses it (phase 5), else the
  model's `min_confidence`. Phase 5: a track seen ONCE only (hits 1) is
  published only when its label is already sure by that rule - a one-hit
  stray piece (a FLARM packet's fragment as "ISM device (FSK) 52 %") no
  longer flashes a "?" label; a device that sends one burst a minute still
  shows at its first burst when the label is sure, and from its second
  burst on otherwise (the tracker keeps it 45 s, so a once-a-minute device
  with an unsure label may never show: by design - one look is not enough
  evidence for a guess); `hz` = the
  track's smoothed centre (the tune target), `lo` / `hi` = the band its
  detections cover (the core's band edges when its `Track` has `lo_hz` /
  `hi_hz`, else hz +- bw / 2; limited to the edge-clipped band), `tuner` =
  its tuner; `wide` = the TRACK's
  latest detection filled the band (`Track::fills_band`), or it is >= 90 %
  of 2.4 MHz; `alt` = the next two (not hidden) classes with their names (a
  third element - a two-element reader still works); `age` - see Ages;
  `min_confidence` = the model's (null without). Numbers are finite
  (`num()` writes null otherwise), text through `json_escape`
- Compatibility: an older page ignores the key and the sync_cmd (tested:
  HEAD's page against this receiver, no errors); this page against an older
  receiver gets no state within 8 s of an open socket and then shows "Not on
  this receiver", the switch disabled (tested). Not before the 8 s, not
  while the socket is down, not while a token is asked for (the timer
  starts again after AUTH)

## Page (kraken_doa.html "SIGNAL ID")

- State in the globals block (drawing reads it): `sigSt` (the last state),
  `sigStAt`, `sigHelloOk`, `sigRects` (the main spectrum's label targets),
  `sigConsumed`, `sigDy` (each track's label row), `sigNoRx`,
  `SIG_FADE_S` [5, 40], `SIG_FADE_MIN` 0.45, `sigUi` (localStorage
  `kraken_sigclass` {min: 0..1 or 'auto', bands: the Bands bar}: per
  browser), the Bands bar's `BAND_TABLE`, `BAND_CATS`, `BAND_GAP_TIP`,
  `bandTextW` (*Bands*, below)
- **Always on the spectrum** (the user's decision, 2026-11-24: "Remove the
  "on spectrum" button. We will always display on the spectrum"): with
  Signal ID on, the labels are drawn on every spectrum - the main display
  (zoomed views and the wideband-scan stitch included) and each
  independent pane. The box's "On spectrum" switch (`sig-spec`,
  `sigSetSpec`, `sigUi.spec`) is gone; a `spec` key an older page stored in
  `kraken_sigclass` is ignored (the rest of the object - Confidence - still
  read) and dropped at the next save
- **Labels** (`sigDrawSpectrum` from `drawBoxTags`, after the saved-frequency
  tags and before the AI badges, so on every path through drawBox; panes:
  `sigPaneTags` from `paneDrawOverlay`, after `sfPaneTags`): a track with a
  class (never a hidden one) and p >= the browser's minimum (`sigMin()`:
  the Confidence choice, default 50 %; Auto = the model's
  `min_confidence`), in view. The box's
  bottom 6 px above the trace's peak inside the track's band (`sigPeak`:
  binary search in the trace's frequencies; the pane's own trace in a pane),
  a bracket 3 px above the peak over the track's band (`sigBand`: lo..hi;
  ends ticked; a
  thin line to the box when it moved), never above y 20 (the VFO labels' and
  AI badges' row) or the saved-frequency tags' rows. Declutter
  (`sigLayout`): the more certain first (p in 10 % steps, then the older
  track - the layout doesn't shuffle with every update), a box that would
  overlap one placed moves up a row (at most 3; down only when it already
  sits at the top), the row it had last time tried first (`sigDy`), else it
  is left out. Colours (palette, *theme.md*): box `--s1` at 84 %, border
  `--l3` (`--l1` subdued), text `--t0` (`--t5` subdued), bracket `--t2`
  (`--t6`). Subdued: p < the track's sure level (`sigSure(t)`: its `min_p` when the
  track carries one - above 1 = never sure -, else the model's `min_confidence`; 0.7
  from a model without one) with a trailing "?", and the unknown families. A track not seen for 5 s (`age`) fades to 45 % by 40 s. Names over 18
  characters cut. Not drawn on the main spectrum in independent mode (each
  pane shows the tracks inside its own span); the waterfall gets nothing
- **Hover** (`sigMarkerHover`, after the AI badges' and the tags' in
  `handleMove`; panes in `paneMove`): the canvas `title` = `sigTip`: name, p,
  family, "Or:" the next two with their p, MHz, bandwidth, SNR, bursts, last
  seen, "Click the label to tune the selected VFO (decoder, demod, width)";
  the hand cursor over a box only (`drawBoxTags` keeps it, as for the
  tags). Hit test (`sigAt`): the boxes; for the tooltip only, also the
  narrowest bracket under the pointer (a band-filling track's bracket spans
  the plot)
- **Click** - on a label's BOX only (`sigAt(..., press)`; the bracket of a DAB
  block or a band-filling track spans the whole plot: as a press target it
  took every press meant for a VFO, a drag or a click-to-tune). A box wins
  over a VFO bar under it, as a saved-frequency tag does
  (`sigMarkerHit` in `handleDown` after the AI badges and the tags;
  `sigConsumed` eats the click so the spectrum's own click-to-tune doesn't
  follow; panes: `paneDown`, with `{tuner: p.ch}`): `sfTune(sigEntry(t))`
  - the saved frequencies' path, so fixed-frequency and lock_vfo decoders
  and independent mode behave the same. Entry: `hz` (rounded), `name`,
  `dec` = `PLUGIN:<id>` when the class has a plugin this receiver lists as
  built - a fixed-frequency plugin (ADS-B, AIS, ...; it retunes the
  RECEIVER to its own frequency) only when the signal is within max(300 kHz,
  half the class's / the track's width) of that frequency or the band the
  signal is in holds it: a wrong label elsewhere must not send the receiver
  away -, else '' (the VFO's decoder stays) - or `OFF` when the VFO's own
  decoder holds it (fixed-frequency / lock_vfo: sfTune would refuse
  otherwise); `mode` = the class's demod; `bw` = the class's `vbw`, else the
  detected width x 1.25 (1 kHz .. 2.4 MHz). Touch: the spectrum's touch
  handler calls `handleDown`, so a tap tunes the same way
- **Sidebar box** "Signal ID" (`data-sb="signal-id"`, after Saved
  Frequencies, every mode, icon `tag`): the switch (`sigSet` sends
  `SIGCLASS:` and puts the switch back until the state moves it - no
  optimistic update; disabled with no state), the state ("Running" with
  analysis time / period / duty / snapshots in its title, "Off", "Paused",
  "Starting", "Unavailable", "Not on this receiver") and the reason as an
  `.sb-note`, "Confidence" (Auto (the model's level, shown), All, 25 .. 90
  %, default 50 %) and "Bands" (the bar of worldwide bands, default off;
  both per browser), the list
  (`#sig-list`: name, MHz to the kHz, p - sorted by frequency, the same
  filter as the labels, subdued / old rows greyed; a click tunes like a
  label; rows are patched in place keyed by track id - rebuilding them with
  `innerHTML` on every state lost a click whose press and release
  straddled a message), the count in the header, a `?` tip
- New icon `tag` (*icons.md*)

### Bands: a bar naming the worldwide bands in view (2026-11-24)

The user asked: "In signal ID settings, also add an option to display known
bands. This would display a bar on top of the spectrum that says what band
we are tuned to (using globally agreed upon bands only) (e.g. ISM band,
BCFM band, satellite etc)".

- **The option**: "Bands" in the Signal ID box (`#sig-bands`, `bandSet`),
  per browser (`sigUi.bands` in localStorage `kraken_sigclass`, beside
  Confidence), default off. Independent of the classifier: it works with
  Signal ID off, without a model and against a receiver that has no Signal
  ID (the page draws it from its own frequency axis - no command, no
  protocol change). `body.bands-on` shows the strips.
- **The bar** (the script's "BANDS" section): a 16 px strip directly above
  the plot of every spectrum - `#band-strip-main` between the main
  display's header and its plot (hidden in independent mode), and
  `#tpane-b-N` between each independent pane's header and its body. Being
  outside the plot it covers nothing that is there (VFO bars and their
  labels, Signal ID labels and brackets, saved-frequency tags, AI badges,
  the hover readout, scanner marks, the bearing colours, the noise floor)
  and takes no spectrum input: click-to-tune, VFO drags, wheel zoom and
  touch work as before. The plot keeps its height; the main display's
  waterfall (and a pane's body) gives up the 16 px. One segment per band
  over the part of it in view (DOM elements, `.bs-seg`, palette tokens:
  `--s2` strip, `--s4` / `--l2` segment, 11 px `--t2` text, `--s5` / `--t0`
  hovered - one step up from the first version's 10 px `--t3`, which was
  the faintest text on the page; the label answers "what band am I on"),
  aligned with the plot's frequency axis (main: the uPlot plot
  area's left edge + `valToPos`; panes: `paneX`), the short label centred
  when it fits (else none), gaps where no worldwide band applies. Every
  segment's `title` (the page tooltip: hover, help mode, press and hold)
  holds the full name with the service(s), the range, the category and the
  RR provision ("Worldwide: ITU Radio Regulations ..."; a regional row
  "Region 1 only: ..."); the empty strip's says "No worldwide band here".
  Segments are DOM elements re-used for whatever band a retune puts there:
  each gets its band's `title` when its band changes (a title set by code
  wins over the `data-tip` the page's tooltip moved it to). A press on
  the strip does nothing else (decided: nothing to act on; the tooltip is
  the content). It follows retunes, zoom and pan at once: the main
  display's `drawBox` and each pane's `paneDrawSpec` call it every frame;
  it is rebuilt only when the axis (min, max), the plot width or its left
  edge changed (`el._mk` / `el._bk`), otherwise a frame costs a comparison.
  Hz are plain JS numbers (the Wideband variant's RF to 6668 MHz: no 32-bit
  operations). Drive mode hides the spectrum and with it the strip
- **The table** (`BAND_TABLE`, one data block in the page's globals,
  between `// BAND TABLE BEGIN` / `END`, one JSON array per row: lo Hz, hi
  Hz, short label (<= 16 characters), full name with the service(s),
  category - `broadcast`, `aero`, `marine`, `amateur`, `satellite`, `nav`
  (navigation and radar), `ism`, `science` (meteorology, radio astronomy,
  time), `mobile` (RLAN) -, the RR provision, and for a regional row only
  a 7th field, its Region). 60 rows. Coverage: 24 MHz (the receiver's
  floor) to 6.7 GHz (the Wideband variant reaches 6668 MHz).
  **The rule for a row** - and for adding one: the same service is
  allocated in all three Regions' columns of the ITU Radio Regulations'
  Table of Frequency Allocations (Article 5) over the range, primary or
  secondary (the row says which where it differs), or a footnote /
  appendix designates the band for the whole world (ISM No. 5.150, maritime
  VHF No. 5.226 with Appendix 18, RLAN No. 5.446A with Resolution 229);
  only the common part of a band (FM broadcasting from 88 MHz, because
  Region 2 starts there). **The one exception** (the orchestrator's review,
  2026-11-24: "ISM band" was the user's own first example, and 433 / 915
  MHz are where ISM devices are): the ISM bands the Radio Regulations
  themselves designate for a single Region - 433.05-434.79 MHz in Region 1
  (No. 5.138; No. 5.280 for the countries that treat it as under No.
  5.150) and 902-928 MHz in Region 2 (No. 5.150) - are in, marked with
  their Region: the 7th field, "(R1)" / "(R2)" in the label, "Region N
  only" in the full name and the tooltip's last line. The 70 cm row is
  split around 433.05-434.79 (430-433.05 and 434.79-440 MHz stay "70 cm
  amateur"; the ISM row's full name says that worldwide this is the 70 cm
  band). Nothing else regional: CEPT's 868 MHz SRD band, PMR446 and the
  cellular bands are not RR designations. **Co-primary services**: where
  another service is co-primary in a Region (or by footnote in parts of
  one) and often defines what is on air there, the full name says so - the
  short label stays: Band I / III and FM (fixed, mobile in Region 3), UHF
  TV 470-608 / 614-698 (Region 3 fixed and mobile - land-mobile networks
  there, e.g. a trunked network at 499 MHz in New Zealand; Region 2 by
  footnote, Nos. 5.292, 5.293, 5.296A, 5.308A), 698-862 (fixed too), marine
  VHF (land mobile outside the maritime channels), the radio astronomy
  rows at 322 / 406 MHz (fixed, mobile), L-band broadcasting (fixed,
  mobile, IMT: Nos. 5.346, 5.346A), 1518-1525 (fixed, mobile), the S-band
  space rows (fixed, mobile), 13 cm (fixed, mobile primary, amateur
  secondary), ISM 2.4 GHz (fixed, mobile), C-band (mobile / IMT: Nos.
  5.430A, 5.431B, 5.433A), the C-band uplink (fixed links, IMT 6425-7125 in
  Region 1: No. 5.457E). Not in the table: plain FIXED / MOBILE
  allocations (they are most of the spectrum and name no use; the IMT
  identifications of Nos. 5.384A / 5.388 are for administrations "wishing
  to implement IMT" and the cellular bands differ by region), national or
  regional plans that are not RR designations (868 MHz devices, PMR446,
  CB, land-mobile and public-safety bands, cellular band numbers, TV
  channel rasters, military plans), and small worldwide allocations nobody would call a
  band (149.9-150.05 MHz mobile-satellite, 400.05-400.15 MHz time-signal
  satellite, 4500-4800 MHz FSS of the Appendix 30B plan, ...). Add a row
  only when both sources below agree; cross-check it, run the checker, add
  it to the table here. **Nesting: none** - rows never overlap; a worldwide
  sub-band inside a band (121.5 MHz in the airband, 1610.6-1613.8 MHz radio
  astronomy in the satellite-phone band, AIS in marine VHF, 2483.5-2500 MHz
  mobile-satellite in ISM 2.4 GHz) is named in its band's full name. Two
  adjacent bands are two segments
- **The checker** `tools/band_table_check.py [page] [-v]` (python3, no
  dependencies): extracts the block and checks every row (integer Hz,
  lo < hi, inside 24 MHz - 6.7 GHz, the label length, a full name, a known
  category, a provision, no emoji), the order, no overlap, coverage (a band
  below 30 MHz, one ending at >= 6 GHz), the regional rows (exactly the two
  of `REGIONAL`, each `ism` with "(R<n>)" in its label and "Region <n>
  only" in its full name - a 7th field anywhere else fails), and the
  classifier's `tools/sigclass/classes.json`: every exclusive band there
  must be covered by rows of the expected category that do not cross its
  edges; `-v` lists each class's `rf_hz` against the rows it meets. 60
  rows: OK; five faulty copies (a regional 868 MHz row, the 915 row without
  its Region, the 433 label without "(R1)", an overlap, a 20-character
  label) each fail
- **Consistency with classes.json** (the two serve different purposes and
  stay separate data: `rf_hz` is where a class can exist anywhere in the
  world, conservatively; this table is what the whole world shares). No
  contradiction; the differences, all of them `rf_hz` wider than the
  worldwide band: `wfm` 64-108.5 MHz vs FM broadcast 88-108 (OIRT and
  Japan's bands are regional); `dab` 174-240 vs Band III 174-216 (Regions 1
  and 3 to 223 MHz, DAB to 240 nationally); the DTV classes 44-88 / 170-230
  / 470-890 vs Band I 54-68, Band III 174-216, UHF 470-608 / 614-698 /
  698-862 (608-614 MHz is radio astronomy in Region 2; 862-890 MHz
  broadcasting in Region 1 is limited to the African Broadcasting Area,
  No. 5.322); `ais` 156-162.6 vs marine VHF 156-162.05 (No. 5.226's
  maritime bands end at 162.05 MHz); `radiosonde` 1668-1700 vs
  meteorological aids from 1668.4 MHz; `dme` 960-1215 = DME / SSR 960-1164
  + GNSS L5 1164-1215 (aeronautical radionavigation in both). The
  exclusive bands (88-108, 108-117.975, 117.975-137, 406-406.1,
  960-1215 MHz) coincide with rows of the matching category

**Sources.** (1) *ITU Radio Regulations, edition of 2024*, Volume 1,
Article 5 (Table of Frequency Allocations and its footnotes), and Volume 2,
Appendix 18 - the free PDF from itu.int (R-REG-RR-2024). (2) The FCC Online
Table of Frequency Allocations (47 CFR 2.106, revised 2022-07-01): its
International Table, columns 1-3, reproduces Article 5 of the 2016
edition, and its paragraph of international footnotes (5.150, 5.226, ...)
the footnote text. A row is in the table only when both agree, except
where a later World Radiocommunication Conference changed it (marked).
Every row was read against both (the "Art. 5" ranges below are the RR
entries whose allocation spans all three Region columns):

| MHz | Label | Category | What makes it worldwide (RR 2024) | FCC table (RR 2016) |
|---|---|---|---|---|
| 24.89-24.99 | 12 m amateur | amateur | Art. 5: AMATEUR, AMATEUR-SATELLITE, all Regions | same |
| 24.99-25.01 | Time 25 MHz | science | Art. 5: STANDARD FREQUENCY AND TIME SIGNAL (25 000 kHz) 24.99-25.005, 25.005-25.01 | same |
| 25.55-25.67 | Radio astr. | science | Art. 5: RADIO ASTRONOMY; No. 5.149 | same |
| 25.67-26.1 | 11 m broadcast | broadcast | Art. 5: BROADCASTING | same |
| 26.957-27.283 | ISM 27 MHz | ism | No. 5.150 (centre 27.12 MHz), in all three columns | same footnote |
| 27.5-28 | Met aids | science | Art. 5: METEOROLOGICAL AIDS, FIXED, MOBILE | same |
| 28-29.7 | 10 m amateur | amateur | Art. 5: AMATEUR, AMATEUR-SATELLITE | same |
| 40.66-40.70 | ISM 40 MHz | ism | No. 5.150 (centre 40.68 MHz) | same footnote |
| 50-52 | 6 m amateur | amateur | Region 1 50-52 Amateur (secondary, WRC-19; No. 5.169B), Regions 2 / 3 50-54 AMATEUR | Regions 2 / 3 only (Region 1's allocation came with WRC-19, after the 2016 edition) - single-source for Region 1 |
| 54-68 | Band I TV | broadcast | Region 1 47-68, Region 2 54-68 (to 72), Region 3 54-68 (with FIXED, MOBILE): BROADCASTING | same |
| 74.8-75.2 | Marker beacons | aero | Art. 5: AERONAUTICAL RADIONAVIGATION; No. 5.180 (75 MHz marker beacons) | same |
| 88-108 | FM broadcast | broadcast | Region 1 87.5-100, Region 2 88-100, Region 3 87-100, all 100-108: BROADCASTING | same |
| 108-117.975 | VOR / ILS | aero | Art. 5: AERONAUTICAL RADIONAVIGATION | same |
| 117.975-137 | Airband | aero | Art. 5: AERONAUTICAL MOBILE (R); No. 5.200 (121.5 MHz emergency) | same |
| 137-138 | Weather sats 137 | satellite | Art. 5: SPACE OPERATION, METEOROLOGICAL-SATELLITE, MOBILE-SATELLITE (to 137.825), SPACE RESEARCH (space-to-Earth) | same |
| 144-146 | 2 m amateur | amateur | Art. 5: AMATEUR, AMATEUR-SATELLITE (146-148 Regions 2 / 3 only) | same |
| 156-162.05 | Marine VHF | marine | No. 5.226 (maritime priority 156-162.05 MHz, 156.525 / 156.8 MHz distress and calling) and Appendix 18 (channels 156.025 - 162.025 MHz, AIS 1 / 2); Art. 5 MARITIME MOBILE 156.4875-156.5625, 156.7625-156.8375 | 5.226 same text |
| 174-216 | Band III bcast | broadcast | Region 1 174-223, Region 2 174-216, Region 3 174-223 (with FIXED, MOBILE): BROADCASTING | same |
| 322-328.6 | Radio astr. | science | Art. 5: FIXED, MOBILE, RADIO ASTRONOMY; No. 5.149 | same |
| 328.6-335.4 | ILS glide path | aero | Art. 5: AERONAUTICAL RADIONAVIGATION; No. 5.258 | same |
| 400.15-406 | Radiosondes | science | Art. 5: METEOROLOGICAL AIDS 400.15-401, 401-402, 402-403, 403-406 (METEOROLOGICAL-SATELLITE to 403) | same |
| 406-406.1 | 406 beacons | satellite | Art. 5: MOBILE-SATELLITE (Earth-to-space); Nos. 5.266, 5.267 | same |
| 406.1-410 | Radio astr. | science | Art. 5: FIXED, MOBILE, RADIO ASTRONOMY; No. 5.149 | same |
| 430-433.05 | 70 cm amateur | amateur | Region 1 AMATEUR, Regions 2 / 3 Amateur (secondary), RADIOLOCATION in all (430-432, 432-438) | same |
| 433.05-434.79 | ISM 433 (R1) | ism, **Region 1 only** | No. 5.138: ISM, centre 433.92 MHz, "in Region 1 except in the countries mentioned in No. 5.280" (special authorization); No. 5.280 (Germany, Austria, ... Switzerland) designates it as under 5.150. Worldwide: the 70 cm amateur band | same two footnotes |
| 434.79-440 | 70 cm amateur | amateur | as 430-433.05; No. 5.282 (amateur-satellite 435-438) | same |
| 470-608 | UHF TV | broadcast | Region 1 470-694, Region 2 470-512 / 512-608, Region 3 470-585 / 585-610: BROADCASTING | same |
| 614-698 | UHF TV | broadcast | Region 1 470-694 / 694-790, Region 2 614-698, Region 3 610-890: BROADCASTING (608-614 Region 2 is radio astronomy) | same |
| 698-862 | UHF TV / mobile | broadcast | Region 1 694-790 / 790-862, Region 2 698-806 / 806-890, Region 3 610-890: BROADCASTING and MOBILE, both primary | same |
| 902-928 | ISM 915 (R2) | ism, **Region 2 only** | No. 5.150: "902-928 MHz in Region 2 (centre frequency 915 MHz)"; Region 2 column 902-928: FIXED, Amateur, Mobile except aeronautical, Radiolocation, 5.150. Regions 1 / 3: 890-942 fixed / mobile | same (5.150 text, Region 2 column) |
| 960-1164 | DME / SSR | aero | Art. 5: AERONAUTICAL MOBILE (R), AERONAUTICAL RADIONAVIGATION; No. 5.328 | same |
| 1164-1215 | GNSS L5 | nav | Art. 5: AERONAUTICAL RADIONAVIGATION, RADIONAVIGATION-SATELLITE (space-to-Earth); No. 5.328A | same |
| 1215-1240 | GNSS L2 | nav | Art. 5: EESS (active), RADIOLOCATION, RNSS (space-to-Earth), SPACE RESEARCH (active) | same |
| 1240-1300 | GNSS E6 / 23 cm | nav | Art. 5: as 1215-1240 plus Amateur (secondary); No. 5.282 | same |
| 1300-1400 | L-band radar | nav | Art. 5: RADIOLOCATION (1300-1350 with AERONAUTICAL RADIONAVIGATION, No. 5.337); 1350-1400 RADIOLOCATION in every column | same |
| 1400-1427 | Radio astr. 1420 | science | Art. 5: EESS (passive), RADIO ASTRONOMY, SPACE RESEARCH (passive); No. 5.340 | same |
| 1452-1492 | L-band bcast | broadcast | Regions 1 and 2 / 3 columns: BROADCASTING, BROADCASTING-SATELLITE (with FIXED, MOBILE) | same |
| 1518-1559 | L-band satcom | satellite | MOBILE-SATELLITE (space-to-Earth) in every column 1518-1525, 1525-1530, 1530-1535; Art. 5 1535-1559 | same |
| 1559-1610 | GNSS L1 | nav | Art. 5: AERONAUTICAL RADIONAVIGATION, RNSS (space-to-Earth) | same |
| 1610-1626.5 | Sat phones | satellite | Every column: MOBILE-SATELLITE (Earth-to-space), AERONAUTICAL RADIONAVIGATION; RADIO ASTRONOMY 1610.6-1613.8; Mobile-satellite (space-to-Earth) 1613.8-1626.5 | same (MMSS 1621.35-1626.5 is WRC-19, named only in the full name) |
| 1626.5-1660 | L-band sat up | satellite | Art. 5: MOBILE-SATELLITE (Earth-to-space) | same |
| 1660-1668.4 | Radio astr. 1665 | science | Art. 5: RADIO ASTRONOMY 1660-1660.5, 1660.5-1668, 1668-1668.4 (SPACE RESEARCH (passive) from 1660.5) | same |
| 1668.4-1710 | Met sats/sondes | science | Art. 5: METEOROLOGICAL AIDS 1668.4-1690; every column 1690-1700 MET AIDS + MET-SAT, 1700-1710 MET-SAT (space-to-Earth) | same |
| 2025-2110 | S-band sat up | satellite | Art. 5: SPACE OPERATION, EESS, SPACE RESEARCH (Earth-to-space) | same |
| 2200-2290 | S-band sat down | satellite | Art. 5: SPACE OPERATION, EESS, SPACE RESEARCH (space-to-Earth) | same |
| 2300-2400 | 13 cm amateur | amateur | Region 1 and Regions 2 / 3 columns 2300-2450: Amateur (secondary) | same |
| 2400-2500 | ISM 2.4 GHz | ism | No. 5.150 (centre 2450 MHz) | same footnote |
| 2690-2700 | Radio astr. 2.7G | science | Art. 5: EESS (passive), RADIO ASTRONOMY, SPACE RESEARCH (passive); No. 5.340 | same |
| 2700-2900 | ATC radar | nav | Art. 5: AERONAUTICAL RADIONAVIGATION (No. 5.337), Radiolocation | same |
| 2900-3100 | Radar 3 GHz | nav | Art. 5: RADIOLOCATION, RADIONAVIGATION (No. 5.426) | same |
| 3100-3400 | Radar 3.1-3.4 | nav | Art. 5 3100-3300 RADIOLOCATION; 3300-3400 RADIOLOCATION in every column | same |
| 3400-4200 | C-band sat | satellite | FIXED-SATELLITE (space-to-Earth) in every column (3400-3600 / 3400-3500, 3500-3600, 3600-3800 / 3600-3700, 3700-4200 / 3800-4200) | same |
| 4200-4400 | Radio altimeters | aero | Art. 5: AERONAUTICAL MOBILE (R), AERONAUTICAL RADIONAVIGATION; No. 5.438 | same |
| 4990-5000 | Radio astr. 5G | science | Art. 5: FIXED, MOBILE, RADIO ASTRONOMY, Space research (passive) | same |
| 5000-5150 | Aero 5 GHz | aero | Art. 5: AERONAUTICAL RADIONAVIGATION in 5000-5010, 5010-5030, 5030-5091, 5091-5150; No. 5.444 (MLS) | same |
| 5150-5350 | Wi-Fi 5 GHz | mobile | Art. 5: MOBILE except aeronautical mobile with No. 5.446A (RLAN, Resolution 229) | same |
| 5350-5470 | Radar 5.4 GHz | nav | Art. 5: EESS (active), RADIOLOCATION, AERONAUTICAL RADIONAVIGATION (No. 5.449) / RADIONAVIGATION | same |
| 5470-5725 | Wi-Fi 5.6 GHz | mobile | Art. 5: MOBILE except aeronautical mobile, No. 5.446A; RADIOLOCATION; MARITIME RADIONAVIGATION 5470-5650 | same |
| 5725-5875 | ISM 5.8 GHz | ism | No. 5.150 (centre 5800 MHz) | same footnote |
| 5925-6700 | C-band sat up | satellite | Art. 5: FIXED, FIXED-SATELLITE (Earth-to-space), MOBILE | same |

**Tests** (2026-11-24, job scratch 9321609f sigid3/w1/: a TEST build of
this tree on ports 47380 / 47381 / 47379 with a fake heimdall on 47391 /
47392, exe `kdsigA_t473`, the shipped model; headless Chromium over the
DevTools pipe, real mouse / wheel / touch events):

- `ui_bands.py` 40 / 40: the On spectrum switch gone; Bands off in a fresh
  browser; labels with Signal ID on and no switch; a stored `{spec:false,
  min:0}` shows labels and keeps Confidence (All), the `spec` key dropped
  at the next save; a real click on Bands shows the strip with Signal ID
  off, stored beside Confidence, kept over a reload. Geometry (each
  segment's edges against the axis): 98 MHz one segment "FM broadcast"
  edge to edge (0.00 px); 108 MHz FM broadcast | VOR / ILS (0.39 px);
  137.5 MHz Airband | Weather sats 137 and a gap from 138 MHz (0.16 px);
  450 MHz empty with its gap tooltip; zoomed by 5 wheel notches (0.00 px);
  after a pan drag (0.00 px); the axis driven to 4380-4420, 5790-5830,
  5860-5940, 6650-6690 and 2399-2401 MHz (the Wideband variant's range;
  <= 0.50 px); the wideband-scan stitch 103-113 MHz (0.02 px); five
  independent panes at 98, 145.5, 162, 450, 406 MHz (<= 0.35 px; 406 MHz:
  Radiosondes | 406 beacons | Radio astr.) and a zoomed pane (0.35 px).
  The tooltip on hover (full name, range, service, provision) and in help
  mode with a touch tap; a click on the strip changes nothing;
  click-to-tune, a VFO drag and wheel zoom on the plot with the strip on;
  drive mode in and out (the strip hidden with the spectrum, right again
  after); no page errors
- `ui_bands_pb.py` 7 / 7: a debug playback (`pocsag-three-rates`, the
  recording at 157.65 MHz): one segment "Marine VHF", 0.00 px; the phone
  layout (412 px, touch): 0.35 px, no horizontal scroll; no page errors
- `ui_bands_old.py` 4 / 4: this page against a receiver built from main
  before Signal ID: "Not on this receiver", the strip at 98 and 137.5 MHz
  as against the new receiver, no page errors
- The Signal ID UI suites of 2026-11-20, rerun with the Bands bar on (the
  On spectrum check replaced by "no switch, labels drawn"): ui_sig 18 / 18,
  ui_fixes 12 / 12, ui_touch 3 / 3 taps, ui_pane 7 / 7
- Drawing cost (`drawBox`, 400 calls, the median of 20-call batch means at
  108 MHz, four runs): off 0.030-0.085 ms, on with the strip cached
  0.030-0.080 ms - no difference above the run-to-run noise (on - off:
  -0.010, -0.005, +0.005, 0.000 ms); rebuilt on every call (a pan / zoom
  frame) 0.035-0.080 ms; the strip alone rebuilt 0.005-0.010 ms
- `band_table_check.py`: 57 rows OK; `theme_palette.py --check` OK

After the orchestrator's review (the same day: the two Region-only ISM
rows, co-primary services in the full names, the label one step brighter
and larger, a test for segment re-use), rerun on the same setup:

- `ui_bands.py` 60 / 60 - the 40 checks above plus segment re-use: after
  retunes 100 -> 406.05 -> 162 -> 433.9 -> 98 -> 902.5 MHz (the same DOM
  element re-used throughout, its title already moved to `data-tip` from
  the second retune on), a real mouse hover on each segment wider than 8
  px shows THAT band's tooltip, its last line "Worldwide ..." or "Region 1
  / 2 only ..." as the row says (10 / 10), and a touch tap in help mode
  shows it too (10 / 10). Drawing cost on - off: 0.000 ms
- `ui_bands_pb.py` 7 / 7, `ui_bands_old.py` 4 / 4 (now waiting until the
  fake heimdall's frames carry the new frequency - it rebuilds its scene
  for seconds; once it swapped the two frequencies' results), ui_sig 18 /
  18, ui_fixes 12 / 12, ui_touch 3 / 3 taps, ui_pane 7 / 7
- `band_table_check.py` 60 rows OK, the five faulty copies fail
  (neg_check.py 6 / 6); `theme_palette.py --check` OK
- Legibility: 11 px `--t2` instead of 10 px `--t3` (screenshots at 1500 x
  1000 in the scratch's shots_legib/: before_* / after_*)

## Tests (2026-11-20, job scratch 9321609f integ/)

Setup: a TEST build of the worktree (ports 4718x / 4719x, MCP 47179, exe
`kraken_doa_sigtest`, own working directories, all 32 plugins built), a
sigclass.model with RANDOM weights written by `tools/sigclass/model_io.py`
from `classes.json` and the built tool's layout (class names arbitrary - the
tests check frequencies, bandwidths, states and the page, not the classes),
the debug playback (no heimdall) and a fake heimdall (Python, its own ports:
coherent UCA scene, plus independent / wideband-scan modes with per-tuner
frequencies and the mode bits, the noise flag). Headless Chromium over the
DevTools pipe with real mouse / wheel events. The machine was loaded (load
6-23 from other jobs) - timings are upper bounds.

- Off by default (fresh settings): the state says off, the worker thread
  used 0 CPU ticks over 15 s (no capture armed)
- On: the first tracks 0.6 s after `SIGCLASS:1` (fake heimdall), 2.5 s
  (playback, which starts meanwhile). Fake scene at 100 MHz: NBFM 99.750 MHz
  (4.1 kHz), CW 100.100 000 (60 Hz), 50 kHz noise band 100.399 8 MHz (49.0
  kHz) - every one where it is
- No control command reached the fake heimdall in 35 s with it on
- Noise source on: parked "calibrating (noise source on)", tracks cleared, no
  snapshot while parked; off: running again, tracks back
- Retune (coherent, 100 -> 145 MHz): the scene's signals at their new
  frequencies within a few snapshots; the 100 MHz tracks aged out
- Independent (tuners at 100 / 145 / 100 / 102 / 100 MHz): tracks of every
  tuner (145.2 and 144.7 MHz only on tuner 1, 101.9 on tuner 3); the mode
  change cleared the coherent tracks. One channel (scene with 1 tuner,
  independent): 36 snapshots of tuner 0, its tracks. Wideband scan (tuners
  96 .. 104 MHz): tracks at 98.3, 99.75, 100.1, 100.4, 101.9 MHz on the
  stitched spectrum
- Debug playback (30 s each, Signal ID on; the sidecar's tuner + band
  offset + truth shift): `pocsag-three-rates` 157.950 MHz (the pager + FM
  interferer); `dmr-repeater-and-direct` 439.0127 MHz (truth 439.0122 -
  .0128, 6.8 kHz wide); `rds-two-stations` 98.500 MHz (+0.0 kHz);
  `dab-single-offset` 227.385 MHz, 1.506 MHz wide (the ensemble);
  `adsb-six-aircraft` a 2.29 MHz-wide bursty track at 1090.0 MHz plus
  narrower pieces; `lora-mesh-five` bursty tracks at 921.30 / 921.74 /
  922.30 MHz (121 - 397 kHz); `ism-six-devices` many short narrow tracks
  across 433.8 - 434.9 MHz (the sidecar has no per-device offsets to check
  against). With the random model the classes mean nothing; the core
  often splits one signal into several overlapping tracks (7 within +-5 kHz
  of the pager) - a core matter. Switching entries / Stop: each entry lists
  only its own band (the previous entry's tracks leave the list at once);
  after Stop "no data from the receiver" and the tracks age out (45 s)
- Persistence: a restart (heimdall connected - the settings replay waits
  for it) comes back enabled; a second page gets the state on connect
- Playback sandbox: `SIGCLASS:1` saved, a playback with it toggled off / on /
  off inside: doa_settings.json byte-identical during and after (md5), on
  again after Stop (the replay)
- Model missing: `available:false`, reason "cannot open sigclass.model",
  one stderr line in 30 s; a model of another feature layout: the loader's
  reason; an off / on loads a replaced file; `KRAKEN_SIGCLASS_MODEL` works
- CPU (pocsag playback, 120 s each, CPU time): the whole process 26.1 %
  of a core on, 23.9 % off; the `sigclass` thread 3.1 % (analysis 55-100 ms
  a snapshot here, period 1.4-2.6 s). `perf stat` is not allowed on this
  machine (perf_event_paranoid 4), so no cycle counts. Pi 5: assume ~3x the
  analysis time - the period then grows to keep the 4 %
- UI (ui_sig.py, 17 checks, on the playback and on the wideband scan):
  state on connect, labels at their signal's x (0 px), above their peak, no
  overlaps, nothing in the top rows, the box's pixels (backing + text), the
  hover tooltip and hand, a click off the label's centre tunes the VFO
  exactly to the signal (frequency, bandwidth, demod; not where the press
  was), the list (a row per signal, the count, a row click tunes), On
  spectrum off / on and remembered, zoom x 6 wheel notches (labels follow),
  the switch off / on through the receiver, no page errors. ui_pane.py (7):
  independent panes - each labels only its own span, x exact, hover, a click
  tunes the VFO onto that pane's tuner. ui_touch.py: three taps (CDP touch
  events) on a label tune the VFO to it. Compatibility (ui_compat.py): this
  page against HEAD's receiver, HEAD's page against this receiver - no
  errors either way
- Not run: Windows / Linux-glibc syntax checks (no MinGW / zig on this
  machine); a Raspberry Pi; Docker itself (the `sigclass.mode[l]` COPY
  pattern relies on BuildKit allowing an empty wildcard); a real KrakenSDR /
  heimdall (only the fake and the playback); the Wideband downconverter
  variant (reasoned from heimdall's code, not run)

### Review round (2026-11-20, after an independent review)

Fixed: tracks frozen by a stream of retunes (no expiry / publish without a
snapshot), a tuner's tracks expiring between its looks with tuners in turn,
the model's `min_confidence` and `hidden` classes ignored, a wide track's
bracket taking every press on the spectrum, Docker replacing a user's
model, `std::isfinite` (now `is_finite_value`), the loader accepting a
temperature that makes every probability NaN and a 32-bit size overflow,
"Not on this receiver" shown from page load, list clicks lost to a state
message, fixed-frequency decoders started for a label far from their
frequency. Tests (scratch integ/ and the reviewer's harness in review/: the
service compiled against stubs with simulated receiver / uWS threads):

- Retune storm (harness, a retune every 0.7 s between 100 and 150 MHz for
  60 s): before, "settling" with the ten 100 MHz tracks listed at ages
  60-64 s and no broadcast after the first; now 69 broadcasts, each band's
  tracks listed only while tuned there, the 100 MHz ones at ages up to 43.7
  s and gone from 44 s on
- 8 tuners in turn, analysis 0.9-1.0 s (8 harness runs sharing one core),
  period 10 s, a round 80 s: every tuner keeps 9-12 tracks at every sample
  over 300 s (before: 3 of 8 tuners empty at any time, at 0.4 s analysis)
- Clang ThreadSanitizer race run (40 s of random enable / disable / state /
  packets from three threads): 0 reports (GCC 11's TSan gives false reports
  on `wait_for` with a steady clock); the model fuzzer: temperature 1e-39
  refused, no non-finite probability in 436 k mutated models
- `sigclass_tool selftest` passes; C++ `predict` vs `model_io.forward` on
  the real model, all 4 views, random / 1e3-scaled / 1e30 inputs: worst
  difference 1.8e-4, all finite
- Docker `link_if_absent` (test_link.sh, 7 checks): image model linked,
  kept on restart, a user's file or own link kept, no dangling link when
  the image has none, `link_files` unchanged
- UI with the REAL model: ui_fixes.py 12 / 12 (state carries
  min_confidence 0.94, default cut 50 %, "?" exactly below it, Auto = 94 %
  and remembered, `none` never sent; a press on a full-width bracket - on
  a fake DAB track - shows its tooltip, no hand, takes no press, the VFO's
  width and decoder unchanged; its box still tunes; a fixed-frequency
  plugin (adsb) 50 MHz off is not started, 1 kHz off is; list rows survive
  a state message and a click across one still tunes; MHz in the list to
  the kHz; no "Not on this receiver" at load), ui_sig.py 18 / 18 (one run
  17 / 18 before), ui_pane.py 7 / 7, ui_compat.py 3 + 3, multi.py 12 / 12
- Real model through the debug playback (40 s each, the label at the
  entry's frequency): POCSAG 100 %, DMR 97 %, P25 Phase 1 100 %, YSF 99 %,
  VDL2 98 %, LoRa 98 %, ADS-B 100 %, UAT 100 %, DAB 100 %, LTE 100 %, FM
  broadcast 99 %, MPT1327 100 %, VOR 99 %, DME 99 %, Analog TV 100 %, AIS
  98 / 96 % (both channels), ISM OOK / FSK 81-99 % (9 devices' tracks),
  FLARM 85-98 % (4 tracks); below the model's 94 % ("?"): TETRA 89 %, DVB-T
  90 %, pulse radar 52 %, wildlife 71 %; wrong: NXDN (Unknown FSK 71 %),
  D-STAR (406 MHz beacon 68 %), APRS (Unknown FSK 20 %), ACARS (ISM OOK 21
  %), radiosonde (Unknown FSK 52 %), 406 MHz beacon (Unknown PSK / QAM 55
  %). Tracks per transmitter: 1 for most narrow signals; 2 (POCSAG), 3
  (DVB-T, UAT, VOR), 6-9 (DME), 41 (the analog TV test card's lines and
  carriers). Data for the trainer, not runtime bugs


### Final verification (2026-11-20, the shipped model: layout v3, model format 3)

TEST builds of the final tree (two instances: 4718x and 4728x), the shipped
`sigclass.model` (min_confidence 0.82, per-class `min_p`).

- Edge filter (edge_test.py, fake heimdall at 100 MHz with a carrier at +1.1
  MHz and a 250 kHz band at +0.88 MHz): with EDGE_CLIP 0.80 the +1.1 MHz
  carrier is not sent and nothing centred beyond +-0.96 MHz is; the band is
  sent with `hi` = +0.96 MHz; with 1.00 the carrier is sent; back to 0.80
  gone again (5 / 5)
- The one-hit rule (onehit_check.py over every snapshot of two 28-entry
  playback tables, from TEST-only instrumentation that logs the trackers'
  tracks): 459 one-hit tracks - 34 sure ones sent, 425 unsure / hidden /
  outside not; 874 tracks with >= 2 hits sent exactly when not hidden and
  inside the clipped band; 0 mismatches
- The page's sure level per track (ui_minp.py, 7 / 7): below its class's
  `min_p` subdued with "?", at or above normal, `min_p` 1.01 never sure, no
  `min_p` = the model's min_confidence; the drawn labels follow
- Suites: multi.py 9 / 13 - off by default, no CPU while off, no command to
  heimdall, noise-source park / resume, independent mode, one channel
  pass; the 4 misses are the test's expectations, not the service: the
  scene's noise-modulated "NBFM" at 99.75 MHz is classed `none` (hidden,
  correctly unsent) by the shipped model, and the retune check expects
  absolute frequencies the coherent fake doesn't use (its tracks follow the
  retune to 144.75 / 145.1 / 145.4 MHz as they should). ui_sig.py 18 / 18,
  ui_fixes.py 12 / 12, ui_touch.py 3 / 3 taps, ui_pane.py 7 / 7,
  ui_compat.py 3 + 3 (both directions)
- The reviewer's harness: retune storm - the 100 MHz tracks listed at ages
  up to 45.6 s then gone, 77 broadcasts; 8 tuners in turn with 0.5 s
  analysis (12 runs sharing one core: 10 s period, an 80 s round) - no
  tuner without tracks in the last 150 s of any run
- Strict debug playback, all 28 entries, 40 s each, twice: 28 / 28 right
  both times (25 + 24 sure; unsure: DMR 81 / 80 % and DVB-T 59 / 69 %
  below 0.82, POCSAG 76 % in the second run, wildlife 82 / 94 % - its
  `min_p` 1.01: never sure). One track per transmitter for most entries;
  ISM 9-13 tracks (its devices sit on several channels), radiosonde 3-4,
  LoRa 3, VOR 1-3, DME 2-3, AIS 2 and FLARM 2-3 (one per channel)
- APRS (the training worker saw no track in 2 of 7 earlier live runs): no
  failure in 7 runs here (2 tables + 5 independent runs from a cleared
  state). Per run 18-25 of 38 snapshots had a detection at 144.575 MHz
  (aprs in 17-19, `none` in 0-6, another class in 0-3); the APRS track was
  sent in every snapshot after its first (never flipped to hidden, never
  held back by the one-hit rule). Not reproduced, so no cause found; if it
  happens again, a track whose first looks are `none` stays hidden until the
  averaged evidence (~10 looks) turns - the tracker / model, not the service
- CPU (TEST build, playback, CPU time of the `sigclass` thread): POCSAG
  2.3 % of a core (analysis 22.5 ms, period 1.0 s - the floor), DAB 2.85 %
  (28 ms), LoRa 3.5 % (36 ms); the whole process with DAB 38.9 % on, 36.8
  % off

### Multi-tuner modes (2026-11-24: wideband scan / independent mode missed labels)

The user's report: labels shown in coherent mode missing in wideband scan
and independent mode. Job scratch 9321609f `sigid3/w2/`: a fake heimdall
(`fake_hd3.py`) that streams REAL recordings in the three modes as
heimdall_v2 does (`tcp_data_server.cpp`: per-channel float32 frequency
and gain, 0x400 / 0x800, the noise flag only for coherent recalibrations,
wideband scan's plan `setup_wideband_frequencies` - the middle tuner on
the centre, 2.4 MHz x EDGE_CLIP apart; a retune changes the header at once
and the samples a few packets later); a tuner whose frequency matches a
recording's centre streams it, the other tuners a busy filler recording
(load). The same IQ through (a) coherent at the recording's centre, (b)
wideband scan with the recording on the middle tuner, (c) independent with
five recordings on five tuners, and (d) the user's own layout (416.760 /
416.815 x3 / 859.440 MHz, the 416.5 MHz capture shifted). 180 s a run, the
TEST build, the shipped model, two runs in parallel on a loaded machine
(analysis 50-200 ms: Pi-like periods of 1.4-5 s). A signal "labelled" =
shown on the page (a class, p >= 0.5) for >= half of coherent mode's last
60 s; scored in the other modes over the same window (`ab_table.py`).

- **Cause 1 (the report)**: every multi-tuner label lost its class and name
  - the overlap rule's self-move (*Tuners in turn*). 9751af3: 0 of 7437
  tracks published in a wideband-scan run named; labelled with the
  coherent class: wideband scan 0 / 78, independent 0 / 78, the user's
  layout 8 / 10 (the tracks above the first duplicate). The page (headless,
  ui_modes.py): no label on the stitched spectrum
- **Cause 2**: too few looks for intermittent signals - with the names
  ignored (p alone), 9751af3 labelled 65 / 78 (wideband scan) and 64 / 78
  (independent); the misses and the low shares (11-59 % of the time
  against 82-100 %) were P25 / DMR voice channels and a telemetry burst:
  a band in turn is looked at every round (5 bands x the period), and the
  2-missed-looks expiry dropped the track before the signal came back,
  its new track starting at one hit (unpublished unless sure)
- **Cause 3**: a mode switch left the old mode's labels on the new mode's
  spectrum (coherent tracks on the stitched spectrum 0.6 s after the
  switch) - the worker saw the change only after its rest / capture
- **Measured, not a cause**: the centre per snapshot (the tracks of one
  signal agree across the modes: median 18 Hz, 90 % within 220 Hz, max
  847 Hz over 69 narrow signals (the smoothed centres) - float32 header plus
  the exact request); captures timing out with 5 channels (looks per tuner
  rise evenly: 13-27 per tuner in 180 s); the edge clip and the overlap
  rule with wideband scan's spacing (2.4 MHz x EDGE_CLIP: the clipped bands
  meet, no gap, no overlap); a retune's stale samples (tuner 0 hopping
  499.8 / 433.92 MHz every 15 s with 136 ms of old samples under the new
  header: 0 ghost tracks published before and after - the per-tuner settle
  is by code)
- **After** (fb5cf77 + 6df39a8), labelled with the coherent class -
  wideband scan / independent, before -> after: P25 499.8 MHz 0/24 ->
  23/25 / 23/25; DMR 456.8 MHz 0/12 -> 13/13 / 10/13; DMR + MPT1327 416.5
  MHz 0/14 -> 16/17 / 16/17; 418.0 MHz 0/10 -> 9/9 / 9/9; P25 142.3 MHz
  0/12 -> 11/13 / 12/13; VOR 114.5 MHz 0/3 -> 2/2 / 2/2; the user's layout
  8/10 -> 11/13. Totals 74 / 79 and 72 / 79 (coherent 92 / 92). Every miss
  is an intermittent signal (coherent: shown 62-85 % of the time or first
  after 19-96 s), plus two class disagreements on unsure labels (P25 / NXDN
  0.66; a 320 kHz "Unknown pulses" hump scored against the DMR channel
  inside it). With the run as a whole (shown at any time, the same class)
  75 / 79 and 73 / 79. The 433.92 / 921.8 / 98 MHz captures gave no label
  >= 0.5 in coherent mode (nothing to compare)
- **Time to the first label**: continuous signals coherent mode labels
  within 10 s: median 2 s in wideband scan and independent mode (the
  first looks at capture speed), the 90th percentile within a round or two
  of the bands (unsure labels need a second look): max 25 s (wideband
  scan), 67 s (independent, five busy bands at 3-4 s periods). The bound
  held: a band's sure labels within one round of captures (~0.6 s + the
  analysis per band), the rest within 3 rounds of periods - on a Pi 5
  (periods 3-8 s) up to ~2 min for an unsure label with 5 busy bands; a
  signal on air for D seconds is seen with probability about (D + 0.5) /
  the round (coherent: / the period)
- **CPU** (the `sigclass` thread's CPU time / the run): coherent 3.80-4.08
  % (before 3.83-4.16 %), wideband scan 3.76-4.03 % (3.89-4.03), independent
  3.99-4.02 % (3.98-4.03) - unchanged; the first-look debt is paid back
  within the run. The user's layout: looks [25, 25, 0, 0, 24] (tuners 2
  and 3 share tuner 1's band) against [14, 13, 13, 13, 13]
- Rejected: a larger share in multi-tuner modes (8 % would halve the round
  - not "a few percent"); looks only where unlabelled energy is (every
  band of a busy trunked site has some; no gain in these scenes);
  detector-only looks for settled bands (the detector is 32 of 100 ms on
  the busy 416.5 MHz snapshot, so ~2x the looks - but a core change on a
  branch measuring the core's speed; left as the next step)
- Page end to end (ui_modes.py, headless Chromium, real mouse, the
  receiver as above): coherent -> wideband scan -> independent (the user's
  layout) -> coherent: labels at their signal's x (<= 1 px) on the main /
  stitched spectrum and in every pane (pane 2, a tuner sharing tuner 1's
  band, too), each pane only its span, a label press tunes the VFO to the
  signal (on that pane's tuner), no stale label in 10 Hz samples after each
  switch, the band's coherent labels back in wideband scan (11 / 11, the
  first after 1.9 s), labels back in coherent after the recalibration
  (4.8 s), no page errors: 18 / 18 (9751af3: 15 / 18 - no label in
  wideband scan). On 1e8871d twice 17 / 18: the band's labels back in
  wideband scan 9 / 13 (the miss: intermittent / unsure ones), then with
  only the steady ones required (5+ looks, in the latest) 7 / 8 - the miss
  the 320-440 kHz hump at 416.4 MHz, whose class is unstable in coherent
  mode too (Unknown pulses / LoRa / DMR)
- Live, the dev machine's KrakenSDR (heimdall from scratch, --serials
  1000..1004, receive only, bias tees 0 at start and end), centre 416.5
  MHz gain 29.7 dB, 3 min coherent -> 3 min wideband scan (tuners 412.66 ..
  420.34, the 416.5 band on CH2) -> 3 min independent (the user's layout,
  bands CH0 416.760 / CH1 416.815 (CH2, CH3 sharing it) / CH4 859.44;
  looks [24, 22, 0, 0, 23]): coherent (CH0) 12 signals labelled; wideband
  scan 4 / 12, independent 5 / 8 (4 outside its tuners). The losses are the
  ANTENNAS: each tuner listens on its own channel, and the levels differ -
  DMR 415.638 MHz 59.9 dB SNR on CH0, 56.4 on CH2; MPT1327 416.587 MHz
  56.5 / 49.6 (CH2) / 56.2 (CH0 again in independent mode); MPT1327
  416.7495 MHz 15 dB on CH0, not detected on CH2, 3.6 dB (p 0.40) on CH1 -
  where the overlap rule then hid CH0's sure view (fixed: the stronger view
  wins, 1e8871d). The weak signals (6-17 dB on CH0) drop below the
  detector on another channel; that is the receiver, not the service.
  Live run 2 (1e8871d; 456.8 MHz, gain 40.2 dB; independent: CH0 and CH1
  on 456.8 - one band, looks [17, 1, 18, 18, 17] -, CH2 416.5, CH3 499.8,
  CH4 98.0 MHz with their gains): coherent 9 labelled; independent 8 / 8
  (CH0 again); wideband scan (the band on CH2) 7 / 9 - DMR 457.682 MHz
  11.9 dB on CH0, not detected on CH2, and an intermittent channel.
  Live run 3 (499.8 MHz, gain 33.8 dB; independent: CH0 and CH4 on 499.8 -
  one band, looks [16, 17, 16, 16, 1] -, CH1 456.8, CH2 416.5, CH3 415.4
  MHz): coherent 14 labelled; independent 12 / 14 (the misses intermittent:
  67 / 75 % in coherent); wideband scan 9 / 14 - P25 499.364 / 500.726 MHz
  and an AM carrier read 1-11 dB on CH0 and nothing on CH2. Bias tee mask 0
  at the start and the end of every run
- Regressions (1e8871d, TEST build, the shipped model): the strict
  debug-playback table twice 28 / 28 (24 + 25 sure; unsure POCSAG 75 / 68
  %, DVB-T 70 / 75 %, wildlife 81 / 65 % - never sure by its min_p -,
  radar 77 % once); p2_ui_sig 18 / 18, p2_ui_fixes 12 / 12 (with the
  plugins built; 11 / 12 without - the adsb plugin check), p2_ui_touch 3
  / 3 taps, p2_ui_pane 7 / 7, p2_switch, p2_sandbox (the settings file
  byte-identical), p2_sc, p2_pb (the DMR entry's three units on one 6.9
  kHz track within 0.5 kHz); p2_multi 9 / 13 - the same 4 misses as the
  shipped state (the scene's noise-modulated NBFM classed `none`, the
  retune check's absolute frequencies), no control command to heimdall in
  35 s, no CPU while off
- Rules (`tools/sigclass_rules_test.cpp`, 20000 random inputs, two seeds):
  every invariant held; the mutants caught - 9751af3's overlap code (a kept
  track changed in 19025 / 20000), the pairwise scan, no tuner test, the
  worse view kept, band representatives chained, the old round robin, no
  settling, no first looks first, a stuck cursor, first looks without a
  debt, no payback, the debt cap ignored

### Review round of the multi-tuner rules (2026-11-24)

An independent review ran the unmodified service in a harness (simulated
receiver / uWS threads) and a time-stepped model of the worker loop on the
real rules; its findings became rules with invariants (*Tuners in turn*):
no band starves whatever is retuned (INV-A..D), a band keeps its labels when
its lead tuner changes (INV-E), a 180 s wall-clock ceiling (INV-F), toggling
Signal ID buys no CPU (INV-G), the trackers of tuners beyond a lowered
channel count are cleared. Job scratch 9321609f `sigid3/w2/` (r1/: the
reviewer's harness, a ceiling scenario added).

- Rules test (`tools/sigclass_rules_test.cpp`, default 200000 rounds, ~25 s;
  seeds 1-16 plain, three more under ASan + UBSan): every invariant held.
  The worker-loop model: 2-8 tuners, 1-4 of them retuned periodically
  (periods 0.3-20 s, now and then onto another tuner's band), random one-off
  retunes, analysis 20-400 ms, captures aborted by a retune, Signal ID
  toggled; INV-A with R = the tuners standing for a ready band in the
  stretch (each has its own token - with the instantaneous ready count three
  scanned tuners taking turns ahead of a steady band broke a 4-look window
  that had 4 contenders). Mutants caught: fb5cf77's pick (INV-A, ~1200 of
  2000 runs), no token (INV-A), a new band first in the turn order (INV-A),
  the token copied with the band (INV-C, a few per 5000 runs), no settling,
  the debt reset on enable (INV-G), no first looks ahead (INV-B), fb5cf77's
  trackers staying with their tuner (INV-E), plus the earlier ones. Found by
  the model on the way: a tuner's band fields copied on regrouping laundered
  its token (fixed: the token is the tuner's own); keeping a retuned band's
  old turn let new bands queue ahead of steady ones (fixed: a new band's
  turn counts from its start)
- The reviewer's harness on the new service: `scan` (5 tuners, tuner 1
  retuned every 2.8 s, 120 s): looks [19, 18, 19, 18, 18] (fb5cf77: [1, 42,
  41, 8, 1]; control without retunes [15 x 5] at 100 s), worker CPU 3.9 %.
  `lead` both ways (tuner 1 leaves the shared 105 MHz band / tuner 0 joins
  it at 30 s): the band's 7 tracks in every 0.5 s state from 25 to 80 s, 0
  snapshots without one (fb5cf77: 0 for 4-5.5 s, all back after 29-43 s;
  a first version of the move still showed 0 for 1-1.5 s: a publish made
  the list from the old slots before the worker's next regroup - now every
  publish regroups first). `ceiling` (new: 5 tuners, no data from 30 s): 34
  tracks until 200 s, 0 from 215 s - 180 s after each was last seen
  (fb5cf77: never - a band without looks had a stopped clock). Clang TSan
  `race 40` (setarch -R): 0 reports; ASan + UBSan `lead 0` and `scan`: 0
  reports. `chan` (new: 5 tuners 25 s, 3 channels 20 s, 5 again): the
  tracks of tuners 3-4 just after the return 0 for the 3 s sampled - new
  looks needed (1e8871d, the same scenario: 17-18 back after 1 s, aged).
  The reviewer's model on these rules (600 s, 300 ms analysis):
  tuner 1 retuned every 2.8 s - looks 17 / 13 / 17 / 17 / 16, the longest
  gap 41 s (61 s for the retuned one); 4 tuners retuned every 2 s - 60 / 6 /
  5 / 5 / 4 (fb5cf77: 20 / 0 / 0 / 0 for the scanned ones), share 4.00 %
- End to end (TEST build + fake heimdall on real IQ, independent mode,
  tuners 499.8 / 456.8 / 416.5 / 418.0 / 142.3 MHz, 180 s): the
  saved-frequency scan on tuner 4 (VFOs on tuners 0-3, 7 saved frequencies
  on 7 bands, 2 s dwell - it hopped tuner 4 throughout): looks [21, 21, 21,
  21, 20], the longest run of looks without a steady tuner 4 (INV-A's bound
  2R = 10), the longest wall gap 10.6-14.7 s; tuner 4 stepped by TUNER_FREQ
  every 2.8 s (64 retunes): [20, 20, 20, 20, 19], longest run 4. The steady
  tuners kept labelling: 13-32 labels each, 3-18 of them first shown after
  the first 30 s
- The A/B matrix again (9346f16, the same IQ and scoring): coherent 92 /
  92, wideband scan 73 / 80, independent 65 / 80 - rerun twice 72 / 80 and
  68 / 80 (the static independent schedule is the round robin as before,
  INV-D; the run-to-run spread is the intermittent signals - every miss had
  coherent shares under 100 % or first labels after 20 s, except two P25
  voice channels that coherent mode happened to hold all of the last 60 s);
  the user's layout 11 / 12. Continuous signals first labelled: median 1.8 s
  (wideband) / 3.1 s (independent), max 35 / 60 s. CPU (the thread):
  coherent 3.69-4.01 %, wideband scan 3.95-4.07 %, independent 3.96-4.02 %
- ui_modes.py 17 / 18 (as on 1e8871d: the miss is the 416.4 MHz hump whose
  class flips between LoRa / Unknown pulses / DMR in coherent mode too)
- Strict debug playback: 28 / 28 once, 27 / 28 once (wildlife-five without
  a track at the end of its 40 s; the entry alone three more times: 3 / 3,
  76-80 %, unsure as always - its min_p 1.01); the p2 suites on the merged
  page: p2_pb, p2_switch, p2_sandbox (byte-identical), p2_sc, p2_ui_pane 7 /
  7, p2_multi as before (the scene's known misses); p2_ui_sig stops at its
  "On spectrum" check (the switch is gone with the Bands page), p2_ui_fixes
  11 / 12 and p2_ui_touch 2 / 3 taps - page checks written before the
  Bands page; the service path they use (a coherent playback) is unchanged
