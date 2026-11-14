# Station on the map

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📡 Station on the map (UI only, kraken_doa.html "📡 STATION ON THE MAP"):**
- A click on the station's mast selects it (`mapStSel`: cyan mast in a
  ring; a click elsewhere or Esc deselects - `mapSelectObj`); a press on
  the SELECTED mast drags it (`mapObjDrag`, started in mapInit's
  pointerdown before the pan; touch: no long-press menu then), and a
  heading line (70 px, label in degrees) ends in a round handle that turns
  it (0.5 deg steps). Only with a static location (`mapStaticSource()` =
  the Station Information select): a GPS / phone position would be
  replaced by the next fix - the station is still selectable there, the
  status line says how to place it by hand
- Right-click menu "📡 Set station here": static location there (heading
  kept); from GPS / mobile it asks (confirm) and switches the source
  (`setLocationSource('static')`). Selects the station
- Sent as the panel's `STATIC_LOCATION:lat,lon,heading` (6 decimals, the
  sidebar fields set too): the position on release, the heading every 300
  ms while it turns (the DoA lobes turn live; settings are saved ~1 s
  debounced) + on release. `mapStLocal` {lat, lon, hdg} is shown meanwhile
  (`mapStationPos()` / `mapStationHdg()` - the drawing, hit tests and the
  viewshed follow use those; popups etc. keep `mapStation`, the receiver's)
  until a map push reports exactly that position (`%.6f`) or 6 s pass
- Hover cursors: `move` over a selected draggable, `grab` over the heading
  handle. State in the globals block (`mapStSel`, `vsSel`, `mapObjDrag`,
  `mapStLocal`)
- Tests (scratchpad): t_vs_station.py (select, heading drag live + release,
  station drag with a pause -> viewshed follows, receiver confirms, click
  away / pan when unselected, Set station here, GPS source: selectable but
  fixed + the confirm switch, a viewshed point and a marker's viewshed
  dragged, Esc, a panel edit followed), t_vs_touch.py (tap select, touch
  drag, touch heading)

