# TEMPEST screen reconstruction (`tempest`)

The `tempest` plugin (`plugins/tempest/`) reconstructs the picture on a nearby
monitor or TV from its **unintended video emanations** (Van Eck / TEMPEST),
automatically detecting the video mode (frame rate, resolution, sync), with
manual overrides, and - using the KrakenSDR array - a bearing to the leaking
device. This is emissions-security (EMSEC) research tooling; the reference open
tool is Martin Marinov's TempestSDR. The neural-network upgrade is planned
separately and NOT implemented - see `tempest-deep-plan.md`.

It shares the raster backbone in `plugins/lib/video_raster.*` with the
*analog-tv* plugin and uses the plugin-SDK video channel (`plugins/SDK.md`
*Video frames*) for the live picture.

## Physics and the identity that makes auto-detection work

A monitor driven at pixel clock `f_p` radiates harmonics of `f_p`; the user
tunes the KrakenSDR to one harmonic (the "Tune near" fact lists the VHF/UHF
harmonics once a clock is estimated). After AM-magnitude detection
(`video::Envelope`) the recovered intensity tracks the high-frequency/edge
content of the raster - strongest on vertical transitions - so there are no
sync pulses, only the raster's self-similarity. The key identity:

    pixel_clock = htotal x vtotal x frame_rate      (totals include blanking)

So two robust autocorrelation observables - the frame period and the line
period - give `frame_rate = fs/frame_period` and `total_lines =
frame_period/line_period`, which snap to a real display mode. The screen is
near-static, so **averaging many aligned frames** is the main SNR win.

## Auto-detection method (the part worth understanding)

- Envelope `|x|` into a rolling ~3-frame buffer; once a second one FFT
  autocorrelation (`video::PeriodFinder`) over the whole buffer.
- The frame-period lag window `[fs/refresh_hi, fs/refresh_lo]` (default
  50-90 Hz) is only a **coarse refresh gate** - it is +-1-line ambiguous under
  vertical coherence, so it is NOT used directly for geometry.
- The mode is chosen by **LINE rate**, not frame rate: among the DMT modes
  whose actual refresh is within 8 % of the coarse frame rate, pick the one
  whose line-rate comb best explains the autocorrelation (a harmonic-sum
  `comb_score` at the mode's line period, requiring the fundamental line peak
  >= 0.12). This is what disambiguates e.g. 1024x768 from a nearby mode.
- `samples_per_line` is refined by anchoring to the frame-period autocorr peak
  within +-0.5 line of `vtotal x nominal`, and using the sharper comb estimate
  when it agrees - ~0.01 % precision, so no horizontal shear.
- **Confidence gate** (so noise never locks): frame peak >= 0.20, frame
  prominence >= 0.12, line comb >= 0.12. Verified: a noise file produces no
  event, no lock, no bearing.
- The candidates that pass (`auto_candidates`, best comb first) are then
  checked against the **measured total lines** (next section): only a table
  mode with the measured count is taken, else the display is reported as
  not listed. Without a measurement (a weak signal) the best comb decides,
  as before the measurement existed.
- Phase (there are no sync pulses) comes from `video::find_blanking_offset`
  (on the full raster's column / row sums, `video::raster_profiles` - the
  raster itself is never stored) converted to a global frame-start sample,
  `anchor_`; frames are resampled anchored to it and EMA-averaged
  (`video::FrameAccumulator`), rendered GRAY8 with a percentile stretch. The
  absolute picture position is otherwise undetermined - `h_offset` /
  `v_offset` nudge it.
- `anchor_` is an absolute sample kept near "now" (each `add_frame` rolls it
  forward by whole frames), and `apply_geom` rolls it forward under the OLD
  period before a re-detection changes `frame_period_`. A phase stored modulo
  the period instead moves the frame start by (frames since lock) x (period
  change) on every update until the next blanking search - measured with the
  blanking search off-cadence (0.7 s): 0.5 / 1.3 / 3.4 samples after 1.3 / 2.3
  / 3.4 s; with the anchor <= 0.02 samples. (At the shipped cadences the
  blanking search runs in the same tick as every re-detection, which hid it.)
- The frame-period search (`try_detect`) runs on the newest `buf_cap()`
  samples - a fixed length per setting - so `video::PeriodFinder` keeps one
  FFT plan; it also keeps at most two plans, so a caller whose length drifts
  can't grow its cache.

One-off lock event (also what the debug playback checks):
`"Detected 1024x768 @ 60 Hz"` (`"Detected {W}x{H} @ {round(refresh)} Hz"`);
for a display no table mode matches, `"Display not listed: 926 lines @ 59.90
Hz - enter its width and height in Manual"` (see below).

## Measured total lines (`measure_lines`)

The line comb picks a table mode by its LINE rate only; nothing checked the
line COUNT, so a display not in the table locked as a wrong one with a
similar line rate and the picture sheared (1440x900 CVT-RB, 926 lines, as
1600x900 / 1000 lines; 1600x1200 as 1600x900; a 935-line custom 1440x900 as
the table's 934-line 1440x900, rolling one line a frame; a custom 1280x800
as 3440x1440). The row history behind the *Total lines* graph measures the
count directly, and detection now uses it.

