# Drive mode

A second, deliberately small face of `kraken_doa.html` for direction finding
from a vehicle: the page's own map full-window, one big bearing readout and
ten big buttons. Script section "🚗 DRIVE MODE", CSS block "Drive mode" (at
the end of the style). *Italic* names are other topics in this folder.

## Why (2026-11-18)

The user accepted the recommendation and asked for it: "Add a drive mode.
Direction finding is often done from a vehicle: a simplified view with a
large bearing readout and the map, big touch targets, and a light or
high-contrast theme for daylight." It is for a phone or a tablet on the
dashboard, or a laptop on the passenger seat: nothing to read at length,
nothing small, nothing that needs typing.

## Entering and leaving

- **In**: the rail's **Drive** button (`#rail-drive`, the steering wheel,
  at the rail's foot - a face of the page, not a panel; in the phone layout
  it is moved into the drawer's Panels box under the panel buttons,
  `MOBILE_RELOCATIONS`), or the address `#drive` - a bookmark on the dash
  device opens straight into it. Entering sets `#drive` (a history entry:
  the browser's Back leaves drive mode); `hashchange` follows the address
  both ways.
- **Out**: the **Exit** button, Esc (unless a popover that takes Esc first
  is open - Layers - or a tooltip), or `#drive` leaving the address. Leaving
  removes the hash (`history.replaceState`, no new entry).
- **One page, a body class**: `body.drive` hides the top bar, the sidebar,
  the spectrum and waterfall, the panels, the rail, the assistant's dock and
  its floating button with `visibility:hidden` - laid out as they were, so
  nothing is resized or rebuilt and everything comes back as it was. The
  map pane (`#map-pane`) is made `position:fixed` over the whole window;
  of the map's own overlays only the canvas, the attribution and the
  offline notice stay (both moved into the part of the map the HUD leaves
  free, `driveLayout`). Panels that open themselves while driving (the
  decoders' panel on a new decoder, the assistant) open behind it and are
  there after Exit.
- **Nothing stored is overwritten**: the map shows because `rpOpen()` and
  `rpApply()` read `driveOn` - `mapState.on / pane / tab` (the right-hand
  pane's state, *map.md*) are not touched. The map's view (zoom, centre)
  and the DF panel's VFO / talker selection are drive mode's own while it
  is on: `mapSave` writes the stored ones in their place (`driveMapJson`)
  and writes nothing at all until something else changes (a base map or a
  layer picked in Layers - the user's choice, kept). Leaving puts the view
  and the selection back. The only new key is the Day / Night choice.
- **While driving the map only pans and zooms**: a click selects nothing,
  nothing can be dragged (the station, a viewshed site), the long-press /
  right-click menu (Add Marker - typing) does not open, Delete does
  nothing. Entering closes the View / Layers / All decoders popovers, the
  map's menu, help mode and a tooltip.

## The readout (one VFO: the selected one)

A card - at the top of a portrait screen, at the left of a landscape one:

- **The dial**: a ring with ticks every 30° and a mark at the top for the
  vehicle's front; the arrow is the bearing **relative to the front**
  (up = straight ahead, clockwise). Its opacity follows the confidence
  (0.3 + 0.7 x conf; not known: 0.7). It turns by the shortest way round:
  the angle drawn is continuous (`driveArrow`; 359° -> 1° is +2°), eased by
  a 0.3 s CSS transition.
- **The digits**: the **compass bearing** ("247°"), and under them the
  direction in 8 sectors of 45° (`DRIVE_REL`: ahead, ahead right, right,
  behind right, behind, behind left, left, ahead left).
- **Level**: a bar and its number, `power_dbfs` of the VFO
  (`system_status.decimator_squelch[]`, the passband's averaged-FFT peak in
  dBFS, clamped to -100..0 - the value the Android app makes its tone from,
  *mobile-df.md*); the bar spans -100..0 dBFS. Kept in `vfoPow` (globals),
  shown while less than 5 s old.
- **Conf**: the MUSIC confidence in % (peak-to-mean of the pseudospectrum,
  0..1 - `music_processor.cpp`); below 25 % in the attention colour.
- **The VFO**: its colour, `D<n> · <MHz>` (read-only).

**Where each number comes from** (`driveBearing`), the page's own data only:

1. The receiver's mobile DF status, `{"rdf"}` at 2 Hz (*mobile-df.md*):
   the VFO's `peak` is its newest MUSIC frame turned to north with the
   station's heading at the frame's time (rdf_mapper.cpp, tau = heading -
   b) - the same number the map's lobe and peak line show. Compass =
   `peak`; relative = `peak - station.hdg`; confidence = the VFO's `conf`.
   With a static location the heading is the one typed in Station
   Information; driving, the GPS course (or a compass).
2. When the receiver has no bearing for the VFO because it has no position
   or no heading yet (state `no_fix` / `no_heading`, or no status at all):
   the MUSIC DoA plot's peak (`PolarPlot.peakAngle`, the array frame, the
   array offset already applied by the receiver) through
   `doaToCompass(az, 0)` = the bearing from the front - the arrow shows,
   the digits are "—" with the reason. `doaToCompass(az, hdg)` is the
   page's single conversion (hdg - az): the DoA plots' Compass labels
   (heading 0) and the DF list's bearing of a talker at a static location
   use it too; it was three copies before.
3. Otherwise there is no bearing: the digits are "—", the arrow hides and
   one short reason shows under them, from the DF panel's state texts
   (`RDF_STATE`) where there is one: not coherent mode ("Needs coherent
   mode"), DoA off ("DoA is off"), calibrating ("Calibrating: bearings
   paused"), the VFO's squelch closed ("Squelch closed" - MUSIC runs only
   while it is open), no position ("No station position"), no heading ("No
   heading yet - drive a little"), no MUSIC frame for 5 s - the receiver
   then drops the lobe ("No DoA results yet"). The reason's tooltip has the
   rest.

**No flicker**: the readout is drawn by a 250 ms timer (`DRIVE_TICK_MS`,
at most 4 updates a second - the `rdf` status itself comes at 2 Hz), and the
DOM is written only when what it shows changed (`driveShown`), whole
degrees.

**The VFO**: the selected VFO (`activeDecimatorId`); if it has no DoA, the
first that has. The VFOs with DoA are those in the receiver's DF status
(MUSIC runs for them), in coherent mode with DoA on. Prev / Next VFO cycle
those only and make the VFO the page's selected VFO (`selectDecimator`: the
cards, the spectrum's bar follow); dimmed with fewer than two.

## The map

The page's own map (the same canvas and code): the station and its heading
tick, the lobe, the bearing lines, the heat map and the estimated
transmitter with its ellipse as the Layers say (`rdfDraw`, *mobile-df.md*),
the decoders' markers, the tiles. Drive mode sets the DF panel's selection
to the readout's VFO (`driveVfoApply`, in memory), so the map shows that
VFO's lobe, heat map and estimate - not every VFO's.

- **Follow** (on when drive mode starts): the vehicle stays in the middle
  of the part of the map the HUD leaves free (`driveFree`), at every tick
  that it moved; the position is the DF status' station (2 Hz), else the
  map push's. While Follow has not been changed by a zoom, the estimated
  transmitter of the VFO shown is kept in view too: the map zooms **out**
  (to a whole zoom, the tiles sharp) until the estimate and its label are
  60 px inside the free part - never in. Panning by hand turns Follow off
  (the map's own pan code calls `driveFollowSet(false)`); the Follow button
  turns it on again (and the transmitter fit). Any zoom by the user (the
  buttons, the wheel, a pinch, a double-click - `mapZoomAt`) keeps
  following the vehicle but stops the zooming out for the transmitter.
- **Zoom**: + / - buttons (zooming about the free part's middle) besides the
  map's own pinch, wheel and double-click.
- **Layers**: its own popover beside the buttons (`#dv-lay-pop`, the View
  popover's look, rows 56 px, big radios and switches) with the subset
  that matters while driving - **Base map** and **Direction finding** (Lobe,
  Lines, Heat) - drawn by the map's `mapLayHtml(['base', 'df'], 'drive')`
  and kept in step with the map's own Layers by `mapLaySync` (*map.md*,
  *Layers popover*). Placed with `popPlace` beside the button (left of the
  right-hand column, above the bottom row), closed by `popDismiss`'s ways.

## The buttons

Ten, each an icon from the set with its word under it, 60 x 60 px on a
phone (56 on the smallest, 360 px), 72 x 72 from 900 x 600 px, 12 px apart: Prev VFO, Next VFO,
**Audio** (the top bar's speaker button - its Audio switch until 2026-11-18 -, `tfm()`; lit while on, the speaker /
the muted speaker), **Reset** (the DF panel's Reset - `rdfReset`, with the
page's own question - for the VFO shown; dimmed when the receiver collects
nothing for it, where the panel's Reset would forget every VFO's), **Exit**,
**Follow** (lit while on), Zoom out, Zoom in, **Layers** (lit while open),
**Day / Night** (says what it switches to: the sun and "Day" at night, the
moon and "Night" by day). At the bottom edge of a portrait screen (two rows
of five), at the right edge of a landscape one (two columns of five; Exit
top right). Disabled buttons are dimmed and `aria-disabled` (not
`disabled`: their tooltip still shows).

**Help**: every button and part of the readout has a `title`, shown by the
page's tooltip (hover, focus, press and hold on touch - *sidebar.md*, *How
help is shown*). There is no `?` help-mode button in the HUD although the
top bar's is hidden: each button already carries its word, and press and
hold explains it - a second way to the same tips was more on the screen
than it gave.

## Sizes

CSS variables on `.drive-hud`: `--dv-b` (a button), `--dv-g` (12 px),
`--dv-dial`, `--dv-fs` (the digits' type size), `--dv-rw` (the card's width
on a landscape screen).

| Window | Layout | Digits (type / digit height, Arial bold) | Dial | Buttons |
|---|---|---|---|---|
| phone portrait 400 x 800 | card at the top, buttons at the bottom | 92 / 66 px | 128 | 60 |
| phone landscape 800 x 400 | card left (248 px), buttons right | 92 / 66 px | 104 | 60 |
| tablet 1024 x 768, laptop 1500 x 900 | card left (360 px), buttons right | 136 / 97 px | 200 | 72 |
| small phone 360 x 640 / 740 x 360 | as the phone | 92 / 66 px | 100 / 80 | 56 (11 px words) |
| tablet upright 768 x 1024 | as portrait, the card 560 px wide, centred | 92 / 66 px | 128 | 60 |

The card is compact - everything in it is large - so roughly half the window
is free map between it and the buttons.

## Day / Night

Night is the page's palette. **Day** is a light, high-contrast look for the
HUD only (the card, the buttons, the Layers popover): near-white surfaces,
near-black text, thicker icon strokes, the accent darkened - every text
token >= 7:1 on the HUD's surfaces, control borders >= 3:1, white on the
accent fill 9.2:1. It is a token block on `body.drive.drive-day
.drive-hud` that redefines the tokens the HUD's rules use, with light
values; the rules themselves are the same in both looks. The values are
the `DAY` set of `tools/theme_palette.py` (`--css-day` prints the block,
`--contrast` their contrast, `--check` fails if the page's block differs) -
*theme.md*. The map's own street tiles are light anyway.

The rest of the page does not get a light theme: its canvases (spectrum,
waterfall, the DoA plots, the map's overlays) are drawn in fixed colours,
and a page-wide light theme was not asked for.

Remembered per browser (`localStorage['kraken_drive_day']`, '1' / '0').
Until the user presses the button, the system's choice decides
(`prefers-color-scheme: light` = Day); the system's later changes then do
not matter.

## The screen kept awake

The Screen Wake Lock API (`navigator.wakeLock.request('screen')`) while in
drive mode: asked for on entering and again whenever the page becomes
visible (the browser lets go of it when the page is hidden), let go on
leaving. It needs a secure context - the page is served over HTTPS (8080);
where the API is missing or refuses (a battery saver), the screen's own
timeout applies. No fallback tricks (no hidden looping video).

## Deliberately absent

No sliders, no text boxes, no dialogs of its own (Reset's question is the
DF panel's), no warnings as paragraphs, no spectrum, no DF talker list, no
tools (viewshed, range, replay - their drawings stay on the map if they
were on). Not changed: how bearings are computed or collected (the receiver
does the same with or without drive mode), DoA Recording (keeps running),
the rest of the page's colours. No route recording.

## Not verified

- A moving vehicle with a real receiver: every number in the tests is a
  fixture of the receiver's messages. How the arrow behaves with real
  MUSIC frames and a real GPS course while turning (the receiver pauses
  collecting in turns, the live lobe does not), and whether 4 updates a
  second feel right at the wheel, were not seen.
- Wake lock on real devices (the tests stub `navigator.wakeLock`), sunlight
  readability of Day on a real screen, Safari / iOS, a car mount's glare.

## Tests (2026-11-18)

`test_drive.py` in the job's scratch directory (369 checks; headless
Chromium, real mouse, key and touch events; the receiver's messages are
fixtures fed to the page's own handlers - `rdfOnStatus`, `rdfOnGrid`,
`mapOnMessage`, `updateDecimatorList` - with the test server's own pushes
held back meanwhile): the single conversion; every HUD colour a token with
a Day value, no literal but white, the palette tool's `--check` OK and
failing a changed Day block; the rail's button (icon, word, tooltip, at the
foot) and `#drive` enter (also a fresh load, Day / Night by the system's
scheme until chosen, the choice over a reload); only the map and the HUD
visible (every visible element walked) in coherent and independent mode;
the map full-window; the readout for straight ahead / 90° right / behind /
behind right / ahead left at heading 0 and 130 (compass digits, relative
arrow and word), 359° -> 1° turning +2° and 1° -> 357° -4°, low and high
confidence, the level bar, the VFO line; each "no bearing" reason, and the
DoA plot's relative arrow without a heading; Prev / Next cycling only VFOs
with DoA and selecting them, dimmed with one; Audio toggling the page's
audio (FM:0 / FM:1, the top bar's speaker button); Follow re-centring as the
station fixture moves, a mouse drag turning it off (and storing no view),
the button turning it on, the estimate brought into view, zoom in / out;
Reset sending RDF_RESET for the VFO shown after the page's question and
nothing for a VFO without DF; Layers (groups, 56 px rows, a switch acting
and stored while the stored view stays, Esc and a press outside closing it
without leaving); Day / Night computed colours, remembered, the rest of
the page unchanged, contrast from computed styles (text >= 7, borders,
dial, bar, icons >= 3); targets >= 56 px and >= 12 px apart, nothing
overlapping or outside the window, the digit height, the free share of the
map at 400 x 800, 800 x 400, 1024 x 768 (touch), 1500 x 900, 360 x 640,
740 x 360 and 768 x 1024, in Night,
Day and a "no bearing" state; Layers placed beside the buttons at each
size; the drawer's Drive button on a phone; the wake lock asked for,
released, asked for again on `visibilitychange`; Exit, Esc, the browser's
Back and removing the hash leaving with the page exactly as before
(storage but the Day choice, the map's state and view, body classes, the
pane on Both or hidden, the sidebar hidden, the panels); a panel opening
itself while driving stays hidden and is there after Exit; no JS errors.

Other suites whose expectations the new rail button made outdated (updated,
nothing else changed): `test_icons.py`'s baseline gained the Drive button's
place on the rail and in the drawer (the other buttons did not move),
`test_rp.py` / `test_top.py` / `test_tr.py` count five rail buttons,
`test_view.py` expects Drive in the drawer's Panels box, `test_layers.py`
counts four users of `popDismiss`. All suites pass.
