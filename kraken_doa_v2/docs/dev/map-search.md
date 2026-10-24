# Map search, 🏠 / 🎯

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🔍 Map search + 🏠 / 🎯 (UI only, kraken_doa.html "🔍 MAP SEARCH"; the user's request 2026-10-21):**
- 🔍 (top of the right-hand buttons) shows `#map-srch` as the first item of
  `.map-tl` (under the top-left buttons; `mapState.srch`, localStorage
  `kraken_map`); the results `#map-srch-res` hang over the map
  (`.map-pane.srch-res-open` lifts `.map-tl` to z 6). State in the globals
  block (`srchQ`, `srchRes`, `srchHl`, `srchListOn`, `srchOnl` / `srchOnlQ`,
  `srchMsg`, `srchBusyQ`, `srchSeq`, `srchLastAt`, `srchCache`, `srchPin`,
  `srchTxAt`) - mapDrawNow -> `srchDraw` / `srchTxBtn` can run first
- Read OFFLINE as it is typed (`srchParse`): a map link (`srchParseLink`:
  Google `@lat,lon,zz` / `q=` / `ll=` / `query=`, OpenStreetMap `#map=` /
  `mlat`, Apple, `geo:`), lat / lon (`srchParseLatLon` -> `srchLatLonRows`:
  decimal, degrees + decimal minutes, DMS; ° ' " ′ ″ or colons or the words;
  signs or N / S / E / W before or after, lon-first when the letters say so;
  "lat:" / "lon:" words; ";" or "a,b c,d" = decimal commas; split at a
  ",", "/", the letters, the second °, or half the numbers - a single
  letter between the two is tried with either; no letters and both fit =
  two rows, lat, lon first; a text with any other letter isn't one, so
  addresses never parse), UTM / MGRS (`srchParseGrid`; UTM's "S" = band S
  (32-40° N) when the northing lands there, else / also the southern
  hemisphere: both rows; MGRS 0-5 digits, the square's centre, the row
  letters' 2000 km repeat chosen by the band, none past the pole), a
  Maidenhead locator (`srchFromLocator`, 4-10 characters). Rows get a zoom
  from the precision typed (`srchZoomFor`). Transverse Mercator =
  Krüger's series to n^6 (`srchTm`, `srchToUtm` / `srchFromUtm`; Norway /
  Svalbard zones), `srchToMgrs` truncates. Then the page's own places
  (`srchLocal`, every word must match): the station ("home", callsign),
  📍 markers (name, notes, their frequencies' names / MHz / tones), decoder
  map points, the assistant's map points, OSM masts (layer on), viewshed
  sites - picked = selected (`mkSelect`, `mapSel`), no pin
- OpenStreetMap's Nominatim (`srchOnline`, `SRCH_URL` jsonv2, limit 10,
  `viewbox` = the view, >= 0.2° across, not bounded; none below zoom 4) BY
  THE BROWSER, like tiles / masts: only on Enter / the 🌐 row (its usage
  policy: no autocomplete), >= `SRCH_GAP_MS` 1100 ms apart (a newer search
  drops the older one's answer - `srchSeq`), kept per text + the view's
  centre to the degree (`srchCache`, 100); offline / 429 / HTTP errors
  said in the list (`srchMsg`). Rows (`srchOsmRow`): name, type + address,
  bounding box (fitted - `srchFitBox`, across 180° too - else zoom 17),
  the openstreetmap.org link only for a valid type + numeric id; every text
  escaped. The first place is picked at once (list kept open on a map >= 720
  px wide); "Places: © OpenStreetMap contributors (Nominatim)"
- Enter (`srchGo`): the highlighted row (↑ / ↓), else a position typed
  (not a 4-character locator: "AB12" may be a postcode - Enter asks
  OpenStreetMap then, the locator stays listed), else an own place named
  exactly so, else OpenStreetMap's first (asked if not yet). Esc: the list,
  then the box; a press outside the box hides the list; ✕ (`srchClear`) =
  text, list and pin gone, box closed
- The pin (`srchPin`, `srchDraw`: orange #ff3d00, bigger than a marker,
  its label; `srchHit` in mapInit's click / hover before decoder points,
  `mapSel` 'srch' -> `srchPopupHtml` in mapRenderPopup; mapOnMessage keeps
  it selected): address, position decimal / DMS (`srchDms`) / grid locator
  (`srchLocator`) / UTM / MGRS (selectable text: `td.sel`), distance /
  bearing from the station, 📍 Add as marker (`srchToMarker` -> mkCreate;
  a typed position gets "Marker N"), Remove pin. ⤢ includes it
- 🏠 = `mapCenterStation` (the ⌖ before). 🎯 = `mapGoTx`: `mapTxEsts()` =
  the ⏯ replay's estimate at the moment shown (`lrEstAt`, before the first:
  the drive's last) + the 📡 DF panel's shown maps' (`rdfShownMaps`, coherent);
  the 95 % region (2 x r95) across ~a third of the map; pressed again with
  the view where it left it = the next (`srchTxAt`); greyed (`.map-btn.dis`)
  with none, the status line says how to get one (or that the DF panel's
  pick has none). Jumps stop a replay's Follow (`mapStopFollow`) - it
  would pull the view back to the vehicle
- Tests (job scratch aa76cf1b t/): t_parse.mjs (node; the section cut out
  of the page and run in a vm - 189 checks: ~40 lat / lon forms and ~20
  non-coordinates, locators, DMS, UTM against pyproj on 3018 points < 1 mm,
  MGRS against NGA GeoTrans (the `mgrs` package) - forward equal except
  within 1 mm of a metre boundary (GeoTrans' own sub-mm noise), 3000 MGRS
  strings at every precision -> square centres within 1 cm -, links);
  t_ui.mjs (headless Chrome over CDP, page served statically with a UTF-8
  charset, Nominatim stubbed by a fetch wrapper: 90 checks - box, typed
  forms, pin + popup, locator Enter, online pick / list / box fit / 180°,
  request spacing, cache, offline / 429, markup escaped, markers / aircraft /
  station found, Esc / ✕, 🏠, 🎯 cycling + DF pick, phone width, one REAL
  Nominatim request, the box left open shown after a reload - the first
  version only rendered it on a click -, no page exceptions)

