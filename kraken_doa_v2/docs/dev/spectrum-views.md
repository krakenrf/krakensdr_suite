# Spectrum views (FFT message type 7)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Spectrum views (FFT message type 7) - `MessageBuilders::build_spectrum_view`, websocket_server.cpp `handle_fft_view` / `send_spectrum_views`, kraken_doa.html "SPECTRUM VIEWS":**
- The user's request (2026-10-17): smaller spectrum frames to the browser,
  and zoomed-in spectra that show more detail without a higher data rate.
  Before: every page got the same frame (type 0, 20 / s): a float32
  frequency for every point + u8 min + u8 max envelope = 6 bytes per point,
  2048 points (16384 / the old downsampling 8), ~250 KB/s per page, uncompressed;
  the page drew only the max envelope and zoomed by cutting that frame (16x
  = ~100 points). Measured on real spectra (the array at 416.8 / 98 / 433.9
  MHz, scratch sp/analyze.py): no frequencies = 34 %, + deflate 16-20 %,
  max only + deflate ~9 % of the bytes; frame-to-frame deltas / keeping the
  deflate context added only a few % - not done
- The page sends `FFT_VIEW:<display>:<seq>:<lo Hz>:<hi Hz>:<points>` per
  display (0 = the main spectrum, 1 + ch = independent-mode tuner ch; lo / hi
  from that display's centre, so a retune needs no new request; lo >= hi =
  the whole band; points = the plot area's width in device pixels - uPlot's
  bbox / a pane's width x dpr). Transport-level like SUBSCRIBE (never reaches
  the control handler, not persisted, per connection). The first one moves
  the socket off TOPIC_FFT (the shared type 0 / 6 broadcast) into
  `g_spec_sockets`. Junk is ignored (`is_finite_value`)
- Every 50 ms tick `build_fft_message(&snap, legacy)` runs as before (it
  feeds the scanners and stitches the wideband scan's spectrum) and also copies
  each display's normalized full-resolution spectrum into a
  `SpectrumSnapshot` (centre in exact Hz - `coherent_tuner_hz()` /
  `exact_tuner_hz()`; the old header's float32 MHz is ~30 Hz coarse); the
  type 0 / 6 message is encoded only while a page without views is
  subscribed (`numSubscribers(TOPIC_FFT)`). `send_spectrum_views`: per view
  socket, every display of the mode (independent: one frame per tuner),
  `ws->send(.., true)` (compressed); a socket with > 256 KB buffered skips
  the frame (uWS would close it at maxBackpressure 2 MB)
- Type 7 (little-endian, 64-byte header): u32 7 | u8 display | u8 marker
  (0x80|ch coherent, 0xFE wideband scan, ch independent) | u8 channels | u8
  flags (1 = the requested window) | u32 seq | f64 centre Hz | f64 band lo /
  hi Hz (first / last bin of the whole spectrum) | f64 start Hz | f64 step Hz
  | f32 gain dB | f32 averaging alpha | u32 n | u8 level[n] (0..255 = 0..120
  dB as before). Points: the bins whose centres lie in the window, MAX-pooled
  (narrow peaks kept) by `pool = max(1, ceil(count / budget), round(count /
  points))` - 0.67-1.5x the plot's width (rounding up made 819 bins in an
  816-pixel plot 410 points, half the resolution), budget =
  `SPECTRUM_VIEW_MAX_POINTS` 4096 (config.hpp; points 0 =
  `SPECTRUM_LEGACY_POINTS` 2048); a point sits at its pool's middle. Zoomed
  in, the same points cover less spectrum, down to one FFT bin each - the
  FFT size is the resolution limit
- No "Downsampling" setting (removed 2026-10-17, the user's call: the page's
  width does its job): `current_fft_decimation`, `FFT_DECIMATION` handling
  (accepted + ignored, not echoed - `is_query_command`), the saved
  `fft_downsampling` and FFT_SIZE's auto-adjust are gone; old-format frames
  (types 0 / 6 / 4 without views) use `legacy_fft_decimation()` = FFT size /
  2048. The Display box's **Slow link** switch (per browser, localStorage
  `kraken_spec_slow`) halves the points the page asks for (`specBinsMain` /
  `specBinsPane`); "Points sent" shows the selected display's frame size
