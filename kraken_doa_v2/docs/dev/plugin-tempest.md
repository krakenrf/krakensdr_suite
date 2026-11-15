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
- Phase (there are no sync pulses) comes from `video::find_blanking_offset`
  converted to a stable global frame phase; frames are resampled anchored to it
  and EMA-averaged (`video::FrameAccumulator`), rendered GRAY8 with a percentile
  stretch. The absolute picture position is otherwise undetermined - `h_offset`
  / `v_offset` nudge it.

One-off lock event (also what the debug playback checks):
`"Detected 1024x768 @ 60 Hz"` (`"Detected {W}x{H} @ {round(refresh)} Hz"`).

## Info and options

`.id="tempest"`, `.name="TEMPEST screen"`, `.category="EMSEC"`,
`.lock_vfo=true`, `.sample_rate=0` (native VFO rate - the full 2.4 MHz),
`.min_vfo_rate=2000000`, `.video=true`, `.multichannel=true` (for DoA),
`.talkers=true`, `.manual_only=true` (it must never run in Auto-detect - it
would false-trigger on anything).

Options (saved per VFO): `mode` (auto | a listed DMT mode | manual), `refresh_lo`
/ `refresh_hi` (the auto search window, default 50/90 Hz), `fps` / `lines` /
`pixel_clock_mhz` / `samples_per_line` (used when `mode=manual`), `h_offset` /
`v_offset` (nudge the alignment), `invert`, `gamma`, `contrast`
(normal|high|low), `average` (EMA factor, default 0.1), `autolock` (freeze the
detected timing), `crop` (active-only vs full raster, default active), `doa`
(report the source bearing, default on).

Facts surfaced in the UI: detected frame/line rate, total lines, matched mode +
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

GRAY8, ~4 frames/s. `crop=1` (default) sends the active area (1024x768 =
0.79 MB, under the 1 MiB wire cap); `crop=0` sends the full raster downscaled to
fit; larger active rasters (e.g. 1280x1024) are box-downscaled under the limit.

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
- Debug playback: `tools/testsignals.json` id `tempest-1024x768` (wide cu8,
  tuner_offset 0, 3 s loop) expects `{"event":"Detected 1024x768 @ 60 Hz"}` and
  `{"bearing":35}`. `make_testsignal.py --check` passes; it builds and decodes
  to the expected event + bearing. (Generators need numpy; the debug playback
  runs them with the environment's `python3 -I`.)

## Licence

KrakenSDR Source Available (no own LICENSE file); clean-room from the physics,
VESA DMT/CVT facts and textbook DSP - no gr-tempest / TempestSDR / Deep-TEMPEST
code consulted (all GPL). See *analog-tv* (the shared backbone) and
*tempest-deep-plan* (the planned neural reconstruction).
