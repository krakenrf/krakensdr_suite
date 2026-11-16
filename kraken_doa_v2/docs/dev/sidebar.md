# The sidebar's boxes, and how controls and text are written on the page

The left sidebar of `kraken_doa.html` is a column of boxes (`.sb-section`),
one per feature. This file covers the box chrome (collapse, move, what is
remembered) and the rules every control and text on the page follows - the
sidebar first, then the top bar, the waterfall bar, the panels, the map and
the dialogs. Each feature's own controls are in that feature's topic file.

## Boxes

- Markup: `<div class="sb-section" data-sb="KEY">` with a `.sb-header`
  (`<h3><span class="sb-title">…</span>` + optional `?` help, optional badge,
  `<span class="sb-toggle">`) and a `.sb-content`. `data-sb` is the box's
  stable key in the stored layout: never rename one, and give a new box a new
  key. The footer box (Open Heimdall / Reset / the layout buttons, then the
  version line - `../../../docs/dev/version.md`) has no `data-sb`: it has no
  header and stays last.
- `sbBoxInit()` (comment title "SIDEBAR BOXES", runs once at script load)
  adds ↑ / ↓ buttons (`.sb-mv`) and the chevron to every keyed header, then
  applies the stored layout and sets up the drag (`sbBoxDragInit`). A click on the header collapses / opens the box
  (`toggleSection`); the buttons stop the click from reaching the header.