- Beamformed overlays (type 4): `MessageBuilders::beamformed_spectrum` once
  per VFO per tick (normalized, the beamformer's 1024 bins), then
  `build_beamformed_fft_message(bf, point_step_hz)` per page: max-pooled by a
  power of two (points evenly over the band - the page places point i at
  centre - bw/2 + i bw/n; >= 16 points) to the spacing of that page's last
  main frame (`PerSocketData::main_step_hz`, set by build_spectrum_view) -
  so it gets finer with the zoom; pages without views get it on TOPIC_FFT at
  the type 0 spacing
- Compression: permessage-deflate negotiated (`SHARED_COMPRESSOR |
  SHARED_DECOMPRESSOR`, no context per socket), used for spectrum frames only
  (types 0 / 6 / 7 / 4 - audio and JSON stay uncompressed)
- Page: `specOnFrame` (header -> `specHeaderSync`, the type-0 header's side
  effects factored out: mode hint, channel, digits, gain / averaging sliders,
  centre) -> main `specMain` + `applyClip` -> `specApplyMain`: `specSpan1`
  (the edge-clipped band; the wideband scan's isn't clipped again) ->
  `zoomWindow(z, lo1, hi1, centre)` (zoomSlice's rule on absolute MHz, keeps
  z.off inside) -> `specWindow(frame, w0, w1)`: the frame's points in the
  window, padded with 0 dB at its spacing where it doesn't reach (zoomed out,
  until the wider frame arrives) - so a zoom answers at once from the last
  frame and the detail follows ~50-100 ms later; evenly spaced (the
  waterfall maps by index). Panes: `specPaneFrame` (as indepOnFrame). Every
  frame / redraw calls `specViewWant` -> `FFT_VIEW` when the window or width
  changed, at most every 100 ms + a trailing send (not while `applyingRemote`
  - the next frame asks again); reset on reconnect. The legacy paths
  (type 0 / 6) ask too - a server with views then switches. Zoom readout
  `#zoom-res`: the points' spacing ("146 Hz"). A server without views just
  keeps sending type 0 / 6 (it ignores FFT_VIEW silently - checked)
- Not changed: the waterfall's older rows keep the span they were drawn with
- Tests (job scratch 46eef211 sp/; fake heimdall `--cw2 100600` = carriers
  600 Hz apart at +100 kHz): t_spec.py (protocol, three clients at once,
  wire bytes from the kernel per connection): an old page without
  compression 250 KB/s, an old page with it 109 KB/s (type 0 compressed),
  a page with views 1x 19.7-21.4 KB/s (12x less), 16x 14.8 KB/s at one FFT bin
  (146.5 Hz) per point - the two carriers resolved (59.8 / 26.4 / 56.0 dB),
  one point at 1x; header (exact centre, band, window, seq), whole band = 2048
  points, junk FFT_VIEWs ignored - 20 / 20. t_spec_ui.py (headless Chromium,
  real wheel events): type 7 used, points ~ the plot width (819 for 816 px),
  edge-clipped 1x window, wheel to 16x -> 146 Hz, carriers resolved, readout,
  waterfall line, <= 12 view requests for 14 notches, zoom out, edge clip
  0.5, independent mode: 5 panes each at its width, a zoomed pane as fine as
  its pixels, the others untouched; the same page against the server from
  before the change (type 0, client-side zoom); no page errors - 20 / 20. The
  page's whole WebSocket traffic (audio + DoA + status included): 262 KB/s
  -> 26-27 KB/s

## Network rate: the receiver's byte counters (NET_STATS, 2026-11-22)

The user: "show the network bandwidth used ... where it shows Points sent.
Show it for all modes." The page can count only what it gets after TLS and
permessage-deflate; the true number for its connection is the kernel's.

**Contract.**
- `NET_STATS:<1|0>` (also true / false; junk ignored) - transport level, per
  connection, every mode, behind the AUTH gate like `FFT_VIEW`; off by
  default. An older receiver ignores it (no reply, no log line).
