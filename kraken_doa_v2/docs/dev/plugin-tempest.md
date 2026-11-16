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
`refresh_lo` / `refresh_hi` (the auto search window, default 50/90 Hz), `fps` /
`lines` / `pixel_clock_mhz` / `samples_per_line` (used when `mode=manual`),
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

GRAY8, ~4 frames/s. The **reconstruction always runs at the display's full
geometry** (`htotal_` x `vtotal_` / the active area), so a 4K monitor is received
and reconstructed in full - `max_display_px` is a **display / transmission cap
only**: `send_video()` box-downscales just the EMITTED frame when its longest
side exceeds the cap (default 1280), never feeding that back into
`add_frame` / detection. The host then compresses each emitted frame to JPEG
before the wire (`plugins/sdk/plugin_host.cpp`, built with libjpeg-turbo; GRAY8
is a native grayscale JPEG). Measured sizes at quality 60: a 1280x720 frame is
~40-70 KB, and a 4K reconstruction emitted at the 1280 cap (1280x720) is the same
~40-70 KB; raising `max_display_px` to 3840 to emit the native 4K raster puts a
few hundred KB on the wire - all well under the 1 MiB `kp::wire::MAX_PAYLOAD`. The
plugin always passes **raw** pixels and sets the `VideoFrame.quality` /
`.lossless` hints from the `quality` / `lossless` options.

`max_display_px` caps the **longest side** of the emitted frame (default 1280,
presets 640/960/1280/1920/3840): past the cap the frame is box-downscaled.
`crop=1` (default) reconstructs/sends the active area, `crop=0` the full raster
(htotal x vtotal, up to 4400x2250 for 4K); either way the emitted frame is
downscaled past the cap while the reconstruction stays at full geometry.

**4K is CPU- and memory-heavy.** The resample / accumulate / render cost grows
with the output pixel count (a 4K float frame is ~33 MB; the EMA average and the
shift-aligned add touch two or three of those). Above ~1440p (`CADENCE_PX_REF`)
the accumulate and video cadences are stretched in proportion so the per-second
pixel work stays bounded - a **Pi** may sustain only a low frame rate at 4K. At
or below ~1440p the cadence is unchanged, so default-resolution performance does
not regress. **No-libjpeg caveat:** without libjpeg the host can only send raw
pixels under the 1 MiB cap, where a GRAY8 frame over ~1000x1000 is **dropped**
(not truncated) - the default 1280 cap keeps the emitted picture visible there,
and lowering it (e.g. 960) shrinks it further.

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
- Debug playback: `tools/testsignals.json` ids `tempest-1024x768` (wide cu8,
  3 s loop) and `tempest-4k` (3840x2160@60 forced via the VFO `mode` opt, 2 s)
  expect `{"event":"Detected ... @ 60 Hz"}` and `{"bearing":35}`.
  `make_testsignal.py --check` passes (42 transmissions, ids unique). (Generators
  need numpy; the debug playback runs them with the environment's `python3 -I` -
  on this dev box use `/usr/bin/python3`, which has numpy.)

## Licence

KrakenSDR Source Available (no own LICENSE file); clean-room from the physics,
VESA DMT/CVT facts and textbook DSP - no gr-tempest / TempestSDR / Deep-TEMPEST
code consulted (all GPL). See *analog-tv* (the shared backbone) and
*tempest-deep-plan* (the planned neural reconstruction).