- **Row history** (`feed_rows`, `update_rows`): the envelope summed over each
  line period L into a ring of `ROWS_N` (16384) line energies. It now runs
  whether or not a viewer watches (it used to stop with `video_wanted`). A
  count V scores the mean unbiased autocorrelation of the ring at V, 2V,
  3V ... (`rows_score`); `rows_acf` caches the ring's autocorrelation for
  the measurement and the graph.
- **What it measures**: the frame in lines of L - frame = V x L. It is the
  display's total lines only when L is the true line period: summed at a
  wrong period it is just as self-consistent (the 1600x900 period 39.83
  gives 1006 for a 40066-sample frame; 4K's alias period 80 gives 500).
  Everything below follows from that.
- **L** (`rows_line_period`): the estimate from the signal
  (`estimate_line_period`: the frame peak / the largest count whose
  `lines_score` is >= 80 % of the best), unless the hypothesis - the lock's
  period, or while searching Auto's best candidate's / Manual's
  `manual_line_period` - is another rate that explains the comb at least
  half as well (an aliased estimate), or the estimate failed. A hypothesis'
  period is snapped to the frame peak divided by the nearest whole number
  of its lines (the refinement alone was 2e-4 off for 2560x1440 at 8 dB, a
  third of a line a frame - it smeared the peak). The estimate is gated by
  `LINE_EST_COMB_MIN` (0.008, below Manual's 0.02: the frame peak stays on
  a whole-line tooth down to ~5 dB where the comb scores 0.009-0.015) and
  `LINE_EST_COMB_REL` (0.035 x the frame peak: real line combs measured
  0.048 - 0.29 of it over every test signal and SNR, 4K's 80-sample alias
  0.022). Below `LINE_EST_PEAK_MIN` (0.05 frame peak, noise ~0.01) there is
  no estimate, so noise never starts a history.
- **Range** (`meas_range`): the counts whose frame (V x L) falls in the frame
  search window (the refresh window, or +-4 % around a forced / entered
  refresh) - that keeps out 2x the count, which repeats as well; Manual with
  a resolution: height + 1 .. height + 20 % + 8. At most `ROWS_MAXLAG` / 3
  (>= 3 multiples a count).
- **Confident** estimate: the best count is inside the range, scores >=
  `MEAS_PEAK_MIN` (0.25), beats both +-1 neighbours by `MEAS_SIDE_MIN`
  (0.010) and every competing peak by `MEAS_OTHER_MIN` (0.10). A competing
  peak is a local maximum with a dip of `MEAS_PROM` (0.05) between it and
  the count: the hill around the count is broad and uneven for pictures with
  large flat areas (1920x1200: 0.942 three lines off with 0.915 between),
  and those bumps must not count.
- **Accepted** count (`meas_v_`): `MEAS_CONFIRM` (2) confident estimates in a
  row with the same count, each on at least half a fresh history. It stays
  until a confirmed different count replaces it, the line rate changes by
  more than `MEAS_L_TOL` (0.5 %, `update_rows`) or an option change drops
  the lock; an unsure estimate doesn't undo it, and a lost lock keeps it (a
  re-lock needn't wait). Estimates run at each frame search (0.3 s
  searching, 1 s locked), and while locked without an accepted count also
  from `tick()` whenever half the history is fresh.
- **At a hypothesis' period** (`LineCount::hyp`) the count is only as exact
  as the hypothesis (4K seen as 1280x720: 749 for 750), so it may confirm
  that hypothesis within +-1 and never sets a count: Auto doesn't lock
  "not listed" from it, Manual doesn't take it, and a forced mode's fact
  says only that the frame doesn't fit.

**Auto** (`resolve_geometry`):
1. An accepted count: the best-comb candidate that has it (`count_agrees`:
   exactly that count - one off rolls the picture - and a nominal line
   period within 0.5 % of L, which lets a 59.94 Hz variant of a 60 Hz mode
   through); else any table mode with that count and line period whose comb
   missed the gate (1920x1080 at 7 dB), the nearest line period; else
   **not listed** (`use_unlisted`): the full raster of the measured count at
   L, `htotal` = 1.8 x the count (the active area is unknown), label `"926
   lines @ 59.90 Hz (not listed)"`, the event above once, Status `the
   display is not a listed mode - choose Manual and enter its width and
   height`, fact `Measured lines: 926 (59.90 Hz) - no listed mode has it`.
2. No accepted count, but this search's estimate is confident and agrees
   with the best candidate: lock it at once (two independent observables).
