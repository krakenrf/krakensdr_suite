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
  - IQ image: a detection at the mirror frequency (-f) of one >= 15 dB
    stronger and at most twice as wide is dropped - the tuner's imbalance
    image carries the strong signal's modulation, and the model named it by
    that class (in the training data the largest group of confident wrong
    labels; with 25-55 dB signals in the composer: images 25-40 dB down)
  - an irregular comb of >= 10 lines forces the band-fill test only on its
    prominence (>= 4), no longer unconditionally; a regular comb (a TV line
    comb) still forces it. A real trunked-radio site (own heimdall capture,
    30-38 carriers / spurs over 2.25 MHz, prominence 1.4-2.5) had become one
    band-filling "DVB-T2" track that swallowed its P25 channels; synthetic
    OFDM combs that need the forced path have prominence 9-16
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
  Long views (2-3) are analysed in 8 chunks of 4096 (was 16), quantiles of
  the whole-view box powers on <= 16384 of them. `sigclass_tool layout`
  prints each group's meaning; every value finite and O(1) (clamped); a
  model of another hash is refused. `SIGCLASS_DEBUG_SYM=1` prints the two
  lines per detection
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
  for the service. Class evidence: EWMA (0.3) of log-probabilities per view,
  the track's view = the one most of its detections had
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
  30 % only); p25p1 48 clips (24 / 12 / 12), nxdn 32 (12 NXDN48), dmr 32,
  dstar 24, tetra 24, vor 24; LTE / NR half lightly loaded (0.02-0.2), 40 /
  32 clips; LoRa: 2 of 5 clips sparse (24 s, a packet every 1.5-8 s). Limits:
  sfn_gen has no `--seed`; dab_gen's audio is a tone per service; analogtv
  one test card; tetra_gen's payload is the empty SCH/F every frame and its
  output does not decode for 18 of 64 colour codes (2, 3, 5, 8, 10, 17-20,
  27, 28, 34, 38-40, 46, 52, 54: the bank uses the others); radiosondes are
  RS41 and M20 only (no generator for DFM / iMet / LMS6 / M10 / MRZ / MTS01:
  left to unknown FSK)
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
  spurs (4 % a comb every 100-300 kHz), IQ imbalance, DC, 8-bit quantisation
  after the noise, gain pulled down near clipping (6 % let clip)
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

### Results (2026-11-20, phase 3, the shipped `sigclass.model`)

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
  RTL-SDRs (one channel too) - the tuners in turn (round robin, tuners at 0
  Hz skipped) at `exact_tuner_hz(ch)`. The centre is the exact request when
  the snapshot's header frequency (float32, ~64 Hz exact near 1 GHz) is
  within 2 kHz of it, else the header's. Labels are absolute Hz, so they land
  on the stitched wideband-scan spectrum and on each pane by themselves
- **CPU budget**: `CPU_SHARE` 4 % - the cycle period (capture + analysis +
  rest) is the analysis time (exponential mean, weight 0.3) / 0.04, clamped
  to `MIN_PERIOD_S` 1 .. `MAX_PERIOD_S` 10 s. The state carries
  `analyze_ms`, `period_ms` and `duty`. No thread priority change: the code
  base has no portable per-thread helper (`kproc::lower_own_priority` lowers
  the whole process on Windows), so the budget is the only limit
