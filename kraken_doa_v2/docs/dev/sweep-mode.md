# Sweep mode

The fourth operating mode (Mode selector: Coherent / Wideband / Independent /
**Sweep**), the user's request of 2026-11-20: the chosen tuners step through a
frequency range and the receiver stitches ONE wide spectrum and waterfall out
of the steps. It is not live and nothing is demodulated: no VFO audio, no
decoders, no DoA. The top bar's tuning digits are replaced by the range
(start, stop) and a selector for the tuners that take part - more tuners = a
faster sweep, and each of them needs an antenna.

Not the same thing as wideband scan mode (tuners side by side around one
centre, live, VFOs work) or the `--wideband` downconverter variant (where,
like the other non-coherent modes, sweep mode is refused: every tuner is
parked at the IF).

Three parts, one section each below: heimdall hops the tuners
(`heimdall_v2/docs/dev/operating-modes.md` has its side), kraken_doa turns
the hops into the sweep spectrum, the page shows it.

## The contract (wire formats; fixed before the three parts were written)

### The grid

- Sample rate fs = 2.4 MS/s. FFT size N = a power of two, 128 ... 16384;
  bin = fs / N.
- Every hop contributes its centre `U = N * 51 / 64` bins (79.7 % of the
  tuner's band; the edges roll off), so the hop spacing is the same for
  every N: `SWEEP_STEP_HZ = 1 912 500` Hz, an integer.
- Range `[start, stop]` in integer Hz, `24 000 000 <= start < stop <=
  1 765 000 000` (the last hop's centre then stays below the tuners'
  1766 MHz limit), `stop - start >= 100 000`.
- Hops `H = max(1, ceil((stop - start) / SWEEP_STEP_HZ))`. Hop h's tuner
  centre is `start + SWEEP_STEP_HZ / 2 + h * SWEEP_STEP_HZ` (integer Hz).
- Sweep bin g is centred at `start + g * bin`. Hop h fills bins
  `[h * U, (h + 1) * U)`: FFT bin k (0 = -fs/2 after the shift) goes to
  `g = h * U + k - (N - U) / 2`. Bins reported: `min(H * U, floor((stop -
  start) / bin) + 1)`.

### heimdall, 8092

- `{"command":"set_operating_mode","mode":"sweep"}` - `OperatingMode::SWEEP`
  (3). Calibration parks as in the other non-coherent modes. Refused on the
  downconverter variant and while the discrete scanner runs.
- `{"command":"set_sweep", ...}` - the plan; accepted in every mode, used in
  sweep mode, persisted. Fields (each optional; the three plan fields only
  together):
  - `"first_center_hz":F0, "step_hz":S, "steps":H` - hop h's centre is
    `F0 + h * S`. F0 and the last centre inside the tuner range, S 100 kHz
    ... 2.4 MHz, H 1 ... 20000.
  - `"tuners":[0,1,4]` - the tuners that take part: unique indices below
    the element count, at least one.
  - `"dwell_ms":D` (5 ... 5000, default 20) - how long the settled stream
    runs on each hop.
  - `"settle_ms":T` (0 ... 1000; default `SWEEP_SETTLE_MS` in config.h) -
    wait between the retune and the flush that precedes the dwell.
  - `"run":true|false` - false pauses the hopping (tuners stay put).
  Reply `{"status":"success","sweep":{...}}` (the status object below) or
  `{"status":"error","message":"..."}`; nothing changes on an error. A plan
  or tuner change restarts the sweep at round 0.
- Who measures what: K' = min(selected tuners, H); the k-th selected tuner
  (k = 0 ... K'-1, in ascending index order) covers the contiguous hops
  `[floor(k * H / K'), floor((k + 1) * H / K'))`. Round r (0 ... R-1,
  `R = ceil(H / K')`): every such tuner retunes to its hop number
  `base + r` if it has one, else it stays where it is. Tuners that take no
  part are never retuned. Contiguous blocks, not interleaved: the tuners'
  floors differ by a few dB, which then shows as K' - 1 steps instead of a
  comb.
- One hop (every tuner of the round together): `retuning_in_progress` = 1 ->
  flush -> retune (parallel through `for_each_tuner`) -> settle -> flush ->
  `retuning_in_progress` = 0 -> dwell.
- In sweep mode `set_frequency` and `set_independent_tuner` are refused,
  `set_gain` sets every tuner (the common gain), `start_scanner` and the
  element-count change are refused.
- Leaving: to coherent every tuner returns to `current_frequency` (which
  the sweep never changed) and the array recalibrates (KerberosSDR: its
  manual-calibration rules); to independent the tuners stay where the sweep
  left them.
- Status JSON (2 Hz broadcast and `get_status`): `"operating_mode":"sweep"`
  and always `"sweep":{"first_center_hz","step_hz","steps","tuners":[..],
  "dwell_ms","settle_ms","run","round","rounds","sweeps","round_ms"}`
  (`sweeps` = completed since the plan changed, `round_ms` = the measured
  time of a round).
- Additions by heimdall's implementation (2026-11-20; nothing above
  changed): entering sweep mode sets `run` true and starts at round 0;
  `dwell_ms` / `settle_ms` / `run` changes take effect at once without a
  restart; a tuner already on its round's hop is not retuned (K' = H: the
  stream runs on without a gap, the counter still + 1 per round, the flag
  stays 0); a dwell lasts at least until one set has gone out with the
  flag 0 (at most 250 ms longer), so no hop goes without data; `set_gain`
  sets the flag until the next settle + flush; the status object also has
  `"retuning"` (the 8091 flag), `"failed_retunes"` and
  `"l1_unquiet_flushes"` (diagnostics). Default `settle_ms` 5.
  A tuner whose retune failed (twice) has the 8091 per-channel frequency
  **0** ("no valid frequency") from that hop until its next successful
  retune - it is not on its old frequency either (the tuner drivers fail
  after writing the new PLL values); 0 is on no grid, so a receiver drops
  that channel. The status object also has `"failed_tuners":[..]` (the
  tuners whose last retune failed). Leaving sweep mode for coherent runs the
  full recalibration (every channel's lag, then the phase): each full-path
  retune shifts that tuner's samples by 2 against the others (measured).

### heimdall, 8091 (sweep mode)

- Phase-state flags `0x2000` (sweep) together with `0x800` (independent): a
  client that does not know sweep mode treats the stream as independent
  tuners (no DoA on it). A client that knows it tests `0x2000` first.
- Per-channel frequency = where that tuner is (as in independent mode);
  per-channel gain = the common gain.
- `frequency_change_counter` + 1 per round, `current_group_index` = the
  round r the tuners are on, `retuning_in_progress` as above.
- The guarantee the client builds on: a packet with `retuning_in_progress`
  = 0 holds only samples taken at the header's per-channel frequencies,
  after the settle. (The header is a float32: ~128 Hz coarse at 1.7 GHz -
  the client finds the hop as `round((f - F0) / S)`.)

### kraken_doa, WebSocket commands (text)

- `OPERATING_MODE:sweep` - mode index 3. Refused on the downconverter
  variant and during a playback, like the other non-coherent modes; allowed
  with generic RTL-SDRs, also with one.
- `SWEEP_RANGE:<start MHz>:<stop MHz>` - clamped into the grid's limits.
- `SWEEP_TUNERS:<i,j,...>|all` - the tuners that take part; indices that
  are not there are dropped, none left = all.
- `SWEEP_RES:<N>|auto` - the FFT size (128 ... 16384); `auto` (0) picks
  one from the span.
- `SWEEP_DWELL:<ms>` - 5 ... 1000, default 20.
- `SWEEP_RUN:<1|0>` - run / pause. Not persisted: entering the mode runs.
- All but `SWEEP_RUN` persisted. Each is answered by the `sweep` message
  to every page, never by an echo.
- `SWEEP_VIEW:<seq>:<lo Hz>:<hi Hz>:<points>` - transport level, per
  connection, like `FFT_VIEW`: the window this page shows and its plot
  width in device pixels (at most `SPECTRUM_VIEW_MAX_POINTS`). `lo >= hi` =
  the whole range. Junk is ignored.

### kraken_doa, messages to the page

- JSON, on connect and on every change: `{"sweep":{"active":bool,
  "start_hz","stop_hz","min_hz","max_hz","tuners":[..],"available":N,
  "res":0|N,"fft_size":N,"bin_hz","bins","hops","rounds","dwell_ms",
  "running":bool,"grid":id}}`. `active` = the receiver is in sweep mode;
  `res` = what was asked for (0 = auto), `fft_size` = what is used;
  `available` = the tuner count. A page takes this message as "the
  receiver knows sweep mode" (an older receiver never sends it).
  Optional, added by the page worker 2026-11-20: `"gain_db"` = the common
  gain (heimdall's `set_gain` value, -1 = AGC). Sweep mode has no type 7
  frame whose header carries the gain, so a page shows another page's gain
  change only through this field; a receiver without it leaves each page's
  gain control as that page set it (nothing else depends on it).
- `{"operating_mode":{"mode":"sweep",...}}` as for the other modes.
- Binary type 10, the sweep spectrum for one page's view (little-endian,
  64-byte header), sent to every socket that sent a `SWEEP_VIEW`, at most
  20 a second while hops arrive, and once when a sweep completes:

  | Offset | Type | |
  |---|---|---|
  | 0 | u32 | 10 |
  | 4 | u8 | flags: 1 = the requested window (else the whole range), 2 = this frame completes a sweep (one waterfall line), 4 = paused, 8 = no data yet |
  | 5 | u8 | tuners taking part |
  | 6 | u16 | 0 |
  | 8 | u32 | the view's seq |
  | 12 | u32 | sweeps completed on this grid |
  | 16 | f64 | range start Hz |
  | 24 | f64 | range stop Hz |
  | 32 | f64 | centre of point 0, Hz |
  | 40 | f64 | point spacing, Hz |
  | 48 | f32 | progress of the sweep under way, 0 ... 1 |
  | 52 | f32 | duration of the last complete sweep, s (0 = none yet) |
  | 56 | u32 | grid id (changes with the range or the FFT size: the page drops its waterfall) |
  | 60 | u32 | n |
  | 64 | u8[n] | level: 0 ... 255 = 0 ... 120 dB over the sweep's noise reference, max-pooled over the point's bins |

  Points: the bins whose centres lie in the window, max-pooled as type 7
  does (*Spectrum views*). A bin that was never measured is 0.
- Levels: ONE reference for the whole sweep (never one per hop: a signal
  that fills a hop must keep its height), the tuners' common in-band shape
  removed (`nfloor::shape_db`, *Noise floor*).

## heimdall

(heimdall's side: `heimdall_v2/docs/dev/operating-modes.md`, "Sweep mode".)

## The receiver (kraken_doa)

Written 2026-11-20 with the sweep engine (`include/sweep.hpp`,
`src/sweep.cpp`, namespace `sweep`).

**Mode plumbing.** `sweep_mode_enabled` (globals.hpp) is mode index 3;
`multi_tuner_mode()` includes it, so everything coherent-only stands down as
in the other two non-coherent modes (DoA parked in `doa_enabled_before_wideband`
and restored in coherent, beamforming, MUSIC, the IQ recorder, the bearing
spectrum, the web mapper's bearings). On top of that the data receiver hands
every packet to `sweep::on_packet` and `continue`s before the uint8 -> complex
conversion: nothing reaches `raw_data_buffer`, so no VFO decimation, audio,
decoder input, squelch or regular FFT runs; the 50 ms tick sends type 10
frames only (no type 0 / 6 / 7 / 4 / 9) and the audio timer sends nothing. The
VFOs and their settings stay as they are, idle. Leaving the mode bumps
`fft_reset_generation` (the averaged spectra held the bands from before).
Also silent in sweep mode: the per-retune lines (`[RETUNING]`, "Server
frequency change detected", "[DoA] Reference frequency changed"), the FM
thread's "No data for 2s", and `ChannelManager::update_channel_info`'s MUSIC
steering-vector update on every channel-0 frequency change (it recomputed
the steering vectors and printed two lines per hop; they follow on the way
back to coherent).

- Header: `0x2000` is tested before `0x800` (mode 3); `note_server_mode`
  adopts a sweeping heimdall after the usual 3 s, then sends its own plan -
  except before the startup replay (`g_settings_replayed`): the settings are
  still the defaults then, and the replay's `OPERATING_MODE:sweep` sends the
  saved plan (found by the restart test: the default plan reached heimdall
  for a moment).
- `OPERATING_MODE:sweep`: refused on the downconverter variant and during a
  playback (`playback::refuse`, as for the other non-coherent modes);
  allowed with generic RTL-SDRs, also with one. Stops the discrete,
  continuous and saved-frequency scanners and a running IQ recording; sets
  running; sends `set_sweep` (the plan) and then `set_operating_mode`.
  Sweep -> independent re-sends the saved per-tuner tuning (tuners without
  one start at the coherent frequency, not at a hop).
- Refused in sweep mode with a reason (`CommandRejected`): `FREQ:`,
  `TUNER_FREQ:` / `TUNER_GAIN:`, `WIDEBAND_FREQ:`, `WIDEBAND_BASE_FREQ:`,
  `SCANNER_START`, `CONTINUOUS_SCANNER_START` (needs coherent),
  `SFSCAN_START` (sfscan::start), an audio recording (recorder), a decoder's
  retune request. `GAIN:` works (heimdall's common gain).
- An older heimdall: it answers `set_sweep` "Unknown command" and the mode
  "mode must be coherent, wideband or independent", and its header never
  shows `0x2000`. `ingest_sweep_reply` (the control socket's drain) takes an
  error reply within 4 s of the request as the reason; `note_sweep_header`
  (every packet) falls back at once on such a reply, else 4 s after the
  request without the flag: back to heimdall's mode (its header's), saved as
  the mode, and `{"operating_mode":{...,"error":"heimdall refused sweep mode:
  ..."}}` to the pages. `note_server_mode` leaves a pending sweep request to
  it. Measured: back after 0.46-0.47 s (the reply), in coherent and in
  independent mode.
- `system_status.sweep`, the dashboard's mode + a SWEEP line
  (`sweep::summary()`), the AI assistant: `get_screen` names the mode and the
  sweep, `get_spectrum` returns the sweep's spectrum (`sweep::pooled_db`),
  `tune` / `add_vfo` / `set_vfo` / `capture_iq` refuse ("The receiver is in
  sweep mode (...) - tuning isn't possible there; ..."), `set_gain` sets the
  common gain.

**Commands** (control_handler.cpp; `is_query_command`: answered by the
`sweep` message to every page, never echoed; the handler saves the applied
form itself - not during the replay or a saved-frequency scan's dispatch).
Junk (no number, `nan`, `inf`, `1.5` as a tuner, a negative FFT size) is
refused; out-of-range numbers are clamped:
- `SWEEP_RANGE:a:b` - swapped when reversed, whole Hz, start in [24, 1764.9]
  MHz, stop in [start + 0.1, 1765] MHz.
- `SWEEP_TUNERS:` - "all", or a list: indices outside 0..7 dropped from the
  request; the request is kept and resolved against the tuner count when
  the plan is made (indices not there dropped, none left = all), so a saved
  `1,3` survives a start with fewer tuners. The plan follows a tuner-count
  change (`clamp_vfo_tuners` -> `sweep::on_elements`).
- `SWEEP_RES:` - `auto` / 0, else the nearest power of two in 128 .. 16384.
- `SWEEP_DWELL:` - 5 .. 1000 ms. `SWEEP_RUN:` - not saved; `set_sweep
  {"run":..}` alone.
- Settings schema (replay order): `SWEEP_RANGE` `SWEEP_TUNERS` `SWEEP_RES`
  `SWEEP_DWELL` right before `OPERATING_MODE:` (defaults 87.5-108 MHz, all,
  auto, 20 ms). During the replay the setters send nothing; the replayed
  `OPERATING_MODE:sweep` sends the plan. `set_sweep` goes to heimdall only
  when what it holds changed (`take_plan_changed`: centres, tuners, dwell -
  an FFT size change alone sends nothing), and again after a reconnect of the
  data port.
- `gain_db` in the `sweep` message (the contract's optional field, asked for
  by the page worker): the common gain from heimdall's 8091 header in sweep
  mode, or a `GAIN:` command; the message goes again when it changes. The
  header trails a command by a few packets: within 1.5 s of a `GAIN:` a
  different header value is not taken (the test saw 25 -> 49.6 -> 25 before).
- `SWEEP_VIEW` (websocket_server.cpp `handle_sweep_view`, per socket like
  `FFT_VIEW`, junk ignored - also a width over 65536): marks the socket dirty,
  so it gets a frame on the next 50 ms tick whatever the sweep does - also
  while paused (a paused page that zooms gets its new window's detail). The
  first one sets the socket's completion count, so its first frame is no
  waterfall line.

**The engine.**
- Plan (`make_plan`): the contract's grid. `H = ceil(span / 1 912 500)`,
  `U = N * 51 / 64`, bins = `min(H U, floor(span N / 2.4e6) + 1)` in integer
  arithmetic, `F0 = start + 956 250`, K' = min(tuners, H), R = ceil(H / K'),
  grid id = FNV-1a of (start, stop, N) - the same grid has the same id after
  a restart, so a page keeps its waterfall. A new grid (range or N) gets new
  arrays (allocated on the loop thread, never per packet).
- FFT size: `SWEEP_RES:N` takes N unless the sweep would hold more than
  `SWEEP_MAX_BINS` (2^22, 4.2 M) bins - then the largest N that fits (the
  full range at 16384 runs at 4096: 2.97 M bins); `fft_size` says what runs.
  `auto` = the largest N whose sweep holds at most `SWEEP_AUTO_BINS` (2^19):
  16384 up to ~123 MHz of span (146 Hz bins), 8192 to 246 MHz, ... 512 for the
  full range (4.7 kHz bins). Why: the sweep's speed doesn't depend on N (the
  dwell and the hops do), the resolution does - and what a plot can show:
  2^19 bins are ~128 per point of a 4096-point plot, so every zoom down to
  1/128 of the span still gets one bin per point; smaller N averages more
  frames per dwell (m = dwell x 2.4 MHz / N: 90 at the full range), which
  smooths the floor of a wide survey; the memory and the worker's work stay
  small. A user who wants 146 Hz over a wide range sets the size.
- Data path: the data receiver copies the bytes of the plan's tuners into a
  ring of 8 slots (vectors that only grow; no allocation per packet) and
  wakes the worker thread (`sweep::start` in main.cpp). Retuning packets go
  in once (no data) - the hop ends there. A full ring drops the packet
  (counted; "Sweep: N packets dropped" on stderr, at most every 10 s) - the
  next packet's counter / frequency still say what changed. Paused or outside
  the mode: nothing is queued.
- Worker, per slot: `frequency_change_counter` moved -> every tuner's hop is
  committed, then the round is counted. A retuning packet -> commit. The
  counter, not the flag, ends a hop: heimdall doesn't retune a tuner already
  on its round's hop, so with K' = H (5 tuners on <= 9.6 MHz, 1 tuner on 1
  hop) the stream runs on without any retuning packet and only the counter
  steps (heimdall's addition 1; tested). Each plan
  tuner of the packet: hop = round((f - F0) / S), taken only when the header
  frequency is within 1.5 kHz of that hop's centre (a tuner still on an old
  hop, or one the sweep doesn't move, is not on the plan's grid; a tuner that
  stays put for a round is - its hop is measured again). A hop change commits
  the tuner's old hop. Samples: N-sample blocks (a block may start in one
  packet and end in the next), each block's mean taken off (the RTL's DC
  offset), Hann, pffft (`kfft`), |X|^2 added DC centred.
- Commit: power / frames in dB; the DC bins N/2 - 1 .. N/2 + 1 replaced by
  the mean of N/2 +- 2 (the mean removal leaves a notch there and an RTL its
  LO leakage - neither a spike nor a hole every 1.9125 MHz); the tuners'
  common response (`nfloor::shape_db`, the measured KrakenSDR shape) taken
  off; the centre U bins go into the sweep array, the latest measurement of a
  hop replacing the one before. The hop's floor phi = its bins' 10th
  percentile (a signal may fill 90 % of the hop) + `spread_db(m)` - 2 dB:
  m frames averaged make a noise bin Gamma(m, 1/m), whose median stands
  `spread_db(m)` over its 10th percentile (8.18 dB at m = 1, 3.85 at 3, 1.73 at
  12, 0.35 at 256: exact - bisection on the incomplete gamma - up to m = 256,
  Wilson-Hilferty above, both checked against scipy). So the noise's median
  is drawn at +2 dB (like the regular spectrum's ~2 dB) for every FFT size
  and dwell; without it the noise moved from 0.5 dB (N = 128) to ~5 dB
  (N = 16384, m = 3). It also keeps hops of different m level: on the
  KrakenSDR a hop holds 1-2 valid packets at dwell 20 (heimdall's addition
  5), i.e. m = 1 or 2 at N = 16384 - each hop is levelled by its own m (a cap
  at the 10th percentile was tried and dropped: it put hops of m = 1 and 2
  ~3 dB apart). Cost: with m = 1 the noise is exponential and ~35 % of its
  bins fall below the 0 dB line (0); a carrier over the noise MEDIAN reads
  1.6 dB more than over its mean (ln 2). One packet (16384 samples = the
  largest N) always makes at least one frame.
- Levels: ONE reference for the sweep - `base` = the 20th percentile of the
  hops' floors - plus each tuner's gain step: where tuner k's block meets
  tuner k+1's, the lowest floor of the 6 hops on each side (neighbours in
  frequency, so they differ by the two dongles' gains only; a signal only
  raises a floor, so the lowest is noise as long as one hop of the six is;
  the first version took medians of 4 and a 7 MHz band over 2 of the 3 hops
  of one side moved a whole block by 10 dB); a step over 6 dB is no gain
  step (0); a block of one hop (K' = H) is its own six (the first version
  took no step there and a 5-hop range on 5 tuners showed the dongles'
  -2 .. +4 dB). Stored values = dB - (base + step of the
  tuner that measured the hop), in 0.01 dB (int16); a hop whose reference
  moved > 0.25 dB is rewritten (evaluated every 250 ms until the first sweep
  on a grid is complete, then every 1 s and at every completion) - so a
  signal that fills hops keeps its height and the waterfall doesn't pump.
  Limit: a signal wider than the whole range (or than six hops on both sides
  of a block seam) becomes the floor.
- Completion: the round index (`current_group_index`) of a counter change
  goes 0 after R - 1 with every round seen since the last 0 = one sweep:
  `done` (on this grid) + 1, its duration = the time between the two round-0
  events; progress = round / R. With R = 1 (one hop, or K' = H) every counter
  step completes a sweep (heimdall's addition 2; tested: one sweep per 20 ms
  round, flag-2 frames at the 20 a second limit). A plan change, a new grid
  or entering the mode restarts the round tracking (a partial pass is not
  counted). heimdall's own `run` on entering and its non-restarting dwell /
  settle / run changes (addition 3) need nothing here; `set_gain`'s retuning
  flag (addition 4) just ends the hop early - the packets after it are a new
  measurement of the same hop at the new gain.
- Frames: `sweep::build_frame` cuts and max-pools as type 7 (`pool = max(1,
  ceil(count / 4096), round(count / points))`, a point at its pool's middle,
  0 = never measured) over a max-pyramid of the array (level l = max of 2
  of level l - 1, updated over a hop's U bins on every commit): a pool is
  the max of ~2 log2(pool) nodes, whatever the bins. Flags 1 / 2 / 4 / 8 as
  the contract. `send_sweep_views` (the 50 ms tick, sweep mode only): a
  socket with `SUB_FFT` gets a frame when the data version, its view or the
  pause changed, or a sweep completed (flag 2, progress 1.0); compressed;
  skipped past 256 KB buffered. One completion frame per tick at most: a
  sweep faster than 50 ms gives one waterfall line per tick.
- Threads: settings and frames on the uWS loop, the producer on the data
  receiver, the engine on its worker; the array, pyramid and per-hop state
  under one mutex (a hop's write ~U values + their ancestors; a frame ~4096
  pools); the plan reaches the worker through a generation number.

**Measurements** (x86 dev machine, shared - load 10-29; no Pi was
available to this worker: the Pi numbers below are not measured).
- Engine bench (job scratch `bench/bench_sweep.cpp`: the real sweep.cpp fed
  5 tuners x 16384-sample packets as fast as it takes them, full range): the
  worker takes 3150-3740 packets/s = 21-25x real time (N = 4096: 21x, N = 128
  / 1024: 25x, limited by the offer rate there).
- `build_frame` at the bin cap (full range, 2.97 M bins): whole range at
  4096 points 55 us with hot caches, ~300 us measured inside the running
  receiver (the worker writes between frames), 100 MHz window 32 us, 1 MHz
  6 us; a plain max over the 3 M bins costs 110 us hot and grows with the
  bins.
- Whole receiver, worst case (full range, SWEEP_RES 16384 -> 4096, 5 tuners,
  dwell 5 ms, fake heimdall at 130 packets/s, t_cpu.py): 6.5 % of a core with
  no page taking frames, 10.3 % with one page at 4096 points, 19.3 % with four
  (one zoomed), 27 messages/s and 35 KB/s per page; for comparison coherent
  mode with one page 32.5 %, independent 48 %. No packet dropped. Of the four
  pages' cost the frame build is ~300 us x 80 frames/s; the rest is uWS /
  TLS / deflate (the same with compression off: 7.5 vs 9.2 %).
- On a Pi 4 / 5: NOT measured. Here the whole of sweep mode costs a fifth
  (no page) to two fifths (four pages) of what independent mode costs with
  one page; the worker does what independent mode's FFT workers do (one FFT
  per sample of every tuner) without the decimation, audio and spectrum
  work, so it should fit where independent mode fits. The frames scale with
  the pages (~300 us each at the cap here - expect several times that on a
  Pi, i.e. a few % of a core per page at 20 frames a second).

**Tests of the receiver** (2026-11-20, job scratch `client/`, not in the
repo: `fake_heimdall_sweep.py` = heimdall with sweep mode per the contract -
block assignment, flags, counter / round, retuning packets of garbage, the
status JSON, `--old`, an RF scene in absolute frequency with the KrakenSDR's
response shape, per-tuner gains 0 / +3 / -2 / +1 / +4 dB, a 3-count DC
offset, a rogue carrier on the tuners that take no part; a port-moved test
build; `run_all.sh`). All pass, also under ASan + UBSan:
- `t_sweep.py` 206 / 206 (range 400-430 MHz, 16 hops, 5 tuners): for N =
  128 / 1024 / 4096 / 16384 / auto six carriers - inside a hop, exactly on a
  hop seam, 1 kHz past one, at two tuner-block boundaries, near the end - on
  their point within half a bin (at most 0.43 bin: 0-48.8 Hz at 146 Hz, <= 7.5 kHz at
  18.75 kHz) and at their height (within -1.0 / +0.6 dB of A^2 N / 3 sigma^2,
  Hann scalloping included); the noise median at 1.88 dB (one level step
  below 2) for every N; the floor across hops and all five tuners within
  one level step (0.47 dB), edges vs centre of a hop the same; no DC spike or
  hole (+-3.3 dB within 6 bins of the hop centres); a band 10 dB over the
  noise (10.41 expected) at 10.35-10.82 dB in the hops it fills, across a block
  seam and in the partly filled ones. Tuners 0,1,2 only: the rogue carrier of
  the parked tuners (on hop 4's centre) not shown (2.8 dB = noise), no
  garbage of the retuning packets (highest point 4.7 dB). Completions one
  per sweep, 6.2 sweeps/s = the fake's 0.164 s, `last_s` median 0.164 s,
  progress 0.25 / 0.5 / 0.75, <= 20 frames/s (shortest gap 46 ms). Views:
  whole range / windows / outside the range, the pool (1004 points for
  1000), the 4096 cap, points 0 = 2048, seq, junk SWEEP_VIEWs, compressed;
  the grid id (same for tuners / dwell, new for N / range, back for the same
  grid). Every command with good and bad values (24 good, 12 junk refused);
  set_sweep only on a plan change; FREQ / TUNER_FREQ / WIDEBAND_FREQ /
  SCANNER_START / CONTINUOUS_SCANNER_START / SFSCAN_START refused; GAIN
  works and `gain_db` reaches a second page, also for a change made at
  heimdall. Pause: flag 4, no frames, a new view still answered, resumes;
  not saved. A data-port drop: the plan again at once. An old page (no
  SWEEP_VIEW): no type 10, no spectrum, JSON flows; type 0 again after.
  Restart: settings and mode back, the replay sends the saved plan before
  the mode, the same grid id.
- `t_edge.py` 50 / 50 (heimdall's additions; the fake then doesn't retune
  a tuner already on its hop - no flag): K' = H (400-409 MHz, 5 hops on 5
  tuners, R = 1) and one hop (433.5-434.5 MHz): a sweep per 20 ms round,
  ~20 flag-2 frames a second, `last_s` 0.020 s (heimdall 0.021), the floor
  flat over the five dongles (gains -2 .. +4 dB) at 1.88 dB, carriers at
  41.4 dB (41.8 expected); one packet per hop (dwell 5 at N = 16384, m = 1)
  and 1-2 packets per hop at random (the KrakenSDR's case) at N = 16384 and
  4096: every hop measured, hop medians within 0.94 dB, carriers on their
  point and height; a GAIN mid-sweep (the fake raises the retuning flag
  until the next settle): the floor stays.
- `t_modes.py` 15 / 15: coherent / independent / wideband scan each stream
  spectra (type 7), audio (1) and, coherent, DoA (3) before and after sweep
  mode; in sweep mode type 10 only; DoA parked and restored; the VFOs kept;
  sweep -> independent re-sends the per-tuner tuning.
- `t_old.py` 9 / 9: an older heimdall (coherent and independent): back to
  its mode in 0.47 s with the reason, the saved mode heimdall's, spectra
  again. `t_mcp.py` 9 / 9: the assistant's tools (above).
- Not run: the MinGW / zig syntax checks of the root CLAUDE.md (no MinGW or
  zig here; the code uses only the portable helpers and int64 / uint64 Hz);
  a debug playback end to end (refused by `playback::refuse` as before, and
  `send_control_command` sends nothing during one); a Pi.

## The page

`kraken_doa.html`, script block "SWEEP MODE" (after "SPECTRUM VIEWS"); its
state (`swp`, `swpKnown`) is in the globals block, because the View popover,
the zoom slider and `applyOpModeUI` read it. Prefix `swp` (the page already
says "sweep" for the path loss and range sweeps, `plSweep`, `rgSweep`).
Written 2026-11-20 against the contract above, before the receiver's engine
existed.

**The Mode selector.** A fourth option, Sweep (`value="sweep"`). It is
hidden and disabled until the receiver has sent its `sweep` message
(`swpKnown`, `opModeOptionsSync`): an older receiver never sends one, and
then nothing of sweep mode is offered, sent or shown - the selector's title
gains its Sweep sentence only then. On the downconverter variant it shows,
disabled, as "Sweep (not on the KrakenSDR Wideband variant)", and the title
says so (the other non-coherent modes are disabled there as before; generic
RTL-SDRs may sweep, also with one tuner). `setOpMode('sweep')` refuses in
both cases without sending anything. `applyOpModeUI` takes `sweep` (body
class `mode-sweep`; `swpEnter` / `swpLeave`), `opOnMessage` takes it as for
the other modes. CSS: `.sweep-only` (shown only in sweep mode), `.not-sweep`
(hidden in it); `.coh-only` and the right-hand DoA pane are hidden as in the
other non-coherent modes (the rail's Map still opens the map alone).

**Mode evidence.** heimdall's sweep stream carries the independent flag too
(0x2000 with 0x800), so in sweep mode the receiver's status and any type 6
/ 7 frame look like independent mode. While the latest `sweep` message says
`active`, `opHint` takes no hint at all (a new connection marks that state
stale until the receiver says it again). In sweep mode a type 7 frame is
dropped (only its hint is noted - it must not reach the main display's plot
and waterfall, which keep their picture for the return); type 6 already was.
A `sweep` message with `active` switches a page that shows another mode to
sweep - the page missed the `operating_mode` message - but not within 1.5 s
of an `operating_mode` message or of this page's own mode change
(`swpOpSentAt`), so a broadcast from before a change cannot pull it back.

**The top bar.** The frequency (`#tb-freq`) is replaced by the range
(`#tb-swp-range`): Start and Stop, `G.MMM.kkk` digits to 1 kHz (`.swp-d`, own
class: the top bar's code queries `.freq-digit` page-wide) in the
frequency's font, colour and box height (50 px - the bar keeps its 63 px when
the mode changes), 24 px digits in 15 px cells so that the range and the
tuner selector fit where the frequency, Audio and Step were (1700 px window
with the sidebar: still one row). Click the upper / lower half of a digit or
turn the wheel on it; Start stays at least 100 kHz under Stop, both inside
the receiver's `min_hz` / `max_hz` (a step past a limit stops at it).
Leading zeros are dimmed. Then the tuner selector (`#tb-swp-tuners`, icon
`tuners`): one toggle per tuner the receiver has (`available`), pressed =
takes part; the last pressed one cannot be released. Its title says what the
user asked to be said: more tuners = a faster sweep, each needs its antenna
connected. Gain stays (one `GAIN:` for every tuner; with the optional
`gain_db` the bar follows other pages). Audio / Volume and the tuning Step
leave the bar (`.not-sweep`). On the phone the range (Start and Stop a row
each, 20 px cells) and the tuners are rows of the drawer's Tuner & Audio box
before Gain (`MOBILE_RELOCATIONS`), with their text labels.

**Commands and echoes.** The page sends `SWEEP_RANGE:<MHz, 3 decimals>:<MHz>`,
`SWEEP_TUNERS:<list>` (`all` when every tuner is pressed), `SWEEP_RES:<N>|auto`,
`SWEEP_DWELL:<ms>`, `SWEEP_RUN:<1|0>`, and redraws from the `sweep` message
every page gets - no optimistic state. Spun controls (the digits, quick
toggles) send at most every 100 ms plus the last value (`swpSendSoon`). What
the user is changing does not jump back: the range and the resolution are
held 1.5 s after the last step (`SWP_HOLD_MS`; then the receiver's value
shows, also one it clamped), the Dwell box while it has the focus, and a
pressed tuner shows at once with a dashed border until the receiver's state
has it (2 s at most, then the receiver's state shows).

**The display.** In sweep mode the main spectrum and waterfall (`#fp`,
`#wf-container`) and the panes are hidden; `#swp-spec-box` (canvas, drawn by
`swpDraw`, at most once per screen refresh) and `#swp-wf-box` (its own
`WaterfallDisplay`) take their place under the spectrum's header. Its own
display rather than the main uPlot: that one is tied to the VFOs (tuning
bars, click-to-place, squelch lines, the beamformed overlay, bearing
colours) and to the coherent centre frequency - none of it belongs here, and
the main display keeps its picture and zoom for the return. The spectrum is
as tall as the main display's (`fpApplyHeight` sets both, the same split
drags it). Frequencies are doubles in Hz throughout (the frame's f64s, the
trace's plain arrays): at 1.765 GHz with 146 Hz bins the points stay on the
bin grid.
- The trace for the window shown is cut / padded from the latest frame
  (`swpTrace`, `specWindow` in Hz), so a zoom or a pan answers at once and
  the detail follows with the next frame.
- `SWEEP_VIEW` when the window or the plot width (device pixels, half with
  Slow link, 16 ... 4096) changes - zoom, pan, a window or sidebar resize, the
  map pane opening -, at most every 100 ms and the last one always; `0:0` at
  1x (the whole range: right after a range change too); again on entering the
  mode and after a reconnect.
- Waterfall: one line per frame with flag 2, never more than 20 a second
  (`SWP_WF_MS`; a quicker sweep's line waits for its turn, the newest
  replacing one still waiting). A new grid id (range or resolution) drops the
  waterfall's rows (`WaterfallDisplay.clear`, new) and the last frame; a new
  range also resets the zoom.
- Old frames: a type 10 frame outside sweep mode, one built for a view from
  before the page entered the mode (`seq < swp.seqMin`), and one whose grid id
  is not the latest `sweep` message's are dropped - the last only while that
  lasts under 1.5 s (a receiver whose message never comes would otherwise
  never show anything).
- Zoom: the wheel (x1.25 around the pointer) on the spectrum or the
  waterfall, and the View popover's Zoom slider, as far as one bin per point
  (`swpZoomMax` = span / (bin x points): ~490x for the whole range at 2.3
  kHz bins, ~7800x at 146 Hz) - the usual 32x would leave a 1.7 GHz range at
  ~54 MHz. The slider's end follows (`zoomTarget` gives `max`; `zoomText`
  writes whole numbers past 100x; other modes: max 125 = 32x, unchanged).
  Zoomed in, a drag pans; nothing is retuned and nothing but `SWEEP_VIEW` is
  sent.
- The View popover works on it with the common values (Scale, Colors, Speed,
  Contrast; Auto ranges the sweep's waterfall from the sweep's trace); its
  zoom readout is the points' spacing.
- The pointer: a line across the spectrum and the waterfall, and a readout
  box at the top of the spectrum - frequency (to 1 kHz, 100 Hz or 10 Hz by the
  points' spacing) and the highest level within the pointer's pixel.
- What the sweep is doing: a bar along the top of the plot and one in the
  spectrum's header (amber while paused), the progress in % and the time of
  the last complete sweep beside it - fixed widths (the frames come 20 a
  second); "No data yet" / "Paused" / "Waiting for the receiver" in the plot.
- Nothing of the VFOs: a click on it sends nothing.

**The sidebar.** A Sweep box (`data-sb="sweep"`, `.sweep-only`, title icon
`span`, placed first after the phone boxes): Resolution (Auto - naming the
width in use - and the FFT sizes 128 ... 16384 as bin widths, 18.8 kHz ...
146 Hz), Dwell (5 - 1000 ms, typed; a value outside is clamped before it is
sent), Run / Pause (`Pause` while running; `Run`, primary, while paused), and
the state: State (Running / Paused / No data yet), Sweep time, Progress,
Steps (hops and the tuners sharing them), Points (bins and their width),
Sweeps. Box by box in sweep mode:
- shown: Sweep; Station Information (the map); Display, but only Slow link
  and Points sent (they apply to the sweep's view; FFT size, Averaging and
  Edge clip belong to the live spectrum); the phone's Tuner & Audio and
  Panels; the footer.
- hidden: Decimators (VFOs need demodulation), MUSIC DoA, Beamforming, Web
  Mapper, DoA Recording, Continuous Scanner, Experimental (`.coh-only`, as in
  the other non-coherent modes), Scanner (wideband scan only), IQ & Audio
  Recorder (IQ is coherent-only, audio needs VFOs), Saved Frequencies (a
  click tunes a VFO; its scan needs one), Debug Playback (a coherent array's
  test transmissions through the VFOs and decoders). The rail's Decoders and
  Transcription buttons and the panel under the waterfall go too (nothing is
  decoded); the panel comes back as it was.

**Tests** (2026-11-20, headless Chromium, real mouse / wheel / key / touch
events, scratch `52196024/tmp/page/`). The receiver's engine did not exist
yet: the page ran against a TEST kraken_doa of the tree before this change
(it serves the page; ports 286xx) with a fake heimdall (coherent scene), and
the contract's engine ran inside the page (`mock_sweep.js`, injected before
the page's scripts: it records everything the page sends and answers the
sweep commands with `sweep` / `operating_mode` messages and type 10 frames
through the page's own `ws.onmessage` - plan, hops per tuner block, pooling,
flags, grid ids, a synthetic 24 - 1765 MHz spectrum). `SOURCE=real` turns the
mock into a recorder only, for the run against the real engine.
- `t_sweep.py` (154 checks): the selector; entering (what shows and hides in
  the bar, the display, the boxes, the Display rows; the first view at the
  plot width; at 1x the trace equals the whole-range frame); layout changes
  (map pane, hidden sidebar, the split) re-ask the view; the range digits
  (wheel, both halves of a click, the hover halves, the limits, start < stop,
  the send rate, the hold against a stale broadcast); the tuner toggles
  (pending look, the last one stays, `all`, the rate); Resolution, Dwell
  (typed, clamped), Run / Pause; the header's fixed widths over 25 frames;
  waterfall lines only on flag 2 and never over 20 a second (sweeps of a
  1-hop range at ~80 a second: ~13 - 20 lines a second), one per sweep
  otherwise; a grid change drops the waterfall and an old-grid frame; zoom
  (the frequency under the pointer stays, the window answers at once, views
  at most every 100 ms, the detail frame follows, the limit = one bin per
  point, pan by drag sends only views, 1x = `0:0`); bin-exact frequencies at
  1.7642 GHz with 146 Hz bins; the View popover (zoom slider, Scale, Colors,
  Speed, Contrast, Auto from the sweep); the readout (433.920 MHz at the
  carrier's level); one `GAIN:`, `gain_db`; a click sends nothing; the mode
  switches sweep <-> coherent / independent / wideband (panes, the main
  display, its frames, the zoom slider's 125, the digits, the VFO cards back;
  a frame after leaving dropped); a missed `operating_mode`; a reconnect.
- `t_compat.py` (22 checks): the page before the change and this one,
  measured in coherent, independent, wideband and coherent again (the top
  bar's controls, the spectrum's header, the display, the panes, every
  sidebar box, the rail) - identical with a receiver that never sends a
  `sweep` message and with one that does; the older receiver: Sweep not
  offered, the title unchanged, nothing of sweep mode sent, a forced
  `setOpMode('sweep')` refused; the downconverter variant.
- `t_bar.py` (16 checks): one row and the coherent bar's height at 1700 and
  1920 px, one centre line, the order, the digits' font, no caption text,
  every icon resolves.
- `t_phone.py` (15 checks, 400 x 860 touch): the drawer's rows, a tap on a
  digit's upper half, finger-sized tuner toggles and a tap on one.
- Not covered: Firefox / Safari; the real receiver and heimdall (to come);
  the theme check passes (`tools/theme_palette.py --check`).

## Tests

(each part's tests and the run on the KrakenSDR)
