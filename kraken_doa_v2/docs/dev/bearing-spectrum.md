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
- Display > **Bearing colours** (`BIN_DOA:0|1`, the receiver's setting:
  persisted - schema `bin_doa`, off by default -, echoed, replayed to new
  pages; every page follows it). Colours the main spectrum (a column from
  0 dB up to each point's level, and its piece of the trace) and the main
  waterfall (each new row; a row keeps the colours it was drawn with).
  Independent-mode panes are not coloured (no coherent array there)
- Colour: hue = the point's compass bearing (360 - MUSIC azimuth: N red, E
  yellow-green, S cyan, W violet; `binDoaRgb`), alpha / blend = the
  confidence - a point without a coherent signal keeps the normal colour.
  Legend: a colour ring oriented like the DoA plots (`PolarPlot.azPos`:
  azimuth 0 = ANT0 at the top, 90 left) with N/E/S/W or 0/90/180/270 by the
  DOA Angles convention - top right of the spectrum (with the bearing under
  the cursor, or "paused" and why), in the sidebar next to the tick, and as a
  thin ring around every VFO's DoA plot while the mode is on, so a VFO's lobe
  and the spectrum's colour can be compared directly
- Parked (no frames, the reason in `{"bin_doa"}`, shown under the tick and on
  the plot): outside coherent mode (wideband scan, independent, generic
  RTL-SDRs), while the noise source is on or just was (`doa_is_calibrating`),
  after a retune (`doa_retune_hold_active`, the DoA's hold), while heimdall
  reports the calibration stale (0x200; the KerberosSDR after a retune),
  when the array model gives no steering (custom positions not set, a UCA of
  < 3 elements). Every park drops the averages. heimdall's noise flag trails
  the samples by up to 2 packets: a packet is only used once `HOLD_PACKETS`
  (3) more have come unparked, so the noise source's first samples (the same
  on every antenna = a confident false bearing everywhere) never reach them
  (measured below)

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
  bins like the spectrum; per bin the M x M cross-spectra x_a conj(x_b)
  added to running sums kept as M^2 float rows of nb (structure of arrays:
  M diagonals, then re / im of each pair a < b; the loop runs over 512-bin
  chunks so the FFT outputs stay in L1 and vectorises). Every 0.05 s of
  stream: the sums published as a snapshot (`Grid`, recycled buffers, with
  the steering and each antenna's noise level - the 20th percentile of its
  diagonal over the middle 80 % of the bins), then aged by exp(-0.05 / tau),
  tau = the spectrum trace's time constant (packet / averaging alpha) within
  0.1 - 2 s. The weights' sum s1 and sum of squares s2 give the effective
  snapshot count per bin, K_time = s1^2 / s2. Per antenna, each bin's power
  since the last snapshot is kept too (`recent`, for the presence gate
  below). A CPU budget (token bucket, `CPU_SHARE` 25 % of one core by the
  worker's own wall-clock timing) skips blocks past it; the share used is
  the state's `coverage`
- Per point (uWS thread, `build_frame`, in `send_spectrum_views` for each
  page that sent `BIN_DOA_VIEW:1`): `build_spectrum_view` hands out its cut
  (`SpectrumViewCut`: first / last bin, pool, n) and the type 9 frame covers
  exactly the type 7 frame's points; it is sent first so the page colours
  the frame it is about to draw. A point's covariance = the sum of the
  bearing bins whose centres lie in its display bins (bearing bins coarser
  than the points - FFT > 8192 zoomed in - : the nearest one, consecutive
  points share it). Summing is the right merge (a covariance of more
  snapshots); averaging bearings would not be. Pages with the same points
  share the estimates of a snapshot (a 4-entry cache, the seq patched)

## Estimator (one dominant source per point)

- Whitening: R_ab / sqrt(n_a n_b) with each antenna's noise level n (gain
  differences between the tuners - heimdall corrects phase only - would
  otherwise read as a coherent part). A common per-bin scale (the tuner's
  roll-off) does not matter: the statistics below are scale-free
- Detection: rho = lambda1 (M - 1) / (trace - lambda1) - the principal
  eigenvalue over the mean of the others; for one source of per-antenna SNR
  s, rho = 1 + M s. White noise with K snapshots reaches its 99 % point at
  ~1 + 3 sqrt(M / K) + 3 M / K (scratch sim/rho.py, M 3-8, K 2M - 4096:
  within -18 / +40 % of the simulated quantile, closest for K >= 4M). K =
  K_time x the effective independent bins of the point (adjacent Hamming
  bins are correlated: power 0.39 / 0.018 at 1 / 2 bins, as
  fft_processor.cpp's noise_quantile). The threshold is that x 1.15;
  confidence rises from 0 there to 1 at 6 dB above. K < 2M: no estimate.
  Before the eigenvector: lambda1 <= the largest absolute row sum
  (Gershgorin) - most noise-only points are rejected with that alone
- Bearing: the principal eigenvector by power iteration (6 steps from the
  strongest column) matched against the steering table: the azimuth with
  the largest |a^H v|^2 (5 deg grid, then 1 deg around the best, a parabola
  through the three best). For one source this is exactly MUSIC's peak with
  one signal (MUSIC maximises 1 / (|a|^2 - |a^H v|^2)) - so the colour and a
  VFO's MUSIC bearing agree (measured below). The fit |a^H v|^2 (unit
  vectors) is the second confidence factor: 0 at 0.5, 1 at 0.8 (a plane wave
  fits ~1; two directions mixed in one point, or a non-planar field, less)
