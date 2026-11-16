# Analog TV decoder (PAL / NTSC / SECAM)

The `analogtv` plugin (`plugins/analogtv/`) receives off-air analog
television and reconstructs a grayscale **luma picture**, shown live on the
decoder tab's video canvas (the plugin-SDK video channel, `plugins/SDK.md`
*Video frames*). It shares the raster backbone in `plugins/lib/video_raster.*`
with the *tempest* plugin: AM envelope -> sync recovery -> raster assembly ->
contrast-stretched 8-bit frame -> `host.video_frame()`.

## What it is and the bandwidth limit (read first)

A full analog TV channel is 6 MHz (System M / NTSC) to 7-8 MHz (System B/G /
PAL) with a vestigial lower sideband. An RTL-SDR tuner gives only a **2.4 MHz**
slice, so only the low-frequency luma near the vision carrier survives. The
honest consequence, surfaced in the UI (the `Note` fact) and the README:

- **Horizontal** detail is soft: ~150 envelope samples across a whole line
  (2.4e6 / 15625 for PAL), ~125 across the active part. Large shapes, test-card
  structure and big captions are recognisable; fine detail is not.
- **Vertical** resolution is genuine (every line is received).
- **Grayscale luma only** - the colour subcarrier (PAL 4.43 MHz, NTSC 3.58 MHz
  from the vision carrier) is outside the slice.
- **No sound** - the sound carrier is +4.5 MHz (M) / +5.5 MHz (B/G) from the
  vision carrier, outside the slice. A future item is a second VFO on the sound
  carrier. The plugin sets `.voice = false`; it does not fake audio.

This is the same bandwidth reality TempestSDR-class tools live with; it is a
limitation of the hardware, not the decoder.

## Signal model

Broadcast analog TV uses **negative AM modulation**: the carrier is at maximum
amplitude at sync tips and minimum at peak white. So the magnitude envelope
`|x|` (`video::Envelope`) is an inverted-brightness signal - **sync tips are the
envelope maxima**. Hence:

- Sync is found with `video::find_broadcast_sync(..., sync_is_min = false)`.
- Displayed brightness is the inverse of the envelope (blanking -> black,
  below-blank -> brighter). The `invert` option flips this for a
  positive-modulation or baseband-composite input.

## How it locks

1. Envelope over a rolling ~2.8-field buffer; `find_broadcast_sync` recovers
   line and field timing.
2. A rough line rate from the median sync-tip spacing picks the standard via
   `video::match_tv_standard` (PAL and NTSC line rates differ ~0.7 %; SECAM
   shares PAL geometry and cannot be told apart by timing, so it maps to PAL).
   No match -> no lock.
3. The **precise** line period comes from the field-sync cadence (successive
   vertical syncs are `total_lines / 2` line periods apart): `spacing /
   (total_lines/2)` is far more accurate than per-line medians and removes
   diagonal shear. Only vertical-sync runs that repeat at the field cadence are
   kept - this rejects the shared detector's known failure mode, where a bright
   picture trips the median-based vsync threshold and injects spurious field
   starts; the interlace partner field is found by stepping back exactly one
   field period.
4. Each field is resampled onto a 720-wide grid snapped to the nearest sync
   tip; two fields are woven to 720 x active_lines; `FrameAccumulator` renders
   GRAY8 with a percentile stretch. ~15 frames/s.

`host.valid(start, end)` fires only on a genuine sync + field-cadence lock, so
the plugin never steals Auto-detect and never locks on noise (verified: pure
Gaussian noise -> 0 valid frames, "searching").

## Info and options

`.id="analogtv"`, `.name="Analog TV"`, `.category="Broadcast & cellular"`,
`.lock_vfo=true` (tune the receiver to the vision carrier; picking the decoder
locks the VFO at the tuner centre at >= 2 MHz, like *dtv*), `.sample_rate=0`
(the VFO's own rate - effectively the full 2.4 MHz), `.min_vfo_rate=2000000`,
`.video=true`.

Options (saved per VFO): `standard` (auto|pal|ntsc|secam), `invert` (0/1),
`deinterlace` (weave|bob|off), `contrast` (normal|high|full), `gamma` (0.2-5),
`line_adjust` (Hz, +-2000, a manual nudge for a stubborn lock), `quality` (WebP
quality for the picture: 1-100, or low/medium/high = 40/65/90, or `default` =
host ~75) and `lossless` (0/1). The frame is small either way (720 x active),
so `quality` is mainly a bandwidth/fidelity knob; the plugin passes raw pixels
and the host (built with libwebp) compresses them.

Facts: Standard, Line rate (Hz), Field rate (Hz), Samples/line, and the Note on
the bandwidth limit. One-off lock event: `"Locked PAL 625/50"` /
`"Locked NTSC 525/60"`.

## Output

720 x 576 GRAY8 (PAL) / 720 x 480 (NTSC) in weave; bob = full height from one
field; off = half height. ~15 fps, well under the 1 MiB wire cap.

## Tests

- `tools/analogtv_gen.py` renders a synthetic test card into a composite PAL or
  NTSC signal (correct H-sync / blanking / active timing, interlaced fields,
  negative AM) as `.cf32`. Needs numpy. (On this dev box the default miniforge
  `python3` lacked numpy; `/usr/bin/python3` has it. The debug playback runs the
  generator with the environment's `python3 -I`, which on a real install has
  numpy/scipy.)
- Offline (`plugins/analogtv/build/decoder --file out.cf32`): locks, prints the
  standard, reconstructs. Pearson correlation vs the generator's ground-truth
  test card (best over a small shift): **PAL 0.91, NTSC 0.90**. Decode time
  ~6-8 % of real time (x86 `-march=native`, 15 fps). ASan + UBSan clean across
  PAL/NTSC, all deinterlace modes, forced standard, invert, and noise.
- Debug playback: `tools/testsignals.json` id `analogtv-pal-testcard` - a wide
  2.4 MHz, 5-antenna, 3 s PAL scene (cu8, lock_vfo, tuner_offset 0, rf
  183.25 MHz). `make_testsignal.py --check` passes; building it produces the
  .dat + sidecar and the decoder locks on the antenna-0 path, emitting the
  expected event.

## Licence

KrakenSDR Source Available (no own LICENSE file); clean-room from ITU-R
BT.470 / BT.1700 timing. See also *tempest* (the shared raster backbone and the
screen-leakage sibling) and `tempest-deep-plan.md`.
