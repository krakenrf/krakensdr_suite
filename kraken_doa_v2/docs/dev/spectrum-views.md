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
  2048. FFT Settings' **Slow link** tick (per browser, localStorage
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

