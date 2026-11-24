# The icon set

`kraken_doa.html` draws its pictograms with one monochrome set of inline SVG
icons, drawn for this page. There is no emoji in the page's chrome. The
colours are in *theme.md*, the wording rules in *sidebar.md*.

## Why (2026-11-18)

The user accepted the recommendation to "replace the emoji icons with one
monochrome icon set": box titles, the rail and the tabs used coloured emoji,
which render differently on a Pi, Windows and Android (or not at all: the
power symbol U+23FB is missing from many fonts, which is why the page
already drew that one) and fight the restrained palette. The View button's
sliders icon and the sidebar's chevron were the model; both are in the set
now.

## How it works

- **The sprite**: one hidden `<svg style="display:none">` right after
  `<body>` holds a `<symbol id="i-NAME" viewBox="0 0 16 16">` per icon.
- **Markup**: `<svg class="ic" aria-hidden="true"><use href="#i-NAME"/></svg>`.
- **Script**: `icon(name, cls)` (top of the globals block, a function
  declaration so any code can use it) returns that string; `cls` adds
  classes (`'t'`: the size of the text around it). No other helper.
- **Style** (the `Icons` block before "Sidebar look"): 16 x 16 grid, strokes
  only - `fill:none; stroke:currentColor; stroke-width:1.6;
  stroke-linecap / linejoin: round` are set once on `.ic`; a symbol fills a
  small detail itself (a dot, the play triangle, the pause bars).
  `.ic {width:1em; height:1em; font-size:16px; vertical-align:-.15em}`.
  The colour is the text colour, so an icon lights and hovers with its
  button.
- **Sizes**: 16 px by default; 14 px in buttons, 13 px in `.btn.sm`; `.ic.t`
  is 1.25 em of the text it sits in (notes, states, tables). Places with
  their own size are listed under "sizes by place" in the CSS (the rail 15,
  the top bar's cog 24 / 22 on a phone, its help-mode ? 17 / 22 (in the
  top bar's block, next to the cog's), the menu button 18 / 22, the top
  bar's field icons 16 (`.tb-ic`, in place of the captions) and its
  speaker and Bias tee buttons 17 (20 in the phone drawer), the
  sidebar chevron 15, close crosses in headers 13, chevrons of folds 12;
  drive mode's buttons 26 / 30 with a 2 px stroke - 2.4 in its Day look).
