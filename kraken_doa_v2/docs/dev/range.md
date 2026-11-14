# Range from signal strength, and each VFO's level on mouseover

Written for the docs/dev layout when the feature was merged (2026-11-15);
the text was a section of `kraken_doa_v2/CLAUDE.md` on its branch. Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`).

**📏 Range from signal strength (🗺 Map, coherent mode; kraken_doa.html "📏 RANGE", `vfo_level.cpp` + `rdf_mapper.cpp` lvl, `FFTProcessor::band_power`) + each VFO's level on mouseover (every mode):**
- The user's request (2026-10-20): from ONE DoA lobe + the received level +
  an estimate of the transmitter's EIRP and our antenna gain, a heat map of
  where the transmitter probably is - with the terrain through ITU-R P.1812
  as the 📶 path loss does. "Range" in the 📡 DF panel's header
  (`#map-df-rng`, `rgToggle`) -> the panel `#map-rg` (in `.map-tl`, the
  viewshed panels' look: `.map-vs.map-pl.map-rg`) + its layer (`rgDraw`,
  under the DF lobes). The source = the DF panel's selection (`rgSource`):
  one radio (a talker), else the selected VFO (all: the sidebar's selected
  one with a lobe, else the first with a lobe and a level)
- THE LEVEL (server): `src/vfo_level.cpp` (`vlvl::`, its own thread, every
  `SAMPLE_MS` 100 ms, EVERY VFO in EVERY operating mode - until 2026-10-24
  it was rdf_mapper's, coherent + DoA only; the hover readout below needs it
  everywhere), not while `doa_is_calibrating`: its squelch open (or off) ->
  `FFTProcessor::band_power(its tuner - coherent: active_channel, independent
  / wideband scan: its tuner_channel -, offset, the VFO's bandwidth, NO DC
  bins out)` (as the FFT squelch: the DC offset is tracked out before the
  FFT - `convert_uint8_to_complex_float_tracked_dc` - and with +-2 bins
  left out, as until the review of 2026-11-12, a carrier tuned onto the
  tuner centre - a CW beacon, AM, a saved-frequency tune - was dropped from
  its own level: a 1 kHz VFO read 9-11 dB low or got none), with that
  tuner's gain (`ChannelManager::get_gain`) = the
  band's bins of the tuner's LATEST
  RAW frame (`fft_magnitudes`, not the dB-averaged spectrum - a log average
  reads noise-like signals 2.5 dB low) summed in power, by Parseval / the
  Hamming window's mean w^2 0.3974 (+4.01 dB) = absolute: 1 = a full-scale
  complex tone ((u - 127.5) / 127.5) = 0 dBFS; minus the band's noise
  (`nfloor::bin_noise_db`: the raw frame's 10th percentile + 10 log10(fft
  size) as nfloor keeps it, -10 log10(N) + 9.773 dB - an exponential's mean
  over its 10th percentile -, or the remembered receiver floor while a
  signal fills the band; wideband scan mode, where nfloor compares the
  tuners instead: the frame's own percentile less what the tuner's own
  floor stands above the one it is measured from - `noise_r` in
  nfloor::on_frame, both branches). A moment counts while the signal is above the
  band's noise (TDMA's empty slots, squelch-less silence don't pull it
  down): `bp.sig > bp.thresh` = BOTH 2x the band's noise (a few dB of
  ripple across the tuner band) AND the noise's own upper quantile for the
  band's bins at ~1e-5 per sample (`noise_quantile` in fft_processor.cpp).
  Plain "sig > noise" (until 2026-11-12) isn't a test for a few bins: a 1
  kHz VFO (7 bins at FFT 16384, ONE at 1024) on pure noise passed 3.5-14 %
  of samples - a silent VFO got a level. The quantile: the n bins' sum as
  a Gamma of shape n_eff = n^2 / Var(sum), Var from the Hamming window's
  own bin-to-bin power correlation (0.39 / 0.018 at 1 / 2 bins; zero
  padding - `channel_frame_samples` - spreads it further), by
  Wilson-Hilferty (on the safe side for shapes 1-200, checked against the
  exact tail) with Z for 4e-6 (the correlated sum's tail measured ~2x the
  moment-matched Gamma's) and the noise estimate's own error folded in
  (nfloor's percentile: 11 % at FFT 1024, 2.8 % at 16384). Measured, noise
  only (scratch t_bp2.cpp, 0.2-9.6 M samples per case): <= 1.1e-5 at FFT
  1024 / 16384 / 65536 zero-padded x4, 1 / 3 / 12.5 kHz, at and off the
  centre (before: up to 1.4e-1). THE COST: a narrow VFO needs more SNR for
  a moment - 1 kHz at FFT 16384 passes 99.9 % at +10 dB band SNR, 46 % at
  +6 (those reading +1.2 dB high: only the moments the noise added to
  pass), 5 % at +3 (+3.6 dB); 12.5 kHz as before (99.9 % at +3). The range
  panel warns "weak" below 10 dB. No consecutive-sample rule (short bursts
  keep their level). Averaged in power (`vlvl::Level::add`: the first ones'
  mean, then EMA 0.2 ~ 0.5 s; a gap > 3 s or another gain starts afresh;
  held after, its age sent; reset on a retune > 10 kHz). `vlvl::level()` =
  the average, `vlvl::moment()` = the latest moment with a growing `seq`,
  its VFO stream position (`pos`, DecimatorInstance::stream_pos) and rate.
  rdf_mapper takes a VFO's level from it; A TALKER's level = the VFO's
  moments that lie INSIDE ITS OWN TRANSMISSION by stream position, as its
  MUSIC frames are matched (`TalkerDoa::talker_at(pos -+ LVL_GUARD_S 0.15
  s, settle LVL_SETTLE_S 0.5 s)` on the spans: 1 = one named span holds
  it, none other touches it, closed or already reported 0.5 s past it - P25
  extends the old radio's span over a new radio's first frames until its
  LDU1 names it; 0 = wait - no span yet / "?" / a slot's channel hold; -1 =
  two spans or across a span's edge: dropped). Each new moment waits in the
  VFO's `lvl_wait` (<= FRAME_WAIT_S 6 s). Until 2026-11-12 the moment went
  to the one talker flagged `active` at that tick - `active` lags the air
  by the decoder's latency and stays up to ACTIVE_MS: a radio keying up
  right after another was averaged into the other's level. Now a talker's
  level comes ~0.65 s + the decoder's latency late and leaves out 0.15 s at
  each end of a transmission; not measured live (reasoned against
  talker_doa.cpp / p25.cpp). Packet talkers - aircraft, ships - and spatial
  ones have no spans: no level. Sent as `lvl: {dbfs (the signal without the noise),
  noise (the band's), total (the whole tuner band: ADC load - summed by the
  FFT loop itself - `bins_to_db` returns its bins' summed power ->
  `channel_frame_power` -, so band_power only reads the band's bins under
  fft_mutex), gain (dB, -1 AGC), age (s), n}`
  (`vlvl::write_json`) in `{"rdf"}` vfos / talkers and in system_status
  `decimator_squelch[]` (every page, 2 Hz)
  Checked (scratch t_bandpower.cpp, the real `process_channel_fft` + pffft +
  nfloor with stubs, MinGW): a bin-centred tone -30 dBFS -> -29.96, between
  bins -50 -> -50.29, NBFM -45 (3 kHz deviation) -> -44.86, an 8 kHz
  64-tone signal -55 -> -54.69, a tone at SNR -2 dB in the band -62 ->
  -61.91; the band's noise within 0.15 dB of the truth in all, the whole
  frame (signal + noise) within 0.1 dB
- THE HOVER READOUT (the user's request 2026-10-24, every mode): the mouse
  over a VFO's bar - main spectrum (`handleMove` -> `vfoLvlHover(e,
  $id('fo'))`, `c.boxes`), waterfall (`handleWaterfallMove`, the overlay's
  boxes), independent panes (`paneMove`, `paneHit` - not on a squelch line
  or a saved-frequency tag; not while dragging, nor over an AI / saved
  frequency marker on the spectrum) - shows beside the pointer
  (`.vfo-lvl-tip`, `vfoLvlRender`, kept on screen): "D0 · MHz · bandwidth",
  dBFS · SNR (the VFO's band: signal over the band's noise) · dBm (dBFS -
  gain + the 📏 range panel's calibration `rgState.cal`), the gain and
  whether calibrated (± sigC), ⚠ near full scale (total > -8 dBFS), "last
  heard N s ago" (age > 2 s), "the tuner AGC is on" (no dBm), "no level
  yet" before a moment. `vfoLvl` from system_status (`vfoLvlNote` in
  updateSystemStatus); updated live while shown. Checked (scratch
  ui_hover.mjs, headless Chrome, real mouse moves over the bar drawn from a
  fake VFO list + spectrum frame): no level yet, live update, the
  calibration applied, AGC / full scale / age, the status handler, the
  waterfall bar, hidden off the bar / away, no exceptions
- dBm (page, the range panel AND the hover readout): dBFS - the tuner gain
  (the moment's, from the server) + C = `rgCal(gain, MHz)` (globals block):
  the NOMINAL model + the user's correction `rgState.calOff`. Not a
  noise-floor (kTB + NF) conversion: the dev PC's noise_floors.json (49.6
  dB -46.7, 25.4 dB -56.5, 20.7 dB -58.7) drops ~10 dB for 24 dB less gain
  - below the top gains the floor is the IF / ADC noise, not the antenna's.
  THE NOMINAL (`rgCalNom`, `RG_CAL_G` x `RG_CAL_F`, sigma `rgCalNomSd`;
  the user's question 2026-10-24 - until then a single -23 dB +- 6 guessed
  from that floor as kTB + a 5 dB noise figure, ~7 dB too low: it put 0
  dBFS at -72.6 dBm at 49.6 dB, the measured overload is -64 to -70 dBm):
  published R820T / R820T2 level measurements (stock librtlsdr's manual
  gain = our fixed IF VGA step 8), converted to our dBFS (0 = a full-scale
  complex tone; an overload / clip point = 0 dBFS) - research in job
  3cc78a36:
  | source | f MHz | gain dB | -> C |
  |---|---|---|---|
  | WPI MQP 2016 Table 6 (6 dongles, Agilent N5181A; 20.4-26.9 GNU Radio magnitude / V rms) | 895 | 0 | -13.2 ... -15.6, mean -14.7 |
  | N8UR, osmocom-sdr 2018-03 (HP 8642A + power meter; first spur = overload) | 144 | 0 / 12.5 / 22.9 / 37.2 / 49.6 | -15 / -20.5 / -21.1 / -19.8 / -17.4 |
  | HB9AJG 2013 (R820T, SDR#, overload) | 145 / 435 | max | -14.4 / -20.4 |
  | J. Adams 2016 (NooElec R820T2, -1 dB saturation ~-65 dBm) | - | 49.6 | -15.4 |
  | J. Clark 2023 (Signal Hound-checked levels, SDR# dBFS) | 915 | 0 / 19.7 | -15...-18 / -18.3, -19.3 |
  | Simon 2014 (gain table check, relative): 43.9 and 44.5 dB "identical", mid steps +1...+3.5 dB over nominal | 141 / 1000 | all | shape |
  -> C by the gain setting [0 -14.5, 4 -17, 12.5 -19, 23 -19.5, 37 -19,
  49.6 -16] (the table reads high mid-range) + the gain's frequency
  roll-off [24 MHz +2, 50-900 0, 1000 +1, 1200 +3, 1400 +6, 1500 +9.5, 1600
  +12, 1766 +15] (WPI's 6-unit sweep, Simon's max-gain sweep, a "10 dB
  down at 1.5 GHz vs 300 MHz" report); sigma 3 dB (unit to unit ~1.2, the
  sources' spread) + 0 ... 6 more toward the band's ends. Against the 14
  points: mean -0.9, rms 1.8, worst -4.4 dB (HB9AJG 435 MHz), all within 2
  sigma (tools/range_validate.mjs 7). With C -16 the dev PC's floor at 49.6
  dB means a system noise figure ~11 dB (published at max gain: 3.5 - 13;
  indoors man-made noise adds). The KrakenSDR's own front end (noise-source
  switch, bias tee) isn't characterised: +0 ... +1 dB more loss likely. The
  AGC's -1: the top setting's C
  A CALIBRATION is a correction on the model (`calOff`, set at `calG` dB /
  `calF` MHz; `sigC` its own +-): "Known level…" (`rgCalLevel`: the
  selected signal arrives with X dBm -> calOff = X - (dbfs - gain) -
  rgCalNom(gain, f), +- 1.5 dB), "Known transmitter…" (right-click its
  place -> "📏 Known transmitter here" `rgMenuItem` / `rgCalTxAt`,
  `rgState.calTx` {lat, lon, h, eirp W}; `rgCalTx` = a LINK job on the range
  worker from it to the station (our antenna height), the model's loss +
  P.2108 -> the level expected = EIRP - L + G - cable -> calOff; +-
  sqrt(sigma_L^2 + model^2), the prediction's own spread), or typed (the
  "correction" field; "Nominal" = 0). `rgCalSd`: calibrated = sigC, away
  from where it was set (gain > 3 dB or frequency > 15 % off) + the
  model's shape (1.5 dB and the frequency terms' difference, in squares).
  The panel's calibration row: "dBm = dBFS - gain + C, C = x dB +- y
  (nominal z at g dB, f MHz, + correction)"; the ± field only once
  calibrated; far (> 1.4x) from calF: a note. The tuner AGC (gain -1): no
  dBm, said
- THE SWEEP: the viewshed worker's signal sweep (`vsWorkerMain` -> runJob /
  runSignal / plSweep, UNCHANGED) on an instance of its own (`rgWorker`;
  also its calibration links) with the STATION as the worker's "Tx" end
  (path loss is reciprocal; each end's clutter by its role: `envTx` of the
  message = OUR surroundings, `envRx` = the transmitter's - checked for the
  link both ways in pathloss_validate.mjs) and a transmitter at the TX
  antenna height (`rgState.txH`, above ground) in every cell; radius
  `rgState.r` (default 30, <= 300 km), `vsPlan` rings, k 4/3, helpers as
  the viewshed's (`vsPar`). The job (`rgSweepMsg`): its ORIGIN (the
  station rounded to 1e-4 deg) + the settings (`rgSKey`: our antenna
  height - `rgStH`: the setting, else the viewshed station's, else 5 m -,
  the radius, f to 3 significant digits (0.3 %: < 0.03 dB), the model,
  polarization, both clutters, N0 / ITM's climate + ground - the 📶 Signal
  settings'). One job at a time, only while the map is shown
  (`rgMapShown`); a settings change cancels it at once. THE ORIGIN FOLLOWS
  THE STATION WITH HYSTERESIS (`rgFits`, the review of 2026-11-12: the key
  held the live rounded position and any change cancelled the sweep -
  driving at 10 m/s it changed every second while a sweep takes 6-20 s, so
  no estimate ever came, and parked GPS jitter threw results away): a
  static location exactly and at once (cancelling); a GPS / compass
  station's only once it is beyond `rgTolM()` = max(`RG_ORG_M` 30 m,
  `RG_ORG_FR` 0.5 % of the radius), at most every `RG_ORG_MS` 30 s from the
  last sweep's start (as `vsFollowStation`), and a running sweep is never
  cancelled for it; estimates keep coming from the last sweep while the
  station is within `RG_ORG_USE` 10 x that of its origin, the panel saying
  "↻ worked out N m back - updating…" (not for a held estimate). The
  viewshed's signal mode is untouched (the
  station stays out of it - the user's call 2026-10-12 was about coverage)
- THE POSTERIOR (a worker of its own, `rgPostMain` - a few million cells; the
  page stays responsive): `init` = the sweep's rings (copies of loss / fad)
  -> each cell's bearing (65536ths) + distance from the station (great
  circle: atan2(|east, north parts|, dot) - the bearing formula's parts
  have the length sin of the angle), each row's cell area. `post` per cell (no data 65534 / 65535
  skipped): density = the bearing factor at its bearing x the level's pdf at
  the margin m = base - loss (base = EIRP + G_rx - cable - P_measured) ->
  mass = density x area (the prior: uniform per area within the radius)
  - bearing factor (`rgAng`, page): the north-frame lobe (rdf u8) blurred
    by the bearing uncertainty (auto: hypot(2, the heading's: compass 4,
    else 1.5 + 8 / speed - static 5.5) as rdf_mapper), normalised,
    (1 - eps) p 720 + eps, eps 0.05, NOT tempered (`RG_ALPHA` 1: mobile
    DF's 0.5 discounts many correlated records; on one lobe its square root
    left ~half of the probability off the bearing - the 90 % region 43.8 ->
    14.5 km² in the Auckland test); cells within 30 m: 1 (no bearing)
  - the level's pdf (`rgLik`): the slope of the 📶 path loss's reliability
    table (`plRelLut`: P(level >= threshold) at a median margin) - rel'(m) =
    the pdf of the measured dB given the cell's median: lognormal sigma =
    hypot(sigma_L (P.1546 for the TRANSMITTER's end: 0.5 / 1.0 / 1.2 + 1.3
    log f by its clutter + height, `rgSigmaL`), model error `sigM` (4), the
    EIRP's `sigE`, the calibration's `sigC`) with the fading around it:
    auto = Rician whose K follows the path per cell - the 📶 path loss's
    OWN helpers with this panel's settings: `plRelSet(sigma, rgState)`,
    `plKeff / plKeffQ(.., K)`, `plAutoFade / plFading(.., s)` take the
    settings as a parameter defaulting to plState (the range's copies -
    rgKeffQ, rgLik's table code, rgKm = vsFmtM - were removed 2026-11-12;
    plRelSet's idx cache is per K, `plKidxs`, so the two K don't evict each
    other; the path loss's tables bit-identical before / after, scratch
    pl_baseline.mjs, 8 settings) -, or
    Rician / Rayleigh / none (a moving transmitter averaged it out). Sent as
    nk x 481 values every 0.25 dB from -50 dB; idx (65536) once per K;
    `rgLik` cached by sigma / fade / K / model
  - the HIGHEST-DENSITY REGIONS: the mass by density (0.01 decade bins over
    12 decades) summed from the top -> each cell's c = the share of the
    probability in places at least as likely -> u8 1 + c / 0.99 x 254 (0 =
    beyond 99 % / no data) -> `rgLut` colours (`RG_STOPS`: red at the most
    likely, orange, yellow 50 %, green, cyan 90 %, blue 99 %; alpha 235 ->
    100), painted per ring (`rgPaint`), composited coarse -> fine at 0.75
    (`rgDraw` via `plDrawLevels`)
  - the numbers: the distance's quantiles 5 / 25 / 50 / 75 / 95 % (2000
    bins over 1.05 x the radius), the 50 / 90 % regions' areas, the share at
    the edge (> 0.9 radius), the margins' range (mMin / mMax: no place fits
    when even the strongest place's median is 3 sigma below the level, or
    the weakest 3 sigma above - said; none within the tables -> "empty"),
    the MOST LIKELY SPOT (the ✕) = the MODE taken INSIDE the core - the
    cells holding the most likely `CORE_P` 1 % of the probability (at least
    the top density bin) - as the one whose density averaged over its 3 x 3
    neighbours (its ring's, with data) is highest: always on the deepest red
    (st.c <= st.core), one odd cell (a ridge top with just the loss) can't
    take it. Until 2026-10-22 it was the centre of the probability in the
    most probable block of a 128 x 128 grid over the radius - a MEAN, not a
    mode: a block of much moderately likely ground outweighed a smaller,
    stronger core, and a block's centre fell between bright cells (a lobe
    crossing it diagonally, bright cells either side of a shadow): the ✕
    sat outside the core (the user's report; 55 deg on a 60 deg lobe in the
    Auckland test, 7.93 km beyond its own 95 % distance in the synthetic
    one). ms
  - the readout leads with the bearing and the distance's quantiles: with
    sigma ~6-9 dB the probability spreads along the lobe (free space: a
    factor of 2 in distance per sigma). The ✕ is the most likely spot PER
    AREA - it can be much nearer than the median distance (Auckland test:
    703 m vs 4.2 km): the lobe widens with distance, so further out more
    ground shares the probability, each place a little less likely, together
    more. Said in the ✕ line's tooltip and the panel's note
- Live: `rgOnStatus` (from rdfOnStatus, 2 Hz) -> `rgPump` (the sweep's key)
  + `rgPostPump`: one post at a time; a setting / the source changed: after
  0.3 s, the live lobe / level only every `RG_LIVE_MS` 2.5 s (each post is
  ~0.1 s on the dev PC at 30 km, 4 rings - several times that in a Pi's
  browser); not while the map isn't shown (`rpApply` draws the map when it
  shows again, so the sweep starts then). `rgPostPump` works out its key
  from cheap inputs first - `rgAng` / `rgLik` only for a post that goes out
  (rgDraw pumps on every map draw). The snapshot (`rgSnap`): the
  source's lobe + level + where the station was - LIVE only while the level
  is fresh (its age now < `RG_FRESH_S` 2.75 s; the server starts a new
  average after 3 s): with the squelch off MUSIC keeps publishing lobes on
  noise after the transmitter stopped, and estimates from a random bearing +
  the old level made the map jump. The lobe gone or the level old -> the
  last fresh one HELD (said, with the level's age) until Clear (`rgClear`)
  or a new one; never a fresh level: "No level yet…" / "No recent level…".
  The DF selection moved to another source (`rgSrcSet` -> `rgDrop`): the
  held snapshot, the estimate and the painted result go (the sweep stays),
  and the post in flight is ignored when it comes (`rgPostReq = null`; it
  only releases rgPostBusy) - Clear too, which a reply in flight used to
  undo. Hover (`rgHover` from vsHover): the region %, distance / bearing,
  the cell's loss and the median level a transmitter there would give vs
  the measured. Map: the radius dashed (faint once painted), ✕ "TX? x km"
  at the spot, the known transmitter (yellow square) while calibrating
- Panel (`rgPanelBuild`, `rgRender` - from mapDrawNow / mapUpdateControls /
  rgOnStatus, innerHTML only when changed): the source line (dBFS at gain
  -> dBm, SNR, held), the sweep's status, the readout + warnings (edge > 5
  %, SNR < 10 dB: may read high, total > -8 dBFS: near full scale - lower the
  gain, uncalibrated, held, no place fits), legend, ◎ / Clear, hover, then
  the fold "Transmitter, our antenna, calibration, model" (`rgState.cfg`,
  open the first time: the map is narrow - the readout first): power +
  antenna - cable or EIRP (W) + presets (`RG_PRESETS`: handheld 5 W -3
  dBi 1.5 m, PMR446 0.5 W ERP, vehicle 25 W, base / repeater 50 W 6 dBi 25
  m, ISM 10 mW) +- dB, TX antenna height + clutter, our antenna gain /
  cable / height / clutter, calibration + the two helpers, model +
  polarization (both antennas: the models' h / v), radius, and the
  sub-fold "Uncertainty, fading" (sigma_L auto, model +-, bearing sigma
  auto, fading + K, the total sigma). `rgSet` validates, `rgChanged`
  re-pumps. `rgState` in the globals block (localStorage `kraken_rg`,
  rebuilt field by field), `rgReady` set at the section's end (the map can
  draw before it)
- Limits: one antenna's level (the FFT channel; no array gain), the bare
  terrain + P.2108 clutter (buildings / trees otherwise not in the data),
  the median models (no ducting), the EIRP is a guess - its +- widens the
  range; the uncalibrated level +- 3 dB (VHF / UHF; up to ~6.7 at the
  band's top end); a packet talker has no level. THE DOWNCONVERTER VARIANT
  (`wbVariant.enabled`, the review of 2026-11-12): the tuner sits at the IF
  whatever the RF, so the nominal's frequency term is the IF's
  (`rgTunerMhz`; at 1268 MHz +4.0 dB: C -12.0 at 49.6 dB - it was the RF's:
  ~4 dB off at 400 MHz, ~10 at 3 GHz), and the mixer's conversion loss +
  the front end before it aren't known: `RG_CAL_DC_SD` 6 dB more in squares
  (+- 7.1), said in the hover, the calibration row and the warnings
  ("downconverter: its conversion loss isn't in the nominal - calibrate");
  `rgCalSd`'s "away" test keeps the RF (the conversion loss changes with it)
- Tests: committed `tools/range_validate.mjs` (node 20+, offline - the
  synthetic terrain of pathloss_validate.mjs through fetch, the page's own
  functions + both workers): the pdf (no fading = the normal pdf to 2e-5;
  Rayleigh 2.51 dB; auto tables integrate to 1), the bearing factor (peak,
  floor eps, peak / floor 548), free space with an exact level (the spot
  4.03 km at 59.9 deg for 4 km at 60 deg, in the core), the ✕ inside the
  core in all 40 random estimates over the hills (lobes 2 deg off, levels
  from the model; all on the lobe), the distance's
  quantiles = an independent posterior in plain loops (worst 2 m), a level
  100 dB too strong (no place fits) / 200 dB too weak (empty), the
  CALIBRATION by Monte Carlo (P.1812 over the synthetic hills, 446 MHz,
  sigma 6 dB, a flat lobe, 150 transmitters uniform over the area, levels
  drawn from the model): the true place in the 50 % region 50 %, in the 90
  % region 91 % of the time; auto fading runs; the station's sweep (clutter
  roles swapped) = links from 25 transmitter cells (1.5 m, urban: up to
  14.8 dB of P.2108) to the station: median 0.03, p90 0.08 dB. Scratch (job 3cc78a36):
  t_bandpower.cpp (above), ui_test.mjs - headless Chrome over the DevTools
  protocol on the page served statically, a fake {"rdf"} (Auckland, 446
  MHz, a lobe at 60 deg, -22.4 dBFS at 49.6 dB), real terrain: the Range
  button, the panel, the sweep (P.1812 30 km, 6.4 s) + estimate (108 ms),
  heat colours along the lobe, the folds (saved), hover, a known level (-80
  dBm -> cal -8), a known transmitter (5 km, 100 W, 40 m: P.1812 137.0 dB
  -> correction +7 on the nominal, C -9.0, +- 6.1; a known level of -80
  dBm -> correction +15 on the nominal -16), the calibration row (C = -16.0
  +- 3.0, nominal at 49.6 dB / 446 MHz), the ± growing away from the
  calibration's gain / frequency, the menu item, AGC, held, off, no
  exceptions. The review's fixes (2026-11-12, two Opus workers; scratch
  ui_fix.mjs on harness.mjs, 23 checks in headless Chrome - counts by
  DevTools breakpoints): a GPS station stepping 15 m per push = one sweep,
  never cancelled, the estimate after 7 s "worked out 195 m back", the next
  sweep 30 s after the first, 9 estimates between, parked +-12 m jitter no
  new sweep, a static move a new sweep at once (a second one cancels);
  lobes turning while the level ages 3-10.5 s = 0 posts, "held", a fresh
  level resumes; Clear with a post in flight stays cleared; another source
  selected = nothing shown, back = its estimate again; 30 map draws + 4
  identical pushes = 0 rgLik / rgAng / posts, a moved lobe exactly 1; no
  sweep behind the MUSIC DoA tab, one when the map shows; the downconverter
  nominal at RF 400 / 3000 MHz (C -12.0 +- 7.1, said) in the panel and the
  hover; range_validate.mjs 7 (the variant: the IF's term at any RF);
  pl_baseline.mjs (the path loss's tables unchanged); t_bp2.cpp (the level:
  the centre carrier, the noise-only false rates, the sensitivity).
  `tools/range_validate.mjs` extracts the shared pl* helpers now
