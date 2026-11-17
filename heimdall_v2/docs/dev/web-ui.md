# The web page (port 8070)

`heimdall_v2/index.html` is heimdall's own page: the array's calibration,
tuning, the noise source and checks, the element count, RTL-TCP, the
Wideband board, forward compensation and the live graphs. This file covers
how it is served, its layout and look, and how it was tested.

## Serving

- `html_loader.cpp` reads `index.html` from the working directory **once at
  startup** (`main.cpp`; in generic RTL-SDR mode once more after the dongles
  are open, so `{{SERIALS}}` lists them) and replaces `{{NUM_DEVICES}}`,
  `{{CORRELATION_SIZE}}`, `{{REF_CHANNEL}}` and `{{SERIALS}}` everywhere in
  the file - don't write those strings anywhere else in it. An edit needs a
  heimdall restart (no rebuild); the page also runs with the placeholders
  unreplaced (`parseTemplateVar`, the serial fallback).
- `web_server.cpp`: `GET /*` the page (no-store), `GET /s2p_files`, `POST
  /s2p_upload?name=`, `POST /s2p_delete?name=` (Origin checked), and the
  WebSocket: a binary frame per broadcast (`build_correlation_message` in
  `correlation.cpp` <-> `processData`) and a `STATE:{json}` text frame on
  connect and at a low rate (`build_state_message` <-> `handleTextMessage`).
- The page sends plain text commands: `FFT_ENABLE` / `_DISABLE`,
  `BIAS_TEE_ENABLE` / `_DISABLE`, `PER_BIN_ENABLE` / `_DISABLE`,
  `ANT_BIAS_MASK:<mask>`, `SDR_SETTINGS:{"frequency":Hz}` /
  `{"gain":dB|-1}`, `RTL_TCP_CHANNEL:<n>`, `NUM_ELEMENTS:<n>`,
  `PERIODIC_RECAL_ENABLE` / `_DISABLE`, `PERIODIC_RECAL_PERIOD:<min>`,
  `CHECK_CAL`, `FORCE_RECAL`, `FWD_COMP_ENABLE` / `_DISABLE`,
  `FWD_COMP_AMP_ON` / `_OFF`, `FWD_COMP_FILE:<ch>:<file>`.
- It must work without internet: uPlot is inlined (the first `<script>` and
  `<style>`, vendored unchanged), no fonts or files from elsewhere. Server
  text goes into the page with `textContent` only.

## Look (2026-11-18)

The user asked for the two pages to match: heimdall's page had kept its old
light look with emoji headings and paragraphs of explanation after the
client page was redesigned. It now follows the client's palette and rules:

- **Palette**: the same tokens with the same values in the page's own
  `:root` (`--s0..s5` surfaces, `--l1..l5` lines, `--t6..t0` text, `--ac-*`
  accent, `--ok-* / --warn-* / --bad-* / --spec-*` states); the reasoning and
  the roles are in `../../../kraken_doa_v2/docs/dev/theme.md`. Every colour in
  the page's CSS is a `var()`; three page values sit after the tokens in
  `:root` (`--on-fill` white text on a fill, `--sw-on` the client's green of
  a switch that is on, `--shadow`). Washes and rings are `color-mix()` of a
  token. The graphs (canvas) take the tokens' hex from `PAL` in the script.
  The channels' line colours (`hsl(ch * 60, 70%, 50%)`) are data colours and
  stayed as they were.
- **Text**: the client's "Rules for controls and text"
  (`../../../kraken_doa_v2/docs/dev/sidebar.md`): labels of one to three
  words without colons, states in a few words, everything else in a `?` help
  tip (`<span class="help">?<span class="tip">…</span></span>`, opened on
  hover, focus or a tap and placed inside the window by `tipPlace`) or a
  `title`. Readouts that carry numbers (the phase line under the graph, the
  Wideband board) stay as they are. No emoji: the few icons are inline SVG
  drawn for this page in the client's icon style (16 x 16, `stroke-width
  1.6`, `currentColor`; `ICONS` in the script, filled into
  `[data-ico]`).
- **Controls**: the client's looks - `.btn` (`.primary` the card's main
  action, `.danger` destructive), switches (`input.sw`) for every on / off
  setting, rows of a 108 px label column, a control and a unit, status lines
  (`.kv`), cards with a header bar like the client's sidebar boxes, status
  chips as dark pills with a dot in the state's colour; Arial 14 px, titles
  13.5 px, labels and controls 12.5 px.

## Layout

- **Top bar**: "Heimdall", then the chips - `Wideband` (only with
  `--wideband`), the calibration, the connection (`#txt`: Connecting… /
  Connected / Disconnected / No data) - and at the right **Open DoA Client**
  (`https://<host>:8080/`, the counterpart of the client's "Open Heimdall
  Server") and the version (`#version-line`, details in its title). The bar
  is sticky on the desktop.
- **Banners** under it, a state in a few words with the rest in the title:
  KerberosSDR (`#kerberos-banner`, the client's wording: uncalibrated /
  calibrating / stale say to disconnect ALL antennas and run Full
  Recalibration) and the mode (`#mode-banner`: wideband scan, independent,
  generic RTL-SDR with the receiver count).
- **Left column** (330 px): Status (config, update rate, what is active),
  Tuning, Calibration Settings (noise source, FFT graphs, per-bin EQ, the
  periodic check and its counters), Array Elements (the count, the bias
  tees), RTL-TCP, Wideband Board (only with `--wideband`), Forward
  Compensation. **Right**: Calibration (the channel table, Check Calibration,
  Full Recalibration, the last check), Phase History, Cross-Correlation. The
  page is at most 1480 px wide, centred. The graphs follow their card's
  width (a resize handler calls `setSize`).
- **Phone** (<= 860 px, or a coarse pointer): one column - Calibration,
  Status, the control cards, then the graphs -, the table scrolls sideways
  inside its frame, buttons / selects / switches at least 32 px, the `?` has
  a 36 px touch area, and the top bar scrolls away with the page.
- What hides when: generic RTL-SDRs hide the noise source, per-bin EQ, the
  periodic check, the bias tees, the Array Elements card and the calibration
  buttons (`applyGenericState`); the Wideband variant hides the bias tees;
  plain `--kerberos` disables the noise source, the periodic check and Check
  Calibration.

### The calibration chip

`updateCalChip` names the phase calibration's state in the client's words and
colours (its `updateCalIndicator`): Waiting (grey), Measuring (amber),
Applying (azure), Verifying (violet), Converged (green), Cooldown (amber),
with " (Noise On)" while the noise source is on. The state is the low byte of
`status_info` in the binary frame (one value for every channel); heimdall
sends 1 for state 0 while the lag servo runs, so 1 with a channel whose lag
hasn't converged reads as Waiting. State 6 (the per-bin EQ measurement) has
no name in the client and reads "Per-bin EQ" (amber). Outside coherent mode
and with generic RTL-SDRs the chip says Disabled. The bar takes a light wash
and a 2 px lower edge of the state's colour (`.top.cal-*`, as the client's
`.top-bar.cal-*`). The card's own chip (`#comp-status`) says whether
compensation is running (Compensating / All stable).