- While on: `{"net":{"tx_bytes":N,"tx_sent":S,"rx_bytes":M,"t_ms":T}}` at
  once (a baseline) and then every 1 000 ms (on the 50 ms broadcast tick:
  measured 1 000 ms apart). `t_ms` = the receiver's monotonic clock; the
  page takes the rate from differences. Whole numbers below 2^53.
  - `tx_bytes` = `tcpi_bytes_acked`: receiver -> browser bytes the
    browser's TCP acknowledged - TCP payload: the TLS records (their
    overhead included) carrying the WebSocket frames, after
    permessage-deflate. Not in it: TCP / IP headers (~3-4 % more on the
    wire), retransmissions, and what still waits (uWS's queue, the kernel's
    send buffer, in flight) - so on a link that can't keep up it rises at
    the link's real throughput. The number to show.
  - `tx_sent` = `tcpi_bytes_sent` (only when the running kernel fills it,
    Linux 4.19+): sent incl. retransmissions; `tx_sent - tx_bytes` = in
    flight + retransmitted.
  - `rx_bytes` = `tcpi_bytes_received`: browser -> receiver (the page's
    commands).
  - The counters run since the TCP connection opened (TLS handshake and the
    HTTP upgrade included): differences only.
- No counter (Windows, macOS, a kernel without TCP_INFO's byte fields): no
  message; the page counts by itself.

**The receiver.** `net_stats::read` (src/networking/net_stats.cpp; Linux:
`getsockopt(TCP_INFO)` with `<linux/tcp.h>` - glibc's `<netinet/tcp.h>` may
lack the newer fields and the two don't mix; a field past the length the
kernel returns is not taken; elsewhere a stub that says "no counter"). The
descriptor: `us_poll_fd((us_poll_t *) ws)` - uWS's WebSocket is its
`us_socket_t`, an SSL socket's struct begins with it and that with the poll,
so this is the TCP socket under the TLS (no OpenSSL call).
websocket_server.cpp: `handle_net_stats`, `g_net_sockets`,
`net_stats_tick()` at the top of the broadcast timer. Cost: one getsockopt
a second a page - 0.46 us a call on x86 (timed from Python, an upper bound),
0.60 us on the Pi 5 - and a ~90-byte message.

**Measured against an independent count** (t_net.py: a TCP proxy in front
of the receiver counts what it forwards; the fake heimdall; x86 / Pi 5):

| case | receiver | proxy | the page's own count (after inflate / TLS) |
|---|---|---|---|
| coherent mode (spectrum, audio, JSON) | 117.1 KB/s | 117.1 (+0.00 %) | 291 KB/s (2.5x) |
| sweep, fit | 19.1 | 19.2 (-0.36 %) | 41.7 (2.2x) |
| sweep, a step (65 536) | 32.0 | 32.0 (+0.00 %) | 56.7 (1.8x) |
| sweep, full (11.9 M bins) | 3 266 | 3 266 (+0.00 %) | 3 285 (1.01x) |
| two pages: full / fit | 3 986 / 18.9 | the same | 4 007 / 41.3 |

The Pi 5 the same within 0.4 KB/s in every case (coherent 116.8 / 116.8, full 3 265 / 3 265; the page's own count 2.5x / 1.01x). Audio was already on in coherent mode (250 Opus messages in 9 s; the VFO's speaker toggle added ~25). The x86 ASan + UBSan build: the same numbers, 14 / 15 (it dropped engine packets during the full sets, as before - not this change). So the page's own count, the fallback, reads 1.8-2.7x high where
the frames are deflated (spectrum, JSON) and right where they are not (full
sets, audio). Tests (t_net.py 15 / 15, x86 normal and ASan + UBSan, Pi 5):
off by default, junk ignored, a baseline at once, `tx_sent` >= `tx_bytes`,
the five cases above within 3 % (all within 0.4 %), each page its own
counter, `NET_STATS:0` stops, a page that never asked gets none.

## The live waterfall recolours (2026-11-21)

The user: "Do the same recolour fix for the live waterfall too" (sweep mode's
waterfall had learnt it that day, *sweep-mode.md*). Before, `WaterfallDisplay`
kept only pixels: a change of the colour map or of the contrast coloured the new
lines, and the lines already on screen kept their old colours. This applies to the
main waterfall (coherent, wideband scan) and to the tuner panes of independent mode.

- **What is kept.** `lv`: one byte per pixel, laid out like the ImageData. It
  holds the level the pixel was painted from: 0..254 = -20 .. +80 dB in 0.39 dB
  steps (`WF_LV_*`). The frames' own steps are 0.47 dB (u8 over 0 - 120 dB). The
  contrast can't tell more apart: Min >= -20 and Max <= 80 (sliders and
  `computeAutoRange`), so a level beyond either end gets the end's colour anyway.
  255 means no line (black). `rowMin` / `rowMax` hold each pixel row's range (the
  one it was painted with; 16 bytes a row). A line is painted from its stored
  levels through a 256-entry table per range (`lutFor`, packed 32-bit pixels), so
  a repaint is exactly a fresh paint. Compared with painting the level itself (the
  code before), the colour index differs by at most 1 where it differs. Measured
  on the receiver's frames: about half the pixels at a 45 dB range, never more
  than one step.
