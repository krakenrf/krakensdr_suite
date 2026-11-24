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

- Sample rate fs = 2.4 MS/s. FFT size N = a power of two, 128 ... 65536
  (16384 until 2026-11-21); bin = fs / N (18.75 kHz ... 36.6 Hz). An N over
  16384 (one 8091 packet) spans the packets of a hop.
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
  - `"dwell_ms":D` (5 ... 5000, default 5 = one packet since 2026-11-21,
    20 before) - how long the settled stream
    runs on each hop. (Made exact 2026-11-21: a hop delivers the fewest
    whole 8091 packets that hold D, `ceil(D x 2.4e6 / (1000 x 16384))` per
    tuner - 5 ms -> 1, 10 -> 2, 20 -> 3, 50 -> 8, 100 -> 15 - and the next
    hop starts as soon as the last of them is out; the status object's
    `hop_packets` says the number.)
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
  `"l1_unquiet_flushes"` (diagnostics). Default `settle_ms` 5 (2 since
  2026-11-21, with the faster hop loop).
  A tuner whose retune failed (twice) has the 8091 per-channel frequency
  **0** ("no valid frequency") from that hop until its next successful
  retune - it is not on its old frequency either (the tuner drivers fail
  after writing the new PLL values); 0 is on no grid, so a receiver drops
  that channel. The status object also has `"failed_tuners":[..]` (the
  tuners whose last retune failed). Leaving sweep mode for coherent: with a
  librtlsdr that defines `RTLSDR_NO_STREAM_SLIP` (2026-11-21: no tune drops
  samples any more) the streams are re-paired and only the phase is
  recalibrated (seconds); with an older library every retune shifted that
  tuner's samples by 2 against the others (a USB control read of 3 bytes
  did it), and heimdall re-acquires every channel's lag first (25-30 s).
