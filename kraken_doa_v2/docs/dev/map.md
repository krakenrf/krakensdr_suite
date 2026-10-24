# Map (right-hand pane) and decoder map points

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🗺 Map (right-hand pane) + decoder map points:**
- Plugins report positions with `host.map_point(kp::MapPoint)` / `map_remove`
  (kraken_plugin.hpp; `kp::Info::map = true` makes the decoder tab offer
  "🗺 Plot on map" - also shown once a decoder has points; off until the
  user ticks it). Stored per decoder in its `Report` (above), sent only
  while ticked. Reporting today: adsb (aircraft), aprs (stations / objects /
  items, marker from the APRS symbol, course / speed from `ccc/sss`, killed
  objects removed, ttl 1 h), dmr (GPS info LC -> the talking radio: id =
  radio ID from the slot's voice LC, label = talker alias), dstar (GPS
  slow data: NMEA RMC / GGA with a valid checksum, DPRS with a valid
  CRC-16/X.25 -> the MY callsign / DPRS callsign). The lib decoders use
  `dig::Report::map()` / `station()` (dig_common.hpp)
- `MessageBuilders::build_map_message()`, 1 Hz on TOPIC_CTL (websocket_server
  map_timer, sent even when empty - it carries the station):
  `{"map":{"full":bool,"station":[lat,lon]|null,"pts":[{v,p,i,la,lo,l,k,h,
  a,s,x,age,ttl}],"keys":["vfo|plugin|id",...]}}` - pts = the points
  updated since the previous push (per-VFO map seq, like the digital
  events), keys = every live point of every decoder that is on with
  "Plot on map" on: pages drop markers whose key is missing (removal,
  expiry, Plot on map off, decoder off). `request_map_full()` (GET_MAP from
  a page opening the map, DIGITAL_OPT map) makes the next push carry all
- UI (kraken_doa.html, "🗺 MAP" block; state `mapState` etc. in the globals
  block because `applyOpModeUI` -> `rpApply` can run before that block is
  evaluated): own canvas slippy map, NO map library - the page must work
  without internet (uPlot is inlined for the same reason). Web Mercator,
  fractional zoom (tiles of the nearest integer zoom scaled) MAP_MIN_Z 2 -
  MAP_MAX_Z 22: past a layer's deepest tiles (`max` 19) those are scaled up
  (z22 ~3 cm/px, 8x); range rings down to 5 m, graticule to 0.00001 deg
  (decimals from the step), drag / wheel
  / pinch / double-click, 🔍 (search - *🔍 Map search* below) +/− 🏠
  (station, was ⌖) 🎯 (the estimated transmitter) ⤢ (fit all), Street (OSM
  tile.openstreetmap.org) / Satellite (Esri World_Imagery + the
  World_Boundaries_and_Places label layer) - or another provider's layers
  (*Tile providers* below), attribution shown. Tiles: cache
  of 400 (LRU), a failed tile retried after 15 s doubling to 5 min, a missing
  tile drawn from a loaded lower-zoom tile; underneath always a lat/lon
  graticule, so with no internet the markers sit on a plain grid + an
  "offline" notice (tile errors in the last minute and no tile loaded for
  30 s). Station = yellow mast + range rings (step to suit the zoom; the
  mast can be selected and dragged - *📡 Station on the map*).
  Markers by `kind` (aircraft rotated by heading, vehicle, ship, station,
  person, point) in the VFO's colour, label + altitude, greyed after 30 s
  without an update; trails (client-side, last 15 min / 400 points);
  click = popup (the plugin's "key: value" info lines + position, distance
  / bearing from the station, age); status line = counts per VFO decoder
- Right-hand pane: `.doa-panel#right-pane` holds the DoA displays and
  `#map-pane`. `mapToggle()` (🗺 Map button under the waterfall, ✕, the
  decoder tab's "Show map") / `rpShow('doa'|'map')` (header tabs, coherent
  only) -> `rpApply()`: body classes `map-on` (pane shown in every mode)
  and `rp-map` (map tab visible - always outside coherent); width
  `--map-w` (42% default, `#rp-resizer` drag on its left edge, double-click
  = default). The calibration greying applies to `.doa-content` only.
  localStorage `kraken_map` {on, tab, w, layer, z, mx, my, trails} (the
  provider: `kraken_map_prov`, below); first
  open centres on the station (else the world) and fits the first markers