### Where the old page's parts went

| Old | Now |
|---|---|
| Title, Wideband badge, Config line, Status line (connection text + FPS line with emoji flags) | Top bar; `#config-info` and `#stats` / `#stats-flags` in Status; the frequency, gain and RTL-TCP channel the FPS line repeated are in their cards |
| System Controls: FFT, Noise Source, Per-Bin, Periodic Calibration Check (checkboxes, coloured status boxes, notes) | Calibration Settings: switches with their state beside them, notes in `?` |
| Antenna Bias Tees | Array Elements, "Bias tees" (a switch per channel) |
| SDR Settings ("Current: …") | Tuning ("Frequency now", "Gain now") |
| RTL-TCP Output Control | RTL-TCP (Source, Server, Streaming) |
| Array Elements (+ hardware notes) | Array Elements, notes in its `?` (with the Windows `--kerberos_sw` line) |
| Forward Phase Compensation (S2P) | Forward Compensation; the explanation in `?`, the folder as a line |
| Compensation Status (table, buttons, notes, last check) | Calibration; column notes in the header titles and the card's `?` |
| Phase History, the correlation plots, the zoom hint | Phase History, Cross-Correlation (zoom hint in its `?`) |
| Version line at the bottom | Top bar, right |

Every element id of the old page is kept, so the script's logic is the old
one; what changed is markup, CSS and the texts it writes.

Small fixes made on the way: the phase graph's x axis was a time axis (it
showed "1/1/70" times) and counts updates now; "No data" (was "Waiting for C
program...") came up whenever the per-second frame counter had just
restarted, so it uses a total now; the lag table's states read Measuring /
Servo / Converged instead of upper case.

## Tests (2026-11-18)

No heimdall could run for this (it needs the hardware, and the live one was
not to be touched). The page was tested in headless Chromium against a
static copy with `html_loader`'s substitutions, with `WebSocket` replaced
before the page's scripts (DevTools `Page.addScriptToEvaluateOnNewDocument`)
by a fake that the test feeds STATE text frames and binary frames built in
the `build_correlation_message` layout, plus fake s2p routes and recorded
`confirm` / `alert`. Scratch `test_heimdall.py`, 173 checks, real mouse, key
and touch events:

- the palette equals the client's (all 45 tokens); no colour literal in the
  page's CSS outside `:root`; every `var()` defined; the script's only hex
  colours are `PAL`'s; no colour in the markup; uPlot and the template
  placeholders unchanged; no external resource;
- every id of the old page (66 + 49 per channel) exists, no duplicates; all
  18 command words of the old page are sent by a control here - each switch,
  button, Enter key, select (by keyboard) and the confirm-cancel paths;
  upload, delete and rescan hit the same routes;
- fixtures: connecting, no data, disconnected and the automatic reconnect;
  lag measuring, servo, phase measuring, applying, verifying, cooldown,
  per-bin EQ, converged (chip text and colour, the bar, the table's colours,
  the controls locking and unlocking); a drifted / no-verdict check and a
  broken STATE; KerberosSDR uncalibrated / calibrating / stale / calibrated /
  with switches; the Wideband variant (badge, card, 6668 MHz range, an
  alarm); wideband scan and independent mode; `--ext_noise`; Windows; 4, 5
  and 8 channels; generic RTL-SDRs with 3 and 1 dongles; server text with
  HTML in it everywhere (escaped); the file with its placeholders unreplaced;
- text: nothing over 40 characters outside tips, readouts and banners;
  labels, titles, buttons and chips one to three words, no colons; no emoji
  in the file or on the page; icons 16 x 16 / 1.6 / currentColor; the type
  scale and the surfaces' colours computed as the client's;
- every `?` opens on hover inside the window, a click pins it, a click
  elsewhere, Escape and scrolling it off close it; it follows a scroll;
- 1900, 1280, 400 px: no sideways scroll, no overlapping cards or parts of a
  row, nothing out of a card, the graphs inside their cards, at most 1480 px
  centred; on the phone one column with every target >= 32 px, tapped tips
  inside the screen, a tapped switch sends its command; no JS errors.

Looked at next to the client's screenshots: every fixture at 1280 px, the
1900 px page, the phone page and its tips. Not seen: a real heimdall and
real hardware states (only the fixtures above), Firefox / Safari, a Pi's
display.