- **Bearing colours** (*bearing-spectrum.md*: the main waterfall's lines take
  the bearing's hue at the level's brightness). `hue` is a second plane, two
  bytes a pixel (0..359 = the row of `binDoaLut`, 0xFFFF = none). It is made with
  the first coloured line and dropped once none is left on screen (`hueAge`). So a
  contrast change repaints coloured lines too: the hue stays and the brightness
  0.2 + 0.8 x the colour scale follows. `binDoaWfTint` now returns `{at: f ->
  hue | -1, rgb}` instead of painting.
- **Bounds.** 1 byte a pixel: 1.31 MB at 1920 x 680, 0.61 MB for the main
  waterfall in a 1920 px window with the sidebar and DoA pane (904 x 677), 65 KB
  for a 360 x 180 pane. Another 2 bytes a pixel only while bearing-coloured lines
  are on screen. The calibration burst's orange marker sits in the left margin,
  which a repaint never touches. Speed sets the height of new lines only. A
  resize stretches the levels and the rows' ranges exactly as it stretches the
  pixels. A zoom changes nothing: older lines keep the span they were drawn with,
  and only their colours follow. Sweep mode's instance never calls `addLine`, so
  it never gets the store: no second copy of its rows, and no repaint over its
  pixels. `swpWfRecolour` still does its repaint, and `clear()` empties the store.
