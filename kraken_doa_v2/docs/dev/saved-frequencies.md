# Saved frequencies and map markers

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📻 Saved frequencies + 📍 map markers (one feature; `src/saved_freqs.cpp`, UI "📍 MAP MARKERS" + "📻 SAVED FREQUENCIES" in kraken_doa.html):**
- Model: CATEGORIES {id, name, col #rrggbb}, LOCATIONS {id, name, lat, lon,
  notes, t} = the map markers, FREQUENCIES {id, name, hz, cat, loc, mode
  ""|WBFM|NBFM|AM|DIGITAL, bw Hz (0 = the VFO's), gain null (keep the
  receiver's) | -1 (Auto / AGC) | dB 0..60 (0.1 steps), dec
  ""|OFF|AUTO|PLUGIN:<id>, tone, notes, t}; only name + hz are required,
  cat / loc are ids or "". A
  frequency with a location is listed in that marker's panel - several
  frequencies at one place share ONE location (the editor snaps a new
  location within `SF_SNAP_M` 15 m of an existing marker onto it); a marker
  without frequencies is a plain marker (Add Marker, OSM mast -> marker)
- Backend = the single source of truth (every browser):
  `SF_SET:{json}` (a frequency; may carry `newcat` / `newloc` - the category /
  location the editor made in the same step: validated and committed
  together on a copy of the state, so a refusal changes nothing),
  `SF_DEL:id`, `SF_LOC_SET:{id,name,lat,lon,notes}`, `SF_LOC_DEL:id` (its
  frequencies stay, `loc` cleared), `SF_CAT_SET:{id,name,col}` (names unique,
  case-insensitive; a bad colour = #90a4ae), `SF_CAT_DEL:id` (its
  frequencies: `cat` cleared), `GET_SF` (page connect). Every one answered
  to ALL pages with `savedfreq::message_json()` = `{"savedfreq":{cats, locs,
  freqs}}`; a refusal adds `error` + `id` (ids are random, chosen by the
  page: only the sender reacts). `SF_` is in `is_query_command` (no sync_cmd
  echo). Ids [A-Za-z0-9_-]{1,24}; text cut on a UTF-8 boundary, control
  characters dropped, trimmed (notes keep \n / \t). Limits: 2000
  frequencies (1 Hz..100 GHz, integer Hz), 500 locations, 64 categories,
  names 80 bytes (categories 40), tone 40, notes 2000 / 4000 (location), and
  the whole list <= `MAX_TOTAL` 1 MB as JSON (one broadcast - the WebSocket
  server closes a client beyond 2 MB of backpressure; 2000 plain entries are
  ~260 KB)
- File `saved_frequencies.json` (cwd-relative = the Docker volume; gitignored -
  positions near the station) `{"version":2,"categories","locations",
  "frequencies"}`, rewritten (tmp + rename) on every change, read by
  `savedfreq::load()` in main; entries are rebuilt one by one (a damaged one
  is skipped, a dangling cat / loc cleared); an unreadable file is renamed
  `.bad` and the list starts empty (not overwritten). No file yet: the
  default categories (Airband, Broadcast, Business band, Amateur, Marine,
  Emergency services - ids airband, broadcast, business, amateur, marine,
  emergency) and, once, the old `map_markers.json` (MARKER_* era: a location
  per marker, a frequency per marker frequency named by its label, else the
  marker's name), then renamed `map_markers.json.migrated`. Not touched by
  RESET_SETTINGS
- Page state (globals block - mapDrawNow / drawBox can run before the
  sections are evaluated): `sfCats / sfLocs / sfFreqs`, `mkSel / mkEdit /
  mkWantEdit / mapMenuAt`, `sfUi` (localStorage `kraken_sf` {spec, col: {cat:
  1} folded groups, hide: {cat: 1} groups without spectrum marks; '' = the
  uncategorised}), `sfSpecRects`. `sfOnMessage` replaces the lists and
  redraws everything (list, categories, marker panel, map, drawBox, panes).
  No optimistic updates
- Sidebar "📻 Saved Frequencies" (`#sf-section`, just above the 🔍
  Continuous Scanner - the user's choice; no mode class, so it shows in all
  modes even though its neighbours are wideband- / coherent-only; header
  count `#sf-count`): search (words in name, MHz, tone, notes,
  location, category, demod, decoder), + Add, "Show on spectrum", ⚙
  Categories (`#sf-cats`: colour picker, rename, count, ✕, + Add; not
  re-rendered while it has the focus - refreshed on focusout).
  `sfRenderList`: groups in category order then "Uncategorised" (empty
  categories left out), rows by frequency: colour bar, name, MHz, a meta line
  (`sfMeta`: demod · bandwidth · decoder · tone · 📍 location), 🗺 (location
  on the map: `sfShowOnMap`) and ✎; a row click = `sfTune`. `sfMarkActive`
  (1 s, while the section is open) outlines rows a VFO is within 500 Hz of,
  in the VFO's colour. ☆ Save on each VFO card = `sfNewFromVfo`
- Editor (`#sf-dlg`, a modal - also opened from the map): name*, frequency*
  (`sfParseHz`: MHz, or kHz / GHz / Hz suffix, comma decimal), ⟵ Selected
  VFO, category (or "+ New category…" -> `newcat`, colour = the first unused
  `SF_PALETTE` entry; an existing name = that one), location (a marker, or
  "+ New location…": name - blank = the frequency's -, lat / lon - "lat, lon"
  pasted in the latitude box works -, 🗺 Pick on map, 📡 Station), demod /
  bandwidth / decoder ("Keep the VFO's" = ""), gain (`#sf-f-gain`: "Keep
  the VFO's" = null, the default; "Current gain (29.7 dB)" = `sfCurrentGain()`
  - the top bar's gain, in independent mode the selected VFO's tuner's -
  resolved to that NUMBER on save (`sfDlgGain`); then every `GAIN_VALUES`
  step, Auto (AGC) = -1; a saved value that isn't a step gets its own
  option), tone / code, notes. New entries are filled from the selected VFO
  (frequency on 100 Hz, demod, bandwidth, decoder when on; gain stays
  "Keep"). Save sends SF_SET and waits for the list
  (`sfDlg.pending`): an error with its id is shown in the dialog, else it
  closes + a toast (`sfFlash`, `#sf-toast`). Tune (the form, unsaved),
  Delete, Cancel; Escape (document level - once) / ✕ / the backdrop ask
  before dropping changes (`sfDlgLeave`). Other pages' changes refresh its
  lists keeping the picks. Pick on map (`sfPickStart` / `sfPickEnd`): the
  dialog steps aside, the map opens with a banner (`#map-pick`,
  `.map-pane.picking` crosshair), the next click (mapInit's pointerup, before
  the usual click handling) = a marker -> that location, elsewhere -> a new
  one there; Esc / Cancel = back
- Tuning `sfTune(id | entry, {tuner})`: checks first (connected, in
  FREQ_MIN..FREQ_MAX unless the decoder is a fixed-frequency one, a VFO
  held on a FIXED frequency - `sfFixedLock`: ADS-B, AIS - needs the entry to
  have a decoder) - nothing changes when refused; then on the SELECTED VFO:
  the decoder (`digSetModeFor`, OFF = `digToggle`; a fixed-frequency plugin
  tunes by itself and ends there; a `lock_vfo` one - ism - puts the VFO on
  its tuner's centre), the demod (`setDecimatorDemodMode`, which sets that
  mode's usual bandwidth) and the bandwidth (exactly - any width) -
  both skipped on a locked VFO -, then `sfTuneHz` (was mkTune): a VFO
  locked on its tuner's centre (`lock_vfo`) stays at offset 0 and the tuner
  retunes (FREQ: whole kHz; independent: its own tuner); otherwise
  coherent / wideband scan - inside `maxOffsetHz` the VFO just
  moves, else `FREQ:` on whole kHz (`sfRetuneCentre` / `sendFreqChange`) with
  the VFO offset taking the rest (145.5125 -> FREQ:145.513 + VFO -0.5 kHz);
  independent - the clicked pane / the VFO's own tuner / any tuner whose band
  holds it (`paneMoveVfo`), else the VFO's own tuner retunes
  (`paneSendFreq`) with the VFO at 0. No VFO at all: the receiver's centre
  (independent: the clicked / selected tuner). Then the entry's gain, if it
  has one (also after a fixed-frequency decoder's own tune): `sfApplyGain` -
  the nearest step, `GAIN:` + the top-bar slider (gainUserChanging, cooldown
  countdown) or in independent mode `TUNER_GAIN:` on the tuner the VFO ended
  up on (`paneSendGain`); NOTHING is sent when the gain already is that (a
  gain change recalibrates the array). A toast says what happened (+ the
  gain)
- Spectrum: `sfDrawSpectrum` (drawBox's `drawBoxTags`, before the AI badges) - tags in the
  category colour from y 18 (under the VFO labels) in up to 3 rows (2 below
  200 px), entries within 3 px share one ("Name +2"), names over 22 chars
  cut; no room = a triangle at the top; a faint dotted line down to the
  bottom (`sfLayoutTags` / `sfDrawTag`). `handleDown` -> `sfMarkerHit` (after
  `aiMarkerHit`) tunes instead of placing the VFO (`sfMarkerConsumed` eats
  the click), `handleMove` -> `sfMarkerHover` (tooltip + hand).
  `drawBoxTags` (tags + AI badges) runs on EVERY path through drawBox -
  the spectrum drag's saved-VFO-bars path (`scrollDragSavedBoxes`, during a
  drag and until the retune settles) and the discrete scan's used to
  return before them: the tags vanished while dragging (the user's report
  2026-10-22). It also keeps the hand cursor over a tag / badge from the
  mouse's last position (`fpHover`, set first in handleMove, null on
  mouseleave): drawBox resets the cursor from the VFO bars on every
  spectrum frame (~20 Hz), and the hand, set only on mouse moves,
  flickered with the arrow. A tag wins over a VFO bar under it (as the
  click does). Test (job scratch): t_sftags_ui.py - real mouse events, the
  committed page 0 / 40 hand samples + 0 / 10 tags during a drag, fixed
  40 / 40 + 10 / 10. Independent
  panes: `sfPaneTags` from `paneDrawOverlay` (9 px, 2 rows, lines through
  the spectrum part), `p.sf` hit-tested in `paneDown` (tunes with
  `{tuner: p.ch}`) and `paneMove` (title + hand). Hidden categories (👁)
  and "Show on spectrum" off draw nothing; the waterfall gets no marks
- Map (📍 MAP MARKERS): right-click on `#map-canvas` (`contextmenu`; touch:
  a 600 ms long press without moving, `lpT` in `mapInit` - a touch pan needs
  8 px, a mouse 3 px) -> `mapMenuOpen` (`#map-menu`): "📍 Add Marker", "📻
  Save a frequency here" (editor with a new location there), Viewshed, Set
  station; on a marker's pin also "📻 Add a frequency to …", Edit, Viewshed,
  Delete; the clicked lat/lon as a footer. Closes on a press elsewhere,
  Escape, wheel, window blur. Pins (`mkDraw`: tip on the spot, head 15 px
  above, `mkHit` tests the head, before decoder points) in the category
  colour when all the location's frequencies share one, else `MK_COL` pink;
  label = name + lowest frequency (+N). `#map-mk` panel (`.map-pane.mk-open`
  narrows the 📡 DF panel): position, distance / bearing from the station,
  every frequency at the location (`sfFreqsAt`, by frequency: a button that
  tunes + ✎), notes, + Frequency / ✎ Edit (name, position, notes - built
  once, `mkLeaveEdit` asks before dropping changes) / Centre / Viewshed /
  Delete (confirm says the frequencies stay). Add Marker sends SF_LOC_SET and
  sets `mkWantEdit`; the list arriving with that id opens its edit form. A
  click on the empty map closes the view (not an edit); ⤢ fits the markers
- Tests (job scratch, not in the repo): `t_sf.cpp` (saved_freqs.cpp offline:
  migration, newcat / newloc in one step, every refusal leaves the state
  unchanged, text cleaning, UTF-8 cuts, location / category delete, reload =
  same list, 2000 limit, damaged file kept as .bad), `t_sf_ws.py` (the
  commands over the WebSocket of a TEST build - ports 28080/28081/28091/28092
  as 1808x were taken -, two pages get every broadcast, refusals, restart
  keeps the list), `t_sf_ui.py` (headless Chromium via CDP + a synthetic FFT
  frame fed to `ws.onmessage`: editor save / refusal / snapping / new
  category, list + search + fold, row / spectrum tag / marker panel tuning,
  retune outside the band, out of range, pane tags, pick on map incl. a
  marker, categories, edit / delete, Escape, marker add / delete). Chromium:
  `/usr/lib/chromium/chromium` directly with `--no-proxy-server` AND
  `--password-store=basic` - without it (Chromium 147 here) every page
  navigation stalls forever before its cookie lookup, waiting on the desktop
  keyring (fetch() from a data: page still works, which makes it look like a
  network problem); when other builds load the Pi, use long CDP timeouts

