# Operating modes, VFO cards, VFO tuning step, spectrum zoom

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Operating modes (top-bar Mode selector):**
- `OPERATING_MODE:coherent|wideband|independent` (control_handler.cpp,
  persisted after FREQ/GAIN/DECIMATORS; `WIDEBAND_MODE:` is the old form):
  refused except coherent on the downconverter variant; stops the discrete
  scanner outside wideband and the continuous one outside coherent; sends
  heimdall `set_operating_mode`; entering independent re-sends the saved
  per-tuner tuning. `apply_operating_mode_state(mode)` is the one place the
  flags (`wideband_mode_enabled`, `independent_mode_enabled`,
  `multi_tuner_mode()`) change: leaving coherent parks DoA
  (`doa_enabled_before_wideband`), returning restores it; DOA:/BEAMFORMING:
  outside coherent only update what is restored. It broadcasts
  `{"operating_mode":{"mode","tuners":[{f,g}]}}` (also sent on connect).
  The coherent `active_channel` (CH selector) is saved on leaving coherent
  and put back on return (the first VFO heard - 🔊 - moves it to its tuner meanwhile).
  The handler saves the mode itself (`SettingsStore::record`): OPERATING_MODE
  is in `is_query_command` (no sync_cmd echo), which also skips the generic
  save - it was never remembered, so each cold start the replayed default
  switched heimdall (which had restored independent) back to coherent
- Page: only the `operating_mode` message switches the mode at once; the FFT
  frame format and system_status flags are hints (`opHint`) that must disagree
  for 1.5 s - stale frames around a change used to tear the panes down and
  rebuild them per message. Pane resizes are debounced (120 ms) and skipped
  when the grid size is unchanged; canvas sizes are only re-assigned when they
  change; `WaterfallDisplay.resize` keeps the picture; the grid reserves its
  scrollbar gutter (`scrollbar-gutter: stable both-edges`); the top bar's
  hw-stats has a fixed width so it can't re-wrap the bar
- `TUNER_FREQ:ch:mhz` / `TUNER_GAIN:ch:db` (independent only) -> heimdall
  `set_independent_tuner`; stored in `indep_tuner_freq_hz` / `indep_tuner_gain_db`
  and persisted as the composite `TUNERS:f/g,...`. `exact_tuner_hz(ch)` =
  the requested frequency when the header's float agrees (~64 Hz resolution)
- `note_server_mode()`: the data receiver reads heimdall's mode from the
  phase-state flags (0x400 / 0x800, once seen) and adopts a mismatch that
  outlasts the client's own change by 3 s (heimdall restored its saved mode)
- Multi-tuner gates (`multi_tuner_mode()`): every channel converted and
  FFT'd, MUSIC/beamforming off, each VFO decimates `tuner_channel`, FFT squelch
  reads that tuner, digital RF label = `exact_tuner_hz(tuner) + offset`
- Squelch outside coherent: the FFT method on the VFO's own tuner (pipeline,
  fm_only indicator and the audio gate), no beamformed-FFT squelch (a saved
  "beamforming on" read a stale overlay); the Digital squelch works in every
  mode (it only needs the decoder - see *Digital squelch*). The VFO cards
  rebuild on a mode change (`decListMode` is part of updateDecimatorList's
  structure check)
- VFO tuner: `SET_DECIMATOR_FREQ:id:khz[:tuner]` (independent: the offset is
  relative to that tuner; the echo carries `tuner`), `decimator_info[].tuner`,
  DECIMATORS snapshot field 10
- Selected VFO (`activeDecimatorId`, all modes): `selectDecimator(id)` -
  clicking its card in the Decimators box (pointerdown anywhere on it) or
  grabbing its tuning bar (main display or pane); the card gets an outline in
  the VFO's colour (`.dec-item.sel`, `decHighlightSelected`, re-applied after
  every list update; a removed VFO hands the selection to the first). A click
  on empty spectrum places this VFO, and only its edges resize
