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

Options (saved per VFO): `mode` (auto | a listed display mode | manual),
`refresh_lo` / `refresh_hi` (the auto search window, default 50/90 Hz), `fps` /
`lines` / `pixel_clock_mhz` / `samples_per_line` (used when `mode=manual`),
`h_offset` / `v_offset` (nudge the alignment), `invert`, `gamma`, `contrast`
(normal|high|low), `average` (EMA factor, default 0.1), `autolock` (freeze the
detected timing), `crop` (active-only vs full raster, default active),
`quality` (WebP quality for the picture: 1-100, or low/medium/high = 40/65/90,
or `default` = host ~75; default 75), `lossless` (lossless WebP, for a frozen /
heavily averaged still), `max_display_px` (longest-side cap on the emitted
picture, 64-4096, default 3840 = native 4K), `doa` (report the source bearing,
default on).

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

GRAY8, ~4 frames/s, sent at the **native active resolution up to 4K**. The host
compresses each frame to WebP before the wire (`plugins/sdk/plugin_host.cpp`,
built with libwebp), so even a 3840x2160 frame (8.29M raw bytes) crosses the
decoder video wire in ~19 KB (1080p ~7 KB), well under the 1 MiB
`kp::wire::MAX_PAYLOAD`. The plugin always passes **raw** pixels and sets the
`VideoFrame.quality` / `.lossless` hints from the `quality` / `lossless` options.

`max_display_px` caps the **longest side** (default 3840): past the cap the
frame is box-downscaled. `crop=1` (default) sends the active area, `crop=0` the
full raster (htotal x vtotal, up to 4400x2250 for 4K, downscaled past the cap).

**4K is CPU- and memory-heavy.** The resample / accumulate / render cost grows
with the output pixel count (a 4K float frame is ~33 MB; the EMA average and the
shift-aligned add touch two or three of those). Above ~1440p (`CADENCE_PX_REF`)
the accumulate and video cadences are stretched in proportion so the per-second
pixel work stays bounded - a **Pi** may sustain only a low frame rate at 4K. At
or below ~1440p the cadence is unchanged, so default-resolution performance does
not regress. **No-libwebp caveat:** without libwebp the host can only send raw
pixels under the 1 MiB cap, where a GRAY8 frame over ~1000x1000 is **dropped**
(not truncated) - on such a build lower `max_display_px` (e.g. 960) to keep the
picture visible.

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
