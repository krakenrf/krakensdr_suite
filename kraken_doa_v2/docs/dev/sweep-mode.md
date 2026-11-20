# Sweep mode

The fourth operating mode (Mode selector: Coherent / Wideband / Independent /
**Sweep**), the user's request of 2026-11-20: the chosen tuners step through a
frequency range and the receiver stitches ONE wide spectrum and waterfall out
of the steps. It is not live and nothing is demodulated: no VFO audio, no
decoders, no DoA. The top bar's tuning digits are replaced by the range
(start, stop) and a selector for the tuners that take part - more tuners = a
faster sweep, and each of them needs an antenna.

Not the same thing as wideband scan mode (tuners side by side around one
centre, live, VFOs work) or the `--wideband` downconverter variant (where,
like the other non-coherent modes, sweep mode is refused: every tuner is
parked at the IF).

Three parts, one section each below: heimdall hops the tuners
(`heimdall_v2/docs/dev/operating-modes.md` has its side), kraken_doa turns
the hops into the sweep spectrum, the page shows it.

## The contract (wire formats; fixed before the three parts were written)

### The grid

- Sample rate fs = 2.4 MS/s. FFT size N = a power of two, 128 ... 16384;
  bin = fs / N.
- Every hop contributes its centre `U = N * 51 / 64` bins (79.7 % of the
  tuner's band; the edges roll off), so the hop spacing is the same for
  every N: `SWEEP_STEP_HZ = 1 912 500` Hz, an integer.
- Range `[start, stop]` in integer Hz, `24 000 000 <= start < stop <=
  1 765 000 000` (the last hop's centre then stays below the tuners'
  1766 MHz limit), `stop - start >= 100 000`.
- Hops `H = max(1, ceil((stop - start) / SWEEP_STEP_HZ))`. Hop h's tuner
  centre is `start + SWEEP_STEP_HZ / 2 + h * SWEEP_STEP_HZ` (integer Hz).
- Sweep bin g is centred at `start + g * bin`. Hop h fills bins
  `[h * U, (h + 1) * U)`: FFT bin k (0 = -fs/2 after the shift) goes to
  `g = h * U + k - (N - U) / 2`. Bins reported: `min(H * U, floor((stop -
  start) / bin) + 1)`.

### heimdall, 8092

- `{"command":"set_operating_mode","mode":"sweep"}` - `OperatingMode::SWEEP`
  (3). Calibration parks as in the other non-coherent modes. Refused on the
  downconverter variant and while the discrete scanner runs.
- `{"command":"set_sweep", ...}` - the plan; accepted in every mode, used in
  sweep mode, persisted. Fields (each optional; the three plan fields only
  together):
  - `"first_center_hz":F0, "step_hz":S, "steps":H` - hop h's centre is
    `F0 + h * S`. F0 and the last centre inside the tuner range, S 100 kHz
    ... 2.4 MHz, H 1 ... 20000.
  - `"tuners":[0,1,4]` - the tuners that take part: unique indices below
    the element count, at least one.
  - `"dwell_ms":D` (5 ... 5000, default 20) - how long the settled stream
    runs on each hop.
  - `"settle_ms":T` (0 ... 1000; default `SWEEP_SETTLE_MS` in config.h) -
    wait between the retune and the flush that precedes the dwell.
  - `"run":true|false` - false pauses the hopping (tuners stay put).
  Reply `{"status":"success","sweep":{...}}` (the status object below) or
  `{"status":"error","message":"..."}`; nothing changes on an error. A plan
  or tuner change restarts the sweep at round 0.
- Who measures what: K' = min(selected tuners, H); the k-th selected tuner
  (k = 0 ... K'-1, in ascending index order) covers the contiguous hops
  `[floor(k * H / K'), floor((k + 1) * H / K'))`. Round r (0 ... R-1,
  `R = ceil(H / K')`): every such tuner retunes to its hop number
  `base + r` if it has one, else it stays where it is. Tuners that take no
  part are never retuned. Contiguous blocks, not interleaved: the tuners'
  floors differ by a few dB, which then shows as K' - 1 steps instead of a
  comb.
- One hop (every tuner of the round together): `retuning_in_progress` = 1 ->
  flush -> retune (parallel through `for_each_tuner`) -> settle -> flush ->
  `retuning_in_progress` = 0 -> dwell.
- In sweep mode `set_frequency` and `set_independent_tuner` are refused,
  `set_gain` sets every tuner (the common gain), `start_scanner` and the
  element-count change are refused.
- Leaving: to coherent every tuner returns to `current_frequency` (which
  the sweep never changed) and the array recalibrates (KerberosSDR: its
  manual-calibration rules); to independent the tuners stay where the sweep
  left them.
- Status JSON (2 Hz broadcast and `get_status`): `"operating_mode":"sweep"`
  and always `"sweep":{"first_center_hz","step_hz","steps","tuners":[..],
  "dwell_ms","settle_ms","run","round","rounds","sweeps","round_ms"}`
  (`sweeps` = completed since the plan changed, `round_ms` = the measured
  time of a round).

### heimdall, 8091 (sweep mode)

- Phase-state flags `0x2000` (sweep) together with `0x800` (independent): a
  client that does not know sweep mode treats the stream as independent
  tuners (no DoA on it). A client that knows it tests `0x2000` first.
- Per-channel frequency = where that tuner is (as in independent mode);
  per-channel gain = the common gain.
- `frequency_change_counter` + 1 per round, `current_group_index` = the
  round r the tuners are on, `retuning_in_progress` as above.
- The guarantee the client builds on: a packet with `retuning_in_progress`
  = 0 holds only samples taken at the header's per-channel frequencies,
  after the settle. (The header is a float32: ~128 Hz coarse at 1.7 GHz -
  the client finds the hop as `round((f - F0) / S)`.)

### kraken_doa, WebSocket commands (text)

- `OPERATING_MODE:sweep` - mode index 3. Refused on the downconverter
  variant and during a playback, like the other non-coherent modes; allowed
  with generic RTL-SDRs, also with one.
- `SWEEP_RANGE:<start MHz>:<stop MHz>` - clamped into the grid's limits.
- `SWEEP_TUNERS:<i,j,...>|all` - the tuners that take part; indices that
  are not there are dropped, none left = all.
- `SWEEP_RES:<N>|auto` - the FFT size (128 ... 16384); `auto` (0) picks
  one from the span.
- `SWEEP_DWELL:<ms>` - 5 ... 1000, default 20.
- `SWEEP_RUN:<1|0>` - run / pause. Not persisted: entering the mode runs.
- All but `SWEEP_RUN` persisted. Each is answered by the `sweep` message
  to every page, never by an echo.
- `SWEEP_VIEW:<seq>:<lo Hz>:<hi Hz>:<points>` - transport level, per
  connection, like `FFT_VIEW`: the window this page shows and its plot
  width in device pixels (at most `SPECTRUM_VIEW_MAX_POINTS`). `lo >= hi` =
  the whole range. Junk is ignored.

### kraken_doa, messages to the page

- JSON, on connect and on every change: `{"sweep":{"active":bool,
  "start_hz","stop_hz","min_hz","max_hz","tuners":[..],"available":N,
  "res":0|N,"fft_size":N,"bin_hz","bins","hops","rounds","dwell_ms",
  "running":bool,"grid":id}}`. `active` = the receiver is in sweep mode;
  `res` = what was asked for (0 = auto), `fft_size` = what is used;
  `available` = the tuner count. A page takes this message as "the
  receiver knows sweep mode" (an older receiver never sends it).
  Optional, added by the page worker 2026-11-20: `"gain_db"` = the common
  gain (heimdall's `set_gain` value, -1 = AGC). Sweep mode has no type 7
  frame whose header carries the gain, so a page shows another page's gain
  change only through this field; a receiver without it leaves each page's
  gain control as that page set it (nothing else depends on it).
- `{"operating_mode":{"mode":"sweep",...}}` as for the other modes.
- Binary type 10, the sweep spectrum for one page's view (little-endian,
  64-byte header), sent to every socket that sent a `SWEEP_VIEW`, at most
  20 a second while hops arrive, and once when a sweep completes:

  | Offset | Type | |
  |---|---|---|
  | 0 | u32 | 10 |
  | 4 | u8 | flags: 1 = the requested window (else the whole range), 2 = this frame completes a sweep (one waterfall line), 4 = paused, 8 = no data yet |
  | 5 | u8 | tuners taking part |
  | 6 | u16 | 0 |
  | 8 | u32 | the view's seq |
  | 12 | u32 | sweeps completed on this grid |
  | 16 | f64 | range start Hz |
  | 24 | f64 | range stop Hz |
  | 32 | f64 | centre of point 0, Hz |
  | 40 | f64 | point spacing, Hz |
  | 48 | f32 | progress of the sweep under way, 0 ... 1 |
  | 52 | f32 | duration of the last complete sweep, s (0 = none yet) |
  | 56 | u32 | grid id (changes with the range or the FFT size: the page drops its waterfall) |
  | 60 | u32 | n |
  | 64 | u8[n] | level: 0 ... 255 = 0 ... 120 dB over the sweep's noise reference, max-pooled over the point's bins |

  Points: the bins whose centres lie in the window, max-pooled as type 7
  does (*Spectrum views*). A bin that was never measured is 0.
- Levels: ONE reference for the whole sweep (never one per hop: a signal
  that fills a hop must keep its height), the tuners' common in-band shape
  removed (`nfloor::shape_db`, *Noise floor*).

## heimdall

(heimdall's side: `heimdall_v2/docs/dev/operating-modes.md`, "Sweep mode".)

## The receiver (kraken_doa)

(written with the sweep engine)

## The page

`kraken_doa.html`, script block "SWEEP MODE" (after "SPECTRUM VIEWS"); its
state (`swp`, `swpKnown`) is in the globals block, because the View popover,
the zoom slider and `applyOpModeUI` read it. Prefix `swp` (the page already
says "sweep" for the path loss and range sweeps, `plSweep`, `rgSweep`).
Written 2026-11-20 against the contract above, before the receiver's engine
existed.

**The Mode selector.** A fourth option, Sweep (`value="sweep"`). It is
hidden and disabled until the receiver has sent its `sweep` message
(`swpKnown`, `opModeOptionsSync`): an older receiver never sends one, and
then nothing of sweep mode is offered, sent or shown - the selector's title
gains its Sweep sentence only then. On the downconverter variant it shows,
disabled, as "Sweep (not on the KrakenSDR Wideband variant)", and the title
says so (the other non-coherent modes are disabled there as before; generic
RTL-SDRs may sweep, also with one tuner). `setOpMode('sweep')` refuses in
both cases without sending anything. `applyOpModeUI` takes `sweep` (body
class `mode-sweep`; `swpEnter` / `swpLeave`), `opOnMessage` takes it as for
the other modes. CSS: `.sweep-only` (shown only in sweep mode), `.not-sweep`
(hidden in it); `.coh-only` and the right-hand DoA pane are hidden as in the
other non-coherent modes (the rail's Map still opens the map alone).

**Mode evidence.** heimdall's sweep stream carries the independent flag too
(0x2000 with 0x800), so in sweep mode the receiver's status and any type 6
/ 7 frame look like independent mode. While the latest `sweep` message says
`active`, `opHint` takes no hint at all (a new connection marks that state
stale until the receiver says it again). In sweep mode a type 7 frame is
dropped (only its hint is noted - it must not reach the main display's plot
and waterfall, which keep their picture for the return); type 6 already was.
A `sweep` message with `active` switches a page that shows another mode to
sweep - the page missed the `operating_mode` message - but not within 1.5 s
of an `operating_mode` message or of this page's own mode change
(`swpOpSentAt`), so a broadcast from before a change cannot pull it back.

**The top bar.** The frequency (`#tb-freq`) is replaced by the range
(`#tb-swp-range`): Start and Stop, `G.MMM.kkk` digits to 1 kHz (`.swp-d`, own
class: the top bar's code queries `.freq-digit` page-wide) in the
frequency's font, colour and box height (50 px - the bar keeps its 63 px when
the mode changes), 24 px digits in 15 px cells so that the range and the
tuner selector fit where the frequency, Audio and Step were (1700 px window
with the sidebar: still one row). Click the upper / lower half of a digit or
turn the wheel on it; Start stays at least 100 kHz under Stop, both inside
the receiver's `min_hz` / `max_hz` (a step past a limit stops at it).
Leading zeros are dimmed. Then the tuner selector (`#tb-swp-tuners`, icon
`tuners`): one toggle per tuner the receiver has (`available`), pressed =
takes part; the last pressed one cannot be released. Its title says what the
user asked to be said: more tuners = a faster sweep, each needs its antenna
connected. Gain stays (one `GAIN:` for every tuner; with the optional
`gain_db` the bar follows other pages). Audio / Volume and the tuning Step
leave the bar (`.not-sweep`). On the phone the range (Start and Stop a row
each, 20 px cells) and the tuners are rows of the drawer's Tuner & Audio box
before Gain (`MOBILE_RELOCATIONS`), with their text labels.

**Commands and echoes.** The page sends `SWEEP_RANGE:<MHz, 3 decimals>:<MHz>`,
`SWEEP_TUNERS:<list>` (`all` when every tuner is pressed), `SWEEP_RES:<N>|auto`,
`SWEEP_DWELL:<ms>`, `SWEEP_RUN:<1|0>`, and redraws from the `sweep` message
every page gets - no optimistic state. Spun controls (the digits, quick
toggles) send at most every 100 ms plus the last value (`swpSendSoon`). What
the user is changing does not jump back: the range and the resolution are
held 1.5 s after the last step (`SWP_HOLD_MS`; then the receiver's value
shows, also one it clamped), the Dwell box while it has the focus, and a
pressed tuner shows at once with a dashed border until the receiver's state
has it (2 s at most, then the receiver's state shows).

**The display.** In sweep mode the main spectrum and waterfall (`#fp`,
`#wf-container`) and the panes are hidden; `#swp-spec-box` (canvas, drawn by
`swpDraw`, at most once per screen refresh) and `#swp-wf-box` (its own
`WaterfallDisplay`) take their place under the spectrum's header. Its own
display rather than the main uPlot: that one is tied to the VFOs (tuning
bars, click-to-place, squelch lines, the beamformed overlay, bearing
colours) and to the coherent centre frequency - none of it belongs here, and
the main display keeps its picture and zoom for the return. The spectrum is
as tall as the main display's (`fpApplyHeight` sets both, the same split
drags it). Frequencies are doubles in Hz throughout (the frame's f64s, the
trace's plain arrays): at 1.765 GHz with 146 Hz bins the points stay on the
bin grid.
- The trace for the window shown is cut / padded from the latest frame
  (`swpTrace`, `specWindow` in Hz), so a zoom or a pan answers at once and
  the detail follows with the next frame.
- `SWEEP_VIEW` when the window or the plot width (device pixels, half with
  Slow link, 16 ... 4096) changes - zoom, pan, a window or sidebar resize, the
  map pane opening -, at most every 100 ms and the last one always; `0:0` at
  1x (the whole range: right after a range change too); again on entering the
  mode and after a reconnect.
- Waterfall: one line per frame with flag 2, never more than 20 a second
  (`SWP_WF_MS`; a quicker sweep's line waits for its turn, the newest
  replacing one still waiting). A new grid id (range or resolution) drops the
  waterfall's rows (`WaterfallDisplay.clear`, new) and the last frame; a new
  range also resets the zoom.
- Old frames: a type 10 frame outside sweep mode, one built for a view from
  before the page entered the mode (`seq < swp.seqMin`), and one whose grid id
  is not the latest `sweep` message's are dropped - the last only while that
  lasts under 1.5 s (a receiver whose message never comes would otherwise
  never show anything).
- Zoom: the wheel (x1.25 around the pointer) on the spectrum or the
  waterfall, and the View popover's Zoom slider, as far as one bin per point
  (`swpZoomMax` = span / (bin x points): ~490x for the whole range at 2.3
  kHz bins, ~7800x at 146 Hz) - the usual 32x would leave a 1.7 GHz range at
  ~54 MHz. The slider's end follows (`zoomTarget` gives `max`; `zoomText`
  writes whole numbers past 100x; other modes: max 125 = 32x, unchanged).
  Zoomed in, a drag pans; nothing is retuned and nothing but `SWEEP_VIEW` is
  sent.
- The View popover works on it with the common values (Scale, Colors, Speed,
  Contrast; Auto ranges the sweep's waterfall from the sweep's trace); its
  zoom readout is the points' spacing.
- The pointer: a line across the spectrum and the waterfall, and a readout
  box at the top of the spectrum - frequency (to 1 kHz, 100 Hz or 10 Hz by the
  points' spacing) and the highest level within the pointer's pixel.
- What the sweep is doing: a bar along the top of the plot and one in the
  spectrum's header (amber while paused), the progress in % and the time of
  the last complete sweep beside it - fixed widths (the frames come 20 a
  second); "No data yet" / "Paused" / "Waiting for the receiver" in the plot.
- Nothing of the VFOs: a click on it sends nothing.

**The sidebar.** A Sweep box (`data-sb="sweep"`, `.sweep-only`, title icon
`span`, placed first after the phone boxes): Resolution (Auto - naming the
width in use - and the FFT sizes 128 ... 16384 as bin widths, 18.8 kHz ...
146 Hz), Dwell (5 - 1000 ms, typed; a value outside is clamped before it is
sent), Run / Pause (`Pause` while running; `Run`, primary, while paused), and
the state: State (Running / Paused / No data yet), Sweep time, Progress,
Steps (hops and the tuners sharing them), Points (bins and their width),
Sweeps. Box by box in sweep mode:
- shown: Sweep; Station Information (the map); Display, but only Slow link
  and Points sent (they apply to the sweep's view; FFT size, Averaging and
  Edge clip belong to the live spectrum); the phone's Tuner & Audio and
  Panels; the footer.
- hidden: Decimators (VFOs need demodulation), MUSIC DoA, Beamforming, Web
  Mapper, DoA Recording, Continuous Scanner, Experimental (`.coh-only`, as in
  the other non-coherent modes), Scanner (wideband scan only), IQ & Audio
  Recorder (IQ is coherent-only, audio needs VFOs), Saved Frequencies (a
  click tunes a VFO; its scan needs one), Debug Playback (a coherent array's
  test transmissions through the VFOs and decoders). The rail's Decoders and
  Transcription buttons and the panel under the waterfall go too (nothing is
  decoded); the panel comes back as it was.

**Tests** (2026-11-20, headless Chromium, real mouse / wheel / key / touch
events, scratch `52196024/tmp/page/`). The receiver's engine did not exist
yet: the page ran against a TEST kraken_doa of the tree before this change
(it serves the page; ports 286xx) with a fake heimdall (coherent scene), and
the contract's engine ran inside the page (`mock_sweep.js`, injected before
the page's scripts: it records everything the page sends and answers the
sweep commands with `sweep` / `operating_mode` messages and type 10 frames
through the page's own `ws.onmessage` - plan, hops per tuner block, pooling,
flags, grid ids, a synthetic 24 - 1765 MHz spectrum). `SOURCE=real` turns the
mock into a recorder only, for the run against the real engine.
- `t_sweep.py` (154 checks): the selector; entering (what shows and hides in
  the bar, the display, the boxes, the Display rows; the first view at the
  plot width; at 1x the trace equals the whole-range frame); layout changes
  (map pane, hidden sidebar, the split) re-ask the view; the range digits
  (wheel, both halves of a click, the hover halves, the limits, start < stop,
  the send rate, the hold against a stale broadcast); the tuner toggles
  (pending look, the last one stays, `all`, the rate); Resolution, Dwell
  (typed, clamped), Run / Pause; the header's fixed widths over 25 frames;
  waterfall lines only on flag 2 and never over 20 a second (sweeps of a
  1-hop range at ~80 a second: ~13 - 20 lines a second), one per sweep
  otherwise; a grid change drops the waterfall and an old-grid frame; zoom
  (the frequency under the pointer stays, the window answers at once, views
  at most every 100 ms, the detail frame follows, the limit = one bin per
  point, pan by drag sends only views, 1x = `0:0`); bin-exact frequencies at
  1.7642 GHz with 146 Hz bins; the View popover (zoom slider, Scale, Colors,
  Speed, Contrast, Auto from the sweep); the readout (433.920 MHz at the
  carrier's level); one `GAIN:`, `gain_db`; a click sends nothing; the mode
  switches sweep <-> coherent / independent / wideband (panes, the main
  display, its frames, the zoom slider's 125, the digits, the VFO cards back;
  a frame after leaving dropped); a missed `operating_mode`; a reconnect.
- `t_compat.py` (22 checks): the page before the change and this one,
  measured in coherent, independent, wideband and coherent again (the top
  bar's controls, the spectrum's header, the display, the panes, every
  sidebar box, the rail) - identical with a receiver that never sends a
  `sweep` message and with one that does; the older receiver: Sweep not
  offered, the title unchanged, nothing of sweep mode sent, a forced
  `setOpMode('sweep')` refused; the downconverter variant.
- `t_bar.py` (16 checks): one row and the coherent bar's height at 1700 and
  1920 px, one centre line, the order, the digits' font, no caption text,
  every icon resolves.
- `t_phone.py` (15 checks, 400 x 860 touch): the drawer's rows, a tap on a
  digit's upper half, finger-sized tuner toggles and a tap on one.
- Not covered: Firefox / Safari; the real receiver and heimdall (to come);
  the theme check passes (`tools/theme_palette.py --check`).

## Tests

(each part's tests and the run on the KrakenSDR)
