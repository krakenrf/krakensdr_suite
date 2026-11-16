# Analog TV (PAL / NTSC / SECAM)

Receives an off-air **analog television vision carrier** and reconstructs a
live **grayscale luma picture**, shown on the video canvas in the decoder's
tab. It locks the line and field timing from the composite sync pulses,
detects the standard from the measured line rate, resamples each line onto a
raster grid, weaves the two interlaced fields into a frame and renders it
with a per-frame contrast stretch.

It decodes the **picture (luma) only** - no colour (the chroma subcarrier is
largely outside the slice and needs the full channel) and no sound (below).
A TV transmitter is a single known source, so there is no DoA / talker
output.

## ⚠ Bandwidth limitation - read this first

A full analog TV channel is **6 MHz** (System M / NTSC) to **7-8 MHz**
(System B/G PAL, System L SECAM) wide, with a vestigial sideband. An RTL-SDR
tuner delivers only a **~2.4 MHz slice**. Centred on the vision carrier, that
slice carries the sync, the low video frequencies and the coarse picture, but
**not** the fine horizontal detail, the colour subcarrier or the sound
carrier.

At 2.4 MHz the native horizontal resolution is only about **150 samples per
whole line** (2.4e6 / 15625 = 153.6 for PAL, 2.4e6 / 15734 = 152.5 for NTSC),
of which ~125 are active video. The result is a **soft, recognisable luma
image** - large shapes, test-card structure, big captions, overall scene
layout - upscaled to the output size, **not** a sharp broadcast picture. This
is a physical limit of the slice, not a decoder shortcoming, and the decoder
states it in a `Note` fact. Vertical resolution is genuine (the full ~576 /
~480 active lines are locked from sync), so the softness is horizontal.

The sync pulses are low-frequency and survive the slice intact, so **locking
is robust** even though the picture is soft.

## Use

Tune the receiver to the channel's **vision carrier** (e.g. 183.25 MHz for a
PAL System B channel; 61.25 MHz for NTSC channel 3), then pick **Analog TV**
on a VFO (Decimators box → Digital decoder → Analog TV, under *Broadcast &
cellular*). The decoder **locks the VFO** to the tuner's centre at the
widest bandwidth (2.4 MHz) and draws it as a single line - you move it by
tuning the receiver. After a couple of tenths of a second - it confirms a
genuine raster over a few fields before locking (the noise-rejection gate
below), so it will not lock instantly or on noise - it shows `Locked PAL
625/50` (or `NTSC 525/60`) and the picture appears on the video canvas; the
VFO's **Digital** squelch opens while it is locked.

While it is still searching (no signal, or a weak / unsynced one), the canvas
shows a **free-run preview** - the raw envelope mapped straight onto a nominal
raster, exactly as an analog TV shows snow with no input and a rolling picture
as you tune one in. This is display-only (see *Free-run preview* below); it
never affects detection or the lock. Turn it off to get a blank canvas until
lock.

Options (saved per VFO):

| Option | Default | What it does |
|---|---|---|
| **TV standard** | Auto | `Auto` picks PAL vs NTSC from the measured line rate. `PAL`, `NTSC`, `SECAM` force one. SECAM shares PAL geometry and cannot be told apart by timing, so it uses PAL geometry. |
| **Free-run preview** | On | Show a live, unsynced picture while still searching (snow with no signal, a rolling picture with a weak one), like a real TV tuning in. **Display only** - never affects detection or the lock. Off = a blank canvas until it locks (the original behaviour). |
| **Invert brightness** | Off | Off = normal broadcast (negative AM: sync tips are the strongest carrier, peak white the weakest). On flips black/white - for a baseband-composite or positive-modulation input. |
| **Deinterlace** | Weave | `Weave` interleaves both fields into a full-height frame (best vertical detail, can comb on fast motion). `Bob` stretches the newest field to full height (softer, no comb, freshest). `Off` shows one field at half height. |
| **Contrast** | Normal | Per-frame percentile stretch: Normal 0.5-99.5 %, High 2-98 % (punchier), Full 0-100 % (no clipping). |
| **Gamma** | 1.0 | Brightness curve: >1 lifts shadows, <1 deepens them (1 = linear). |
| **Line nudge (Hz)** | 0 | Fine nudge to the assumed line rate if the picture won't lock or tears diagonally. Normally 0; try a few Hz either way. |
| **Picture quality** | default | WebP compression quality for the picture sent to the browser: a number 1–100, or `low`/`medium`/`high` (40/65/90), or `default` (host ~75). The 720 × active frame is small either way, so this is mainly a bandwidth/fidelity knob; the plugin passes raw pixels and the host (built with libwebp) compresses them. |
| **Lossless** | Off | Send the picture losslessly instead of lossy WebP. |