- Additions 2026-11-21 (faster hops; nothing above changed): a hop
  delivers exactly `hop_packets` = `ceil(dwell_ms x 2.4e6 / (1000 x
  16384))` valid packets per tuner (it waits for them at most 250 ms
  longer than they take; this replaces "at least one set") and then hops
  at once; the status object also has `"hop_packets"`, `"stalled_tuners"`
  (tuners that delivered no data at the last gate: while one is silent no
  valid packet goes out and the flag stays 1), `"gates_failed"`,
  `"gates_late"` (these three replace the first version's
  `"gates_unsteady"`) and `"timing"` (the hop timeline, averages of the
  last 64 hops: `flag_ms`, `locked_ms`, `retune_ms`, `tune_max_ms`,
  `settle_ms`, `gate_ms`, `clear_ms`, `first_valid_ms`, `dwell_ms`,
  `round_ms`, `round_min_ms` / `round_max_ms`, `valid_per_hop`,
  `valid_min` / `valid_max`, `hops`, `moved`; diagnostics). The counter
  steps once per round (a round retried after a stalled tuner is not a new
  one). The guarantee's bound, exactly: every sample of a valid packet was
  taken at or after retune end + settle - L* - 33 us, L* = that tuner's
  fastest USB delivery among its last RTL_USB_BUF_COUNT + 16 (48) packets
  (~1 ms on a PC; a reader running steadily behind makes it larger - the
  settle has to cover it; heimdall's operating-modes.md, *Review of the
  hop loop*). Also `"slow_tuners"` (tuners that deliver but whose packets
  never reach the hop's start: lost packets) and `"l1_depth"` (each
  tuner's L1 now; diagnostics), added in the third review round;
  `"l1_put"` (packets each tuner has delivered into L1 since its stream
  started - a tuner a packet short of the others for good lost a USB
  transfer) and `"arrival_phase_us"` (where each tuner's latest delivery
  lies against tuner 0's, within +-P/2: the spread of the buffer
  boundaries), added in the Pi round (round 3 under *Tests*); diagnostics,
  in every mode.

- Added 2026-11-24 (round 11, gain by region; nothing above changed):
  `{"command":"set_sweep_gains","points_hz":[f1,...,fn],"gains_db":[g0,
  ...,gn]}` - n <= 64 points (whole Hz, strictly rising) cut the sweep's
  axis into n + 1 regions, `gi` for `points[i-1] <= f < points[i]`; gains in
  dB as `set_gain` takes them (0 - 50; the AGC refused in a table), each put
  on the nearest value of the tuners' gain list. `[]` / `[]` = no table: the
  common gain everywhere. Reply `{"status":"success","points_hz":[..],
  "gains_db":[..]}` (as applied) or an error (nothing changes). A hop takes
  the gain of the region its centre lies in, from the next hop on, without
  a restart of the plan; with a table the common gain is left alone (the
  other modes use it, every tuner returns to it when the mode is left), and
  it wins over the common gain's AGC in sweep mode. Accepted in every mode,
  not saved by heimdall. Status (the sweep object, always):
  `"gain_points_hz":[..],"gain_db":[..]` (empty = no table) - their presence
  is how a client knows heimdall has it; also `"gain_writes"` (of the last
  completed sweep), `"gain_write_failures"`, `"gain_fast"`, and
  `"gain_real_db"` (heimdall's measured gain curve: per value of the tuner's
  gain list the real dB against the list's top; R820T / R828D only).
  Rounds 9 - 10's `set_sweep_auto_gain` was removed the same day (an older
  client gets "Unknown command").

### heimdall, 8091 (sweep mode)

- Phase-state flags `0x2000` (sweep) together with `0x800` (independent): a
  client that does not know sweep mode treats the stream as independent
  tuners (no DoA on it). A client that knows it tests `0x2000` first.
- Per-channel frequency = where that tuner is (as in independent mode);
  per-channel gain = the gain that tuner's samples in this packet were taken
  with: the common gain, or with a gain table the gain of the region its hop
  lies in (-1 = the tuner's AGC). Read with the frequency, before the flag.
  (Round 10's trial bits, `0x4000` / `0x10000 << ch`, were never released
  and are gone with Auto gain.)
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
- `SWEEP_RES:<N>|auto` - the FFT size (128 ... 65536; 16384 until
  2026-11-21); `auto` (0) picks one from the span (never over 16384). What
  is used is at most what fits: the sweep's bins (`SWEEP_MAX_BINS`, 2^24 =
  16.8 M since 2026-11-21, 2^22 before) and a hop's samples at the dwell
  (`ceil(dwell x 2.4e6 / (1000 x 16384))` packets of 16384: 32768 from
  7 ms, 65536 from 21 ms). The `sweep` message says what is used.
- `SWEEP_DWELL:<ms>` - 5 ... 1000, default 5 (one packet a hop; 20 until
  2026-11-21).
- `SWEEP_GAINS:<f1,f2,...>|<g0,g1,...>` (2026-11-24, gain by region; Hz |
  dB) - the whole table: n <= 64 points strictly rising, n + 1 gains 0 - 50
  dB (not Auto); `SWEEP_GAINS:|` clears it. A malformed table is refused
  (CommandRejected: not saved, nothing sent). Saved (`sweep_gains`, replayed
  after `SWEEP_DWELL` and before `OPERATING_MODE`), sent on to heimdall as
  `set_sweep_gains` right after every `set_sweep` (mode entry, the settings
  replay, a data-port reconnect, a plan change) and at once on a change in
  sweep mode; what heimdall answers (its gains on the tuners' list) is what
  is kept, saved and broadcast. Kept by frequency whatever the range,
  tuners, dwell or resolution do; one table per receiver. Not refused
  outside sweep mode: it takes effect on entry.
- `SWEEP_AUTO_GAIN:` (rounds 9 - 10, removed 2026-11-24): accepted and
  ignored for an older page (no reply, not saved, nothing to heimdall); an
  old saved `sweep_auto_gain` is dropped from `doa_settings.json` at the
  next save.
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
  Optional, added 2026-11-21: when `fft_size` < `res`, `"cap":"bins"` (the
  sweep would hold more than 2^24 bins: a narrower range gets the size) or
  `"cap":"dwell"` plus `"dwell_ms_needed":D` (a hop holds fewer samples
  than the size: dwell D gets it - or the largest size the bins allow, if
  that is smaller). Neither field when the size asked is used. `bin_hz`
  carries 8 decimals (exact to 65536: 36.62109375; 6 before). Also added
  2026-11-21, for the page's Resolution list (always sent): `"sizes":[128,
  ...]` = the sizes that run as asked at this range and dwell;
  `"sizes_capped":{"32768":"bins"|"dwell",...}` = every other size of 128
  ... 65536 and why - `bins` when the range is too wide for it (a longer
  dwell doesn't help), else `dwell`; `"min_dwell_ms":{"32768":7,
  "65536":21}` = the dwell each size past one packet needs. A page without
  these fields assumes 128 ... 16384; an older page ignores them.
  Optional, added by the page worker 2026-11-20: `"gain_db"` = the common
  gain (heimdall's `set_gain` value, -1 = AGC). Sweep mode has no type 7
  frame whose header carries the gain, so a page shows another page's gain
  change only through this field; a receiver without it leaves each page's
  gain control as that page set it (nothing else depends on it).
- Gain by region (2026-11-24), additive fields of the `sweep` message,
  always sent: `"gain_points_hz":[..]` (Hz), `"gain_regions_db":[..]` (dB,
  one more; both empty = no table - the common gain stays in `"gain_db"`),
  `"gain_regions_known"` (true once heimdall has shown it has the table - a
  status line with `"gain_points_hz"` or its answer to `set_sweep_gains`;
  false before, and when "Unknown command" answers the command within 2 s:
  an older heimdall - the page then offers no lane). Rounds 9 - 10's
  `auto_gain*` fields and their `{"sweep_gain":{..}}` map message are gone
  (an older page ignores their absence: its Auto gain switch stays
  disabled); the table serves the page's readout of a step's gain.
- `{"operating_mode":{"mode":"sweep",...}}` as for the other modes.
- Binary type 10, the sweep spectrum for one page's view (little-endian,
  64-byte header), sent to every socket that sent a `SWEEP_VIEW`, at most
  20 a second while hops arrive, and once when a sweep completes:

  | Offset | Type | |
  |---|---|---|
  | 0 | u32 | 10 |
  | 4 | u8 | flags: 1 = the requested window (else the whole range), 2 = this frame completes a sweep (one waterfall line), 4 = paused, 8 = no data yet, 16 = overview (added 2026-11-21, below) |
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
- Added 2026-11-21 (the user: rows of the waterfall made while zoomed in
  held only the zoomed window): `SWEEP_OVERVIEW:<points>` - transport level,
  per connection, like `SWEEP_VIEW` (not persisted, not replayed, not
  echoed, not a control command); 0 = off (every socket's default), else
  clamped to 16 ... `SPECTRUM_VIEW_MAX_POINTS`; junk (no whole number) is
  ignored and the setting stays. While it is on AND the socket's view is
  narrower than the whole range (its frames carry flag 1), every type 10
  frame with flag 2 that the socket gets is preceded IMMEDIATELY by one more
  type 10 frame, the **overview**: flags 16 | 2 (| 4 when paused; never 1,
  never 8 - a completed sweep has data); `seq` = the view's seq; `sweeps`,
  start / stop / grid, progress (1.0, as on every flag-2 frame) and
  duration as in the frame that follows (the page pairs the two by
  `sweeps`); point 0, spacing, n and the levels = exactly what a
  whole-range `SWEEP_VIEW` of that many points gets (n is about `points`,
  0.67 ... 1.5x of it, never above 4096 - read it from the frame). Both
  frames are built under one lock, so they show the same state of the
  sweep. They go out on the 50 ms tick like every type 10 frame (at most one
  flag-2 frame, so at most one overview, per socket and tick); a socket
  skipped for backpressure misses both. A socket whose view is the whole
  range gets no overview (its frame is one); a socket that never sent the
  command gets what it got before, frame for frame. An older receiver
  ignores the command (no reply, no log line, no echo, not saved: checked
  on 3919d64); an older page never sends it.
- Added 2026-11-21 (the user: "the full resolution sent to the browser"):
  `SWEEP_FULL:<1|0>` (also true / false; junk ignored) - transport level,
  per connection like `SWEEP_OVERVIEW`, default 0; an older receiver
  ignores it. While it is on, a socket with a sweep view gets the **full
  set** - the whole range at the sweep's own bins - as binary type 11:

  | Offset | Type | |
  |---|---|---|
  | 0 | u32 | 11 |
  | 4 | u8 | flags: 1 = the set's last part, 2 = a snapshot (the store as it is, not a completed sweep: the trace only, no waterfall row), 4 = paused, 8 = deflated (added 2026-11-23, below) |
  | 5 | u8 | 0 |
  | 6 | u16 | pool = bins per value (1: the sweep's own bins) |
  | 8 | u32 | grid id |
  | 12 | u32 | sweeps completed on this grid (a completion set: its flag-2 frame's) |
  | 16 | f64 | Hz of value 0 (its centre) |
  | 24 | f64 | spacing, Hz (bin x pool) |
  | 32 | u32 | values in the whole set |
  | 36 | u32 | this part's first value |
  | 40 | u32 | n |
  | 44 | u32 | set id (per socket, + 1 a set; all its parts carry it) |
  | 48 | u8[n] | levels, type 10's scale (0 = never measured) |

  Parts of at most 524 288 values, ascending, no gaps. When: with every
  completed sweep AFTER its flag-2 frame (not before, as first proposed:
  uWS drops a socket's data past 2 MB queued, and a set is up to 16.8 MB -
  it is paced by the socket's queue, the live frames go first); the page
  attaches a complete set to the row of the same grid and `sweeps`. On
  `SWEEP_FULL:1` the store as it is, once, right away (flag 2; also while
  paused; nothing while there is no data) - unless that tick's completion
  set goes, which serves for it. Per socket one set at a time: a completion
  while a set is in flight, or with more than 256 KB queued, gets no set
  (skipped whole: the row keeps its overview / window levels). A set in
  flight is abandoned at a grid change, at `SWEEP_FULL:0` and out of the
  mode (the page drops what is incomplete). Not compressed (until
  2026-11-23: `SWEEP_FULL_Z` below). No bin cap and
  no rate limit (the measurements below); `pool` is there for one. A socket
  that never sent the command gets what it got before.
- Added 2026-11-22 (the user: "other resolution options for the display
  too"): `SWEEP_FULL:<n>` with n >= 2 = a set of at most n values (n below
  1 024 is taken as 1 024: `pool` is a u16, and 1 024 keeps it <= 16 384
  for every grid). pool = the smallest power of two with ceil(bins / pool)
  <= n, 1 when bins <= n - one level of the store's max pyramid, value i =
  the max over bins [i pool, (i + 1) pool), the last group maybe shorter.
  Header: `pool`, Hz of value 0 = start + (pool - 1) / 2 x bin (the first
  group's centre, as type 10's point 0), spacing = bin x pool, values =
  ceil(bins / pool); the rest of type 11 as above. Any whole number is
  taken (not only the listed ones); junk is ignored. A change of n
  abandons a set in flight and brings a snapshot at the new n on the next
  tick (or that tick's completion set, which serves for it); the same n
  again does nothing. One set per pool in use is built a tick, shared.
  The `sweep` message carries (additive) `"full_sets":[{"n":16384,
  "pool":1024,"values":11607,"step_hz":150000.0}, ..., {"n":1,"pool":1,
  "values":11885227,"step_hz":146.484375}]`: the steps this receiver
  offers for the running grid - n = 16 384, 65 536, 262 144, 1 048 576,
  4 194 304, each with what it gives, a step whose pool would be 1 left out
  - and full (n 1) last. Absent: a receiver of 2026-11-21 (`SWEEP_FULL`
  on / off only; it ignores `SWEEP_FULL:65536` as junk) or older. "Fit to
  the plot" (0) is the page's and not in the list.
- Added 2026-11-23 (the user: "the sweep full resolution compression"):
  `SWEEP_FULL_Z:<1|0>` (also true / false; junk ignored) - transport
  level, per connection, default 0: this page can inflate. Its type 11
  parts then go deflated where the receiver has them in time and they are
  smaller: flag 8, and the bytes after the 48-byte header are ONE complete
  zlib stream (RFC 1950: header, deflate data, Adler-32) inflating to
  exactly `n` values (`total`, `first`, `n` count values as before; the
  message is 48 + the stream). A part without flag 8 is raw as before; a
  set may mix both (one stream a part: the parts inflate independently,
  the page assembles them in order). Read per part, so a change of the
  command mid-set changes only the parts still to go. The `sweep` message
  carries `"full_z":true` (this receiver deflates). An older receiver
  ignores the command (raw sets); an older page never sends it and gets
  exactly what it got before. Lossless: one byte a value after inflating.
- Levels: ONE reference for the whole sweep (never one per hop: a signal
  that fills a hop must keep its height), the tuners' common in-band shape
  removed (`nfloor::shape_db`, *Noise floor*). A hop's levels are referred
  to the reference gain - the gain table's highest, else the common gain
  (round 11; the common gain, "the ceiling", in rounds 9 - 10): shifted by
  real(reference) - real(the gain its packets were taken with), real() =
  heimdall's measured curve (`"gain_real_db"` in its status' sweep object:
  per value of the tuner's gain list the real dB against the list's top;
  linear between its points, clamped at its ends), or - no curve: an older
  heimdall, a tuner it has not measured - the list's own difference (which
  over-corrects: the R820T's 8.7 is -34.2 dB, not -40.9) - so a signal keeps its
  true height across hops of different gain; the floor stays flat where the
  tuner's noise dominates, and shows higher where the lower gain puts the
  hop on the converter's own floor (the sensitivity actually lost - not
  hidden by forcing it flat). One gain everywhere: no shift. Not corrected:
  the curve's dependence on frequency (2 - 3 dB, the FM band's top end
  more), the spread between units (not measured); the header and the table
  stay the list's values (the gain set).

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

- **Debugging the levelling** (2026-11-24): `KRAKEN_SWEEP_SEAM_DEBUG=1` in
  the receiver's environment makes every `evaluate` (each completed sweep
  and at most once a second between) write one line to stderr: `seamdbg
  <epoch ms> done <sweeps>`, then per seam `| a|b <src> <step> (now <raw>)`
  with the hops taken on each side (`a ...` / `b ...`, each `h:floor@gain`,
  `*` = at the reference gain), and `|| base <dB> off <per rank>`. `<src>`:
  C = the median of the kept reference steps with now's, M = the kept ones
  alone (now's past the 6 dB cap, or a side without a hop at the
  reference), S = the hops at a gain both sides have (a seam not yet seen
  at the reference), 0 = none (round 11; until then A = all hops, - = none). Off by default (read once
  at startup); about 12 lines a second at a fast sweep - it slows a loaded
  Pi a little, so leave it off for timing. `t_agc`'s seam failures were found
  with it (*Seam steps, the median* below); the test folder's
  `count_glitches.py` counts isolated step spikes in such a log.
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
- A replayed `FREQ:` (the startup replay with heimdall already sweeping,
  Reset settings in sweep mode) is not refused: it is kept
  (`requested_freq_hz`, MUSIC's frequency) and goes to heimdall
  (`set_frequency`) after `set_operating_mode` when the mode leaves sweep for
  coherent / wideband scan; sweep -> independent seeds unset tuners from it
  (review finding 3: it was refused, so independent seeded from a hop and
  Reset settings left the receiver off the file's 100 MHz).
- Entering sweep mode - by command or by following heimdall - ends a
  recording (IQ: a fixed band; audio: nothing to demodulate). Following
  heimdall stopped none (review finding 5). The calibration state stays live
  (review 4): the data receiver updates the scanner manager's phase state /
  noise flag and `cal_live` from the header in sweep mode (the decimation
  thread, their only updater, gets no packet then: the pages' chip said
  "Converged" for the whole session on the KrakenSDR).
- After a settings replay (startup, Reset settings) a client still in sweep
  mode sends heimdall the replayed plan (`sweep_plan_after_replay`): with
  generic RTL-SDRs a replayed `OPERATING_MODE:coherent` is refused and the
  client stays in sweep, and a sweep adopted from heimdall before the replay
  had heimdall's own plan - the setters send nothing during a replay, and
  their dirty flag was consumed without a send (review finding 1: "No data
  yet" until a setting was touched). The flag is now taken only when the
  plan goes out.
- An older heimdall: it answers `set_sweep` "Unknown command" and the mode
  "mode must be coherent, wideband or independent", and its header never
  shows `0x2000`. `ingest_sweep_reply` (the control socket's drain) reads
  whole lines (a reply split over two reads is joined) within 4 s of the
  request and takes only `set_operating_mode`'s refusals ("mode must be
  coherent, wideband or independent", "sweep mode refused (not available on
  this hardware)", "stop the scanner first"; "Unknown command" only names the
  reason). The first version took any error with "mode" / "sweep" /
  "scanner" in it (review finding 7: heimdall's "Not in wideband scan mode"
  landing before the first 0x2000 header made the client fall back, rewrite
  the saved mode and re-adopt sweep 3 s later); `note_sweep_header`
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

Two changes outside sweep mode, both intended (confirmed in the review
round): following heimdall into any non-coherent mode (`note_server_mode`)
now also stops the continuous scanner, which needs coherent mode (it kept
running against independent / wideband-scan tuners); the assistant's
`set_gain` reply says "the array recalibrates" in coherent mode only
(`multi_tuner_mode()`: no calibration runs in wideband scan mode either).
Also silent in sweep mode now: the FM thread's "No data for 10s / 20s / ..."
(only the 2 s line was), and the assistant's `get_screen` gives VFOs no level
and no open squelch (the spectrum it read was from before the sweep);
`get_spectrum` pools the sweep once (it pooled twice: half the points).

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
- `SWEEP_RES:` - `auto` / 0, else the nearest power of two in 128 .. 65536.
- `SWEEP_DWELL:` - 5 .. 1000 ms. `SWEEP_RUN:` - 1 / 0 / true / false, else
  refused (junk paused the sweep: review finding 6); not saved; `set_sweep
  {"run":..}` alone.
- Settings schema (replay order): `SWEEP_RANGE` `SWEEP_TUNERS` `SWEEP_RES`
  `SWEEP_DWELL` (and since round 11 `SWEEP_GAINS`, default `|`) right
  before `OPERATING_MODE:` (defaults 87.5-108 MHz, all, auto, 20 ms). During the replay the setters send nothing; the replayed
  `OPERATING_MODE:sweep` sends the plan. `set_sweep` goes to heimdall only
  when what it holds changed (`take_plan_changed`: centres, tuners, dwell -
  an FFT size change alone sends nothing), and again after a reconnect of the
  data port.
- `gain_db` in the `sweep` message (the contract's optional field, asked for
  by the page worker): the common gain from heimdall's 8091 header in sweep
  mode, or a `GAIN:` command; the message goes again when it changes. With
  a gain table a header's gain is that hop's region's and is not taken
  (round 11). (History, rounds 9 - 10: with
  Auto on a header's gain is that hop's: the ceiling then comes from `GAIN:`
  and heimdall's `gain_ceiling_db` (its status; 2026-11-24 - the highest
  gain of the map when heimdall doesn't send it: in a search sweep every
  hop is a step below the ceiling, and taking the sweep's highest gain made
  the ceiling, and so every hop's shift, flip every other sweep).
- **Gain by region** (round 11, 2026-11-24; `sweep::set_gains`,
  `gains_command`, `take_gains_save`, `regions_active`): the table (`g_reg`,
  under `g_reg_mu`; points Hz, gains 0.1 dB) is the receiver's own: the
  command sets it as asked, heimdall's answer (`ingest_control_reply`: a
  success line with `"points_hz"` and `"gains_db"`) replaces it with what
  heimdall applied, and the broadcast tick saves that (`take_gains_save`)
  and sends the state. `ControlHandler::send_sweep_plan_now` sends the
  table after every plan (`send_sweep_gains_now`: the control socket
  drained first, the ask noted - "Unknown command" within 2 s without an
  answer = an older heimdall). The levels: `g_ref` (`update_ref`, on every
  change of the table, of heimdall's knowledge or of the common gain) = the
  table's highest gain while one is in force (and heimdall doesn't say it
  has none), else the common gain; `commit` shifts by real(`g_ref`) -
  real(the packets' gain) as before. The seams (`evaluate`) take a tuner
  pair's step from hops at `g_ref` on both sides - round 10's "at the
  ceiling" with the ceiling made the reference: a region turned down over a
  seam keeps the step last taken at the reference (the dongles' step
  doesn't move with a gain; taken at a lower gain the floors' difference
  shrinks as the converter's equal noise takes over, which would move the
  block's signals); a seam never seen at the reference takes the hops at
  the gain both sides have most (`S`), else 0 - no longer all of its hops
  (that folded a lower region's lost sensitivity into the next block, and
  its signals' heights with it). The histories (`Store::gain_changed`)
  restart when `g_ref` changes, not when a region below it does. The data
  receiver no longer takes the header's channel 0 gain for the common gain
  while a table is in force (`regions_active`).
- Round 10 (2026-11-24; history - Auto gain was removed in round 11): heimdall's Auto gain seeks each hop's highest
  gain at which the chain is still linear (the first design - the lowest
  floor referred to the input - was dropped after the first hardware scan:
  compression fools it; heimdall's `operating-modes.md`, "The linearity
  search") - probes (a visit a step off), a search phase (the testable
  hops a step down in every other sweep), blocks below the ceiling for
  good. The receiver: each visit is shifted by the real gain of its own
  packets (as before); the gain map from the hops' own (non-trial) visits; the
  ceiling from heimdall; the seam steps kept (below); heimdall's status
  fields `gain_floor_hops`, `gain_probes`, `gain_floor_decisions`,
  `gain_search_hops`, `floor_scatter_db`, `gain_pairs`,
  `gain_pairs_unbracketed`, `gain_pairs_nonlinear`, `gain_step_learned`,
  `gain_probes_skipped`, `gain_level_guesses` (numbers, the last completed
  sweep's) and `gain_probe_sweep` (true / false) passed through to the
  `sweep` message as heimdall sends them - each one absent when heimdall
  doesn't send it (an older heimdall; additive). The state goes again when
  `gain_floor_hops` or `gain_search_hops` changes or a field appears /
  goes; the per-sweep ones (`gain_probe_sweep`, `gain_probes`, ...) ride
  along with the next state (any `SWEEP_` command answers with it). Also
  `"trials_left_out"`: the trial visits this receiver left out of the
  picture since it started (the receiver's own count; at first only the
  up-trials, see the next point).
- Trial visits (history: round 10, the 8091 trial bit of the visit's channel; the
  last packet of a visit decides): since the first hardware run (heimdall
  62b62c1, 2026-11-24) EVERY trial visit is left out of the picture while
  its hop has a current row - one stored at most `TRIAL_ROW_SWEEPS` (3)
  completed sweeps ago: not in the stored row, so not in the live frames, a
  full set, the overview or the waterfall, which all read the store, and
  not in the floors the reference and the seam steps are taken from. An up
  trial may be past the chain's linear range (ghost carriers, a carrier
  short); a down trial was meant to be harmless, but on the hardware a
  visit 6 - 8 dB down where the tuner's noise nears the converter's own
  (the FM band, above 1 GHz, hops already far down; LTE hops at 16.6 tried
  at 8.7 every other sweep) showed a floor several dB higher in that row.
  (Before that: only trials above the hop's last drawn gain were left out.)
  A hop with no row yet on the grid, or only a stale one, draws its trial
  (something rather than nothing). No trial visit changes the hop's gain
  map entry, so neither the map, `gain_reduced_hops` / `gain_min_db` nor
  the page's amber strip move for a probe. `trials_left_out` counts every
  trial visit left out (both directions). Without the bits (a heimdall that
  doesn't send them) every visit is drawn as before. A hop whose visits are
  left out keeps the row of its last drawn visit; nothing waits for a drawn
  visit: a sweep completes on heimdall's round counter, and its full set,
  the `sweeps` count and the progress come from the store as it is then.
  Tested (`t_trial.py`, fake heimdall: 16 hops, every hop's own gain 33.8
  dB under a 42.1 ceiling; up-probes (36.4) carry a ghost carrier 40 counts
  at +333 kHz, down-probes (32.8) and search-sweep visits a marker at +555
  kHz; every bin of every sweep through `SWEEP_FULL`): sparse probes 1 in 3
  per sweep - no ghost and no marker in any of 122 sweeps' rows, no gain
  map sent, `trials_left_out` 0 -> 948; without the bits ghosts in 313
  hop-rows (the check sees them when drawn); the search phase (every hop a
  down trial in every other sweep) - no marker in 98 sweeps, 12.2 complete
  sets a second as without probes (nothing waits), ~half the stores left
  out; every visit a trial for good - every hop keeps a row, the sweeps go
  on, a row older than 3 sweeps draws the trial (1.6 times the search
  phase's share left out); a new grid where every visit is a trial: every
  hop has a row. `t_probe.py`: 12.2 sweeps a second in every phase; the
  clean hops' mean level in the even against the odd sweeps (the normal
  against the whole-plan probe sweeps) +0.006 dB in the search phase,
  +0.003 with tuner 2's block reduced, manual +0.006 (it was -0.038 /
  -0.044 when the down trials were drawn; without the bits -0.037 / -0.050,
  as before); a hop probed every other sweep shows its own-gain row, at
  most 1 sweep old.
- Round 10 tested (history) with the fake heimdall (`t_probe.py`: 16 hops, 5 dongles
  0 / 3 / -2 / 1 / 4 dB apart, their real gains off the published curve by
  -0.4 ... +0.4 dB per 10 dB down; every bin of every sweep through
  `SWEEP_FULL`): the search phase, sparse probes (1 visit in 6), tuner 2's
  block at 29.7 dB for good with probes and with the search phase on top -
  every quiet hop at the ceiling within one level step (0.47 dB) sweep to
  sweep in 95 % of the sweeps, as manual; every block within 0.5 dB of
  manual; the reduced block 6.2 - 6.6 dB up for 6.05 predicted (the
  converter's floor +6.85, its unit's curve error -0.8), its probe visits'
  floor 1.4 dB apart (a step lower on the converter's floor: real); the map
  steady (no map at all while it doesn't change; the reduced block's one
  change-over, then steady). Before (67a7060): the search phase flipped the
  ceiling - quiet hops jumped 2.35 dB, the map flickered (119 maps of 2
  kinds in 8 s) -, and the reduced block was folded into tuner 2's seam step
  (shown at 2.75 dB, its sensitivity loss hidden).
- The gain curve (2026-11-23): `sweep::ingest_control_reply` takes
  `"gain_real_db":[[g, real], ...]` from heimdall's status lines
  (`parse_curve`: at least 2 points, the gains rising within 0 .. 100 dB,
  the real gains within -100 .. 10 dB, else ignored and the last good one
  kept); the worker copies it when its version moves (`curve_real`). The
  `sweep` message carries `"gain_curve_points"` (0 = the list's values).
  Tested (`t_agc.py curve`; the fake's tuner applies the KrakenSDR's
  measured curve - 44.5: -3.2, 38.6: -7.9, 29.7: -15.6, 22.9: -21.2, 16.6:
  -27.8, 12.5: -31.9, 8.7: -34.2, 7.7: -35.7 dB): the 52 dB carrier at its
  hop's 8.7 dB reads +6.3 dB over its true height without the curve (the
  list's error there: 6.7) and -0.3 with it once heimdall publishes it
  after the first sweeps; the reduced hop's floor 21.7 dB over the others
  for 22.2 predicted from the real gain; five malformed curves ignored (one
  point, falling gains, a string, a string value, unclosed); manual gain
  with the curve unchanged.
- Seam steps, round 10 (2026-11-24): a tuner pair's steps taken from hops at
  the ceiling on both sides are kept (`S.seam_hist`, the last 4 completed
  sweeps'); a side without one -
  heimdall's search sweep, a block reduced for good - uses the kept step
  instead of falling back to all hops (that folded the reduced hops'
  pedestal, or the units' curve error, into the next block). Only a seam
  never seen at the ceiling on its grid takes all of its hops. The kept
  steps belong to the tuner pairs on the ranks: a new grid, other sweep
  tuners or other hops for them (`f0`, `H`) clear them (a dwell change
  doesn't); up to ca7d6da (the review's find) a tuner change kept rank 0's
  old pair's step (tuner 0 | 1, +3 dB) for the new pair (tuner 0 | 2, -2
  dB) while tuner 2's block was reduced for good - `t_seamtun.py`: blocks
  6.59 / 1.95 / 9.42 / 9.49 dB for manual's 2.28 / 2.32 / 2.24 / 2.32. The step is
  the dongles' and doesn't move with a gain. The pre-existing 719 | 721 MHz
  step and the FM block's offset between runs are untouched by this (both
  from manual, where every hop is at the ceiling: a signal or a captured
  hop beside a seam, the 6 dB cap).
- Seam steps, the median (2026-11-24, the review's open finding - `t_agc
  seam` on the Pi, a block off by 1 - 6.6 dB now and then): the step used
  is the median of the ceiling steps of the last 4 completed sweeps and of
  the evaluation's own (`SEAM_HIST`); a step past the 6 dB cap is no
  evidence (the median of the kept ones is used), no longer 0. The base
  (the reference's 20th percentile) likewise: the median of the last 4
  completed sweeps' and now's (a round whose visits all came out short
  moved it for an evaluation). A change of the common gain (the Gain
  slider; with Auto heimdall's ceiling) or Auto switched on or off moves
  every hop at once, and the steps with it (the converter's share of a
  floor changes with the gain): both histories start again then
  (`Store::gain_changed`), and with fewer than 2 kept sweeps now's value is
  used alone - the sweep the change fell in holds hops from before it - so
  the picture settles in the first sweep at the new gain as on round 9's
  build (`t_gainstep.py`, manual 49.6 -> 37.2 -> 49.6, the clean hops'
  level from the change: 66b8298 off by up to 8 dB for 3 sweeps, now and
  round 9 only in the sweep the change fell in). A seam with no step of its
  own at the new gain yet (a side without a ceiling hop) keeps the median
  from before the change. Spikes of the base over 0.5 dB, x86
  under load with the log: round 9 4.9 a minute, c419b4b 4.1, the step
  median alone 0.8, with the base median 0 in 3 runs - and 0 step spikes).
  Found with
  `KRAKEN_SWEEP_SEAM_DEBUG=1` (every evaluation logs, to stderr, each
  seam's hops `h:floor@gain` with `*` at the ceiling, where the step came
  from - C, M, A, - -, the step and the raw ceiling difference; then the
  base and the offsets): one hop's odd floor for one visit - a short visit
  3.5 dB low, or a hop clipped at the ceiling whose floor reads -64 dB -
  made one evaluation's step 0 (past the cap) or bent it by its own error;
  the whole chain of blocks after that seam, and the base, moved with it
  for that evaluation (~80 ms at 12 sweeps a second), the rows were
  rewritten, and a measurement that caught it saw a block 1 - 5 dB off.
  Load makes the odd visits more frequent (the Pi, x86 pinned to 2 busy
  cores). The mechanism is round 9's (the
  same step code; a round-9 build with the same log, x86 pinned to 2 busy
  cores: 10.1 isolated step spikes > 1 dB a minute; c419b4b 8.1; with the
  median 0.8); `t_agc seam` only fails when its measurement catches one
  (x86 under load, 10 runs each: round 9 10/10 green, c419b4b 10/10, the
  median 10/10; the Pi: round 9 10/10, c419b4b with the log 9/10 - the
  failure a glitched manual reference -, earlier c419b4b runs 4 failures in
  6). The median also takes the manual clipped hop's error away (`t_agc
  seam`, hop 5 hot in manual: tuner 1's block +4.7 dB before, within one
  level step now). Final build (9b64106, with the restart on a gain
  change), `t_agc seam`: the Pi 10/10 and 3/3 with the log; x86 under load
  with the log 17/20 - the three misses each one block 1 - 2 dB low while
  the reference stood still: the row of one odd visit itself (hop 7's
  floor 3.6 dB under its others for one visit, in the block's measurement;
  a row is its hop's last visit - as in round 9, whose logs show such
  visits at the same rate). The spikes left (x86 1.8 / 1.4 a minute, the
  Pi 3.0 / 1.8 for steps / base) all fall where the test switches the
  scene and Auto: the histories restart there and the first 2 sweeps use
  now's value alone, as round 9 always did. Round 9b's hardware oddities, from the log's point of
  view (not yet looked at on the hardware): a seam whose sides differ by
  more than 6 dB - a signal on every hop within 6 of one side, as in the
  700 MHz LTE bands - was and is shown uncorrected (the ~11 dB floor step
  at 719 | 721 MHz fits that: the cap, not a wrong step); one whose
  difference sits near the cap used to flip between the step and 0 from
  one evaluation to the next, and now keeps the median of its in-cap steps.
  The FM block's offset, bistable by about +-5 dB between runs, fits a
  seam beside it whose difference lies over the cap in one run and under
  it in another (the band's signals set the lowest floors): the median
  steadies it within a run, not between runs.
- Seam steps with auto gain (history: 2026-11-23, after the orchestrator's question
  whether reduced hops fool them): `evaluate` takes a tuner pair's step only
  from hops at the ceiling (their gain = the common one) on each side of the
  seam, and falls back to all of a side's hops only when none of them is.
  Why: the step is the difference of the LOWEST floors within 6 hops each
  side; a reduced hop's floor is usually a pedestal (the converter's floor:
  higher, never the lowest), but where it is the tuner's noise and the gain
  curve states too little of the reduction, it is the lowest and moves the
  whole next block. `t_agc.py seam` (16 hops on 5 tuners whose dongles
  differ by 0 / 3 / -2 / 1 / 4 dB; a carrier 52 dB over the noise in hop 5,
  the LAST of tuner 1's block, then in hop 6, the FIRST of tuner 2's): Auto
  with the list, with the curve, with a curve 13 % short, with a converter
  far quieter: every block within one level step of manual without the
  signal - 83760a1 the same (the pedestals are never the lowest); a 20 dB
  reduction (29.7 dB, a carrier 34 dB over) with a curve stating half of
  it: 83760a1 moved tuner 1's block 4.2 dB and tuners 0 - 1 6.1 dB, this
  rule keeps every block within 0 dB. Manual itself is fooled by a clipped
  hop at a seam: the carrier captures the converter, the hop's floor drops
  and is the lowest (blocks 1.9 - 4.7 dB off in the test) - Auto removes
  that cause.
- Auto gain (history: 2026-11-23 - removed 2026-11-24): `sweep::set_auto_gain` / `auto_gain_command`,
  `ControlHandler::send_sweep_plan_now` (the plan, then the switch: every
  place that sent the plan; `ask_sweep_auto_gain` reads what heimdall sent
  before the ask first - a status line of the heimdall before would answer
  it); heimdall's answers through
  `sweep::ingest_control_reply` (data_receiver's drain of the control
  socket, ~1 s). The engine: the slot carries each channel's header gain,
  the accumulator the gain of its hop's packets; `commit` shifts the hop's
  dB values and its floor estimate by (ceiling - gain) before the floor's
  percentile is used (the dead-tuner check stays on the raw floor); `store`
  keeps the hop's gain; at a completion `update_gain_map` builds the map,
  its counts, and (Auto on) the ceiling. The server sends the map when its
  version moves and the state when its counts change (the broadcast tick).
  - Measured with the fake heimdall (`t_agc.py`; a scene: tuner noise 6
    counts at the ceiling 49.6, the converter's own 1.5, clipping at 0 /
    255; a carrier 34 dB (amp 400) and one 52 dB (amp 3 500) over the others
    in hop 7, which hop 6's tuner also sees at its band edge; the fake's rule
    is heimdall's (f3ca686): -18 / -9 / -4.5 dB for a clipped share > 1 % /
    > 0.1 % / > 0.01 %, +3 dB after 8 clean visits and 20 s with 6 dB of
    headroom, the hold doubling after a raise that clips; the tests shorten
    the hold to 4 visits, 0 s for the way back):
    - manual: the clipped hop's carrier 8 / 27 dB short of its height (the
      converter clipped: 59.8 dB for 68.0 / 86.9 expected), and its floor
      estimate off (the carrier captures the converter: the noise under it
      is suppressed - the hop's median fell below the others in most runs);
    - Auto: no clipped hop from sweep 2 - 4; steady at 29.7 / 8.7 dB for
      hops 6 and 7, the rest at the ceiling (heimdall on the real antenna:
      415.1 MHz at 8.7 dB); the map = the headers' gains (= the fake's), its
      counts in the `sweep` message; not sent again while steady;
    - the levels: the quiet hops as in manual (within one level step); the
      carrier at its true height referred to the ceiling (67.8 dB for 68.0
      expected at 29.7 dB; 86.6 for 86.9 at 8.7 dB); the reduced hop's floor
      8.0 / 28.7 dB over the others for 8.5 / 28.9 predicted from the two
      noises - the sensitivity lost at that gain, the converter's floor
      dominating (a 41 dB reduction leaves ~29 dB of it);
    - the signal gone: the gain back at the ceiling after 20 / 41 sweeps
      (+3 dB a step, 11 steps from 8.7 dB), the hop's floor with the others
      again, the counts 0 - nothing sticks;
    - a heimdall without the command: `auto_gain_known` false, active
      false, the setting kept, sweeps go on at the common gain; a restart
      replays the setting, heimdall gets it after the plan; a data-port
      reconnect sends both again. The
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
  arrays (allocated on the loop thread, never per packet) - only while the
  mode runs: a receiver that never sweeps holds none, and leaving the mode
  gives them back (since 2026-11-21; *Finer resolutions*).
- FFT size: `SWEEP_RES:N` takes N unless the sweep would hold more than
  `SWEEP_MAX_BINS` (2^24, 16.8 M; 2^22 until 2026-11-21) bins or N is more
  than a hop's samples at the dwell - then the largest N that fits (the
  full range at 32768 runs at 16384: 11.9 M bins); `fft_size` says what
  runs, `cap` why.
  `auto` = the largest N whose sweep holds at most `SWEEP_AUTO_BINS` (2^19)
  AND whose hop still averages `SWEEP_AUTO_FRAMES` (8) frames at the dwell.
  A hop's samples are `ceil(dwell x 2.4e6 / (1000 x 16384))` whole packets
  of 16384 (`hop_samples`): exactly what heimdall delivers per tuner and hop
  since 2026-11-21 (`heimdall_v2/docs/dev/operating-modes.md`, *Hop
  timing*). Until then the model was `max(16384, dwell x 1200)`, half of
  what the dwell holds: heimdall delivered 1.3-1.6 valid packets per hop at
  dwell 20 (its flushes; the hardware run of 2026-11-20). Frames overlap by
  half (Hann) where a hop would give fewer than 16 frames of its own;
  `"avg"` in the `sweep` message (additive) = the frames a hop is expected
  to average. Auto at dwell 5 / 10 / 20 / 50 / 100 ms (1 / 2 / 3 / 8 / 15
  packets): 2048 / 4096 / 8192 / 16384 / 16384 (15 / 15 / 11 / 15 / 29
  frames; with the old model 2048 / - / 4096 / 8192 / 16384); with the
  span: 512 for the full range (4.7 kHz bins, 32 frames at dwell 5). With
  an older heimdall (wall-time dwell: 1.3-1.6 packets at 20 ms) this model
  overestimates a hop - Auto plans for 3 packets and a hop then averages
  3-7 frames instead of >= 8 (noisier, it works); not detected, the two
  ship together. The dwell
  re-evaluates it (a new N is a new grid: the pages drop the waterfall).
  Why: the sweep's speed doesn't depend on N (the dwell and the hops do),
  the resolution and the averaging do. The first rule (span only) chose 16384
  for 88-108 MHz: 1-2 frames a hop, the trace speckled and the waterfall
  banded row to row on the KrakenSDR (hw_3_fm_band.png). Measured on the fake
  with 1-2 packets a hop: the noise's spread (sd of its dB, one bin a point)
  1.21-1.22 dB at auto (4096, ~10 frames) vs 2.45-2.52 dB at 16384 (1-2
  frames), its median 1.4-2.4 dB in both. 2^19 bins are ~128 per point of a 4096-point
  plot, so every zoom down to 1/128 of the span still gets one bin per point.
  A user who wants 146 Hz sets the size (and raises the dwell; `avg` says 1
  at 16384 / dwell 20).
- **Finer resolutions** (2026-11-21; the user: "the full band sweep of 24 MHz
  - 1.766 GHz tops out at 586 Hz"). The cap was `SWEEP_MAX_BINS` = 2^22: the
  whole range is 911 hops, 2.97 M bins at 4096, 5.94 M at 8192, 11.9 M at
  16384. Nothing else in the engine or the frames depends on the bin count
  (the pyramid makes a frame a few node reads a point whatever the bins;
  every index is 64-bit). Now: 2^24 (16.8 M bins; the whole range at 8192
  and 16384), and N up to 65536 across the packets of a hop - a frame's
  samples already went through `pend` across a packet boundary, so the only
  new rule is N <= a hop's samples at the dwell (`fft_size_for`; 32768 from
  dwell 7 ms, 65536 from 21 ms). The same 2^24 bounds them: 32768 up to
  642 hops (24-1250 MHz), 65536 up to 321 (~614 MHz of span). Auto is
  unchanged (it starts at 16384: `SWEEP_AUTO_FFT_MAX`; t_res.py compares it
  with the old rule for 48 range / dwell pairs). The `sweep` message says
  why a size is capped (`cap`, `dwell_ms_needed`).
  - The store: ~4 bytes a bin (int16 bins + their max pyramid; bench 49 MB
    for 11.9 M bins), made on the loop thread outside `g_mu` - the old one
    freed first, so two never exist together (the worker, meanwhile, stores
    nothing; no frame is built). A grid change answers in 6-14 ms (x86 and
    Pi 5 alike; the bench: 2.3 / 4.9 / 10.0 ms for the whole range at 4096
    / 8192 / 16384 on the Pi 5, 12.5 ms for 16.4 M bins). Leaving the mode
    gives it back (before: kept until the next grid; ~16 MB at the old cap).
    On Linux its arrays get their own pages (`BigAlloc`: mmap / munmap):
    with glibc's heap, whole range 4096 -> 8192 -> 16384 -> 4096 held 69 MB
    instead of 57 and leaving the mode then held all 91 MB (its mmap
    threshold rises with every big block freed, so later stores landed in a
    thread arena whose freed top is never returned; malloc_trim did not
    help). Elsewhere the C library maps blocks that big itself.
  - The receiver's RSS (t_res.py mem; x86 / Pi 5): coherent mode, never
    swept 45.6 / 51.3 MB; whole range at 4096 56.8 / 64.3, 8192 68.3 /
    76.0, 16384 91.7 / 99.6; back to 4096 57.8 / 65.7; out of the mode 47.2
    / 54.6; a small grid (400-430 MHz) 47.6 / 55.0; 100-700 MHz at 65536
    (16.4 M bins, near the cap) 114 / 117 MB.
  - CPU (t_rescpu.py: the receiver, % of one core, 5 tuners, one page
    zoomed out at 1600 points / zoomed in to 50 kHz; the fake heimdall at
    real time; tb_ov = before, where 16384 ran at 4096):

    | range, N, dwell | x86 | Raspberry Pi 5 |
    |---|---|---|
    | 24-1765, before (16384 -> 4096), 5 ms | 4.9 / 4.3 | 9.2 / 8.0 |
    | 24-1765, before (16384 -> 4096), 20 ms | 6.2 / 6.0 | 12.8 / 11.7 |
    | 24-1765, 4096, 5 ms | 5.3 / 4.4 | 9.1 / 8.0 |
    | 24-1765, 8192, 5 ms (3 frames a hop) | 6.6 / 5.6 | 10.8 / 9.7 |
    | 24-1765, 16384, 5 ms (1 frame) | 7.8 / 7.4 | 13.3 / 12.4 |
    | 24-1765, 8192, 20 ms | 7.3 / 7.4 | 14.6 / 13.4 |
    | 24-1765, 16384, 20 ms (5 frames) | 8.5 / 7.9 | 16.7 / 15.6 |
    | 100-700, 32768, 10 ms (1 frame) | 10.7 / 11.0 | 18.9 / 18.7 |
    | 100-700, 65536, 25 ms (1 frame) | 14.2 / 13.5 | 25.8 / 26.9 |
    | 100-700, 16384, 25 ms (7 frames) | 8.6 / 9.3 | 16.9 / 16.6 |

    The worker thread is most of it (a hop's commit costs per bin: log10,
    the 10th percentile, the pyramid - the bins a second are the same at
    every N, the FFT and the cache misses grow with N). One packet dropped
    (of ~4400) on the Pi in the 65536 run: a hop's frames are done at once
    when it ends (5 tuners x a 65536 FFT + 52 k bins). Engine bench
    (`bench/bench_res.cpp`, CPU a packet of 5 x 16384 samples; x86 / Pi 5):
    4096 627 / 1206 us, 8192 857 / 1533, 16384 1192 / 1779 (one packet a
    hop), 32768 1176 / 1789, 65536 1095 / 1602 - at real time (146
    packets/s) 9-17 % / 18-26 % of a core.
  - `build_frame` at 11.9 M bins (bench, with a 1600-point overview each;
    x86 / Pi 5): the whole range at 4096 points 165 / 404 us, at 1600 70 /
    103 us, a 100 MHz window 126 / 301, 1 MHz 81 / 136, one bin a point
    70 / 119 us.
  - Levels (t_res.py levels = t_sweep's grid checks at 400-430 MHz, x86 and
    Pi 5): at 8192 / 16384 / 32768 / 65536 (dwell 25: 15 / 7 / 3 / 1
    frames), 16384 at dwell 5 and 32768 at dwell 10 (one frame) - six
    carriers on their point (within half a bin: 36.6 Hz bins at 65536) and
    height, the noise median at 1.88 dB (one level step under 2) for every
    size, the floor flat over hops and the five tuners (spread <= 0.47 dB),
    edges vs centre, the band 10.35-10.82 dB (10.41). With one frame a hop
    a carrier stands 1.6 dB higher over the noise median than with many
    (the noise's median is 1.59 dB under its mean at one frame and is what
    sits at +2 dB - as at 16384 / dwell 5 before); the test expects it. No
    DC artefact at the hop centres at one frame (t_dcprobe.py, 12 sweeps:
    the bins within 6 of the centres as the bins 300 kHz away - median,
    90th / 99th percentile, 33 / 28 % at 0 dB); the check's limits were the
    many-frame ones (a max of 39 one-frame bins passes 8 dB ~40 % of the
    time) and are now set by the frames.
  - Tests (fake heimdall; x86 normal and ASan + UBSan, Pi 5 normal):
    `t_res.py` plan 102 / 102 (the size used, `cap`, `dwell_ms_needed`,
    `sizes` / `sizes_capped` / `min_dwell_ms` for 6 ranges x 8 dwells x 7
    sizes; auto against the old rule), levels 141 / 141 (Pi 5: 3 x 49),
    mem 10 / 10; `t_sweep.py` 214 / 214 (its expectations moved: 99999 ->
    65536, the whole range at 16384 now runs at 16384, 32768 there is
    capped by the bins); overview 23 / 23, `t_ovcompat.py` 3 / 3, edge 50,
    fix 29, modes 15, MCP 9. `t_fix.py dead` sits at the edge of its limit
    (2.82 dB usually, 3.29 > 3.2 in 1 of 5 runs - also on the build before
    this change with the same fake): not from this change.
  - The fake heimdall's carriers are phase-continuous across packets now
    (each packet restarted the phase: unlike a dongle, and a frame past one
    packet - or an overlapped one - smeared the carrier).
  - A packet missing inside a hop: one the receiver drops (the ring full)
    now marks the next one, and a frame begun before the gap is dropped
    (the frames already summed stay; at one frame a hop that hop keeps its
    last sweep's values). One heimdall never sends can't be seen (the 8091
    header carries no packet count; heimdall reports an L1 overflow as a
    coherence loss).
  - The memory on a 1 GB Raspberry Pi 4 (not measured: .62 is a Pi 5,
    8 GB): the largest grid is 2^24 x ~4 bytes = 64 MB more than not
    sweeping, the whole range at 16384 48 MB, never two at once. That is
    7 % of the Pi 4's memory, on top of kraken_doa's own 51 MB (Pi 5,
    before sweeping; heimdall's not measured here). If small machines need it, a rule (not
    built): the cap = min(2^24, MemAvailable at the grid change / 16 / 4
    bytes), reported as `cap: "bins"` like the fixed cap.
- Overlap's cost (bench, x86, 5 tuners x 16384-sample packets): 394 us of CPU
  a packet at N 4096 without overlap, 714-853 us with it (~2x: twice the
  FFTs), 230-390 us at N 128 / 1024 (no overlap there at dwell 20). On a Pi
  the overlapped case is the heavy one (not measured): auto only uses it
  where a hop holds fewer than 16 frames - the full range at auto doesn't.
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
  frequency is within 1.5 kHz of that hop's centre AND the hop lies in that
  tuner's block (the contract's assignment: the k-th tuner taking part, k <
  K'; a tuner that stays put for a round is on its block's last hop - measured
  again). Frequency 0 (heimdall: "the retune failed, no valid frequency"),
  NaN / inf or off the grid: not used. Found by the review: a selected tuner
  heimdall doesn't use (K' < selected) is never retuned and may sit on
  another tuner's hop centre - 433-436 MHz on tuners 3,4, then 0,1,2 added:
  3 and 4 stayed on the centres and, committed after 0 and 1, overwrote both
  hops every round (and the gain steps found no seam). A hop change commits
  the tuner's old hop. Samples: N-sample blocks (a block may start in one
  packet and end in the next; 50 % overlap as above), each block's mean taken
  off (the RTL's DC offset), Hann, pffft (`kfft`), |X|^2 added DC centred. A
  hop that never ends (another control client paused heimdall, this one
  didn't) is committed every 2.4 M samples and measured again (the sums grew
  without bound). A hop whose 10th percentile is below -100 dB (a tuner
  delivering constant samples - a dead dongle: the mean removal leaves
  nothing, ~-200 dB) is no measurement and not stored: 20 % of such hops
  dragged the reference to ~-194 dB. Such a hop also leaves the reference
  with what it measured before (the ASan run caught the hop measured as the
  dongle died - part live, part constant samples: the noise elsewhere showed
  at +4.9 dB; the seam steps take the lowest floor next to a seam).
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
  Wilson-Hilferty above, both checked against scipy). K frames with 50 %
  overlap spread like Gamma(K) at these two quantiles (simulated, numpy, 1 ..
  20 frames: within 0.06 dB - a variance-based "equivalent" m was 0.2-0.4 dB
  off), so m = the frames counted. So the noise's median
  is drawn at +2 dB (like the regular spectrum's ~2 dB) for every FFT size
  and dwell; without it the noise moved from 0.5 dB (N = 128) to ~5 dB
  (N = 16384, m = 3). Hops of different m level too: on the KrakenSDR a hop
  held 1-2 valid packets at dwell 20 (heimdall's addition 5; exactly 3 since
  2026-11-21, though a stalled stream can still end a hop short), m = 1 or 3
  (overlapped) at N = 16384. The hop's floor phi is the noise's MEAN (its
  median estimate less `median_db(m)`, the median of Gamma(m, 1/m) in dB:
  -1.59 at m = 1, -0.5 at 3), the reference and the gain steps are taken from
  those, and each hop is stored against base + step + `median_db(m)` - 2 dB,
  so its noise median shows at +2 dB whatever its m. Found in the fix round
  (fake heimdall with 1-2 packets a hop at random): with one reference for
  all m the hop medians stood 1.88 .. 3.29 dB (m = 3 hops higher); now
  1.88 .. 2.35 (one level step). Cost: a carrier in a hop of m = 1 reads
  1.6 dB higher over the noise median than one in a hop of large m (its power
  isn't Gamma), and with m = 1 ~35 % of the noise falls under the 0 dB line.
  A cap at the 10th percentile was tried and dropped (it put hops of m = 1
  and 2 ~3 dB apart). One packet (16384 samples = the largest N) always makes
  at least one frame.
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
  of a block seam) becomes the floor. A design limit too: a carrier within
  +-1 bin of a hop centre is replaced by the DC fill (its neighbours' mean).
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
- The overview (2026-11-21): `handle_sweep_overview` (websocket_server.cpp)
  keeps the points per socket (`PerSocketData::sweep_overview`);
  `send_sweep_views` asks `sweep::build_frame(view, completes, points,
  &overview)` for both frames and sends the overview first. `build_frame`
  cuts the overview as a whole-range view (`cut_for(0, 0, points)`) inside
  the lock it takes for the frame (`frame_locked`, the frame builder both
  share). Cost (x86, load 2.5-3.3, `t_ovcpu.py`, % of one core, the
  receiver; the views zoomed, overview 1600 points; no packet dropped):

  | range / FFT | 1 page off / on | 4 pages off / on | messages a page off / on |
  |---|---|---|---|
  | 24-1765 MHz, auto (512) | 2.6 / 2.5 % | 3.1 / 3.0 % | 26 / 27 (a sweep every 3.7 s) |
  | 24-1765 MHz, 16384 -> 4096 | 4.5 / 4.7 % | 5.6 / 5.4 % | 27 / 27 |
  | 88-108 MHz, auto (2048: 16 sweeps a second) | 3.9 / 3.9 % | 3.9 / 4.2 % | 26 / 43, 21.5 / 33.3 KB/s |

  The overview is one more frame per completed sweep and page: within the
  noise of the measurement in CPU; in bytes, at a fast sweep, +12 KB/s a
  page at 1600 points. On the Raspberry Pi 5 (24-1765 MHz, auto): 4.7 /
  4.7 % with one zoomed page, 6.1 / 6.1 % with four. Tests (fake heimdall, x86, normal and ASan + UBSan
  builds): `t_overview.py` 23 / 23 (only asked-for and zoomed, the pairing
  and order over 61 sweeps, the header, n / first point / spacing against a
  whole-range view, 60 of 61 sweeps byte-identical to a whole-range page's
  frame of the same sweep - the one other differs in 50 of 1004 points, a
  hop stored between the two pages' builds, not between the two frames of
  one page -, several sockets at 300 / 1000 / 2000 points, the clamps, junk,
  0 = off, a view change, zooming out, pause); `t_ovcompat.py` 3 / 3 (a page
  that never asks gets the same frames from this receiver as from 3919d64 -
  every header field and size, the same count -, and 3919d64 ignores the
  command); the earlier suites unchanged (sweep 213, edge 50, fix 29, modes
  15, MCP 9). Note: a page that sends its view as the explicit whole range
  (not `0:0`) gets flag 1 (the window is clamped to the range) and so an
  overview of the same content; a page zoomed out sends `0:0`.
- The pooled sets (2026-11-22, `SWEEP_FULL:<n>`): `sweep::full_pool(n)` /
  `pool_for` (sweep.cpp) give the pool; `build_fulls(pools)` converts each
  pool's level of the pyramid as the full set's level 0, every pool of a
  tick under ONE lock (the sets of one tick are one state of the sweep
  whatever their pool - before that, an ASan run saw a pooled set differ
  from the full set of the same `sweeps`: a hop stored between two builds);
  `send_sweep_views` collects the pools whose sets start in the tick, then
  `pump_full` sends. The lock held (bench, 20 sets of the whole range at
  16384, 11.9 M bins; x86 under load / Pi 5): full 4.3 / 8.7 ms, pool 4
  (2.97 M values) 0.96 / 2.2, 16 0.19 / 0.54, 64 0.05 / 0.14, 256 0.012 /
  0.034, 1 024 0.003 / 0.009 ms; several pools in one tick add up. Bytes a
  set: 11.6 KB / 46 KB / 186 KB / 743 KB / 2.97 MB / 11.9 MB; a second at
  the 3.75 s sweep 3.1 / 12.4 / 50 / 198 / 792 / 3 170 KB (measured: type
  11 3.5 / 13.7 / 36 / 218 / 870 / 3 482 KB/s within a 10 s window).
  CPU (t_drescpu.py: one normal page and one at each step, then two pages
  at two steps; % of one core; no packet dropped):

  | A / B | x86 (under load, +-3 %) | Raspberry Pi 5 |
  |---|---|---|
  | off / off | 14.8 | 15.3 |
  | 16384 / off | 14.0 | 15.6 |
  | 65536 / off | 14.6 | 15.7 |
  | 262144 / off | 11.2 | 15.6 |
  | 1048576 / off | 9.5 | 15.9 |
  | 4194304 / off | 14.4 | 15.8 |
  | full / off | 14.1 | 17.7 |
  | full / 65536 | 14.1 | 17.4 |
  | 16384 / 4194304 | 13.6 | 15.7 |
  | off / off (again) | 13.8 | 15.4 |

  On the Pi 5 every step below full costs at most 0.6 % of a core; full
  2.4 %. Tests (`t_dres.py` 34 / 34, x86 normal and ASan + UBSan, Pi 5):
  `full_sets` for six grids against the rule; for every offered step and
  n = 2, 1 000, 50 000, 3 000 000, bins and 2^32 - 1 (paused) the set's
  pool, count, Hz of value 0 and spacing, and its levels = the full set of
  the same store max-pooled, byte for byte (whole range at 11.9 M bins and
  400-430 MHz); four sockets at n 1 / 65 536 / 16 384 / 4 194 304 at once,
  each set = the full socket's set of the same sweep pooled; a change of n
  mid-set (a paused reader, 64 KB receive buffer): the set in flight stops
  incomplete, then only sets at the new pool; the same n again: no
  snapshot; another n: a snapshot at it first.
- The full set (2026-11-21, `SWEEP_FULL`): `sweep::build_full` converts
  the store's level 0 to u8 in one pass under `g_mu` (`level_u8`: exactly
  build_frame's rounding - checked for all 65 536 values - in a loop the
  compiler vectorizes; a 65 536-entry table was 2x slower on x86, 1.4x on
  the Pi 5), into a buffer made before the lock and shared (`shared_ptr`)
  by every socket that sends it, built at most once a tick when a socket
  starts a set. `pump_full` (websocket_server.cpp) sends parts while the
  socket has less than 1 MiB queued - every tick after the type 10 frames,
  and from the socket's `drain` event. One lock, so a set is one state of
  the sweep (its `sweeps` and its levels read together).
  - The engine's stall = the lock held for a set (bench, 20 sets; x86 /
    Pi 5): whole range at 16384 (11.9 M bins) 2.5 (worst 3.3) / 8.5 (8.6)
    ms; 100-700 MHz at 65536 (16.4 M) 3.8 / 11.9 ms. The worker's ring holds
    ~55 ms of packets: no packet dropped in any normal-build run, x86 or Pi
    5. A 1 GB Pi 4 (not measured; ~2x slower than a Pi 5 here) would hold it
    ~17-24 ms. The ASan + UBSan build (several times slower) dropped 9
    packets during the 11.9 M-bin sets. The 23 parts are made in 0.8 / 1.6
    ms; 11.9 MB arrives on localhost in 10-45 ms.
  - Memory: one buffer of `bins` bytes per set (11.9 MB at 146 Hz, 16.8 MB
    at most), alive while a socket still sends it - the latest and any a
    slower socket is still on; released out of the mode.
  - Compressed for a page that asks (`SWEEP_FULL_Z`, 2026-11-23; until then
    not: deflate's default strategy on the fake's scene, 57 % for 291 ms a
    set on the loop, did not pay). zlib, `Z_HUFFMAN_ONLY`, windowBits 15,
    memLevel 9, one stream a part, on a compressor thread (`z_main`,
    sweep.cpp): the levels of one FFT a point are noise to an LZ search -
    Huffman alone is at the bound for this data and the fastest. On the
    REAL sets of the PC's KrakenSDR (job scratch `e2e/sets`; bench
    `bench/zbench.cpp`, this exact call; x86 / Pi 5): the whole range at
    146 Hz 60.1 % in 82 / 210 ms (145 / 56 MB/s; inflate 40 / 63 ms),
    dwell 21 ms 51.6 %, 586 Hz 55.0 % in 20 / 48 ms, Auto 55.0 % in 2.5 /
    6.3 ms, the FM band 58.1 % in 0.12 / 0.32 ms - every part inflating back
    byte for byte; a part of uniformly random values does not shrink and
    goes raw. Default strategy level 1: 66.4 % at a third of the speed
    (the orchestrator's study, `e2e/comp_*.py`).
  - The mechanics. After the tick's `build_fulls` (the lock as before) the
    sets that a deflating socket starts are handed over (`z_submit`): their
    per-part slots are made on the loop thread, the compressor fills them
    part by part (`zpart[k]`, then `zstate[k]` = READY or RAW with release;
    the loop reads with acquire) - the deflated copies live and die with
    the set's buffer. A newer hand-over replaces what the compressor has not
    done (it marks the old sets dropped): no queue. The sets of a hand-over
    go smallest first and each deadline counts the bytes handed over ahead
    of it (the review of 2026-11-23: in build order, a page at Full ahead of
    one at a step held the step's part 0 past its deadline - 11.9 MB, 210
    ms on a Pi 5 - and that page's sets went raw while the other was at
    Full; a step is at most a quarter of Full, a few ms). Out of the mode
    `z_submit({})` drops the work. `pump_full` sends part k of a deflating
    socket deflated when READY; raw when RAW (not smaller), when the set
    was dropped, when the part before it is still pending (the compressor
    is behind: the rest of the set goes raw at once), or past its deadline
    = the hand-over + 50 ms + parts 0..k at 32 MB/s (half the Pi 5's
    rate); else the socket waits - the compressor wakes the loop
    (`loop->defer`, coalesced) as each part is ready. So a set is late by
    at most about a tick plus its own compression: measured 0.08 s (x86) /
    0.15 s (Pi 5) after the raw page's for 11.9 M bins; with a compressor
    slowed to 300 ms a part (`KRAKEN_DOA_TEST_Z_DELAY_MS`, a test hook)
    every part goes raw, every set complete, 0.09 s late.
  - What it saves and costs (t_zipcpu.py: the whole range at 146 Hz; one
    normal page and one full page raw / deflating; % of one core; no packet
    dropped): x86 8.6 / 11.3 %, Pi 5 15.8-18.8 / 24.0 % - the compressor
    is ~5.6 % of a Pi 5 core at a 3.75 s sweep (210 ms a set). The wire: the
    fake's scene goes to 47.9 % (a step: 38.8 %), the real 146 Hz set to
    60.1 % - 3.5 MB/s raw -> about 2.1 MB/s on the user's KrakenSDR. 586 Hz
    (2.97 M): x86 5.5 / 6.4 %, Pi 5 11.2 / 13.3 %; 400-430 MHz at 16384
    (12 sets a second): x86 8.0 / 9.8 %, Pi 5 15.3 / 18.3 %.
  - Tests (`t_zip.py`; x86 normal 25 / 25, ASan + UBSan the same but the
    "no packet dropped" checks (that build drops engine packets during
    full sets, as before), Pi 5 25 / 25): a deflating and a raw socket,
    full and a step, get the same sets byte for byte after inflating; every
    deflated part one complete zlib stream inflating to `n`, smaller than
    raw; an older socket's parts raw, `n` bytes as before (and
    `t_ovcompat.py` against 3919d64); a part deflated exactly when
    zlib makes it smaller (sets of 54, 107 and 51 201 values); the slowed
    compressor: raw, complete, not late; mid-set with it busy:
    `SWEEP_FULL_Z:0`, `SWEEP_FULL:65536`, a grid change, pause, leaving the
    mode and coming back; deflating pages at Full + 65 536 and at Full +
    65 536 + 4 194 304 at once: every part of every set deflated, each the
    raw full page's set of the same sweep pooled - x86 and Pi 5, where the 4 194 304 step (2.97 MB, ~50 ms) goes ahead of Full and Full's part 0 deadline counts it (161 ms); SIGTERM while a
    set is compressed and sent (the slow compressor): exit 0 after 2.4 s, no
    crash (normal and ASan + UBSan builds). TSan builds and runs (`setarch -R`: this kernel's
    address layout), but its mutex tracking is broken here - it reports
    "double lock" on plain lock_guards and races between two threads
    holding the same mutex all over the program; none of its ~120 reports
    names the compressor or its pump, which with that noise proves little.
  - CPU (t_fullcpu.py, the receiver, % of one core; one normal page plus 0
    / 1 / 2 full-resolution pages; then 0 again; type 11 KB/s a full page;
    dwell 5, 5 tuners, the fake at real time; no packet dropped):

    | case | x86 | Raspberry Pi 5 | wire, a full page |
    |---|---|---|---|
    | 24-1765 MHz auto (0.37 M bins, 3.75 s) | 2.9 / 3.1 / 3.2 / 3.2 | 6.1 / 6.1 / 6.1 / 6.1 | 122 KB/s |
    | 24-1765 MHz 4096 (2.97 M) | 5.8 / 5.6 / 5.7 / 5.5 | 11.1 / 11.6 / 12.1 / 11.4 | 883 KB/s |
    | 24-1765 MHz 16384 (11.9 M) | 8.5 / 8.5 / 8.7 / 8.1 | 15.3 / 17.2 / 18.8 / 15.5 | 3.5 MB/s |
    | 88-108 MHz auto (17 k bins, 16 sets/s) | 3.9 / 4.0 / 4.0 / 4.0 | 8.1 / 8.4 / 8.5 / 8.2 | 291 KB/s |
    | 400-430 MHz 16384 (205 k, 12 sets/s) | 7.9 / 7.7 / 7.9 / 7.4 | 13.1 / 14.2 / 15.2 / 13.1 | 2.5 MB/s |

    So no bin cap and no rate limit: the heaviest case costs a Pi 5 ~1.7
    % of a core per full page (sending 3.5 MB/s); the wire is the limit on a
    slow link (the page's Slow link wins).
  - Tests (`t_full.py` 31 / 31, x86 normal and ASan + UBSan, Pi 5): a
    complete set at once on `SWEEP_FULL:1` (the snapshot; or the tick's
    completion set); one set per completed sweep, after its flag-2 frame,
    the same `sweeps` (48 of 48), ids + 1; a second socket the same sets
    byte for byte; a socket that never asked none (and `t_ovcompat.py`:
    its type 10 stream as from 3919d64); paused, the snapshot flags 2 | 4,
    and the set against the store's views at the same instant -
    max-pooled = the whole-range views of 4096 / 1000 points exactly, the
    windows at one bin a point = its bins (400-430 MHz, and the whole range
    at 11.9 M bins in 23 parts); `SWEEP_FULL:0` / junk / `:1`; none out of
    the mode, sets again back in it; a grid change mid-set (a reader with a
    64 KB receive buffer, paused): the old set stops incomplete (6 of 23
    parts x86, 10 Pi), the new grid's sets complete; a reader paused 12 s
    (~3 sweeps): every set it got complete, the sweeps it could not take
    skipped whole, the socket open (uWS's 2 MB limit never reached).
- Threads: settings and frames on the uWS loop, the producer on the data
  receiver, the engine on its worker; the array, pyramid and per-hop state
  under one mutex (a hop's write ~U values + their ancestors; a frame ~4096
  pools); the plan reaches the worker through a generation number.

**Measurements** (x86 dev machine, shared - load 10-29; 2026-11-20. The
Raspberry Pi 5 figures of 2026-11-21 follow below).
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
  mode with one page 32.5 %, independent 48 %. No packet dropped. With the
  fix round's overlapped frames (this case overlaps: 16384 samples a hop at
  dwell 5 < 16 frames of 4096) 8.3 / 14.9 / 23.9 % (coherent 32.8,
  independent 49.0 in the same run): the worker thread 6.1-7.8 % against
  4.0-4.2 % before. Of the four
  pages' cost the frame build is ~300 us x 80 frames/s; the rest is uWS /
  TLS / deflate (the same with compression off: 7.5 vs 9.2 %).
**On the Raspberry Pi 5** (2026-11-21, the user's test Pi: Pi 5 Model B
Rev 1.1, 8 GB, Debian 13, kernel 6.18.34, gcc 14.2; the integration branch
at 3919d64 - heimdall's dwell in whole packets, default dwell 5 = one 8091
packet a hop; no KrakenSDR used: the fake heimdall). Every figure with
45.5-53 C and `get_throttled` 0x0; load 0.0-1.3 before each run. x86 = the
dev machine the same day (load 4-13).
- Clean test build (`-j3`): 179 s. No warning from sweep mode's code; the
  7 warnings are the old ones (two sign-compares in fft_processor.cpp, one in
  message_builders.cpp, Eigen's maybe-uninitialized).
- The Python fake heimdall keeps real time ON the Pi: 146.6 packets/s
  (1.00x, 24.0 MB/s) with 27 % (independent) / 44 % (sweep) of a core - the
  CPU figures below are the receiver alone, with the fake on the same Pi.
- The receiver (`t_cpu.py`; % of ONE core, the whole kraken_doa process;
  the busiest threads in brackets; no packet dropped in any run; every page
  got 20.0 type 10 frames a second, 26-27 messages and 27-33 KB a second):

  | | Pi 5 | x86 |
  |---|---|---|
  | sweep, defaults (24-1765 MHz, auto = 512, dwell 5, 5 tuners): no page / 1 / 4 pages | 4.2 / 5.9 / 10.3 % (2.5 + 1.2; 6.1 + 2.5 with 4 pages) | 2.1 / 4.6 / 5.6 % |
  | sweep, worst case (SWEEP_RES:16384 -> 4096, overlapped, dwell 5) | 7.8 / 10.3 / 15.8 % (6.1 worker) | 7.0 / 7.2 / 11.3 % |
  | coherent, 1 page (spectrum, audio, DoA) | 26.2 % | 17.0 % |
  | independent, 1 page (5 spectra, audio) | 78.3 % | 29.7 % |

  So a Pi 5 runs sweep mode at heimdall's new rate (one packet a hop, ~47
  hops a second a tuner) with most of a core to spare even at the worst
  case and four pages: at the defaults with one page under a quarter of
  what coherent mode costs there, the worst case with four pages 60 %.
- The engine bench on the Pi (`bench/bench_sweep.cpp` built there, the real
  sweep.cpp, 5 tuners x 16384-sample packets, one packet a hop, offered at
  ~2x real time; us of CPU a packet, = x real time on one core):

  | N | Pi 5 | x86 | Pi 5 no overlap | Pi 5 always overlap |
  |---|---|---|---|---|
  | 128 | 424 (16.1x) | 365 (18.7x) | 572 (11.9x) | 1038 (6.6x) |
  | 512 | 501 (13.6x) | 412 (16.6x) | 669 (10.2x) | 897 (7.6x) |
  | 1024 | 733 (9.3x) | 429 (15.9x) | 733 (9.3x) | 1042 (6.6x) |
  | 2048 | 1076 (6.3x) | 913 (7.5x) | 918 (7.4x) | 1072 (6.4x) |
  | 4096 | 1351 (5.1x) | 1176 (5.8x) | 1146 (6.0x) | 1345 (5.1x) |
  | 16384 -> 4096 | 1238 (5.5x) | 1102 (6.2x) | 863 (7.9x) | 1245 (5.5x) |

  (with one packet a hop the overlap is on from N 2048 at dwell 5; the
  columns vary by +-20 % from run to run - the 128 row's "no overlap" is the
  same code path as the first column). `build_frame` (whole range at the
  cap, 2.97 M bins, 4096 points) 203 us on the Pi with hot caches, 55 us on
  x86; 100 MHz window 77 us, 1 MHz 13 us; a plain max over the 3 M bins
  451 us. At 2x real time the bench's 8-slot ring overflowed now and then
  (7-24 of ~860 packets at N >= 2048) - at real time the receiver dropped
  none.
- The same figures from a second Pi 5 (Rev 1.0, 4 GB, Wi-Fi - the board the
  router's DNS named `krakensdr`): the fake 146.6 / 146.7 packets/s (27 /
  45 %), the build 187 s, t_sweep grid 122 / 122 and the rest 90 / 91 (the
  duration check - see the tests).

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
- `t_fix.py` 29 / 29 (the fix round: one test per review / hardware
  finding; the fake heimdall gained hooks for each): H1 auto picks 4096 /
  avg 10 for 88-108 MHz at dwell 20, follows the dwell (2048 / 4096 / 8192 /
  16384 at 5 / 20 / 50 / 100 ms, all >= 8 frames), the noise's spread halved
  (sd 1.21 vs 2.45 dB; ASan build 1.48 vs 2.24) with 1-2 packets a hop, carriers at their
  height; H2 a tuner reporting frequency 0 with garbage for 12 rounds: not
  shown (99.9th percentile 5.2 dB before and during), sweeps go on; F1
  generic RTL-SDRs: Reset settings in sweep mode -> heimdall gets the
  default plan and the display data, and a start with heimdall sweeping
  another plan -> the saved one; F2 tuners 3,4 parked on the hop centres of
  433-436 MHz after 0,1,2 were added: their carrier not shown (1.9 dB =
  noise); F3 the replayed FREQ kept: sweep -> independent seeds 433.92 MHz,
  sweep -> coherent and Reset settings send `set_frequency` after the mode;
  F4 phase state / noise flag live in sweep mode (system_status, cal_live);
  F5 an audio recording ends on OPERATING_MODE:sweep, an IQ recording when
  heimdall switches by itself; F6 `SWEEP_RUN:yes` / `:` / `:2` refused;
  F7 an unrelated "Not in wideband scan mode" before the first 0x2000 header
  (delayed 2.5 s): no fallback; an old heimdall's refusal split over two
  writes: back in 0.43 s with its reason; a dead tuner: the reference kept.
- The reviewer's unit tests (job scratch `review_c/`, run against this code
  in `client/review/`, ASan + UBSan): t_parked (hop 0 / 1 by tuners 0 / 1,
  the parked tuner's carrier at 3.0 dB = noise; before: by tuner 3), t_grid
  416 / 416 with its tone check narrowed from a 40-bin to a 4-bin window (two
  of its tones are 10 bins apart and the 40-bin window took whichever was
  higher: 8 failures, the same with overlap off), t_grid2 45 / 45, t_hostile
  and t_stress 0 failed.
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
  `send_control_command` sends nothing during one).
- On the Raspberry Pi 5 (2026-11-21; the suite brought to heimdall's
  whole-packet dwell - dwell 20 = 3 packets: auto 8192 for 88-108 MHz, 16384
  averages 5 overlapped frames - by the hop-loop worker, then made location
  independent and run there; no test needs scipy): t_sweep 122 / 122 + 91 /
  91, t_edge 50 / 50, t_modes 15 / 15, t_old 9 / 9, t_mcp 9 / 9, t_fix 16 / 16
  + 13 / 13 - on the normal build AND on the ASan + UBSan build (built on the
  Pi in 307 s at -j3; t_mcp not run on it), no sanitizer report, no aarch64
  / NEON / alignment finding. x86 the same day: the same counts. Two TEST
  changes the runs asked for: t_sweep's sweep duration check took the median
  of the single intervals between completion frames as the truth - those go
  out on the 50 ms ticks (100 / 150 ms for a 137 ms sweep), so the second Pi
  5 failed it at 0.137 vs "0.150" - now the period from the first to the last
  completion frame; the engine counts every sweep (a probe: 89 of the fake's
  90 in 12 s). t_edge's carrier check for hops of 1 or 3 frames got 3.1 dB of
  room under the expected height (x86 once 2.68: a one-frame hop's own noise).

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
The receiver's `system_status.sweep` (true while it sweeps) is a hint for
sweep mode like `independent` / `wideband` for theirs.

**The calibration chip** (end to end, 2026-11-20): `system_status` in sweep
mode carries heimdall's last phase state (4, "converged", in the test; 1 on
the KrakenSDR) with `wideband` and `independent` false - the chip said
"Converged" and coloured the bar green, though heimdall parks the
calibration. Now `system_status.sweep` (or the page's own sweep mode) counts
like the other non-coherent modes: the chip shows "Disabled" and the bar has
no calibration colour - the same as in wideband scan and independent mode
today, so the chip means one thing in all three. The status message itself
is right (it says `sweep: true`); the page had not used that field.

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
- The waterfall follows the window (2026-11-20, from the orchestrator's run
  on the KrakenSDR: zoomed from 24 - 1765 into 96.5 - 109.4 MHz, the old rows
  still showed the whole range under the new axis - with a sweep every 8 -
  40 s for minutes, a line under "100 MHz" really at 800 MHz; the live
  spectrum's rule, "older rows keep the span they were drawn with", is fine
  where rows are replaced within seconds, not here). Each row keeps what it
  showed - its first point's frequency, the spacing, its levels as u8 (the
  frames' own resolution) and its height in pixel rows (`swp.rows`, newest
  first, only the rows that fit the waterfall: at most ~4 KB each) - and
  every row is painted through the window shown (`swpWfRow`: per pixel the
  highest of the row's points in it, between two points the level at its
  middle, black outside the row's span). A zoom, a pan or a resize repaints
  all rows once per screen refresh (`swpWfRepaint`); a new row is painted the
  same way, so it lines up with the trace exactly (the main display's
  `addLine` maps the first to the last point across the plot). Zoomed in
  past a row's detail the old rows are coarser - honest -, and the new rows
  come in the new detail.
- Rows made while zoomed in, and colour changes (2026-11-21, the user: "When
  we change the waterfall colors it should change the colors of the existing
  waterfall too, not just the new scans. Also zoom causes a problem where the
  waterfall only updates on the zoomed part. Then when I zoom out, there is
  missing data on the other parts."):
  - A row is `{w, o, h, s}`: `w` the levels of the window shown when its sweep
    completed - exactly the completing frame's points (its point 0, spacing
    and n), never the trace: the trace is padded with 0 dB where the frame
    doesn't reach the window shown, and a sweep that completed during a
    zoom-out or a pan, before the wider frame came, stored that padding as
    levels which then painted over the overview (dark-blue steps from the
    plot's edges in the orchestrator's run on the KrakenSDR, 2026-11-21; fixed
    the same day) -, `s` its sweep count, `o` the whole range of the same sweep when that window was
    narrower (the receiver's overview), `h` its height; each part `{f0, st,
    d}` (first point Hz, spacing, u8 levels). A pixel takes `w` where it
    reaches, `o` elsewhere, black only where a row has neither
    (`swpWfLevel`, `swpWfRow`). Bound: the rows that fit the waterfall's
    height, each at most 2 x 4096 bytes (a 677 px waterfall at speed 1: 677
    rows, ~5.5 MB at the worst; measured 118 KB for 44 rows at speed 5).
  - The overview: the page sends `SWEEP_OVERVIEW:<points>` (the contract,
    above) with its first view and whenever the view's points change (plot
    width, Slow link), again after a reconnect and on entering the mode. A
    frame with flag 16 is kept as `swp.ovPend` (the trace, the readouts and
    "Points sent" stay the view's) and taken by the next flag-2 frame of the
    same `sweeps` and grid (`swpWfLine`; a line held back by the 20-a-second
    limit keeps its overview with it). n, point 0 and the spacing come from
    the overview's own header - it is a whole-range view, ~0.67 - 1.5 x the
    points asked for. A receiver without it ignores the command silently
    (the receiver worker's finding: no reply, no log line, the stream
    unchanged): no overview ever comes, `o` stays null and the rows are as
    before - black outside the window they were made in.
  - Colours: a change of the colour map or of the contrast (Min / Max, the
    sliders or Auto's range) repaints every row there (`swpWfRecolour` ->
    `swpWfRepaint`, once per screen refresh while a slider is dragged; Auto's
    500 ms tick repaints only when its range moved). The speed changes the
    height of new rows only. Cost of a full repaint, 1573 x 677 px (a 1920
    px window), 578 rows: 22 - 37 ms (median of 7, headless Chromium on the
    shared dev machine, two runs); 163 rows: 8 ms.
  - Tests (2026-11-21, scratch `t_wf.py`, the waterfall read back from its
    ImageData; real wheel / mouse events): after a Colors change and a Min
    drag every row equals a fresh paint with the new setting, none left in
    the old colours, none black; the drag repainted at most once a frame;
    Auto repaints only on a new range; Speed changes no old row; rows made
    zoomed in (x6, 30 sweeps) then zoomed out: every pixel by the rule -
    38 550 from the window, 190 200 from the overview, 0 black, 0 wrong;
    the same after a pan, a zoom deeper than the rows' detail and a resize;
    memory within the bound; Slow link and a reconnect ask for overviews
    again; a zoom while paused makes no row; a range change clears. Mock with
    overviews 30 / 30, mock as an older receiver 28 / 28, today's receiver
    build (ea21532, the real engine, no overview) 28 / 28 - there the rows
    are as before (190 200 black pixels outside the old window, the window
    inside), the colour fix works, no error and no notice on the page, and
    the receiver logs nothing for `SWEEP_OVERVIEW`. The earlier suites
    again: mock `t_sweep` 160 / 160, `t_compat` 22 / 22, `t_bar` 16 / 16,
    `t_phone` 15 / 15; today's receiver `t_sweep` 161 / 161, `t_real` 42 /
    42, `t_bar` 16 / 16, `t_phone` 15 / 15.
  - End to end with the receiver's overview (2026-11-21, bb1cb6d merged, a
    clean test build, the fake heimdall): `t_wf.py` 37 / 37 on each of
    400 - 700 MHz, 87.5 - 108 MHz at dwell 5 (~11 - 16 sweeps a second: 812
    flag-2 frames and 160 overviews seen, the 50 ms line limit holding lines
    back with their overviews) and the whole range (a sweep every ~4 s).
    Rows made zoomed in, then zoomed out: 30 rows, 189 880 pixels from the
    overview, 38 870 from the window, 0 black, 0 wrong (whole range: 63 400
    / 12 850). Rows completed during a wheel zoom-out and a pan - the new
    "late frame" check, each row's window levels equal to its completing
    frame's span (matched by `sweeps`), every pixel by the rule: 74 rows on
    the fast sweep, 7 on 400 - 700 MHz, 1 on the whole range, all exact, 0
    black (the page before the fix fails it). Zoomed out by the wheel, by
    the Zoom slider and by a range change while zoomed in the page asks for
    `0:0`; at 1x no overview and no flag-1 frame comes. The older page
    (ea21532) against this receiver, `t_oldpage.py` 7 / 7: it never asks,
    no flag-16 frame reaches it (61 + 80 type 10 frames counted, at 1x and
    zoomed x6), its sweep works as before. Mock: `t_wf` 37 / 37, as an
    older receiver 35 / 35; the earlier suites unchanged (mock 160 / 22 /
    16 / 15, real `t_sweep` 161 / 161, `t_real` 42 / 42, `t_bar` 16 / 16,
    `t_phone` 15 / 15). The page's cost of two level sets a row: only
    where the window's levels don't reach is the overview read, so a full
    repaint costs the same per pixel - 21.4 ms for 570 rows at 1573 x 677
    px on the fast sweep, 7.6 ms for 162 rows, 3.3 ms for 35.
  - The other modes' waterfalls (the live one, the independent panes)
    recolour their old lines too since 2026-11-21 (*spectrum-views.md*, "The
    live waterfall recolours"), with one difference: there Auto's own moves
    colour only the new lines. This waterfall's instance never gets that
    level store (it never calls `addLine`).
- Old frames: a type 10 frame outside sweep mode, one built for a view from
  before the page entered the mode (`seq < swp.seqMin`), and one whose grid id
  is not the latest `sweep` message's are dropped - the last only while that
  lasts under 1.5 s (a receiver whose message never comes would otherwise
  never show anything).
- Zoom: the wheel (x1.25 around the pointer) on the spectrum or the
  waterfall, and the View popover's Zoom slider, as far as one bin per point
  (`swpZoomMax` = span / (bin x points): ~490x for the whole range at 2.3
  kHz bins, ~7800x at 146 Hz; ~10 700x for 600 MHz at 37 Hz) - the usual 32x would leave a 1.7 GHz range at
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

**Full resolution in the browser** (2026-11-21, round 5; the user: "we can
actually have a much better resolution sent to the browser. Perhaps even
the full resolution. Since it's only updating every few seconds. Add an
option to choose the full resolution under "Display" when sweep mode is
used"):
- The Display box's "Display resolution" (`#swp-dres`, `.sweep-only`, a
  list since round 6 - round 5 had a "Full resolution" switch; the user,
  2026-11-22: "Give us other resolution options for the display too via a
  drop down"). The choice `swp.dres`: 0 = Fit to the plot (the view's
  points only; the default), 1 = Full (every bin), n >= 2 = at most n
  values a set (pooled by the receiver). localStorage `kraken_swp_dres`,
  this browser only; round 5's `kraken_swp_full` = '1' carries over as
  Full, and is kept in step (set exactly while the choice is Full) for an
  older page. Slow link wins: with Slow link on, the list is disabled and
  nothing is asked for (the user picked Slow link for a slow line); off
  again, the choice comes back.
