# The sidebar's boxes, and how controls and text are written on the page

The left sidebar of `kraken_doa.html` is a column of boxes (`.sb-section`),
one per feature. This file covers the box chrome (collapse, move, what is
remembered) and the rules every control and text on the page follows - the
sidebar first, then the top bar, the spectrum's header, the panels, the map and
the dialogs. Each feature's own controls are in that feature's topic file.

## Boxes

- Markup: `<div class="sb-section" data-sb="KEY">` with a `.sb-header`
  (`<h3><span class="sb-title">…</span>` + optional `?` help, optional badge,
  `<span class="sb-toggle">`) and a `.sb-content`. `data-sb` is the box's
  stable key in the stored layout: never rename one, and give a new box a new
  key. The footer box (Open Heimdall / Reset / the layout buttons, the Debug
  switch - DoA through the noise source, *reference.md* -, then the version
  line - `../../../docs/dev/version.md`) has no `data-sb`: it has no header
  and stays last.
- `sbBoxInit()` (comment title "SIDEBAR BOXES", runs once at script load)
  draws the chevron of every keyed header, then applies the stored layout
  and sets up the drag (`sbBoxDragInit`). A click on the header collapses /
  opens the box (`toggleSection`).
- **Move**: by dragging the header, and only so. The headers had ↑ / ↓
  buttons (`.sb-mv`, `sbBoxMove`, `sbBoxButtons`) until 2026-11-17, when the
  user asked for them to go ("we will only move by click dragging"); a
  header now holds its title, an optional `?` / badge and the chevron. A
  move counts only *visible* boxes: boxes hidden by the operating mode
  (`.coh-only`, `.wb-only`), the Wideband Board box and the phone-only boxes
  keep their place in the order, so a layout made in one mode still makes
  sense in another. `moveBefore` is used where the browser has it (it keeps
  focus and inner scroll positions), `insertBefore` elsewhere. There is no
  keyboard way to reorder the boxes any more (the drag is pointer-only);
  "↺ Order" in the footer box still restores the shipped order.
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
  - Not handles: the `?`, and any button, input, select,
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
    the gap, moving up right before the one below,
    so boxes hidden by the mode keep their relative order. Back at its own
    slot it returns to exactly where it was.
  - Drop: the box moves to the placeholder (`moveBefore`), the boxes open
    again and the sidebar scrolls so the dropped header stays where it was let
    go, with the `sb-moved` flash; then `sbBoxSave()`.
    A drop at the original slot changes nothing and restores the scroll
    position. Escape, `pointercancel` and the window losing focus cancel the
    same way. The click that the release makes on the header is swallowed (a
    capture listener on window, for 600 ms after the release; also after an
    Escape while the button was still down); the next real click collapses as
    usual.
  - Look: `cursor:grab` on keyed headers, a six-dot grip in the header's left
    padding, always there (opacity .55, full on hover and while dragged):
    with the buttons gone it is the only sign that a box can be moved;
    `grabbing` and no text selection on the whole page while dragging
    (`html.sb-drag-on`). No text on the controls.
