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
4. **Noise-rejection gate** (before declaring a lock). A matched line/field
   rate is not enough: a lucky sync-tip + field cadence appears in complex
   Gaussian noise (its envelope is Rayleigh, with heavy-tailed spikes the sync
   tracker latches onto - note this is why *uniform* random bytes never
   reproduced it but Gaussian noise did). So the lock is gated on:
   - **Genuine line-rate periodicity**: `video::PeriodFinder` (the same
     confidence gate *tempest* uses) over the most-recent ~32 k envelope
     samples must give autocorrelation `confidence >= 0.35` and `peak >= 0.30`
     at the detected line period, and the autocorrelation period must agree
     with the sync-derived samples-per-line (within 8 %). Measured separation:
     a real test card at the generator's SNR (and at ~2x it) gives confidence
     >= 0.89 / peak >= 0.95; complex-Gaussian noise gives <= 0.033 / <= 0.019 -
     a ~10x margin on either side of the thresholds. Noise has a cadence but no
     raster, so its line-period autocorrelation is flat and it fails outright.
   - **Persistence**: the matched standard and samples-per-line must hold over
     `LOCK_DEBOUNCE` (3) consecutive builds (noise's samples-per-line jumps
     build to build), so a single lucky field pair never locks.
   The autocorrelation FFT is the only added cost, so it runs on a candidate
   (all the cheap gates already passed - sparse on noise) and, once locked, at
   most once a second (`CONF_RECHECK_S`), not on every block. Decode time is
   unchanged (~6 % of real time; lower on noise, since the gate short-circuits
   the reconstruction). No user option: the margin is wide enough that it needs
   no tuning. This fixed a pre-existing false-lock on Gaussian noise.
5. Each field is resampled onto a 720-wide grid snapped to the nearest sync
   tip; two fields are woven to 720 x active_lines; `FrameAccumulator` renders
   GRAY8 with a percentile stretch. ~15 frames/s.

`host.valid(start, end)` and the one-off `Locked` event fire only after the
periodicity + persistence gate passes, so the plugin never steals Auto-detect
and never locks on noise (verified: complex Gaussian noise across >= 10 seeds
and several lengths, and uniform noise, both auto and with a forced standard ->
0 locks, 0 valid frames, "searching"). The lock now takes ~0.19 s rather than
~0.05 s (the debounce), still far inside any test clip.

## Free-run preview (display only)

So the video canvas is never blank or frozen while searching (as a real TV
free-runs the raster and shows snow / a rolling picture), `process()` emits a
**free-run** frame whenever the decoder is NOT locked (searching or the
acquisition debounce). It maps the raw envelope straight onto the nominal raster
for the selected standard (auto defaults to **PAL 625/50**) via the shared
`video::free_run_frame` helper - `OUT_W` (720) wide, `active_lines` tall,
`samples_per_line = rate / line_rate` and `samples_per_pixel = active_us / OUT_W`
(the same active-span mapping and the same negative-AM invert / contrast / gamma
as the locked path) - with **no sync and no frame averaging**, so the image is
live and rolls (correct: there is no sync to anchor to). Label
`"<std> - free-run (searching)"`, `quality = 50` (a throwaway preview), capped at
`FREE_RUN_FPS` (12). It is **strictly display-only**: `emit_free_run()` never
calls `host.valid()`, never fires a lock event, and never touches `locked_`,
`confirms_`, `pending_*`, the period finder or the facts (they stay
`searching`), so the noise-rejection gate above is untouched. Gated by the
`free_run` option (default on; off restores the blank-until-lock behaviour). The
cadence is independent of the reconstruct() build rate; once the real raster
locks, `locked_` is set and the synced path takes over (free-run is skipped).

## Info and options

`.id="analogtv"`, `.name="Analog TV"`, `.category="Broadcast & cellular"`,
`.lock_vfo=true` (tune the receiver to the vision carrier; picking the decoder
locks the VFO at the tuner centre at >= 2 MHz, like *dtv*), `.sample_rate=0`
(the VFO's own rate - effectively the full 2.4 MHz), `.min_vfo_rate=2000000`,
`.video=true`.

Options (saved per VFO): `standard` (auto|pal|ntsc|secam), `free_run` (0/1,
default 1 - the unlocked preview above), `invert` (0/1), `deinterlace`
(weave|bob|off), `contrast` (normal|high|full), `gamma` (0.2-5), `line_adjust`
(Hz, +-2000, a manual nudge for a stubborn lock), `quality` (a **slider**,
`choices="range:10:95:5"`, default 60 - the JPEG quality the host encodes the
picture at: lower = less bandwidth/CPU, higher = sharper) and `lossless` (0/1,
max-quality JPEG). The frame is small either way (720 x active), so `quality` is
mainly a bandwidth/fidelity knob; the plugin passes raw pixels and the host
(built with libjpeg-turbo) compresses them (GRAY8 -> native grayscale JPEG).

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
- Offline (`plugins/analogtv/build/decoder --file out.cf32`): locks (first
  valid frame ~0.19 s, after the persistence debounce), prints the standard,
  reconstructs. Pearson correlation vs the generator's ground-truth test card
  (best over a small shift): **PAL 0.91, NTSC 0.90** (unchanged by the
  noise-rejection gate, which only gates *when* a lock starts, not the
  reconstruction). Decode time ~6 % of real time (x86 `-march=native`, 15 fps).
  ASan + UBSan clean across PAL/NTSC, all deinterlace modes, forced standard,
  invert, and noise.
- Noise rejection (the fixed false-lock): complex Gaussian noise (`.cf32` at
  2.4 MHz, >= 10 seeds, 2-6 s, various amplitudes) and uniform noise, both auto
  and with a forced standard, give 0 locks / 0 valid frames / 0 events /
  `searching`. Before the gate, Gaussian noise produced 4-10 false
  `Locked PAL|NTSC` events per 3 s clip (uniform noise did not, which is why it
  was missed). Reproduce with complex-Gaussian cf32, not `/dev/urandom`.
- Free-run preview: a throwaway harness with a capturing `kp::Host` (the
  offline `--file` host drops `video_frame`) drives `process()` with
  complex-Gaussian noise and with the PAL/NTSC cards. Noise -> a continuous,
  **changing** 720x576 image at the ~12 fps cap (32/33 consecutive frames
  differ) with **0 valid frames / 0 lock events** (still `searching`); a card ->
  2-3 free-run frames then the switch to the synced 720x576 (PAL) / 720x480
  (NTSC) reconstruction on `Locked`; `free_run=0` -> no frames until lock, lock
  unchanged. ASan+UBSan clean on all these paths. Decode cost of the preview on
  noise ~2-3 % of real time (x86); it does not run once locked.
- Debug playback: `tools/testsignals.json` id `analogtv-pal-testcard` - a wide
  2.4 MHz, 5-antenna, 3 s PAL scene (cu8, lock_vfo, tuner_offset 0, rf
  183.25 MHz). `make_testsignal.py --check` passes; building it produces the
  .dat + sidecar and the decoder locks on the antenna-0 path, emitting the
  expected event.

## Licence

KrakenSDR Source Available (no own LICENSE file); clean-room from ITU-R
BT.470 / BT.1700 timing. See also *tempest* (the shared raster backbone and the
screen-leakage sibling) and `tempest-deep-plan.md`.