- Presence now: the average is linear power, the trace averages dB - a
  40 dB burst stayed detectable in the average for ~1 s after it ended (with
  a burst every second the point was coloured all the time while the trace
  showed noise; measured). So the point's power since the last snapshot
  (whitened, all antennas, per block) must stand above its noise - the
  average's eigenvalues past lambda1 - by 2 (no colour) to 5 (full) standard
  deviations of that power (1 / sqrt(blocks x bins x M)). A steady signal
  passes as the detection does; a burst keeps its colour while it is there
  plus ~1 snapshot (measured below). A first version compared the recent
  power with the average's (ratio 0.35 - 0.65): the residual of a strong
  burst keeps that ratio up for ~0.6 s - it did not help
- The array model is the MUSIC one, never a copy: the worker fetches it every
  0.5 s from the first VFO's MUSICProcessor (`azimuthSteering(freq)`, which
  builds the steering with the same code as MUSIC - `buildAzimuthSteeringLocked`,
  split out of `updateSteeringVectors` - at the tuner centre: topology,
  radius / spacing, custom positions, `uca_angle_sign()`, the ✈ array
  calibration) together with the half-plane mask and the array offset MUSIC
  applies; the bearing sent is MUSIC's azimuth with the offset added (type
  3's convention). One wavelength for the whole band: at the tuner centre
  (+-1.2 MHz is +-0.3 % at 433 MHz, +-5 % at 24 MHz - for a UCA a scale error
  of the phases moves the peak's height, not its azimuth)
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
  ULA front assumed, 2 3D horizon) | u8 elements | u8 topology (0 UCA, 1
  ULA, 2 custom) | u32 view seq | u32 n | f64 start Hz | f64 step Hz | u8
  confidence[n] | u16 azimuth[n] (x 360 / 65536 deg, 0 where confidence 0 -
  deflates well). permessage-deflate like the spectrum frames.
  `NATIVE_APP_API.md` §5.2b
- `{"bin_doa":{on, state off|idle|running|parked, reason, fft, elements,
  coverage}}` on every change (coverage at most once a second)
- Page: `binDoaOnFrame` keeps the latest frame; points are matched by
  frequency (a frame for another zoom still colours what it covers); a frame
  older than 400 ms isn't used. State used by drawing (`binDoaOn`,
  `binDoaFrame`, `binDoaState`) is declared right after the globals block

## Limits