- **Remembered**: the order and which boxes are collapsed, in
  `localStorage['kraken_sb']` = `{v:1, order:[keys], closed:{key:0|1}}`,
  written on every move and collapse. A box the page opens by itself (the
  Wideband Board box, from the banner's `wbOpenSection`) is open for now
  only: its stored state stays what it was (`sbBoxTransient`, saved in its
  place, also by a later move or another box's toggle) until the operator
  clicks its header or uses the footer buttons (pull request #8,
  2026-11-24). It is per browser, like the sidebar
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

The menu button at the left of the top bar (`#menu-btn`, `toggleSidebar()`)
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

## The scrollbar's room (2026-11-18)

The user reported that the sidebar going from fitting to scrolling was
"a little jarring" and asked for "the scroll bar always there, or at least
the space for it". With an 8 px classic scrollbar (Windows, Linux desktops)
every box lost 8 px of width the moment a box was opened far enough, and the
VFO card's frequency row wrapped (*operating-modes.md*, "VFO card
frequency").

- `.sidebar {scrollbar-gutter:stable}`: the room is always kept, so the
  content is 281 px of the 290 px sidebar (1 px border, 8 px gutter) whether
  it scrolls or not, and nothing moves when the scrollbar comes or goes. The
  boxes are laid out for that width: it is the only one now.
- The track is transparent (`::-webkit-scrollbar-track`; Firefox's
  `scrollbar-color: var(--l1) transparent`): the empty gutter is the
  sidebar's own background, not a strip, and with a scrollbar only the
  thumb shows. The gutter adds 8 px to the right padding's 6 px; left as it
  is (a thumb right against the boxes looked cramped).
- No `scrollbar-gutter` (Safari before 18.2): `overflow-y:scroll` instead
  (`@supports not`), a scrollbar always. Overlay scrollbars (macOS, phones,
  so the drawer) take no room either way - nothing changes there.
- The zoomed sidebar keeps the gutter in its 290 px layout (zoomed with it).
  The box drag positions the lifted box from its placeholder's
  `offsetLeft` / `offsetWidth`, so it follows; the header-only list it shows
  while dragging no longer changes the width either. The resizer strip is a
  sibling, untouched; a hidden sidebar is `display:none` as before.
- Headless Chromium's `--hide-scrollbars` still reserves the gutter, so the
  suites' sidebar measurements are 8 px narrower than before (281 px).

Tests (scratch `test_sidefix.py`, real classic scrollbars): the same
content width with the sidebar short (no scrolling) and long; no box's
left / right edge moves when opening a box starts the scrolling; the empty
gutter's and the track's pixels equal the sidebar's background.

## Rules for controls and text

The sidebar was reworked on 2026-11-15 at the user's request - it was too
text heavy, with explanations written on the controls - and the rest of the
page got the same treatment on 2026-11-16.

- **No explanations on the controls.** A label is one to three words. What a
  control does, its limits ("0 = forever", "1 ms - 60 s") and advice go in a
  `?` help tip (`<span class="help">?<span class="tip">…</span></span>`)
  or, for minor points, a `title` (or `data-tip`) - both reach touch
  screens (*How help is shown*, below). Status text says the
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
  Exceptions, icon buttons lit while on: the top bar's audio on / off (a
  speaker, the user's wish 2026-11-18) and the VFO cards' speakers.
- **Status lines**: `<div class="kv"><span>Name</span><span id=…>value</span></div>`.
  Sub-headings: `.sb-sub`. Small notes: `.sb-note` (hidden while empty).
- **Buttons**: `.btn` (`.primary` for the box's main action, `.danger` for
  destructive ones, `.sm` small). The scanners' Start buttons are blue while
  idle and red while running (by id, their handlers also rewrite
  `className`).
- Colours: only the palette's tokens (*theme.md*).
- Pictograms: the page's icon set - `icon('name')`, an icon then the label
  in a `<span>` - never an emoji (*icons.md*, 2026-11-18). The boxes'
  titles, the rail and the tabs carry those icons.
- Two CSS blocks hold the look: "Sidebar look" (scoped to `.sidebar`: the
  box chrome, the label column) and "Page look" (the same buttons, text
  boxes, sliders and switches for the top bar, the spectrum's header and its
  View popover (the display controls, once the waterfall bar), the
  panels, the map, the dialogs and the assistant; panels with a header bar,
  pill tabs and badges). Both sit before the phone rules. The phone layout
  moves top-bar controls into the sidebar (`#mobile-controls-slot`); their
  id-scoped rules win, which is why the switch selector lists that id and
  repeats its `background-image` in the on / off states (a `background`
  shorthand there would drop the knob).
- The sidebar is scaled with CSS `zoom` (dragged width): anything positioned
  from `getBoundingClientRect()` inside it divides by the zoom, and box
  widths are designed for 290 px - 281 px of content, the scrollbar's room
  always kept (*The scrollbar's room*).
- A row of ticks looks alike: the VFO card's Digital decoder, Transcribe and
  Squelch are a tick and a word each - Transcribe's speech icon went
  (the user's report 2026-11-18: only it had one).

### Outside the sidebar

- **In JS-built text** use `helpTip(html, wide)` for the `?` (a global
  function; `wide` for long texts - `initHelpTips` places the popup, also
  from inside the map's panels and the tabs under the waterfall), or a
  `title` for a minor point.
- **Panels with a long "how it works" note** (Viewshed's two modes, Range,
  Replay, a decoder's description, Transcription) carry it in a `?` next to
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

### How help is shown (2026-11-18)

The user asked to make help reachable on touch: much of the explanation
lives in `title` tooltips, and the browser shows those on mouse hover only -
on a phone, a tablet or a Pi touchscreen that text did not exist. The `?`
tips already worked on touch (a tap pins them); the titles needed an
equivalent. Now the page shows every `title` itself, in one tooltip for the
whole page (`#page-tip`, the `?` tips' look), reached three ways:

- **Mouse and keyboard**: on hover after 400 ms (`TIP_HOVER_MS`) and at
  once on keyboard focus (`:focus-visible`), under the element (over it
  when there is no room below), inside the window. It goes when the pointer
  leaves, on Esc, on a press (and stays away until the pointer moves to
  something else), on a scroll of what holds the element, on a resize.
- **Help mode** (touch, discoverable): the `?` button at the end of the top
  bar's right block (`#help-btn`, `helpModeToggle()`; on the desktop small,
  on the telemetry line beside ⚙ - the chips' line has no room, its width
  is the scanner chip's reserve; on touch layouts as big as ⚙, beside it).
  While it is on, the button is lit, a banner says "Tap a control to see
  what it does" ("Click ..." with a mouse) and the cursor is `help`. A tap
  on a control with help shows its tip and does NOT work the control (a
  capture-phase handler on `window` eats the whole press: pointer, mouse
  and touch events and the click - no click, no focus, no slider moving, no
  select opening, no switch flipping); the tip stays until the next tap (on
  it: closed; on another control: that one's); a tap on something without
  help leaves help mode and does nothing else. Esc or the button again
  leaves it. A swipe still scrolls (touchstart is not cancelled, except on a
  slider, which moves at the touch) and shows nothing. Dialogs and the phone
  drawer cover the top bar, so the Setup Settings and Save frequency dialogs
  and the drawer have the same `?` in their headers (`.hm-btn`; all of them
  light together). A press on a `?` button is not a press "elsewhere": the
  View popover, a menu or a list stays open to be explained.
- **Press and hold** (touch, the shortcut): 550 ms without moving more than
  8 px shows the tip; the click that follows the release is eaten, and
  Chrome's long-press menu / text selection for that press are cancelled. A
  quick tap still clicks; a swipe shows nothing. Not where a long press
  already means something (`TIP_HOLD_SKIP`): a sidebar box's header (it
  lifts the box, `sbBoxDrag…`), canvases (the map's long press opens its
  menu; the spectrum, waterfall and panes), the overlays over them, text and
  number boxes (text selection), sliders (a held thumb drags), the
  resizers. There help mode is the way.

Which help: the closest ancestor-or-self with a `title` / `data-tip`, as
the browser's own tooltip picks it - a `<label title>` wrapping a control
counts for the control; an empty one, or one that only repeats the
element's visible text, means none. The `?` tips are left to their own code.

How it works (the "HELP TOOLTIPS" section, `pageTipInit()`): one set of
listeners on `window` / `document`, nothing per element and no
MutationObserver, so markup built later (VFO cards, decoder tabs, map
popups) is covered without setup. At the first hover, focus or press the
`title` moves to `data-tip` and stays there (`tipAdopt`; every title up the
tree, or the browser would fall back to an ancestor's), so the native
tooltip never shows next to the page's. The accessible text is kept: a
control with no name of its own (an icon-only button) gets it as
`aria-label`, anything else as `aria-description` (on a `<label>`'s control).
A title that code sets later still works - present, it wins over `data-tip`,
and it is moved again while the pointer rests there (a canvas naming what
is under the pointer) - but **code that reads help text uses
`tipText(el)`**, not `el.title` (which is empty after the move). The bubble
(`.page-tip`, z-index 10100) is on top of everything, dialogs (2050) and the
View popover (1500) included: it only ever shows for the element under the
pointer or the finger. Plain text (`textContent`, line breaks kept), at most
320 px wide. Both kinds of tip are placed by `tipPlace()` (inside the zoomed
sidebar a fixed `?` tip divides by the zoom; the page bubble lives in
`<body>`).

**So a control needs only a `title` (or `data-tip`)** - no setup, no
per-element script - and its help works with a mouse, a keyboard and on
touch. An icon-only control without one is a gap: give it a `title` or an
`aria-label`. The audit on 2026-11-18 found the map popups' close crosses
(now `title="Close"`, `role="button"`) and the assistant's step states
(`role="img"` + Running / Done / Failed); every other icon-only control had
a name.

Tests: `test_tips.py` in the job's scratch directory (desktop: hover, the
title gone, at every edge - top bar, rail, pane headers, the sidebar's
bottom, Setup Settings, the View popover -, Tab, Esc / leave / scroll /
press, the accessibility tree, a VFO card and the decoder panel built
later, a title set by code; a phone and a touch tablet with real touch
events: help mode on buttons, switches, a slider, a select, inside the
dialog and the drawer, the backdrop, Esc; press and hold, a quick tap, a
swipe; a box header still lifts, the map's menu still opens, a held slider
still drags; no JS errors).

### Dialogs: confirm, alert, prompt (2026-11-18)

The user asked: "I don't like the browser popups we get for some options,
like when I click on "reset" on the map DF box. Instead it should be a
styled popup within the software itself, not from the browser." The page
called the browser's `confirm` / `alert` / `prompt` in 46 places (20 / 21 /
5) and the array calculator once (`prompt`). All of them are now the page's
own dialog, and the page has no native dialog call left (a rule in
`kraken_doa_v2/CLAUDE.md`).

**One component, three functions** (the DIALOGS section at the top of the
script, right after `icon()`; the markup `#ui-dlg-bg` / `#ui-dlg` is static,
filled per request):

- `uiConfirm({title, text, ok, danger, cancel, icon, tip})` -> `Promise<boolean>`
- `uiAlert({title, text, kind, icon, tip, quiet})` -> `Promise<void>`;
  `kind` 'error' (the `no` icon, red) | 'warn' (`warn`, amber) | 'info'
  (the caller's icon)
- `uiPrompt({title, text, fields, ok, icon, tip})` -> `Promise<{name: value}>`
  or `null` (cancelled). `fields`: `[{name, label, value, placeholder (a
  text, or values => text), type ('text' | 'number' | 'password'), unit,
  maxlength, validate(value, values) -> '' or the error}]`. A number is
  typed as text (`inputmode=decimal`) and checked by `validate`, as the
  old prompts' `Number()` did; a password field has a Show / Hide button
  like the Map tab's key.

The look is the Restart dialog's (*power.md*): `.sf-dlg` on a `.sf-dlg-bg`
backdrop, a title line with an icon from the set and a short name for the
thing ("Reset bearings", "Delete recording"), under it one line with the
specifics (which item, what is lost, "This cannot be undone"); a longer
explanation goes behind a `?` (`tip`, `helpTip()` - constant HTML of the
page only). Buttons right-aligned: Cancel, then the action **named by its
verb** - Delete, Reset, Remove, Forget, Clear, Discard, "Use fixed
location", Download, Create, Add, Unlock - never "OK" for a question (`cancel`
renames Cancel: the editors' "Keep editing"). An alert has one button, OK.

- **Focus**: a destructive action (`danger`: the button is `.btn.danger`)
  opens with the focus on **Cancel**; a harmless one (`.btn.primary`) on the
  action; a prompt on its first field (its text selected); an alert on OK.
  Enter activates the focused button; in a prompt Enter in a field submits
  if every field validates. Esc, the header's x and a press on the backdrop
  are Cancel (an alert: closed) - a backdrop press counts only when it went
  down and up there (its release must not land on the page under it) and
  not within 350 ms of opening (the second click of a double click). Tab
  stays inside; focus moved out by code comes back; on close the focus goes
  back to what had it (if it is still there).
- **Validation in place**: each field's error is a short line under it
  (`.ferr`, `aria-invalid`, `aria-describedby`), shown on submit and gone as
  soon as the field is right - never another dialog.
- **Text is text**: the title, the text parts (`text` is a string or a list
  of strings, `{b: text}` bold and `{code: text}` - a selectable line, e.g. a
  path) and the field values are set with `textContent` / `.value`. Names
  from the receiver, plugins, the assistant or the user show literally.
  Typed values are emptied when the dialog closes (the access token is in
  the field only while it is open, then in `localStorage` as before).
- **A queue**: one dialog at a time; requests made meanwhile wait their turn
  in the same dialog (several errors in a row: each shown, none lost). The
  same question (type, title and text) asked again while it is open or
  waiting is not shown twice - that request resolves as cancelled at once:
  a second click on a button, or a guard called twice, can't act twice.
- **quiet**: an alert nobody just asked for - the receiver's answer that
  arrives later (the scanner configuration refused, no lobe for a
  transcript) - waits while the user is typing in a field outside the
  dialog and is shown when the field is left. The browser's alert took the
  focus at once; this one doesn't take it more than that.
- **ARIA**: `role="alertdialog"` (confirm, alert) / `role="dialog"`
  (prompt), `aria-modal`, `aria-labelledby` the title, `aria-describedby`
  the text.
- **On top**: z-index 2200 - above the other dialogs (2050: a question
  raised from the Save-frequency editor or Setup Settings), the toast, the
  popovers (a press or Esc in the dialog is not "outside" for `popDismiss`:
  the View popover under it stays open), the assistant's dock and drive
  mode's map and HUD; under the power notice (3000), the help banner and
  the page's tooltip (which shows for its own controls). Keys don't reach
  the page's own handlers while it is open.
- **Help**: the `?` in the text works as anywhere; help mode and press and
  hold work on its controls (the x explains "Cancel (Esc)"); Esc in help
  mode leaves help mode first and the dialog stays.
- **Touch, drive mode, phones**: on touch layouts (`pointer:coarse`) the
  buttons are at least 44 px high; in drive mode (`body.drive`) the dialog
  is bigger (460 px, 18 px title, 15 px text, buttons at least 56 x 120 px,
  like the HUD's) and follows drive mode's Day look - the Day token block
  (`tools/theme_palette.py --css-day`) is set on `.ui-dlg-bg` too, with light
  plain buttons, the accent and a red Reset with white text. On a phone it
  fits 320 px (the backdrop's 12 px padding) and the buttons wrap rather
  than overflow.

**The callers are asynchronous now.** The browser's dialogs blocked the
page; these don't. Every caller captures what the action needs before it
asks (ids, names, the command) and checks on yes that the thing still
exists (a marker, a saved frequency, a recording, a layer deleted from
another page meanwhile: nothing is sent); what the question named is what
goes, whatever is selected by then (the DF Reset of a radio, a scanner row:
removed by identity, not by its old index). The two editors' guards -
`mkLeaveEdit()` (a map marker) and `sfDlgLeave()` (a saved frequency) -
return `true` when nothing changed, else the promise of "Discard changes?";
their callers go on through `uiThen(v, f)` (at once for `true`, so nothing
changes when there is no question): `mkSelect` (a click on another marker,
the panel's Edit, search results, Show on map), `mkCreate` (Add Marker, a
mast / search result / assistant item to a marker), `sfDlgOpen` (another
entry while one is open) and `sfDlgCancel` (the x, Cancel, the backdrop,
Esc). Inline handlers that built `onclick="if(confirm('...'))send('...')"`
call a named function now (`incClearAsk()`, `aiMarkForget()`).

The array calculator (`array_calculator.html`, its own page) has its own
copy of the look for its one case (`copyDialog`): the clipboard refused
(not a secure page, no permission) - "Copy the CSV" with the text selected
in a read-only box, Close and Copy (the older copy command), Esc / the
backdrop, Tab inside, the focus back. Its sprite carries `i-close` and
`i-doc` for it.

**Every converted site** (the old call -> the dialog: title, verb, what it
sends):

| Where | Old | Title | Text (the specifics) | Verb | Sends / does |
|---|---|---|---|---|---|
| API token (`askForToken`) | prompt | Access token (lock) | "This KrakenSDR requires an access token: the api_token file on the device"; after a refusal "Wrong access token - ..." | Unlock (password field, Show) | `AUTH:<token>`; Cancel = locked as before |
| Scanner: the receiver's refusal | alert | Scanner configuration refused (quiet) | a frequency or bandwidth is out of range | OK | - |
| Scanner: Load, not JSON | alert | Invalid scanner file | `<file>: not valid JSON` (or the field's error) | OK | - |
| Scanner: Load, display failed | alert | Invalid scanner file | `<file>: not a scanner configuration` | OK | - |
| Scanner: Save without one | alert | Nothing to save | create one with New first | OK | - |
| Scanner: New | prompt | New scanner configuration | field Name (My Scanner, 64) | Create | the configuration, as before |
| Scanner: Start without frequencies | alert | No frequencies to scan (warn) | at least one enabled frequency | OK | - |
| Scanner: Start outside Wideband | alert | Wideband mode needed (warn) | pick Wideband as the Mode | OK | - |
| Scanner: Next while stopped | alert | Scanner not running | start it first | OK | - |
| Scanner: + Add without one | alert | No scanner configuration | create or load one first | OK | - |
| Scanner: + Add | 3 x prompt + 2 alerts | Add frequency | fields Frequency (MHz, 24-1766), Bandwidth (kHz, 0.1-2400), Label (empty = the frequency) - the old error texts under the fields | Add | `SCANNER_LOAD_CONFIG:{...}` |
| Scanner: Remove (a row) | confirm | Remove frequency | `<label> (<MHz>) from the scanner list?` | Remove | `SCANNER_LOAD_CONFIG:{...}` without it |
| Decoder tab (Pagers): incidents' Clear | confirm (inline) | Clear incidents | Remove every incident (N)? | Clear | `INCIDENTS_CLEAR` |
| Assistant: a conversation's x | confirm | Delete conversation | `"<title>" and its files?` | Delete | `AI_SESSION_DELETE:<id>` |
| Assistant: an identification's Forget | confirm (inline) | Forget identification | `<name> at <MHz> MHz?` | Forget | `AI_MARK_DELETE:<id>` |
| Array calibration: Reset | confirm | Reset array calibration | Forget the N aircraft packets collected so far? (nothing collected: no question) | Reset | `ARRAY_CAL_RESET` |
| Array calibration: Remove | confirm | Remove array calibration | MUSIC and beamforming go back to the nominal array | Remove | `ARRAY_CAL_REMOVE` |
| Transcription: a bearing's lobe, the receiver's answer | alert | No lobe (quiet) | the receiver's reason | OK | - |
| Transcription: Play failed | alert | Cannot play the audio | the reason (e.g. the audio is gone) | OK | - |
| Transcription: CSV with none shown | alert | Nothing to export | No transcripts to export (none shown) | OK | - |
| Transcription: Clear | confirm | Delete transcripts | Delete all N transcripts and their audio on the receiver? The decoder log files keep theirs. | Delete | `VOICE2TEXT_CLEAR` |
| DoA Recording: Download / Replay / Delete with none selected | 3 x alert | No recording selected | Select a recording first. | OK | - |
| DoA Recording: Download failed (HTTP, error) | 2 x alert | Download failed | `<name>: HTTP <n>` / the error | OK | - |
| DoA Recording: Replay failed (HTTP, error) | 2 x alert | Could not load the recording | `<name>: HTTP <n>` / the error | OK | - |
| DoA Recording: Delete | confirm | Delete recording | `"<name>"? This cannot be undone.` | Delete | `LOG_DELETE:<name>` |
| Map menu: Set station here (GPS / mobile) | confirm | Use a fixed location (harmless) | The station's position comes from the GPS / the mobile app now. Use a fixed location here instead? | Use fixed location | `LOCATION_SOURCE:static`, then `STATIC_LOCATION:...` |
| DF panel Reset, drive mode's Reset | 2 x confirm | Reset bearings | Forget the bearings collected for `<kind> <radio>` / `D<n>` and its radios / every VFO and radio? | Reset | `RDF_RESET:<vfo>:<tid>` / `:<vfo>` / `:-1` |
| Map marker: Delete | confirm | Delete marker | `"<name>"?` Its N saved frequencies stay in the list, without a location. | Delete | `SF_LOC_DEL:<id>` |
| Map marker editor: leaving it changed | confirm (guard) | Discard changes | Discard the changes to the marker `"<name>"`? | Discard / Keep editing | the switch / new marker goes on |
| Saved frequencies: a category's x | confirm | Delete category | `"<name>"?` Its N frequencies stay in the list, uncategorised. | Delete | `SF_CAT_DEL:<id>` |
| Saved-frequency editor: leaving it changed | confirm (guard) | Discard changes | Discard the changes to the saved frequency `"<name>"`? | Discard / Keep editing | the editor closes / the other entry opens |
| Saved-frequency editor: Delete | confirm | Delete saved frequency | `"<name>" (<MHz> MHz)?` | Delete | `SF_DEL:<id>` |
| Map: an assistant's layer x | confirm | Remove map layer | `"<name>" (N items) for every page?` | Remove | `AI_MAP_DELETE:<id>` |
| Continuous scanner: Start in Wideband | alert | Coherent mode needed (warn) | leave Wideband mode first | OK | - |
| Settings: Reset Settings to Default | confirm | Reset settings | Reset all settings to their defaults? The page will reload. | Reset | `RESET_SETTINGS`, reload |
| IQ & Audio Recorder: a recording's trash | confirm | Delete recording | `"<name>"` from the receiver? This cannot be undone. | Delete | `RECORDER_DELETE:<name>` |
| IQ & Audio Recorder: > 1.5 GB without streaming | confirm | Download into memory (harmless) | the size held in memory; where to copy it from (a selectable line) | Download | the download, as before |
| Array calculator: copy refused | prompt | Copy the CSV / positions | the text, selected | Copy / Close | the older copy command |

Two of these can't be reached from the page as it is: Start outside
Wideband mode and the continuous scanner's Start in Wideband mode - their
boxes are hidden in those modes (`.wb-only`, `.coh-only`). The guards stay
(converted), for a box shown some other day.

No alert became a non-blocking notice. Candidates were the ones about the
user's own click that only say "nothing to do" (Nothing to export, No
recording selected, Scanner not running) and "No lobe" (the map's status
flash is where the map talks), but each is the answer to something the user
just did, and the user asked for styled popups - they are dialogs.

Tests (2026-11-18, scratch `test_dlg.py`, headless Chromium, real mouse,
key and touch events; 250 checks; `cdp.py`'s `watch_native_dialogs()`
records and dismisses any native dialog and the suite fails on one): the
component (destructive / harmless focus, Esc, backdrop, x, Cancel, Enter, a
double click's second press, alert, prompt with one and three fields,
validation in place, the queue of three, the same question twice, Tab and
Shift+Tab, focus back, ARIA, `<img onerror>` shown as text, the `?` tip and
help mode, over the Save-frequency dialog and the View popover, drive mode
Night and Day at 1500 and 400 px with 56 px buttons, touch 44 px and press
and hold, phones 400 and 320 px); every call site from the page's control
(a list checked against a grep of the page's calls), Cancel sending nothing
and the verb sending what the old code sent; the target gone while asked,
the list changed under a Remove, the DF selection changed under a Reset; both
editors' Discard guards on every path; the access token against the test
receiver started with `KRAKEN_API_TOKEN` (first ask, empty, wrong, Cancel =
locked, right one, remembered; the token nowhere in the DOM); the array
calculator's copy dialog; no native call left in either page.

### What moved out of boxes of their own (2026-11-17)

The user asked for fewer boxes: three were folded away, and their `data-sb`
keys (`gps`, `fft`, `digital-decoders`) are retired - never reuse them. Later
the same day (2026-11-17) the user asked for "everything relating to setting
up the software" to be in the top bar's cog dialog, now Setup Settings:
the Voice to Text box went there, and its key `voice-to-text` is retired
too. On 2026-11-19 the user asked for the Decoder Logging box to move there
as well ("Move the "Decoder Logging" settings box in the left sidebar into
the new "Setup Settings" cog icon settings area"): its key
`decoder-logging` is retired, and the sidebar has 16 keyed boxes. A layout
stored with any of them still loads (`sbBoxOrder` skips keys it has no box
for; the next save drops them). The phone drawer is the same sidebar, so it
lost the box too; the section is reached through the cog there as well.

- **GPS status** is the last part of *Station Information* (`#gps-sec`: the
  "GPS" row with the old box's `?`, then `#gps-details`; `updateGpsStatus`
  is unchanged). It is shown for every Location source: "From GPS" of a
  static location uses the same fix.
- **FFT size, Slow link, Points sent** are in *Display* (FFT size first).
- **Digital Decoders** is the Decoders tab of Setup Settings.
- **Voice to Text** is the Voice to Text tab of Setup Settings (below).
- **Decoder Logging** is the Decoder Logging section of Setup Settings
  (below; *decoder-log.md*).

**Trunk Following** (`trunk`, the `trunk` icon, 2026-11-20 -
*trunk-following.md*, "Page") was added after Saved Frequencies, in every
operating mode but sweep mode (`.not-sweep`, since the merge with main of
2026-11-25: no VFO runs there): one more keyed box. Main's Signal ID box
(`signal-id`, *signal-classifier.md*, "Page") was added at the same place;
the merge put Trunk Following first, then Signal ID - with the Sweep box the
sidebar has 19 keyed boxes. A layout stored before either box loads with
both after Saved Frequencies (`sbBoxOrder`), open. It is `display:none` until the receiver
answers `GET_TRUNK` with its status, so an older receiver leaves the sidebar
as it was (a hidden box keeps its place in a stored order, as the Wideband
Board box does). Its rarely used settings fold out under the ⚙ in its first
row (the saved-frequency scan's pattern); a system's names, talkgroups and
channel table are in its own dialog (the Setup Settings look, sections as a
strip across the top on every width). Its last part, *Control channels* (2026-11-22), holds
the control-channel search (a form folding out of its Search bands…
button) and the remembered control channels, a list kept still under the
pointer (*trunk-following.md*, "Page").

### The Sweep box (2026-11-20)

Sweep mode (*sweep-mode.md*, "The page") brought another keyed box, `sweep`,
shown only in that mode (`.sweep-only`; placed first after the phone
boxes): Resolution, Dwell, Run / Pause and what the sweep is doing. The mode
hides the boxes that need demodulation, VFOs or DoA (`.not-sweep` with the
`.coh-only` ones; the list and the reasons are in that file), and the Display
box keeps only Slow link and Points sent. In the top bar the range (Start -
Stop digits) and the tuner toggles take the frequency's place and Audio /
Step leave; on the phone the range and the tuners are rows of the drawer's
Tuner & Audio box, before Gain, with their labels. A move by drag counts the
box like any other hidden one: outside sweep mode it keeps its place.

### Array diagrams (2026-11-17)

The MUSIC DoA box's diagrams (`#uca-diagram`, `#wb-diagram`, `#ula-diagram`,
`#custom-diagram`, `#layout-diagram`) were flat SVG sketches - circles with
"CH0" inside. The user asked for something better looking that reads as a
top-down view, "maybe an actual raster png". They are rendered pictures
now, drawn on a canvas at run time rather than shipped as PNG files: the
picture depends on the number of antennas (1-8), the topology, the custom
positions and the dimension typed in the box, so no set of files could
cover it, and the page stays one file that works offline.

- `arrCanvas(container, w, h, label, draw)` makes / reuses the canvas (CSS
  size w x h, bitmap x devicePixelRatio x the sidebar's `zoom`, capped at
  4; `role="img"` + an `aria-label` that says what is shown) and paints the
  surface; `draw(c)` works in CSS px. `arrLast[id]` keeps each diagram's
  draw call and a debounced `resize` listener runs them again, so a
  dragged-wider sidebar or another screen gets a sharp bitmap. `ARR_C` (the
  palette's colours) and `arrLast` are in the globals block: a diagram can
  be drawn before the diagram section is evaluated.
- `arrAntenna()` is one antenna seen from above: the shadow it casts (the
  light comes from the upper left), a dark metal magnetic base with a rim
  in the channel's colour, and the whip end-on - a bright tip with a glow.
  `{square:true}` is a patch antenna. `arrPlate()` is the plate / rail /
  panel under them with its own shadow; `arrDim()` a dimension line;
  `arrCaption()` the "TOP VIEW" caption; `arrForward()` the arrow for
  forward (0 deg, the CH0 direction).
- UCA (also the Wideband ring): CH0 on top under the forward arrow, the
  others clockwise (`UCA_ANGLE_SIGN`), names outside the ring, the radius
  from the centre to CH1 with its value under the centre (above it when
  that line goes down: 2 or 3 antennas) - a place no antenna or name can
  be. ULA: a row on a rail, the spacing between CH0 and CH1 under the
  names (digits only from 7 antennas). Custom: X / Y axes, a dashed ring
  for raised / lowered elements. Patch / 3D: the panel "SEEN FROM BEHIND"
  with square patches, or the top view with forward marked.

Tests (scratch `test_arr.py`, headless Chromium, 35 checks): a spy on
`arrText` / `arrAntenna` for what is drawn and where (1-8 antennas: every
name inside the picture, the dimension's value on no antenna, CH0 on top
and clockwise, none touching), pixels (bright whip tips at the antennas and
nowhere else), bitmap size at 1x, 2x and in a zoomed sidebar, the Radius
box redrawing it, every topology tab, the phone drawer; and the VFO
readout's dots (*operating-modes.md*).

### Setup Settings dialog (2026-11-17)

`#set-dlg` (the top bar's cog, `setDlgOpen(tab)`) is "Setup Settings": the
user asked (2026-11-17) for everything relating to setting the software up
to be in it, under that name ("Settings" before; the cog's tooltip and
aria-label say "Setup Settings" too). It has sections (`.set-tabs` buttons
and `.set-pane`s with the same `data-t`, ARIA tabs; `setDlgTab(t)`; the
section shown last is kept while the page is open, `setDlgTabId`), in this
order: Map, Decoders, Decoder Logging, Voice to Text, Assistant, System.
The dialog has a fixed size so the sections stay put, and each pane scrolls
by itself.

**The navigation is a list down the left side (2026-11-19).** Until then
the sections were a strip of tabs across the top of a 540 x 540 px dialog;
five just fitted one line. Decoder Logging made six, which do not fit
across 540 px, and the user's rule that everything about setting the
software up goes into this dialog means more will come - a list grows
downwards without a limit worth worrying about, a strip does not. So:

- Desktop (window 640 px or wider): the dialog is 720 x 560 px (still at
  most the window: `.sf-dlg`'s `max-width` / `max-height` 100 %, inside the
  backdrop's 12 px padding). A CSS grid: the header across the top with a
  line under it, the list (`.set-tabs`, 168 px from the dialog's edge: a
  154 px column plus the 14 px of padding it reaches into; a darker
  background, `--s2`, a line on its right) and the shown pane to its right.
  A row is 34 px: the icon, then the name (cut with an ellipsis if a name
  ever gets too long), the warning marker (`#set-tab-dec-n`,
  `#set-tab-declog-n`, `#set-tab-v2t-n`) at its right end. The selected row
  has the accent bar on its left edge (`--ac`), the lit text the strip had
  (`--ac-t`) and a lighter background (`--s4`); a hovered row `--s3`. The
  panes lost 168 px of width, not height; the Map pane's Test / Cancel /
  Save stay at its bottom.
- Keyboard: the list is a vertical `tablist` (`aria-orientation`
  "vertical"; "horizontal" while it is the strip, a `matchMedia` listener);
  Up / Down move to the previous / next section and show it (automatic
  activation, wrapping at the ends), Left / Right do the same (the strip
  had no arrow keys at all before), Home / End go to the first / last. Only
  the selected row is in the Tab order (roving `tabindex`), so Tab goes
  from the list into the pane.
- Phones (window under 640 px): the list is the strip across the top again
  (`flex-direction:row`), and it **wraps**: at 400 px Map, Decoders,
  Decoder Logging on the first line and Voice to Text, Assistant, System on
  the second; at 320 px the same two lines - three while a tab carries its ⚠ (the tabs' side padding is 6 px,
  5 px below 420 px). Wrapping was kept over a sideways-scrolling strip,
  which would hide a section behind the dialog's edge (a fade or an arrow
  could show it, but six names in two lines are all visible at once, and
  two lines cost ~30 px of a phone's height). With touch (`pointer:coarse`)
  each tab is at least 40 px high.

- **Map** (`map`): the tile provider and its key - *map.md*. Test /
  Cancel / Save belong to this tab only (Enter in one of its fields saves;
  Enter elsewhere in the dialog does nothing). The other tabs have nothing
  to save: what they show is the receiver's state, and a tick acts at once.
- **Decoders** (`dec`): the voice codecs (`#dig-codecs-box`,
  `digOnCodecs`, ↻ Check = `PLUGIN_CODECS`) and the decoder plugins
  (`#ai-plugins`, `aiRenderPlugins`, ↻ Rescan = `PLUGINS_RESCAN`, the Auto
  detect ticks) - *digital-decoders.md*. The list is always open (the box
  had it collapsed); `#set-tab-dec-n` on the tab counts plugins that need
  attention.
- **Decoder Logging** (`declog`, the `log` icon; the sidebar box
  `decoder-logging` until 2026-11-19 - *decoder-log.md*). The box's
  controls in the dialog's patterns, with the box's element ids: on top a
  card in the codecs' look (`#dl-card`) holding the master switch - renamed
  from "Log to disk" to **"Allow logging to disk"**, the user's wording -
  with its state beside it (`#dl-state`: Off / Logging / Not writing), the
  box's `?`, the problems as warning lines and the status rows (Logging -
  what is logging now, "D0, D2 · transcripts" -, Folder, Disk free + its
  bar, Today, Per day, All logs, Disk lasts; `.kv`); then *Files* -
  Keep (days) and Then compressed (days) side by side, then Save to (the
  drive list, the folder + Apply) as labelled fields. Positions every (s)
  was the first field until 2026-11-19, when the user asked for it to move
  into the decoders that need it (a field in their row; shown here only for
  an older receiver); Then compressed came the same day (plain files for
  Keep days, gzip for Then compressed days, then deleted - *decoder-log.md*). *What to log* (the type ticks in two
  columns, one on a phone) was removed on 2026-11-19 at the user's request:
  the switch is a permission and what is written is chosen where the data
  is - each decoder's Logging (Off / Log / Log All) in its tab, the
  transcripts' in the Transcription panel (*decoder-log.md*); the block
  shows only for an older receiver, which still obeys it, with the line
  "Per-decoder logging needs the updated program". Each control acts at
  once, as in the box (Apply stays the folder's). A problem while logging
  is on (not writing, records dropped, the disk nearly full) puts a ⚠ on
  the section's row (`#set-tab-declog-n`) and a line with a Decoder
  Logging button in the decoder tabs under the waterfall; the links and
  tips that named the box point at the section (*decoder-log.md*).
- **Voice to Text** (`v2t`; the sidebar box `voice-to-text` until
  2026-11-17 - *voice-to-text.md*). On top a card in the codecs' look
  (`#v2t-card`, `v2tRenderSetup`) with the state from the voice2text
  status: *Checking…* / *Not set up* (`v2tInstallHtml()`: the command for
  the receiver's OS, or the Windows package's note) / *No speech model* /
  *Set up* (the model, how many are downloaded); the box's `?` is in the
  card's header. Then the box's controls in the dialog's own rows (`.lbl`
  above, `.row`, `.sec` headings): *Speech model* - Model, Download + Get
  and its progress line, Language + → English (multilingual models only),
  CPU cores + speed, Words to expect + Apply; *Audio* - Skip non-speech and
  Keep the audio as switches (name left, switch right: `label.swr`; they
  were plain ticks in the box), the clips line; *Status* - Status, VFOs,
  Waiting, Transcribed, To disk (`.set-dlg .kv`, the sidebar's look). The
  element ids are the box's, so `v2tRenderBox` drives them as before; each
  control sends its command when it changes. The box header's badge
  (`#v2t-badge`: "N VFOs", or "state" while VFOs were ticked and Voice to
  Text was not installed / had no model / failed) became a warning on the tab
  (`#set-tab-v2t-n`, titled with the state) for the problem case; the VFO
  count is the VFOs row. The box's "⤢ Transcripts" button was dropped: the
  rail's Transcription opens the panel. A VFO card's "Not installed"
  (or "Error") and the "Setup Settings" link in the Transcription
  panel's warning open the dialog on this tab (`setDlgOpen('v2t')`).
- **Assistant** (`ai`, `setDlgAi()` -> `#set-ai`): set up on the
  receiver or not, from `ai_state`, in the codecs' look: *Not set up*
  (install Claude Code, then `kraken_ai.py setup`) / *Claude Code not found*
  (enabled, backend "claude", no CLI) / *Set up* (the CLI's version or the
  backend; the dock's two warnings - no API token, no sandbox). Drawn again
  when `ai_state` or the receiver's OS arrives (`aiOnState`, `setServerOs`),
  so a dialog left open follows a setup done meanwhile.
- **System** (`sys`, `setDlgSys()` -> `#set-sys`, the `power` icon;
  2026-11-18): one card, "Restart and shut down from the page" - each of
  Restart software / Reboot computer / Shut down offered or why not, and
  for what the user can allow, the command (`./install-pi-service.sh
  --power-only`) - *power.md*, "What the page shows". Drawn again on every
  power state.
- **Add-ons** (`addons`, `setDlgAddons`, `#set-addons`) was the third tab
  until 2026-11-17: a card for the assistant and one for Voice to Text. It
  was split into the two tabs above - the Voice to Text card is the top of
  its tab, next to the controls it is about.

Tests (2026-11-17, scratch `test_v2t.py`, headless Chromium, real mouse,
key and touch events; 96 checks): no Voice to Text box (17 keyed boxes),
every moved control once and inside the tab; the cog's tooltip, the title,
the four tabs in order, on one line inside the dialog at 1700 and 400 px
and wrapped, uncut and inside at 320 px (also with the warning on the tab); the
receiver's "not set up" card, the `?` on screen; simulated states (set up,
no model, error with escaped text, multilingual, a download running,
Windows source tree / package) with the receiver's messages held: card,
rows, controls filled, the tab's warning only while VFOs are ticked; every
control's command from a click, Enter or a change (Enter in Words to expect
doesn't close the dialog); a real Transcribe tick on D0 -> the card's
"Not installed" link and the panel's link open the tab; a stored layout
with `voice-to-text` loads and the next save drops it; nothing overflows its
pane at any width; no JS errors. The other suites pass with outdated
expectations updated (box count 18 -> 17, the tab list, the Assistant tab in
place of Add-ons, the dropped Transcripts button).

Tests (2026-11-19, scratch `test_declog.py`, headless Chromium, real mouse,
key and touch events): no Decoder Logging box (16 keyed boxes), every moved
control once and inside the section, a stored layout with
`decoder-logging` loads; each control's command; the status in each state
(off, on, disk nearly full, an error) from fixtures; the list - six
sections in order, vertical at 1700 and 1280 px, the dialog's size and that
it fits a 1280 x 720 window, the selected marker, Up / Down / Home / End,
ARIA, every pane without sideways overflow, the Map pane's buttons at its
bottom, the section remembered, `setDlgOpen('declog')`; phones at 400 and
320 px (the wrapped strip, every section reached by a tap, the pane usable
with touch sizes); the pointers that named the box; the problem noticed
from the decoder tab and the list; no JS errors, no native dialog. The
suites that counted boxes, tabs or measured the dialog got new expectations
(box count 17 -> 16, six sections, 720 x 560, the list's rows).

### Top bar and rail (2026-11-16)

The user found the top bar cluttered, busy and ugly; the mode chip
duplicated the Mode selector; "CH" did not say that it picks the displayed
channel; and the panels' buttons should sit on the right-hand bar with the
Assistant button. What it is now (CSS block "Top bar and right-hand rail"):

- **Order** (2026-11-18): the menu button, the frequency, then the fields -
  **Gain, Audio, Step, Mode** -, the Bias tee button (`#tb-bias`: icon only,
  *bias-tees.md*) and at the right `.tb-right`: the status chips above the
  telemetry chips. The user asked for Step "just to the left of the mode"; it
  was first (Step, Gain, Audio, Mode). The separators are gone.
- **Icons, not captions** (2026-11-18): the user asked to "remove all text
  like 'step', 'gain', 'audio', 'mode' on the top control bar, and do
  everything with icons instead" - the controls' own values stay text (the
  readings, the selects' options). A field is a `.tb-group.fld`: an icon
  (`.tb-ic`, 16 px, `--t3`) 6 px left of its control, the fields 12 px apart
  (the bar's gap), so a field reads as one object. Gain: `gain` (an
  amplifier triangle with a line in and out); Audio: the speaker button
  itself; Step: `step` (two marks with a double arrow between them); Mode:
  `array` (the array's ring of elements - the mode is how its tuners are
  used); the Wideband variant's mixer: `mixer` (a circle with a cross, the
  RF mixer symbol). The icons are not buttons: `aria-hidden`, with a short
  `title` naming the control (a hover or a help-mode tap on the icon says
  "Gain", "Tuning step", "Operating mode"; the mixer's carries its whole
  help). Each control carries its name (`aria-label`: Gain, Volume, Audio,
  Tuning step, Operating mode, Bias tee) and a `title` that starts with it.
  **This is the bar's rule, not the drawer's**: on the phone the relocated
  fields are drawer rows and keep their text labels (`.tb-label` - still in
  each group's markup, hidden in the bar, shown in `#mobile-controls-slot`;
  the icons are hidden there). Mode stays in the phone's bar as icon +
  select.
- **One centre line**: the selects, the value bars, the speaker and the
  Bias tee button are 28 px high on the bar's centre line, and so are the
  icons.
- **Value bars for Gain and Volume** (2026-11-18). After the readings had
  gone above the sliders (right-aligned with them, the same day) the user
  wrote: "The dB and % text looks out of place, and the two sliders being
  different lengths and designs looks odd." Shown a mock of three options -
  A value bars, B the gain as a dropdown of its 29 steps (the volume a value
  bar), C two identical thin sliders with the value only in a bubble while
  hovered or dragged - they chose A: "ok change to value bars". So both are
  now one control, `.vbar`: a box like the dropdowns (28 px, the `--l2`
  border, radius 5, `--s1`, the dropdowns' 12.5 px text in `--t0`, tabular
  numbers) with its value written inside, centred ("49.6 dB", "Auto",
  "50%"), and a fill from the left up to the value (`--ac-d2` -> `--ac-d`)
  with a 2 px `--ac-l` edge at its end. Nothing floats beside or above it;
  both bars are 96 x 28 px. The text has a soft shadow; its contrast is
  10.7 over the fill's dark end, 6.6 over its bright end, 15.0 over the
  empty part (3.5 / 5.2 dimmed).
  - **Built on the native range input** (`#gn-slider`, `#vsl`, the same ids,
    `oninput` handlers, commands and rate limits as before), styled as the
    whole box: `appearance:none`, laid over the box inside its border, no
    track, the thumb the 2 px full-height edge, the fill the input's own
    background, as wide as `calc((100% - 2px) * var(--p) / 100 + 2px)` - so
    it ends exactly at the edge. Keyboard (arrows, Page Up / Down, Home /
    End), screen readers (role slider, `aria-valuetext` = the shown text),
    touch and every script that sets `.value` keep working. At the top
    (`.max`) the edge is not drawn: it would double the box's right border.
    At the bottom there is no fill and the edge sits at the left. Firefox
    draws the fill with `::-moz-range-progress` (an `@supports
    (-moz-appearance:none)` block; not tried in a Firefox, there was none at
    hand). The text (`.vbar-t`, the old reading's id `#gn-value` / `#vv`)
    lies over the input with `pointer-events:none`, `aria-hidden`.
  - **One function shows a value**: `vbarShow(input, text)` sets `--p` (the
    value in % of the range), `.max`, the text and `aria-valuetext`.
    Callers: `updateGainDisplay` (the top bar's gain - so its own callers: a
    drag / key press, the wheel, the receiver's header `specHeaderSync`, the
    selected pane's gain in independent mode (two frame paths and
    `paneSelect`), the saved frequencies' gain `sfApplyGain`), `uv` (the
    volume), `panesSyncHeads` (each pane's gain), and `applyMobileRelocation`
    through `vbarShowAll` (every bar's fill from its value: at start-up and
    when the layout switches between the bar and the phone drawer). The
    markup carries the start values (`--p:100` / `--p:50`, `.max`).
  - **Operation**: press or drag anywhere inside the box - the value follows
    the pointer and the edge lands under it (the input's travel is the box
    less its 1 px border and the 2 px edge: 92 of 96 px, so a press at 10 %
    of the box gives 8 %, at 90 % 92 %; the first pixel inside the border is
    the minimum, the last the maximum). The **mouse wheel** (`vbarWheel`, a
    non-passive listener on each bar, `preventDefault`: the page never
    scrolls) steps one notch at a time like the frequency digits: the gain
    one entry of its table, the volume 5 % onto the multiples of 5
    (`data-wheel="5"`; 52 -> 55). A gain change costs the receiver a
    recalibration pause, so the top bar's gain shows each notch at once
    (the echo guard `gainUserChanging` set) but sends only when the wheel
    has rested 250 ms (`GAIN_WHEEL_MS`), through `sgn` and its 100 ms rate
    limit: several notches, one GAIN command for the last value. A drag
    sends as before (`sgn`: every 100 ms and the final value). Cursor
    `ew-resize`; focus ring as the dropdowns' (`--ac-l` border and a soft
    ring) on keyboard focus (`:focus-visible`). Titles: "Gain - ...; drag,
    or use the mouse wheel", "Volume - ...". Press-and-hold help stays off
    sliders, so a bar on a touch screen is always a control.
  - **Audio off**: the volume bar is dimmed (opacity .55, `.tb-spk.off +
    .vbar`, no script) and still adjustable; the speaker button is
    unchanged. Until the value bars it was not dimmed.
  - **Phone drawer** (Tuner & Audio): the same bars, as wide as the row
    allows and 34 px high (the speaker's height); the rows keep their text
    labels. The readings above the sliders went with `.tb-pair` (which only
    the Wideband mixer field still uses).
  - **Independent mode's panes**: each header's gain is the same control,
    84 x 22 px (the View button's height, 12 px text) in place of the 70 px
    slider and the value beside it - the page has one gain control, not two
    designs (*operating-modes.md*). The header stays on one line (38 px at
    the 432 px panes of a 1700 px window; the bar saves 36 px), a click in
    it still selects the pane, the wheel on a pane's bar selects it too.
  - Not changed: the View popover's sliders (label / slider / value rows),
    the sidebar's sliders, drive mode.
- **The speaker button** (`#fmt`, 2026-11-18, the user: "change the audio
  mute button to a speaker symbol"): the audio on / off is a 30 x 28 icon
  button (`.tb-btn.tb-spk`) - the `speaker` symbol, lit (`--ac`) while the
  audio is on, `mute` (the crossed speaker), unlit, while off;
  `aria-label="Audio"`, `aria-pressed`, title "Audio on / off". Not a
  switch any more (no `data-sw`, no ON / OFF text): `tfm()` still sends
  `FM:0` / `FM:1` and rewrites `className` (`tb-btn tb-spk on` / `off`), and
  now also the icon and `aria-pressed`; the receiver's echo
  (`applySyncCmd('FM:…')`), drive mode's Audio button and the VFO cards'
  speakers (`.mute-all`) follow `fmEnabled` as before. The volume bar dims
  while the audio is off (still adjustable - since the value bars). On the
  phone the drawer's Audio row has the same button, 42 x 34.
- **Width budget at 1700 px with the sidebar** (bar 1371 px, 1347 px of
  content): ☰ 30, frequency 276, Gain 118 (icon 16 + 6 + a 96 px value
  bar), Audio 132 (speaker 30 + 6 + a 96 px value bar), Step 108, Mode 128,
  Bias tee 30, seven 12 px gaps, the right block 436 (its reserve): 4.6 px
  to spare, with or without the scanner chip (8.6 px with the 112 / 76 px
  sliders before the value bars, 2026-11-18; two 98 px bars would leave
  0.6 px). History: removing the switch saved 25 px, the three icons cost
  66; the bar's gap went from 14 to 12 px, and the sliders got back 12 / 4
  of the 20 / 8 px the Bias tee button had taken (120 / 80 until then,
  100 / 72 since the button).
  Narrower, the bar wraps by whole fields (1500, 1300: the right block on a
  second row at the right; 1100: Mode and Bias tee move down beside it).
  The Wideband variant's mixer field (`#wbv-group`, ~253 px: its icon, the
  ring / LO readout right above the side buttons and the LO drive, right
  edges together) does not fit on one row at 1700 px - it did not before
  either (it needs ~250 px more, the WB board telemetry chip more still):
  the controls keep the first row and the right block wraps under them as
  a whole. Styled from a forced display only, no downconverter was at hand.
- **Tests** (2026-11-18): `test_bar.py` in the job's scratch directory (164
  checks since the value bars, real mouse / key / wheel / touch events): no
  caption text in the bar; the order by x, Step one gap left of Mode; every
  control's name and title, the icons `aria-hidden`, resolved in the
  sprite, 16 px and 6 px from their controls; one centre line (1 px) and
  one height (28 px). The value bars: identical size; the dropdowns' box
  and text; the range input covering the box, role slider,
  `aria-valuetext`; the text centred and unclipped at "Auto", "0.0 dB",
  "22.9 dB", "49.6 dB", 0 / 5 / 50 / 100 %, nothing moving; the fill and
  edge read off screenshots (pixel rows) at the minimum, a middle value and
  the maximum, in the top bar and in a pane; contrast; presses at 10 / 50 /
  90 % and the far ends, a drag across (the GAIN commands, rate-limited);
  the wheel (one step a notch, four quick notches = one GAIN for the last
  value, the page not scrolled, the volume's 5 %); arrows / Home / End and
  the focus ring; the receiver's header and `paneSelect` moving fill and
  text; dimmed while the audio is off and still adjustable. Then the
  speaker (FM:0 / FM:1, icon, lit, `aria-pressed`; from the receiver; from
  drive mode's button and back); help mode names each control and icon
  (and a help-mode tap on a bar changes nothing); one row at 1700 px with
  the scanner chip; the Wideband field; the wraps at 1500 / 1300 / 1100
  px; independent mode (the panes' bars: size, one line, a press, the
  wheel - one TUNER_GAIN -, the header's click-to-select and View button);
  the phone bar, the drawer's rows (34 px bars, a tap and a finger drag,
  dimmed while off) and back to a desktop width; no JS errors.
- **Chips** (`.status-badge`): a dark pill with a dot in the state's
  colour - the scripts still set the chip's `background` to that colour;
  an inset shadow darkens the pill and the dot (`::before`) inherits it.
  Left to right: the scanner (only while a scanner runs - no "Idle" chip),
  the calibration (fixed width; the cooldown countdown shows in it:
  `updateCooldownBadge`), the connection. So a chip that appears or changes
  its text never moves a control or another chip. No mode chip: the Mode
  selector stands next to them. Remove a chip's element only together with
  its script: the countdown's timer was inside `if (cooldownEl)`.
- **`.tb-right`** is a grid: the chips on its first line, the telemetry on
  its second, and the Setup Settings button (`#settings-btn` - "Setup
  Settings dialog" above) in a column of its own as tall as both lines (46 px wide, a
  26 px icon - the user asked for it "two rows big", 2026-11-17).
  `min-width:436px` reserves the room of one chip that comes and goes (the
  scanner's 95 px, or the RTL-SDR chip's 93 px: 95 + 170 + 104 + the gaps +
  the cog's column), so such a chip can't wrap the bar or move a control. To
  keep the bar on one row at 1700 px with that, the bar's gap was 14 px and
  the Gain / Audio sliders 120 / 80 px (12 px and 112 / 76 px since the
  icons, 2026-11-18 - *Width budget* above). With generic RTL-SDR dongles
  an "RTL-SDR" chip (`#status-generic`) stands first. The phone layout puts
  the cog (42 px) at the end of the chips' line.
- **Merged with main 2026-11-17** (map providers' Settings dialog - Setup Settings now -, generic
  RTL-SDR mode): main had put the cog after the chips and "(RTL-SDR)" on the mode
  chip - with the cog on the chips' line the scanner chip wrapped the bar at
  1700 px, hence its own place in the grid and the reserved width. Main's new lines
  were brought into the palette the way the page was converted (*theme.md*;
  only literals that the converted page didn't already hold), and the
  dialog's long labels (the tile URL's placeholders, the attribution, the
  note about where the key is kept) moved into `?` tips and titles. 31
  checks for the merged parts; the six other suites pass.
- **Bar colour**: the calibration state still colours the bar, but lightly -
  a faint tint and a 2 px line at its lower edge (`.top-bar.cal-*`); the
  chip names the state. The playback's red line (`body.pb-on`) wins over it.
- **The antenna shown** (`#chs`, was "CH" in the top bar): a selector on the
  spectrum's header reading "Antenna N" (`updateChannelSelector`), next to
  the title, which is now "Spectrum · <demods heard>" (`updatePlotTitle`).
  Coherent mode only. The phone layout no longer moves it into the drawer.
- **The tuners shown** (`#pane-pick`, 2026-11-24): independent mode's
  buttons in the same place - `T0`..`Tn` lit while that tuner's pane is
  shown, then `All` - `.view-btn`s without the header's `margin-left:auto`;
  they wrap under the title on a phone. *operating-modes.md*.
- **The display controls** (Scale, Zoom, Speed, Colors, Min, Max, Auto)
  were moved from a bar under the waterfall into the spectrum's header on
  2026-11-17 (the user's wish; the waterfall now reaches the panel's
  bottom), where they wrapped to two or three lines. The same day they went
  behind the header's **View** button, in a popover - "View button and
  popover" below.
- **The rail** (`#rail`, at the right edge after the assistant's dock): one
  button per panel - `#map-btn` (DoA & Map, `mapToggle`: the whole
  right-hand pane, MUSIC DoA included - *map.md*), `#rail-dec` (Decoders) /
  `#rail-voice` (Transcription - "Voice" until 2026-11-17)
  (`railPanel`: opens the panel under the waterfall on that feature, or
  closes it when it already shows it; the panel shows the decoders' tabs or
  the transcripts alone, never both - *voice-to-text.md*; the decoders' tabs
  are only those in use, with "All decoders" after them, a popover listing
  every installed decoder - since 2026-11-18, *digital-decoders.md*) - lit while their
  panel shows (`railSync`, called from `infoRenderTabs` and
  `infoPanelClose`; Transcription carries the count of unseen transcripts; on the
  phone the three share a row of the drawer's Panels box and wrap when it
  is too narrow), and `#rail-ai`, the assistant's, always there
  (`aiDockToggle`; lit while its dock is open - by CSS from the body
  classes; while the assistant is not set up on the receiver it opens a
  dock that only says how to set it up - *ai-assistant.md*). It replaces the collapsed assistant's strip (`#ai-strip`) and
  the three buttons at the end of the old waterfall bar. The phone layout has
  no rail: `applyMobileRelocation` moves `#rail-panels` into the drawer's
  phone-only box `mobile-display` (where the buttons were before; titled
  "Panels" since 2026-11-17, when the display controls it also held
  went behind the View button - the key stayed), the assistant keeps its
  floating button.
- **Drive** (`#rail-drive`, 2026-11-18, *drive-mode.md*): at the rail's
  foot (`margin-top:auto`), apart from the panels - it switches the whole
  page to drive mode, it is not a panel; in the phone layout it goes into
  the drawer's Panels box under the panel buttons, full width
  (`MOBILE_RELOCATIONS`). The other rail buttons did not move
  (`test_icons.py`'s baseline gained only its entries).
- Tests (48 checks, headless Chromium, real mouse / touch): the bar's
  content and order, one row at 1700 px, labels above controls, the chips
  at rest; the scanner chip appearing and going without anything moving;
  the calibration chip's texts at a fixed width, the cooldown in it, the
  bar's class; the antenna selector (place, wording, it switches the
  channel, rebuilt options, hidden in independent mode); the rail (Map,
  Decoders, Voice toggle and light up, the panel's own close button and a panel opened
  from elsewhere keep the buttons right, the assistant's button with the
  assistant enabled); no overflow at 1100 px; phone: no rail, the buttons in
  the drawer, a tap opens the map, back on the desktop they return to the
  rail; no JS errors. The other suites pass unchanged (53 + 78 + 35 + 33).
- Not covered here: `array_calculator.html` (already in this style).
  heimdall's own page (`heimdall_v2/index.html`) was a light page of its
  own until 2026-11-18; it now follows these rules and the palette -
  `heimdall_v2/docs/dev/web-ui.md`.

### View button and popover (2026-11-17)

The user asked to "compress the Scale / Zoom / Speed / Colors / Waterfall
min/max etc into a smaller space", because in independent mode each tuner
should have its own settings in its own header - "a simple settings button
to press to open these settings as a box that floats just underneath the
button". Script block "VIEW POPOVER", CSS `.view-btn` / `.view-pop`; the
per-tuner model is in *operating-modes.md*.

- **The button**: `#view-btn-main` on the spectrum's header -
  `[Spectrum · WBFM] [Antenna 0] ...... [View] [?]`, one line again at every
  width down to the narrowest spectrum (400 px) and on a phone. A "sliders"
  icon (inline SVG in `currentColor`, three tracks with knobs; `VIEW_ICON`)
  and the word View. Not the cog (that is Setup Settings), and not
  "Display": the sidebar's Display box holds other settings (FFT size,
  averaging, edge clip). In independent mode every tuner pane's header ends
  with the same button, icon only (`#tpane-vb-N`, title "View: scale, zoom,
  waterfall - tuner N"), after the VFO badges; its click also selects the
  pane (the header's click handler). `aria-haspopup="dialog"`,
  `aria-expanded`; lit (accent) while its popover is open.
- **The popover** (`#view-pop`, `role="dialog"`, `aria-label` = its title):
  "View" (single spectrum), "View · all tuners" (the header's button in
  independent mode) or "View · T2". About 250 px wide: SPECTRUM - Scale,
  Zoom (the points' spacing, `#zoom-res`, small under the zoom value);
  WATERFALL - Speed, Colors, Contrast (its `.sw` switch is Auto), Min,
  Max (the waterfall contrast, below). A label
  column, the control, the value right-aligned in a 48 px column with
  tabular numbers, so nothing jitters while dragging. While Auto is on, Min
  and Max are disabled (dimmed) and show what Auto set, moving with it -
  before, they could be dragged and the next auto tick (500 ms) put them
  back. The old element ids are kept (`fft-ymax`, `zoom-sl`, `wf-speed`,
  `wf-colormap`, `wf-min`, `wf-max`, `wf-autorange`, their value spans,
  `wf-controls-bar` on the popover's body).
- **One set of controls with a target** (`viewTarget`: `'main'` or a tuner
  number; null = closed). The controls hold no state: `viewCommon` (the
  common values) and `viewStore` (the tuners' own) are in the globals block,
  every reader goes there - the receiver's replay (`applyUiSetting` ->
  `viewOnRemote`), `panesBuild`, `updateWaterfallRange`,
  `autoRangeWaterfall` (it wrote the Min / Max sliders, now
  `viewCommon.min / max`), the pane drawing (`paneYMax`) - and
  `viewPopSync()` fills the controls from the target when it opens and when
  a value changes. So a replayed value can never land in a control that
  shows another tuner. The receiver relays every `UI:` command to every
  page, the sender too: the page notes what it sent (`viewEcho`, 5 s) and
  takes those echoes as its own, so an older echo arriving during a slider
  drag no longer needs the focus guard the controls had.
- **The element moves to its button** (`btn.after(pop)`), so Tab goes from
  the button into the controls in order; it goes home after
  `#view-btn-main` when it closes. Placed `position:fixed` from the
  button's rect (the panes scroll inside `#indep-grid`, which clips; the help
  tips work the same way): under it with the right edges aligned; left-
  aligned when that would leave the window, above the button when there is
  no room below (the bottom pane of a small window); always inside the
  window (8 px margin). One open at a time.
- **Closes** on a second click of the button, Esc (the focus goes back to
  the button), a press outside the popover and its button, a scroll of
  anything holding the button (the tuner grid; the phone's page), a change
  of the window's size, an operating mode change and `panesTeardown` (the
  panes rebuilt). The page dispatches `resize` itself after layout changes
  (the right-hand pane, the sidebar, the mode): when the window's size did
  not change the popover stays and is placed again under its button. Not
  modal, the page is not dimmed: the spectrum and waterfall change while a
  slider is dragged.
- **Shared with the page's other popovers** (2026-11-18, when the map's
  Layers popover made it the third - *map.md*): two helpers in VIEW
  POPOVER. `popPlace(pop, rect, {align, within, m, shrink})` places by the
  button - View aligns the right edges (as before), Layers the left edges
  inside the map pane and, with `shrink`, gets shorter and scrolls rather
  than going above its button. `popDismiss({btn, pop, close, place})`
  installs the closing ways once per popover (a press outside it and its
  button, Esc with the focus back, a scroll of what holds the button, a
  window size change; a page resize event places it again) and returns
  `opened()`, which records the window's size. View, All decoders
  (*digital-decoders.md*; its placement stays its own - it fits the list's
  height to the room above or below) and Layers use it; the behaviour of
  the first two is unchanged (`test_view.py`, `test_hdr.py`,
  `test_contrast.py`, `test_dectabs.py` pass as before).
- **Phone**: the same button (32 px) and popover (`min(300px, 100vw -
  24px)`, rows 38 px, 22 px thumbs). `wf-controls-bar` is no longer in
  `MOBILE_RELOCATIONS`; the drawer's phone-only box `mobile-display` (key
  kept) holds only the three panel buttons and is titled "Panels".
- **The waterfall contrast** (2026-11-17): the user asked to "make it
  clearer that "auto" and "min" "max" are for the waterfall contrast" -
  they were three bare rows. Now one group (`.vp-grp`, `role="group"`
  named by its label): the row **Contrast** with the switch and the word
  "Auto" beside it (the switch's meaning is unchanged: on = Auto), and Min
  / Max under it in `.vp-sub`, indented, with a 1 px `--l3` rule down
  their left side. The indent (`--vp-ind`, 12 px) comes out of their
  label column, so their sliders and values stay in the columns of Scale /
  Zoom / Speed; the popover's width is unchanged (250 px; phone
  `min(300px, 100vw - 24px)`). The columns are CSS variables on
  `.view-pop` (`--vp-lw` / `--vp-vw`: 46 / 48 px, 50 / 52 px on a phone).
  The Contrast row's label column is `minmax(--vp-lw, max-content)`, so
  a wider font pushes the switch right instead of clipping the word. The
  wording is in the `title`s ("Waterfall contrast: the signal levels
  shown as the darkest and the brightest colour. Auto sets them from the
  signal levels."; Min / Max: "... the level shown as the darkest /
  brightest colour"). Ids, behaviour and commands are as before.

Tests (2026-11-17, scratch `test_view.py`, headless Chromium with real
mouse, key and touch events; 96 checks): the header's order and one line at
1700 px with the right-hand pane open, without it, with a long title, at
1300 and 1100 px; the button's icon, wording, aria; the popover under the
button, right-aligned, on top, inside the window at 1700 / 1300 / 1100 px;
its rows, columns and headings; every control by mouse or key with what it
sends (a spy on `send`) and stores (`localStorage`); echoes and another
browser's change; every way to close it, a page `resize` without a window
change keeping it; Tab / Enter / Space / arrow keys / Esc; independent
mode: the pane buttons, T2's Scale / Speed / Colors / Auto / Min / Zoom
change T2 only and nothing is sent, the auto tick leaves its range alone,
the popover's values follow the target, a replayed unchanged common value
keeps T2's own values and a changed one drops that key's, T2's values over
a reload, "all tuners" sets every pane and drops the own values, tuners
that are not there, broken storage, the panes rebuilt, one popover at a
time; a 1300 x 640 window: the right-most pane, the grid scrolled by the
wheel closes it, the bottom pane opens it above its button; phone: the
header, a tap opens it inside the screen, a finger drags a slider, a tap
outside closes it, the Panels box; no JS errors. `test_hdr.py` (the
header) was rewritten for the button (28 checks); `test_page.py` opens the
popover before it drags Min.

The waterfall contrast (2026-11-17, scratch `test_contrast.py`, 62
checks; `test_view.py` now expects the label Contrast and Min / Max's
labels indented): on the desktop with Auto on and off, for T1 in
independent mode and on a 400 px phone - the rows, the popover's width
(250 / 300 px), nothing outside it and no clipped label, the switch and
the word Auto on the Contrast row with the switch in the controls column,
Min / Max in the group below it, indented 8-16 px, the `--l3` rule over
their height inside the label column, their sliders' edges and values
within 1 px of Scale / Zoom / Speed; the tooltips; a mouse click on the
switch, on the word Auto and on the label toggles Auto (`UI:WF_AUTO`; T1's
own value, nothing sent), Min / Max disabled and dimmed with Auto on; a
tap on the phone; no JS errors.

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
from the Debug Playback's ADS-B test transmission): the Audio switch
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
next real click does; `?` still works and a press on it that moves is
no drag; the first box dragged down and the last one up; Escape mid-drag puts the box and the scroll position
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

2026-11-24, the automatic reveal (pull request #8; 11 checks, the page from
file:// in headless Chromium over the DevTools pipe, no receiver; the
reveal through a click on `#wb-banner`): a collapse by header click is
saved; the reveal opens the box and leaves the stored 1; another box's
toggle keeps it; after a reload the box is collapsed again; after a reveal
a header click collapses (1) and the next opens (0), both saved, and the box
stays open over a reload; Open all after a reveal saves it open; Reset
clears the store; no JS errors from the sidebar code. Main before the change
failed exactly the three reveal checks (the reveal saved 0, the box came
back open after a reload).