- **When it repaints.** `recolour()` repaints at the next screen refresh
  (requestAnimationFrame), so a slider drag repaints at most once a refresh. A new
  line in between paints only itself, and the full repaint waits for the
  refresh. A waterfall that is not shown (another mode's) waits until it is shown
  again. A value that doesn't change repaints nothing: `setColormap` with the
  same map, or `setRange` with the same range when every line already has it.
- **Auto does not recolour (the orchestrator's call, from a measurement).** The
  first version repainted every line whenever Auto's range moved, as sweep mode's
  waterfall does. Measured with scratch `t_auto.py` (coherent mode, the receiver
  worker's RF-scene fake, Auto's 500 ms tick, 60 s per band):
  - On a static scene (98 / 433.92 / 120 / 162 / 300 MHz) the range never moved.
  - With signals that come and go (0 - 3 strong carriers or 200 kHz bands near
    the centre, changed every 1 - 3 s), it moved 16 - 22 times a minute. At
    98 MHz 13 of the 16 moves flipped back within 5 s (-10:35 <-> -10:40).
  - At every move 92 - 100 % of the waterfall's pixels visibly changed colour,
    the mean change reaching 38 / 94 / 126 of 255.
  - In independent mode one pane flipped between -5:45 and 0:45 20 times in 20 s
    even on the static scene (`t_indep.py`).

  The whole picture flashing up to 20 times a minute is clearly worse than the
  bands Auto leaves when only new lines take its range. So the rule is:
  - Auto's own moves (`setRange(min, max, true)`: `autoRangeWaterfall` ->
    `updateWaterfallRange(true)`, the panes' tick, a pane's Auto switched on)
    colour only the new lines; each line keeps its range, as before.
  - Switching Auto on repaints nothing.
  - A change the user makes repaints every line, which then takes the range set:
    Min / Max by hand (also from another browser, `viewOnRemote`, and a pane's
    own View), or Auto switched off (`toggleAutoRange(false)` ->
    `updateWaterfallRange()`; the range where Auto left it becomes the manual
    one).
  - A colour-map change repaints every line with its own range, so the picture
    keeps Auto's bands, in the new colours.

  Sweep mode keeps its own rule (Auto repaints its rows; a sweep line comes every
  few seconds). Don't make the live Auto recolour without new measurements.
- **Cost** (scratch `t_perf.py`, headless Chromium on the shared 16-core dev
  machine, medians; the same frames through the class before the change, taken
  from the page at HEAD):
  - A full repaint at 1920 x 680: 1.4 - 1.8 ms (max 2.2; 1.5 - 1.7 / max 3.1 in an earlier run at load 11) - the sweep's repaint of 570 rows at 1573 x 677 is 21 ms, it paints through its rows' windows.
  - A new line at 1920 x 680: 0.46 - 0.52 ms against 0.40 - 0.45 ms before (+12 - 18 %: the level plane and the rows' ranges scroll with the pixels; speed 1 and 5).
  - 904 x 677: 0.19 ms a line against 0.17 - 0.18 ms, a repaint 0.7 ms.
  - Phone sizes (400 x 300, 360 x 180): 0.015 - 0.03 ms a line (before 0.015 - 0.025), a repaint 0.1 ms; load 3 - 4 for these numbers. The picture is the same as the old class's within one colour step on 99.9 % of the pixels (the rest: the SDR# map's flat stretch, where equal colours hide the index).
- **Tests** (scratch `52196024/tmp/live/`; `stack.sh` = a clean test build with
  ports 288xx and the receiver worker's fake with its RF scene, or the coherent
  UCA fake with `FAKE=simple`; real mouse / key events; every check compares each
  plot pixel of the ImageData with a fresh paint from its level and its row's
  range, and the canvas with the ImageData):
  - `t_live.py` 30 / 30. Covered:
    - Auto on: a move leaves the old pixels as they were (0 px) and repaints
      nothing, with three ranges on screen at once.
    - Auto on + Colors: every line recoloured with its own range.
    - Auto off by the switch: every line takes the manual range.
    - Colors by a key, Min dragged (40 pointer moves: 37 repaints over 54 - 58
      refreshes, never two in one), Max by keys.
    - Unchanged values (remote, the appliers, setRange / setColormap) repaint
      nothing; Speed repaints nothing; a zoom repaints nothing.
    - A second browser's Colors + Max: this page repaints.
    - A resize to 1500 x 950 keeps the levels with the pixels; Colors after it.
    - Auto on again: 0 repaints in 20 s.
    - Quantisation: the colour index moves by at most 1.
  - `t_indep.py` 14 / 14. Covered:
    - A pane's own Colors repaints that pane only (pane 0: 0 px changed, no
      repaint).
    - Its own Auto off + Min by keys repaints it.
    - "View · all tuners" Colors repaints all five panes.
    - The panes' Auto (up to 20 moves in 20 s) repaints nothing, and every line
      matches a fresh paint with its own range.
  - `t_bin.py` 7 / 7. Covered:
    - Bearing colours: 298 440 coloured pixels, consistent; Max -6 dB repaints
      them (165 200 px of the top 200 rows changed).
    - The hue plane goes after the colours are switched off.
    - The calibration burst's marker (1 500 px) is kept through a recolour.
  - `t_auto.py`: the measurement above, then the same dynamic scene with the
    rule. 14 / 25 / 19 moves a minute at 98 / 433.92 / 300 MHz, 0 repaints;
    each move again would have changed 91 - 100 % of the pixels.

## A picture of the spectrum and the waterfall (2026-11-21)

The user: "add an option to save a screenshot of the FFT and waterfall together".
It lives in kraken_doa.html, script block "SPECTRUM PICTURE" (after the View
popover's).

- **The button.** A camera icon (`camera`, *icons.md*) sits right after View on
  the spectrum's header (`#snap-btn-main`, 6 px from View, as tall as it; 32 px on
  a phone). That header is the same in sweep mode, so the button needs nothing in
  the sweep block. In independent mode it saves the selected tuner, and its title
  says so (`snapBtnTitle`, called with `viewBtnTitle`). Each tuner pane's header
  has its own after its View button (`#tpane-pb-N`; the click also selects the
  pane). It fits: every pane header stays on one line with the button inside it,
  at 504 px panes (1920 window) and 483 px panes (1366 window).
