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
  (*Tile providers* below), picked in the Layers popover (*Layers popover*
  below), attribution shown. Tiles: cache
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
  `#map-pane`. `mapToggle()` (the rail's button, the pane's ✕; `mapToggle(true)`
  = "show it on the map": the decoder tab's "Show map" and every feature
  that puts something on the map) / `rpShow('doa'|'map'|'both')` (header
  tabs, coherent only) -> `rpApply()`: body classes `map-on` (the pane
  shows), `rp-off` (coherent mode, the pane hidden), `rp-map` (the map is
  what shows - always outside coherent) and `rp-both`.
  **The rail's button is "DoA & Map" and shows / hides the whole pane
  (2026-11-17, the user's wish; it was "Map" and switched only the map,
  MUSIC DoA always showing).** Two stored flags, because the pane is two
  different things: in coherent mode it is MUSIC DoA / Map / Both -
  `mapState.pane` (default true) says whether it shows, `mapState.tab`
  (default 'doa') what, and the tabs are always there while it shows; hidden,
  MUSIC DoA is hidden too and the spectrum takes the room. Outside coherent
  mode the pane is only the map, shown while `mapState.on` (default false:
  no map until asked for), and the button's label drops "DoA & " (a
  `.coh-only` span). `mapState.on` also means "the map is wanted": picking
  the Map / Both tab sets it. `rpOpen()` = the pane shows in this mode;
  `mapIsOn()` = `on` and `rpOpen()` - what "is the map open" checks use
  (`mapState.on` alone is true while a coherent pane is hidden). A state
  stored before `pane` existed is read as pane shown, on the DoA tab if the
  map was off.
  **Width: one for every tab** (2026-11-17, the user's wish - the MUSIC DoA
  tab was a fixed 280 px and the pane jumped when the tab changed):
  `--map-w` (`rpApplyWidth`, `mapState.w`; 42 % of the room until dragged,
  260 px .. the room less 420 px for the spectrum), `#rp-resizer` on its
  left edge whenever the pane shows (drag; double-click = default). The
  DoA plots fill the pane: `#doa-displays` is a grid of equal columns (one
  per VFO while each gets 240 px, else they stack), and a plot is as large
  as its column and the pane's height allow (`.doa-content` is a size
  container, the plot's `max-width` is `100cqh - 118px` = the card's other
  lines; without container units it stays 250 px). Desktop layout only - in
  the phone layout the pane has no height of its own and nothing changed.
  The plots are drawn at the size shown: `PolarPlot.fit()` /
  `ElevationPlot.fit()` (from a ResizeObserver per display) set the bitmap
  to the canvas's size on screen x devicePixelRatio and scale the drawing
  code's fixed 250-unit space with a transform, so a large plot is sharp. The calibration greying applies to `.doa-content` only.
  localStorage `kraken_map` {on, pane, tab, w, layer, z, mx, my, trails} (the
  provider: `kraken_map_prov`, below); first
  open centres on the station (else the world) and fits the first markers
- Tests: scratchpad mock-server test t_map (offline tiles forced by
  pointing the layers at a closed port) and an end-to-end run of the real
  kraken_doa (from a scratch dir - its own doa_settings.json) fed by a fake
  heimdall streaming a synthetic 1090 MHz recording

**Drive mode (2026-11-18, *drive-mode.md*):** this map, full-window
(`#map-pane` fixed over the hidden page), with a bearing readout and big
buttons over it. `rpOpen()` / `rpApply()` read `driveOn` (the map shows in
every mode; `mapState.on / pane / tab` untouched); `mapSave` keeps the
stored view and DF selection while it is on (`driveMapJson`); the pan /
zoom code turns its Follow off / stops its zoom-out; clicks select
nothing and the menu does not open. Its Layers are `mapLayHtml(['base',
'df'], 'drive')`.

**Layers popover (2026-11-18):**
- The user asked to give the map the View popover's pattern: the layer and
  DF buttons (the base map's segment, Trails, Masts, Viewshed, Replay at the
  top left; Lobe, Lines, Heat, Range in the DF card's header) sat on top of
  the map. Now the map's top left holds one button, **Layers**
  (`#map-lay-btn`, the `layers` icon and the word, the View button's look
  `.view-btn`, 26 px tall like the map's buttons), and they are rows of its
  popover (`#map-lay-pop`, `.view-pop.map-lay-pop`, `role="dialog"`, its
  button `aria-haspopup` / `aria-expanded`; lit while open). Groups, in the
  View popover's style (small caps headings, one row each, the control at
  the right): **Base map** - the provider's layers as a radio list (the
  picked one's name in the accent colour; the old segment's tooltip);
  **Show** - Trails, Masts; **Direction finding** (`.coh-only`) - Lobe,
  Lines, Heat (`mapState.rdfLobe / rdfLines / rdfHeat`); **Tools** (a panel
  of their own) - Viewshed, Replay, Range (Range `.coh-only`), a switch each,
  on while its panel is open (`vsToggle`, `lrToggle`, `rgToggle`). Each row's
  `title` is the text of the button it replaced. A row is a `<label>`: a
  press on the name works the control.
