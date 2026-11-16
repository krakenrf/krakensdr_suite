# TEMPEST screen reconstruction (`tempest`)

Reconstructs the picture on a nearby monitor or TV from its **unintended video
emanations** (Van Eck phreaking / TEMPEST), and — with the KrakenSDR array — a
**bearing to the leaking device**. This is emissions-security (EMSEC) research
tooling: it lets an operator verify, from their own receiver against their own
displays, how much of a screen's content leaks over the air and from which
direction, so shielding and TEMPEST countermeasures can be evaluated. The
reference open tool for this attack is TempestSDR; this decoder is a clean-room
implementation from the physics and the published VESA timings (see *Licensing*).

Category: **EMSEC**. It is `manual_only` — it never runs in Auto detect (it
would false-trigger on anything), so you select it deliberately on a VFO.

## The physics

A digital display is scanned pixel by pixel at a **pixel clock** `f_p`. Every
pixel transition (an edge in the image) is a fast current step in the cable and
drivers, which radiates. The radiation is rich in **harmonics of `f_p`**; you
tune the KrakenSDR to one harmonic (e.g. the 8th harmonic of a 65 MHz clock is
520 MHz). There are **no sync pulses** on the air — unlike broadcast TV, nothing
marks the start of a line or frame.

After **AM-magnitude detection** (`|x|`), the recovered intensity tracks the
**high-frequency / edge content** of the raster being drawn: flat areas are
dark, vertical edges (transitions as the beam scans across a line) are bright.
During the horizontal and vertical **blanking** intervals nothing is drawn, so
the envelope drops to the noise floor once per line and once per frame — the
only timing features the signal carries.

The whole job is therefore (1) recover the display **timing** with nothing to
lock to, and (2) because the screen is **near-static**, average many aligned
frames to lift the picture out of the noise. Averaging is the main SNR win and
the reason the attack works at all.

## Auto-detection

The core identity ties the two robust observables to a real display mode:

```
pixel_clock = htotal × vtotal × frame_rate         (totals include blanking)
frame_rate  = fs / frame_period                     (fs = VFO sample rate)
line_rate   = fs / line_period = pixel_clock / htotal
```

The decoder keeps a rolling buffer of the AM envelope (`video::Envelope`) and,
about once a second, runs one FFT autocorrelation over it
(`video::PeriodFinder`, Wiener–Khinchin) up to the longest candidate frame
period. From that single autocorrelation it reads:

- **Frame period** — the peak in the lag window `[fs/refresh_max, fs/refresh_min]`
  (default 50–90 Hz, options `refresh_lo` / `refresh_hi`). This gives a coarse
  frame rate. It is only *coarse*: when the screen has large vertical features
  (text, panels) the frame peak is ambiguous by ±1 line, so it is not trusted
  for the resolution.
- **Line period** — the once-per-line blanking comb, read from the same
  autocorrelation in the physical line-rate band `[fs/140 kHz, fs/30 kHz]`, then
  sharpened on a higher comb harmonic (dividing the refined lag by the harmonic
  number divides the sub-sample error) to ~0.01 %. This precision matters: a
  0.1 % line-period error shears the raster horizontally across the frame.

The **mode** is then chosen from the VESA DMT table (`video::display_modes`):
among the modes whose refresh is near the coarse frame rate, the one whose
**line rate** (`pixel_clock / htotal`) matches the measured line period. The
line rate separates the modes cleanly where the frame rate alone cannot (e.g.
1024×768 vs 1366×768 are both 60 Hz but have distinct line rates). That fixes
`htotal`, `vtotal`, active `width`×`height` and the pixel clock. A **confidence
gate** (frame-peak and line-peak prominence above thresholds, and the mode's
line rate within tolerance) means noise produces no lock and no output.

On a confident lock the decoder emits one event, e.g. `Detected 1024x768 @ 60 Hz`,
and facts: frame rate, line rate, total lines, the matched mode and its error,
the estimated pixel clock, a **suggested tuning** fact listing the pixel-clock
harmonics that fall in VHF/UHF (where to tune the receiver), the horizontal
resolution limit, and the number of frames averaged.

