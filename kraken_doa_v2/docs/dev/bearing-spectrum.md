# Bearing spectrum (FFT message type 9, `BIN_DOA:`)

Not loaded automatically. *Italic* topic names refer to the other files in
this folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`).

**The spectrum coloured by direction - `src/signal_processing/bin_doa.cpp`
(`include/signal_processing/bin_doa.hpp`), `MUSICProcessor::azimuthSteering`,
kraken_doa.html "🎨 BEARING SPECTRUM":**

- The user's request (2026-11-18): "a mode that colors the entire fft
  spectrum bins with different colors based on the doa result of that bin".
  Without a VFO on a signal there was no bearing; with this every signal in
  the 2.4 MHz shows its direction at a glance - signals from one direction
  share a colour, a hopper or a short burst shows where it came from, an
  interferer stands out from its neighbours
- MUSIC DoA > **Bearing colours**, the row under DoA ON / OFF, with the
  legend ring and the state / park-reason line under it (moved there from
  Display on 2026-11-19, the user's call). Its own switch: it works with
  DoA off (nothing there greys it out). The MUSIC DoA box is coherent-only
  (`.coh-only`, hidden in wideband scan / independent mode) - so is the
  mode, which is parked there anyway; the reason then shows on the
  spectrum's legend (wideband scan) - kept that way rather than giving the
  row an exception. `BIN_DOA:0|1` is the receiver's setting (persisted -
  schema `bin_doa`, off by default -, echoed, replayed to new pages; every
  page follows it). EVERY point of the main spectrum is drawn
  in its bearing's colour at full strength: the trace line itself (uPlot's
  own stroke is made transparent while a fresh frame exists and the line is
  drawn point by point by the draw hook - each point's half segments to its
  neighbours, runs of one colour in one path), a translucent column (alpha
  0.3) from 0 dB up to the point, and the main waterfall (each new row: the
  bearing's hue at the level's brightness, 0.2 + 0.8 x the colormap's 0..1;
  a row keeps the colours it was drawn with). The noise between signals is
  coloured too - its bearing is random, so it shows as a mix. Independent-
  mode panes are not coloured (no coherent array there)
- The user's calls after trying it on the real array (2026-11-18): the
  first version coloured only "confident" points (a noise test x a
  steering-fit test x a "power now" test) - on the real KrakenSDR at 90 MHz
  only 3.8 % of the points visibly above the floor got a colour and a narrow
  carrier none (measured below: the steering fit killed them, a real array
  matching its model ~0.4); "the coloring should color the FFT line itself
  too"; then "Probably best to just ignore the confidence factor, and
  display everything". So: no gating, no fading by confidence, no slider.
  The confidence byte stays on the wire (informational, for native clients)
- Colour: hue = the point's compass bearing (360 - MUSIC azimuth: N red, E
  yellow-green, S cyan, W violet; `binDoaRgb`). Legend: a colour ring
  oriented like the DoA plots (`PolarPlot.azPos`: azimuth 0 = ANT0 at the
  top, 90 left) with N/E/S/W or 0/90/180/270 by the DOA Angles convention -
  top right of the spectrum (with the bearing under the cursor, or "paused"
  and why), in the MUSIC DoA box next to the tick, and as a thin ring around every
  VFO's DoA plot while the mode is on, so a VFO's lobe and the spectrum's
  colour can be compared directly
- Parked - no valid bearing exists (no frames, the reason in `{"bin_doa"}`,
  shown under the tick and on the plot): outside coherent mode (wideband
  scan, independent, generic RTL-SDRs), while the noise source is on or just
  was (`doa_is_calibrating`), after a retune (`doa_retune_hold_active`, the
  DoA's hold), while heimdall reports the calibration stale (0x200; the
  KerberosSDR after a retune), when the array model gives no steering
  (custom positions not set, a UCA of < 3 elements). Every park drops the
  averages. heimdall's noise flag trails the samples by up to 2 packets: a
  packet is only used once `HOLD_PACKETS` (3) more have come unparked, so
  the noise source's first samples (the same on every antenna = a false
  bearing everywhere) never reach them (measured below)

## Design

- Data: in coherent mode the receiver converts every antenna while
  `bin_doa::wanted()` (on, a page takes type 9, coherent, not generic) -
  with DoA on it does anyway. At the end of each packet's pass the
  decimation thread MOVES the packet's antenna vectors to the worker
  (`bin_doa::on_packet`; nothing reads them after that point - no copy), with
  the park reason. A queue of up to 8 packets (the decimation thread hands
  them on in bursts: a one-slot mailbox lost most of them in the test); past
  that the oldest is dropped (a gap; blocks restart after it; counted in the
  coverage). Whole blocks are taken straight from the packet's vectors (a
  remainder goes through a stash - never with heimdall's 16384-sample
  packets: copying every packet cost ~2 % of a core)
- Worker thread `bin_doa` (started on the first enable): blocks of `nb`
  samples of every antenna (`nb` = the spectrum's FFT size, 1024 ..
  `MAX_FFT` 8192; contiguous samples only - a packet gap restarts the block
  assembly), Hamming window (as the spectrum), pffft per antenna, DC-centred
  bins like the spectrum. Per bin the snapshot (its M antennas) is
  normalised to power w (the block's weight), then the M x M cross-spectra
  x_a conj(x_b) are added to running sums kept as M^2 float rows of nb
  (structure of arrays: M diagonals, then re / im of each pair a < b; the
  loop runs over 512-bin chunks so the FFT outputs stay in L1 and
  vectorises); the bin's raw power goes to a `pow` row
- The average: exponential, time constant tau = the spectrum trace's (one
  FFT per packet, weight averaging_alpha: 45 ms at the default 0.15) within
  0.03 - 2 s. No pass over the sums per block: a block's weight is
  exp(t / tau), t = its time since the last snapshot, and every snapshot
  (0.05 s of stream) scales the sums by exp(-0.05 / tau). The normalised
  snapshots make every block count alike however strong it is, so a burst
  leaves the average at the trace's pace (its dB average does the same): the
  first version averaged linear power with tau >= 0.1 s, and a 40 dB burst
  stayed in the average for ~1 s after it ended (with a burst every second
  the point kept the burst's bearing all the time; measured). For one
  source plus noise the normalised covariance has the same principal
  eigenvector (it is the spatial sign covariance); MUSIC agreement below.
  s1 / s2 (sum of the weights / of their squares) give the effective
  snapshots per bin, K_time = s1^2 / s2 (for the confidence byte)
- Each snapshot is published (`Grid`, recycled buffers) with the steering
  and each antenna's noise weight (the 20th percentile of its diagonal over
  the middle 80 % of the bins). A CPU budget (token bucket, `CPU_SHARE` 25 %
  of one core by the worker's own wall-clock timing) skips blocks past it;
  the share used is the state's `coverage`
- Per point (uWS thread, `build_frame`, in `send_spectrum_views` for each
  page that sent `BIN_DOA_VIEW:1`): `build_spectrum_view` hands out its cut
  (`SpectrumViewCut`: first / last bin, pool, n) and the type 9 frame covers
  exactly the type 7 frame's points; it is sent first so the page colours
  the frame it is about to draw. A point's covariance = the sum over its
  PEAK bins: the bearing bins in its display bins whose averaged power is
  within `PEAK_SHARE` (0.5) of the strongest - the trace shows the point's
  max, so a one-bin carrier in a wide point is not diluted by the point's
  noise (the snapshots are normalised: summing every bin would give the
  carrier one bin's say). Bearing bins coarser than the points (FFT > 8192
  zoomed in): the nearest, consecutive points share it. Pages with the same
  points share the estimates of a snapshot (a 4-entry cache, the seq patched)

## Estimator (one dominant source per point)

- Whitening: R_ab / sqrt(n_a n_b) with each antenna's noise weight (the
  real capture: one tuner 3.2 dB noisier than the others)
- Bearing (every point): the principal eigenvector by power iteration (6
  steps from the strongest column, single precision), un-whitened (D^1/2 v:
  for one source plus independent noise that is the array response itself),
  matched against the steering table: the azimuth with the largest
  |a^H v|^2 (every 5 deg at once - a vectorised loop over the coarse table -,
  then 1 deg around the best, a parabola through the three best). For one
  source this is MUSIC's peak with one signal (MUSIC maximises
  1 / (|a|^2 - |a^H v|^2)) - so the colour and a VFO's MUSIC bearing agree,
  whatever the model fit is (measured below, real and synthetic)
- The confidence byte (not used by the page): rho = lambda1 (M - 1) /
  (trace - lambda1) over the white-noise 99 % point for K snapshots, ~1 + 3
  sqrt(M / K) + 3 M / K (scratch sim/rho.py, M 3-8, K 2M - 4096: within -18
  / +40 % of the simulated quantile), x 1.15; 1 at 6 dB above. K = K_time x
  the effective independent bins (adjacent Hamming bins correlate: power
  0.39 / 0.018 at 1 / 2 bins)
- The array model is the MUSIC one, never a copy: the worker fetches it every
  0.5 s from the first VFO's MUSICProcessor (`azimuthSteering(freq)`, which
  builds the steering with the same code as MUSIC - `buildAzimuthSteeringLocked`,
  split out of `updateSteeringVectors` - at the tuner centre: topology,
  radius / spacing, custom positions, `uca_angle_sign()`, the ✈ array
  calibration) together with the half-plane mask and the array offset MUSIC
  applies; the bearing sent is MUSIC's azimuth with the offset added (type
  3's convention). One wavelength for the whole band: at the tuner centre
  (+-1.2 MHz is +-0.3 % at 433 MHz, +-1.3 % at 90 MHz, +-5 % at 24 MHz)
- Topologies: UCA and the Wideband topology (a UCA with the ring's radius)
  as MUSIC. ULA: a ULA can't tell front from back; with the ULA half-plane
  setting on BOTH the bearing spectrum assumes the front half (as FORWARD;
  type 9 flag 1) instead of picking one of the two mirror answers at random;
  FORWARD / BACKWARD as set. Custom: as MUSIC's XY steering; a 3D custom
  array (z spread) gets the bearing at the horizon (flag 2) - MUSIC searches
  azimuth x elevation there. The Wideband variant's high-side spectral
  inversion is corrected by heimdall at the source, so the stream (and MUSIC
  and this) need no orientation handling - an uninverted stream mirrors the
  frequency AND the bearing of both alike (tested below)

## Protocol

- `BIN_DOA:0|1` (ControlHandler, uWS thread; `CommandRejected` otherwise).
  `BIN_DOA_VIEW:0|1`: transport-level like `FFT_VIEW` (websocket_server.cpp,
  per connection, never reaches the control handler): this page takes type
  9; answered with the state. The page sends it on every connect (resent
  every 3 s until the state comes - nothing passes before AUTH; after 5 tries
  it says the receiver is older). An older page never sends it and never gets
  a type 9 frame or the state (it ignores the `BIN_DOA` sync_cmd); an older
  receiver echoes both commands as sync_cmd and sends nothing else - the page
  shows "This receiver has no bearing colours"
- Type 9, little-endian, 32-byte header: u32 9 | u8 display (0) | u8 flags (1
  ULA front assumed, 2 3D horizon, 4 every point's azimuth is its bearing) |
  u8 elements | u8 topology (0 UCA, 1 ULA, 2 custom) | u32 view seq | u32 n
  | f64 start Hz | f64 step Hz | u8 confidence[n] (informational) | u16
  azimuth[n] (x 360 / 65536 deg). permessage-deflate like the spectrum
  frames. `NATIVE_APP_API.md` §5.2b. (A frame without flag 4 - the first,
  never-merged version - had no bearing where the confidence was 0; the page
  treats those points as uncoloured)
- `{"bin_doa":{on, state off|idle|running|parked, reason, fft, elements,
  coverage}}` on every change (coverage at most once a second)
- Page: `binDoaOnFrame` keeps the latest frame; points are matched by
  frequency (a frame for another zoom still colours what it covers); a frame
  older than 400 ms isn't used (the trace then falls back to its normal
  stroke). State used by drawing (`binDoaOn`, `binDoaFrame`, `binDoaState`)
  is declared right after the globals block

## Limits

- One dominant source per point: two transmitters from different directions
  in the same point give the stronger one's bearing (or a mix). Bearing
  resolution in frequency: 2.4 MHz / 8192 = 293 Hz at most (FFT 8192 and up;
  the spectrum's own bins below that). Zoomed in further (FFT 32768 / 65536)
  several points share a bearing bin, and a strong carrier's Hamming leakage
  (+-2 bins = +-600 Hz) carries its bearing past what the finer trace shows
- Noise points show random bearings (by design, the user's call): at full
  span the noise floor is a multicoloured band under the signals
- At full span (~1 point per pixel) neighbouring points' segments share
  pixels: ~3 % of the line's vertex pixels show a neighbour's colour
  (measured below); zoomed in every point's own colour
- ULA: the front half unless the half-plane setting says otherwise (flag 1).
  3D custom arrays: horizon bearings (flag 2). The steering's wavelength is
  the tuner centre's for the whole band
- Not for the wideband scan, independent mode or generic RTL-SDRs (no
  coherent array). The KerberosSDR after a retune: parked until its manual
  recalibration (stale)
- A noise flag trailing the samples by more than `HOLD_PACKETS` (3) packets
  would still let the noise source colour the band for a frame or two
  (measured below with a lag of 10); heimdall's trail is 0-2

## Measurements, round 2 (2026-11-18 evening, the user's report from the real array)

- Real data: one read-only 15 s capture of the user's live heimdall stream
  (127.0.0.1:8091; tcp_data_server.cpp checked first: non-blocking sockets,
  a slow client only drops its own packets, up to 8 clients): 2198 packets,
  5 channels, 90.0 MHz, gain 49.6, phase state converged, no noise source,
  no gaps. Replayed packet by packet through the fake heimdall into the
  TEST build at the user's settings (UCA r 50 mm, FFT 16384, alpha 0.15).
  Signals: FM broadcast at 89.40 / 90.20 / 90.98 MHz (+12 to +24 dB on the
  trace), a narrow carrier at 91.04 MHz; one tuner's noise 3.2 dB above the
  others. Offline replica (scratch real_est.py) of the first version on it:
  of the points >= 3 dB above the floor 99.6 % passed the noise test, 92.6 %
  the power-now test, 13.2 % the steering fit (median fit 0.41) - the fit
  zeroed the colour. Through the real pipeline (real_probe.py, 1600-point
  view): the first version coloured 3.8 % / 10.6 % / 16.1 % of the points
  >= 3 / 6 / 10 dB and 0 % of the carrier's; now every point has its
  colour (ui_real.py: 1.000 of the points)
- Colour vs a VFO's MUSIC on the real signals (test_vfo.py, a 150 kHz VFO on
  each FM station, 3 kHz on the carrier; colour = the circular mean of the
  band's points >= 6 dB, unweighted, as the page shows them): 89.40 MHz
  -5.5 deg, 90.20 +1.9 / +2.7, 90.98 -0.3 / -0.5, carrier 91.04 -2.3 / -2.4
  (two runs). The array at 90 MHz with r 50 mm is electrically tiny (r /
  lambda 0.015): the per-point bearings of an FM station scatter (median
  deviation 6-25 deg across its band); a VFO averages its whole band
- Realistic synthetic regression (scene_hard.json): UCA r 294 mm at 433.9
  MHz with per-element errors the model doesn't know (gain 1.5 dB, phase 22
  deg, position 15 mm std: model match 0.59-0.65), unequal tuner noise (one
  +3 dB), a one-bin carrier (+29 dB), a weak one-bin carrier (+17 dB), NBFM
  (2.5 kHz deviation, noise message), AM, WBFM (60 kHz deviation), NBFM with
  a 200 deg echo at 0.6 / 7 samples, a weak NBFM: colour vs MUSIC worst
  0.25 deg over the five VFOs (MUSIC itself is 1-10 deg off the true
  bearings there - the model errors); the per-point bearings of every
  signal (>= 6 dB points) within 0.2-1.2 deg median of their mean, the weak
  carrier 9.7
- Burst (burst.py, the 20 ms burst once a second, all points coloured): the
  burst's bearing at its point in the frame of the burst and the next one
  (50-100 ms, as the trace shows it), then the noise's random bearings
  again - no lingering (15 of 149 frames, 2 random)
- UCA (scene_uca.json) after the change: colour vs MUSIC worst 0.08 deg;
  the line pixel at each signal's vertex = its bearing within 0.1 deg; a
  noise point drawn in its own (random) bearing (ui_test.py 19 / 19, the
  noise check inverted)
- The line (ui_real.py on the replay, 7 checks): at full span 733-737 of 757
  vertices (96.8-97.4 %) within 10 deg of the point's hue, no pixel of the
  normal trace colour left; zoomed 16x (146 Hz points) 100 %; waterfall row
  271 / 271 sampled pixels in the bearing colours; hover readout; no page
  errors. Screenshots: shots2/real_full.png, real_zoom.png
- Noise-source leak (flag trailing by 3 packets, 6 switches): 0 frames
- CPU (powersave governor again - CPU time at an unknown clock; the same
  synthetic UCA load as round 1): the worker 8-13 % of a core and the whole
  process 22-36 % on, 10 % off, in runs minutes apart - the machine's clock
  state moved more than the feature's cost (round 1's off runs were 23 %).
  At a steady clock (bench/bench.cpp): the normalisation adds 9 % to a
  block (158.8 vs 145.8 us at nb 8192, M 5 = 4.65 % of a core at 2.4 MS/s),
  the removed `recent` rows took about that back. The uWS thread: every
  point now gets the full estimate (power iteration + bearing search):
  build_frame 1.3-2.9 ms for a 1639-point view (scratch prof build), ~1 us a
  point; on a Pi 4 maybe 6-12 ms per frame per distinct view (20 / s:
  12-25 % of the loop thread) - not measured on a Pi. Levers: a 0.1 s
  snapshot (`TICK_S`, halves both) or `MAX_FFT` 4096

## Measurements, round 1 (2026-11-18, job scratch 8d3d8a3e bin-doa/; the confidence-gated version)

Setup: a TEST build (ports moved), a Python fake heimdall (scratch
fake_heimdall.py) streaming a prebuilt ~1 s loop of whole 16384-sample
packets in real time (146.5 / s, checked by its own counter - the first
version held the stream at 20 packets / s with a 50 ms accept timeout per
packet; every figure below is from the fixed one): 5 antennas, receiver
noise (std 6 counts per I / Q) plus signals at given offsets and MUSIC
azimuths on a UCA wired clockwise (r 294 mm at 433.9 MHz, the client's
RADIUS 294): CW +100 kHz 30 deg (amp 12), FM -250 kHz 135 deg (amp 10),
50 kHz noise band +400 kHz 250 deg (amp 8), CW -500 kHz 300 deg (amp 5),
CW -700 kHz 75 deg (amp 1.5, ~+5 dB on the trace at 1600 points), a 20 ms
CW burst +600 kHz 200 deg once a second (amp 15). Clients: scratch wsc.py
(TLS WebSocket), cdp.py (headless Chromium, real mouse / wheel events).

- Bearings (probe.py): median / max error 0.29 / 3.7 deg (CW 30: its
  skirts), 0.02 / 0.13 (FM), 0.08 / 0.67 (band), 0.01 / 0.08 (CW 300), 0.05
  / 0.33 (weak CW 75), burst 0.06 / 1.6
- VFO vs colour: worst 0.08 deg (UCA); with ARRAY_OFFSET:40 both +40
- Topologies: ULA (spacing 300 mm, signals at 30 / 300 / 70 / 345 / 135):
  FORWARD - worst 0.25 deg from MUSIC; BOTH - the colours give the front
  answer (135 shows as its mirror 45, as MUSIC FORWARD would), MUSIC on BOTH
  picks either mirror (240 for 300, 195 for 345). Custom (5 irregular
  elements up to 0.3 m) 2D and 3D (z 0 / 150 mm): worst 0.28 deg. Wideband
  variant (`-w`, TOPOLOGY:WIDEBAND, outer ring 127.5 mm): worst 0.14 deg.
  The same stream conjugated (an inversion heimdall did not undo): every
  signal at the mirrored frequency with its bearing + 180 deg - MUSIC and
  the colours alike (worst 0.36 deg apart): nothing here needs to know the
  mixer side, as long as heimdall corrects it (it does)
- Park: noise source on - parked within one packet of the flag; off -
  running again ~0.5 s later. Calibration stale (0x200): parked / resumed
  alike. Independent mode: "only in coherent mode". Leak test (at 100 MHz
  where the noise source's all-ones vector fits the steering well): flag
  trailing by 2 packets - 0 / 6 switches with or without the hold; by 3 -
  0 / 6; by 4 - 1 frame in 8 switches without the hold, 0 with it; by 10 -
  4 in 4 switches (beyond the hold)
- Compatibility (compat_test.py): the page from before the change against
  this receiver with the mode on - no page errors, a client without
  BIN_DOA_VIEW gets no type 9; this page against the receiver from before -
  "This receiver has no bearing colours" after the 5 tries, no page errors
- CPU: mode off - the build from before 23.2 / 23.9 / 23.5 % of a core,
  this one 23.9 / 22.8 / 23.3 % (unchanged; off, the only new work is two
  atomic loads per packet). Mode on, FFT 16384: 43.5-44.7 % (the worker
  19-20 %); FFT 65536: off 36.5 %, on 58.1 %. Kernels at a steady clock: 142
  us per block = 17.3 ns per sample-time = 4.2 % of a core at ~4.4 GHz
  (M 8: 33 ns); pffft 16384 35 us there (Pi 5: 89 us, fft-pffft.md); the
  live thread runs at a low clock (~4x the bench's time)
- Pi estimate (not measured on a Pi): ~15 % of a Pi 5 core (11-19 %), ~30 %
  of a Pi 4's at 1.8 GHz (22-40 %) for the worker. Hence `CPU_SHARE` 25 %:
  full coverage expected on a Pi 5, possibly 60-100 % on a Pi 4 (`coverage`
  shows it). Levers: `MAX_FFT` 4096 (12 % cheaper, the sums 0.4 MB - in the
  Pi 4's 1 MB L2 - at 586 Hz resolution) or a lower `CPU_SHARE`
