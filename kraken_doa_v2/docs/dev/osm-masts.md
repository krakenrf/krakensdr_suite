# OpenStreetMap masts layer

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📡 OpenStreetMap masts layer (UI only, kraken_doa.html "📡 OSM MASTS"):**
- "Masts" button (`mapState.masts`, localStorage like the other view
  settings) + a colour legend (`#map-legend`). The BROWSER fetches from
  OpenStreetMap's Overpass API (`MAST_URL` overpass-api.de, CORS `*`), like
  the tiles - kraken_doa is not involved. overpass-api.de answers 406
  without a Referer (browsers send one: the page has no referrer policy -
  keep it that way) and OFTEN 504 under load
- Query: `nwr` man_made=mast (tower:type != lighting), man_made=tower +
  tower:type=communication, man_made=antenna in the bbox; `out tags center
  qt 5000`; `[timeout:25][maxsize:67108864]` (a small maxsize is admitted
  more often than the 512 MiB default under load)
- overpass-api.de RATIONS queries per connection (`/api/status`: "Rate
  limit: 2", a slot blocked for a while after each query; 429 beyond) - the
  first version loaded view by view, got 429 after a few pans and only ever
  showed the area loaded first ("masts only within a radius of me"). Now,
  in zoom-12 cells (`mastCells`: "x,y" -> load time), one query at a time:
  1. the REGION: MAST_REGION_KM 150 each way around the station
     (`mastRegionCells`; no station 5 s after the first try: around the
     view, z >= 8) in ONE query ([timeout:60], abort 80 s; NZ test: 1301
     masts in 8 s). 5000 answers = too dense: `mastRegionKm` halves (< 20
     = no region). Re-centred when the station moves a third of it.
     After 2 failed region queries the view goes first
  2. elsewhere the view's missing cells (+1 around) at zoom >= MAST_MIN_Z
     11, 0.7 s after the view stops changing ([timeout:25], abort 45 s; 5000
     answers = shown, cells not marked, no new query of that bbox below zoom
     + 1: `mastDense`)
  `mastSchedule()` runs after every draw but restarts the countdown only
  when the VIEW changed (`mastViewKey`; reset while the layer is off) -
  redraws of the same view (DF status 2 Hz, map push 1 Hz) restarted it
  forever. Errors: 429 / 5xx / no answer / 200 with an error `remark`
  (query cut short - its cells are NOT marked) = busy; fetch rejected = no
  internet; retry after 15 s doubling to 5 min (`mastRetryAt`, shown in the
  status line). Memory cleared above MAST_MAX 30000 masts
- Loaded masts are drawn at ANY zoom (dots below 13); status adds "zoom in
  to load other areas" below zoom 11 when the view has cells not loaded.
  localStorage `kraken_masts` {v:1, cells:[[key, t]], masts:[[type, id, lat,
  lon, tags]]} (saved 2 s after a load, dropped if > 3.5 M chars; read on
  the first draw): cells older than MAST_KEEP_DAYS 7 and their masts are
  dropped, so a reload asks Overpass nothing (NZ region: 266 k chars)
- Third-party data: elements kept only with type node / way / relation and
  an integer id (key `n123` goes into onclick), every tag escaped
  (`digEsc`). Category (`mastCat`, colour): broadcast (communication:
  television / radio / broadcast / dab) > mobile (mobile_phone, gsm, umts,
  lte, 2G-5G, nr) > microwave > amateur (amateur_radio[:repeater] - not
  the POTA / SOTA award tags) > not stated
- Drawn after the DF heat maps, before the station / markers: lattice
  icon (z >= 13) or dot, names at z >= 15. Click (after user markers and
  decoder points): popup in `#map-pop` (`mapSel` = "osm:<key>"; a map push
  doesn't close it) - type + construction, "Used for" (`mastServices`:
  "Mobile phone (LTE, UMTS)"), operator, height, position, distance /
  bearing, every other tag; link to openstreetmap.org; "📍 Add as marker"
  (`mastToMarker` -> `mkCreate`: name + OSM details as the notes, opens the
  editor). Attribution adds "masts © OpenStreetMap contributors" on the
  satellite layer
- Tests (scratchpad): t_masts.py (popup, legend, 504 / timeout remark /
  offline, malformed / injected elements) and t_masts2.py (region first,
  any zoom, the localStorage cache incl. expiry, too-dense region, region
  failures -> view first, off / on) - headless Chromium, real Overpass only
  for the first region load, the rest stubbed via
  `Page.addScriptToEvaluateOnNewDocument` (a fetch wrapper)

