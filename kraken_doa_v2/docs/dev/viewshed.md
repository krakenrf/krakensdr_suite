# Viewshed

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**👁 Viewshed (UI only, kraken_doa.html "👁 VIEWSHED"):**
- Line of sight over the terrain from the station or any picked place:
  "Viewshed" button (`mapState.vs` layer + `vsOpen` fold). The STATION'S
  viewshed is always there (`vsEnsureStation`, from `vsToggle` / `vsRender`:
  re-added when missing; its row has no ✕, `vsRemove` refuses it - hide it
  with its colour box) - no "+ Station" button (the user's call
  2026-10-12, and no "+ TX Node" either: TX antennas are added only by
  right-clicking). DELETE removes the selected node (`keydown` beside Esc:
  a viewshed site - not the station's - or the link's place = the link;
  not while typing in a field). The station's mast on
  the map carries the callsign (Station Information `station-id`, redrawn
  on `STATION_ID:` / an edit). LINE OF SIGHT MODE IS THE STATION'S
  VIEWSHED ONLY (the user's call 2026-10-13): `vsJobItems` = the station,
  `vsDraw` skips the rest, their rows are hidden (CSS), `vsHit` picks none,
  and the right-click menu / marker / mast have no add item there - nor
  any viewshed / path loss item at all outside signal mode or with the
  viewshed off (`plMenuOn`, 2026-10-13) (it was
  "👁 Line of sight from a TX antenna here", before that "👁 Viewshed from
  here"). Signal mode adds TX antennas: right-click "📶 Add TX antenna
  here" / "at “marker”" (`vsAddLabel` / `vsAddShort` / `vsAddTip`), the
  marker panel and the OSM mast popup (`vsAddMast`: antenna
  at the mast's `height` tag, feet with ' / ft). Up to `VS_MAX` 6, each in
  its own colour, drawn over the tiles (`vsDraw`, before the DF heat maps)
  with a dashed radius ring (the circle on the ground, drawn point by point
  - at 1000 km it is visibly egg-shaped on the map). Radius 1 to
  `VS_RMAX_KM` 1000 km (globals block; aircraft at 10 km drop below the
  horizon at ~450 km, balloons at 30-35 km at ~750-800 km)
- Terrain: AWS Terrain Tiles (Mapzen terrarium PNG, `elevation-tiles-prod`
  on S3, CORS `*`, max zoom 15; SRTM / GMTED2010 / ETOPO1 + national models,
  e.g. LINZ 8 m in NZ), fetched BY THE BROWSER with `cache: 'force-cache'`
  (S3 sends no Cache-Control) - kraken_doa is not involved. Decoded by the
  worker's own PNG decoder (DecompressionStream + unfilter), NOT a canvas:
  anti-fingerprinting canvas noise (Brave, Firefox resistFingerprinting)
  would flip the R channel's low bit = 256 m
- Worker (`vsWorkerMain`, started from a Blob of its source; only its own
  scope runs there; `self.vsTest` exposes its functions to node tests):
  one job at a time. A job first loads the tiles it doesn't hold in memory
  (6 at a time; the progress counts only those - "loading terrain k/n",
  then "computing"), then cleans the tiles that need it (below), then
  computes ring by ring. Decoded tiles stay in memory (`CACHE_MAX` 256 =
  64 MB, Float32, each with its clean-up's fixes; a job marks its tiles
  used, and after it the least recently used go first, never the tiles
  that job used): a moved point or a
  changed height / k loads only the tiles it hasn't got (a 3 km move at
  1000 km: 16-21 of ~200). It was an LRU of 80 - smaller than one 300 / 1000 km
  job (150-200 tiles) read in the same order every time, so it never hit
  and every update decoded everything again (~2.3 s at 1000 km; the
  download itself was always the browser's HTTP cache). `{cancel: job}`
  stops one while loading (between tiles) or between rings: a
  `setTimeout(0)` before each ring (and every 0.1 s of cleaning) lets the
  message in - with every tile in memory nothing else waits, so a dragged
  point's stale job would run to its end
- RINGS (`vsPlan`, the job's `levels: [{z, r0, r1}]`): zoom `VS_ZFINE` 14
  around the antenna (~8 m cells at 37 deg, to ~3.8 km), each next ring one
  zoom coarser (`VS_ZSTEP` 1) out to about twice the distance, each grid
  <= `VS_NMAX` 1025 cells a side (radius measured at the poleward edge):
  cell / distance stays 1/512..1/256, so terrain is seen at about the same
  angle everywhere. 30 km = 4 rings (z14-11, 8-61 m), 300 km = 8 (z14-7,
  8-978 m), 1000 km = 10 (z14-5, 8 m - 3.9 km). A ray of ring k starts at
  r0 with the slope carried in from ring k-1 (`hzIn`: its rays' slopes at
  r1 by start bearing; every ring has the same `nRays` = 8 x the largest
  half-size, so they line up), skips everything inside r0, assigns cells
  to rExt = r1 + 0.75 of the next ring's cell (overlap, no gaps) and
  returns its slopes at r1. The antenna's ground = the z14 ring's.
  `ringGrid()`: the grid (half-size from the poleward edge's cell size)
  and the tiles touching the ring by great-circle distance to their edges
  (sampled every 8 px; not those inside r0). WHY: the first
  version used ONE grid for the radius (~400 m cells at 300 km): the hill
  next to the user's antenna (1.2-2 deg high to the NE) was averaged away
  and the antenna stood on a 400 m average, so aircraft at 10 km looked in
  sight at 300 km where they need 11.5 km. Against a high-resolution ray
  reference (scratchpad vs/ref_rays.py: z14 to 10 km, z13 to 40, z12 to
  160, z11 beyond) at the user's station, 14 bearings x 9 distances, the
  lowest altitude in sight: one grid mean error 74 / 415 / 1243 m (0-30 /
  30-120 / 120-300 km, 50 points "visible" > 200 m too low); rings 1 / 9 /
  33 m, none
- Cost: 30 km = 90 tiles, 7.6 MB (Denver 8.7), 300 km = 171 tiles, 15 MB,
  1000 km = 194 tiles, 16.8 MB; the first download ~6-15 s. Compute (Pi 5,
  node, Auckland) for 30 / 300 / 1000 km: the first time (tiles decoded +
  cleaned) 2.2 / 4.9 / 5.9 s, again (same place, new height / k) 1.1 /
  1.8 / 2.1 s - the sweep 0.9 / 1.5 / 1.8 s of it. Mountains clean longer
  the first time (Dolomites 1000 km: 9.6 s, 5.9 of it the clean-up of 189
  tiles; again 2.2 s). A point moved at 1000 km (5 places, downloads
  cached): 3 km 2.3-2.5 s, 0.5 km 1.9-2.2 s. History: 9-28 s before the
  tile memory and the faster passes (every tile decoded again, every pass
  a full scan), then 4.8-8.6 s, then ~5-11 s -> 2-2.5 s with the
  interpolated sweep + per-tile clean-up (t_cmp.mjs; the first run in
  mountains ~5-10 % slower than cleaning the grids: whole tiles + edges)
- Terrain clean-up (`despike(raw, N, cellM)`, then <= 0 m -> 0 - the tiles
  hold the sea FLOOR; land below sea level also becomes 0, Dead Sea /
  Caspian basins are wrong). The tiles have resampling artefacts where
  sources meet: Auckland coast z11-12 (+948 next to -1819 m, 100-500 m
  pillars), a seam on the Colorado plains (912-1610 m in 1377 m flat), the
  Rockies (-218 m next to 2520 m), and a BAND 2-4 rows thick at -36.12 deg
  in the sea north of the Coromandel (NZ), zooms 8-13, up to 32767 m (100-
  150 km NE of the user's station). Rules: > 9000 m = invalid (5x5 median
  of the valid ones); then up to 8 passes (until nothing changes): a cell
  with |e - mean of 8| > 25 m that stands out from the 24 around it (5x5,
  clamped values) > 40 m, > 6 MAD + 40 m and > 1.5 cells' worth of height
  (at coarse zooms an island / headland / cone is 1-3 cells on flat ground:
  zoom 8 around Auckland flattened 271 real ones without it), or - cells
  <= 100 m - with the resampling overshoot the other way in its raw 3x3
  (below the median by half its height); or standing > max(500 m, 4
  cells) and > 6 MAD + 40 ABOVE the 12 cells of its column or row (+-6:
  the band hides from the 5x5; lower thresholds cut a real 240 m ridge in
  Colorado and, for pits, filled a real gorge). Checked on 742 tiles: no
  cell of the clean LINZ z14 tiles, Everest, Matterhorn, Mt Cook, El
  Capitan, Dolomites, Dead Sea is touched; what goes is the band, coast
  artefacts (982-1314 m "peaks" around Auckland, max real ~700 m) and
  seams. Tiny islets / sea stacks of 1-3 cells at fine zooms can go too.
  Run PER TILE (`clean(z, x, y, t)`): the tile + `PAD` 16 pixels of its 8
  neighbours (those in memory; a missing one = the tile mirrored at that
  edge), the tile's own latitude for the cell size; kept as the cells it
  changed (`t.fix` {i, v}) and the neighbours it had (`t.nb`, bit (v+1)*3
  + u+1). A job cleans a tile only when it has no fixes yet or a neighbour
  that is in this job wasn't there then (`nbMask`): a 0.6 km move at 1000
  km cleans ~5 tiles, 3 km ~45 (16 new + those next to them), the same
  place again none. The rings are put together from raw tiles + fixes.
  Checked (t_worker 7): 133 cached tiles with all neighbours, incl. the
  Coromandel band, give exactly the cells of despiking their 3x3 mosaic at
  once (PAD 8 left 220-340 m of a band's edge in the sea: a band clears
  from the side over several passes); with a neighbour missing, the cells
  > 2 PAD from that edge are unchanged; at 5 places x 1000 km the terrain
  the rays use is identical to cleaning each ring's grid (what it did
  before: every job, every grid - and the grids held zeros inside each
  ring's hole, so cells next to it were "fixed" too: the most of the
  Dolomites' 120 / Colorado's 324 spikes, all in cells no ray reads).
  Faster passes (same result): the passes after the first test only the
  cells a fix of the pass before can reach (the 5x5 around it, +-6 along
  its row and column) - each pass was a full scan, and in mountains a
  few cells keep changing for 5-7 passes (Dolomites 1000 km: 12.4 s ->
  4.3 s); the MAD is only worked out when the test gets that far
- Sweep of one ring along GREAT CIRCLES (the path a line of sight takes).
  The first versions stepped straight lines on the map (R2) = rhumb lines:
  at 300 km their start bearing is ~0.7 deg off the great circle's to the
  same place, at 1000 km ~2 deg, and mid-way they run up to ~15 km beside
  the true path (at the user's station, 300 km to the NE: 11483 m needed
  along the rhumb line, 11762 m along the great circle). `nRays` rays at
  even start bearings (<= 0.8 cell apart at the ring's edge), each walked
  from r0 in steps of 0.4 of the ring's smallest cell: the position on the
  sphere by rotation (p, q unit vectors; no drift over 3000 steps), -> cell
  coordinates by atan2 longitude (wrapped around the observer's - the
  antimeridian) and atanh Mercator latitude - exactly every K = 8 steps,
  linear between (a great circle bends on the Mercator grid by <= 1 /
  radian, so over 8 steps, <= 3.2 cells, it leaves the line by < 0.001
  cell at zoom 5; a jump - the longitude wrap, a pole - falls back to
  every step exactly). The two transcendentals were most of the sweep:
  3.5 -> 1.8 s at 1000 km; vs every step exact, 0.014 % of the cells'
  `need` moved > 1 m and none changed in sight at 2 m - 10 km; terrain
  bilinear; each cell takes the sample nearest its centre (`best`, u8 of
  the squared offset); a cell no sample landed in (>= 3 of 4 neighbours
  have one) takes their mean. Distance = the exact great-circle distance.
  Curvature d^2 / (2 k R), R 6371 km, k 4/3 radio / 7/6 light / 1
  (`vsState.k`; vs the exact form 100 m at 1000 km). Output
  per cell: `need` = the height a target needs above the ground to be in
  sight (u16: 0.1 m steps to 3 km, then 10 m; 65535 = not this ring) and
  `ground` (i16 m, the ray's interpolated ground) - so the target height,
  above ground (`need <= t`) or above sea level (`ground + need <= t`,
  aircraft), repaints at once (`vsPaint`) without recomputing
- Page: `vsState` (globals block; localStorage `kraken_vs`: tgt = ONE
  target height (default 2 m) that `amsl` reads as above ground or above
  sea level - switching keeps the number (it was a value per reference, so
  the box jumped to another number; an old {agl, amsl} pair loads as the
  one that was shown), amsl, k, h / r = what new ones get (the last used), items
  [{id, name, lat, lon, h, r, col, on, st = the station, mk = from a
  marker}] - rebuilt field by field on load, a damaged entry is dropped),
  `vsRes` (results by item, key = lat, lon, h, r, k; `levels` = the rings,
  each with its canvas), `vsFail`. Jobs start from `vsDraw` (so after a
  reload only with the map shown). Drawing: each viewshed's rings coarse ->
  fine, opaque, onto one layer (`vsOff`, map-sized), then that layer at
  alpha 0.44 onto the map - the rings' overlapping edges don't tint twice.
  `% in sight` by area (cells weighted by their size). Hover: the finest
  ring with data there. A hidden item frees its canvases; ImageData only
  while painting (phones). `vsReady` (globals block, set at the section's
  end) guards vsDraw / vsRender: rpApply can draw the map before the
  section's consts exist. Panel `#map-vs`
  in `.map-tl` (under the top-left buttons, with the masts legend): target
  + above ground / sea level + refraction, a row per viewshed (show / hide
  swatch, ⌖ fits its circle, ✕, Antenna / Radius - a change recomputes,
  "% in sight", tooltip: ground + antenna, cell size, zoom, time, spikes),
  the readout under the pointer (a tap
  on touch): ground height, per viewshed in sight / "hidden - needs N m
  above ground (or sea level)", distance + bearing from it
- The station's viewsheds (`st`) FOLLOW the station (`vsFollowStation`,
  from every map push and from the drags below): moved > 20 m -> new
  position + recompute; with a GPS / phone source at most every 30 s
  (driving would recompute without end). A point's viewshed (not the
  station's) is selected by a click on its dot (white ring) and dragged
  (`vsMoveItem`; from a marker: the `mk` link is dropped). While dragging,
  a 0.4 s pause updates (the running job is cancelled between rings,
  ~0.6 s), the release does. See *📡 Station on the map* below
- Tests (scratchpad vs/ + t_vs_ui.py): `node t_worker.mjs [--net]`: PNG
  decoder = PIL on 27 tiles; despike = the numpy reference
  (ref_despike2.py; scan_despike2.py runs it over every cached tile) incl.
  the band; flat sea: hidden height = (D - sqrt(2kRh))^2 / 2kR within
  0.12 m at 1080 points around 48.5 N (synthetic worlds built on the
  sphere with `ringGrid`), a 10 m target from 100 m in sight to 4.12
  (sqrt 100 + sqrt 10) km; a flat-topped wall (no curvature): 198.8 m
  needed 6 km behind (10 + 90 x 6 / 2.83-2.87 km); two rings (19 m / 76 m)
  = one 38 m grid at 60 N (median 0, worst 7.7 m; 16 km behind a 60 m
  hill 2 km out 430 vs 430 m - 1 m without the carried horizon); real
  terrain (Auckland 30 / 300 / 1000 km, Mt Cook, Zermatt, Denver 80 km,
  across the antimeridian in Fiji, Tromso 600 km at 69.6 N) vs a brute-
  force great-circle line of sight per cell through the ring holding each
  step: same in-sight answer for 99.5-100 % of 2000 cells each; offline /
  HTTP 404 / garbage tile / cancel; tiles in memory (same place again: 0
  fetches, same result; a 2 km move loads 13 of 154; a cancel while
  computing stops the job after the ring it is in; trimmed to CACHE_MAX
  keeping the last job's tiles; least recently used dropped first - 30 km
  jobs A, B, C, A, D: A still all in memory); per-tile clean-up = the 3x3
  mosaic's (above). t_pole.mjs: 84 N / 83 S at 1000 km - ~0.1 % of the
  sweep's 8-step chunks fall back to exact steps (past a pole), no odd
  values. t_cmp.mjs <old copy of the page>: old vs new worker on real
  terrain at 5 places (terrain / need differences, in-sight flips at 2 m
  - 10 km) and the time first / again / after moves (t_equiv.mjs: the
  same, for changes that must be exact). ref_rays.py (great circles; z10 past
  400 km) + dump_run.mjs + compare.py: at the user's station, 8 bearings x
  11 distances to 995 km, mean error 1 / 6 / 22 / 107 m (0-30 / 30-120 /
  120-400 / 400-1000 km; out there 14-93 km are needed and a 3.9 km cell
  spans ~470 m of that). t_vs_ui.py (headless Chromium against the
  test build, real tiles): button, station auto-add, progress, hover,
  target / sea-level repaint, right-click, radius change (rings 14-12 for
  10 km), hide / show, ⌖, k change, marker + mast, cancel by removing, the
  limit, reload, station moved, damaged localStorage, 300 km (8 rings, no
  seams in the screenshot), 1001 km refused, 1000 km (10 rings, 194 tiles;
  a 30 km balloon in sight to ~590-700 km)