- One dominant source per point: two transmitters from different directions
  in the same point give the stronger one's bearing (or, comparable, a low
  fit - faded). Bearing resolution in frequency: 2.4 MHz / 8192 = 293 Hz at
  most (FFT 8192 and up; the spectrum's own bins below that). Zoomed in
  further (FFT 32768 / 65536) several points share a bearing bin, and a
  strong carrier's Hamming leakage (+-2 bins = +-600 Hz) is coloured with
  its bearing past what the finer trace shows (measured: at FFT 65536,
  36.6 Hz points, 56 points from -1.7 to +1.5 kHz around a +47 dB carrier,
  bearings 26.5 - 30.3 deg for its 30)
- ULA: the front half unless the half-plane setting says otherwise (flag 1).
  3D custom arrays: horizon bearings (flag 2). The steering's wavelength is
  the tuner centre's for the whole band
- Not for the wideband scan, independent mode or generic RTL-SDRs (no
  coherent array). The KerberosSDR after a retune: parked until its manual
  recalibration (stale)
- A noise flag trailing the samples by more than `HOLD_PACKETS` (3) packets
  would still let the noise source colour the band for a frame or two
  (measured below with a lag of 10); heimdall's trail is 0-2

## Measurements (2026-11-18, job scratch 8d3d8a3e bin-doa/)

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

- Bearings (probe.py, 1600 points over the band, 4-6 s of frames; points
  with confidence > 0.2 within each signal): median / max error 0.29 / 3.7
  deg (CW 30: its skirts), 0.02 / 0.13 (FM), 0.08 / 0.67 (band), 0.01 /
  0.08 (CW 300), 0.05 / 0.33 (weak CW 75), burst 0.06 / 1.6; noise-only
  points (1530 per frame, away from signals and DC): 0 with confidence > 0
  in every run; DC: 0. Tolerance stated: +-4 deg on the drawn colour
- VFO vs colour (test_vfo.py: a 12.5 kHz VFO on each signal, 60 kHz on the
  band, MUSIC's type 3 peak with the page's parabolic interpolation, median
  over 4 s; the colour = the confidence-weighted circular mean of the
  points in the VFO's band): worst |colour - MUSIC| 0.08 deg (UCA), 0.05
  after the last changes; with ARRAY_OFFSET:40 both +40, worst 0.08
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
- Burst (burst.py): coloured in 13 of 150 frames over 6 s - each burst 1-2
  frames (50-100 ms) at 200 deg, as long as the trace shows it above
  10 dB. Before the presence gate: coloured in 230 of 259 frames (always)
- Park: noise source on - parked within one packet of the flag ("calibrating
  (noise source on)"), no frames; off - running again ~0.5 s later
  (doa_is_calibrating's quiet time + the hold). Calibration stale (0x200):
  parked / resumed alike. Independent mode: "only in coherent mode". Leak
  test (test_leak.py, at 100 MHz where the noise source's all-ones vector
  fits the steering well, frames with > 200 confident points = the band
  painted by the noise source): flag trailing by 2 packets - 0 / 6
  switches with or without the hold; by 3 - 0 / 6; by 4 - 1 frame in 8
  switches without the hold, 0 with it; by 10 - 4 in 4 switches (beyond
  the hold)
- UI (ui_test.py, 19 / 19): the saved setting on, the page asks and gets
  the state; the colour drawn at each signal's peak (the most saturated
  pixel of the stroke, hue -> bearing) within 0.1 deg of the truth, a
  noise-only point not coloured; waterfall row colours within 0.2 deg; the
  hover readout (compass convention) = the bearing; wheel zoom to 14.6x:
  146 Hz points, still the bearing; a real click off -> receiver off, every
  page synced, no colours; on again; independent mode - paused with the
  reason under the tick; back; noise source - paused with the reason, then
  running; no page errors. Screenshots: shots/spectrum_on.png,
  spectrum_zoom.png
- Compatibility (compat_test.py): the page from before the change against
  this receiver with the mode on - no page errors, a client without
  BIN_DOA_VIEW gets no type 9 (types 7 and 3 only); this page against the
  receiver from before - "This receiver has no bearing colours" after the 5
  tries, ticking it does nothing harmful, no page errors
- CPU (the dev PC: i7-11800H, 16 threads shared with other jobs, governor
  powersave - cores run 0.8 - 4.5 GHz, so these are CPU time at whatever
  clock a thread got, not cycles; perf is not allowed there,
  perf_event_paranoid 4). kraken_doa user + system time over 30 s, one
  client with a 1600-point view, DoA on (one VFO): mode off - the build
  from before 23.2 / 23.9 / 23.5 %, this one 23.9 / 22.8 / 23.3 % of a core
  (unchanged; off, the only new work is two atomic loads per packet). Mode
  on, FFT 16384: 43.7 / 44.7 / 43.5 % (+20 %; the `bin_doa` thread 19-20 %,
  the uWS thread +~1.2 % for the page's estimates); FFT 65536: off 36.5 %,
  on 58.1 % (the thread 20.4 % - the bearing FFT stays at 8192). Inside the
  thread (scratch prof build): a block (5 x 8192) 535 us - 411 us of it the
  windows + FFTs -, the 20 snapshots a second 1.7 %, copying each packet
  into the stash 2.3 % (removed since). At a steady clock the same kernels
  (bench/bench.cpp, the block loop copied) take 142 us per block = 17.3 ns
  per sample-time = 4.2 % of a core at ~4.4 GHz (~76 cycles per
  sample-time; M 8: 33 ns); pffft 16384 there 35 us (Pi 5: 89 us,
  fft-pffft.md). The live thread wakes every packet for ~1.3 ms and runs at
  a low clock - hence ~4x the bench's time
- Pi estimate (not measured on a Pi): pffft needs ~1.4x the cycles on the Pi
  5 (89 us at 2.4 GHz vs 35 us at ~4.4 GHz) and the cross-spectra loop
  (AVX2 here, 128-bit NEON there) ~2-3x: ~150 cycles per sample-time =
  ~15 % of a Pi 5 core (11-19 %), ~30 % of a Pi 4's at 1.8 GHz (22-40 %),
  plus ~1-2 % per page on the uWS thread. Hence `CPU_SHARE` 25 %: full
  coverage expected on a Pi 5, possibly 60-100 % on a Pi 4 (fewer
  snapshots, a short burst can be missed; `coverage` shows it). Levers if
  that is too much: `MAX_FFT` 4096 (bench: 12 % cheaper, the sums 0.4 MB -
  in the Pi 4's 1 MB L2 - at 586 Hz resolution) or a lower `CPU_SHARE`