- **What stayed on the map**: the navigation column at the top right
  (search, zoom, home, the estimated transmitter, fit all) - used while
  looking at the map, one press each - and the DF card's live part: its
  toggle (folds the card), the VFO and talker selectors, the body (status,
  SFN profile, talker timeline, Collect, range km, centre, Reset). The card's
  header is one line now (`.map-df-h` no longer wraps; its selectors give
  way in a narrow pane).
- **Code** (🗺 MAP, after `mapUpdateControls`): `mapLayHtml(groups, scope)`
  draws the rows of any subset of the groups ('base', 'show', 'df',
  'tools') - so another place (drive mode) can show some of them; `scope`
  keeps the ids and the radio name of each copy apart. Every copy is kept
  in sync by `mapLaySync()` (inputs `data-ml` = trails / masts / rdfLobe /
  rdfLines / rdfHeat / vs / lr / rg, `data-ml-layer` = a layer key) and
  works through `mapLaySet()` (`mapSet` - so `localStorage['kraken_map']`
  as before - or the tool's own toggle). `mapLayRender()` (from
  `mapUpdateControls`, so also after Setup Settings → Map saves another
  provider) redraws the rows when the provider's layers change. The sync
  runs from `mapUpdateControls`, `rdfRenderPanel` and `rgRender`, which
  every path that changes these states already calls: the assistant's map
  tools, the map's right-click menu (viewshed sites), a talker picked in the
  DF panel (lobes on), a recording's Replay or a dropped log, the panels'
  own closes. `mapLayOn` / `mapLayRO` are in the globals block, the rows'
  table inside `mapLayHtml` (`mapUpdateControls` can run before the
  section is evaluated).
- **Placement and dismissal** are shared with the View popover and "All
  decoders" (VIEW POPOVER: `popPlace`, `popDismiss` - *sidebar.md*): under
  the button with the left edges aligned, inside the window AND the map
  pane's visible part (6 px in, as the map's buttons); with no room below
  (a short map in Both, a phone) it gets shorter and scrolls - never above
  the button (that is outside the map) - and narrower than 220 px only in
  a pane narrower than that. Placed again when the map pane changes size
  (a ResizeObserver: the pane dragged, Both's divider) and on a page resize
  event; closed by the button, Esc (focus back on the button), a press
  outside it and its button, a change of the window's size, a scroll of
  what holds the button (the phone's page) and the map being hidden (the
  button has no box any more). Not modal: a switch acts at once and the
  popover stays open. A press on a `?` help-mode button keeps it open; in
  help mode a tap on a row shows its tip and does nothing (the page's help
  code; press and hold works on the rows, they are not the canvas).
- **Viewshed and Range got a close cross** (`.map-vs-h .x.cl`, right of
  their fold): their buttons were one press away on the map, the switch is
  two; the cross closes the tool in one (`vsToggle` / `rgToggle`), the
  Replay strip already had one (`lrClose`). The switches follow all three.
- **Phone**: the button 32 px tall, the popover `min(320px, 100vw - 16px)`
  wide (inside the map pane), rows 42 px, 20 px radios, the switches at
  their touch size.
- Tests (scratch `test_layers.py`, headless Chromium with real mouse, key
  and touch events, 138 checks): the removed ids nowhere in the source; the
  top left holds only the Layers button (icon, word, aria, the right-hand
  column's height); the DF card's header one line (with the talker
  selector too, in a 260 px pane), its fold, VFO selector, Collect; the
  popover under the button inside the map pane and the window, on top, at
  1700 / 1300 px, with the pane dragged to 260 px, in Both with a short
  map (it scrolls; placed again when the divider moves) and on a 400 px
  phone (as wide as allowed, 42 px rows); the groups, rows, titles, one
  column for the controls; a base map picked (state, storage, the tiles
  redraw), also after Setup Settings saved Stadia Maps (four layers) and
  the custom provider (one); each switch by its control and by its name,
  stored, following a change made elsewhere; Viewshed / Replay / Range open
  and close their panels and follow the panels' own closes and toggles
  from code; every dismissal path, Tab into it, Enter; help mode and press
  and hold; View and Layers closing each other; independent mode without
  Direction finding and Range; no JS errors. `test_page.py` opens the
  viewshed and the replay through the popover now, `test_merge.py` counts
  the base map rows.

**Tile providers (⚙ Setup Settings - "⚙ Settings" until 2026-11-17 -, 2026-10-25):**
- The top bar's ⚙ (`#settings-btn`, at the right end, as tall as the chips' and the telemetry's lines;
  on the phone's bar on the status chips' line - *sidebar.md*, "Top bar and
  rail") opens ⚙ Setup Settings (`#set-dlg-bg`, the saved-frequency dialog's
  `.sf-dlg` look; "⚙ SETUP SETTINGS" section of the page; its tabs - *sidebar.md*,
  "Setup Settings dialog"). Its 🗺 Map tab picks
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
  `location.origin`). Git backstop: `.githooks/pre-push` blocks a push
  that adds a recognisable map key to any file - not every provider's key
  can be told by its shape (`docs/dev/run-and-deploy.md`, *Git Secret
  Guards*, lists what is caught) - so keep real keys out of tests and docs.
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
  Tiles are `<img>` loads without a readable status, so the viewport
  request is the session's probe: HTTP 400 / 401 / 403 / 404 = Google
  refused the session before its expiry (revoked, wrong clock) ->
  `gmReject()` drops it and the next draw makes a new one - at once the
  first time, then only after 15 s doubling to 5 min (`rej`, reset by an
  accepted viewport request), so a bad key can't loop `createSession`.
  Save in ⚙ Setup Settings discards every stored session (re-saving the same
  key is a real fresh start).
  Attribution: "Google Maps" (the policy accepts the text where the logo
  doesn't fit) + the `copyright` of `tile/v1/viewport` for the view.
  `maxZoomRects` sets the view's zoom limit (`mapDynZ`, read by
  `mapLayerMax()`; `mapZoomFor()`): the rects can nest, so the deepest
  zoom of the rects holding the view's centre, else (none holds it) the
  shallowest of all; applied only if the reply is still for the view
  shown, a change redraws at once (tiles at the new limit), reset when the
  provider / layer changes or Settings are saved.