- The list is the receiver's `full_sets` for the running grid (each entry:
  `n`, `pool`, `values`, `step_hz`; the steps whose pool would be 1 are left
  out by the receiver; the last is Full), shown as "Fit to the plot",
  "37.5 kHz (46,427 points)", ..., "Full: 146 Hz (11,885,227 points)" -
  the page computes nothing (`swpDresRender`; rebuilt only when the
  entries change, so an open list stays open across sweep messages).
  Without the field (round 5's receiver, which takes `SWEEP_FULL:65536` as
  junk, or an older one): Fit / Full only.
- What is asked (`swpFullAsk`): with the field, the stored choice as it is -
  a stored n that the running grid doesn't list (it was left out as equal
  to Full there) is asked anyway, the receiver gives pool 1, and the list
  shows Full (`swpDresShown`); so a grid change never needs a new ask, and
  on a grid where that n pools again it pools again. Without the field:
  Full asks `SWEEP_FULL:1`; any other choice shows and asks Fit (0).
- The page sends `SWEEP_FULL:<n>` (per connection, with its first view, on
  entering the mode, after a reconnect, when the choice or Slow link
  changes, and if the field comes or goes). The receiver then sends the
  whole range at the sweep's own bins or pooled (binary type 11, the
  contract above: `pool` = 2^k, Hz of value 0 = the first group's centre,
  spacing = bin x pool) after each completed sweep's flag-2 frame, in
  parts - and on a change of n at once a snapshot at the new n (flag 2, the
  trace only); a set in flight at a change is abandoned by the receiver.
  `swpOnFull` puts a set together by its set id, pool and first index; a
  gap, a part out of order or an old grid drops it (that row keeps its
  window / overview levels). The layout is read in one place,
  `swpFullParse`.
