# Path loss (the viewshed's Signal mode)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📶 Path loss = the viewshed's 📶 Signal mode (UI only, kraken_doa.html "📶 PATH LOSS"; the engine lives in the viewshed worker):**
- ONE feature with the viewshed (the user's call, after a separate "Path
  loss" mode that was the viewshed OR path loss): the viewshed panel's mode
  switch `vsState.mode` = 'los' (👁 Line of sight: the plain viewshed,
  unchanged) | 'sig' (📶 Signal: how often (%) the signal between each site
  and a radio at the target height is good enough - also WITHOUT line of
  sight, coloured by that reliability, NOT restricted to the cells in
  sight; the edge of the line of sight as a black line, switchable). Each site has
  a ROLE: TX or RX (the station's RX, other sites as added). The STATION
  ISN'T PART OF THE SIGNAL MODE (the user's call 2026-10-12): its row is
  hidden there (`.vs-it.vs-st`), `vsJobItems` / `vsLinkItem` / `plEnds` /
  `plPickSite` / `plDraw`'s tags skip it, a link needs a TX antenna
  (none: no path loss menu item / buttons, `plLinkTo` flashes). TX
  antennas come only from the right-click menu ("📶 Add TX antenna here",
  `vsAddAt('tx')`, named "TX n"; markers too) - no RX adding, no TX / RX
  tag to switch a role (the user's call 2026-10-13; `vsSetRole` is left
  for old RX sites / tests). ONLY
  TRANSMITTERS ARE COLOURED (the user's call, 2026-10-12: an RX site used
  to get its own "where would a transmitter be heard" map - reciprocal, the
  same numbers, but centred on the receiver; with its radius smaller than
  the link it showed green next to a red link, and "best per place" mixed
  TX coverage with RX hearing areas): TX sites (receivers at the target
  height), and the link's TX place when the link's site receives -
  `vsLinkItem()`, id `VS_LINK_ID` '_link', not in vsState.items: around the
  place at the target height (above sea level too: the worker's `m.amsl`),
  receivers at the RX site's antenna height (`vsTgtOf`), radius max(the RX
  site's, 1.25 x the distance + 1 km), worked out when the place is
  DROPPED (vsPump skips it while mapObjDrag is 'pl'), dropped when the link
  goes / its site transmits (`vsLinkSync`). `vsJobItems()` = what is worked
  out / coloured / hovered (LOS mode: every site). An RX site in signal
  mode (old saved ones): no job, no radius circle, its row says "an RX
  antenna - not coloured ..." or
  its link's TX status. Every shown transmitter at once, the BEST per
  place. The LINK is a tool on top: a site <-> a place
  (at the target height), its profile + every model + the budget. The
  other end can be in the air (target above sea level: aircraft, balloons,
  drones - the aircraft is often the TX for a KrakenSDR). The user's
  reference (github.com/mariohm1311/DroneRanger, no licence: ideas only)
  has free space + a line-of-sight check only; the NLOS physics comes from
  the ITU-R Recommendations and NTIA's ITM
- Engine (vsWorkerMain; `self.vsTest` exports it for node):
  - ITM / Longley-Rice: `itmP2P(pfl, htx, hrx, {climate, N0, f, pol, eps,
    sigma, mdvar, time, loc, sit})` = NTIA ITM 1.3 (C++, = ITM 1.2.2),
    point-to-point mode, ported function by function (FindHorizons ->
    itmHorizons, ...; the area mode left out). NTIA's notice is in the code
    (not copyrighted in the US; derivative works allowed with a note of the
    changes + an acknowledgement). Checked: NTIA's 5 published cases
    (p2p.csv / pfls.csv: LOS, diffraction, troposcatter) to their 0.01 dB,
    and 3000 random profiles / parameters against the compiled NTIA C++:
    worst relative difference 4.5e-10, the same mode and warning bits
  - ITU-R P.1812-6: `p1812P2P(d km, h m, R clutter m|null, htg, hrg, f GHz,
    ae km, omega sea fraction, N0, pol 0 h / 1 v, flag4)` - Bullington
    (`p1812Bull`, 4.3.1), the smooth-path Bullington of Attachment 4
    (`p1812BullSmooth`, flag4), spherical Earth (`p1812Se` 4.3.2, first term
    `p1812SeFtIn` 4.3.3 land / sea mixed by omega), smooth-Earth heights +
    horizons (`p1812Heights`, eq 85-95 with the obstruction correction 89-91),
    troposcatter (`p1812Tropo`), at p = 50 %: Lb = max(Lbfs, -5 log(10^-0.2
    Lbs + 10^-0.2 (Lbfs + Ld))). No ducting / layer reflection (they matter
    only for small time percentages), no location variability (the page's).
    Checked against the 63 ITU-R SG3 validation logs in Py1812's tests:
    every intermediate (Lbfs, Bullington, spherical, Ld50, Ldb, horizon
    angles / distances, hstd / hsrd, Lbs, Lb) within 5e-8 dB. NB Py1812's
    dl_p logs Lbulla / Lbulls / Ldsph of its LAST run, the beta0 radius 3 x
    6371 km - not the median one
  - ITU-R P.2108: `p2108Terminal(fGHz, h, type)` (3.1, 30 MHz - 3 GHz: 1
    water / 2 open: height gain; 3 suburban / 4 urban, trees / 5 dense urban:
    diffraction over the clutter, street 27 m; R 10 / 10 / 10 / 15 / 20 m)
    and `p2108Aero(fGHz, theta deg, p %, h, hm)` (3.3 of P.2108-2:
    Earth-space / aeronautical clutter by elevation; erfinv = Giles' single
    precision). Checked: the 282,800 rows of Py2108's validation table
    within 2e-6 dB. Used below its 0.5 GHz floor too
  - Okumura-Hata / COST-231 was a model too (map + link row) and was
    DROPPED (the user's call, 2026-10-12): no terrain - concentric circles
    on the map -, valid only for 150-2000 MHz, 1-20 km, a 30-200 m base, so
    most KrakenSDR paths (handheld to handheld, < 1 km) were outside it. A
    saved 'hata' falls back to P.1812
- LINK (`runLink`, message `m.link`: a = the TX, b = the RX, each {lat,
  lon, h, amsl}): the great circle A -> B (slerp)
  sampled every cell of the finest zoom <= 14 that keeps it to `PL_NMAX`
  4000 points (30 km: z14 8 m; 300 km: ~z11; 1000 km: ~z9), bilinear on the
  job's tiles cleaned as a viewshed's (missing neighbours mirrored), the ends
  on zoom-14 ground; sea = raw height <= 0. P.1812 with flag4 (as the
  coverage, so the two agree), ITM median (mdvar 12, 50 / 50 / 50) and at the
  reliability (mobile mode 2: time = location = rel, situation 50), free
  space (slant); clutter: one end above sea level (in the air) -> 3.3
  at the OTHER end by the direct ray's elevation there (hm from its type: 8
  suburban / 15 urban / 25 dense, none for water / open), both on the
  ground -> 3.1 at both, both in the air -> none (checked: the same path
  both ways round = the same P.1812 loss, the clutter moved). Also the
  elevation at each end, the least first-Fresnel clearance and where, the
  Bullington edge (dbp km, hbp m incl. the Earth bulge) and the profile
  (Float32 d km, h m) for the chart. Its OWN worker instance (`plWorker`,
  the same code: a link must not wait behind a coverage job; one job at a
  time, a newer link cancels the running one)
- COVERAGE (`runSignal` + `plSweep`: a viewshed job with `m.sig` = {f, tgt
  (the other end's height), amsl, pol, N0, envTx, envRx, hmTx, hmRx} - the
  worker's "Tx" is always the SITE, its clutter by the site's role
  (`plSigMsg`); the site's `m.h` above its ground (`m.amsl`, a site in the
  air, is supported by the worker but not used by the page); the
  viewshed's own job, so its terrain tiles are shared):
  every ring's terrain at once (~4 MB each, 10 rings
  at 1000 km), the rays of sweep() but RAY-MAJOR through all the rings, each
  ring from where the previous ended to its rExt (the coarser ring's cells
  there stay empty, under the finer ones). Each ray carries the upper
  convex hull of its profile in the curved-Earth frame (e = h - s^2 / 2 ae,
  where rays are straight): the receiver's horizon is the tangent from it
  (binary search) -> Bullington's S_rim (P.1812's slopes differ from these
  by +-s / 2 ae: the same argmax); the hull of the plain heights for the
  obstruction correction (hobs = a support query, alpha_obr a tangent,
  alpha_obt a running max); running sums v1 / v2 (eq 85-86) and the sea
  count (omega). Line of sight: nu_max over the profile EXACTLY, by branch
  and bound over blocks of 32 / 1024 samples (their highest point bounds
  nu; J = 0 below -0.78, so clear paths rule out whole blocks) - the point
  of least clearance is often not a hull vertex (a 2 m receiver's own
  foreground): hull candidates alone were up to 10 dB off. The profile =
  every 2nd sample (0.8 cell; the terrain is no finer). Two passes: 1 finds
  each cell's sample (nearest its centre, as sweep), 2 evaluates P.1812
  once per cell + the clutter (both on the ground: 3.1, fixed per job; the
  RX in the air: 3.3 at the TX by the elevation there; the TX in the air:
  3.3 at the RX). Per ring `loss` (u16, 0.1 dB; 65535 = the receiver under
  the ground) + `need` / `ground` FROM sweep() itself, run after plSweep on
  the same terrain (plSweep's own line of sight - one ray through all the
  rings - differed from sweep()'s ring-by-ring one in ~0.1 % of the cells;
  now both modes show the identical line of sight, checked cell by cell;
  a cell only sweep() sampled takes its neighbours' loss). Per ring also
  `fad` (u16, the fading's path factors from P.1812's terrain analysis -
  with Longley-Rice too, ~+15 % there: high byte the terrain's knife-edge
  loss beyond the smooth Earth's, Lbull - Lbulls, 0.25 dB; low byte Lbs -
  Lbd + 32 dB, 0.25 dB; 255 = free space / no troposcatter), carried
  through the fills, the helpers' merge and the link of a cell's
  neighbours; checked against exact P.1812 per recorded sample (±1 per
  byte). Spherical-Earth constants hoisted (`seConst`), one
  polarization, one ground when omega is 0 / 1. Progress `['sig', %]`
  (status: "signal N %…"), a break for a cancel every 0.25 s
  Checked (tools/pathloss_validate.mjs; scratch t_cov.mjs): the loss at
  every recorded ray sample = `p1812P2P(flag4)` on that ray's profile prefix
  within 1e-11 dB (Auckland, Zermatt, the Coromandel coast, a synthetic
  coast; receivers above ground / sea level, a TX in the air, clutter, both
  polarizations); a
  link to a cell's centre vs the cell: median 0.04-0.1 dB, p90 0.3-1.3 dB -
  single cells up to ~16 dB apart at shores / ridges (the coverage's outer
  rings use coarser terrain: 30 m at 15 km). Time (node on the Pi 5, tiles
  in memory): 30 km ~6 s, 300 km ~11 s (2.5 / 4.4 M cells; the viewshed
  1-2 s); in the browser on the Pi it took ~17-21 s for 30 km
- SEVERAL CORES (`plSweepPar`, the user's request after WebGPU was weighed:
  no f64 in WGSL - the exact-to-the-reference checks would go -, per-ray
  sequential hulls, no WebGPU on the Pi's Chromium): the signal sweep's
  rays split over HELPERS - nested workers running vsWorkerMain itself
  (`helperPool`, kept between jobs) - contiguous ray ranges, both passes:
  pass 0's best[] merged by the minimum, pass 1's cells merged in ray
  order (a later ray overwrites, as in one plSweep) -> THE SAME CELLS TO
  THE BIT (checked: hash of every need / ground / loss, P.1812 + ITM). The
  coordinating worker does the line of sight (sweep() into needL / groundL)
  meanwhile and the finish (`plSweep(.., {finish: true})`). Helpers:
  hardwareConcurrency - 1 (max 7), fewer if their terrain copies would
  pass 600 MB (no SharedArrayBuffer without cross-origin isolation), none
  below 2; `m.par` / page `vsPar` (null auto, 0 one worker - tests); not
  for m.debug or without nested workers (node: the validator). Replies
  carry job + pass (a cancelled job's late reply can't answer the next
  job); a failure / cancel stops every helper. The row's tooltip says "on
  N workers". Only the signal sweep. The LINE OF SIGHT viewshed was
  parallelized too (the user asked; ring by ring - the horizons carried
  between rings, the next ring posted as soon as the last one's horizons
  were back, compact cell lists merged in ray order: bit-identical) and
  REVERTED: its sweep is memory-bound, ~175 ns a cell, so copying each
  ring's terrain to every helper (~270 ms at 300 km, 3 helpers) + merging
  their cells + the fill-in (~450 ms) cost what the helpers saved - on the
  Pi 5 with the receiver running: 30 km 0.85 -> 0.77 s, 300 km 1.43 ->
  1.5-1.7 s (slower); more helpers make the copies worse. Without
  SharedArrayBuffer (cross-origin isolation) that won't change.
  The line of sight runs DURING PASS 1 (pass 0 is quick - only each cell's
  sample -, so during pass 0 it held pass 1 back: 1.9x -> 2.3x). Measured
  (scratch t_par.py, browser on the Pi 5, 4 cores, the KrakenSDR stack
  using ~0.85 of one): P.1812 30 km 8.6 -> 3.8 s, Longley-Rice 10 km 27.8
  -> 11.6 s, the same hash of every cell value; a parallel job cancelled
  at 94 % and the next = one worker's
- THE MODEL of the coverage = the page's choice (`m.sig.model`; the user
  expected the map to follow it - it was P.1812 only at first): p1812 (all
  of the above); fs = free space (32.45 + 20 log f + 20 log of the slant
  distance, as the link's) + the clutter; itm = Longley-Rice per cell
  (`itmCell`) + the clutter - the picker says "much slower"
  (`PL_MODEL_OPTS`). fs skips the terrain work (~3.5 s for 12 km in node).
  Loss 65534 = outside the model's range (ITM: an antenna > 3000 m above
  its ground) - transparent, the hover says so
- ITM PER CELL (`itmCell`, model 'itm'): ITM 1.2.2 point to point on the
  ray's own points (the site, the points added so far, the cell). Their
  spacing grows ring by ring, so each of ITM's steps in profile INDICES is
  taken in DISTANCE - and reduces to NTIA's exactly on an evenly spaced
  profile: hs = the mean of the middle 80 % of the points (index-based as
  ITM; -> Ns -> ITM's own Earth radius, per cell), the horizons over the
  interior points, delta h resampled by distance (p10 from the fractional
  point-index span), LinearLeastSquaresFit with trapezoid weights in
  distance (= ITM's half-weighted ends). The shared code: `itmQuickRest`
  (QuickPfl after the horizons), `itmDhFit` + `itmSelect` (quickselect),
  `itmZg`, `itmLongleyRice`, `itmVariability` (median, mdvar 12 - as the
  link's). Incremental per ray (`itmAdd`): prefix sums (hs, the fits:
  O(1)); the TX horizon = the upper envelope of the lines (z - zt) / s - c
  s in the cell's curvature c = 1 / 2 ae (exact for each cell's own c, a
  binary search); the RX horizon = the hull of (s, z - cRef s^2) (cRef from
  the site's Ns) + a climb along it in the cell's c (they differ only on
  near ties). Delta h per cell (`itmDhRay`: <= 245 points; each one's
  neighbours from the runs of even spacing - a ring's - not a search) -
  about half of ITM's time. Reusing a ray's last delta h while its window
  moved < 2 % halved that but put single cells up to ~5 dB off: rejected.
  Checked (tools/pathloss_validate.mjs): the distance-based definitions
  (`itmRay`, plain loops) = NTIA's itmP2P on 300 random evenly spaced
  profiles within 1e-8 dB; the sweep = itmRay on every recorded ray sample
  within 1e-8 dB; itmRay on the ray's own uneven points vs itmP2P on them
  resampled every 10 m: median 0.03, max 0.22 dB; a link vs its coverage
  cell: median 0.05, p90 0.1 dB. Time: ~6x P.1812 (node on the Pi 5, the
  same 1.94 M cells: 47 vs 7.8 s; delta h ~40 % - its resampling, the fit,
  two quickselects -, LongleyRice + variability ~25 %) - the model's
  tooltip says so. Tried for delta h: a binary search per point, then the
  runs (2x), no closures in the loop, a top / bottom-k buffer instead of
  quickselect (slower: terrain residuals come in rising / falling runs)
- Page: `plState` (globals block; localStorage `kraken_pl`, rebuilt field by
  field): f MHz, ptx W, gt / gr dBi, lt / lr dB, bw kHz, nf, snr (->
  sensitivity = -174 + 10 log BW + NF + SNR), model (p1812 | itm | fs -
  the map's and the link's budget; the strip lists all three), polT /
  polR + angT / angR (the TX / RX antennas' polarization, see below; an
  old single `pol` 0 / 1 becomes both antennas' h / v), N0 (301:
  troposcatter + ITM), climate + ground (ITM, `PL_GROUNDS`), envTx / envRx
  (P.2108 types, `PL_ENVS`: the transmitter's / receiver's surroundings,
  whichever end that is), fade (auto = Rician, K per cell from the path,
  see FADING | rician = K everywhere | rayleigh | none), K dB (a clear
  path's), rel % (the wanted reliability), sigL
  (null = P.1546-6 (12): K + 1.3 log f, K 0.5 open / a receiver in the air,
  1.0 an antenna up near the clutter, 1.2 one in it - of the RECEIVER: the
  site when it is RX, else the target, `plSiteSigma`), set / prof (folds),
  link {site (a viewshed item id), lat, lon, name, mk}. What each setting
  redoes (`PL_REDO`): S = the sites' signal jobs (f, the TX antenna's
  polarization - the models' h / v -, N0, clutter, model; climate + ground
  only with ITM - they are in `plSigMsg`, so in `vsKey`: a job runs only
  where the key changed), L = the link; everything else (the RX antenna's
  polarization, fade, K too) only repaints from the losses + `fad` kept (`vsRepaint` ->
  `plPaintSig`). The viewshed's target height / reference are shared
  (`vsSetTgt` / `vsSetRef` call `vsSigChanged`). The REFRACTION is not:
  `vsK()` = 4/3 in the signal mode (the user's call: its models are for
  radio waves - light / no bending made P.1812's effective Earth radius,
  the line of sight edge / fading choice, the link's profile and the
  air-to-ground clutter angle inconsistent; ITM always took its own from
  N0), the stored choice (`vsState.k`, kept) in the line of sight mode -
  in the job key, the job, the link (`plLinkMsg`); the selector shows
  "Radio (k = 4/3)" disabled in the signal mode (t_kref.py)
- POLARIZATION (`plPolLoss(ends)`, `plPolEnds(it)` / `plLinkPolEnds(e)`,
  `plWavePol`): each antenna 'v' / 'h' / 'c' (circular) / 'a' (linear at
  angT / angR degrees from horizontal). The mismatch relative to matched
  antennas (the models are for those): the share co of the power the RX
  antenna takes - two linear cos^2 of the angle between, linear /
  circular 1/2, circular / circular 1 (the same hand assumed) - with
  depolarization x: co (1 - x) + (1 - co) x, over (1 - x). x = the
  antennas' own (XPD 20 dB, `PL_XPD_ANT`) and each END's clutter
  (`PL_XPD` by the Clutter type: none / water none, open 15, suburban 10,
  urban / forest 7, dense urban 5 dB - mobile radio measurements, 3GPP
  38.901's scattered rays 7-12 dB), fully for an antenna down in it, x (1
  - (h / R)^2) up to P.2108's clutter height R (`PL_ENV_R`), none above it
  or in the air (`plDepol`), combined as mixings in turn. The wave keeps
  its polarization along the path (direct, reflected, diffracted,
  troposcatter: forward, in the path's plane) - scattering off the clutter
  mixes it, so the figure is the SAME over the whole map. Until
  2026-10-12 XPD was 20 dB in sight / 8 dB beyond by the cell's line of
  sight: the user's 320 m TX over the sea, H TX / V RX, fell to 79 % at
  the 87 km horizon and jumped back to 96 % just past it (~13 dB less
  mismatch); scratch t_seapol.mjs / t_seapol2.mjs. The propagation models'
  own polarization (P.1812 / ITM take h or v) = the TX antenna's (a
  slanted one: the nearer, 45° horizontal; circular: vertical). Shown
  under the model ("matched" / "mismatch x dB": the link's ends, else the
  first transmitter's - `plPolNow`), in the hover ("+ polarization x dB")
  and as a budget row in the strip
- FADING (`plFading(terr, dts)`, `plKeff`): auto = Rician whose K (the
  setting: a clear path's) follows the path, continuously - TERRAIN in
  the way lowers it dB for dB by its knife-edge loss beyond the smooth
  Earth's (Lbull - Lbulls: the steady path dims behind a hill, scattered
  power still reaches round it; the Earth's bulge over open sea / flat
  land adds none), TROPOSCATTER (Rayleigh) taking over far beyond the
  horizon: K = Kt (1 - w) / (1 + Kt w), w = 1 / (1 + 10^((Lbs - Lbd) /
  10)). From P.1812 with either terrain model (the cell's `fad`, the
  link's `p1812`); free space: K. Until 2026-10-12 auto was Rician in
  sight / Rayleigh beyond - a ~19-point step down at the same horizon
  once the polarization step was gone. Shown in the hover ("Rician K x
  dB") and the strip ("Rician K x dB (6 dB on a clear path; terrain in
  the way y dB, troposcatter z %)", "≈ Rayleigh" below -10 dB)
- LABELS (`vsRender`, BOTH modes - the user's call 2026-10-12: line of
  sight said "Antenna" / "Target"): a site row's "TX antenna" / "RX
  antenna" by its role; the target -> the other end's ("RX antenna" for
  TX sites, "TX antenna" for RX sites, "TX / RX antenna" when the shown
  sites have both, or none). No TX / RX tag on the rows any more (removed
  2026-10-13: TX antennas are placed by right-clicking)
- Reliability: `plOutage(m, sigma, kind, K)` = integral of phi(z)
  F_fade(sigma z - m dB) over +-6 sigma (121-point trapezoid; the Rician
  power CDF tabulated ONCE per K, `plFadeTab`: the power pdf with I0e - A&S
  9.8.1 / 9.8.2 - integrated by Simpson every 0.02 dB from -60 to +20 dB,
  within 1e-4 of a fine reference up to K 25 dB, ~7x faster than the old
  per-call Simpson over the envelope; 'none' = Phi(-m / sigma) exactly; K
  >= 30 dB = none) -> `plNeed(rel, sigma, kind, K)` (bisection, cached).
  Per cell: `plRelLut(sigma, kind, K)` (the reliability every 0.25 dB of
  margin from -50 to +70 dB, linear between), auto: `plRelSet(sigma)` = a
  table per K level (Rayleigh at -20 dB, Rician up to the setting <= 30 dB
  in <= 1 dB steps, ~0.3 s the first time per sigma on the Pi) + idx[fad]
  (65536 entries, per K setting) -> `plRelK` interpolates between the two
  levels around the cell's K: within 2e-4 of the integral at its exact K.
  Checked: closed forms (Rayleigh 90 % = 9.773 dB, lognormal 1.2816 /
  2.3263 sigma) and Monte Carlo (Rayleigh / Rician 6 dB with sigma ~4 dB:
  10.03 +- 0.02 % outage at the 90 % margin)
- The viewshed side (👁 VIEWSHED, hooks only): `vsKey` = the geometry +
  `|` + JSON `plSigMsg(it)` in signal mode; `vsResOk` also accepts a signal
  result in line of sight mode (its `need` / `ground` are sweep()'s - mode
  switches compute nothing); `vsPump` posts `sig` in signal mode;
  `vsOnWorker` keeps `loss`; `vsPaint` hands a signal result in signal mode
  to `plPaintSig`; `vsDraw` draws `plDrawSig` instead of the tinted
  per-site layers (the radius circles + dots stay); `vsHoverRender` prefixes
  `plHoverSig` (TX / RX, reliability %, dBm, loss). Items: `role` 'tx' |
  'rx' (old saved items: the station rx, others tx), `vsSetRole` (a new job
  only when the two ends' clutter differs; always a repaint - the
  receiver's sigma). `vsSetMode` (persisted), `vsSigChanged` (a stale
  running job is cancelled, the next pumped)
- Paint: `plPaintSig` -> each ring's canvas pixel = ABGR with R = 1 + the
  reliability x 254, G = 255 in sight, A 255 (no data / the target under the
  ground: transparent); res.vis = the area >= the wanted reliability ("%
  covered"). `plDrawSig` -> `plComposite`: each shown site's rings coarse ->
  fine onto `plOffS`, then onto `plComp` with 'lighten' (= the max per
  channel: R the BEST site's reliability, G in sight of ANY), getImageData,
  per screen pixel `plColLut`: a GRADIENT (`PL_STOPS` 0 / 25 /
  50 / 75 / 90 / 95 / 99 %, red - orange - yellow - greens, linear in RGB
  between - `plRelRgb`; the user's calls: first hard bands, then every
  place coloured (below 25 % red, not blank), then blended - no hard
  edges); EVERY place with data within the radius is coloured; below the
  wanted reliability the alpha ramps 255 -> 105 over the 10 % below it
  (a step was a hard edge too); `plComp` is drawn at 0.6. The LINE OF
  SIGHT EDGE on its own layer `plLine`, drawn over it at full strength:
  pixels in sight next to one out of sight AND the other way round (2
  device px; dilated once more at devicePixelRatio >= 1.5 = 4, i.e. 2 CSS
  px), black alpha 0xd9 (the user's call, 2026-10-12: it was 1 px faint
  white inside the colours, hard to see). `plState.losEdge` (default OFF -
  the user's call; `kraken_pl` - its first version `los` defaulted on and
  was saved with everything, so that key isn't read): the "Line of sight
  edge" tick on the Wanted reliability row
  (always shown), a redraw only - in the composite's key, not the jobs';
  the legend names it only while on (`.pl-ll` swatch). The legend: a gradient bar with the stops
  evenly spaced + their labels (`.pl-leg-g`, `.pl-leg-t`),
  putImageData, drawn at alpha 0.6. Cached by `plCompKey` (view, size,
  `plSigVer` - bumped by every paint -, rel, the sites): pointer moves
  don't redo it. Checked in the browser: every sampled pixel's colour = the
  band of the best site's reliability from the cells (2000+ pixels, one and
  two sites; faint pixels read back +-1 per channel - premultiplied canvas)
- The link: `plState.link` = a site + a place; `plEnds()` -> {tx, rx} by the
  site's role (the place at the target height / reference, so the link =
  the coverage cell there: 146.9 vs 146.6 dB in the test). Made by the
  right-click menu ("📶 Path loss analysis from TX 1 to here": the
  selected TX antenna, else the first placed - none: no item; `plMenuItem`
  / `plLinkLabel` / `plLinkTip` via `plLinkSite` - until 2026-10-12 "📶
  Link: TX here ↔ “Station”", then "📶 Signal from here to …" / "… to
  here" + "Add RX antenna here", reconciled by the user 2026-10-13), a
  marker (menu + "📶 Path loss from TX 1" in its panel, `plLinkShort`,
  `plLinkMarker`), an OSM mast (the same, `plLinkMast` - at the target
  height; "👁 Line of sight" / "📶 Add TX antenna" adds the mast as a TX antenna
  at its own height). The marker panel and the mast popup re-render on
  every map draw, so their words follow the mode and the picked site. The
  menu wraps long items (max 280 px) instead of cutting them; the site = `plPickSite()`:
  the selected TX antenna, else the first one placed (never the station or
  an RX site; none: null). Turns
  the viewshed on in signal mode. Only in signal mode (`plLinkOn`). The
  place: selected + dragged (`mapSelectObj('pl:far')`, drag kind 'pl' ->
  `plMoveFar` after a 0.15 s pause); the site's point moves the link with
  it. ✕ (`plLinkRemove`; removing the site - Delete on the selected place too -
  removes it too). `runLink` on its own worker (`plWorker`, see LINK above)
- UI: the signal block `#map-vs-sig` in the viewshed panel (class
  `vs-sig`: shown only by `.map-vs.sig`; `vs-los` the reverse): Freq + TX
  power, Model (and ITM's climate / ground when it is Longley-Rice), the
  TX / RX antennas' Polarization (+ an angle field for 'custom', + the
  mismatch) - not folded: they change the map -, the rest folded
  under "Antennas, receiver, channel" (gains, cables, receiver, N0,
  clutter TX / RX, fading + K, sigma_L), "Wanted reliability", the legend
  (`plLegendHtml`, naming the model); `#map-vs-link` (the link in one line + Show / Hide). `plRenderSig`
  runs from `vsRender`; `plRender` = `vsRender`. Strip `#map-plk` at the
  bottom of the map (`.map-pane.pl-open` lifts the DF panel, the marker
  panel and the status line): TX → RX, the path, the three models (a row
  click = the budget's and the map's model), clutter (which end,
  air-to-ground), polarization (the mismatch, TX → RX), received (dBm,
  µV), margin + reliability, ITM at the reliability, the heights above sea
  level, terrain zoom / tiles / ms; the profile canvas (terrain + the
  Earth bulge over the chord, direct ray, F1 + 0.6 F1, Bullington's
  construction, sea blue, TX / RX in their colours; under the pointer:
  distance / ground / clearance, and a dot on the map). The chart's F1
  radius is clamped at >= 0: Dm * i / n rounds a hair past Dm at some
  lengths, the sqrt went NaN and took the whole y scale with it (the
  user's "the profile sometimes doesn't draw after a move" - reproduced
  by t_drag.py, real mouse drags, deterministic per position). On the map
  (`plDraw`, signal mode only, after the rings, before the markers): a
  TX / RX tag beside each shown site, the link's great circle coloured by
  its reliability band (dashed while computing), the place's TX / RX badge,
  the edge ▲
- Tests: committed `tools/pathloss_validate.mjs` (node 20, from
  kraken_doa_v2: 1-3 the references, cloned from GitHub into --refs; 4-5
  synthetic terrain served as generated terrarium PNGs through fetch -
  offline, the whole decode / clean path; 6 `--net` real terrain).
  Scratch (job 26b3d7af): t_itm_rand.mjs (vs the compiled NTIA C++),
  t_cov.mjs, t_link.mjs, t_need.py (signal vs line of sight `need`: 0
  cells differ), t_drag.py (real mouse drags of the link's place, random
  speeds / pauses: the profile drawn for the current link whenever the
  worker is idle - 2 of 40 blank before the F1 clamp, 0 of 80 after), and
  t_uni_ui.py - 91 checks (incl. TX antennas only + the one path loss
  item, line of sight = the station's only, no menu items outside signal;
  the labels by role;
  polarization: RX-only a repaint, TX h / v
  a job, 60° = cos^2 30°, circular, urban receivers less mismatch - each
  vs an independent formula, one figure; the link's budget row; the
  fading: tables vs the integral, K per cell 6 dB clear / lower behind
  terrain, hover + strip K, the K setting a repaint only; the edge line:
  off by default, black, >= 2 px, switched on / off - no job, the same colours, the
  legend, kept over a reload; the
  TX / RX antenna labels; an old `pol` / a saved 'hata' migrating; t_dpr2.py:
  the line 2 / 4 device px at devicePixelRatio 1 / 2) in headless Chromium on the
  page served statically WITH a UTF-8 charset (no receiver; the station set
  from the test): no Path loss button, line of sight as before, signal
  (job, status, legend, roles), the composite (line pixels, coverage out of
  sight, per-pixel colour = band of the best reliability at zoom 16 - at
  zoom 10 a pixel averages ~16x16 fine cells), the reliability table,
  hover, repaint-only power / wanted reliability, role swap (same clutter:
  no job, sigma changes; other clutter: a job), a second site (best server,
  more coverage), the menu's words ("📶 Signal from “Point 1” to here"
  with a TX site selected, "📶 Signal from here to “Station”" + its tooltip,
  "📶 Add TX antenna here" / line of sight "👁 Line of sight
  from a TX antenna here", the buttons), link + strip +
  budget + link = coverage cell, chart + hover, model row, drag the place
  (no coverage job), Esc, ⇄, line of sight again (no job, strip hidden) and
  back, viewshed off / on, ✕, a link from line of sight mode, the selected
  site, a removed site's link, reload, damaged localStorage, no exceptions