**Reconstruction.** The frame phase (there are no sync pulses) is recovered by
resampling one full `htotal×vtotal` raster and finding where the dark blanking
band sits (`video::find_blanking_offset`); that is converted to a stable global
frame phase. Each frame is then resampled (`video::resample`, linear
interpolation) into the active `width×height` grid, anchored to that phase so
rows never shear or wrap, and added to a running average
(`video::FrameAccumulator`, EMA with a small shift realignment). The average is
rendered to 8-bit grayscale with a percentile contrast stretch and sent a few
times a second with `host.video_frame()`, at the **native active resolution up
to 4K**: the host compresses each frame to WebP, so a 3840×2160 frame (8.29M raw
bytes) crosses the wire in ~19 KB (1080p ~7 KB). `max_display_px` caps the
longest side (default 3840); past it the frame is box-downscaled. 4K
reconstruction is CPU/memory-heavy (a 4K float frame is ~33 MB), so above ~1440p
the frame cadence is stretched to stay bounded — a Raspberry Pi may manage only
a low frame rate at 4K. Without libwebp the host can send only raw pixels under
the 1 MB wire cap and drops anything larger, so lower `max_display_px` there.

## Manual controls (options, saved per VFO)

| Option | Default | Meaning |
|---|---|---|
| `mode` | `auto` | `auto`, a specific listed mode to force it, or `manual` (see the mode list below) |
| `refresh_lo` / `refresh_hi` | 50 / 90 | auto-detect refresh search window (Hz) |
| `fps` | — | manual frame rate (Hz) |
| `lines` | — | manual total lines (vtotal, blanking included) |
| `pixel_clock_mhz` | — | manual pixel clock, if known (sets pixels per line) |
| `samples_per_line` | — | manual samples/line at the VFO rate, instead of deriving it |
| `h_offset` / `v_offset` | 0 | nudge the picture left/right (samples) and up/down (lines) |
| `invert` | 0 | swap black/white (AM video is often negative) |
| `gamma` | 1.0 | output gamma (>1 lifts shadows) |
| `contrast` | normal | percentile stretch: `normal` / `high` / `low` |
| `average` | 0.1 | frame-average strength (EMA factor 0–1; smaller = steadier, slower) |
| `autolock` | 0 | freeze the detected timing so it stops re-estimating / drifting |
| `crop` | 1 | show only the active area, or the full raster incl. blanking |
| `quality` | 75 | WebP quality for the picture: 1–100, or `low`/`medium`/`high` (40/65/90), or `default` (host ~75) |
| `lossless` | 0 | send the picture losslessly (best for a frozen / heavily averaged still) |
| `max_display_px` | 3840 | longest-side cap on the emitted picture (64–4096); 3840 sends 4K natively, lower it for less CPU or on a no-libwebp build |
| `doa` | 1 | estimate a bearing to the source (coherent mode only; never affects the picture) |

When `mode` is not `manual`, auto-detect drives the timing; manual offsets,
averaging, contrast, crop and the picture-quality controls still apply.

**Supported display modes** (the `mode` list): VESA DMT **640×480, 800×600,
1024×768, 1280×720, 1280×1024, 1366×768, 1600×900, 1920×1080** (all @60), and
the high-resolution **1920×1200@60, 2560×1440@60 (QHD), 2560×1600@60 (WQXGA),
2560×1080@60 and 3440×1440@60 (ultrawide), 3840×2160@30 and 3840×2160@60 (4K
UHD)**. The high-res modes are published VESA CVT reduced-blanking / CTA-861
timings. **Auto detect is reliable for 640×480…1920×1080**; the high-res modes
are best **forced**, because at 2.4 MHz their very short lines are ambiguous —
2560×1440 and 3440×1440 share a line rate, 4K@60 (≈18 samples/line) aliases to a
harmonic-related lower mode, and 4K@30 is below the default 50–90 Hz window.

## Direction finding of the source (secondary)

With `.multichannel` + `.talkers` set, in coherent mode with DoA on the decoder
receives every antenna (`process_multi`). It reconstructs the picture from
antenna 0, and — strictly as a secondary path — accumulates a spatial
**covariance** of the leakage over the strongest-edge samples (where the
emanation dominates the wideband noise), over ~0.2 s windows. It reports that as
one talker **packet** (`id = "screen"`, `t.packet`, `t.m`, `t.cov`), so
kraken_doa runs MUSIC with one source and shows the device's bearing in the 📡
DF panel. The DoA is gated behind the `doa` option and only runs once the
picture is locked; outside coherent+DoA mode only `process()` is called and the
reconstruction works exactly the same, just without a bearing. A DoA failure can
never break the picture.

## Bandwidth / SNR limitations (surfaced in the UI)

- **Horizontal resolution is limited by the VFO bandwidth.** At 2.4 MHz the
  capture carries only `~pixel_clock/fs ≈ line_period` effective samples per
  line (about **50 px/line** for 1024×768@60). Large features — layout, bars,
  panels, big blocks — reconstruct well; **fine text is not legible** at
  2.4 MHz. Reading text needs a wider capture than a single RTL-SDR provides
  (the `Resolution limit` fact shows the achievable px/line). The vertical
  resolution is full (one sample per line).