- A pooled set can be coarser than the window levels of a row made zoomed
  in, or than the live frame: a row then paints from its window levels
  first and from the set where they don't reach (`swpWfRow`), and the
  instant trace keeps the frame where the frame covers the window at least
  as finely (`swpTrace`). "Points sent" reads "1525 + 46,427 at 37.5 kHz"
  for a pooled set, "1525 + full 11,885,227" for a full one.
- **Gain by region** (round 11, 2026-11-24; the user: "Auto gain doesn't
  work well. Remove it. Instead we'll add a feature where we can manually
  set the gain for different regions of the spectrum. We'll have a line
  under the FFT spectrum that we can add keyframe like points to. Clicking
  on one segment of the line will change the gain control for only that
  currently selected segment. Keyframe points can be removed by right
  clicking on them. The keyframe points should be able to be moved left or
  right by clicking and dragging."; SWEEP MODE's "gain by region", `swpReg*`
  / `swpLane*`):
  - The lane: `#swp-lane-box` (a canvas, `#swp-lane`) between the sweep's
    spectrum and the split, 30 px (40 where the pointer is coarse), shown
    (body class `swp-regions`) only while the `sweep` message's
    `gain_regions_known` is true - an older receiver or heimdall: no lane,
    the Gain is the common gain as before. It follows the spectrum's
    frequency axis exactly (`swpLaneX`: the plot's margins `SWP_L` /
    `SWP_R`, `swpWindow()` - zoom and pan included; redrawn with every
    `swpDraw`). It draws the receiver's table (`swpRegRx`) plus a drag in
    progress (`swpLaneTable`) - never a change it has only sent: each
    region a step of a line whose height is its gain (0 - 50 dB over the
    lane's height; no points: one line at the common gain, dashed at Auto),
    its gain written in it where there is room, the selected region shaded
    (`--ac-d2`) with its range ("29.7 dB · 417.213 - 438.250 MHz") where
    that fits; the points as handles (a bar with two grips, brighter under
    the pointer or dragged). Colours: the palette's hex values.
  - Gestures (pointer events on the lane: mouse, touch and pen alike;
    `touch-action: pan-y`): double-click / double-tap (two taps within 350
    ms, 24 px) adds a point at the hop edge nearest (`swpEdgeSnap`: start
    + k x 1 912 500 Hz, k = 1 ... hops - 1), refused within a hop of
    another point; both halves keep the region's gain - with no points the
    common gain, and none while that is Auto (the lane blinks amber: set a
    gain first). A press on a point (6 px; 16 px by touch) and a drag move
    it, snapped to hop edges, held at least a hop from its neighbours in
    the range and inside the range's inner edges (`swpRegLimits`); the
    table goes on release. A right-click on a point (`contextmenu`, the
    browser's menu never shows on the lane) or a press and hold (600 ms,
    touch, without moving) removes it: the merged region keeps the
    selected one's gain if either was selected, else the left one's; the
    last point's removal sends `GAIN:<that gain>` then `SWEEP_GAINS:|`. A
    click / tap off the points selects the region under it (`swp.reg.selF`:
    a frequency in it - after loading, a range change or a region that left
    the range: the middle of the view). A drag off the points pans the view
    when zoomed in, as on the plot; the wheel zooms. A drag that starts on
    a point never pans. Keys (the lane takes focus): Left / Right select,
    Shift + Left / Right move the selected region's right border a hop,
    Delete / Backspace remove it, Insert / + add one in the middle of the
    selected region.
  - The Gain control (`sgn` -> `swpRegSetGain`): while there are points it
    sets the selected region only - the whole table with that one gain
    changed, never `GAIN:` -; its range starts at 0 dB (`min` 1: Auto can't
    be a region's gain), the bar is outlined (`gn-region`) and its tip names
    the region's range; it shows the selected region's gain (synced from
    every state message, and at once on a new selection - a pending send of
    the bar for the old region goes first, `swpRegFlushGain`). No points,
    and every other mode: the common gain, as before. Changes are built on
    the table this page sent last until the receiver's answer is there
    (`swp.reg.pend`, at most `SWP_PEND_MS`): a gain set right after a move
    doesn't undo the move.
  - The Sweep box's Gain line (`#swp-gain-st`, `swpGainText`): "49.6 dB" /
    "Auto", or "3 regions, 16.6 - 49.6 dB" (the regions in the range); its
    tip says how to use the lane, as does the spectrum header's help. The
    pointer's readout adds the gain of the step under it while there are
    regions (`swpRegGainAt`: the hop's centre's region). The picture (PNG)
    has the lane between the spectrum and the waterfall; its caption's gain
    is the Gain line's.
- **Auto gain** (history: round 9, 2026-11-23 - removed in round 11, 2026-11-24; the user: "Could we auto adjust the
  gain based on the current swath, and still have the sweep update fast,
  and have the sweep stitch together well?"). heimdall learns a gain per
  step; the page shows the switch and what it does:
  - The switch: the Sweep box's "Auto gain" (`#swp-agc`, under Dwell).
    Shared state: a click sends `SWEEP_AUTO_GAIN:<1|0>` and puts the
    switch back to what the receiver last said (`swpAgcSet`: no
    optimistic update); the `sweep` message's `auto_gain` sets it
    (`swpAgcRender`, from `swpRenderCtl`). Disabled, with the reason
    added to its tip, when `auto_gain_known` is not true: `false` = this
    heimdall has none (older), absent (`null` in `swpNorm`) = an older
    receiver; nothing else changes then. With the gain at the tuners' own
    AGC (-1) it is on but idle (`auto_gain_active` false when the
    receiver says so, else derived) and the line says so.
  - The ceiling: in sweep mode with Auto on the top bar's gain reads
    "≤ 49.6 dB" (`updateGainDisplay`; off again on leaving the mode). The
    Sweep box's "Gain" line (`#swp-agc-st`, `swpAgcText`): "49.6 dB, every
    step" / "≤ 49.6 dB · 53 lower, min 16.6 dB" / "≤ 49.6 dB ·
    none lower" / "49.6 dB · Auto idle (tuners' AGC)" - the receiver's
    `gain_reduced_hops` / `gain_min_db`, else counted from the map. Short
    on purpose: the first wording ("up to 49.6 dB, 62 steps reduced,
    lowest 8.7 dB") wrapped to two lines at the sidebar's default width on
    the KrakenSDR (the orchestrator's run); "≤ 49.6 dB · 911 steps lower,
    min 16.6 dB" still wrapped by a character, so the line leaves "steps"
    to its label and tip and stays on one line, also for 911 steps.
  - The map: `{"sweep_gain":{grid, sweeps, f0_hz, step_hz, hops,
    ceiling_db, gain_db:[...]}}` (JSON - an older page ignores it; a
    binary type would fall into its FFT path), hop h centred at
    `f0_hz + h x step_hz`, +- step_hz / 2; `swpGainMapParse` is the one
    reader. A map of another grid is dropped, a new grid drops the one
    held (the receiver sends the new grid's); kept across mode changes
    (the receiver sends one only on a page's first view and after a grid
    change).
  - On the plot: a 3 px strip on the plot's bottom edge (under the
    lowest level, over the frequency labels, which moved down 2 px) where
    steps are reduced, step by step in whole pixels, amber - the stronger,
    the lower (full at 33 dB under the ceiling) - with the zoom and the
    pan; nothing with Auto off or no step reduced. The levels stay
    referred to the ceiling (the receiver shifts a step's levels by
    ceiling - its gain), so a signal keeps its height; on a reduced step
    sitting on the converter's own floor the noise floor shows higher -
    the strip right under it says why. On the spectrum's canvas, so the
    picture has it; its caption gets "Auto gain: ≤ 49.6 dB · ...".
  - The pointer's readout adds the step's gain: "415.106 MHz  31.4 dB
    gain 16.6 dB".
  - Tests (`t_agc.py`, the mock speaking the receiver worker's contract):
    the switch (sent, not checked until the broadcast; off; kept across a
    mode change), the line and the "≤", the strip against the map at x1,
    x4, x20, x80 and after a pan (every visible step's centre pixel: amber
    exactly where the map is under the ceiling), the readout over the
    lowest step and over one at the ceiling, the picture (its caption and
    the strip's pixel in it), a range change (the old map dropped, the new
    grid's drawn), idle at the tuners' AGC, an older receiver and an older
    heimdall (disabled with the reason, nothing sent, nothing drawn), the
    phone layout, the line on one line (also for 911 steps): 36 / 36.
  - The pair (the receiver's 66f358f + 83760a1 on heimdall's f3ca686; the receiver
    worker's fake heimdall with heimdall's rule and a scene whose carriers
    at 415.1, 457.2 and 499.3 MHz overload at 49.6 dB; `t_agc2.py`, 25 / 25;
    `t_agc.py` against it 29 / 29): two tabs - one switches, the other
    follows the broadcast, both ways; the map of the grid with one gain a
    hop, every value from the R820T's list; on 400 - 520 MHz 16 hops
    reduced (lowest 19.7 dB): the three carriers' hops, their neighbours
    (a tuner takes 2.4 MHz, wider than the 1.9 MHz step, so it sees the
    carrier too) and the scene's strong TV-band noise - no quiet hop; the
    line = the map = the receiver's figures; both pages' strips = the map,
    zoomed too; the readout over the lowest hop its gain; off: heimdall
    puts every hop back at the ceiling (the map), no strip; a restart of
    kraken_doa with Auto on: `set_sweep_auto_gain` replayed, the page
    shows it on; an older heimdall (the fake's `fake_no_auto_gain`, then a
    restart): `auto_gain_known` false, the control disabled with the
    reason, nothing drawn.
  - Only the receiver's current word (round 9b, 2026-11-23; a user's
    report: "Auto gain doesn't seem to be working ... still getting severe
    overload", with the receiver's saved `sweep_auto_gain` and heimdall's
    `auto_gain` both false). While the WebSocket is down, or before the
    new connection's first `sweep` message (`swpAgcLive`: open and not
    `swp.stale`), the switch keeps the receiver's last word but is
    disabled, its tip says "Not connected to the receiver ...", the line
    reads "not connected", and the top bar's "≤", the strip, the readout's
    gain and the caption are gone (`swpAgcOn` is false). Before this a
    click in that gap went nowhere (`send` drops it) with the switch
    looking live. Rendered on `ws.onclose` and `swpOnOpen`.
  - The flows (`t_agc3.py`, 45 / 45, and `t_agc4.py`, 9 / 9, on the
    fake heimdall with heimdall's rule; each flow ends comparing the
    switch, `swp.st.auto_gain`, the fake's `get_status` `sweep.auto_gain`
    and the receiver's saved `sweep_auto_gain`): a mouse click on the
    switch and on the label text; Space on the focused switch; a touch tap
    on the switch and on the label (phone layout); press-and-hold (no
    toggle, nothing sent); help mode then a tap (explains, nothing sent);
    with Auto on: the top bar's gain by keys, by the wheel, to Auto (-1:
    on, idle) and back; range, resolution, dwell, tuners, pause / resume,
    the display resolution list, zoom and pan; coherent / wideband /
    independent and back (on and off); a page reload; a second tab
    opened, switching it off and on, closed (the first tab follows, and
    with it off shows no "≤", no strip, "every step"); a tab frozen
    (`Page.setWebLifecycleState`) for 30 s while another switches it off,
    then used; a click while the WebSocket is down (disabled, nothing
    sent); kraken_doa restarted (the replay); the fake heimdall restarted
    under the running receiver; a heimdall up 25 s after the receiver; a
    heimdall refusing `set_operating_mode` for its first 20 s; a heimdall
    without the command and back. No flow turned Auto gain off without a
    click, and in none did the four disagree. A side finding for the
    receiver: with a heimdall refusing `set_operating_mode` at start, the
    replayed sweep mode is lost (the stack comes up in coherent mode; Auto
    gain stays saved and comes back on choosing sweep again).
  - Round 10's words (2026-11-24; the user: "the SNR of the narrowband
    signals at 416 MHz get alot better if I turn the gain down. Maybe the
    overload flag isn't good enough"). heimdall now also takes a step
    lower where a strong signal saturates the tuner without clipping (the
    rule: per step the highest gain at which it is still received
    cleanly, found by trial visits a step lower or higher; the receiver's
    section has the fields). The page reads, from the `sweep` message
    (`swpNorm`; NaN / null when absent): `gain_floor_hops`,
    `gain_search_hops`, `gain_probes`, `gain_probe_sweep` and the
    receiver's `trials_left_out`. The other counts (`gain_pairs*`,
    `gain_floor_decisions`, `gain_step_learned`, `floor_scatter_db`) are
    not shown.
    - Which words: round 10's once this connection's receiver has sent
      `gain_floor_hops` (`swp.agcR10`, reset in `swpOnOpen`), so an older
      receiver or heimdall keeps today's tips and today's look, pixel for
      pixel.
    - The switch's and the label's tip (`SWP_AGC_TIP10`): the top bar's
      Gain is the upper limit; each step gets the highest gain at which it
      is still received cleanly; lower where a strong signal overloads the
      tuner, also when it is just outside the step and nothing clips
      (weaker signals squashed, false carriers); found by trying steps a
      little lower or higher on some passes (about 20 sweeps to settle,
      then a few steps a sweep); back up when the signal has gone. No
      "linear", "probe" or "bracketed".
    - The state line: "≤ 49.6 dB · 68 lower · settling" while the search
      is a real share of the sweep (the passing lowest gain leaves its
      room to the word: one line, also at 911), today's "≤ 49.6 dB · 68
      lower, min 8.7 dB" after. Nothing new with Auto off, idle (the
      tuners' AGC) or not connected.
    - When "settling" shows (`swpSettleStep`, once a state message;
      `swpSettling`). The first run on the real KrakenSDR (911 steps)
      showed that heimdall's `gain_search_hops` never reaches 0: 906 ->
      ~25 by the 10th sweep -> under 10 by the 15th - 19th, then 1 - 8
      steps are always tried again (signals that come and go). So the
      word does not show for any count above 0. The rule:
      - on at the limit, max(2, ceil(1 % of the sweep's steps)) (911
        steps: 10; 13 steps: 2), off below it;
      - once it has gone off, back only at twice the limit, so a count
        hovering at the limit doesn't make it blink;
      - a fresh search after Auto off / on, a grid change or a new
        connection: on at the limit again.
    - The state line's tip (`#swp-agc-kv`, `swpAgcKvTip`; only the
      receiver's figures, only while Auto is on and working): "Now: 68
      steps lower: 41 because the converter clipped, 27 for a strong
      signal nearby (no clipping)" (`gain_reduced_hops` -
      `gain_floor_hops` / `gain_floor_hops`); "18 trial visits in the
      last sweep" (`gain_probes`; "(it tried every step one lower)" with
      `gain_probe_sweep`); "6 steps still being tried" whenever
      `gain_search_hops` > 0 ("settling: 120 steps still being tried"
      while the word shows); "37 trial
      visits not drawn (since the receiver started)"
      (`trials_left_out`; the receiver leaves out every trial visit of a
      step that has a current row, lower or higher - the tip first said
      "at a higher gain", from the receiver's first rule).
    - The strip and the readout's gain are unchanged: they follow the map,
      from which the receiver keeps the trials out.
    - Tests (`t_agc10.py`, mock, the counts injected into its `sweep`
      message, 44 / 44):
      - the counts absent: the Auto gain rows the same pixels as the page
        before round 10 (ebac1b3, served in its place), off and on, and
        the same three tips. With Auto off, 12 pixels of the switch's
        knob differ by one level, and they differ the same way between
        two loads of the old page itself (raster jitter), so the check
        allows 1 level;
      - with the counts: the tips' words; "settling" there and gone;
        one line also for "≤ 49.6 dB · 911 lower · settling"; the box
        the same place and size;
      - "settling" on the whole plan (911 steps, limit 10), fed the real
        sequence 906, 326, 80, 25, 12, 9, 6, 8, 3, 5, 1, 6: on through
        12, off from the 9 and through the 8 / 6. Then hovering at 9, 10,
        11, 10, 12, 19: no word. 20, 14, 9: on, on, off. Auto off and on:
        on at 10, off at 9. The tip says "settling: 906 steps still
        being tried", then "6 steps still being tried", then "1 step";
      - a 13-step sweep (limit 2): 1 step tried shows no word and the
        tip "1 step still being tried". 13, 2, 1, 2, 3, 4: on, on, off,
        off, off, on;
      - the tip's split, singular and plural; `trials_left_out` in the
        tip only; not connected / off / idle without counts; the counts
        gone again: today's line;
      - the strip: the same map again changes no pixel, and a map with 3
        hops changed changes only those hops' pixels.
    - End to end (`t_agc10r.py`, 18 / 18): the receiver at ca7d6da on the
      receiver worker's fake with round 10's counts (`fake_r10_status`)
      plus a hook that sets them (`fake_r10_over`), scene_agc. Checked:
      - the counts absent: round 9's tips, today's line;
      - the counts there: round 10's tips; "16 steps lower: 13 because
        the converter clipped, 3 for a strong signal nearby";
      - a search: "≤ 49.6 dB · 16 lower · settling", gone when it ends;
      - up-trials with the trial bits (`fake_probe` sparse, up): "246
        trial visits at a higher gain not drawn" in the tip (the wording
        then), nothing in the line;
      - the strip over 81 samples under trials: 4 map changes, no strip
        change without one;
      - a second tab switching Auto off: the first shows "every step",
        no counts, no "settling"; on again: "settling" back;
      - the counts gone: today's line; a reload: round 9's tip.
- **Deflated parts** (round 8, 2026-11-23; the user: "Implement the sweep
  full resolution compression"). The page asks `SWEEP_FULL_Z:1` when the
  browser has `DecompressionStream("deflate")` (`SWP_Z_OK`: the
  constructor tried once; a browser without it, or one where it throws,
  never asks and gets raw sets as before), once a connection, just
  before `SWEEP_FULL` so that its snapshot comes deflated too; again
  after a reconnect (`swp.zSent`). A part with `flags & 8` is one zlib
  stream (RFC 1950) of its `n` values: `swpInflate` hands it to the
  browser's own zlib, and the value lands at its `first` whatever order
  the promises resolve in. The assembly (`swpOnFull`) still checks the
  parts' ARRIVAL order (`next`: no gap, nothing out of order); `filled`
  counts the values in place, `pend` the parts still inflating; the set is
  complete when its last part has come, nothing is pending and every value
  is in (`swpFullPartIn`) - then everything as before (`swpFullDone`: the
  pyramid, the row, the trace). Raw parts in a deflating page's set are
  normal (the receiver sends a part raw when it isn't smaller or wasn't
  ready in time). A part that fails to inflate - or inflates to another
  length - drops its set like a gap (that row keeps its own levels); after
  `SWP_Z_FAILS` (3) the page sends `SWEEP_FULL_Z:0` and stays raw until it
  is loaded again (a safety net).
  - Native or the page's own? Measured on the real 146 Hz set (the PC's
    KrakenSDR, its 23 parts deflated Huffman-only, 7.14 MB for 11.9 MB;
    headless Chromium, `zbench/`): `DecompressionStream` 77 - 87 ms one part
    after another, 69 - 84 ms all at once, no long task; a decoder of my
    own written for the measurement from RFC 1951 (all three block types,
    length / distance codes, Adler-32) 250 - 275 ms, 7 - 18 ms a part
    synchronous on the main thread. Both byte for byte; a corrupted part:
    both fail (the browser's rejects with TypeError, mine at the Adler-32).
    So the browser's, and no decoder in the page.
  - Through the page's own handler (`t_zip.py`): the real set's 23 parts
    handed over in 11 - 12 ms, the set complete 114 - 115 ms after the first,
    byte for byte (FNV), no long task; the mock's deflated sets (the
    browser's `CompressionStream`), and sets mixing deflated and raw
    parts, byte for byte; a corrupted part drops its set, the next ones
    come right; three switch the request off and the sets come raw again;
    no `DecompressionStream`, or one that throws: never asked; memory over
    200 more sets flat (the renderer 295 -> 290 MB, the JS heap 5.2 MB);
    the phone layout.
  - With the receiver's ca62c27 (`t_zip2.py`, the fake heimdall, through
    the counting proxy): a page that inflates and one whose browser has no
    `DecompressionStream`, on one receiver, hold the SAME sets for the same
    sweeps (FNV of every row's set) at Full on the whole range at 146 Hz,
    at the 2.34 kHz and 37.5 kHz steps, on 400 - 700 MHz and on the FM
    band; every part of the inflating page came deflated. Full on the whole
    range: 1.59 MB/s on the wire deflated against 3.19 raw (49.9 % - the
    fake's scene compresses to 47.9 %, the real 146 Hz set to 60.1 %: about
    3 -> 1.8 MB a second on the Pi's 4 s sweep, the figure the tips and the
    README quote); the Data rate 1.59 - 1.60 MB/s; the page's own count
    "≤ 1.60 - 1.61" - never below the wire. With the receiver's
    `KRAKEN_DOA_TEST_Z_DELAY_MS=300` (every part raw for a page that asked)
    the sets the same as the raw page's. Long tasks of 52 - 60 ms come with
    a 146 Hz set raw or deflated alike (the pyramid of 11.9 M values, the
    row) - none from inflating.
- End to end (round 6, 2026-11-22; the receiver's 4295642 merged, a clean
  test build, the fake heimdall; `t_dres.py`, `t_keep.py`, `t_pool2.py`):
  the list from the real `full_sets` - the whole range at 146 Hz six
  entries (150 kHz / 11 607 values ... 586 Hz / 2 971 307, Full 11 885
  227), at Auto four, 88 - 108 MHz at Auto two; each step's sets on their
  rows with the field's pool, values and spacing; two browsers on one
  receiver (Full and 65 536 on 400 - 700 MHz at 4096): the pooled set =
  the full set of the same sweep max-pooled by 8, byte for byte, its Hz of
  value 0 and spacing as the contract says; a row of each kind on one
  waterfall at x69 - the longest one-colour stretch 4 px (full), 7 (pool
  8), 96 (fit), every row equal to a fresh paint by its own rule; a change
  of step while a full set was in flight (1.57 M of 11.9 M values): the
  part dropped at once, the snapshot and sets at the new pool, that
  sweep's row by its own levels; the instant trace from a pooled set,
  exact; the budget with mixed sizes (a 48 MB budget for the test: 3 full
  sets kept, the older pooled ones given back, all older than any kept);
  a stored step across a reload, across a range change that drops it from
  the list (Full shown, nothing asked again) and back; Slow link; the
  phone layout: 42 / 42. `t_keep` (the user's case: Full on, rows, then
  off or Slow link - the old rows keep their sets byte for byte, x69 into
  them: 495 peaks against 7 without) 21 / 21. Round 5's receiver (no
  field): Fit / Full only, round 5's "on" carries over as Full, a stored
  step shown and asked as Fit (never `SWEEP_FULL:65536`), no set at Fit;
  the receiver before `SWEEP_FULL` (178f007): the same, nothing comes.
  A full repaint, round 5's `swpWfRow` against round 6's in one browser,
  136 fit rows: median 17 / 16 - 17 ms (no cost of the finer-first test).
- A complete set is the trace's source while no frame for the window shown
  has come: right after a zoom or a pan the trace is cut from the set at the
  view's points (a point = the highest of its bins) - sharp at once, before
  the receiver's frame for the new window; that frame then takes over
  (`swpTrace`: "for the view" = the frame's seq is the last view's and the
  view sent is the window shown). A completed sweep's set is attached to its
  waterfall row (same `sweeps` and grid; the row exists already, or still
  waits for the line limit) and that row alone is painted again with it.
- A row paints from its set where it has one (`row.f`), else from its window
  / overview levels as before (never black where it has data). Each set gets
  a max pyramid (factor 4: ~1/3 more bytes; `swpPyr`, `swpPyrMax` - a
  pixel's max over any number of bins in a few dozen steps), so a repaint
  stays fast at 11.9 M bins a row.
- Memory: the rows' sets within `swpFullBudget()` - 256 MB on a desktop, 64
  MB on a phone layout. A set costs its bins x ~1.33 (11.9 M bins: 15.8
  MB, so 16 rows keep theirs at 146 Hz over the whole range; 371 k at Auto:
  0.5 MB, every row): the newest rows keep their sets, the oldest give them
  back first and fall back to their window / overview levels
  (`swpFullTrim`; strictly by age since round 6, also when the rows hold
  sets of different sizes - a small old set never outlives a bigger newer
  one). A new grid and leaving the mode give every set back
  (`swpFullDrop`).
- A change of the option changes only what NEW sweeps bring (round 6,
  2026-11-22; the user: "when I turn off full resolution, the previous data
  in the waterfall becomes low resolution. When we change display
  resolution, we should only be changing the new display resolution, not
  the old ones already on the waterfall"). Round 5 gave every set back when
  the switch went off or Slow link on; now a row keeps the set it got until
  it scrolls out, the budget takes it, the grid changes or the mode is
  left, and the waterfall can hold rows of different detail, each painted
  from its own. What a change does drop is the trace's source (`swp.full`)
  and a set being put together (the receiver abandons it). A set that the
  receiver finished before it saw the change still goes to its row.
- The trace's instant source is only ever the set of the newest sweep
  completed here (`swp.done`, from the flag-2 frames): a set skipped by a
  slow line, or one still to come after its flag-2 frame, leaves the
  frames' trace - never a sweep older than the one on screen, and never a
  set kept for an old row.
- "Points sent" reads "1525 + full 11,885,227": the view's points per frame
  and the set's bins - the second part only once a set has come (an older
  receiver never sends one: the view's points alone).
- Measured (headless Chromium, the mock, 1573 x 677 px waterfall): a full
  repaint with 57 rows, 16 of them with an 11.9 M-bin set, 16 ms; a wheel
  step (window, trace from the set, plot, full repaint) 21 ms; at 4096
  (512 k bins), 40 rows, 29 with a set: 6 - 8 ms.
- End to end (2026-11-22, the receiver's 6d5866f and the live-waterfall
  page work 2eab7de merged with this; a clean test build, the fake
  heimdall): `t_full.py` against the real engine - the snapshot (or a first
  completion set), sets after their flag-2 frames on the right rows, the
  instant zoom (x69 at 433.92 MHz, read back in the same script turn as the
  zoom: the trace from the set, exact against a brute-force max over its
  bins, 218 points, 0 wrong; then the live frame takes over), a row made
  before the zoom with its detail (494 peaks across the plot against 6
  without its set, exact, 0 black), 146 Hz over the whole range (24 sets,
  the 16 newest rows keep theirs: 241.8 MB of the 256 MB budget; repaint
  9 - 10 ms, a wheel step 8 - 9 ms) and 16.4 M bins at 37 Hz (1000 - 1600
  MHz, dwell 25: 12 rows with a set, 250 MB; repaint 7 ms, wheel step 7
  ms), Slow link, a range change, a range change while a set is on its way
  (nothing of the old grid kept), pause, leaving the mode: 31 / 31.
  `t_slow.py` through a 2 MB/s proxy (`slowproxy.py`; on localhost a set
  takes milliseconds and a busy page thread doesn't make the socket back up
  - the browser keeps reading): 16.4 M-bin sets every ~2.8 s, 9 rows, 3
  sets - the 7 rows without one painted from their window / overview
  levels, 0 black pixels, every set that came complete; the browser
  stopped 20 s: 0 black: 9 / 9. Against the receiver before SWEEP_FULL
  (178f007): no set, the page as before, 10 / 10. The other page worker's
  picture in sweep mode with Full resolution on (its `t_snap.py` with the
  switch on): 22 / 22.

**Finer resolutions on the page** (2026-11-21, with the receiver's 178f007;
the user asked for a finer resolution):
- The Resolution list is the receiver's: `sizes` (run as asked at this
  range and dwell) and `sizes_capped` (the others and why) - ten sizes, 128
  ... 65536 (18.8 kHz ... 37 Hz). The page never works out from the dwell or
  the span what can run. A receiver without these fields gets the eight
  sizes it takes (128 ... 16384): a size it would clamp silently is never
  offered (`swpResSizes`, `SWP_FFT_SIZES`).
- A size in `sizes_capped` is disabled with a short reason on it - "73 Hz ·
  7 ms dwell" (the dwell from `min_dwell_ms`), "37 Hz · range too wide" -
  and a sentence each in the select's title ("73 Hz needs a dwell of 7 ms
  or more (one step must hold 32768 samples)", "... would make too many
  points for this range - a narrower range gets it"). The list follows every
  `sweep` message: a dwell or range change moves sizes between the sets.
- The size asked for but capped (`fft_size` < `res`) stays selectable and
  shows what runs ("37 Hz → 146 Hz"); the title gives the reason from `cap`:
  "a step holds too few samples at this dwell - a dwell of 21 ms gets it"
  (`dwell_ms_needed`) or "the range is too wide for it - a narrower range
  gets it". An older receiver sends no `cap`: the general sentence.
- Checked at these sizes (nothing else changed): Points reads "11,885,227 ·
  146 Hz" (the whole range at 16384) and "16,384,001 · 37 Hz" (1000 - 1600 MHz
  at 65536); Steps "911 · 5 tuners"; the zoom's limit is span / (bin x
  points) - 7 794x for the whole range at 146 Hz (slider end 324), 10 744x
  for 600 MHz at 37 Hz (slider end 335) - with frames of one bin per point (36.62109375 Hz)
  and the readout to 100 Hz at 146 Hz, 10 Hz at 37 Hz; `swpZoomMax`'s bin
  before any `bin_hz` is the list's finest; the waterfall's rows and the
  overview stay <= 4096 points whatever the bins (frames are views).

**The sidebar.** A Sweep box (`data-sb="sweep"`, `.sweep-only`, title icon
`span`, placed first after the phone boxes): Resolution (Auto - naming the
width in use - and the FFT sizes as bin widths: 128 ... 65536, 18.8 kHz ...
37 Hz, from a receiver that sends its list (`sizes`, since 2026-11-21),
else 128 ... 16384 - see *Finer resolutions on the page* below; a size the
receiver had to cap shows both, "146 Hz → 586 Hz", and the title says
why), Dwell (5 - 1000 ms, typed; a value outside is clamped before it is
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
  click tunes a VFO; its scan needs one), Trunk Following (since the merge
  with trunk following, 2026-11-25: its voice and control VFOs can't run -
  the receiver pauses it, "sweep mode", and its top-bar banner is hidden
  too; *trunk-following.md*, "Page"), Debug Playback (a coherent array's
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
- Not covered: Firefox / Safari; the theme check passes
  (`tools/theme_palette.py --check`).

**End to end** (2026-11-20, phase 2): the same suites against the merged
tree's test build (the real engine) and the receiver worker's
`client/fake_heimdall_sweep.py` (sweep mode per the contract, an RF scene of
24 - 1765 MHz in absolute frequency: FM, DAB, DVB-T, LTE 800, GSM 900, ADS-B
...), scratch `stack_real.sh` + `run_all.sh` (`SOURCE=real`: the page's mock
only records). Found and fixed on the page:
- the calibration chip in sweep mode (above);
- the waterfall's rows after a zoom / pan (above);
- the Resolution select named the asked size when the engine capped it (the
  full range at 16384 runs at 4096): it shows both now;
- the zoom slider's end did not follow a change of the plot's width (a stale
  `max` until the next zoom): `swpResize` re-syncs it.
Checked against the engine with nothing to change: Auto's widths (512 =
4.69 kHz for the full range, 16384 for 20 MHz), `gain_db` from another page,
a new view answered while paused (flags 1 and 4), K' = H (400 - 409 MHz on 5
tuners: ~20 lines a second, the sweep time 0.02 s, the progress readout
steady), a slow sweep (1 tuner, the full range: the progress climbs, no line
before the sweep completes, "911 · 1 tuner"), nothing the page sends by
itself in sweep mode (20 s idle: no command at all; no `Rejected control
message` in the receiver's log over the whole run), round trips to coherent /
wideband / independent with a VFO's audio (type 7 and audio again, the VFO's
tuning bar, no sweep display left, no refusal), two pages (range, tuners,
resolution and gain follow; each its own zoom). Results, each suite on a
freshly started stack: real engine - `t_sweep.py` 161 / 161 (with the
waterfall-follows-the-window checks: a 538 MHz DVB-T mux keeps its colour
under its axis position after a x3.8 zoom and a 300 px pan, noise under 520
MHz; the rows stay within the waterfall's height), `t_real.py` 42 / 42,
`t_bar.py` 16 / 16, `t_phone.py` 15 / 15; the mock (the page's own engine,
the real one's sweep traffic kept from the page) - `t_sweep.py` 160 / 160,
`t_compat.py` 22 / 22 (the other modes identical to the page before sweep
mode, measured against the new receiver), `t_bar.py` 16 / 16, `t_phone.py`
15 / 15.
- A level of 0 in a frame means "never measured" (the contract) but also
  "measured at or under the sweep's reference": at 146 Hz bins with 1 - 2
  averages a quiet hop's noise sits partly under the one reference, and the
  trace shows runs of 0 there (89.6 - 91.3 MHz in the FM-band test). The page
  cannot tell them apart; it draws both at 0 dB, which is right for both.

## Tests

Each part's own tests are in its section above (heimdall: on the KrakenSDR
and against a fake librtlsdr, `heimdall_v2/docs/dev/operating-modes.md`; the
receiver: a fake heimdall with sweep mode, normal and ASan + UBSan builds;
the page: headless Chromium against the real engine and an in-page mock).
Two independent reviews (heimdall's code, the receiver's code) followed the
first merge; their findings and the fixes are in those sections.

**End to end on the KrakenSDR (2026-11-20, x86 PC, 5 tuners, the merged
tree).** heimdall and kraken_doa as port-moved test builds, the page in
headless Chromium driven as a user would (the Mode selector, real wheel
events and clicks; the range, gain and pause as another page would send
them), heimdall's status and the 8091 headers read beside it. Job scratch
`e2e/run_hw.py`, `probe_bars.py`, `probe_change.py`.

- Coherent first: CONVERGED after 17 - 22 s. Mode selector -> Sweep: 8091 flags
  0x2801, the tuners on their blocks, the page's chip "Disabled".
- 24 - 1765 MHz (911 steps): 8.1 - 8.3 s a sweep with 5 tuners (183 rounds,
  41 - 48 ms each), 39.2 - 39.7 s with one. Auto resolution 4.69 kHz
  (371 414 points). 88 - 108 MHz (11 steps): 6.8 - 7.0 sweeps a second with
  5 tuners, 5.0 - 5.4 with 3 (two toggled off in the top bar by clicks),
  Auto 586 Hz. The FM stations, DAB / TV, the UHF and mobile blocks at
  their frequencies; signals that fill whole steps keep their height: over
  758.9 - 802.3 MHz (some 23 steps of mobile downlink) the six highest
  points read 22.8 - 23.9 dB.
- Wheel zoom on the full range down to 96.5 - 109.4 MHz (135x), the pointer
  readout, a gain change (20.7 dB: applied by heimdall, `gain_db` in the
  `sweep` message), pause (the sweep count stands) and run.
- Steady state, 463 sweeps of 88 - 108 MHz (5 093 step measurements): no
  step's median more than 3 dB from its own median over the run, and a
  whole sweep's level within -0.44 .. +0.11 dB - nothing pumps. The one
  exception is the first sweep after a resolution change (two steps -3 and
  -4 dB, the sweep -2.0 dB).
- Every frame of the first 8 s after a range change (zoomed in before it,
  161 frames): no run of 20 equal levels, no complete frame more than
  1.5 dB over the steady median; the page is back at 1x on the new range.
- Back to Coherent after sweeps with 5, 3 and 1 tuners: CONVERGED 29.0 s
  after the switch, calibration check worst |lag| 0.0 samples, |phase|
  0.2 deg, the page's frequency digits, VFO card, tuning bar and DoA pane
  back. 0 coherence events, 0 failed retunes, 0 unquiet flushes, no page
  error in every run.
- What the first run (before the fix rounds) found: the return to coherent
  took > 100 s - every channel 1874 - 2220 samples off, re-acquired one by
  one (a full-path retune moves that dongle's stream by 2 samples, and the
  tuners had been retuned unequally; heimdall's *Review fixes*); Auto gave
  146 Hz bins with 1 - 2 averages on 88 - 108 MHz; the waterfall's rows kept
  their old span after a zoom; the chip said "Converged".
- A lesson of the test itself: a first version of the script put the zoom
  back by writing the page's zoom variables, which left the page and the
  receiver's view of that socket apart - the next range change then drew a
  row with step-wide blocks of one level. Not reproducible with user
  actions (wheel, slider, the page's own reset on a range change).

Not tested in that round: a Raspberry Pi (a Pi 5 is round 3, below - it
turned out as fast as the PC), Windows (tuners are written one
by one - a round will be several times slower), macOS, the MinGW / zig
syntax checks (no toolchain on the test machine), generic RTL-SDRs and
`--kerberos` on real hardware (both against the fake librtlsdr only),
Firefox / Safari.

**Round 2, 2026-11-21: the library no longer shifts a stream, the hop loop
wastes nothing.** Two things changed under the mode (the details and every
worker's test are in `docs/dev/librtlsdr-fork.md`, *The rule, and the fix*
and *Review round*, and `heimdall_v2/docs/dev/operating-modes.md`, *Hop
timing* and its review rounds):
- librtlsdr (two commits on the fork, not upstream yet; the patches are in
  `docs/dev/librtlsdr-patches/`): any USB control IN of 3, 7, 11 ... bytes
  made the RTL2832U drop 4 bytes of its stream - every retune's lock check
  read 3 tuner registers. Such reads are padded and the lock check reads 4;
  a PLL settle after big VCO jumps; registers repaired after a lock retry;
  a tune flag that saves a transfer. heimdall, built against it
  (`RTLSDR_NO_STREAM_SLIP`), goes back to coherent with the phase-only
  recalibration.
- heimdall's hop: the dwell in whole packets, the per-dongle gate in place
  of the cross-dongle quiet point, the first valid packet no longer thrown
  away, the fast tune call (7.5 ms against 14.4 ms a tune on the PC).
  Defaults since: dwell 5 ms (one packet a hop), settle 2 ms.

The orchestrator's own runs on the KrakenSDR (x86 PC, 5 tuners; the merged
tree and the fork with both commits, rebuilt from the patches; job scratch
`e2e/`: `lagprobe.py`, `run_hw.py`, `skewtest.py`), each compared with the
same test before the round:

| | Before | Now |
|---|---|---|
| Lag probe, 200 tunes a pattern on one dongle (same frequency, + 100 kHz, 433 MHz and back, 24.5 / 1700 / 100 MHz, a divider boundary; fast path + finish; a 3-register read) | -2.000 samples a tune (-2.02 fast, -2.000 the read) | 0.000 each |
| 24 - 1765 MHz, 5 tuners / 1 tuner (through the page) | 8.1 - 8.3 s / 39.5 s | 4.0 s / 18.9 s (21 ms a round, exactly 1 packet a hop) |
| 88 - 108 MHz, 5 tuners / 3 tuners | 7.0 / 5.0 - 5.4 sweeps a second, Auto 586 Hz | 11.3 / 9.4, Auto 1.17 kHz |
| Sweep (5, 3 and 1 tuners) -> coherent | 29 s (every lag re-acquired; > 100 s before that fix) | 1.5 s as the script counts (its 1 s of holding CONVERGED included: ~0.5 s), check \|lag\| 0.0, \|phase\| 0.2 deg |
| 2048 / 4096 / 6144 / 9000 retunes of one tuner (independent), and three tuners 2048 / 4096 / 6144, then coherent | lags -4096 ... -18000 samples (no peak at 18000), 26 - 40 s | no lag at all (no servo engaged), 1.6 s each |
| The sweep with the tuners' buffer timing spread | 81 ms a round, 191 unquiet flushes in 15 s | as from an aligned array, 0 |

20 of 20 steps of the end-to-end script, 0 coherence events, 0 failed
retunes, no gate failed or late, no page error. At settle 5 the same run
gives 5.1 s / 22.8 s for the whole range (one more packet period in nearly
every hop, with the receiver attached), which is why the default is 2; at
the old dwell of 20 ms (3 packets a hop) 6.5 s / 31.3 s.

**Round 3, 2026-11-21: on a Raspberry Pi 5.** The suite's main target, and
nothing of the mode had run on one. The Pi: Raspberry Pi 5 Model B Rev 1.1,
8 GB, Debian 13, kernel 6.18.34, gcc 14.2, on Ethernet, its own KrakenSDR
(5 tuners behind the board's hub on a USB 2 port). The tree and the fork
were built there (`-j3`: the fork and heimdall 49 s, kraken_doa with
liquid-dsp 198 s, no warning from the mode's code). Every figure at
45 - 55 C with `vcgencmd get_throttled` 0x0. Each part has its own Pi record:
the receiver above (*On the Raspberry Pi 5*: CPU, the engine bench, the
suite on the normal and the ASan + UBSan build), heimdall in
`heimdall_v2/docs/dev/operating-modes.md` (*The failed return on the Pi*,
*On the Raspberry Pi 5*), the library in `docs/dev/librtlsdr-fork.md` (*On a
Raspberry Pi 5*).

The orchestrator's runs there (job scratch `pi/`: `lagprobe.py` on the Pi
against the library built on it; `run_pi.py` = the steps of `run_hw.py`
with heimdall and kraken_doa as port-moved test builds ON the Pi and the
page in headless Chromium on the PC, over the LAN - the way a Pi receiver
is used; CPU per thread from /proc on the Pi), beside the PC's:

| | Raspberry Pi 5 | x86 PC |
|---|---|---|
| Lag probe, 100 operations a pattern on one dongle (full retunes: same frequency, + 100 kHz, 433 MHz and back, 24 / 1700 / 100 MHz, a divider boundary; fast path + finish; gain write; lock read; 1 - 5 register reads) | 0.000 samples each, the control dongle 0.00, 0 failed calls | 0.000 |
| One tune: full / band change / fast + finish | 14.3 / 24.1 / 10.0 ms | 13.8 / 23.6 / 9.5 ms |
| Coherent start | CONVERGED 24.4 / 25.4 s after heimdall was started (two starts) | 17 - 22 s after its control port opened |
| 24 - 1765 MHz, 5 tuners / 1 tuner (through the page) | 4.03 s / 19.0 s (21.2 ms a round: tune 8.6, settle 2.1, gate 1.9; exactly 1 packet a hop) | 4.0 s / 18.9 s (21.3 ms: gate 3.0) |
| 88 - 108 MHz, 5 tuners / 3 tuners | 11.3 / 9.2 sweeps a second (the tunes take 14.2 ms in that band) | 11.3 / 9.4 (14.2 ms) |
| Sweep -> coherent | CONVERGED 0.53 s after the last sweep packet (8091 headers; the phase-only recalibration), check \|lag\| 0.0, \|phase\| 0.1 deg | the same |
| Type 10 frames reaching the page | 20 a second | 20 |
| heimdall, % of one core: sweep / coherent | 23 / 31 - 38 (the conversion worker 17 - 19 / 25 - 32) | - |
| kraken_doa with one page, % of one core: sweep whole range / FM band / coherent | 4.7 / 5.5 - 6.0 / 22 - 26 | 4.6 / - / 17 (the receiver's table) |

20 of 20 steps in each of three runs (before heimdall's safety net of this
round, after it, and after main - Voice to Text's Live text - was merged
into the branch; the PC's run on that last tree 20 of 20 as well, 4.00 s /
18.9 s, 11.8 sweeps a second), 0 coherence events, 0 failed retunes, no gate failed or late, no
stalled or slow tuner, no unquiet flush, no page error, both processes exit
0; no core more than 9 % busy while sweeping (12 - 21 % in coherent mode).
So a Pi 5 sweeps exactly as fast as the PC - the hop is USB and PLL time,
not CPU - and sweep mode is the lightest of the modes on it.

What the Pi found, and what it could not show:
- **One return to coherent that never converged** (the hop-loop worker's
  exit-stress test: 1 of the first 10 returns; then none in 580 more on the
  Pi - 1 in 590 - and none in 95 on the PC). One channel had no correlation
  peak after the re-pairing flush - paired a packet or more off, further
  than the lag correlator sees (inferred from the next flush repairing it;
  the offset itself was never measured) - and heimdall waited for the next
  mode change. Its cause is not known (the library is cleared by 50
  bare-library exits; a lost USB transfer or a pairing cut remain). What is
  fixed is the consequence: a channel without any correlation peak in the
  phase recalibration now raises a coherence loss, and the recovery
  re-pairs and recalibrates by itself (5.2 - 5.7 s with a forced offset on
  the fake librtlsdr, Pi and PC; with a dead noise source one recovery,
  then the same stable uncalibrated state as before). heimdall's doc has
  the measurements; the next occurrence is classified by its log line.
- **The gate's bound cannot be reached on the hardware.** `t_contrast.py`
  with carriers 31 and 36 dB over the floor: at settle 0, 2 and 5 ms no
  valid packet holds a trace of the old frequency (first valid packets
  within 0.4 dB of later ones) and the flagged packets hold it at full
  strength - but the same with the gate taken out. The tune call (7.6 ms)
  is longer than a packet (6.83 ms): the packet across the retune's end
  holds only samples from inside the tune. The bound "retune end + settle -
  the fastest USB delivery" is exercised on the fake librtlsdr only (on the
  Pi as on the PC). Nothing measured on the Pi argues against settle 2.
- **The library is the PC's on the Pi.** The old fork slips 2.000 samples a
  tune there too, the fixed one 0.000 on every path; a control IN of 3, 7,
  11, 15 bytes drops 4 bytes on the Pi's xHCI as well; a tune costs the
  same (7.5 ms on the sweep's path, 14.4 ms full); the PLL settle's margin
  is the same (no lock retry at 5000, 3000 and 2500 us, 1609 in 28 000
  tunes at 1500 us). The flag that skips the dummy read: 59 720 tunes with
  it and 56 555 without, 0 errors of any kind - still without a positive
  control, nobody has produced the stall it guards against. The default
  path, as the scanners tune: 20 000 full-path tunes on each of the five
  dongles in turn with the streams running, the lag afterwards within
  -0.02 ... +0.03 samples of where it was.

**Round 4, 2026-11-21: the waterfall after a colour change and a zoom; finer
resolutions.** Three things the user asked for after the first evening with
the mode: a colour change should recolour the rows already there; rows made
while zoomed in were black outside the zoomed window once zoomed out; and
the whole range stopped at 586 Hz. The parts' records: the contract's
`SWEEP_OVERVIEW` bullet and the `sizes` fields, the receiver's *Finer
resolutions*, the page's *The display* (the overview, the rows) and *Finer
resolutions on the page*. What was changed, in one line each:
- the page repaints its rows on a change of the colour map or the level
  range (it kept the rows as levels already, and repainted them only for a
  zoom);
- the receiver sends a zoomed page the whole range with every completed
  sweep when the page asks (`SWEEP_OVERVIEW`, a type 10 frame with flag 16
  before the flag-2 frame), and a row keeps both: the window's levels where
  that sweep's frame reached, the overview's elsewhere;
- `SWEEP_MAX_BINS` 2^22 -> 2^24 (the cap was the only thing between the
  whole range and 293 / 146 Hz - ~4 bytes a bin), FFT sizes 32768 and 65536
  across the packets of a hop (from dwell 7 / 21 ms, up to ~1.2 GHz / ~614
  MHz of span), the store given back when the mode or a big grid is left,
  and the Resolution list built from what the receiver says is possible.

The orchestrator's runs, on the Pi 5's KrakenSDR again (the dev machine's
was in use: the user's live stack ran from the integration worktree, so
candidates went to the Pi as patches - `git diff A B --output` - before
anything was merged there; job scratch `e2e/wf_smoke.py`, `pi/wf_pi.py`,
`pi/res_pi.py`, `pi/reslist_pi.py`: the page driven with real wheel events
and its own controls, the waterfall's pixels read back):

| | Before | After |
|---|---|---|
| 88 - 108 MHz, zoomed in 9.3x for 50 sweeps, zoomed out: black share of each of those rows | 0.892 (44 rows) | 0 |
| Rows completed while the view was changing (wheel steps out): longest stretch of one colour in a row | 220 px (first version of the fix: the trace's 0 dB padding stored as levels) | 8 px, as steady rows |
| Colour map changed / level range changed, sweep paused: rows of 60 that changed | 0 / 0 | 60 / 60, and new rows match the recoloured old ones |
| 24 - 1765 MHz at `SWEEP_RES` 4096 / 8192 / 16384 | 586 Hz / capped to 586 / capped to 586 | 586 / 293 / 146 Hz: 2 971 307 / 5 942 614 / 11 885 227 bins, the sweep 4.03 s each (Auto: 4.02 s) |
| kraken_doa's memory over coherent mode: Auto / 4096 / 8192 / 16384 | - | + 1.4 / + 11.5 / + 23.3 / + 47 MB; back to + 7 MB on a narrow Auto sweep, + 8 MB in coherent mode again |
| kraken_doa, % of one core, one page: Auto / 4096 / 8192 / 16384 | 4.7 / - / - / - | 4.3 / 8.8 / 10.7 - 13.3 / 13.7 - 16.5 |
| 400 - 470 MHz at 65536 | (16384 was the limit) | dwell 5: capped to 16384, the list says "37 Hz -> 146 Hz ... a dwell of 21 ms gets it"; dwell 25: 36.62 Hz bins, 1 911 467 bins, a sweep in 0.35 s (heimdall: 4 packets a hop, 40 - 47 ms a round), 27 - 30 % of a core |
| A carrier at the zoom limit (7 794x on the whole range at 146 Hz; 1 253x at 37 Hz) | - | 416.587061 MHz at 146.48 Hz spacing, 416.587085 MHz at 36.62 Hz |
| The Resolution list, whole range dwell 5 / 400 - 470 MHz dwell 5, 10, 25 | eight sizes | ten: 73 and 37 Hz "range too wide" / "7 ms dwell", "21 ms dwell", enabled as the dwell allows; a disabled size cannot be chosen |

Every run: no page error, both processes exit 0, 45 - 51 C, throttled 0x0;
the 20 steps of `run_pi.py` on the final tree 20 of 20 (4.03 s / 19.0 s,
11.4 sweeps a second, back in coherent mode 0.53 s after the last sweep
packet). Noted on the way: a row made BEFORE a deep zoom shows as one flat
band at that zoom (it holds ~1500 max-pooled points of the whole range - a
strong carrier fills its pixel); at 146 Hz with dwell 5 a hop is ONE frame
and the trace scatters by ~ +-10 dB until the dwell is raised; a faint
hop-wide banding of the FM band's waterfall that appeared during these runs
was there with the page before and after the change alike (the band, not
the page). The other modes' waterfalls (the live one, the tuner panes) kept
pixels, not levels, and still coloured only new lines after a colour change
(round 5 changed that).

**Round 5, 2026-11-22: the full resolution in the browser; the live
waterfall recolours; a picture of the display.** Three more requests of the
user. The parts' records: the contract's `SWEEP_FULL` / type 11, the
receiver's and the page's *Full resolution*, and - for every mode -
*Spectrum views* (the live waterfall's levels, why Auto's own moves recolour
nothing, the picture). In one line each:
- **Full resolution** (a switch in the Display box, sweep mode only, kept in
  the browser, off at first, Slow link wins): the receiver sends every bin
  of each completed sweep after its flag-2 frame (type 11, in parts paced by
  the socket's buffer, one set at a time per socket, a snapshot when the
  switch goes on). The page draws the trace from the latest set right after
  a zoom or a pan, and a waterfall row keeps its sweep's set within 256 MB
  (64 MB in the phone layout) - older rows fall back to their window /
  overview levels.
- **The live waterfall** (`WaterfallDisplay`, the main one and the tuner
  panes) keeps the level each pixel was painted from (1 byte a pixel) and
  repaints its lines when the USER changes the colour map or Min / Max or
  switches Auto off. Auto's own range moves recolour nothing: measured, they
  come 14 - 25 times a minute on a busy band and each one changed 92 - 100 %
  of the picture's pixels - a whole waterfall that flashes; each line keeps
  the range it was painted with instead. (Sweep mode's waterfall, which
  ranges on the stitched sweep, keeps following Auto as a whole.)
- **The picture**: a camera button right of View saves ONE PNG of the
  display's canvases with a caption line, in the browser only.

The orchestrator's runs on the Pi 5's KrakenSDR (the integrated tree as
patches on the Pi's test build, before the merge; job scratch `pi/r5_pi.py`,
`pi/live_pi.py`, with the earlier checks again):

| | Before | Now |
|---|---|---|
| Live waterfall (coherent), grayscale map chosen: share of the lines already there that are still coloured 0.35 s later | 1 | 0 |
| ... Auto off, Min + 20 dB: mean gray of those lines | 197.2 -> 197.0 | 97.9 -> 6.3 |
| The camera button, coherent / sweep / sweep with Full resolution | - | a PNG each (906 x 986, 1573 x 986), spectrum, waterfall, caption; e.g. `krakensdr_sweep_88-108MHz_20261122_003722.png` |
| Full resolution on, 24 - 1765 MHz at 146 Hz: "Points sent" | 1525 | 1525 + full 11,885,227 (1.2 s after the click: the snapshot) |
| ... rows made BEFORE a zoom of 6 000x onto a carrier: longest stretch of one colour in a row (px of 1525) | 160 - 1525 (one flat band) | 2 - 4 (the sweep's detail) |
| ... kraken_doa on the Pi, % of one core / RSS, one page | 16.7 % / 99 MB | 16.3 % / 113 MB (the set's buffer); heimdall's round 20 ms, 0 failed retunes |
| ... 88 - 108 MHz at Auto (11 sweeps a second), Full resolution on | 5.8 % / 68 MB | 8.0 % / 68 MB, "1552 + full 17,067" |
| The waterfall checks of round 4 with Full resolution on (rows whole after a zoom, no padded stretch, recoloured rows) | - | all pass |

27 of 27 steps there, and on the same build the earlier checks again: the
waterfall 14 of 14, the resolutions 15 of 15, the Resolution list 15 of 15,
`run_pi.py` 20 of 20 (4.03 s / 18.97 s, 11.3 sweeps a second, coherent again
0.53 s after the last sweep packet) - this tree also holds main's Auto detect
change (data_receiver.cpp). No page error, both processes exit 0, 47 - 52 C,
throttled 0x0. "A zoom is sharp at once" is the page worker's measurement
(the zoom and the read-back in one script turn, so that no frame can arrive
in between: the trace equals the brute-force maximum over the set's bins);
the orchestrator's script waits a second after its wheel events and cannot
show it. A lesson of the Pi runs themselves: sources copied with `rsync -a`
keep the PC's older file times, so objects the Pi had built in between
looked newer than a changed source - a link error after main was merged
(`undefined reference to dig::DigitalDecoder::DigitalDecoder(bool, bool)`);
the copies now go by checksum without the times, and test builds start
clean.

**Round 6, 2026-11-22: the display resolution as a list; rows keep the
detail they have.** The user, after an evening with round 5's switch: other
resolutions for the display "via a drop down", and "when I turn off full
resolution, the previous data in the waterfall becomes low resolution ...
we should only be changing the new display resolution, not the old ones
already on the waterfall". The parts' records: the contract's
`SWEEP_FULL:<n>` and `full_sets`, the receiver's and the page's *Display
resolution*. In one line each:
- a change of the display resolution (any choice, Slow link too) no longer
  frees the sets the rows hold - only the trace's set and a set still being
  put together go; a row keeps its set until it scrolls out, the budget
  takes it (oldest first), the grid changes or the mode is left; the trace
  after a zoom draws from a set only when it is of the newest completed
  sweep;
- the switch became the list **Display resolution**: Fit to the plot, the
  receiver's steps (`SWEEP_FULL:<n>` = at most n values: a level of the
  store's max pyramid, 16 384 ... 4 194 304) and Full, each shown with the
  point spacing and count the receiver says it gives for the running grid
  (`full_sets`); a receiver without the field gets Fit / Full only.

The orchestrator's runs on the Pi 5's KrakenSDR (candidates as patches on
the Pi's test build before each merge - the user's live stack ran from the
integration worktree again; job scratch `pi/keep_pi.py`, `pi/dres_pi.py`):

| | Before (round 5) | Now |
|---|---|---|
| The user's case: 24 - 1765 MHz at 146 Hz, full for 5 sweeps, then fit, a zoom of 6 000x onto a carrier: longest stretch of one colour in the rows made with full (px of 1525) | 1525 each (one band: their sets were freed) | 2 - 4 (their detail is still there) |
| ... the rows made after the change | coarse | coarse (38 - 44 px: a gradient between their few points), not black |
| The list, whole range at 146 Hz | a switch | Fit to the plot / 150.0 kHz (11,607 points) / 37.5 kHz (46,427) / 9.38 kHz (185,707) / 2.34 kHz (742,827) / 586 Hz (2,971,307) / Full: 146 Hz (11,885,227) |
| ... whole range at Auto / 88 - 108 MHz at Auto | - | Fit, 150.0 / 37.5 / 9.38 kHz, Full: 4.69 kHz (371,414) / Fit, 2.34 kHz (8,534), Full: 1.17 kHz (17,067) |
| "Points sent" at the 2.34 kHz step | - | 1525 + 742,827 at 2.34 kHz |
| One waterfall with rows made at full / at the 2.34 kHz step / at fit, zoomed 6 000x: longest stretch of one colour | - | 3 - 6 / 26 - 35 / 41 px: each row at its own detail, none black |

15 of 15 and 10 of 10 steps there; on the same build round 5's checks 27 of
27 (through the list), the waterfall 14 of 14, the resolutions 15 of 15, the
Resolution list 15 of 15, `run_pi.py` 20 of 20 (4.04 s / 19.0 s, 11.1 sweeps
a second, coherent again 0.5 s after the last sweep packet); the receiver's
change alone under the page with the switch passed the same set before it
was merged. No page error, 48 - 53 C, throttled 0x0. The receiver worker's
Pi figures for a step: a set of 11.6 KB ... 2.97 MB (full: 11.9 MB), the
store's lock held 0.009 ... 2.2 ms (full: 8.7 ms), the CPU of a page at a
step within noise of none.

**Round 7, 2026-11-22: the Data rate.** The Display box shows what this
browser's connection carries, in every mode (*Spectrum views*: "Data rate"
and "Network rate" - the receiver's `NET_STATS` counters, the page's own
count as an upper bound, and in this mode the rate over whole sweeps, since
a set is one burst a sweep). What the display resolutions cost on the Pi 5's
KrakenSDR, the whole range at 146 Hz (4.02 s a sweep), as the kernel counts
it: Fit to the plot 30 KB/s, the 2.34 kHz step 208 - 216 KB/s, Full 2.97 -
3.00 MB/s (a longer dwell: 12 MB a sweep whatever its length - 1.56 MB/s at
7.8 s, 0.86 MB/s at 14 s); the FM band at Full 213 - 216 KB/s; for
comparison coherent mode 28 KB/s, wideband scan 28, independent 46. The
record, with every shown value against the kernel's count, is in *Spectrum
views*.

**Round 8, 2026-11-23: the sets compressed** (`SWEEP_FULL_Z`; the receiver
section's "Compressed for a page that asks", the page section's deflated
parts). The user asked whether the full-resolution data could be compressed
better, then: "Implement the sweep full resolution compression".

*What was measured first* (real sets taken from the dev machine's KrakenSDR
through the page, 24 - 1765 MHz, gain 49.6: job scratch `e2e/sets`,
`cap_full.py`, `comp_study.py`, `comp_more.py`, `comp_time.py`). The 146 Hz
set, 11.9 M values: 17 % of them 0, 0.6 % more than 10 dB over their block's
median - one FFT a point, so nearly all of it is noise; 5.96 bits a value
over the whole set, 4.79 given the value before.

| coding (lossless) | size | one core, x86 / Pi 5 |
|---|---|---|
| deflate, Huffman only (the one built) | 60.1 % | 83 / 186 ms |
| deflate level 1, default strategy | 66.4 % | 212 / 297 ms |
| deflate level 6 | 72.6 % | 340 ms / - |
| bzip2 / LZMA | 72 % / 69 % | far slower |
| an adaptive coder that knows the value before (bound) | 58 % | - |

No gain from the differences against the sweep before (5.74 bits a value:
the noise is new every sweep) or from taking the local floor out first
(61 - 63 %). More frames a point compress better: dwell 21 ms 51.6 %, the
586 Hz grid 55.0 %. The sets max-pooled to a step: 61 - 63 % (37.5 kHz:
69 %). Not built, lossy (not asked for): level steps of 0.94 dB in place of
0.47 would give 49.8 %, 1.88 dB 39.0 %. "Should we only compress when the
sweep takes longer than the compression?" (the user): it never comes near -
2.1 % of the sweep time on x86, 4.7 % on a Pi 5 at most, and both grow with
the points; the rule in use is the receiver's deadline (a part not ready in
time goes raw). The live modes were looked at too and left alone: their
spectrum (type 7) already goes through permessage-deflate to 62 - 67 % of
its bytes (coherent mode's 27.7 KB/s on the wire: spectrum 11.1, the DoA
message 7.5 uncompressed, audio 4.0, JSON 3.9; `e2e/live`, `live_study.py` -
the wire rebuilt from the captured messages matches the receiver's counter
within 1 %); coding each frame against the one before, or a window kept
across frames, would save 2.4 / 8.6 / 5.3 KB/s (coherent / independent /
wideband scan), and Huffman-only is no better there (the averaged spectrum
is smooth).

*End to end on real hardware* (the page in headless Chromium; the reference
is the kernel's own count of each connection, as in round 7). The Pi's
receiver has been a KrakenSDR Wideband since round 7 (plain mode: sweeps of
its noise, nothing converges), so the checks that need signals ran on the
dev machine's KrakenSDR (the Pi checks with the stack local), and the Pi ran
what needs none. The compression check (`zip_pi.py`): a normal page and a
page whose browser has no `DecompressionStream` (it must not ask) take the
same sweeps.

| | dev machine (x86), its KrakenSDR | Pi 5, the Wideband unit's noise |
|---|---|---|
| the check | 40 / 40 | 40 / 40 |
| the two pages' sets of the same sweeps, at Full (146 Hz), 2.34 kHz, 586 Hz, the FM band at Full | identical, value for value | identical |
| the normal page's parts deflated | all (115 of 115 at Full) | all |
| size at Full / 2.34 kHz / 586 Hz / FM | 0.600 / 0.606 / 0.629 / 0.583 | 0.545 / 0.513 / 0.580 / 0.419 |
| a third page at a step beside one at Full | both all deflated | both all deflated |
| every set whose last part arrived completed (the inflate is asynchronous) | yes, both pages | yes |
| Data rate, Full: the deflating page / the raw page (the two add up to the wire) | 1.82 / 3.01 MB/s | 1.64 / 2.98 MB/s |
| `SWEEP_FULL_Z:0`, then `:1`, from the page | raw, then deflated again | the same |
| the receiver, one page at Full, raw -> deflating (% of a core) | 8.7 -> 10.4 | 14.6 -> 19.4 (the compressor thread 5.4) |
| packets dropped by the engine; page errors; exit codes | none; none; 0 | none; none; 0 |

The Data rate after the change on the dev machine's KrakenSDR (shown,
against the kernel within 3 %): the 2.34 kHz step 217 -> 143 KB/s, Full
3.03 -> 1.82 MB/s, the FM band at Full 228 -> 138 KB/s, a 14 s sweep at
Full 873 -> 424 KB/s; Fit to the plot and the other modes as before (30,
28, 47 KB/s). The Pi's earlier checks there with the pair: the display list
15 / 15, the kept rows 10 / 10, round 5's 27 / 27, the waterfall 14 / 14, the
resolutions 15 / 15 and 15 / 15, end to end 20 / 20, the Data rate 19 / 19 -
the same before the change at 82572e1, which also carries main's da35b40
(heimdall's retune cooldown, the Wideband mixer crossover). Mixed versions
on that hardware: the new receiver under the page of before (it never asks:
raw, identical sets, 24 / 24, end to end 20 / 20) and the new page on the
receiver of before (it asks, the receiver ignores it: raw, 36 / 36, the
display list 15 / 15, the kept rows 10 / 10).

Misses, none in the compression. The Pi's "CONVERGED" step (the Wideband
unit). The steadiness check (round 7's, every shown Data rate against the
kernel): before the change, on the dev machine loaded by other tests, the
2.34 kHz step shown 207 - 217 KB/s against a reference of 194, and the FM
band's values 6.6 % apart (5 % allowed); with the pair, at Fit to the plot -
where no set is sent - 28 - 30 KB/s, 6.8 % apart; on the Pi after a resume
1.57 - 1.61 MB/s, steady, against a reference of 1.75. In the two reference
cases the check's window is a whole number of sweeps of the duration it
read at its start (after the resume 4.22 s read, 4.03 real), which puts the
reference off by up to a set in eight. The display list's "each row keeps
its own detail" once of four runs (the new receiver under the page of
before): the rows in the right order - longest one-colour stretch 5 - 15 px
at Full, 41 - 101 at the step, 1347 at Fit - but the step less than the five
times coarser the check asks for; it depends on what is on the air at the
zoomed place. Before the change "pause stops the sweep" counted one more
sweep once on the FM band (11 a second, localhost: the step's own race;
passed in the three later runs).

After main's 93be402 (the Inmarsat decoder plugins; outside them 21 lines
of the decoder engine and 3 of the page) was merged in, the dev machine's
KrakenSDR again: the compression check 40 / 40 (0.599 of raw at Full, 1.82
against 3.02 MB/s), the Data rate 19 / 19, the kept rows 10 / 10, round 5's
27 / 27, the waterfall 14 / 14, the resolutions 15 / 15 and 15 / 15; the
display list 14 / 15 and end to end 19 / 20 with the two steps named above
("each row keeps its own detail": 4 - 9 px at Full, 27 - 38 at the step,
1299 at Fit; "pause stops the sweep" 73 -> 74 - both checks ask for more
than the page promises: a step of four bins is not five times coarser, and
the sweep in flight ends after the pause is acknowledged); the steadiness
check 23 / 26 - coherent mode 27 - 29 KB/s (one KB/s either side of 28 is
7 %), Fit to the plot 28 - 30 KB/s against 30.0. That last one is round
7's rate over whole sweeps with the receiver's counters where there is no
set: the receiver's sample is up to a second older than the sweep's end, a
second of the 30 KB/s trickle is 6 % of four sweeps' bytes, and with a
sweep of 3.98 s the two clocks drift against each other (steady on the
Pi's 4.02 s sweep). Harmless, and removable: at Fit to the plot the 5 s
window would do.

Not tested: Firefox, Safari, iOS (a browser without `DecompressionStream`
stays raw; with it, untried there); a slow link with deflated parts; a
Windows or macOS receiver (zlib is in their link lines; not built); a Pi 4;
the thread sanitizer (unusable on the dev machine's kernel - the receiver
section).

**Round 9, 2026-11-23: a gain per hop (Auto gain).** The user: "for
broadcast FM we get best results with the gain maxed. But for 414 MHz
business bands, we get extreme overload. Could we auto adjust the gain
based on the current swath, and still have the sweep update fast, and have
the sweep stitch together well?" The parts: heimdall's rule and its
measurements (`heimdall_v2/docs/dev/operating-modes.md`, "Auto gain in sweep
mode"), the receiver and page sections here (`SWEEP_AUTO_GAIN`, the
`sweep_gain` map, the levels), the tuner library's call
(`docs/dev/librtlsdr-fork.md`, "A fast tuner gain write").

*What was measured first* (the dev machine's KrakenSDR on the user's
antenna, 24 - 1765 MHz, 911 hops, five tuners, dwell 5 ms; heimdall's 8091
stream read raw, per hop the share of samples at 0 / 255 and the level
against full scale - job scratch `e2e/gain_probe.py`):

| gain | hops with clipped samples | the worst |
|---|---|---|
| 49.6 dB (the list's top) | 53 of 911 | 457.2 MHz 26 % of its samples, 415.1 MHz 25 %, 499.3 MHz 15 %, 759 - 801 MHz 6 - 15 % |
| 38.6 | 36 | 415.1 MHz 23 % |
| 28.0 | 25 | 415.1 MHz 18 % |
| 16.6 | 0 | (415.1 MHz at -10.5 dBFS; every quiet hop on the converter's own floor, -48 dBFS) |
| the tuner's own AGC | 132 | 457.2 MHz 26 % |

The FM band at 49.6: -38.7 dBFS, nothing clipped. So one gain for all hops
is wrong everywhere, and the chip's AGC is worse than the maximum.

*What turned up on the way* - each by a measurement, none foreseen:
- The engine has ONE noise reference a sweep, not one a hop: a hop taken
  33 dB lower would have been a 33 dB hole. So each hop's levels are
  shifted by what its gain lacks to the ceiling.
- The tuner's gain list is not true: 49.6 -> 8.7 dB is -34.2 dB, not -40.9
  (and 2 - 3 dB more or less by frequency). heimdall publishes the measured
  curve and the shift uses it.
- A gain write cost 9.3 ms - more than the retune - and made a sweep 5 - 6 %
  longer; the fork's write inside the tune: one more USB transfer.
- The first rule (18 dB down at 1 % clipped) left hops 8 - 20 dB lower than
  needed for minutes: now a probe straight back up after a step down.
- A hop beside a saturated one is spoiled by products without clipping
  (they fall 29 - 44 dB for 11 dB of gain): held 12 dB under the ceiling.
- heimdall's lag filter carried 3 samples across a flush into the next
  hop's first packet (1 run in 10 on the fake; 7 packets in 10 mode entries
  on the KrakenSDR): fixed, in every mode.
- The seam steps between two tuners' blocks: from hops at the ceiling only.

*End to end on the dev machine's KrakenSDR* (heimdall 1544126 with the
fork's patch 0003, receiver ae8e4fa, page 115be78; the reference is again
heimdall's own stream read raw, beside the page: job scratch `pi/agc_lib.py`,
`agc_h.py` 22 / 22, `agc_pi.py` 23 / 23):

| | |
|---|---|
| manual, 49.6 dB | 53 - 56 hops clip in every sweep (455 - 457 MHz 23 - 52 % of their samples, 415.1 MHz 22 - 45 %, 499.3 MHz 15 - 26 %) |
| Auto, through the page's switch: hops clipping, sweep after sweep | 41, 22, 8, 6, 2, 1, 1, 1, 1, 0, 0, 3, 1 (over 1 % of the samples: 24, 3, then none) |
| the same through 8092 | 27, 16, 10, 1, 3, 1, 1, 0, 1, 3, 2, 2, 0, 0 |
| 25 sweeps on | 0 - 3 hops clip in a sweep, 0 - 3 change gain; 65 - 70 hops reduced, the lowest at 8.7 dB; the FM band's 11 hops at 49.6 |
| a sweep, manual / Auto | 3.948 / 3.939 s and 4.094 / 4.117 s (-0.2 % and +0.6 %), 48 - 50 gain writes; with the ordinary gain call +5.8 - 6.1 % |
| the page's map against the stream | equal on 911 of 911 hops; "≤ 49.6 dB · 67 lower, min 8.7 dB"; the readout "415.100 MHz  41.9 dB  gain 8.7 dB" |
| 415.1 MHz (its hop at 8.7 dB) | the strongest carrier 80 dB (clipped) -> 98.4 dB; the hop's median 42.4 dB against 35.8 beside it (with the list in place of the curve: 49.4, the carrier 105.9) |
| 457.2 MHz (22.9 dB) | 75.8 -> 85.6 dB |
| the FM band and 1400 MHz (no hop reduced) | their signals as high over the hop's median as in the manual sweeps before and after (25.0 / 26.8 / 26.8 dB; 5.6 / 6.1 / 6.1) |
| Auto off; on again; the ceiling to 38.6 and back | every hop at 49.6 within a sweep, 53 - 55 clip again; none clip from the first sweep; no hop over the ceiling, what was learned kept |
| sweep -> coherent with Auto on, and back | CONVERGED 0.6 s after the switch, the five tuners at 49.6; back: Auto on (the receiver replays it), the learned gains in use |

At 414 - 418 MHz the manual picture is a forest of false peaks every 125 -
250 kHz at 40 - 60 dB; with Auto they are gone, the real carriers stand on a
flat floor, and the amber strip marks the two hops. Once the air itself
changed in the middle of a run - 104 hops clipping from one sweep to the
next (manual showed 102 - 103 for some minutes after, then 53 again): Auto
had it at 2 or fewer in six sweeps, with 177 hops reduced.

The earlier checks on that hardware, Auto off: the compression 40 / 40, the
Data rate 19 / 19, the display list 15 / 15, the kept rows 10 / 10, round 5's
27 / 27, the waterfall 14 / 14, the resolutions 15 / 15 and 15 / 15, end to
end 20 / 20; the steadiness check 22 / 26 (the small rates' wobble, as in
round 8). With Auto on: 40 / 40, 19 / 19, 15 / 15, 10 / 10, 14 / 14, 15 / 15,
15 / 15, 20 / 20.

Noted on the way, not from this round: the FM band's displayed level sits
at one of two offsets 5 dB apart from one minute to the next, in manual and
Auto alike (block 0 hangs on the 371 | 373 MHz seam step, and a signal or
a clipped hop within six hops of a seam moves it - the receiver section);
the floor steps 11 dB at the 719 | 721 MHz seam (over the 6 dB the seam
logic corrects). And a lesson for the tests: another job on the machine
stopped ITS test receiver with `pkill -x kraken_doa_test` and took this
job's with it in the middle of five checks (rerun under a process name of
their own).

After the merge (the four branches; the fork's patch applied in the
checkout's `librtlsdr/`) and again after main's 52cb2da (more of the
Inmarsat plugins, heimdall's generator mode for the Wideband board's LO),
built from the merged tree: `agc_pi.py` 23 / 23 both times (a sweep 3.939 /
3.939 s and 3.951 / 3.941 s manual / Auto), end to end 20 / 20, the
compression 40 / 40, the kept rows 10 / 10, round 5's 27 / 27, the Data
rate 19 / 19, the waterfall 14 / 14, the resolutions 15 / 15 and 15 / 15;
the display list 14 / 15 (its too-strict step: the rows in the right order,
4 - 9 px at Full against 41 - 102 at the step).

On a Raspberry Pi 5 (a clean build of the merged tree with the patched
fork; its receiver is a KrakenSDR Wideband run in plain mode - sweeps of
its noise, where nothing clips, so no hop is reduced): heimdall reports
`gain_fast` and publishes the curve, the page shows "≤ 49.6 dB · none
lower" with a map of 911 hops; a sweep takes 4.021 s with Auto off and
4.022 s with it on; heimdall 19.7 -> 20.7 % of a core (the per-hop
statistics in its conversion thread, 12.8 -> 13.9), the receiver 17.1 ->
17.4 %; the compression check 40 / 40 on that build. Not measured there: a
gain write (none was needed), the rule on real overload.

Not tested: Windows (expected about 0.15 s a sweep
with the fork's call, about 1 s without); generic RTL-SDRs on hardware
(other tuners take the ordinary gain write and have no measured curve);
more than a few minutes of steady state; the neighbour cap's release; the
curve on a second unit; Firefox / Safari.

**Round 9b, 2026-11-23 evening: "Auto gain doesn't seem to be working".**
The user, on their own stack (the round-9 build; 60.9 - 1641 MHz, 827 hops,
the gain at 49.6, a browser on another machine): "I've set the max gain to
49.6dB, and turned auto gain on. But i'm still getting severe overload in
some bands". What it was - three different things, found by reading that
stack's status (8092 `get_status`) and viewing its page with a second,
view-only browser at the user's word, never by sending it a command:

- *Auto gain worked.* When the report came the switch was off in the
  receiver and in heimdall (the user had switched it off again); switched on
  later, heimdall had 70 hops reduced at once - its map from earlier that
  evening - and 0 - 4 hops clipping a sweep, the lowest gain 14.4 dB. At
  410 - 420 MHz and over the 758 - 803 MHz mobile band the page showed the
  real carriers and blocks, no false peaks. Nothing in the page or the
  receiver could show the switch on while it was off (the page worker ran
  54 flows with real mouse, key and touch events: label, keyboard, touch,
  gain / range / mode changes with it on, reloads, a second tab, restarts
  of either program - the switch, the receiver, heimdall and the saved
  setting agreed after each); the one hole - a click while the WebSocket
  was down went nowhere - is closed (the page section).
- *What still looked overloaded is not overload.* Between about 135 and
  520 MHz the floor stands 20 - 30 dB over the floor above 1.1 GHz, and at
  160 MHz it is a comb: a tooth every 250 kHz (160.07, 160.32, 160.57 ...),
  each about 60 kHz wide and 15 - 20 dB over the gaps; by 435 MHz the teeth
  have run together. In the morning's full-resolution set the spacing was
  251.1 - 251.4 kHz (in the evening about 250.1: it drifts), the teeth 20
  kHz wide at 140 - 170 MHz and 35 - 60 kHz at 250 - 350, none at 1.2 GHz -
  the harmonics of a free-running switching converter. Its level follows
  the gain one for one (four gains, the morning's probe: 1 - 2.7 dB off over
  17 dB), no hop of it clips, and it does not follow what is on the air at
  2, 3, 4 or 5 times the frequency (correlations 0.12, 0.08, -0.23, -0.31 -
  not the mixer answering at LO harmonics). The other KrakenSDR in the same
  house showed the same hump. So it reaches the tuners' inputs - through
  the antennas or along the supply; no gain setting changes its ratio to a
  wanted signal, and Auto gain rightly leaves it alone. (Job scratch
  `e2e/lin_probe.py`, `img_study2.py`, `comb_study.py`, `pi/live_look*.py`.)
- *A real defect, seen in that session's gain map:* every hop of one
  tuner's block (1326 - 1641 MHz, 166 hops) at 48.0 dB. One hop of it had
  been reduced and raised again to 48.0; after it every hop wanted 49.6, a
  step of 1.6 dB, and steps up of less than 3 dB were skipped (round 9,
  when a gain write cost 9 ms) - for the rest of the block and the next
  sweeps. The page said "237 lower" and drew its strip under a quarter of
  the range; heimdall's own count said 72. Fixed in heimdall (its section
  "Auto gain in sweep mode"): no skip with the fast gain write, at most one
  hop without it; its counts are of the gains used. Its test found a second
  one: a hop could stay low for good after one loud visit, because the
  hold's highest peak never decayed.

The two fixes on the dev machine's KrakenSDR (heimdall 3513728, page
3104767; `agc_h.py` 23 / 23, `agc_pi.py` 23 / 24): no hop just under the
ceiling in any of the last six sweeps (the run of 166 gone), heimdall's
count the stream's (70 / 70), a sweep 3.998 -> 4.004 s and 3.939 -> 3.941 s
manual -> Auto, hops clipping after the switch 21, 6, 11, 4, 6, 1, 1, 2, 3,
0, 2, 0, 0, 1, then 0 - 4 a sweep (0.7 on average) with 0 - 3 hops changing
gain; coherent again 0.6 s after the switch. The miss: in the page run the
12th sweep had 5 hops clipping where the check allows 3 (46, 17, 15, 3, 8,
2, 2, 4, 0, 2, 3, 5, 0 - late-evening traffic bursts; one of them over
1 %). End to end 20 / 20 with Auto off and on, the display list 15 / 15,
the compression 40 / 40, the kept rows 10 / 10.

Again from the merged tree after main's 30cabcc (the receiver's HTTPS
certificate authority, the telemetry logs; one conflict, the receiver's
Makefile source list - both entries kept): `agc_pi.py` 23 / 24 with the same
miss (43, 16, 14, 4, 4, 2, 6, 1, 0, 0, 3, 6, 0 hops clipping; none over 1 %
after the 4th sweep), a sweep 3.953 -> 3.977 s, no hop just under the
ceiling; end to end 20 / 20, the compression 40 / 40, the kept rows 10 / 10,
round 5's 27 / 27, the Data rate 19 / 19, the waterfall 14 / 14, the
resolutions 15 / 15 and 15 / 15; the display list 14 / 15 and, with Auto on,
end to end 19 / 20 (the two steps that ask too much, rounds 8 and 9). So in
the settled state a sweep now and then has up to 6 of 911 hops with a few
clipped samples (under 1 % of the hop's) - by the rule: a first mild clip
is only noted, and since the hold starts over a hop climbs back sooner.
Whether that is the late evening's traffic or the price of the hold's fix
was not separated (the midday runs before it had at most 4).

**Round 10, 2026-11-24: "the overload flag isn't good enough".** The
user, with round 9b's Auto gain on: "I'm still seeing overload. For example
the SNR of the narrowband signals at 416 MHz get alot better if I turn the
gain down. Maybe the overload flag isn't good enough. It may not activate
when there is still intermodulation causing reduced SNR." They were right
about the flag, and the first answer proposed (per hop the gain with the
lowest noise floor referred to the input) was wrong as well. What the
hardware said, and what was built (heimdall: its `operating-modes.md`, "The
linearity search"; the receiver and the page: their sections above):

- *The measurements* (the dev machine's KrakenSDR, round 9b's builds, 01:13 -
  01:48; job scratch `pi/imd_probe.py`, `imd_full.py`, `imd_pairs.py` and
  their `*_study.py` / `imd_comp.py` / `imd_full_lin.py`: every gain of the
  list in turn, per hop the averaged spectrum of heimdall's own 8091 samples
  - floor, total power, strongest carrier, clipped share; and the whole plan
  with the common gain alternating sweep by sweep, so that every hop has
  before / trial / after triples).
  - The cause at 416 MHz is saturation by a carrier OUTSIDE the hop's
    sampled band. The 415.64 MHz carrier clips the converter where it is in
    band (20 - 29 % of the samples above 33 dB gain, none at 29.7). In the
    hop above it - 417.02 MHz in the whole plan, 417.43 with one tuner: the
    carrier 1.4 - 1.8 MHz below the centre, outside the 2.4 MS/s band,
    inside the tuner's IF filter - the output stands pinned at -8 ... -9
    dBFS from 49.6 down to 30 - 34 dB gain, the 416.59 MHz control channel
    is compressed by 12 - 16 dB at 49.6, ghost carriers stand 35 - 42 dB
    over the floor, and 0.00 - 0.07 % of the samples clip: the decimation
    filter removes the carrier after the damage. Carrier over floor there:
    52.9 dB at 49.6, 56.5 at 37.2, 59.9 at 29.7 (whole plan); 60.7 / 63.5 /
    69.3 at 49.6 / 37.2 / 33.8 (one tuner).
  - The floor referred to the input looks BEST at the saturated top gain
    (compression lowers the small-signal gain): a rule on it stays at the
    ceiling.
  - The hop below the carrier and the second hop above are linear at every
    gain: round 9's cap (12 dB under the ceiling within 2.4 MHz of a hop
    held far down) was too high for the one and pointless for the others.
  - The gain list's steps are not the curve's everywhere: 49.6 -> 48.0
    changes the output by +1 dB at 86 - 98 MHz, +0.7 dB at 633 - 681 MHz,
    -1.1 dB at 411 MHz; 49.6 -> 43.4 is 5.5 dB at 174 - 380 MHz (curve 4.2).
    At 86 - 98 MHz the output at 49.6 is only 6 - 17 dB over the converter's
    own noise (-45.0 dBFS rms): every dB of gain is sensitivity there.
  - Round 9b's Auto in the same hour: 417.02 at 37.2 (the cap) for 36
    sweeps and then 29.7 by two mild clips; 413.19 and 418.93 capped to
    37.2 for nothing; 415.11 at 19.7 where 29.7 was clean.
- *The rule now*: per hop the highest gain at which the chain is linear.
  heimdall takes trial visits a step lower or higher (valid packets, their
  own gain in the header, the 8091 trial bits) and compares the hop's total
  power and floor with the visits before and after; a first guess comes
  from the level of the carrier in the hop below when that carrier lies
  within 2.2 MHz of the hop's centre; the clipping rule's cuts and raises
  follow the measured level. The receiver leaves every trial visit out of
  the picture; the page says "settling" while more than 1 % of the steps
  are being searched.
- *What the first hardware runs of each candidate found* (heimdall worker's
  batches and the orchestrator's sets, the trace `HEIMDALL_AG_TRACE=1`): a
  search that never ended on hops whose trials gave no verdict (the LTE
  block probed 8 dB down every other sweep); trials on hops where nothing
  can be measured (the FM band, above 1 GHz); 31 - 39 trials a sweep when
  settled; a tolerance of "1.5 dB or half the step, the larger" that let
  2.2 dB of compression pass (the orchestrator's specification - the
  smaller is right); a mildly clipped baseline that cancelled every trial
  of the hop; an up-trial that passed on a constant carrier 0.7 of full
  scale and clipped afterwards; the page's "settling" that never ended (a
  few bursty steps are always being tried again); down-trials drawn into
  the picture (a floor several dB up for one row where the tuner's noise
  is near the converter's). All fixed, each with its numbers in the
  section it belongs to.
- *The last pass* (heimdall 62da1b1, receiver 64e2e63, page 374f432 - the
  merged tree's sources, byte for byte; 10:42 - 12:48, a busier band than at
  night: the 415.64 and 416.59 MHz carriers about 6 dB stronger, a
  transmitter at 413 MHz keying every minute or so, so each run is judged
  against a manual scan of the same hour):
  - Whole plan, cold start, 60 sweeps: 417.02 MHz at 29.7 dB by the 6th
    sweep, then 25.4 and 22.9 as its carrier grew; carrier over floor 59.7
    dB against 60.1 at the scan's best gain. 415.11 at 16.6 = the scan's
    best gain (61.7 dB). 418.93 follows the keyed 417.86 MHz carrier (32.8
    ... 44.5; the scan's best 38.6). 411.28 and 420.84 at the ceiling.
    Nothing below 108 MHz, in 174 - 380 MHz, 620 - 690 MHz or above 1 GHz
    lower when settled (one hop, 321.39 MHz at 44.5 in some runs: the
    clipping rule's - wideband content at -9.4 dBFS rms). Trials 6 - 13 a
    sweep (at most 1.4 % of 911), the search down to 1 % of the hops by
    the 17th sweep, no gain decision in the settled sweeps, a hop over 1 %
    clipped at its own gain after the 4th sweep in 2 of 30 windows (the
    transmitter at 413 MHz, a burst at 415.11).
  - One tuner, 405 - 428 MHz: 415.52 at 25.4 (the scan's best: 68.6 dB),
    417.43 at 29.7 (the scan: clean from 33.8 down; 66.6 dB there, 69.0 at
    its best), 419.34 at the ceiling, 413.61 at 29.7 (its transmitter:
    4.7 % clipped at 49.6 when it keys). The heimdall worker's five cold
    starts of this plan: 417.43 within one list step of 33.8 by the 8th
    sweep in all five.
  - Round 9's whole chain (`agc_pi.py`) 23 / 24: a sweep 3.946 s manual,
    3.942 s with Auto; the gain map equal to the stream's own gains on all
    911 hops; coherent again 3.7 s after the switch with Auto on, what was
    learned used on the way back. The miss: hops over 1 % clipped after the
    switch 29, 23, 6, 2, 1, 0 ... - the check allows 1 from the 4th sweep
    (round 9b: 0 from the 4th; the trial sweeps and the level-sized cuts
    cost about two sweeps on the heaviest hops).
  - The page against the real heimdall (`page_r10.py`): with Auto off the
    counts are there and the new words show; after the click "settling"
    from the 2nd to the 14th sweep, then "≤ 49.6 dB · 62 lower, min 16.6
    dB"; the tip "65 steps lower: 61 because the converter clipped, 4 for
    a strong signal nearby (no clipping); 11 trial visits in the last
    sweep; 4 steps still being tried; 930 trial visits ... not drawn";
    the gain map still while trials run; no page error.
  - Rounds 3 - 8's sets with Auto off and with Auto on (20 sets, 425
    steps): 422 pass - the Data rate 19 / 19 and 18 / 19, the rate over
    whole sweeps 26 / 26 and 25 / 26, the display list 14 / 15 and 15 / 15,
    the kept rows 10 / 10 twice, round 5's 27 / 27 twice, the waterfall
    14 / 14 twice, the resolutions 15 / 15 four times, end to end 20 / 20
    twice, the compression 40 / 40 twice. The misses: the display list's
    row-detail step (the one that asks too much, round 9b); a rate that
    wobbled 6.6 % where the step allows 5; and with Auto on the readout
    141 KB/s against 117 on the wire at that moment - in the search's
    trial sweeps about 30 % of the hops are not redrawn, so less is sent,
    and the readout is an average over 15 s.
  - 20 minutes (300 sweeps): the quiet bands never move, 2 - 3 trials a
    sweep, 3 - 9 hops being tried again, the search over at the 16th sweep.
- *The cost on a Pi, found after that pass* (the test Pi 5; its receiver is
  the Wideband unit run in plain mode - noise only, nothing to adjust;
  `agc_cpu_pi.py`): with Auto on a sweep took 4.142 s against 4.021 s,
  +3.0 %, with no gain write at all - the conversion worker computed the
  five tuners' spectrum estimates before the set was counted, and the hop
  waited for them (7.12 -> 7.67 ms a hop; on the dev machine +0.1 %).
  heimdall a352c26 computes the estimate on its own thread and learns from
  a visit when its result is there: the same check then reads 4.021 s with
  Auto off and on, heimdall 18.0 -> 21.3 % of a core, the receiver 15.4 ->
  15.5. Its first form numbered the visits with one counter for all
  tuners, which lost every result when 2 or 4 tuners sweep (the review;
  the heimdall worker's fake: 99 late visits against 0); per tuner now. On
  the KrakenSDR with that build (`est_tuners.py`: 380 - 520 MHz with 4, 2,
  5 and 1 sweep tuners, 25 s each): no estimate late in any sweep, 121 /
  85 / 57 / 80 pairs kept; the whole plan and one tuner as in the last
  pass (417.02 at 28.0 by the 4th sweep, then 22.9 - that hour's best
  gain; a sweep 3.941 s manual, 3.942 s with Auto); the page 19 / 19,
  round 9's chain 22 / 24 (the clipping wobble; an FM station's peak
  moving 21.2 / 26.8 / 24.0 dB over a median that stood still), end to end
  20 / 20 with Auto off and on. And once more from the merged tree's own
  builds: the page 19 / 19, end to end 20 / 20 twice, round 9's chain
  23 / 24 (hops over 1 % clipped after the switch 31, 26, 9, 2, 0 ...; a
  sweep 3.939 s manual, 3.951 s with Auto).
- *Limits, measured and left*: (1) a hop whose own carrier stands within a
  few dB of full scale and varies (415.11 MHz: -1.1 dBFS rms at 29.7 dB on
  a quiet night, 5 dB more at midday) is cut at each burst and comes back
  by the clipping rule's hold, which doubles: at the end of the 20 minutes
  it stood at 8.7 dB where the scan's best was 16.6 - about 5 dB of
  sensitivity on that one hop. (2) After a strong transmission the hop
  keeps the gain it needed (413 MHz: 29.7 dB) until the hold is over -
  seconds after one event, minutes when it keeps keying; the price of not
  clipping a row at every key-up. (3) The strong-carrier hops settle one
  or two list steps under their best gain (the headroom: -7 dBFS rms for a
  hop's own carrier, -2 dBFS for the neighbour's), 0.4 - 2.4 dB of carrier
  over floor in these runs. (4) heimdall's two fake scenes that end about
  2 dB short of their best (products just over the converter's noise; a
  keyed carrier). (5) Levels: the client still shifts a hop by ONE curve,
  and the real steps differ by band (above) - a hop far down can stand 1 -
  2 dB off its neighbours. Not run: the Pi with a KrakenSDR (the test Pi
  carries the Wideband unit: the build, the cost and the fake only - no
  strong signal to turn down there), a Pi 4, Windows and macOS, Firefox /
  Safari, a fork without the fast gain write on hardware.

Open after round 6, as after round 5: Firefox, Safari and iOS have not run the picture's
download or the full sets (headless Chromium only); a slow link was tried
through a 2 MB/s proxy by the page worker, not over a real one; a 1 GB Pi 4
(the full set adds one buffer of `bins` bytes in the receiver - 12 - 17 MB
at the finest grids - and holds the store's lock 8.5 ms a set on the Pi 5,
unmeasured there). And as before: the fork's commits are local (the Dockerfile's pin and
heimdall's minimum stay 578b43d; an older fork gives the round-1
behaviour). The cause of the one failed return. A Raspberry Pi 4 (another
USB controller and a third of the CPU: the 9 % a core that sweeping takes
on the Pi 5 leaves room, but it is not measured - nor is the 48 - 64 MB of
the finest grids on its 1 GB; the receiver worker's proposal, not built: a
cap from the memory available), Windows and macOS have
not run it; nor has a browser on the Pi itself. At 1.56 - 1.75 GHz a dongle
of the PC's KrakenSDR can read 13 - 15 dB louder when it is tuned there
from above (seen on downward and random tunes, not on the sweep's upward
steps; the old library does it too; cause unknown; the Pi's KrakenSDR
showed it on another dongle, at 1698.4 MHz, three times in a plan of random
jumps and never in a sweep).

**Round 11 (2026-11-24): Auto gain out, gain by region in.** The user,
after rounds 9, 9b and 10: "Auto gain doesn't work well. Remove it.
Instead we'll add a feature where we can manually set the gain for
different regions of the spectrum. We'll have a line under the FFT
spectrum that we can add keyframe like points to. Clicking on one segment
of the line will change the gain control for only that currently selected
segment. Keyframe points can be removed by right clicking on them. The
keyframe points should be able to be moved left or right by clicking and
dragging. Remove the auto gain button and feature, and implement this
instead". Removed: heimdall's `set_sweep_auto_gain` with its clipping rule,
linearity search, spectrum estimator thread, `HEIMDALL_AG_TRACE`, the 8091
trial bits (never released) and its status counts; the receiver's
`SWEEP_AUTO_GAIN` (an old page's is ignored, an old saved
`sweep_auto_gain` dropped at the next save), the trial-bit handling, the
pass-through counts and the `sweep_gain` map message; the page's switch,
"settling", the amber strip. Kept as the base of the new feature: the gain
per hop written with its retune and carried in the header, every tuner
back at the common gain before the mode is left, heimdall's gain curve and
the receiver's shift by it, the seams' and the base's medians. Built: a
gain table by region - heimdall `set_sweep_gains`, the receiver
`SWEEP_GAINS:`, the page's lane (the contract, *The receiver* "Gain by
region", *The page* "Gain by region"). Tests with the fakes only (job
scratch 52196024 `regions/`; the KrakenSDR was in use by the live stack;
the orchestrator runs the hardware):
- heimdall, its fake build (`h/t_regions.py`, `h/t_regcost.py`; the
  numbers in heimdall's `operating-modes.md`, "Gain by region"): every
  valid packet's header gain on the plan's tuners is its hop's region's,
  border hops included, for 5 / 4 / 2 / 1 sweep tuners; a table, a change
  of a region's gain and border, and the clearing in force 0.00 - 0.01 s
  after heimdall's answer, the plan running on; a strong carrier's level
  minus the header gain the same at 49.6 / 38.6 / 29.7 dB (12.91 / 12.97 /
  12.84); 15 malformed tables refused; the AGC as the common gain (the
  table wins), back to the AGC when cleared; leaving for coherent: every
  packet at the common gain, CONVERGED. 8 regions on 314 hops: +0.0 % a
  sweep with the fork's fast gain write, +5.0 % without (11 writes a sweep
  for 7 borders inside the tuners' blocks).
- The receiver with the fake heimdall (`rx/t_rx.py`; 32 / 32 again after
  542a38e): cmd 18 / 18 - the
  table validated (10 malformed ones rejected, nothing sent), sent at once
  and after every `set_sweep`, saved as heimdall answered it (30 -> 29.7),
  in a second page's first message and every change, sent again 2.6 s
  after heimdall restarted, kept when an older heimdall (no regions)
  answers "Unknown command" (`gain_regions_known` false) and sent again
  when a newer one is back, replayed after the receiver's restart (after
  its `set_sweep`), cleared; an old page's `SWEEP_AUTO_GAIN:1` - nothing
  sent, saved or rejected. Levels 14 / 14 (5 dongles 0 / 1.5 / -1 / 2 /
  -0.5 dB apart, tuner noise 6 counts, the converter's 1.5, heimdall's
  curve published; every bin through `SWEEP_FULL`): a region taken 20 dB
  down (49.6 -> 29.7 over hops 24 - 36): its carrier's height +0.00 dB
  (one level step is 0.47), a carrier in the other tuner's block of the
  region -0.31, the one outside +0.00; the region's floor +4.86 dB for
  +4.91 predicted from the two noises; outside it +0.00; the tuners'
  offsets [0.0, 1.39, -0.97, 1.88, -0.51] -> [0.0, 1.34, -0.95, 1.88,
  -0.44] dB; the border moved 3 hops and onto a block start: the same
  within 0.2 dB; cleared: all back within 0.00. Rounds 3 - 8's suites on
  this build with no table (the earlier workers' tests, my fake): `t_sweep`
  207 / 207 (rerun after 542a38e: 206 / 207 - its sweep-duration check, the
  python fake's own pace 0.137 s against the 0.176 s it expects), `t_full` 31 / 31, `t_overview` 23 / 23, `t_res` 175 / 181 - the same 6 failures as fe8eb74's build (checks written for round 4's limits: the grid at 8192 / 16384 on a 1.3 GHz range, sizes past 16384 at a short dwell); its first run here 174, one carrier's height 2.9 dB low at one frame a hop, not in the rerun.
- The page (headless Chromium, real mouse / touch / key events, heimdall's
  fake build + the receiver; `p/t_lane.py`, `p/t_lane_old.py`): desktop 40
  / 40 - the lane under the spectrum and over the split; no points: the
  Gain bar sends `GAIN:`; a double-click adds a point on the nearest hop
  edge (one command, the whole table), the Gain line "2 regions"; a click
  selects; the Gain bar then sends only the table (49.6 -> 43.4 on that
  region), stops at 0 dB (never Auto), shows the other region's gain on
  selecting it; a double-click within a hop of a point adds nothing; drags
  snap to hop edges (5 hops), stop a hop before a neighbour on either side
  and at the range's last inner edge; zoomed in the lane follows the
  window, a drag that starts on a point moves it and not the view, a drag
  off the points pans, the plot still pans; the readout names the step's
  gain; keys select and move a border; a second browser shows the same
  table and its added point shows in the first; right-click removal with
  the merge rule (selected / left); reload keeps the table and selects the
  middle region; the picture has the lane; the last point's removal sends
  `GAIN:` then `SWEEP_GAINS:|`; no page errors. Phone (400 x 860, touch) 9
  / 9: the lane 40 px; double-tap adds, a tap selects, a finger drags a
  point 4 hops, press and hold removes. An older heimdall (the python fake
  without regions) and the receiver of fe8eb74 serving this page: no lane,
  the Gain bar sends `GAIN:` from Auto, no errors (10 / 10).
- Found and fixed on the way: the answer to the previous
  `set_sweep_gains`, drained just before the next one went, put its table
  back (542a38e: only the answer that leaves none open is taken).
- On the KrakenSDR (the orchestrator; the dev machine's unit, 5 tuners,
  24 - 1765 MHz, 911 hops, dwell 5, the common gain 49.6 dB; job scratch
  `pi/regions_pi.py`, `pi/regions_lane.py`): 32 / 32 with the worker's
  tip, and again with the builds from the merged tree (774c394).
  - heimdall's own 8091 stream, the header gain of every valid packet
    over 3 sweeps (2733 hop visits a case): no table - every hop at 49.6;
    a table (points 412 / 419 / 750 / 810 MHz, gains 49.6 / 25.4 / 49.6 /
    16.6 / 49.6) - 0 wrong, no hop with two gains; two borders moved -
    followed within two sweeps; cleared - all back. 4 gain writes a
    sweep, none failed, the fork's fast write in use; a sweep 3.944 s
    without the table, 3.939 s with it.
  - Levels. The regions left at 49.6 dB keep their floors (1400 MHz 8.0
    -> 8.0, the FM band 17.4 -> 17.4, 650 MHz 17.9 -> 17.9 dB). The
    416.59 MHz carrier, whose hop the 415.64 MHz carrier saturates at
    49.6 dB (round 10): with 49.6 dB everywhere it reads 74.3 dB, 54.1 dB
    over a floor the compression has pulled ~16 dB under its neighbours'
    (those two hops are dark in the waterfall); with its region at 25.4
    dB it reads 90.3 - 92.2 dB, 59.3 - 59.8 dB over the floor (the whole
    sweep at 25.4 dB: 60.2), and stands 18.3 - 20.2 dB above the
    whole-sweep-at-25.4 picture for the 19.8 dB of heimdall's curve - its
    true height on the 49.6 dB picture's scale. Hops clipping a sweep: 54
    - 55 without the table, 31 - 33 with it.
  - Across a change (the region's hop that is not saturated, 413.3 -
    414.1 MHz): the floor 36.7 dB before, 36.2 / 35.8 / 35.3 / 34.8 /
    35.3 dB in the next five sweeps (levels not shifted would read 20 dB
    low).
  - A reload keeps the table; sweep -> coherent with a table set:
    CONVERGED in 3.7 s; back in sweep mode the table is in force again.
  - The lane, real mouse events at 1920 x 1080, the view 409 - 440 MHz:
    30 px between the spectrum and the waterfall; two double-clicks put
    points on the hop edges 412.2375 and 419.8875 MHz, three regions at
    49.6; a click on the middle one and the Gain bar: 25.4 dB there only
    (the common gain stays 49.6, no Auto on the bar), and in heimdall's
    stream the hops 413.2 - 418.9 MHz at 25.4, their neighbours at 49.6;
    a drag moves the point exactly two steps (423.7125 MHz), the stream
    follows; a drag off the points pans the view and changes no point; a
    right-click removes a point, the merged region keeps the selected
    one's gain; the last point removed: no table, the common gain 25.4
    dB, Auto on the bar again, every hop at 25.4. No page error.
  - With no table, rounds 3 - 8's sets on this build: net 19 / 0, swing
    24 / 2 (its two steadiness checks at 6.4 / 6.8 % for 5 %, as before
    this round), dres 15, keep 10, r5 27, wf 14, res 15, reslist 15, e2e
    19 / 1 ("pause stops the sweep": the known race), zip 40.
  - Two faults of the first run's checks, not of the code: absolute
    heights compared across two references (a table's reference is its
    highest gain, a common gain's is itself), and a pan drag started
    outside the view.
- On the test Pi 5 (a Wideband unit run in plain mode: noise only; built
  there from the merged tree; `pi/regions_cpu_pi.py`): 13 / 13. The fast
  gain write is in its library; 8 regions (7 borders, 11 writes a sweep,
  none failed): a sweep 4.025 -> 4.069 s (+1.1 %), heimdall 19.6 -> 17.3
  % of a core, kraken_doa 16.8 -> 14.9 % (no cost). A region turned down
  shows the converter's noise as a higher floor (at 29.7 dB: 18.8 ->
  26.4 dB; 25.4: 16.5 -> 29.6; 16.6: 14.6 -> 37.2; 37.2: 12.7 -> 18.8).
- Not tested: a Pi with a KrakenSDR on the air (the test Pi has the
  Wideband unit: noise only) and a Pi 4, Windows (a gain write's ~8
  transfers at 3-4 ms without the fast call) and macOS, MinGW and zig
  (the portability syntax checks: not on this machine), Firefox / Safari,
  a real phone (touch was run in Chromium's emulation only).

**Merged with main (Signal ID, Bands, tuner buttons), 2026-11-24.** Local
`main` (8f8bbb4: Signal ID - the signal classifier, its runtime service and
labels -, the Bands bar of worldwide ITU bands over every spectrum,
independent mode's tuner buttons T0..Tn / All) merged into this branch
(5b56cfe); nothing in `heimdall_v2/` or `gr-krakensdr_v2/` came from main
(`git diff worktree-sweep-mode HEAD -- heimdall_v2/src heimdall_v2/config.h`
empty).
- Conflicts (8 hunks, 6 files), all kept from both sides: the data
  receiver (Signal ID's tap first - every packet, one atomic load while no
  capture is armed -, then this mode's branch), `main.cpp` (`sweep::start()`
  then `sigsvc::start()`), the Makefile's `APP_SRCS` (sweep.cpp,
  cert_status.cpp, sigclass_service.cpp), the page (the spectrum header
  holds `#swp-hdr` and `#pane-pick`; the globals block `swp` and
  `paneHidden`; `specOnFrame` builds main's frame with its view `seq`, then
  drops it in sweep mode as before), `CLAUDE.md` and `README.md` (both
  sides' sentences).
- Decided: **Signal ID is parked in sweep mode** (`sigclass_service.cpp`,
  mode index 3 - never independent's 2, although heimdall sends 0x2000 with
  0x800): no capture, no look, no tracks, "Paused" / "Sweep mode (the tuners
  hop)" in its box; the mode change clears the labels at once (main's rule)
  and leaving the mode brings it back by itself (*signal-classifier.md*,
  Runtime, Sweep mode). **The Bands bar** is drawn over the sweep's plot
  (`bandStripSweep` from `swpDraw`, main's `bandStripDraw` with the sweep's
  window; `bandStripMain` skips sweep mode) - the strip sits above the plot,
  the lane stays between the plot and the waterfall. **The tuner buttons**
  need nothing: `.indep-only`, and in sweep mode there are no panes
  (`panesTeardown`), no pane views are asked; the hidden set
  (`kraken_pane_hidden`) is the same when independent mode comes back.
- Tests (fakes only; job scratch 52196024 `mergemain/`: a TEST receiver of
  the merged tree on 2877x, exe `kdmrg11`; heimdall's fake build `hmrg11` /
  the python fake on 2876x): the full `make -C kraken_doa_v2`
  (kraken_doa, liquid-dsp, 35 of 35 plugins) and heimdall's fake build
  without errors, the same 3 `-Wsign-compare` warnings as main's and this
  branch's builds (fft_processor.cpp, message_builders.cpp). Signal ID's
  own checks on the merged tree: `sigclass_rules_test` all passed (seeds
  12345 and 4711, 38 checks each), `sigclass_tool selftest` passed,
  `band_table_check.py` 60 rows OK, `theme_palette.py --check` OK. This
  mode's suites: `t_rx` 32 / 32, `t_sweep` 207 / 207, `t_full` 31 / 31,
  `t_overview` 23 / 23, `t_res` 174 / 181 (the same 7 as round 11's run of
  it: the grid / fft_size limits and one scalloping height - test
  expectations, not the merge), the lane 49 / 49 (40 desktop + 9 phone) and
  again 49 / 49 with Bands on, the page against an older heimdall and the
  receiver of fe8eb74 10 / 10 (also with Bands on). The seams (`p/t_seam.py`,
  the python fake in all four modes, a scene of carriers and noise bands at
  432 - 436 MHz): desktop 34 / 34 - coherent: Signal ID labels a carrier on
  the spectrum, the bar on the axis (0.08 px); -> sweep: the tracks gone
  0.10 s after the mode command, "Paused" / "Sweep mode (the tuners hop)",
  nothing drawn, no snapshot in 12 s; the bar over the sweep's plot on its
  axis at 400 - 470 MHz (0.71 px), through 8 wheel notches (<= 0.47 px), a
  pan, Bands off / on; the strip above the plot, the lane between the plot
  and the waterfall; no tuner buttons, no panes; -> coherent: labels again
  1.1 s after the command; independent: T1 hides its pane, its view asks
  16 points, labels in the panes; -> sweep: no pane views asked, no
  buttons; -> independent: the same pane hidden as before, All shows all;
  wideband scan: labels on the stitch, the bar on its axis; no page error.
  In sweep mode with Signal ID on and off (two of each, 30 s): the
  `sigclass` thread 0.00 / 0.01 s of CPU with it on, the process 4.2 % of a
  core both ways, 365.8 against 365.8 sweeps a minute, the carrier and the
  median level identical. The settings replay with both sides' keys
  (`sigclass`, `sweep_gains`, the mode): a restart in sweep, independent,
  wideband scan, coherent and sweep mode - each mode back, Signal ID on
  (parked in sweep mode, the gain table in force), 0 commands refused.
  24 - 1765 MHz: the bar on the axis at 1x (0.47 px; its 8 segments under
  2.5 px are drawn 2 px wide by main's CSS borders, within 1.5 px) and
  through 40 wheel notches to the zoom's end, 5.18 MHz (<= 0.47 px). Phone
  (412 x 860, touch) 7 / 7: every mode, no horizontal scroll, the strip and
  the lane in place in sweep mode. Main's build (8f8bbb4) against the merged
  one on the same fake and scene: the same signals and classes in coherent
  (4), wideband scan (6) and independent mode (6; centres within 1 kHz),
  labels drawn 4 / 4, 6 / 6, 18 / 18, no page errors.
- On the KrakenSDR (the orchestrator; the dev machine's unit, the merged
  receiver and page, heimdall's binary the same file as round 11's):
  - Round 11's gain-by-region run again (`pi/regions_pi.py`): 32 / 32 -
    every hop's gain as the table says (0 wrong of 2733 hop visits a
    case), a sweep 3.939 s with and without the table, the 416.59 MHz
    carrier 59.3 - 60.2 dB over its floor in its 25.4 dB region (54.6
    saturated), the lane's gestures with real mouse events. Its return to
    coherent took 24.6 s in this run where round 11's took 3.7 s: this
    start's first calibration (lags of ~250 samples to servo) had not
    converged when the check entered sweep mode ~15 s after the start, so
    the lag states were not CONVERGED and the return ran the whole lag
    acquisition - heimdall's behaviour at any start, not the merge's (its
    binary is unchanged); the check's start is a race with the first
    convergence.
  - Signal ID beside sweep mode on signals on the air (`pi/sig_pi.py`,
    coherent at 416 MHz, 25.4 dB): 20 / 20. Coherent: 6 signals labelled
    0.7 s after switching it on (DMR at 415.638 / 415.676 / 415.714 MHz,
    MPT1327 at 416.587 / 416.675 / 416.750 MHz). Into sweep: the labels
    gone 0.16 s after the mode command, "Paused" / "Sweep mode (the tuners
    hop)", no snapshot, track or label in 35 s; a sweep 3.970 s with
    Signal ID on, 3.964 s with it off; the lane directly under the
    spectrum, the Bands bar (37 segments at 24 - 1765 MHz) above it, no
    tuner buttons. Back to coherent (the calibration converged before the
    sweep): lag states kept, CONVERGED within a second, the same six
    labels 1.2 s later. Independent: the old labels off, new ones 0.6 s
    later, drawn in every pane (7 / 6 / 7 / 6 / 6), the tuner buttons
    shown; -> sweep: gone in 0.15 s, parked, no buttons; -> coherent:
    CONVERGED. No page error.
- Not tested: the Pi with the merged build, MinGW and zig (the portability
  syntax checks: not on this machine), Windows / macOS, Firefox / Safari, a
  real phone; main's own job-scratch suites (`ui_bands.py`, `ui_sig.py`,
  the strict playback) - not this job's, not rerun.
