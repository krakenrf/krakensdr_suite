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

**Measured total lines.** The line rate alone can't tell a display that is
not in the list from a listed one with a similar line rate (a 1440×900
reduced-blanking monitor looks like 1600×900 to it). So the decoder also
counts the lines: it sums the envelope over each line into a history of
~16 000 line energies and finds the count V at which that vertical profile
repeats (the blanking band and the picture's horizontal edges come round
every frame, and slip by a line a frame for a count one off). The count is
trusted once it stands clearly above its neighbours twice in a row; it is
shown as the **Measured lines** fact. Auto then takes only a listed mode
with that count. If none has it, Auto does **not** show a wrong mode: it
reports the display as not listed - the event `Display not listed: 926
lines @ 59.90 Hz - enter its width and height in Manual`, Mode `926 lines @
59.90 Hz (not listed)`, a Status line saying to choose Manual, and the
full raster (blanking included) at the right line count, so the picture is
steady while you enter the resolution. When the signal is too weak for a
trusted count, Auto picks by line rate as before. Detection takes about
0.6 - 0.9 s from the moment the signal appears (at most ~2.3 s when no
count can be trusted); the count works without anyone watching the
picture.

On a confident lock the decoder emits one event, e.g. `Detected 1024x768 @ 60 Hz`,
and facts: frame rate, line rate, total lines, the matched mode and its error,
the estimated pixel clock, a **suggested tuning** fact listing the pixel-clock
harmonics that fall in VHF/UHF (where to tune the receiver), the horizontal
resolution limit, and the number of frames averaged.

**Reconstruction.** The frame phase (there are no sync pulses) is recovered by
summing one full `htotal×vtotal` raster's columns and rows and finding where the
dark blanking band sits (`video::find_blanking_offset`); that is converted to a
global frame-start sample, kept near "now". Each frame is then resampled
(`video::resample`, linear interpolation) anchored to it so rows never shear or
wrap, and added to a running average (`video::FrameAccumulator`, EMA with a
small shift realignment). The average is rendered to 8-bit grayscale with a
percentile contrast stretch and sent a few times a second with
`host.video_frame()`. The **reconstruction uses every line and every envelope
sample of the display**, so a 4K monitor is received and reconstructed in full -
on a raster with one row per line but only 8 columns per envelope sample (a
2.4 MHz capture has only ~18-60 samples per line, whatever the display's
width), mapped back to the display's geometry for the emitted picture;
`max_display_px` is a **display / transmission cap** that scales down
only the emitted frame when its longest side exceeds the cap (default **1280**,
presets 640/960/1280/1920/3840). The host then compresses each emitted frame to
JPEG (GRAY8 → native grayscale), so a 1280×720 frame is a few tens of KB on the
wire; raising `max_display_px` to 3840 emits the native 4K raster at a few
hundred KB — all under the 1 MB wire cap. Because the work raster follows the
sample rate, not the display's width, 4K costs about 3x 1024×768, not 10x.
While no browser shows the picture, the render and JPEG encode are skipped. Without
libjpeg the host can send only raw pixels under the 1 MB wire cap and drops
anything larger, so the default 1280 cap keeps the picture visible there.

## Manual controls (options, saved per VFO)

| Option | Default | Meaning |
|---|---|---|
| `mode` | `auto` | `auto`, a specific listed mode to force it, or `manual` (see the mode list below) |
| `free_run` | 1 | show a live, unsynced preview while still searching (like a TempestSDR while you tune); **display only**, never affects detection / the lock / the bearing. Off = blank canvas until lock |
| `refresh_lo` / `refresh_hi` | 50 / 90 | auto-detect refresh search window (Hz) |
| `width` / `height` | — | manual active resolution in pixels (e.g. 1680 / 1050); set both. The picture is cropped to that area and shown at its shape, and the timing fields left empty are filled in (see *Manual mode* below) |
| `fps` | — | manual frame rate (Hz); a click on the *Frame rate* graph sets it (and Manual) |
| `lines` | — | manual total lines (vtotal, blanking included). Required unless `width` and `height` are set; a click on the *Total lines* graph sets it (and Manual) |
| `pixel_clock_mhz` | — | manual pixel clock, if known (sets the total pixels per line) |
| `samples_per_line` | — | manual samples/line at the VFO rate, instead of deriving it |
| `h_offset` / `v_offset` | 0 | nudge the picture left/right (samples) and up/down (lines) |
| `invert` | 0 | swap black/white (AM video is often negative) |
| `gamma` | 1.0 | output gamma (>1 lifts shadows) |
| `contrast` | normal | percentile stretch: `normal` / `high` / `low` |
| `average` | 0.1 | frame-average strength (EMA factor 0–1; smaller = steadier, slower) |
| `autolock` | 0 | freeze the detected timing so it stops re-estimating / drifting |
| `crop` | 1 | show only the active area, or the full raster incl. blanking |
| `quality` | 60 | JPEG quality for the picture (a slider, 10–95): lower = less bandwidth/CPU, higher = sharper |
| `lossless` | 0 | send the picture at maximum JPEG quality (best for a frozen / heavily averaged still) |
| `max_display_px` | 1280 | longest-side cap on the **emitted** picture only (presets 640/960/1280/1920/3840); reconstruction always uses every line and sample, so 4K is still received in full. 3840 emits the native raster; lower it for less bandwidth/CPU or on a no-libjpeg build |
| `doa` | 1 | estimate a bearing to the source (coherent mode only; never affects the picture) |