- **Move** (`sbBoxMove`): the box swaps with the nearest *visible* box in
  that direction. Boxes hidden by the operating mode (`.coh-only`,
  `.wb-only`), the Wideband Board box and the phone-only boxes keep their
  place in the order, so a layout made in one mode still makes sense in
  another. After the move the sidebar scrolls by the distance the header
  moved (divided by the sidebar's CSS `zoom`), so the button stays under the
  pointer and can be pressed again. ↑ of the first and ↓ of the last visible
  box are disabled (`sbBoxButtons`, re-run when the body class - the mode -
  changes, the Wideband Board box appears, or the window is resized).
  `moveBefore` is used where the browser has it (it keeps focus and inner
  scroll positions), `insertBefore` elsewhere.
- **Drag** (`sbBoxDrag…`, added 2026-11-16): a box is dragged by its header.
  Pointer events on the sidebar (mouse, pen and touch), not HTML5
  drag-and-drop (no touch support, no control over the look).
  - Start: mouse / pen after 5 px of movement (`SB_DRAG_PX`), so a click
    still collapses. Touch after a 400 ms press (`SB_DRAG_HOLD`) that stays
    within 8 px (`SB_DRAG_SLOP`): a swipe that starts on a header scrolls the
    sidebar as before (the press is dropped on the slop or on the browser's
    `pointercancel`); the header darkens during the hold (`.sb-press`) and a
    short vibration marks the lift where the browser allows it. Chosen over a
    dedicated grip with `touch-action:none` because the whole header is the
    target (a grip is a small finger target and would take room from the
    title) and a long press is how phones reorder lists. Once lifted, a
    non-passive `touchmove` listener stops the scroll; it sits on the headers
    only, so a swipe that starts in a box's content never waits for this
    page's busy main thread. The long press's context menu is suppressed.
  - Not handles: the ↑ / ↓ buttons, the `?`, and any button, input, select,
    link or label in the header. The footer box has no header and can't be
    dragged.
  - While dragging, the sidebar has `.sb-dragging`: every keyed box shows
    only its header (the 800 px boxes would make the drag awkward; the
    `collapsed` classes are not touched, the chevrons keep showing the real
    state). The box is `position:absolute` in the sidebar's scrolled content
    (the sidebar gets `position:relative` on the desktop for the drag), lifted
    (shadow, blue border) and follows the pointer; a dashed placeholder
    (`.sb-drop-ph`) stands where it will land. Right at the lift the sidebar
    scrolls so the box's slot is where its header was, under the pointer; the
    short header-only list usually can't scroll that far, and a spacer
    (`.sb-drag-pad`) above the list (or below the footer) makes up the
    difference - an empty band at the top while dragging, but nothing jumps
    and a small move is a small move. The box stays between the first box and
    the footer. Near the sidebar's top / bottom edge (44 px, below the phone
    drawer's sticky header) it auto-scrolls, faster closer to the edge.
  - The placeholder goes where the middle of the dragged box is, counting
    only visible boxes. Moving down it lands right after the visible box above
    the gap, moving up right before the one below - the same as `sbBoxMove`,
    so boxes hidden by the mode keep their relative order. Back at its own
    slot it returns to exactly where it was.
  - Drop: the box moves to the placeholder (`moveBefore`), the boxes open
    again and the sidebar scrolls so the dropped header stays where it was let
    go, with the `sb-moved` flash; then `sbBoxSave()` and `sbBoxButtons()`.
    A drop at the original slot changes nothing and restores the scroll
    position. Escape, `pointercancel` and the window losing focus cancel the
    same way. The click that the release makes on the header is swallowed (a
    capture listener on window, for 600 ms after the release; also after an
    Escape while the button was still down); the next real click collapses as
    usual.
  - Look: `cursor:grab` on keyed headers, a six-dot grip in the header's left
    padding that shows on hover (always, faint, on touch screens);
    `grabbing` and no text selection on the whole page while dragging
    (`html.sb-drag-on`). The ↑ / ↓ tooltips mention the drag; no text on the
    controls.
- **Remembered**: the order and which boxes are collapsed, in
  `localStorage['kraken_sb']` = `{v:1, order:[keys], closed:{key:0|1}}`,
  written on every move and collapse. It is per browser, like the sidebar
  width (`kraken_sidebar_w`): a layout is a per-screen choice, and it
  survives restarts of the receiver and of the browser. It is not a receiver
  setting, so "Reset Settings to Default" does not touch it and other
  browsers are not affected.
- Applying a stored order (`sbBoxOrder`): unknown keys are ignored; a box
  the store doesn't list (added by a later version of the page) is placed
  after the box it follows in the markup. A box with no stored collapsed
  state uses the markup's (`collapsed` class: Experimental, Debug Playback).
- Footer buttons: Collapse all / Open all (`sbBoxCollapseAll`) and ↺ Order
  (`sbBoxReset`: the markup's order and collapsed boxes, store cleared).
- Names: the width code a few thousand lines later owns `sbSave` /
  `sbApply` / `sbWidth`; the box code uses the `sbBox…` prefix. (A second
  `function sbSave` silently replaced the first one during development.)

## Hiding the whole sidebar

The ☰ button at the left of the top bar (`#menu-btn`, `toggleSidebar()`)
hides and shows the whole sidebar on the desktop; in the phone layout the
same button opens the drawer, as before.

- Desktop: `body.sb-hidden` hides `.sidebar` and its resizer strip
  (`display:none`), the main area takes the room, and a `resize` event
  refits the spectrum, the waterfall, the panes and the map (as the width
  drag does). The button is lit while the sidebar is hidden and its
  `title` / `aria-expanded` say which way it goes (`sbHideBtn`).
- Remembered per browser in `localStorage['kraken_sidebar_hidden']` ('1' or
  absent), like the width and the box layout; applied at script load, before
  the plots are made, so they start at the right width.
- `toggleSidebar(true)` shows the sidebar in either layout (drawer open /
  not hidden): code that points the user at a box calls it
  (`wbOpenSection`). `toggleSidebar(false)` closes the drawer / hides.
- The phone layout ignores `sb-hidden` (its CSS puts the drawer back) and
  never changes the stored desktop state; a layout switch back to the
  desktop only closes the drawer.
- Tests (2026-11-16, 33 checks, headless Chromium with real mouse, key and
  touch events): hide / show by click and by Enter; the main area, the
  spectrum, the waterfall and the map gain and give back the room; the
  state over a reload and a browser restart (plots made at the full width);
  `wbOpenSection` brings a hidden sidebar back; independent mode; the box
  layout untouched; phone: the stored state doesn't hide the drawer, the
  tap opens it, the button keeps its size and is not lit; no JS errors.

## Rules for controls and text

The sidebar was reworked on 2026-11-15 at the user's request - it was too
text heavy, with explanations written on the controls - and the rest of the
page got the same treatment on 2026-11-16.

- **No explanations on the controls.** A label is one to three words. What a
  control does, its limits ("0 = forever", "1 ms - 60 s") and advice go in a
  `?` help tip (`<span class="help">?<span class="tip">…</span></span>`,
  works on touch) or, for minor points, a `title`. Status text says the
  state ("Idle", "Not playing", "No calibration in use"), not what to do
  about it. JS-built notes follow the same rule (the recorder's "Needs
  24.0 MB/s · drive not tested", the array's "Spacing 0.42λ · resolution
  ~6.1°" - details in their tooltips).
- **Rows**: `<div class="ctrl-row"><label>Name</label> control [unit]</div>`.
  The label is a fixed 100 px column (no colon); selects, number and text
  boxes fill the rest; a unit is `<span class="u">mm</span>`; a slider's
  value is `<span class="sv">`. A label that is not the row's name (a
  checkbox with its text) carries its own `style` or class and is left alone
  by the column rule (`label:first-child:not([style])`).
- **On / off**: a setting that is on or off is a switch - a checkbox with
  `class="sw"`, or one of the older ON / OFF buttons marked `data-sw` (their
  handlers rewrite `className` to `btn on` / `btn off`, so the switch look
  hangs on the attribute, and the ON / OFF text stays for screen readers).
  Plain checkboxes are for lists of options with the text to their right.
- **Status lines**: `<div class="kv"><span>Name</span><span id=…>value</span></div>`.
  Sub-headings: `.sb-sub`. Small notes: `.sb-note` (hidden while empty).
- **Buttons**: `.btn` (`.primary` for the box's main action, `.danger` for
  destructive ones, `.sm` small). The scanners' Start buttons are blue while
  idle and red while running (by id, their handlers also rewrite
  `className`).
- Colours: only the palette's tokens (*theme.md*).
- Two CSS blocks hold the look: "Sidebar look" (scoped to `.sidebar`: the
  box chrome, the label column) and "Page look" (the same buttons, text
  boxes, sliders and switches for the top bar, the waterfall bar, the
  panels, the map, the dialogs and the assistant; panels with a header bar,
  pill tabs and badges). Both sit before the phone rules. The phone layout
  moves top-bar controls into the sidebar (`#mobile-controls-slot`); their
  id-scoped rules win, which is why the switch selector lists that id and
  repeats its `background-image` in the on / off states (a `background`
  shorthand there would drop the knob).
- The sidebar is scaled with CSS `zoom` (dragged width): anything positioned
  from `getBoundingClientRect()` inside it divides by the zoom, and box
  widths are designed for 290 px.

### Outside the sidebar

- **In JS-built text** use `helpTip(html, wide)` for the `?` (a global
  function; `wide` for long texts - `initHelpTips` places the popup, also
  from inside the map's panels and the tabs under the waterfall).
- **Panels with a long "how it works" note** (Viewshed's two modes, Range,
  Replay, a decoder's description, the Voice tab) carry it in a `?` next to
  the panel's title or first row - never as a paragraph in the panel.
- **States** are two to five words with the reason in the `title`: the DF
  panel's `RDF_STATE` is `[text, tooltip]`, the range's `why` has a `tip`,
  the assistant's warnings are `[text, tooltip]`, the KerberosSDR banner
  keeps what to do (antennas disconnected) and puts the rest in its
  tooltip. Diagnostics that carry numbers (the range's warnings, a replay's
  rebuilt times) stay as they are.
- **Bars**: labels without colons (`Mode`, `Gain`, `Scale`, `Zoom`); the
  top bar's Audio is a switch (`data-sw` on the `.tb-btn`); a decoder
  option that is on / off is a button lit while on (its name only -
  `digOnOffText` adds ": state" for anything that isn't On / Off).

### Top bar and rail (2026-11-16)

The user found the top bar cluttered, busy and ugly; the mode chip
duplicated the Mode selector; "CH" did not say that it picks the displayed
channel; and the panels' buttons should sit on the right-hand bar with the
Assistant button. What it is now (CSS block "Top bar and right-hand rail"):

- **Order**: ☰, the frequency, then fields - Step, Gain, Audio, Mode - and at
  the right `.tb-right`: the status chips above the telemetry chips. A
  field is a `.tb-group.fld`: a small upper-case label above its control
  (a grid; Gain's and Audio's value sits on the label's line). The
  separators are gone. One row from about 1650 px of window (with the
  sidebar); narrower, the right block wraps under the controls. The
  Wideband variant's mixer (`#wbv-group`: side, LO drive, ring / LO as its
  readout) is a field too - styled from a forced display only, no
  downconverter was at hand.
- **Chips** (`.status-badge`): a dark pill with a dot in the state's
  colour - the scripts still set the chip's `background` to that colour;
  an inset shadow darkens the pill and the dot (`::before`) inherits it.
  Left to right: the scanner (only while a scanner runs - no "Idle" chip),
  the calibration (fixed width; the cooldown countdown shows in it:
  `updateCooldownBadge`), the connection. So a chip that appears or changes
  its text never moves a control or another chip. No mode chip: the Mode
  selector stands next to them. Remove a chip's element only together with
  its script: the countdown's timer was inside `if (cooldownEl)`.
- **Bar colour**: the calibration state still colours the bar, but lightly -
  a faint tint and a 2 px line at its lower edge (`.top-bar.cal-*`); the
  chip names the state. The playback's red line (`body.pb-on`) wins over it.
- **The antenna shown** (`#chs`, was "CH" in the top bar): a selector on the
  spectrum's header reading "Antenna N" (`updateChannelSelector`), next to
  the title, which is now "Spectrum · <demods heard>" (`updatePlotTitle`).
  Coherent mode only. The phone layout no longer moves it into the drawer.
- **The rail** (`#rail`, at the right edge after the assistant's dock): one
  button per panel - `#map-btn` (`mapToggle`), `#rail-dec` / `#rail-voice`
  (`railPanel`: opens the panel under the waterfall on that tab, or closes
  it when it already shows it) - lit while their panel shows (`railSync`,
  called from `infoRenderTabs` and `infoPanelClose`; 🗣 carries the count of
  unseen transcripts), and `#rail-ai` while the assistant is enabled
  (`aiDockToggle`; lit while its dock is open - by CSS from the body
  classes). It replaces the collapsed assistant's strip (`#ai-strip`) and
  the three buttons at the end of the waterfall bar. The phone layout has
  no rail: `applyMobileRelocation` moves `#rail-panels` into the drawer's
  Display box (where the buttons were before), the assistant keeps its
  floating button.
- Tests (48 checks, headless Chromium, real mouse / touch): the bar's
  content and order, one row at 1700 px, labels above controls, the chips
  at rest; the scanner chip appearing and going without anything moving;
  the calibration chip's texts at a fixed width, the cooldown in it, the
  bar's class; the antenna selector (place, wording, it switches the
  channel, rebuilt options, hidden in independent mode); the rail (Map,
  Decoders, Voice toggle and light up, the panel's own ✕ and a panel opened
  from elsewhere keep the buttons right, the assistant's button with the
  assistant enabled); no overflow at 1100 px; phone: no rail, the buttons in
  the drawer, a tap opens the map, back on the desktop they return to the
  rail; no JS errors. The other suites pass unchanged (53 + 78 + 35 + 33).
- Not covered: heimdall's own page (`heimdall_v2/index.html`, a light page
  of its own) and `array_calculator.html` (already in this style).

## Tests (2026-11-15, headless Chromium, real mouse / touch events)

A TEST build on ports 2808x without heimdall. 53 checks: buttons on all 21
keyed boxes and none on the footer; move down / up; the button stays under
the pointer (also with the sidebar zoomed to 420 px); a button press does
not collapse the box, the `?` in a header neither; collapse by header and by
chevron; order and collapsed boxes kept over a reload and over a browser
restart with the same profile; boxes hidden in independent mode are skipped
and keep their relative order; disabled ends follow the mode; Collapse all /
Open all / ↺ Order; a stored order with an unknown key, duplicates and
missing boxes; a broken store; the switches (the old handlers' `className`
rewrite keeps the look); every slider row on one line; no box overflowing
sideways; phone layout (finger-sized buttons, a tap moves a box); no JS
errors.

2026-11-16, the rest of the page (35 checks, the same setup; live content
from the 🧪 Debug Playback's ADS-B test transmission): the Audio switch
toggles and keeps its knob (also moved into the phone's sidebar); every new
`?` opens inside the window and on top - spectrum header, both Viewshed
modes, Range, Replay, a decoder's description; no long note left in those
panels; the waterfall sliders still work; a decoder option button toggles
and is lit while on; short dialog labels; the independent mode's title; no
JS errors on desktop and phone. The 53 sidebar checks pass unchanged.

2026-11-16, dragging boxes (78 checks, the same setup; mouse through
`Input.dispatchMouseEvent` press / moves / release, touch through
`Input.dispatchTouchEvent` in a 400x800 phone emulation; three runs, no
flakes): drag down 5 places and up 4; the placeholder where the box lands
while dragging; order saved and kept over a reload; the dropped box in view
near where it was let go; footer last; no text selected; a plain click (also
with 3 px of jitter) still collapses, the release after a drop doesn't, the
next real click does; ↑ / ↓ and `?` still work, a press on them that moves is
no drag; first / last ↑ / ↓ disabled states after dragging the first box down
and the last one up; Escape mid-drag puts the box and the scroll position
back and the release then doesn't collapse; independent mode: drag down and
up across hidden boxes, which keep their relative order (down = right after
the visible box above, up = right before the one below); auto-scroll in a
560 px high window carries the first box past the end of the list (the last
box started off-screen) and back to the top; zoomed sidebar (420 px): the
lifted box has the slot's left edge and width, stays under the pointer, drops
where let go; phone: a swipe that starts on a header scrolls and is no drag, a
tap collapses / opens, a 200 ms press doesn't lift, a 600 ms one does with the
box under the finger and the placeholder still in place, moving drags (no
pointercancel, no scroll), drop 4 places down, touchcancel puts the box back;
no JS errors. The 53 sidebar and 37 page checks pass unchanged. Not tested:
a real phone (Android's long-press handling, iOS Safari, which has no
`moveBefore`), the hover-only grip (headless Chromium reports `hover:none`, so
the grip shows faintly there as on a touch screen).