- Azure: `map/tile` 2024-04-01 (`microsoft.base.road`, `microsoft.imagery`
  + `microsoft.base.hybrid.road` labels, `microsoft.base.darkgrey`,
  `tileSize=256`, `subscription-key` in the query); attribution from
  `map/attribution` for the view (its HTML links reduced to text with
  DOMParser, then escaped); a view across the antimeridian (west > east)
  asks with bounds -180..180. Until (or whenever) that is missing the
  layers show a static "© Microsoft, © TomTom" (`attrFb`: the layer's
  `attr` stands in for the fetched credit and is dropped once it is there).
- `mapAttrDyn()`: providers with `attrq` (Google, Azure) are asked for the
  view's attribution 1 s after the view stops changing (key: provider,
  layer, the view's zoom - not the limited one, so a limit that arrives
  doesn't ask again - bounds to 0.01 deg); the result shows until the next
  one.
  A failed fetch is retried after 15 s doubling to 5 min even if the view
  stays (`mapDynRetryMs`, also a test hook). A reply applies only if it is
  for the view wanted and the provider generation `mapDynN` (bumped by
  Save, which also clears the pending timer) - no late write from the old
  provider. The state (`mapDynAttr`, `mapDynWant`, `mapDynT`, `mapDynZ`,
  `mapDynN`, the retry, `gmSess`) is in the globals block.
- The offline notice names the provider's key as a possible cause
  (or Google's session error) when tiles fail.
- Custom `http://` URLs: an https page can't load them (mixed content;
  only loopback - localhost, 127.x, [::1] - is exempt), so
  `mapMixedBlocked(url, protocol)` makes Save / Test refuse them with that
  reason when the page is https, and a stored one requests nothing and
  shows the reason in the offline notice at once.
- The dialog's Test is invalidated (`setDlgTestN`) by a provider change and
  by any edit of the key or a custom field (`setDlgEdit()`), so a late
  result can't show under settings it didn't test.
- Test: scratchpad t_mapprov.py - headless Chrome over CDP, every outside
  request intercepted (tiles answered with a generated PNG, createSession /
  viewport / Azure attribution mocked, a 401 for a bad Mapbox token): the
  cog's place, every provider's URLs, key encoding, Save validation, Test
  messages, session reuse after a reload, escaped attribution, the custom
  template, junk ids in localStorage, phone width; and (review fixes) the
  zoom limit redraw / stale reply / rect rule, a refused session renewed
  and rate-limited, Save discarding sessions, Azure's static credit +
  retried fetch + antimeridian bounds, no request to the old provider
  after Save, the mixed-content check (via its protocol parameter - the
  test page is http), Test invalidation, the globals

