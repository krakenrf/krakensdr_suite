# Mobile direction finding

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Mobile direction finding (🗺 Map, coherent mode; `src/rdf_engine.cpp`, `src/rdf_mapper.cpp`):**
- Live lobes: the sampler thread (rdf_mapper, 10 Hz) takes every NEW MUSIC
  frame of every VFO with DoA (`getResultStampMs` changes; not
  `isResultStale`), turns it to the north frame with the station heading
  (compass bearing tau of array angle b: tau = heading - b; b unit circle
  CCW from ANT0 = vehicle front) and publishes it as 720 x 0.5 deg u8 - the
  pseudospectrum LINEAR, min..max, the MUSIC DoA plot's (PolarPlot) scale; a
  dB scale drew a far fatter lobe than the plot - + the sub-bin peak in `{"rdf":{on, range_km, state, station
  {lat, lon, hdg, spd, src}, vfos:[{id, mhz, lobe, peak, conf, held, frames,
  gate_m, moved_m, records}]}}` (websocket_server rdf_timer, 2 Hz).
  `held` = GPS course while slower than MIN_SPEED (drawn dashed)
- Time alignment (the Android app pairs a frame with the latest fix): the
  GPS track is kept 20 s (fix valid time = local receive time -
  FIX_LATENCY_S 0.15 s); a frame is placed at its stamp - FRAME_LAG_S 0.3 s
  and gets the interpolated position + heading (circular) + the turn rate
  over +-0.5 s (`fix_at`)
- Gating per frame (`state`): coherent mode + DoA on + a fix (< 3 s old) +
  no noise-source calibration (`doa_is_calibrating`) + collecting on (RDF:)
  + not a static location + GPS course: speed >= 2 m/s and turn rate <= 8
  deg/s (compass: <= 30 deg/s). Frame bearing sigma = hypot(2 deg MUSIC /
  site, heading: compass 4, GPS course 1.5 + 8 / speed)
- `rdf::north_lobe` (rotate + interpolate + Gaussian blur + normalise) ->
  `rdf::Gate` per VFO: frames are combined as the normalised GEOMETRIC mean
  of their (5 % floored) probabilities - repeated views of one bearing
  multiply (an arithmetic mean only widened the lobe and lost to the
  Android app in the simulator) - and the window closes after R *
  tan(3 deg) of travel, 15-60 m (R = distance to the estimate, 1 km before
  one); a pause > 10 s drops the window, > 90 s without closing too.
  Stopped = no records (no double counting)
