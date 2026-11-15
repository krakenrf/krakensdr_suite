# The sidebar: boxes, their order, and how controls are written

The left sidebar of `kraken_doa.html` is a column of boxes (`.sb-section`),
one per feature. This file covers the box chrome (collapse, move, what is
remembered) and the rules the controls inside follow. Each feature's own
controls are in that feature's topic file.

## Boxes

- Markup: `<div class="sb-section" data-sb="KEY">` with a `.sb-header`
  (`<h3><span class="sb-title">…</span>` + optional `?` help, optional badge,
  `<span class="sb-toggle">`) and a `.sb-content`. `data-sb` is the box's
  stable key in the stored layout: never rename one, and give a new box a new
  key. The footer box (Open Heimdall / Reset / the layout buttons) has no
  `data-sb`: it has no header and stays last.
- `sbBoxInit()` (comment title "SIDEBAR BOXES", runs once at script load)
  adds ↑ / ↓ buttons (`.sb-mv`) and the chevron to every keyed header, then
  applies the stored layout. A click on the header collapses / opens the box
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

## Rules for controls in the sidebar

The sidebar was reworked on 2026-11-15 at the user's request: it was too
text heavy, with explanations written on the controls.

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
- The styles are scoped to `.sidebar` (block "Sidebar look" in the CSS):
  `.btn` and `.ctrl-row` are also used by dialogs and panels, which keep
  their look. The phone layout moves top-bar controls into the sidebar
  (`#mobile-controls-slot`); their id-scoped rules win over the sidebar's.
- The sidebar is scaled with CSS `zoom` (dragged width): anything positioned
  from `getBoundingClientRect()` inside it divides by the zoom, and box
  widths are designed for 290 px.

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