- **The screen must be near-static.** Averaging is the SNR win; moving content
  (video, scrolling) blurs.
- **SNR falls off fast with distance and shielding** — the whole point of the
  tool. Dim flat areas sit at the noise floor; only edges carry signal.
- The absolute **vertical and horizontal position** of the picture is ambiguous
  (no sync); the image may appear rolled. `h_offset` / `v_offset` nudge it.

## Test results

Generator: `tools/tempest_gen.py` (python3 -I, numpy) renders a synthetic test
pattern (luma bars, checkerboard, UI panels — no copyrighted assets) to the
mode's framebuffer, models the emanation as the per-line magnitude of the luma
derivative across pixels at the pixel clock with blanking intervals,
AM-modulates it at an offset, steers it onto kraken_doa's UCA at a chosen
bearing and adds receiver noise, writing a 2.4 MHz multi-antenna scene
(cu8/cf32 + JSON sidecar).

Offline, `build/decoder --file scene.cf32 --channels 5 --uca 0.12 --rf 520e6`
on a 1024×768@60 scene (pixel clock 65 MHz, 8th harmonic at 520 MHz, bearing
35°, SNR 26 dB):

- **Mode detection:** `Detected 1024x768 @ 60 Hz`, total lines 806, line rate
  48.4 kHz, pixel clock 65.0 MHz, frame rate 60.00 Hz — exact (match error
  ~0 %). Every DMT mode tested (640×480, 800×600, 1024×768, 1280×720, 1280×1024,
  1366×768, 1920×1080, all @60) is detected correctly at the right refresh.
- **Reconstruction** vs the ground-truth edge map: vertical-structure
  correlation **0.99** (layout and per-line brightness fully recovered),
  horizontal-structure **0.53** and full-frame 2-D correlation **0.62** —
  limited by the ~50 px/line horizontal bandwidth, as expected. The picture is
  clearly recognizable (bars / checkerboard / panels).
- **DoA:** source bearing **35.0°** (truth 35°), error < 1°; verified exact at
  several bearings (15°, 75°, 110°, 190°, 250°, 300°).
- **Noise:** on a pure-noise file the decoder stays in "searching", emits no
  event and reports no bearing — no false lock.
- **Cost:** decode time ~9 % of real time at 2.4 MHz × 5 antennas (x86) for
  1024×768; the once-a-second autocorrelation FFT dominates, the per-frame
  resample/average is cheap. Output frames are GRAY8, native active size up to
  4K, WebP-compressed by the host (4K ≈ 19 KB, 1080p ≈ 7 KB on the wire).

- **High-resolution (forced mode):** a 2560×1440@60 and a 3840×2160@60 (4K)
  scene both detect **exactly** (2560×1440: 1481 lines / 88.8 kHz / 241.5 MHz;
  4K: 2250 lines / 135.0 kHz / 594.0 MHz, err 0 %), with **vertical-structure
  correlation 0.998 and 0.999** against the generator's edge map (horizontal is
  bandwidth-limited to ~27 and ~18 px/line respectively), and DoA exact (50°,
  35°). 4K decode is ~30 % of real time (x86); the cadence scaling keeps it
  bounded and does not regress the ≤1440p default. 1024×768 is unchanged.

Debug playback: `tools/testsignals.json` has `tempest-1024x768` (1024×768@60) and
`tempest-4k` (3840×2160@60, mode forced via the VFO option, 2 s) that play the
leakage through the real pipeline and check `{"event": "Detected … @ 60 Hz"}`
and `{"bearing": 35}`.

## Deep (neural) reconstruction — not implemented

A future plan to add neural-network raster reconstruction ("Deep-TEMPEST") on
top of this classical decoder — and the licensing / platform walls that govern
how it may be done here — is described in
`kraken_doa_v2/docs/dev/tempest-deep-plan.md`. Nothing of it is built; this
plugin is the classical, real-time starting point.

## Licensing

No `LICENSE` file in this folder → part of the KrakenSDR DoA software under the
KrakenSDR Source Available Licence. Written clean-room from the physics, textbook
DSP (AM envelope, autocorrelation, interpolation) and the published VESA DMT /
CVT timings via the shared `plugins/lib/video_raster` backbone. The GPL tools
gr-tempest, TempestSDR and Deep-TEMPEST were **not** consulted or translated.