- **Icon + label**: the label is a `<span>` (or `<b>`) right after the icon;
  one rule, `.ic + span {margin-left:6px}`, gives the gap. Containers that
  space their items with a flex `gap` zero it (the map popups' headers). In
  a one-line title (sidebar box titles, the right pane's tabs, dialog
  titles) the icon is `vertical-align:top`, so the bar keeps its height.
- **Names**: icons are `aria-hidden`; an icon-only button keeps its `title`
  or gets an `aria-label` (the drawer's close button: "Close").
- **Third-party text is left alone**: plugin names and descriptions (which
  contain emoji of their own), decoder output, OSM tags, the assistant's
  map-layer symbols (`aimDraw`, drawn on the map as the agent sent them).

## The set

One concept, one icon, reused everywhere (the map icon of the rail, the tab,
the pane tab and the "Pick on map" button is the same symbol).

| Name | Stands for |
|---|---|
| `close` | close, remove (x) |
| `help` | help mode (the top bar's ?, the drawer's and the dialogs' headers) |
| `menu` | the sidebar (top bar, three lines) |
| `chev-r` `chev-l` `chev-d` | fold / unfold (sidebar boxes, map panels, table details, categories), step, the dropdown arrow, collapse the assistant |
| `sliders` | View (display settings) |
| `gear` | settings: Setup Settings, Categories, scan settings, model and effort, Generate |
| `refresh` | refresh, check, rescan, regenerate, restart |
| `reset` | reset to defaults, the boxes' order; drive mode's Reset (bearings) |
| `power` | shut down; Setup Settings' System tab (restart and shut down from the page) |
| `edit` | edit (pencil) |
| `trash` | delete |
| `download` `upload` | download / get, upload |
| `plus` `minus` | a new conversation; zoom in / out (drive mode) |
| `search` | the map search |
| `home` | the map back to the station |
| `wheel` | drive mode (a steering wheel: the rail's Drive button) |
| `sun` `moon` | drive mode's Day / Night (the button shows the look it switches to) |
| `mute` | audio off (the top bar's speaker button while off, drive mode's Audio button) |
| `target` | the estimated transmitter, the most likely spot |
| `crosshair` | centre the map on something, a typed coordinate; drive mode's Follow |
| `fit` | show everything (fit the map) |
| `enter` | run the search |
| `play` `pause` `stop` `next` `first` | start / play, pause / hold, stop, skip / next, to the start |
| `star` | saved frequencies, save a frequency |
| `tag` | Signal ID (a label tag: signals labelled with their type; 2026-11-20, *signal-classifier.md*) |
| `eye` | viewshed / line of sight, show or hide (marks, a key) |
| `lock` | a locked signal |
| `pin` | a marker, From GPS, incidents |
| `clock` | time (auto-name, earlier conversations) |
| `timer` | the drive's write-speed test |
| `folder` | open a file |
| `hourglass` | working, calibrating |
| `warn` | a warning (also an alarm) |
| `ok` | done, installed, matches |
| `no` | missing, refused, failed |
| `spin` | in progress (rotated by `.ai-spin`) |
| `split` | the Both tab |
| `arrow-l` | take from the selected VFO |
| `tune` | Tuner & Audio (phone) |
| `panels` | Panels (phone) |
| `vfo` | Decimators (the VFOs) |
| `doa` | MUSIC DoA, the DF panel, a talker's own DoA |
| `bearing` | a bearing in degrees (transcripts), a navaid |
| `beam` | Beamforming |
| `antenna` | the station / receiver: Station Information, Open Heimdall Server, IQ, set the station, beacons |
| `tower` | a mast (OSM masts), a site, a ground station |
| `radar` | a radar, a ground interrogator |
| `plug` | the Wideband board |
| `biastee` | the antenna bias tees (the top bar's Bias tee button: a T on a coax line whose stem is a bolt; 2026-11-18, *bias-tees.md*) |
| `gain` | the top bar's Gain (an amplifier triangle with a line in and out; 2026-11-18, *sidebar.md* "Top bar and rail") |
| `step` | the top bar's tuning Step (two marks with a double arrow between them; 2026-11-18) |
| `mixer` | the Wideband variant's mixer field in the top bar (a circle with a cross, the RF mixer symbol; 2026-11-18) |
| `globe` | Web Mapper, the online search, web tools |
| `doc` | DoA Recording |
| `log` | Decoder Logging (its Setup Settings section, the decoder tabs' button to it) |
| `tape` | IQ & Audio Recorder, playback of a recording |
| `speaker` | audio, listen, heard; the top bar's audio on / off (its speaker button, lit while on) |
| `display` | Display |
| `radio` | the Scanner, a station |
| `sweep` | the Continuous Scanner |
| `flask` | Experimental |
| `bug` | Debug Playback |
| `plane` | aircraft, Array Calibration (from aircraft) |
| `decoder` | the decoders |
| `speech` | Voice to Text, Transcription (not the VFO card's Transcribe tick: 2026-11-18, like its neighbours it has none) |
| `bot` | the AI assistant |
| `map` | the map |
| `layers` | the map's Layers (stacked sheets) |
| `replay` | the DoA log replay |
| `signal` | path loss / signal mode, TX antennas, links, paths |
| `range` | range from signal strength |
| `array` | the Array Calculator; the top bar's operating Mode (how the array's tuners are used) |
| `chart` | telemetry history |
| `terminal` `tool` | the assistant's shell, other tools |
| `package` | an archive recording |
| `hash` `id` `bell` | a selcall / DTMF sequence, a radio's ID, a tone (analog FM) |
| `ship` `anchor` | a ship, an aid to navigation |
| `glider` `heli` `balloon` | FLARM / OGN paraglider, helicopter, balloon (also a radiosonde) |
| `pager` `phone` | a device / pager, a cell |
| `sos` `paw` `mic` | a SAR beacon, a wildlife collar, a broadcast; `mic` also a transmission still on the air in the Transcription panel (Live text, 2026-11-20; `hourglass` once it ended, its final coming) |
| `car` `person` | a vehicle, a person (map search) |

The kinds in the last rows are what the DF panel's talker list and the map
search show for each kind of transmitter (`rdfIcon`, `SRCH_KIND_IC`).

## What stays text, and why

- `●` / `○` state dots (recording, an option on / off, a talker active),
  `■` colour swatches in legends, `▲` / `▼` sort marks, arrows in sentences
  ("Setup Settings → Voice to Text", "(←)"), `↗` after a link to another
  site, `│` between the telemetry chip's values: text, not pictures.
- Canvases: the spectrum's beam label and the saved-frequency scan's
  squelch label lost their emoji ("D0 BEAM 30°", "Scan -40.0 dB"); the
  map's data glyphs (markers, the station triangle, the range's cross) are
  drawn shapes and stay.
- `<option>` and `title` text can't hold an icon: a saved location in the
  editor's list is just its name; tooltips name a control without its old
  emoji ("the Map", "Transcribe").
- A CSS pseudo-element can't `<use>` the sprite: the saved-frequency list's
  "listened to" speaker (`.sfs-cur`) is the `speaker` symbol's paths as a
  mask (keep the two alike). The path-loss table's selected-row mark is a
  CSS triangle.
- Code comments and section titles ("📡 STATION ON THE MAP") keep their
  emoji: the topic files cite them.

## The array calculator

`array_calculator.html` is its own page: it carries its own copy of the
symbols it uses (`i-array` for its title; `i-doc` and `i-close` for its copy
dialog, 2026-11-18) and the same `.ic` style. Keep the copy identical to the
page's symbol.

## Rule

New UI uses `icon()` (or the `<svg class="ic">` markup) - never an emoji. A
new concept gets a new symbol, drawn here from plain shapes on the 16 grid
(no paths copied from an icon library: the licence), and a row in the table.

## Tests (2026-11-18)

`test_icons.py` in the job's scratch directory (151 checks): no pictograph
outside the allow-list above in the rendered DOM - text and `title` /
`aria-label` - in every state (all boxes open, the rail, each Setup Settings
tab, the View popover, the decoders' and transcription panels, the
assistant's dock, the map with viewshed, range, AI layers, replay, a marker,
the right-click menu and search, the recorder's list, the power dialog, a
debug playback of six aircraft with the DF panel and the decoder tab, the
phone drawer, the array calculator); every `<use>` resolves and every
symbol is used; icon-only buttons have a name; icons are `aria-hidden` and
take the text colour (the lit and the hovered rail button); no label wraps
under its icon; whole-pixel icons at 1x and 2x; the sidebar headers, the
titles' line, the rail, the tabs and the phone drawer as tall / placed as
before within 1 px (measured on the page before the change); no emoji drawn
on a canvas; no JS errors.