When `mode` is not `manual`, auto-detect drives the timing; manual offsets,
averaging, contrast, crop and the picture-quality controls still apply. A
forced mode is always used as chosen; if the measured line count disagrees
with it, the **Measured lines** fact says so (e.g. `926 (59.90 Hz) - the
forced mode has 1000: try Auto, or Manual with the resolution`).

**Manual mode** is for a display that is not in the mode list. The usual way
is to enter only its resolution - **Manual width** and **Manual height** - and
leave the rest empty:

- *Total lines* are measured from the signal (the **Measured lines** count
  above), so a custom timing or a fractional refresh such as 59.94 Hz comes
  out right without entering anything else. When the signal is too weak
  for a trusted count, they come from the known timing of that resolution
  (a listed mode of the same size, or the VESA CVT reduced- /
  standard-blanking timing) that fits the measured line rate, else the
  count giving a whole-number refresh (60.00, 75.00 ...) - then check the
  **Total lines** fact, and enter `fps` or `lines` for a custom timing. An
  entered `lines` always wins (a measured count that disagrees is shown in
  the **Measured lines** fact).
- *Pixels per line* come from the pixel clock if entered, else from the known
  timing with the nearest line count. For a custom timing that is an
  approximation: the crop can then sit a little off horizontally - enter the
  pixel clock, use the horizontal nudge, or switch **Crop** to the full raster.
- With a signal too weak to measure the line rate, the CVT reduced-blanking
  timing of the resolution is used as it stands.

Without a resolution, `lines` is required and the whole raster (blanking
included) is shown, labelled by its line count. Manual mode has no confidence
gate: it shows a picture for whatever is tuned, noise included.

Without a resolution or a pixel clock the pixels per line are not known: the
raster is then drawn about 1.8 times as wide as it has lines (what DMT / CVT /
CTA timings run), and the pixel clock fact is only a rough estimate.

**Timing graphs (click to set).** Right of the picture (below it in a narrow
panel) two graphs show where the video timing is, live while searching as well
as locked - the way to find the timing of a display that isn't listed:

- **Frame rate**: the envelope's autocorrelation against refresh rate over the
  frame search window (50-90 Hz by default; +-4 % around `fps` once that is
  set), the highest lag per point. With a vertically coherent picture every
  line multiple correlates, so it is a comb of peaks one line apart, and the
  tallest is not always the frame itself (it is the line multiple nearest a
  whole number of samples - on the test signals 1 to 4 lines off). The marker
  is the frame rate in use.
- **Total lines**: for each count V, how well the picture's vertical profile
  (the envelope summed over each line - the blanking band and horizontal edges)
  repeats every V lines: the mean autocorrelation of ~16 000 line energies at
  V, 2V, 3V ... It peaks at the true total-lines count (the picture slips by
  one line a frame for a count one off), whatever the frame-rate graph's
  tallest tooth is. The marker is the count in use.

Hover shows the value a click sets; a click sets `fps` / `lines` and switches
`mode` to Manual (shown in the options and in every browser, saved per VFO).
The value snaps to the highest point nearby - on the total-lines graph to the
peak's whole line count, so it is exact - and Shift+click takes the exact
position. The usual way: click the frame-rate graph near the tallest teeth
(this narrows its window), then the total-lines peak; the picture locks. A
count one line off makes the picture roll. The graphs are drawn at the line
period measured from the signal, so on a forced mode that doesn't fit the
total-lines peak still shows the real count, with a second marker
(`926 measured`). Drawing them costs ~0.3 % of real time while shown (x86);
the line history behind the total-lines graph runs all the time, because
detection uses the count (~0.2 - 0.4 % of real time, x86).