## How it works

1. **Envelope.** The complex baseband is reduced to its AM magnitude `|x|`
   (`video::Envelope`). Broadcast TV uses **negative modulation**, so the
   envelope is an inverted-brightness signal: sync tips are the envelope
   **maxima**, peak white the **minimum**.
2. **Sync lock.** `video::find_broadcast_sync(…, sync_is_min = false)` tracks
   the periodic sync-tip minima (after it flips the envelope so sync is the
   extreme) to recover the line period and the vertical-sync runs over a
   rolling buffer of ~2.8 fields.
3. **Line period from the field period.** The per-line tip positions give a
   rough line rate; the standard (PAL vs NTSC) comes from it. The precise
   line period is then taken from the **vertical-sync cadence** - successive
   field syncs are exactly `total_lines / 2` line periods apart, so their
   spacing divided by that count is far more accurate than per-line medians
   and cancels the diagonal shear a 0.03 % line-rate error would cause over
   ~300 lines. This also makes the field detection robust: a bright picture
   can trip the shared detector's median sync threshold and add spurious
   vertical-sync runs, so the decoder keeps only the runs that repeat at the
   field cadence.
4. **Raster.** Each field's active lines are resampled (`video::resample`,
   linear interpolation) onto a `720 × active-lines/2` grid anchored to the
   nearest sync tip (so the horizontal phase matches the sync grid), then the
   two fields are woven into a `720 × active-lines` progressive frame.
5. **Render.** A light temporal average (not heavy - broadcast content moves)
   and a per-frame percentile contrast stretch (`video::FrameAccumulator`)
   produce an 8-bit GRAY8 frame, brightness inverted for negative-AM video,
   sent with `kp::Host::video_frame` ~15 times a second (720 × 576 PAL / 720 ×
   480 NTSC).

On a confident lock it reports each frame's samples with
`host.valid(start, end)` (drives the "receiving" state and the Digital
squelch) and emits `Locked <standard>` once.

**Free-run preview (display only).** Whenever the decoder is **not** locked
(searching, or a weak / unsynced signal), it also maps the raw AM envelope
straight onto the nominal raster for the selected standard - **auto defaults
to PAL 625/50** - and sends it with `video_frame`, so the canvas always shows a
live image: snow with no input, a rolling / tearing picture with a weak signal
(the roll is correct - there is no sync to anchor to). The brightness mapping
is identical to the locked path (negative-AM invert, the `invert` / contrast /
gamma options). It is purely a preview: it never calls `host.valid()`, never
emits a `Locked` event, and never touches the lock, periodicity or debounce
state, so the facts stay at `searching` and the noise-rejection gate is
unchanged. It is capped at **~12 fps**, drawn at **720 × active** (PAL 576 /
NTSC 480), and sent at a lower WebP quality (a throwaway preview; noise frames
never dedup, so the modest size + rate bound the bandwidth). The moment a real
raster locks, the synced reconstruction takes over unchanged. The **Free-run
preview** option (default on) turns it off for the original blank-until-lock
behaviour.

**Noise rejection.** A sync + field cadence can appear by luck in complex
Gaussian noise (whose envelope is Rayleigh, with heavy-tailed spikes the sync
tracker latches onto), so a matched line/field rate alone is not enough. Before
locking, the decoder also requires:

- **Genuine line-rate periodicity.** The envelope's autocorrelation
  (`video::PeriodFinder`, the same confidence gate the *tempest* plugin uses)
  must show a strong, prominent peak at the detected line period, and that
  period must agree with the sync-derived samples-per-line. Noise has a lucky
  cadence but no raster, so its line-period autocorrelation is flat and it
  fails this outright (measured: a real test card gives autocorrelation
  confidence/peak ~0.9-1.0, noise < 0.04).
- **Persistence.** The matched standard and samples-per-line must hold over
  several consecutive builds before the lock is declared, so a single lucky
  field pair never counts.

The autocorrelation is the only added cost, so it runs on a candidate (all the
cheap gates already passed) and, once locked, at most once a second - not on
every block. The result: it only `valid()`s on a genuine, persistent raster, so
it does not steal **Auto detect** from other decoders and does not lock on noise
(verified: complex Gaussian noise across many seeds and lengths, and uniform
noise, produce 0 locks, 0 valid frames, standard `searching`).

## Standards and timing (ITU-R BT.470 / BT.1700)

From `video::tv_standards` / `video::match_tv_standard`
(`plugins/lib/video_raster`):