- VFO card frequency (the user's request 2026-10-20: wheel-tunable like the
  top bar; first 14 px, then "double the size" 2026-10-21): digits
  `G.MMM.kkk.hhh` down to 1 Hz (`.vfd-box` / `.vfd`, 28 px - the top bar's
  size - in 16 px cells, "MHz" inside the box; a row of its own without a
  "Freq:" label, 236 of the card's 241 px with the ✎), click the
  top / bottom half or a wheel notch = +- that digit (`vfdInit` / `vfdStep`,
  selects the VFO); clamped inside the VFO's band like a typed frequency
  (coherent: `maxOffsetHz`; independent: its OWN tuner - a typed one may move
  to another, `paneTuneVfoTo`), never retunes the receiver. Sent at most
  every 100 ms + the last value (the drags' `setDecimatorFreq` debounce
  waited for the spin to end), echoes ignored 1.5 s after a step (`vfdHeld`
  in updateDecimatorList). A wheel gesture stays with what it began on: one
  that started scrolling the sidebar keeps scrolling when digits slide under
  the pointer (capture listener, 400 ms of quiet ends a gesture), and a
  tuning gesture only starts where the pointer last MOVED over the digits
  with the box still there (`vfdHover`): Chrome scrolls the sidebar for a
  notch over a part without a blocking wheel handler (the card title) and
  only then hit-tests it - the very first notch of a scroll landed on the
  digits now under the pointer and retuned the VFO by 1 GHz (clamped). ✎ swaps in the old text box
  (`dec-actual-freq-<id>`, Enter / blur = apply, Escape = cancel); locked
  with the Freq / BW fields by fixed-frequency / `lock_vfo` decoders
  (`digRenderCards`). Test: job scratch t_vfd.py (headless Chrome over CDP,
  fake WebSocket, 33 checks) + t_vfd_zoom.py (sidebar zoom 1.8)
- VFO tuning STEP (the user's request 2026-10-22, "a global setting up on
  the top bar"; kraken_doa.html block "VFO tuning step"): the top bar's
  `Step:` (`#tb-step` / `#vfo-step`, every mode, moved into the sidebar on
  mobile like CH / Audio) - 1 / 10 / 100 / 500 Hz, 1 (default) / 2.5 / 5 /
  6.25 / 8.33 (25/3) / 10 / 12.5 / 25 / 50 / 100 kHz, per browser
  (localStorage `kraken_vfo_step`, `vfoStepHz`). A tuning-bar drag (main
  spectrum + waterfall: `pointerMove`; panes: `paneMove`) or a click on the
  spectrum (`centerActiveBarAt`, the pane click) puts the VFO on a multiple
  of the step of its ABSOLUTE frequency from 0 Hz (`vfoSnapOffset(centre,
  offset, max)`: centre = `vfoCenterMhz` / the pane's tuner; the nearest
  grid point inside the band, else the clamped offset to the Hz). A drag
  follows the pointer from where it was pressed (it used to move only once
  the change passed 1 kHz, then restart from there: whole pixels just over
  1 kHz - ~1093 Hz on a 1760 px plot - and offsets on no round frequency);
  at 1 Hz it moves pixel by pixel. SHIFT while dragging = fine: 1/100 of
  the movement (`VFO_FINE`), to the Hz, off the grid - re-anchored where
  the bar is whenever Shift changes (`dragFine`, `paneDrag.fine / fx /
  fmhz`). `paneMoveVfo(p, d, mhz, snap)`: 'step' / 'fine' / none - typed
  (`paneTuneVfoTo`) and saved frequencies, tuner moves and a pane press on
  the VFO where it is stay exact; the card's digits / ✎ too. Test: job
  scratch e2e/t_step_ui.py (headless Chromium on the Pi against the test
  build, real mouse events incl. Shift): 19 / 19 - selector + labels +
  default, 12.5 kHz drag on the grid with every value sent and the
  receiver's, 1 Hz = 3 px of frequency, Shift 1/100 off the grid, Shift
  let go mid-drag back on it, a 25 kHz click, 8.33 kHz grid, the card's 1
  Hz digit, kept over a reload, pane drag / Shift-drag / typed frequency;
  t_bw_ui.py 24 / 24 again. The top bar's height unchanged at 1366 / 1920
  / 2560 px windows (e2e/t_topbar_w.py)
- FFT message type 6 (independent; pages without spectrum views - see
  *Spectrum views* for type 7): `u32 6, u32 nch, f32 avg_alpha, f32
  sample_rate, f32 bin_step`, per tuner `u32 center_hz, f32 gain_db, u32 ds,
  u8 min[ds], u8 max[ds]` (bin i at center - sr/2 + i*step)
- UI (kraken_doa.html, "OPERATING MODE" block): `setOpMode` / `applyOpModeUI`
  (body classes `mode-*`; `.coh-only`, `.wb-only`, `.indep-only` elements),
  `#indep-grid` panes (`panesBuild`; flex rows sized by `panesLayout`: up to
  3 per row - 5 tuners = 3 + 2, 4 = 2 x 2 -, the last row centered, >= 360 px
  wide / 230 px tall; each head has a digit frequency selector like the top
  bar's - click top/bottom half or mouse wheel, `paneFreqSelectorInit`, own
  `.pfd` class since the top bar queries `.freq-digit` page-wide - plus a gain
  slider; canvas spectrum `paneDrawSpec`, a
  `WaterfallDisplay` each, overlay `paneDrawOverlay` with VFO bars + AI
  badges - tuning bars in the main display's style: a grab handle centred in the spectrum AND the waterfall section, edge handles; `paneHit` = handle > active VFO's edges > body, the edges drag the bandwidth (any width like `drawBox`, tooltip), bars are clipped to the plot area and a clipped side keeps its edge `PANE_CLIP_EDGE_PX` 5 px inside the plot border (`bars[].xl / xr`; the drag is relative to where it started, so it resizes alike - until 2026-10-21 a clipped side had none, and a 2.4 MHz VFO, wider than the pane's edge-clipped band, couldn't be resized at all: the user's report); an FFT squelch is drawn like `drawBox`'s - dashed yellow line at the level inside the bar, on the spectrum, labelled - and is draggable (all modes: `squelchHitMain` / `paneHit` kind 'squelch' within 5 px of the line, highest priority, ns-resize cursor; `squelchSetFromDrag` moves the card slider, sends DEC_SQUELCH_LEVEL at most every 80 ms + the final level, 0-80 dB); pointer handling `paneDown/Move/Up`: a VFO bar drags the VFO, empty spectrum pans the tuner like the main display - throttled TUNER_FREQ every 50 ms, final value on release, the drawn spectrum follows the data, not the drag - and a click without moving places the selected VFO); the top bar's frequency
  digits and gain (`.not-indep`) are hidden in independent mode - each pane
  has its own (`selTuner` = the highlighted pane; `sendFreqChange` / `sgn`
  still route to TUNER_FREQ / TUNER_GAIN for it), `vfoCenterMhz(dec)` for VFO absolute
  frequencies, per-pane waterfall auto-range (`computeAutoRange`)
- Zoom (kraken_doa.html "SPECTRUM ZOOM" block): `{level 1..ZOOM_MAX, off}`
  per display - `mainZoom` (coherent / wideband) or each pane's `p.zoom`;
  ZOOM_MAX 32 (16 until 2026-10-22, the user's request; the slider 0-125).
  Past ~16x a wide plot gets fewer FFT bins (146.5 Hz) than pixels - the
  spectrum is magnified, not finer (32x on a 1600 px window: a 60 kHz
  window, 410 points, one per bin), the tuning bars are (~34 Hz a pixel).
  Test: job scratch e2e/t_zoom32.py (headless Chromium, real wheel events:
  slider, wheel stops at 32x, the server's frames for the 60 kHz window at
  one point per bin with the tone in it, a pane to 32x; 12 / 12);
  the server sends each page the window it shows at its plot's width
  (*Spectrum views*, type 7: zoomed in = more detail, same data rate) and
  `zoomWindow()` / `specWindow()` cut / pad the latest frame to it; with a
  server without views `zoomSlice()` cuts the visible part out of the data
  BEFORE it reaches the plot / waterfall (`applyClip`, `indepOnFrame`), so
  spectrum, tuning bars and waterfall share it. Mouse wheel = x1.25 around the cursor (`zoomWheel`; on
  #fp / #wf-container, or a pane's overlay - which selects it); the Zoom
  slider (wf-controls bar, log2 x 25) shows / sets the main or the selected
  pane's zoom (`zoomSyncSlider`)
- `applyClip(newFrame)`: ONLY a frame from the receiver (`applyClip(true)`)
  adds a waterfall line; zoom / zoom slider / edge-clip slider redraw the
  last frame without one (the waterfall just takes the new range for its
  overlay + clicks), and a wheel notch that changes nothing (`zoomWheel`
  returns false at 1x / ZOOM_MAX) redraws nothing. Every redraw used to push the
  frame into the waterfall again: it ran 2.5-3.5x fast while zooming
- Split: `#fp-split` drags the main spectrum height (`kraken_fp_h`,
  localStorage; window resize refits uPlot); each pane's `.tpane-split` sets
  the grid's `--spec-frac` (shared by every pane, `kraken_pane_split`);
  double-click = default. Pane digits are 22 px (1.5x)
- The assistant's `capture_iq` (`ai_capture.cpp`) and `ai/sigtool.py
  capture` record the tuner whose band holds the frequency (nearest centre)
  outside coherent; sigtool's `--channel N` forces one