- `rdf::Grid`: cells in METRES (azimuthal equidistant around an origin,
  n x n), cell bearing = planar angle in that frame + the meridian
  convergence at the record (exact to << 0.1 deg over 50 km; the Android
  app's grid is a lat/lon lattice). Each record adds alpha (0.5) * ln((1 -
  eps) * p(bearing - delta) * BINS + eps) (log-likelihood ratio vs "no
  information", eps 0.05 = multipath / outlier floor; p linear-interpolated
  on 0.5 deg bins) per offset plane delta in {-8..8 step 2} deg; cells within
  max(30 m, 1.5 cells) of the record get 0 (the bearing of a cell around the
  receiver is undefined). Marginal M = log-sum-exp over delta with a N(0, 4
  deg) prior: a systematic rotation of all bearings (array mounting,
  compass) is integrated out instead of fitted + applied (the Android app's
  CAL sweep) - the offset's posterior mean / sd is reported. Posterior read
  scale s = min(1, 40 / (alpha * records)) (systematic errors don't average
  out). `estimate()`: MAP cell + parabola per axis (the posterior mean of a
  banana-shaped early posterior is far off), the main mode (8-connected
  cells >= 1e-4 of the peak) -> covariance ellipse (+ a cell's variance),
  50 / 95 % HPD radii, mode mass, `at_edge`
- `rdf::Solver`: coarse grid 256 x 256 over +-range_km (default 10 -> 78 m
  cells), fine grid 128 x 128 around a confident coarse estimate (mode mass
  > 0.5, not at the edge, r95 < range / 3; half = clamp(4 * r95, 250 m,
  range / 3)); the estimate comes from the fine grid while its mode isn't at
  its edge. Max 4000 records (then the older half is thinned)
- SLIDING WINDOWS (the user's request 2026-10-21: "the heatmap moves
  smoothly as the vehicle moves, rather than in jumps when the vehicle nears
  the edge"; before, the coarse grid stayed on its first record until the
  vehicle was 0.7 range out and was then REBUILT around a new centre - a ~7
  km jump of the heat map, 5 of the user's 20 Android logs had one - and the
  fine grid was rebuilt each time the estimate moved 0.3 of its size). Each
  `rdf::Grid` is now a window onto a fixed metric frame (azimuthal
  equidistant around `lat0 / lon0`; the window centre `cx / cy`, cells at
  cx - half + (i + 0.5) cell) that `scroll(di, dj, recs, count)`s by WHOLE
  cells: the cells it keeps keep their sums (exact - their frame
  coordinates don't change), the column / row bands coming in get every
  record (`add_rect`); bit-identical to a window built there (scratch
  t_scroll.cpp, 22 shifts incl. >= n). The coarse window follows the VEHICLE
  (`follow_coarse`, before each record is added: centred on it, the LAST
  confident estimate - `pull_` - kept within KEEP_EST 0.7 of the half-width,
  at most FOLLOW_STEP 2 cells a record or 1.5x what the vehicle moved -
  never lags it); the fine window follows the ESTIMATE (scrolls; rebuilt
  only when its size must change > 1.6x or it jumps >= half its width).
  Past REANCHOR_M 100 km from the frame's origin a window gets a new frame
  there (the same place on the map). Away from the origin the frame's north
  isn't true north (meridian convergence: -0.32 deg 47 km east at 37 S):
  `Grid::north_deg()` -> the grid message's `rot`, the ellipse's `ang` is
  turned to true north. Cost: each scroll = the band's cells x all records
  (~records x 15 us on a desktop per cell column); a long drive is CHEAPER
  than before (no rebuilds: 30 km, 900 records 48.5 -> 17 ms per record),
  a drive that never left the old 7 km pays the bands (the JS replay:
  0.7-16 s for the user's logs, ~+10 %; a 2 h 4-rows/s log 26 -> 41 s). The
  simulator (tools/rdf_sim.cpp, 40 x 5 / 15 min) gives the same errors and
  95 % hits as before (36 / 40, 40 / 40); the user's logs the same
  estimates (0-57 m apart)
- Engine thread (rdf_mapper): owns the solvers; a VFO retuned by more than
  10 kHz starts over (its old session is parked by frequency); records are
  saved to `rdf_session.bin` (cwd, gitignored; "KRDF1" + blocks of freq,
  saved time, records with the lobe as u8 log probability over 20 nats) every
  30 s and at shutdown, and a VFO on a frequency within 5 kHz of a saved
  (< 24 h) session resumes it. Since the per-talker maps the file is
  "KRDF2" (each block also carries the talker ID, "" = the VFO; "KRDF1" is
  still read) and every map is keyed VFO + talker (`Key` in rdf_mapper.cpp) Grid messages at most every 2 s per VFO:
  `{"rdf_grid":{vfo, mhz, records, range_km, nats, coarse/fine:{n, cell_m,
  nw, se, c: [lat, lon] (the window's centre), half (m), rot (deg: the
  true bearing of its "up" - the page's `rdfGridDraw` draws the raster
  there turned by it; a page from before stretches it between nw and se),
  data (b64 u8, row 0 north, 255 * (1 + s (M - Mmax) / 12 nats))},
  est:{lat, lon, r50, r95, a, b, ang, mass, edge, off, off_sd}, lines:[[lat,
  lon, bearing, t]] (last 150 records)}}`; `GET_RDF` (page opening the map)
  makes the next push carry every VFO
- Right-hand pane tabs (coherent mode, map on): MUSIC DoA | 🗺 Map | ⊞ Both
  (`mapState.tab` 'both' -> body class `rp-both`: the DoA displays on top -
  side by side - and the map below, `#rp-vsplit` divider dragging
  `mapState.dh` -> `--doa-h`, double-click = 42 %; `rpApplySplit`)
- UI (kraken_doa.html "Mobile DF" block): "📡 DF" panel bottom-left of the
  map (coherent only, hidden until the first `rdf` message): VFO select (All
  / Dn), Lobe / Lines / Heat toggles (`mapState.rdf*`, localStorage;
  Lobe = the lobe shapes, Lines = EVERY bearing line: the lobes' peak lines,
  a talker's earlier bearings, the driving records' lines - the peak lines
  used to come with Lobe, so Lines did nothing at a fixed station),
  Collect (RDF:), Range (RDF_RANGE_KM:), ◎ (centre on the estimate), Reset;
  status per VFO (bearings, next in N m, TX +-r95, edge / mode-mass
  warnings, measured offset when |off| >= 2 deg and > 2 sd after 20
  records) and the reason frames aren't used (`RDF_STATE`). `rdfDraw` (in
  `mapDrawNow` before the rings): heat canvases (`RDF_LUT` blue..red,
  transparent below 8 %) drawn by `rdfGridDraw` - a square of 2 x half
  around c, turned by rot (nw / se from an older receiver), bearing lines (1.5x the
  range, older fainter), estimate X + 2.45-sigma (95 %) ellipse (dashed at the
  edge), lobes (radius 0.3 of the map, dashed when held) + the peak line,
  the heading tick. Heat map: the selected VFO's, or in All the only VFO
  that has one
- Simulator (`tools/rdf_sim.cpp`, `g++ -std=c++20 -O2 -Iinclude
  tools/rdf_sim.cpp src/rdf_engine.cpp`): Monte-Carlo drives (2.5 deg noise,
  +-5 deg mounting offset, 25 % multipath stretches with a stronger
  reflection, lagging GPS course, 0.4 s frame latency) vs a re-implementation
  of the Android app's choices (dB-averaged 20 m windows, 3 deg blur, alpha
  0.25, cap 25, nearest bin, best-offset sweep -6..6, latest fix). Env knobs
  SIM_CLEAN, SIM_MP, SIM_ALPHA, SIM_ESS, SIM_GATE_MAX, SIM_RANGE0, SIM_NOMARG,
  SIM_MAP. Result: equal or better accuracy, a 95 % region that holds the
  transmitter (40/40 vs the Android-style 28/40 on 5-minute drives), the
  mounting offset measured to ~0.5 deg (see the report in the commit / chat)
- End-to-end test (scratchpad h/t_rdf.py + e2e5/fake_drive.py: fake gpsd on
  12947 via `KRAKEN_GPSD_PORT` + a fake heimdall streaming a 5-element UCA
  tone from a hidden transmitter along a driven loop): lobe within 0.2 deg
  of the true bearing, estimate within ~10 m after 26 bearings, VFO select,
  Reset