- Tests: scratchpad mock-server test t_map (offline tiles forced by
  pointing the layers at a closed port) and an end-to-end run of the real
  kraken_doa (from a scratch dir - its own doa_settings.json) fed by a fake
  heimdall streaming a synthetic 1090 MHz recording

**Tile providers (⚙ Settings, 2026-10-25):**
- The top bar's ⚙ (after the status badges, next to the scanner's "Idle")
  opens the Settings dialog (`#set-dlg-bg`, the saved-frequency dialog's
  `.sf-dlg` look; "⚙ SETTINGS" section of the page). Its 🗺 Map part picks
  the provider and takes its API key: OpenStreetMap + Esri (default, no
  key), Mapbox (public token, `styles/v1` raster tiles, 512 px), Google
  Maps (Map Tiles API), Azure Maps, MapTiler, Thunderforest, Stadia Maps
  (key optional: domain auth), HERE (Raster Tile API v3), Custom (an XYZ
  template: `{z} {x} {y} {-y} {s} {key}`, deepest zoom, dark flag,
  attribution text - escaped). Test loads one tile (on failure a `fetch`
  of it reads the server's error where CORS allows, e.g. Mapbox's "Invalid
  Token"); for Google it creates a session (Google's error says what's
  wrong: key invalid, API not enabled). Save refuses a missing key or a
  non-http(s) URL; Escape / ✕ / a click outside cancels, Enter saves.
- Per browser, deliberately not in the receiver: localStorage
  `kraken_map_prov` {p, keys: {provider: key}, cu: {url, max, attr, dark}}
  - every provider's key is kept, so switching back needs no retyping.
  The key is only in the tile URLs the browser fetches; it never reaches
  kraken_doa, `doa_settings.json` or other browsers. Referrer / origin
  restrictions on a key must allow the page's address (the dialog shows
  `location.origin`).
- Code: `MAP_PROVIDERS` (map section) - each provider's `layers` (Street /
  Satellite / Terrain / Dark ...; the keys are shared, so `mapState.layer`
  keeps "satellite" across providers and a provider without it shows its
  first layer: `mapLayerKey()`), `mapProvId()` (falls back to OSM if the
  key is missing), `mapLayer()`, `mapProvCtx()` (the URL-encoded key).
  Layer flags replace the old `'street'` / `'satellite'` tests: `light`
  (rings drawn dark), `bg`, `osm` (credits OSM itself: no extra OSM
  attribution for masts / addresses), `tile: 512` (fetched from the zoom
  below; the lower-zoom fallback crops by `naturalWidth`), `labels`
  (overlay: Esri, Azure). A url function may return null (not ready: no
  tile drawn, `mapDraw()` again when it is).
- Google: `createSession` (POST, mapType roadmap / satellite + layerRoadmap
  = hybrid / terrain + layerRoadmap; language / region from
  `navigator.language`) per map type, stored in localStorage
  `kraken_map_gs` with a hash of the key so a reload reuses it (and the
  browser's HTTP cache of the tiles); renewed 10 min before `expiry`;
  failures back off 15 s doubling to 5 min and show in the offline notice.
  Attribution: "Google Maps" (the policy accepts the text where the logo
  doesn't fit) + the `copyright` of `tile/v1/viewport` for the view;
  `maxZoomRects` sets the layer's deepest zoom (`L.zmax`).
- Azure: `map/tile` 2024-04-01 (`microsoft.base.road`, `microsoft.imagery`
  + `microsoft.base.hybrid.road` labels, `microsoft.base.darkgrey`,
  `tileSize=256`, `subscription-key` in the query); attribution from
  `map/attribution` for the view (its HTML links reduced to text with
  DOMParser, then escaped).
- `mapAttrDyn()`: providers with `attrq` (Google, Azure) are asked for the
  view's attribution 1 s after the view stops changing (key: provider,
  layer, zoom, bounds to 0.01 deg); the result shows until the next one.
- The offline notice names the provider's key as a possible cause
  (or Google's session error) when tiles fail.
- Test: scratchpad t_mapprov.py - headless Chrome over CDP, every outside
  request intercepted (tiles answered with a generated PNG, createSession /
  viewport / Azure attribution mocked, a 401 for a bad Mapbox token): the
  cog's place, every provider's URLs, key encoding, Save validation, Test
  messages, session reuse after a reload, escaped attribution, the custom
  template, junk ids in localStorage, phone width