**Free-run preview (display only).** While the decoder is **not** locked
(searching, or a weak / unsynced leakage), it maps the raw magnitude envelope
straight onto the assumed geometry - the forced mode's active size, else a
**1024×768** default - and sends it with `video_frame`, so the canvas always
shows a live image, exactly as a TempestSDR shows snow with no input and a
rolling picture as you tune a harmonic in. The brightness mapping is the locked
path's (the `invert` / contrast / gamma options). It is purely a preview: it
never touches the detection, lock, confidence gate or DoA state, so the facts
stay at `searching` and the confidence gate is unchanged; no bearing is ever
produced while unlocked. The preview raster is **capped to a 720-px longest
side regardless of the mode** (a 4K mode must not make the preview huge - it is
resampled straight into the small raster, which is also cheaper), at **~12 fps**
and a lower JPEG quality (noise frames never dedup, so the modest size + rate
bound the bandwidth). The moment the real timing locks, the synced
reconstruction at the native active resolution takes over unchanged. The
`free_run` option (default on) turns it off for a blank-until-lock canvas.

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
- **Free-run preview** (capturing host; the offline `--file` host drops
  `video_frame`): on complex-Gaussian noise it emits a continuous, **changing**
  720×540 image (the 1024×768 default downscaled to the 720-px cap) at the
  ~12 fps cap with **no event and no lock** (still `searching`); forcing
  1920×1080 it previews at 720×405 (the cap holds for high-res modes). On a real
  1024×768 scene it emits ~3 free-run frames and then switches to the synced
  1024×768 reconstruction on `Detected`. With `free_run=0`, no frames appear
  until lock, and detection is unchanged. The preview adds ~2-3 % of real time
  on noise (x86); on a locked signal it does not run.
- **Cost:** decode time ~4 % of real time at 2.4 MHz × 5 antennas (x86) for
  1024×768 (~9 % before the work raster followed the sample rate); the
  once-a-second autocorrelation FFT dominates, the per-frame resample/average
  is cheap. Output frames are GRAY8, reconstructed from every line and sample
  and emitted at the `max_display_px` cap (default
  1280), JPEG-compressed by the host (a 1280×720 frame is a few tens of KB on
  the wire).

- **High-resolution (forced mode):** a 2560×1440@60 and a 3840×2160@60 (4K)
  scene both detect **exactly** (2560×1440: 1481 lines / 88.8 kHz / 241.5 MHz;
  4K: 2250 lines / 135.0 kHz / 594.0 MHz, err 0 %), with **vertical-structure
  correlation 0.998 and 0.999** against the generator's edge map (horizontal is
  bandwidth-limited to ~27 and ~18 px/line respectively), and DoA exact (50°,
  35°). 4K decode is ~12 % of real time (x86; ~30-35 % when the reconstruction
  ran on the full 3840-wide raster).

Debug playback: `tools/testsignals.json` has `tempest-1024x768` (1024×768@60) and
`tempest-4k` (3840×2160@60, mode forced via the VFO option, 2 s) that play the
leakage through the real pipeline and check `{"event": "Detected … @ 60 Hz"}`
and `{"bearing": 35}`; `tempest-manual-1680x1050` (Manual width / height) and
`tempest-unlisted-1440x900` (a 1440×900 reduced-blanking display, Auto:
`{"event": "Display not listed: 926 lines @ 59.90 Hz"}`) check the bearing too.

Measured total lines (offline, generated displays, 1 antenna): Auto gives the
same mode as before for every listed mode, and frame rates within 0.005 Hz
of the truth (1920×1080 at 59.94 Hz: 59.94, was 60.19); displays not in the
list are reported as not listed with the right count - 1440×900 reduced
blanking (926 lines, was shown as 1600×900), 1600×1200 (1250, was 1600×900),
custom 1440×900 / 1280×800 timings at 59.94 Hz (935 / 828, were 1440×900 /
3440×1440). Manual with only the width and height gets the total lines and
frame rate exactly right on 68 of 80 display × SNR cases from 25 down to
2 dB (45 before; none worse), including custom 59.94 Hz timings and 4K at
30 Hz, and stays exact down to ~4 dB. The count was never confidently wrong
when measured at the measured line period (723 trusted estimates). Noise:
no lock. Bearing unchanged (35.0°).

Timing graphs (`build/decoder --file X.cf32 --plots out.jsonl`, 25 dB): the
total-lines peak sits on the true count for 1024×768 (806), 1440×900 CVT-RB
(926), 1600×900 (1000), 1680×1050 CVT (1089), 1920×1080 (1125), 2560×1440
(1481) and forced 4K (2250) - also where Auto locked the wrong mode (1440×900
-> 1000 lines: peak still 926) or where the frame-rate graph's tallest tooth is
2 - 4 lines off. With Manual and nothing entered, or only `fps`, it shows the
same peak while searching; `lines` = that peak locks at the true refresh
(1680×1050: 59.954 Hz). Noise: no total-lines curve (no line structure).
Browser (debug playback `tempest-1024x768`, two browsers): click the frame-rate
graph -> `fps` + Manual in both, the plugin searches; click near the total-lines
peak -> `lines` = 806 in both, locked at 60.004 Hz (truth 60.0038).

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