| Standard | System | Lines/frame | Active | Line rate | Frame | Fields |
|---|---|---|---|---|---|---|
| PAL   | 625/50 | 625 | 576 | 15625 Hz      | 25 fps     | 50, interlaced |
| NTSC  | 525/60 | 525 | 480 | 15734.266 Hz  | 29.97 fps  | 59.94, interlaced |
| SECAM | 625/50 | 625 | 576 | 15625 Hz      | 25 fps     | 50, interlaced |

Horizontal timing used (System B/G PAL: sync 4.7 µs, back porch 5.7 µs,
active 51.95 µs of a 64 µs line; System M NTSC: sync 4.7 µs, back porch
4.5 µs, active 52.86 µs of a 63.556 µs line). SECAM is decoded with PAL
geometry (the line / frame rates are identical, so rate matching cannot
separate them).

## No sound (yet)

**Audio is out of scope for v1.** The TV sound carrier sits **+4.5 MHz**
(System M) or **+5.5 MHz** (System B/G) above the vision carrier - outside
the 2.4 MHz slice when the receiver is centred on the vision carrier. The
decoder sets `voice = false` and does not fake audio. A future version could
retune a second VFO to the sound carrier (FM) to add it.

## Tests

`tools/analogtv_gen.py` renders a clean-room grayscale test card (gradient,
centre disc, vertical bars, black/white squares) into a composite PAL or
NTSC signal with full H-sync, vertical sync and two **truly interlaced**
fields (split at `total_lines / 2 = 312.5` / `262.5` lines, the real
half-line offset), negative-AM modulates it onto the vision carrier and
writes a `.cf32` / `.cu8` recording (single- or multi-antenna) plus a
sidecar; `--gt` saves the ground-truth luma for a correlation harness.

```
tools/analogtv_gen.py out.cf32 --standard pal --seconds 2 --rate 2400000 --gt gt.pgm
build/decoder --file out.cf32            # locks, prints the standard, facts
```

Offline results (synthetic test card at 2.4 MHz, decode time measured on the
full `process()` path):

| Standard | Lock | Line rate (true) | Field rate | Reconstruction correlation | Decode time |
|---|---|---|---|---|---|
| PAL 625/50  | ~0.19 s | 15625.7 Hz (15625)      | 50.00 Hz  | **0.91** | ~6 % of real time |
| NTSC 525/60 | ~0.19 s | 15733.5 Hz (15734.266)  | 59.94 Hz  | **0.90** | ~6 % of real time |

Lock takes a couple of tenths of a second (not instant) because of the
noise-rejection persistence gate above; the autocorrelation gate adds no
measurable decode time (it short-circuits the reconstruction on a non-raster
candidate, so decode time on noise is if anything lower).

Correlation is the Pearson correlation between the reconstructed 720 × active
frame and the ground-truth test card (best over a small shift search), i.e.
large-scale structure is recovered faithfully; the fine horizontal detail is
limited by the ~125 active samples per line, as expected for a 2.4 MHz slice.
Complex Gaussian noise (several seeds and lengths) and uniform noise produce
**no lock and no valid frames** - the line-rate periodicity + persistence gate
above rejects them, including when a standard is forced.

The free-run preview was checked with a capturing host (the offline `--file`
host drops `video_frame`): on complex-Gaussian noise it emits a continuous,
**changing** 720 × 576 image at the ~12 fps cap with **0 valid frames / 0 lock
events** (still `searching`); on a real card it emits 2-3 free-run frames and
then switches to the synced 720 × 576 (PAL) / 720 × 480 (NTSC) reconstruction
on lock; with the option off, no frames appear until lock. The preview adds
~2-3 % of real time on noise (x86); on a locked signal it does not run.

`tools/testsignals.json` registers the PAL test card for the sidebar's
**🧪 Debug Playback** (a wide 2.4 MHz, 5-antenna scene written straight to
`.cu8`); its check expects the `Locked PAL 625/50` event.

## Files

- `decoder.cpp` - the decoder (`AnalogTv`) and `KRAKEN_PLUGIN` registration.
- `tools/analogtv_gen.py` - the composite-video test-signal generator.
- `tools/testsignals.json` - the Debug Playback catalogue (one PAL entry).

The picture-reconstruction DSP (envelope, sync recovery, resampler, frame
accumulator, standard tables) is the shared `video::` backbone in
`plugins/lib/video_raster.{hpp,cpp}`; this plugin adds the broadcast-TV front
end. Everything is clean-room from the ITU-R TV standards and textbook DSP -
no GPL analog-TV code was consulted.