- **Trackers**: one per tuner (`TunerTracks`; coherent mode uses [0] only),
  `TRACK_EXPIRE_S` 45 s (a pager or a repeater between overs keeps its
  label). A track's age counts against the looks at ITS band: each tracker
  has its own clock, which moves only with a snapshot of its tuner - by the
  wall time since its previous one, at most `EXPIRE_STEP_S` (half the
  expiry + 10 ms). So a track goes once it was missed for 45 s AND in at
  least 2 snapshots of its tuner. Before (one tracker on the wall clock), 8
  tuners in turn at a 10 s period (analysis >= 225 ms; 5 tuners: >= 360 ms
  - inside the Pi 5 estimate) revisited a tuner every 80 s and 3 of 8
  tuners had no labels at any time. In coherent mode (one tuner; every
  snapshot is a look) the clock also runs between snapshots (no data,
  retunes): there it is the wall clock. Track ids sent = the tracker's id x
  8 + the tuner. Cleared on an operating-mode change, on disable, and when
  parked for the noise source; kept over a retune (absolute Hz: the ones
  still in band go on). Only the tracks inside the band their tuner receives
  now are sent (coherent: the centre +- 1.2 MHz): after a retune - or a
  switch between playback entries - the old band's tracks leave the list at
  once, but stay in the tracker (and come back if the receiver returns
  before they expire). Two overlapping tuners (wideband scan) seeing one
  signal: sent once (the fresher / more often seen track). Classes marked
  `"hidden": true` (`none`: a false detection) are never sent, nor offered
  as an alternative
- **Ages**: `age` is 0 while the track's tuner's latest snapshot had it and
  that snapshot is not overdue (a round of the tuners + 5 s) - with tuners
  in turn a round can take 80 s, and the page's fade needs no knowledge of
  it; otherwise the wall time since it was seen (no data, retunes: the
  labels fade). The CPU budget is spent per snapshot (the 1 s floor applies
  per snapshot, so N tuners take N periods a round) - not changed
- **Parked** (no captures, the reason in `parked`): "calibrating (noise
  source on)" - `doa_is_calibrating()` or a noise packet seen by the tap
  within `NOISE_HOLD_MS` 1.5 s (tracks cleared: the noise source would be a
  band-filling "signal"); "settling after a retune" (500 ms, tracks kept);
  "no data from the receiver". Calibration state (phase states, KerberosSDR
  stale) doesn't matter otherwise: one antenna's samples need no array
  calibration
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
  duty, snapshots, min_confidence, tracks:[{id, hz, bw, lo, hi, tuner, snr,
  duty, burst, wide, cls, name, family, p, alt:[[cls, p, name], ...], plugin,
  demod, vbw, age, hits, unknown}]}}` on the control topic (`WebSocketServer::broadcast_json_message`):
  on enable / disable at once; after each analysis and on a park change when
  the track list's signature (ids, classes, p in 2 % steps, hz in 200 Hz
  steps, bw on a log scale, the state) changed, at most every 500 ms
  (`SEND_MIN_MS`; a held change goes out from the worker's next wake-up),
  else every 5 s (`SEND_REFRESH_MS`: ages, analysis numbers). A page that
  connects gets the current state in the connect sync
  (`get_connect_sync_messages`, ages computed then). Tracks: sorted by
  frequency, at most 200 (the most recently seen); none while off. `cls`,
  `name`, `family`, `plugin`, `demod` are null without one; `vbw` = the
  class's `bw_hz` (the VFO width a click sets; null = none); `hz` = the
  track's smoothed centre (the tune target), `lo` / `hi` = the band its
  detections cover (the core's band edges when its `Track` has `lo_hz` /
  `hi_hz`, else hz +- bw / 2), `tuner` = its tuner; `wide` = the TRACK's
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
  `kraken_sigclass` {spec, min: 0..1 or 'auto'}: per browser)
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
  (`--t6`). Subdued: p < the model's `min_confidence` (`sigSure()`; 0.7
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
  `.sb-note`, "On spectrum" and "Confidence" (Auto (the model's level,
  shown), All, 25 .. 90 %, default 50 %; both per browser), the list
  (`#sig-list`: name, MHz to the kHz, p - sorted by frequency, the same
  filter as the labels, subdued / old rows greyed; a click tunes like a
  label; rows are patched in place keyed by track id - rebuilding them with
  `innerHTML` on every state lost a click whose press and release
  straddled a message), the count in the header, a `?` tip
- New icon `tag` (*icons.md*)

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