3. Otherwise a first lock waits (`Geom::wait`, Status `measuring the total
   lines...`) up to `MEAS_WAIT_S` (2 s) from the first search that could
   have locked, and so does a lock whose line rate just changed (the
   display changed: it keeps its geometry meanwhile instead of taking the
   comb's guess); then the best comb alone, as before.
When the measured count agrees and the refinement is more than 1e-4 off L,
the mode runs at L (`use_auto_mode`): 1920x1080 at 59.94 Hz locked at
60.19 Hz before (refine_spl_frame's comb climb), 59.94 now. The old
no-candidate estimate path shows a 1.8:1 raster too (it was one column per
sample, a sliver many screens tall), labelled `"~926 lines @ 59.9 Hz
(estimate)"`.

**Forced** mode: never overridden. A measured count that disagrees is the
fact `Measured lines: 926 (59.90 Hz) - the forced mode has 1000: try Auto,
or Manual with the resolution` and a second marker (`926 measured`) on the
*Total lines* graph.

**Lock time** (Auto, 25 dB): 0.6 - 0.9 s from the start of the signal (was
0.3 s - the history needs 0.12 - 0.55 s of lines, then an estimate), 1.2 s
for a custom 828-line display; at most ~2.3 s when no estimate is
confident (the wait runs out, then the comb as before).

## Free-run preview (display only)

So the canvas is never blank while searching (a real TempestSDR free-runs the
raster), `tick()` emits a **free-run** frame whenever `!locked_` (gated by the
`free_run` option, default on). `send_free_run()` takes the assumed geometry -
the forced DMT mode, else `FREE_RUN_DEFAULT_MODE` (`1024x768@60`) - and maps the
raw magnitude envelope straight onto it with the shared `video::free_run_frame`
helper, using the nominal `samples_per_line = fs/line_rate` and
`samples_per_pixel = spl/htotal` and the locked path's invert/contrast/gamma.
There is no sync recovery and no frame averaging, so the image is live and
rolls. The preview raster is **capped to `FREE_RUN_MAX_PX` (720) on the longest
side** regardless of the mode (a 4K mode must not make the preview huge) -
`send_free_run()` scales the active W/H down and divides both resample strides
by the same scale, so it resamples straight into the small raster (cheaper than
resampling full-res then downscaling). `~12 fps` (`FREE_RUN_S`), label
`"<W>x<H> - free-run (searching)"`, `quality = 50`. It is **strictly
display-only**: it never touches the detection, lock, confidence gate or DoA
state (`accumulate_cov` already refuses while `!locked_`), so the facts stay
`searching` and no bearing is produced while unlocked. The moment `try_detect`
sets `locked_`, the `if (locked_)` render block in the same `tick()` takes over
and free-run is skipped.

## Info and options

`.id="tempest"`, `.name="TEMPEST screen"`, `.category="EMSEC"`,
`.lock_vfo=true`, `.sample_rate=0` (native VFO rate - the full 2.4 MHz),
`.min_vfo_rate=2000000`, `.video=true`, `.multichannel=true` (for DoA),
`.talkers=true`, `.manual_only=true` (it must never run in Auto-detect - it
would false-trigger on anything).

Options (saved per VFO): `mode` (auto | a listed display mode | manual),
`refresh_lo` / `refresh_hi` (the auto search window, default 50/90 Hz), `width` /
`height` / `fps` / `lines` / `pixel_clock_mhz` / `samples_per_line` (used when
`mode=manual` - see *Manual mode* below),
`free_run` (0/1, default 1 - the unlocked preview above), `h_offset` /
`v_offset` (nudge the alignment), `invert`, `gamma`, `contrast`
(normal|high|low), `average` (EMA factor, default 0.1), `autolock` (freeze the
detected timing), `crop` (active-only vs full raster, default active),
`quality` (a **slider**, `choices="range:10:95:5"`, default 60 - the JPEG
quality the host encodes the picture at: lower = less bandwidth/CPU, higher =
sharper), `lossless` (max-quality JPEG, for a frozen / heavily averaged still),
`max_display_px` (a preset dropdown 640/960/1280/1920/3840, default 1280 -
longest-side cap on the **emitted** picture only; see Output), `doa` (report the
source bearing, default on).

Supported display modes (the `mode` choice list): the VESA DMT modes 640x480,
800x600, 1024x768, 1280x720, 1280x1024, 1366x768, 1600x900 and 1920x1080 (all
@60), plus the CVT reduced-blanking / CTA-861 high-resolution modes
1920x1200@60, 2560x1440@60 (QHD), 2560x1600@60 (WQXGA), the ultrawides
2560x1080@60 and 3440x1440@60, and 3840x2160 at 30 and 60 Hz (4K UHD). All live
in `video::display_modes` (`plugins/lib/video_raster.cpp`); the CVT-RB rows are
reproduced exactly by this file's `cvt_mode(w, h, 60, reduced=true)`, the 4K
rows are the CTA-861 timings (htotal 4400, vtotal 2250; 594.0 MHz at 60 Hz,
half at 30 Hz).

**Auto-detect works reliably for 640x480..1920x1080**; the high-res modes are
best **forced**: 2560x1440 and 3440x1440 share a line rate (both vtotal 1481,
~88.8 kHz) so Auto can report either; 4K @60 has only ~18 samples per line, so
its line comb aliases with harmonic-related lower modes (e.g. 1280x720, whose
line period is exactly 3x) and Auto can mis-pick it; 4K @30 (30 Hz) is below
the default 50-90 Hz refresh window. Forcing the mode fixes the geometry and
detection is then exact (verified below).

### Manual mode (`resolve_manual`)

For a display that is not in the list. Two ways to describe it:

- **Resolution** (`width` + `height`, both): the active area. The picture is
  cropped to it and labelled "1680x1050 @ 60.0 Hz (manual)"; `lines`,
  `pixel_clock_mhz`, `fps` and `samples_per_line` are optional overrides.
- **Timing only** (`lines` required): the full raster, labelled by its line
  count, as before the resolution fields existed.

What the code fills in when a field is empty:

1. **Line period** (`manual_line_period`). The period finder's frame peak is
   NOT trusted as the frame period: with a vertically coherent picture every
   line multiple correlates, and the strongest integer lag is the multiple
   nearest a whole number of samples - measured 1 to 3 lines off the true frame
   on the test signals (1024x768: +1, 1680x1050: -2, 1440x900: -3). It is
   still a whole number of lines, so `frame / n` is the line period for some
   count `n`: every `n` in the plausible range (the entered `lines` +-8, else
   `height` + 1 .. `height` + 20 %) is scored by the mean autocorrelation at
   `frame * N / n` for every N across the middle of the frame (`lines_score`).
   The right `n` sits on a comb tooth at all of them, a wrong one sweeps every
   line phase: measured 0.09-0.21 for the right count against <= 0.007 for
   every other, 0.000 on noise (threshold `MAN_COMB_MIN` 0.02). A sparse set
   of lags is not enough - with 6 or 12 lags particular wrong counts (+-17,
   +-21 lines) scored within a few % of the right one. The resulting line
   period is exact (the frame peak is sub-sample over ~1000 lines) and is used
   as is; `refine_spl_frame` is only the fallback when nothing was measured
   (its comb climb landed 0.25 % off on the 1440x900 signal, where the 28-line
   tooth falls half-way between two samples).
2. **Total lines** (`lines` not entered): the accepted **measured count**
   (*Measured total lines*), when it lies in height + 1 .. height + 20 % + 8
   and was summed at the measured line period (not a hypothesis', and
   within 0.5 % of step 1's). The one exception is a +-1 tie: a *known
   timing* of the resolution that fits (below) one line from the measured
   count wins - a display whose refresh differs from a known timing's by
   ~0.065 Hz at the same line rate is far less likely than a one-line
   measurement error. The first lock waits up to `MEAS_WAIT_S` for the
   count when a line period was measured (on noise there is none: it locks
   at once, as before). Without an accepted count (a weak signal), the
   known-timing inference as before: the known timing whose own refresh
   the line period reproduces to `MAN_FIT_TOL_HZ` (0.03 Hz, about half a
   line per frame) - the table modes of that size, then `cvt_mode` reduced
   and standard blanking - else the count giving the whole-number refresh
   (`fps` if entered, else the rounded measured rate), and with no
   measurable line period the first known timing (a table mode, else
   CVT-RB). The known timings are looked up at the entered `fps`, else the
   measured count's refresh, else the frame peak's (a 30 Hz display's frame
   peak sits at 89 Hz under the default window: 4K@30 got 2253 lines at
   89.29 Hz before, 2250 / 30.00 Hz / 297.0 MHz now). Step 1's line period
   (`manual_spl0`) is computed before the measurement, so it seeds the row
   history from the first search.
3. **Pixels per line**: `pixel_clock_mhz` if entered, else the known timing
   nearest in line count - exact for CVT / table timings, an approximation for
   a custom one (then the crop can sit off horizontally; nudge or crop off).

4. **Pixels per line with nothing to go on** (timing only: no resolution, no
   pixel clock): `MAN_HT_PER_VT` (1.8) x the line count - DMT / CVT / CTA
   rasters run 1.5 - 2.1. It used to be one column per sample (`spl`, ~50), so
   `lines=806` alone emitted a 50 x 806 frame that the page drew ~8000 px tall
   (found with the timing graphs, whose clicks lead straight to this case). The
   pixel clock fact is then a rough estimate (806 lines: 70.2 MHz est, true 65).

Manual mode has no confidence gate (it never had): with `lines`, or now a
resolution, it locks on anything, noise included. With `lines` entered, a
measured count that disagrees is the fact `Measured lines: 1089 (59.95 Hz)
- 1087 in use` and a marker on the graph; the entered value is used.

## Timing graphs beside the picture (`send_plots`)

Two `kp::Plot`s with `place = PLOT_BESIDE_VIDEO` (the SDK's PLOT_EX; ids
`fps`, `lines`), every `PLOT_S` (0.5 s) of input, **only while
`host.video_wanted()`** - the page draws them in a column right of the video
canvas, so they are seen exactly when the picture is. The graphs are
display only; the row history behind the total-lines one is not any more -
it runs regardless of a viewer and feeds detection (*Measured total
lines*), and the graph reads the same history. A click sets `fps` / `lines` with
`click_also = "mode=manual"` (the page sends the value, then the mode if it
isn't Manual; both through DIGITAL_OPT).

- **Frame rate** (`send_fps_plot`): `pf_frame_.autocorr()` of the last
  `try_detect` (window `acf_flo_..acf_fhi_`, kept with the result) mapped to
  600 refresh-rate bins, the highest integer lag inside each bin (a plain
  sample of the comb aliases it away). Marker: `frame_rate_` when locked, else
  a Manual `fps`. Snap radius half a line period in Hz, step 0.001 Hz. The
  tallest tooth is NOT the frame (see *Manual mode* 1.): measured 1-4 lines
  off on 1024x768 / 1440x900 / 1680x1050. That is the signal, not the graph:
  even the 2x-upsampled envelope's autocorrelation and a 32-tap sinc
  interpolation of the lag put the true frame's tooth below its neighbours on
  these signals - the envelope is far from band-limited (pixel-rate detail
  sampled at 2.4 MHz), so a lag half a sample off a whole number decorrelates.
- **Total lines** (`send_lines_plot`): no single-lag observable gives the
  count (the `lines_score` peak is the frame PEAK's count - 1087 for the
  1680x1050 signal, true 1089). The graph instead folds the vertical profile:
  `feed_rows` integrates the envelope over each line (boundary sample split by
  its fraction) at a line period L into a ring of `ROWS_N` (16384) line
  energies; their normalised autocorrelation (a second `video::PeriodFinder`,
  one fixed plan, lags to `ROWS_MAXLAG` = 12288), unbiased by the overlap,
  averaged over the lags V, 2V, 3V ... is the score of V. The blanking band and
  horizontal edges repeat exactly every vtotal lines and slip by (V - vtotal)
  lines a frame for any other count, and the line energies don't depend on the
  sub-sample phase. Measured peak = truth on all eight test signals (incl.
  forced 4K), ~0.95-1.0 at the peak, 0.87-0.95 at +-1, ~0 median. Range +-25 %
  of the count in use (else of the frame peak's), on 50-line steps. Snap 12
  lines, step 1 - a click lands on the peak's whole count. Markers: the count
  in use, and the accepted measured count where it differs (`926 measured`).
- **L**: `rows_line_period` (*Measured total lines*) - measured, not the
  lock's, so a wrong lock doesn't skew the graph; the lock's when the
  estimate aliased (4K). A period change of more than 2e-5 restarts the row
  history (0.12 - 0.55 s to refill).
- **Cost** (x86 i7-11800H, user CPU of `decoder --file`, 15-run means): locked
  1680x1050 0.405 -> 0.417 s per 4 s (+0.3 % of real time), 1440x900
  0.405 -> 0.415, forced 4K 0.527 -> 0.543; noise 0.281 -> 0.282 (the
  estimate is gated off); Manual-searching 1680x1050 0.381 -> 0.389. The
  unwatched cost the always-on history adds is under *Tests*.

Facts surfaced in the UI: detected frame/line rate, total lines, the accepted
measured count ("Measured lines", with what it disagrees with), matched mode +
error, estimated pixel clock, "Tune near" (the clock's VHF/UHF harmonics),
"Resolution limit ~N px/line" (~50 at 2.4 MHz), and frames averaged.

## Direction finding of the source (secondary, defensive)

Only in coherent mode (`process_multi`, m >= 2), with `doa` on, and only once
locked: a spatial covariance is accumulated over the strongest-edge samples in
~0.2 s windows and reported as one talker packet (`id="screen"`, `t.packet`,
`t.m`, trace-normalised `t.cov`, `avg_s=4`, `keep_s=120`). kraken_doa runs MUSIC
with one source -> a bearing in the map's DF panel. Reconstruction runs
identically on a single channel (`process()`) with no covariance; a DoA failure
never affects the picture.

## Output

GRAY8, ~4 frames/s. The **reconstruction uses every line and every envelope
sample of the display**, so a 4K monitor is received and reconstructed in full -
but on a **work raster** (`work_width()`): one row per display line (the active
area, or `htotal_` x `vtotal_` with `crop=0`), and only `RECON_OVERSAMPLE` (8)
columns per envelope sample across the line. At 2.4 MHz a line holds only
~18-60 samples whatever the display's width, so a 4K line is ~124 columns, not
3840, and more columns would only repeat the linear interpolation between the
samples. `send_video()` maps the work raster back to the display's geometry:
rows box-averaged past the cap as before, each output column interpolated from
the work raster at the centre of the display columns it covers. The shift
realignment searches +-3 lines and +-3 display pixels (as many work columns as
that spans - usually 0 or 1). `max_display_px` is a **display / transmission
cap only**: past it the EMITTED frame is scaled down (default 1280), never
feeding back into `add_frame` / detection. The host then compresses each
emitted frame to JPEG before the wire (`plugins/sdk/plugin_host.cpp`, built with
libjpeg-turbo; GRAY8 is a native grayscale JPEG). Measured sizes at quality 60:
a 1280x720 frame is ~40-70 KB, and a 4K reconstruction emitted at the 1280 cap
(1280x720) is the same ~40-70 KB; raising `max_display_px` to 3840 to emit the
native 4K raster puts a few hundred KB on the wire - all well under the 1 MiB
`kp::wire::MAX_PAYLOAD`. The plugin always passes **raw** pixels and sets the
`VideoFrame.quality` / `.lossless` hints from the `quality` / `lossless`
options. While no browser shows the video (`host.video_wanted()` false) it
skips the render and scaling but keeps averaging.

`max_display_px` caps the **longest side** of the emitted frame (default 1280,
presets 640/960/1280/1920/3840). `crop=1` (default) reconstructs/sends the
active area, `crop=0` the full raster (htotal x vtotal, up to 4400x2250 for 4K);
either way the emitted frame is scaled down past the cap.

**Cost.** The blanking search still forms the full `htotal_` x `vtotal_`
raster's column / row sums twice a second (`video::raster_profiles`, no stored
raster - it used to be a ~40 MB float raster at 4K); the per-frame resample /
accumulate / render runs on the work raster (4K: 124 x 2160 = 0.27 Mpx instead
of 8.3 Mpx). `CADENCE_PX_REF` (stretch the cadences above ~1440p-worth of work
pixels) is kept as a guard but no table mode reaches it any more. Measured
(x86, `--file`-like harness, 4 s scenes): 1024x768 x5 antennas 7.9 % -> 4.0 %
of real time, 1920x1080 (1 antenna) 18.4 % -> 4.5 %, `crop=0` 21.4 % -> 4.8 %,
4K x5 35.5 % -> 11.9 % (and now 4 frames/s with 65 frames averaged, where the
stretched cadence gave 7 frames / 30 averaged). With the shift realignment off,
the work raster reproduces the full-geometry picture (corr 0.9999-1.0000 at
1024x768 / 1080p, 0.998 at 4K, where the cadence differs); with it on the two
differ by the realignment's frame-to-frame choices (corr 0.85-0.99 against the
old build), vertical profiles >= 0.996; against a clean (60 dB SNR) scene's
reconstruction the vertical profile correlates as before (1024x768 0.938 vs
0.939, 1080p 0.917 vs 0.927, 4K 0.871 vs 0.870). **No-libjpeg caveat:** without libjpeg
the host can only send raw pixels under the 1 MiB cap, where a GRAY8 frame over
~1000x1000 is **dropped** (not truncated) - the default 1280 cap keeps the
emitted picture visible there, and lowering it (e.g. 960) shrinks it further.

## Limitations (surfaced in the UI and README)

- **Horizontal** detail is bandwidth-limited to ~50 px/line at 2.4 MHz - the
  real TEMPEST limit; the picture is clearly recognisable (layout, bars, large
  text) but fine text is not legible without a wider capture. **Vertical**
  structure is faithful (corr ~0.995 in tests).
- No sync -> the absolute picture position is undetermined until nudged.
- SNR depends heavily on proximity, the chosen harmonic, cabling and shielding.

## Tests

- `tools/tempest_gen.py` (python3 -I, numpy) renders a synthetic test pattern
  (bars / checkerboard / UI panels - no copyrighted assets), models the
  emanation as the per-line `|luma derivative|` across pixels at the pixel clock
  with blanking (linearly interpolated so it is band-limited and exactly
  periodic), AM-modulates at an offset, steers onto kraken_doa's UCA and adds
  noise, writing a 2.4 MHz cu8/cf32 + JSON sidecar (sample-major antennas, like
  `radar_gen.py`). `--mode`, `--bearing`, `--seconds`, `--snr`.
- Offline (1024x768@60, 65 MHz clock, 8th harmonic @ 520 MHz, bearing 35 deg,
  25 dB): `decoder --file scene.cf32 --channels 5 --uca 0.12 --rf 520e6` ->
  mode EXACT (1024x768@60, 806 lines, 48.4 kHz line, 65.0 MHz clock, 60.00 Hz);
  reconstruction vertical corr 0.995, horizontal 0.53, full-frame 0.62; DoA
  35.0 deg (<1 deg error). All of 640x480 / 800x600 / 1024x768 / 1280x720 /
  1280x1024 / 1366x768 / 1920x1080 @60 detect correctly; bearings exact at
  15/75/110/190/250/300 deg. Noise -> no lock. Decode ~9 % of real time at
  2.4 MHz x 5 antennas (x86). macOS clang-14 + Linux GCC both compile.
- High-resolution (offline, cf32, SNR 26 dB, 5 antennas @2.4 MHz, reconstruction
  correlated against the generator's dpix edge map, vertical = per-row profile):
  - **2560x1440@60** (bearing 50 deg): forced -> mode EXACT (1481 lines,
    88.8 kHz, 241.5 MHz, err 0 %); reconstruction **vertical corr 0.998**; DoA
    50.0 deg. Under Auto it reports 3440x1440 (the shared-line-rate twin) - also
    exact geometry, only the width label differs.
  - **3840x2160@60** (4K, 594 MHz fundamental, bearing 35 deg): forced -> mode
    EXACT (2250 lines, 135.0 kHz, 594.0 MHz, 60.00 Hz, err 0 %); reconstruction
    **vertical corr 0.999**, horizontal 0.45 (only ~18 px/line at 2.4 MHz); DoA
    35.0 deg. Under Auto the 17.8-sample line comb aliases to 1280x720, so force
    4K. Decode ~30 % of real time at 2.4 MHz x 5 antennas (x86); the per-frame
    resample/render dominates (vs ~9 % for 1024x768) - the cadence scaling keeps
    it bounded. 1024x768 regression unchanged (~8 %).
- Free-run preview: a throwaway harness with a capturing `kp::Host` (the
  offline `--file` host drops `video_frame`) drives `process()` with
  complex-Gaussian noise and with a 1024x768@60 card. Noise (mode auto) -> a
  continuous, **changing** 720x540 preview (the 1024x768 default under the
  720-px cap) at the ~12 fps cap with **no event / no lock** (still
  `searching`); forcing 1920x1080 -> 720x405 (the cap holds for high-res). Card
  -> ~3 free-run frames then the switch to the synced 1024x768 reconstruction on
  `Detected`; `free_run=0` -> no frames until lock, detection unchanged.
  ASan+UBSan clean on all these paths. Preview decode cost on noise ~2-3 % of
  real time (x86); it does not run once locked.
- Work raster / anchor / video_wanted (the same capturing harness, old vs new
  build on the same scenes): 640x480 .. 1366x768 and 2560x1440 (Auto -> the
  3440x1440 twin, as before) detect identically with the fixed-length search
  window; noise -> no lock; DoA 35.0 deg. Costs and picture comparison under
  *Output*. Toggling `doa` 0 -> 1 mid-stream in coherent mode used to write
  into an empty covariance (ASan SEGV in `accumulate_cov`); now clean.
  `video_wanted=0` over the wire -> no VIDEO messages, lock and facts
  unchanged; set to 1 -> the first frame within ~0.15-0.2 s of input, JPEG
  decodes. ASan+UBSan clean (crop 0/1, max_display_px 64/3840, nudges, manual
  mode, 4K x5).
- Manual width / height (`decoder --file`, 1 antenna, 25 dB; nothing but
  `mode=manual width=W height=H`): total lines, frame rate and pixel clock all
  equal the generator's truth for 1680x1050 CVT (1089, 59.95 Hz, 146.3 MHz),
  1440x900 CVT-RB (926, 59.90, 88.8), 1600x900 DMT (1000, 60.00, 108.0),
  1024x768 DMT (806, 60.00, 65.0), 1920x1080 CTA (1125, 60.00, 148.5) and
  2560x1440 CVT-RB (1481, 59.95, 241.5). The 1600x900 picture correlates 0.74
  with the forced mode's (same content, a few px horizontal shift). `lines`
  alone (the old path) now reports 59.95 Hz on the 1680x1050 signal where it
  gave 60.30 (the frame-peak alias). One of width / height alone, or neither,
  without `lines`: "searching". Noise + a resolution: locks at CVT-RB (no gate).
  Seen while testing, not changed: the blanking search places the forced
  1600x900 picture ~27 % off horizontally on this test pattern (11 % blanking
  against wider dark areas in the picture) - the same at the previous commit.
- Timing graphs (offline `--plots`, and a browser test over CDP with two
  browser contexts on the `tempest-1024x768` debug playback; scratch scripts):
  total-lines peak = truth (806, 926, 1000, 1089, 1125, 1481, 2250 forced),
  also on Auto's wrong 1440x900 lock (1000 lines in use, peak 926); Manual with
  nothing / only `fps` -> searching with the peak there; `lines` = the peak
  locks at 59.954 Hz on 1680x1050; `lines=1087` (the frame peak's count) gives
  60.06 Hz, rolling, with the graph still peaking at 1089. Browser: graphs
  right of the canvas, column as tall as it, below it at 560 px wide; hover
  readout snaps to 806 (Shift: 814); clicking the frame-rate graph from Auto
  sent `DIGITAL_OPT:0:tempest.fps:60.284` then `...mode:manual`, both
  browsers' options followed, plugin searching, lines graph still live; the
  total-lines click sent `lines:806`, the plugin locked "806 lines @ 60.0 Hz
  (manual)", markers 806 / 60.004 Hz (truth 60.0038) in the second browser; no
  page errors. The pre-change page against the new receiver: no plots shown,
  no errors, video fine.
- Measured total lines (scratch generator copy with extra modes: 1440x900
  CVT-RB 926 lines, 1600x1200 DMT, 1920x1080 at 59.94 Hz, custom 1440x900
  1680x935 and 1280x800 1440x828 at 59.94 Hz; `decoder --file`, 1 antenna,
  3 s cf32 unless stated; old = the previous commit's working tree):
  - Auto, 25 dB, every generator mode: every table mode gives the same mode
    as before (640x480 .. 1920x1080, 1920x1200, 2560x1080, 2560x1600; the
    2560x1440 / 3440x1440 twin as before; 4K@60 still 1280x720, 4K@30 still
    no lock), lock at 0.6 - 0.9 s instead of 0.3 s, frame rates now within
    0.005 Hz of truth (1280x720 59.99 -> 60.00, 1366x768 59.80 -> 59.79,
    640x480 59.95 -> 59.94, 1920x1080 at 59.94 Hz 60.19 -> 59.94, 1920x1200
    60.07 -> 59.95 - which also announced "Detected 1600x900" first before).
    Unlisted, old -> new: 1440x900 CVT-RB 1600x900 -> "926 lines @ 59.90 Hz
    (not listed)", 1600x1200 1600x900 -> 1250 @ 60.00, custom 1440x900
    1440x900 (934, 60.01) -> 935 @ 59.94, custom 1280x800 3440x1440 -> 828 @
    59.94. (1680x1050 CVT is in `video::display_modes` - only not in the
    option list - and is detected as 1680x1050 both before and after.)
  - Auto across SNR (`--snr` 15, 10, 8, 7, 6, 5): the same mode as before at
    each; at 7 dB both 1920x1080 signals are now 1920x1080 (were 2560x1080 /
    "Detected 2560x1080" first); 6 dB and below neither locks (the frame gate).
  - Measurement across SNR (traced, every estimate): exact and accepted down
    to 4 dB for 1024x768, 1440x900 (both), 1680x1050, 1920x1080 (both), down
    to 5 dB for 2560x1440 and 6 dB for 640x480 (its peak scores 0.29 at 5 dB,
    0.22 at 4 dB); below ~3 dB the frame peak is under 0.05 and nothing is
    estimated. Over 234 runs (117 signals, Auto and Manual) 723 confident
    estimates, none wrong at the measured line period; the 20 "wrong" ones
    were all at a hypothesis' period (the frame in that period's lines -
    hence the rule that those only confirm). Lowering `MEAS_PEAK_MIN` from
    0.30 to 0.25 gained 640x480 at 5 dB; a 0.25 run before the hypothesis
    rule took 937 lines at 3 dB from rows summed at a wrong lock's period.
  - Manual width + height only (old -> new, lines / Hz exact against truth),
    8 displays x 10 SNRs (25 .. 2 dB): 45 / 80 -> 68 / 80, no cell worse.
    25 dB: 1920x1080@59.94 1124 / 59.99 -> 1125 / 59.94, custom 1440x900
    934 / 60.00 -> 935 / 59.94, custom 1280x800 827 / 60.01 -> 828 / 59.94,
    4K@30 2253 / 89.29 Hz -> 2250 / 30.00 / 297.0 MHz; the others unchanged
    and exact. Low SNR: exact to 4 dB (old: misses from 6 dB, e.g. 1440x900
    934 / 60.10 at 5 dB); at 3 and 2 dB both fail the same way. Pixel clock
    of a custom timing stays an approximation (custom 1280x800: 83.4 MHz,
    true 71.5 - the nearest known timing's htotal).
  - cu8 (`--noise-lsb` 0.5 / 1 / 3 x `--snr` 25 / 10 / 6, 1024x768, 1440x900
    CVT-RB, custom 1440x900): equal or better everywhere; Auto never shows
    1600x900 / 1440x900 for the two unlisted ones; Manual exact at 6 dB
    where the old gave 934 / 60.10, 919 / 60.98, 60.30 Hz ...
  - Noise (two cf32 levels, cu8, 10 s): no event, no lock, no estimate.
  - Hysteresis (capturing harness, 10 s at 7 - 8 dB): one mode per run, no
    label toggling; 1024x768 -> 2 s noise -> 1440x900: the lock holds through
    the gap (as before), then "Display not listed: 926 lines" (the old build
    silently switched to 1600x900; a first version showed 1600x900 for 1 s
    in between, hence the re-measuring wait).
  - Forced: 1600x900 on the 1440x900 CVT-RB signal keeps 1600x900 with the
    fact "926 (59.90 Hz) - the forced mode has 1000 ..." and a `926 measured`
    marker; forced 4K (cf32 and the cu8 playback signal) measures 2250 and
    agrees - the cu8 one first showed "500 ... the forced mode has 2250"
    (the 80-sample alias), hence `LINE_EST_COMB_REL`.
  - Pictures (harness, last locked frame vs the generator's edge map, row
    profile correlation at the best shift): 1440x900 CVT-RB Auto 0.904
    (1600x900, sheared diagonals) -> 0.989 (stable full raster); custom
    1440x900 Manual 0.975 -> 0.995 (sharper rows); 1920x1080 at 59.94 Hz
    Auto 0.933 -> 0.997.
  - Five antennas (`--channels 5 --uca 0.12`): 35.0 deg on 1024x768 (Auto,
    Manual), 1440x900 CVT-RB (Auto not listed, Manual), custom 1440x900 -
    as before (a few packets fewer in 3 s: the later lock).
  - CPU (x86 i7-11800H, the capturing harness's in-`process()` CPU, 10 s
    signals, 8 interleaved runs each, old -> new, % of real time): without a
    viewer (`video_wanted` off) 1680x1050 Auto +0.22, 1440x900 CVT-RB
    +0.20, 1024x768 +0.37, forced 4K +0.25, Manual searching +0.33, Manual
    w+h +0.20, noise -0.01 (no estimate, no history); with a viewer -0.01 ..
    +0.20 (the history ran then already). The always-on row history is
    ~0.2 %; the rest of 1024x768's is the later lock (two more searching-
    cadence frame FFTs).
- Debug playback: `tools/testsignals.json` id `tempest-manual-1680x1050`
  (`tempest_gen.py --mode 1680x1050@60`, a mode not in the option list,
  received with VFO opts `mode=manual width=1680 height=1050`) expects
  `{"bearing":35}`; id `tempest-unlisted-1440x900` (`--mode 1440x900rb@60`,
  Auto) expects `{"event":"Display not listed: 926 lines @ 59.90 Hz"}` and
  `{"bearing":35}` (offline: that event at 0.9 s, 35.0 deg; the old build
  said "Detected 1600x900 @ 60 Hz").
- Debug playback: `tools/testsignals.json` ids `tempest-1024x768` (wide cu8,
  3 s loop) and `tempest-4k` (3840x2160@60 forced via the VFO `mode` opt, 2 s)
  expect `{"event":"Detected ... @ 60 Hz"}` and `{"bearing":35}`.
  `make_testsignal.py --check --plugins plugins` passes (44 transmissions, ids unique). (Generators
  need numpy; the debug playback runs them with the environment's `python3 -I` -
  on this dev box use `/usr/bin/python3`, which has numpy.)

## Licence

KrakenSDR Source Available (no own LICENSE file); clean-room from the physics,
VESA DMT/CVT facts and textbook DSP - no gr-tempest / TempestSDR / Deep-TEMPEST
code consulted (all GPL). See *analog-tv* (the shared backbone) and
*tempest-deep-plan* (the planned neural reconstruction).