- **The picture** (`snapRender`). The display's canvases are drawn onto one
  canvas at their places on screen, in device pixels (`devicePixelRatio`):
  - The spectrum canvases come at their own resolution. uPlot's is plot width x
    dpr; sweep mode's and the panes' are dpr too.
  - Canvases kept in CSS pixels (the waterfalls, the main display's VFO overlay
    `#fo`) are scaled up without smoothing.
  - Under them go the boxes' backgrounds and top borders as the page shows them
    (computed styles, so the light look's colours too).
  - What is in it is everything drawn on a canvas: the trace, grid, axes and
    their labels, the tuning bars and their labels, squelch lines, the bearing
    colours and their ring, sweep mode's progress bar and its messages, the
    waterfall and its VFO overlay.
  - What is not: the DOM over the canvases. That is the pointer's level box,
    uPlot's cursor lines, the "DEBUG PLAYBACK" banner (the caption names it), and
    a pane's header (its frequency and gain go into the caption). The pointer's
    readout in sweep mode is on a canvas, but the pointer is on the button when it
    is pressed.
- **The caption** sits under the picture, on --s2 with --t1 text and an --l1
  line. Its parts, joined by " · ":
  - "KrakenSDR", then the mode: "Coherent · Antenna N", "Wideband scan",
    "Independent · Tuner N" or "Sweep · K tuners".
  - The centre, to the Hz where it has them; the range shown (about a thousandth
    of the span as the last digit; in sweep mode the sweep's Range, plus Shown
    when zoomed).
  - FFT N (sweep mode: with its bin width) and a point's width (the View
    popover's readout).
  - The zoom when > 1, the gain (the top bar's, or the pane's), and the date and
    time, local with the UTC offset ("2026-11-21 23:11:11 UTC+13:00").

  It is one line at 12 px, shrunk down to 10 px where it would not fit. A picture
  too narrow even for that (a pane, a phone) gets it on more lines, broken
  between its parts and never cut. In the tests every picture's caption fitted
  one line.
- **The file.** Its name says what and when:
  `krakensdr_<coherent|wideband|sweep|T<n>>_<lo>-<hi>MHz_<YYYYMMDD>_<HHMMSS>.png`,
  for example `krakensdr_sweep_400-700MHz_20261121_231218.png`. It is the range
  shown, at local time. The PNG is saved through `canvas.toBlob` -> a blob: URL
  -> a hidden `<a download>` clicked. The URL is revoked after 60 s, because
  Firefox and Safari read it after the click returns. A browser without `toBlob`
  goes through `toDataURL`. Nothing is sent and nothing is uploaded; there is no
  library (the page works offline).
- **Browsers.** Only headless Chromium was tested. The rest is from how these
  browsers handle a blob download, not tried here:
  - Firefox and Safari desktop take `<a download>` with a blob: URL.
  - iOS Safari (13 and later) asks whether to download the file and puts it into
    Files > Downloads, not into Photos. iOS 12 and older would open the PNG in a
    tab, where a long press saves it.
  - Over HTTPS with the self-signed certificate nothing changes: the blob belongs
    to the page's own origin.
- **Tests** (scratch `t_snap.py`, 117 / 117).
  - What every picture is checked for:
    - Pressed by a real click (a touch tap on the phone).
    - The download seen through the DevTools events (`Browser.downloadWillBegin`
      with a blob: URL and the suggested name, then the file in a folder).
    - The PNG decodes (PIL) and its size equals the display's area in device
      pixels plus the caption strip.
    - Every display pixel equals a composite made in the test from the page's
      own layers (each canvas read back with getImageData, each box's
      background), within 3. The stream is held meanwhile, so the canvases read
      back are the ones in the picture.
    - Known pixels: the trace's peak where uPlot puts it, and waterfall pixels
      where they are on screen.
    - The caption's strip, text and parts, and the file name.
  - Modes and settings covered: coherent at 433.92 MHz; zoomed x3.8; Slow link;
    dpr 2 (1600 x 1000: the spectrum canvas at its own 1436 px inside a 1440 px
    picture, which is #fp x 2); a phone (400 x 860, touch, dpr 2: the button 32
    px next to View); independent (a pane's own button, and the header's button
    for the selected pane); wideband scan; sweep mode with the receiver's engine
    (400 - 700 MHz, 5 tuners), whole and zoomed x3.1. In sweep mode the sweep's
    waterfall still keeps no level store.
