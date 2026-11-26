# Calibration: lag servo, phase compensation, retune timing, calibration hold, IF VGA

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

## Key Algorithms

### Lag Compensation: Closed-Loop Proportional Servo

Compensates for sample-clock offsets between devices. The entire acquisition
is ONE mechanism: `MEASURING` (brief reading-consistency check) hands any lag
magnitude straight to `SERVOING`, a closed-loop proportional-rate servo on
the **register-only** actuator `rtlsdr_set_sample_freq_correction_f()`
(RTL2832 sample-frequency-offset regs 0x3e/0x3f, ±0x1FFF counts of 2^-24 ≈
±488 ppm; the fork clamps).

- **Counts ∝ measured lag** (median of last 5 readings), saturating at
  `servo_max_counts` (6000 ≈ 357 ppm ≈ 858 samples/s) far out and tapering
  exponentially — the endgame (freeze near zero, write-kick learning,
  software `frac_delay` trim, ±0.02 lock) is unchanged.
- **Why this actuator**: it moves ONLY the sample clock. The correlation
  peak attenuates as ~2·Si(πΔ/2)/(πΔ), Δ = ppm×NUM_SAMPLES drift per
  window: 69% at 100 ppm, 17% at 357 ppm, 12% at the ±488 ppm register
  ceiling — hardware-verified usable right AT the ceiling (477 ppm probe:
  clean tracking at ~1100 samples/s), so the register range, not peak
  visibility, is the binding limit. The servo measures WHILE correcting.
- **Two-speed cadence**: coarse flight (|med| ≥ 2) updates on EVERY reading
  (rolling median, register writes hysteresis-gated at ≥12.5% change);
  the endgame keeps the original every-5th-reading cadence and untouched
  freeze/kick logic. Faster updates raise the delay-safe gain ~5×, and the
  exponential taper (τ = 1/gain) dominates total convergence time.
- **Delay-aware gain**: readings lag reality by pipeline transit + median
  window + update period. A delayed proportional loop rings unless
  gain×delay ≪ 1, so effective gain is capped at ~0.15/update_period
  (measured per control update); `servo_gain` (0.5/s) is the idle-machine
  ceiling. Above |lag| 2 the drive direction FOLLOWS the measurement (an
  overshoot self-corrects); below 2 it keeps the entry direction and the
  freeze logic owns stopping (noise flips the median's sign there).
- **Engage** zeroes the register first (a stale correction from an
  interrupted run would read as unfixable clock drift).
- Verified on Pi 4: lags up to −4818 → all-channel lag + phase convergence
  in ~25 s; −10,635 under full CPU load converged monotonically, zero
  coherence events.

**No-peak warning (added 2026-11-13; warning only)**: the correlator sees
lags within ±`NUM_SAMPLES` (16384) and the peak shrinks with the block
overlap. If the dongles' streams are paired further apart than that, there is
no peak at all: the servo chases noise (readings jump by thousands of samples,
the register swings to ±6000 counts) and never locks - before this, heimdall
logged nothing, counted no coherence event and stayed unconverged.
`check_lag_no_peak()` (compensation.cpp) runs on every fresh reading of a
SERVOING channel, so only in coherent mode with the noise source on.
`process_correlations()` publishes per channel the correlation peak / mean
magnitude (`peak_ratios`, summed in the loop that already builds the trace).
A channel warns once per calibration attempt when it has servoed
`LAG_NO_PEAK_WARN_S` (60 s) without a lock AND the median peak / mean of its
last `LAG_NO_PEAK_READINGS` (32) readings is below `LAG_NO_PEAK_RATIO` (8):
one stderr line (contains "WARNING", so the dashboard's ALERT line shows it),
and `lag_no_peak` in the 8092 status / web STATE lists the channel until it
locks or a new attempt starts (servo engage, `reset_lag_compensation_all_channels`).
The time counts only servoing on fresh readings (gaps capped at 0.5 s per
reading) and stops while a calibration is held, the discrete scanner runs, an
element-count change reconfigures, or with plain `--kerberos`. Measured
2026-11-13 (x86_64, 5-channel KrakenSDR; the fault reproduced with a librtlsdr
build that maps zero-copy buffers 100 us each at stream start, see
`../../../docs/dev/librtlsdr-fork.md`, *Zero-copy USB buffers*):
- No peak: 8 of 8 such starts left 2-3 channels unlocked; their readings
  4.2-6.8 peak / mean (window medians 4.9-5.1 when the warning fired), lags
  jumping within about +-8000. The warning fired 60.0 s after the servo
  engaged (60.9-61.0 s after process start), once per channel, the field set
  at the next 2 Hz status; with a 80 s calibration hold in between it fired
  at 141 s (24 s before the hold + 36 s after), after a 60 s wideband-scan
  detour 60 s after the return (the servos re-engage there: a new attempt).
- With a peak: 221 channel acquisitions that locked (19 healthy starts on the
  default and plain zero-copy libraries, element-count changes, retunes, gain
  changes, Full Recalibrations, mode switches, a hold, one run on one core,
  and the in-range channels of the fault starts): first lags up to 14173,
  the slowest lock after 38.5 s of servoing; peak / mean never below 22.8
  except a segment's first reading (a stale set: 4.7-5.5 after a recovery's
  flush, 13.5-13.9 after a mode switch), typically 36-200 (~190 at lag 0).
  No warning, `lag_no_peak` stayed `[]`.
- Not measured: the Raspberry Pi (fewer readings per second; the window is
  32 readings, the time is wall time) and the `--wideband` variant (weaker
  noise coupling may lower the healthy ratios).

What fixes it, measured: a Full Recalibration (`reset_lag_compensation`, web
Full Recalibration) converged 4 of 4 times (26-34 s), lags then within a few
thousand samples; an element-count change too (1 of 1). It does not restart
the streams, but its L1 flush waits for a quiet point and drops every queue at
once, so the next sets are paired by arrival time - inferred cause: at startup
(no such flush) the drain pairs each stream's first buffers by count, and a
stream whose first transfers came late starts that many samples behind. A
return from wideband scan or independent mode does not flush and does not fix
it (measured once). The inferred cause was confirmed and fixed 2026-11-13:
*Stream start* below.

**Stream start (added 2026-11-13)**: the first lag of each channel is the
offset between the dongles' stream starts - nothing else. Each stream used to
start whenever its reader thread's first transfer went out (plus up to ~0.8
ms of stale FIFO content, `../../../docs/dev/librtlsdr-fork.md`, *FIFO
hold*), and the drain paired the streams' buffers by count from each one's
first packet. Measured (heimdall, per stream: first-callback time, then the
servo-engaged lag): lag = -2.4 x (first callback_i - first callback_0) in us,
residual median 76-97 samples, max 324-664, over 770 channel starts in all
variants below. Two parts: whole buffers by which a late stream is paired
behind (only when a reader is held up - one CPU core, a slow buffer mapping),
and the sub-buffer offset of the starts. Now:
- **Held start** (Linux, fork with `RTLSDR_HAS_FIFO_HOLD`):
  `init_rtlsdr_device()` holds each dongle's USB FIFO empty
  (`rtlsdr_set_fifo_hold`) instead of resetting it, and
  `start_pipeline_threads()`, once the readers are created, calls
  `rtlsdr_release_fifos()`: it waits (`STREAM_START_WAIT_MS`) until every
  reader has queued its transfers and releases all FIFOs with asynchronous
  control transfers back to back - the streams start within ~0.1 ms and
  with no stale data. One thread submitting all five asynchronously, not
  five waiting threads each writing one: both issue the releases in
  parallel (a control write takes ~1.9 ms here, the Pi 5's
  `rtlsdr_reset_buffer` 3.6-3.9 ms, and five at once take no longer than
  one there), but the threads add their wake-up jitter - scratch tool, 15
  starts each: async 145 / 286 / 411 samples max |lag|, threads 211 / 357 /
  962. heimdall logs (stdout) each release's completion and their spread,
  "USB streams released together: completed at ... us (spread N us)", and
  the drain each stream's first packet, "Sample drain: first packets at
  ... us" - first lag ~= -2.4 x that difference - so a Pi's spread can be
  read from a verbose log. Not on Windows (control transfers 3-4 ms, one at
  a time: the releases would spread the starts by ~14 ms) or macOS (not
  measured), nor with an unpatched fork: those use the reader gate below.
- **Startup flush** (every platform): the drain, before its first set of
  each pipeline start, waits for every stream's second packet
  (`STARTUP_FLUSH_WAIT_MS`) and flushes once at a quiet point
  (`startup_flush()`, the existing `flush_l1_now()` + `flush_downstream()`),
  so buffers pair by arrival time: whatever held a reader up, the lag stays
  within one packet - about half a packet (8192 samples) when the streams'
  packet times lie within half a period of each other, up to almost a whole
  one (16384, the correlator's edge, little overlap left) when they spread
  wider: then there is no quiet point and the flush pairs each stream's next
  packet. Lags below that are unchanged.
  An element-count change already had such a flush (its recovery flushes ~40
  ms after the streams start - why it fixed the no-peak starts); a cold start
  had none.
- **Reader gate** (only without the held start): the USB reader threads get
  ready (realtime priority set) and wait; `start_pipeline_threads()` lets
  them call `rtlsdr_read_async` together (bounded by
  `STREAM_START_WAIT_MS`; a stop opens the gate). Unloaded no gain - each
  stream's own buffer allocation and submit still spread them - but under
  CPU load on every core it kept the readers from starting milliseconds
  apart (table below; the final code built against the unpatched fork:
  1056 / 1172 / 1791 unloaded, 3 starts; 774 / 979 / 5177 under load, 4).
- Not done: a FIFO reset of running streams
  (`rtlsdr_reset_buffer()` STALLs the queued transfers - every stream died;
  with the hold value the streams survive but each loses a different 1.75-2.24
  ms mid-buffer, which shifts the alignment instead of re-anchoring it:
  librtlsdr-fork.md, *FIFO hold*).

Measured 2026-11-13 (x86_64, 5-channel KrakenSDR, 2.4 MSPS; each start a
cold start, then 5->4->5 element changes and `check_calibration`; the
variants interleaved in rounds; max |first lag| per start and convergence to
CONVERGED, min / median / max):

| variant | starts | max \|lag\| cold | after 5->4 / 4->5 | converged cold s | 5->4 / 4->5 s |
|---|---|---|---|---|---|
| before | 10 | 254 / 820 / 1568 | 659 / 746 (median) | 20.0 / 23.4 / 26.5 | 23.2 / 25.6 |
| reader gate (A) | 10 | 344 / 906 / 1480 | 458 / 1061 | 21.8 / 23.5 / 26.8 | 25.3 / 27.5 |
| startup flush (C) | 10 | 410 / 1003 / 1560 | 766 / 888 | 22.3 / 24.9 / 31.5 | 24.0 / 26.6 |
| A + C | 10 | 208 / 950 / 1675 | 674 / 718 | 22.8 / 23.6 / 27.8 | 25.1 / 26.2 |
| held start (B) | 10 | 47 / 264 / 411 | 247 / 280 | 20.0 / 22.0 / 24.8 | 24.4 / 23.6 |
| B + C | 10 | 99 / 232 / 426 | 320 / 320 | 19.3 / 21.0 / 23.8 | 23.6 / 23.5 |
| A + B + C | 10 | 160 / 258 / 453 | 345 / 326 | 19.0 / 23.2 / 24.3 | 22.5 / 22.4 |
| final code (B + C), a second series | 10 | 92 / 320 / 436 | 261 / 292 | 20.0 / 22.0 / 24.3 | 24.0 / 23.9 |
| before, same series | 10 | 621 / 1136 / 1949 | 494 / 864 | 22.0 / 24.4 / 30.0 | 23.1 / 26.4 |

Stress cases, 4 starts each (max |lag| cold; converged cold s; min / median /
max):

| variant | one CPU core (`taskset`) | all cores busy (a busy loop each) | zero-copy mapping 100 us per buffer, no prealloc |
|---|---|---|---|
| before | 7753 / 8669 / 9074; 30.8 / 32.2 / 42.8 | 379 / 4618 / 5454; 24.8 / 30.4 / 32.0 | no lock in 4 of 4 (2-3 channels each, 12 no-peak warnings) |
| A | 1873 / 6270 / 7736; 27.5 / 32.5 / 34.0 | - | no lock in 4 of 4 (10 warnings) |
| C | 7812 / 8605 / 8839; 31.0 / 31.4 / 32.8 | - | 6918 / 10068 / 14032; 32.3 / 37.5 / 41.0 |
| A + C | 2041 / 7873 / 8356; 27.3 / 34.1 / 35.3 | 496 / 791 / 1434; 22.8 / 23.4 / 24.8 | 7521 / 9849 / 13086; 32.8 / 35.5 / 40.8 |
| B | - | - | 239 / 288 / 370; 21.3 / 23.9 / 24.5 |
| B + C (final code) | 152 / 215 / 398; 21.8 / 23.3 / 23.8 | 134 / 229 / 284; 19.8 / 21.8 / 23.3 | 156 / 267 / 362; 20.3 / 20.6 / 23.0 |
| A + B + C | 230 / 336 / 436; 21.0 / 22.6 / 24.0 | - | - |

(The no-lock "lags" are noise readings, not listed.) Zero-copy ON with
`rtlsdr_prealloc_async` (`-DENABLE_ZEROCOPY=1`), 4 starts each: before 794 /
1202 / 1505, A + C 859 / 1203 / 1449, B + C 137 / 263 / 377 (converged 20.3 /
21.6 / 23.0 s).

Every check OK (|lag| 0.0), `coherence_events` 0, `lag_no_peak` empty and no
failed start in all of these except the slow-mapping starts marked "no lock"
(those warned, as designed). Retunes, gain changes, a Full Recalibration,
wideband-scan and independent mode and back and a calibration hold with the
final code: all reconverged as before, `coherence_events` 0. Not measured:
any Raspberry Pi, Windows, macOS, the `--wideband` and `--kerberos`
variants. The lag servo removes ~858 samples/s, so the ~600 samples saved
on a normal start are worth well under a second of the ~21-25 s; what the
change buys is the stress cases, where a held-up reader used to start a
channel thousands of samples - or out of range - behind.

**Why not timed "bursts" on the PLL path** (`rtlsdr_set_freq_correction`,
the historical approach — code removed July 2026): that call retunes the
tuner LO by ppm×RF (10 kHz at 100 ppm/100 MHz), which destroys the
correlation peak while active (mid-burst readings are garbage), each
set/reset pair slips a random ~2–4 samples, and open-loop timing makes the
landing point dice on a loaded machine (landing = lag −
ppm×2.4×(duration + timing jitter) − slip). The register-only servo has
none of these failure modes.

### Fractional-delay trim filter (16 taps since 2026-11-26)

The servo's endgame folds a residual below 0.75 samples into
`compensation.frac_delay` (clamped to ±0.9), which the conversion loop
(`samples_to_complex_with_compensation`, `sdr_pipeline.cpp`) applies as a
Hann-windowed sinc on every channel: `SDRDevice::FRAC_TAPS` = 16 taps, group
delay `FRAC_CENTRE` (7) + d, unity DC gain. CH0 and untrimmed channels keep a
pure delta, so all share the 7-sample base delay. The lock nulls the
broadband correlation peak - the filter's mean delay -, not the filter's
phase curvature across the band, and that curvature lands on the DoA of a
VFO away from the centre.

Why 16, not 8 (model: phase error after removing the constant and the slope
over |f| <= 0.42 fs, worst case over d in ±0.9):

| taps | 0.2 / 0.3 / 0.4 fs | droop at 0.4 fs |
|---|---|---|
| 8 | 4.0 / 5.0 / 11.8° (worst at d −0.45) | up to 3.1 dB |
| 16 | 0.06 / 0.15 / 0.20° | ≤ 0.07 dB |

The window is fixed (centred on tap (N−1)/2), so negative trims were the
8-tap kernel's worst case; a window that follows d would only gain 0.20 ->
0.14° at 16 taps. 12 taps would still leave 1.8° at 0.4 fs.

Hardware A/B (2026-11-26, x86 dev machine and its KrakenSDR, 100 MHz, gain
49.6, five full recalibrations per build, job scratch 14951841 `trimtest/`):
the phase of each channel against CH0 from the 8091 stream with the noise
source on in CONVERGED (1024-point cross-spectra over ~2.5 s), constant and
slope removed. The difference between the 8- and 16-tap builds is the 8-tap
kernel's predicted curvature for each channel's logged trim (CH1, d +0.134:
−4.03 / +3.90° at −0.4 / +0.4 fs, predicted −4.18 / +4.02; all four
channels alike, CH4 with trims of opposite sign in the two runs). Measured
minus predicted - the dongles' own curvature - agrees between the builds
within ~0.1-0.7°: 1.6-2.1° at −0.4 fs, 0.5-1.0° at ±0.3 fs, 3.7-7.0° at
+0.4 fs. That is what remains at the band edges with 16 taps; only the
per-bin EQ (off by default) flattens it - where it comes from and what the
EQ does with it: *Band-edge phase* below. Both builds: CONVERGED 23.3 s
after a cold start, 2.6-4.7 s per full recalibration, Check Calibration
|lag| 0.003-0.019 (8 taps) / 0.004-0.005 (16), |phase| 0.13-0.31°,
`coherence_events` 0, `lag_no_peak` empty.

CPU (one core: the convert thread filters the channels one after another;
5 channels x 2.4 MS/s): the loop alone on the i7-11800H dev machine 0.4 %
(8 taps) -> 0.7 % (16) of a core. Not measured on a Pi (none reachable
that day); estimated from the Cortex-A76's throughput and kraken_doa's
measured Pi 5 cost per tap (`R_TAP`, `shared_decimator.cpp`): Pi 5 2.0-2.6 %
of a core (+1.0-1.3), Pi 4 (Cortex-A72, 1.8 GHz) 5.3-6.0 % (+2.7-3.0). The
convert thread was 32.6 % of a Pi 5 core in coherent mode
(`operating-modes.md`, the per-thread table); no Pi 4 figure. GCC compiles
each multiply-add into an FMA (C++'s default `-ffp-contract=fast`; the x86
build has 118 `vfmadd` against 10 `vmulps` in the function) - without them
the Pi 4 figure would double. The body is written out tap by tap
(`static_assert` on 16) so the compiler vectorizes over the output with
every tap in a register.

Found while measuring, not changed: `main.cpp`'s startup banner leaves
`std::cout` at `std::fixed` + `setprecision(1)`, so every lag / trim /
residual heimdall logs after it is rounded to 0.1 sample (the test builds
restored the default format after the banner). Not measured: any Pi, the
wideband and Kerberos variants, frequencies other than 100 MHz.

### Band-edge phase: what one delay and one phasor per channel leave (measured 2026-11-26)

The calibration gives each channel one delay (the servo and the trim: a
phase proportional to frequency) and one unit phasor (a constant), both
taken over the whole band - the correlation peak and the dominant
eigenvector are power-weighted means. A phase that bends at a band edge is
in the noise-source data (it was measured from it), but neither number can
hold it, so it stays. That is the design, not a fault of the noise source.

What bends: the corners of each R820T's IF band-pass, which differ by ~8 %
between the chips of one unit. With the fork's filter setting the high-pass
corner (IF 0.47-0.51 MHz) lies at the + edge of the streamed band; the
low-pass corner the fork moved out of the band (commit 359a94d) lay at the
- edge. Registers, the A/B and the measurements that pin it on those
corners: `../../../docs/dev/librtlsdr-fork.md`, *The R820T IF filter*.

As heimdall leaves the stream (phase of CH1-CH4 against CH0, referred to
the band centre, no slope removed; 100 MHz, gain 49.6, x86 dev machine's
KrakenSDR): within +-0.2 fs <= 1.2°, within +-0.3 fs <= 2.2°, at +0.4 fs
4.2 / -5.6 / -7.8 / -8.5°, at -0.4 fs 3.4 / -3.1 / -1.7 / -2.5°. A VFO
at the + edge of the span gets a DoA from phases up to ~8° off; the same
from 70 to 1575 MHz (the curve is fixed in the IF: <= 0.9° change over
RF), at every gain, and for at least 20 minutes after a calibration (<=
0.44° change, the band centre <= 0.21°, the lag <= 0.001 sample).

**The per-bin EQ** (`PER_BIN_ENABLE`, off by default; 32 snapshots of the
cross-spectrum after the scalar phase has converged, a phase-only target
per bin, 32 complex taps per channel, Hamming window; re-measured on every
recalibration) does remove most of it. Worst phase over |f| <= 0.4 fs,
CH1 / CH2 / CH3 / CH4, EQ off -> on, 5 full recalibrations (round sd <=
0.25°), for the fork's filter setting and three others set by a test build:

| tuner IF filter | EQ off | EQ on | EQ on, no clipping (RF gain 0) |
|---|---|---|---|
| fork (FILT_CODE 0000, IF 1.6 MHz) | 4.1 / 5.6 / 7.7 / 8.3° | 0.6 / 0.9 / 1.3 / 1.3° | 0.6 / 0.7 / 0.7 / 0.9° (off: 5.0 / 6.8 / 7.5 / 8.2°) |
| before 359a94d (FILT_CODE 1111) | 29.4 / 15.6 / 16.5 / 18.1° | 6.3 / 2.3 / 2.4 / 2.5° | 2.3 / 1.0 / 1.0 / 1.1° (off: 36.3 / 18.9 / 20.6 / 23.3°) |
| stock osmocom's values (calibration code, IF 1.815 MHz) | 1.8 / 2.5 / 3.0 / 1.8° | 0.5 / 0.5 / 0.6 / 0.7° | |
| FILT_CODE 0000, IF 1.815 MHz | 3.0 / 3.2 / 2.8 / 3.8° | 0.5 / 0.5 / 0.4 / 0.8° | |

- Cost in time: +0.5-0.6 s per calibration (full recalibration 3.3 -> 3.9
  s, a retune 3.45 -> 4.0 s including the 3 s cooldown). CPU, measured
  afterwards: 35-61 ns per sample and channel on the x86 dev machine, most
  of a core, and a form 14-27x cheaper - *A faster per-bin EQ* below.
- It is phase-only: the level difference between channels stays (fork
  <= 0.25 dB at +-0.4 fs; with the old filter value 3.0 dB on CH1 at -0.4
  fs, 0.7-1.0 dB on the others).
- 32 taps cannot follow a steep corner: on clean data the old filter value
  still leaves 1.0-2.3° (an emulation of the design from the same frames
  agrees with the stream within 0.06° without clipping, and gives a third
  to a quarter of that with 64 taps).
- The noise source clips (14 % of CH0's samples with the fork's filter
  value, 26 % with the old one), and a clipped measurement shows a steep
  corner too small (24.6° against 31.0° at -0.4 fs for the old value; the
  fork's curve hardly changes, 0.3-0.5°). Inferred from the measured curves,
  not measured on a signal: an EQ designed on the clipped noise source
  leaves ~9 / 2 / 3 / 3.5° on an unclipped signal with the old filter value
  and 0.3-1.0° with the fork's.
- The "EQ on" column at gain 49.6 is read from the 8091 stream, which
  clamps the EQ-filtered clipped noise to 8 bits; that alone hides ~10 % of
  a correction (reproduced offline), so the 6.3° includes it.
- Found, not changed: the design's gate (`SNR_GATE`, bins below 5 % of the
  PEAK reference bin get no correction) compares with the strongest bin,
  DC included. With a weak noise-source level (RF gain 0 and IF VGA step 4,
  -31 dBFS) CH0's DC bin was 14 dB above the rest: every other bin fell
  below the gate, heimdall logged "designed" and the stream was unchanged.
  At the normal level the gate left the outermost bins uncorrected: beyond
  +0.47 fs with the fork's filter value, beyond -0.45 / +0.47 fs with the
  old one.

So why open the filter instead of calibrating its corner: nothing in the
default calibration can hold a corner at all; the optional EQ can, but a
corner inside the band is exactly where it is weakest (tap count, level
difference left in place, clipping bias, the gate), while a filter moved
out of the band leaves it a gentle curve it flattens to ~1°. Drift is not
the reason - the curves held for 20 minutes within the noise.

Data and method: job scratch 14951841 `rtlfilt/w_hw2/analysis/` (`D1_`,
`D2_`, `D3_`, `D5_COMPACT.txt`), tooling `../../tools/rtl_filter_phase/`.
One unit, one machine, room temperature; no Pi, no off-air signal with a
known bearing, no EQ timeline (the 20 minutes are of the uncorrected curve).

### A faster per-bin EQ (measured 2026-11-26; prototype on branch `eq-fast`, not merged)

The question: can the per-bin EQ be made computationally cheaper, perhaps as
a "multi-bin" EQ that combines bins. Answer: yes, 14-27x in the running
heimdall (the more the busier the machine) at the same accuracy, by two independent changes - a kernel the
compiler vectorizes (same taps, same output) and a design that needs 12 taps
where today's needs 32. Nothing of it is in the product: the code is a
prototype behind test commands on the branch `eq-fast`; the EQ stays off by
default and its default design and loop are today's.

**What it costs today** (x86 dev machine, i7-11800H, other jobs running:
load average 10-22, so absolute x86 figures are pessimistic; ratios hold):

- The EQ block is a plain `std::complex<float>` loop over 32 taps. GCC does
  not fully unroll a loop of more than 16 iterations
  (`max-completely-peel-times`), so at 32 taps it is NOT vectorized: 490
  instructions per sample, a loop-carried add chain, 36 ns per sample and
  channel at best and 53-67 ns on the busy machine (the 16-tap trim filter
  in front of it: 2.2 ns; the whole function without the EQ: 3.3 ns). clang
  does not vectorize the `std::complex` form at any length.
- In the running heimdall (5 channels, 100 MHz, CONVERGED, noise source
  off) the `convert` thread goes from 5.1-5.3 % of a core to 75-78 % with
  the EQ on and no 8091 client: 58-61 ns per sample and channel. With one
  client reading the stream it is 26 % without and 98.5 % with the EQ -
  saturated: 8091 delivered 130-138 sets a second instead of 146.5, 7-11 %
  of the sets dropped at L2-raw (whole sets, so coherence holds), with no
  counter and no log line; only the frame rate shows it.
- On the Pis (measured 2026-11-27, *On the Raspberry Pi* below): today's
  EQ alone needs 104 % of a Pi 4 core and 52 % of a Pi 5 core (`eq_bench`,
  pinned). In a running heimdall it saturates a Pi 4's convert thread
  (97 %; with one 8091 client only 89 of 146.5 sets a second delivered) and
  takes a Pi 5's from 6.4 % to 70 % (with a client 88 %, no sets dropped).
  Not usable on a Pi 4; on a Pi 5 it works with little headroom.

**Change 1, the kernel** (same taps; output equal to today's within 5e-7):
deinterleave the block once into I and Q arrays, run the taps in chunks of
four over 256-sample blocks with the tap count a compile-time constant,
CH0 (a delta) as a plain delay. Plain C++, no intrinsics; g++ and clang
both vectorize it, and clang's aarch64 code has exactly L `fmla` per sample.
Micro-benchmark at 32 taps: 4.2-4.4 ns (8-14x); at 12 taps 1.8-2.0 ns, at
16 taps 2.4-3.5 ns. (One line, `#pragma GCC unroll 64` on today's loop,
gives 5-7 ns with g++ and nothing with clang.)

**Change 2, the design** (`src/dsp/per_bin_wls.cpp` on the branch; reference
`design_ref.py` in the job scratch). Today's design takes the phase of each
of 32768 bins, transforms back and cuts the impulse response to 32 taps with
a Hamming window - the window spends most of the taps, and simply shortening
it fails (16 taps leave 3.2° with the fork's filter setting, 7-10° with
the old one). The new one:

1. sums the cross-spectrum over 128 groups of 256 adjacent bins (bins within
   4 of DC left out) - this is the "multi-bin" step;
2. gates a group on CH0's power against the MEDIAN group (10 %), not against
   the peak bin;
3. fits the L complex taps by weighted least squares to the phase-only
   target exp(-j arg S) over |f| <= 0.42 fs, with a weight of 1e-3 and target
   1 outside (a don't-care band, so the fit does not spend taps on the
   phase jump at the Nyquist edge) and a small ridge toward the delta at
   tap L/2; an L x L Hermitian solve (Cholesky, double precision; condition
   number 49 at L = 12, 249 at L = 16).

On "multi-bin": an L-tap FIR is already an EQ with L degrees of freedom -
L "bins" of fs/L - however many bins it was designed from, and its cost is
L. Combining bins before the phase is taken does not change the accuracy at
a given L (offline: within 0.0-0.3°), but it makes the design immune to the
snapshot count and to a weak noise source. Taking it literally (L groups,
an L-point inverse FFT, no window) is worse than the least-squares fit at
every L <= 16 (12 taps: 2.2-5.5° against 0.5-1.0°). The taps are saved by
the fit and its don't-care band; the binning makes it robust.

**Accuracy on the hardware** (the prototype's test modes in ONE heimdall
session per row, 5 calibrations per mode, the modes interleaved; worst
phase against CH0 over |f| <= 0.4 fs as the stream leaves heimdall, CH1 /
CH2 / CH3 / CH4, mean of 5; round sd 0.01-0.03° without clipping, 0.05-0.3°
with):

| condition | EQ off | today (32 taps) | least squares, 12 taps | least squares, 16 taps |
|---|---|---|---|---|
| fork's filter, 100 MHz, gain 49.6 (noise source clips) | 5.3 / 4.6 / 4.9 / 7.2° | 0.5 / 0.7 / 0.8 / 0.9° | 0.7 / 0.6 / 0.8 / 0.9° | 0.6 / 0.7 / 0.8 / 0.8° |
| the same, RF gain 0 (no clipping) | 5.0 / 6.3 / 7.3 / 8.6° | 0.7 / 0.6 / 0.6 / 0.7° | 0.5 / 0.2 / 0.6 / 0.7° | 0.3 / 0.2 / 0.3 / 0.3° |
| the same, 868 MHz, gain 49.6 | 3.6 / 6.8 / 7.6 / 8.1° | 0.6 / 0.7 / 1.2 / 1.0° | 0.6 / 0.9 / 1.1 / 1.2° | 0.6 / 0.6 / 0.9 / 0.9° |
| old filter value (FILT_CODE 1111), gain 0 | 36.6 / 18.6 / 20.3 / 23.1° | 1.2 / 0.6 / 0.6 / 0.7° | 0.4 / 0.3 / 0.3 / 0.3° | 0.3 / 0.4 / 0.4 / 0.4° |
| old filter value, gain 49.6 | 30.2 / 16.8 / 18.0 / 19.5° | 6.3 / 2.1 / 2.3 / 2.4° | 4.9 / 2.6 / 2.8 / 2.7° | 4.6 / 2.4 / 2.6 / 2.5° |
| weak noise source (gain 0, IF VGA step 4) | 5.1 / 6.3 / 7.6 / 8.1° | 4.9 / 6.1 / 7.1 / 7.7° (the gate: no correction) | 0.6 / 0.3 / 0.7 / 0.4° | 0.4 / 0.3 / 0.3 / 0.3° |

- Today's design with the new kernel gives today's numbers (the mean curves
  differ by 0.05-0.07° without clipping, round sd 0.025°).
- The old filter value at gain 49.6 stays at 2-6° with every design: the
  clipped noise source shows the steep corner too small (*Band-edge phase*
  above) - a limit of the measurement, not of the filter.
- Offline, on the frames recorded for *Band-edge phase* (design from 32
  frames, residual on other data, also checked by filtering held-out frames
  sample by sample): 12 taps match or beat today's 32 on all nine
  recordings, 16 taps reach what today's design gets from 64, 10 taps tie on
  the fork's setting, 8 are not enough (1.1-1.9° on the fork's and the old
  setting).
- 8 snapshots instead of 32 are enough for the least-squares design on the
  hardware (same residual within 0.1°) and take ~0.4 s off a calibration;
  the design itself takes 0.7-1.4 ms against 5-12 ms today.

**CPU in the running heimdall** (`convert` thread, % of an x86 core, no
8091 client, 25 s windows, modes interleaved, 3 windows each; load 11-18):

| | convert thread | the EQ alone, ns per sample and channel | against today |
|---|---|---|---|
| EQ off | 5.3 % | - | |
| today: 32 taps, today's loop | 74.6 % (72.7-76.9) | 57.8 | 1 |
| 32 taps, new kernel | 11.5 % (11.0-12.2) | 5.2 | 11x |
| least squares 16 taps, new kernel | 8.7 % (8.4-9.1) | 2.8 | 20x |
| least squares 12 taps, new kernel | 7.8 % | 2.1 | 27x |

A second run by another script at load 6-8 (2 windows of 20 s each): EQ off
2.8 %, today 44.8 % (35 ns), 32 taps with the new kernel 8.7 % (4.9 ns),
16 taps 7.2 % (3.7 ns), 12 taps 5.8 % (2.5 ns). Today's loop is bound by a
dependency chain and slows down more than the vectorized kernels when the
core's sibling is busy, so the ratio moves with the load: 12 taps 14x on
the quieter machine, 27x on the busy one; the kernel alone 7-11x.
With one 8091 client today's loop saturated the thread and dropped sets;
every fast mode delivered all 146.5 sets a second. (The Pi estimate first
written here - ~3 / 4 / 8 % of a Pi 5 core at 12 / 16 / 32 taps from
kraken_doa's `C_TAP` - was about 2.5x too low; measured below.)

**On the Raspberry Pi (measured 2026-11-27).** Pi 4 Model B Rev 1.5
(Cortex-A72, 1.8 GHz, 1 GB) and Pi 5 Model B Rev 1.0 (Cortex-A76, 2.4 GHz,
4 GB), each with a KrakenSDR, GCC 14.2, the `eq-fast` build made on the Pi
(`-mcpu=native`). Two measurements per Pi: `eq_bench` pinned to one core
(it measures the core clock itself), and the running-heimdall A/B of the
x86 run (the same `ab_cpu.py`: modes interleaved, 3 x 25 s windows each,
convert thread's CPU from schedstat, 100 MHz, gain 49.6), with the cpufreq
governor at `performance` and the clock logged every second (all samples at
the top clock; the governor restored to `ondemand` after). 0 coherence
events, no throttling (`get_throttled` 0x0, 53-64 °C on the Pi 4).

Running heimdall, convert thread, % of one core; "+EQ" = the EQ's own cost
per sample and channel (mode minus off):

| | Pi 4 no client | Pi 4 one client (sets/s) | Pi 4 +EQ | Pi 5 no client | Pi 5 one client (sets/s) | Pi 5 +EQ |
|---|---|---|---|---|---|---|
| EQ off | 16.9 % | 53.2 % (146.5) | | 6.4 % | 24.1 % (146.5) | |
| today, 32 taps | 97.2 % (saturated) | 96.6 % (**89**) | > 67 ns | 70.0 % | 87.9 % (146.5) | 53.0 ns |
| new kernel, 32 taps | 60.8 % | 95.5 % (**141-142**) | 36.6 ns | 21.5 % | 39.4 % (146.5) | 12.6 ns |
| least squares, 16 taps | 41.5 % | 79.0 % (146.5) | 20.5 ns | 14.2 % | 32.1 % (146.5) | 6.5 ns |
| least squares, 12 taps | 38.6 % | 76.0 % (146.5) | 18.0 ns | 12.5 % | 30.4 % (146.5) | 5.1 ns |

- Speed-up of the EQ stage over today's: Pi 5 4.2x (new kernel, 32 taps),
  8.2x (16 taps), 10.4x (12 taps); Pi 4 > 1.8x / 3.3x / 3.7x (today's is
  capped by the saturated thread, so its true cost - and the ratios - are
  higher; `eq_bench`: 87 ns).
- On a Pi 4 with a client only the least-squares designs keep up (76-79 %
  of the core: little headroom); the new kernel at 32 taps still drops
  3-4 % of the sets. On a Pi 5 every fast mode keeps up with room to spare.
- **`ondemand` distorts this measurement.** The first runs, under the
  default governor, put the Pi 5 at 1.6-1.7 GHz for most of the run and the
  Pi 4 at 0.8-1.0 GHz in the light "EQ off" windows - which inflated the
  baseline (Pi 4 33 % instead of 16.9 %) and made every EQ look cheaper
  (Pi 4 12 taps 5.5 ns instead of 18.0). Measure CPU on a Pi at a fixed
  clock.
- **The prototype's kernel is not the best one on ARM.** `eq_bench`'s plain
  planar kernel (row `K1c planar`) beats the prototype's (`K1g planar
  chunk4`, chosen on x86) on both Pis: Pi 4 16.9 / 23.0 / 38.2 ns against
  29-30 / 33-34 / 53-54 ns at 12 / 16 / 32 taps, Pi 5 4.9 / 7.0 / 13.8
  against 5.9 / 7.7 / 14.9 ns. A productized kernel should be chosen on the
  Pi (not yet measured in a running heimdall).
- Cache, Pi 4 only (1 MB shared L2): the prototype's kernel on one
  cache-resident channel is 1.2-1.4x faster than with five channels'
  buffers in rotation (`kern_test.cpp`: 15.5 vs 21.9 ns at 12 taps); the
  running heimdall matches the five-buffer case. No such effect on a Pi 5.
- Pi 5 running heimdall against `eq_bench` (four filtered channels + CH0's
  delay): within 6 %. Pi 4: `eq_bench` reads 18-34 % high.
- **ARM-specific kernels** (`neon_eq.cpp`, same day; checked against a
  double-precision FIR, then timed on five channels' buffers in rotation,
  pinned, top clock). ns per sample and channel at 12 / 16 / 32 taps:

  | kernel | Pi 4 | Pi 5 |
  |---|---|---|
  | `K1c planar` (portable C++, the compiler vectorizes it) | 11.9 / 17.9 / 33.4 | 4.7 / 6.8 / 13.7 |
  | the prototype's (`K1g planar chunk4`) | 21.9 / 26.3 / 45.8 | 5.0 / 7.4 / 14.7 |
  | NEON intrinsics: LD2 / ST2 on the interleaved I/Q (no deinterleave pass), taps by lane, 4 / 8 / 16 outputs a step, split accumulators | 13.8-15.7 / 19.6-20.3 / 38.5-40.0 | 5.3-6.0 / 6.9-7.5 / 14.5-14.7 |
  | FP16 vector arithmetic, 8 lanes (Pi 5 only - ARMv8.2; the Pi 4's A72 has none) | - | **2.7 / 4.2 / 7.5** |

  Hand-written NEON gains nothing over the compiler's code on either Pi.
  The kernel choice does: `K1c` is 1.4-1.8x faster than the prototype's on
  a Pi 4 (about 1.07x on a Pi 5). FP16 halves the Pi 5's EQ cost (1.7-1.8x)
  at a relative error of 1.2-2.3e-3 against 2-3e-7 (fp32) - above the
  float path but below an 8-bit input's quantization step (1/127.5); a Pi 5
  already has room, so it saves ~2 % of a core. Not available on these
  cores: FCMLA (complex multiply-add, ARMv8.3), SVE.

Data: job scratch 14951841 `pi/` - `res_pi4/`, `res_pi5/` (`eq_bench_*.txt`,
`*fix_CPU.txt` = the fixed-clock tables, `*fix_clock.txt`, `*_results.json`,
the `ondemand` runs `pi4_CPU.txt` / `pi5_CPU.txt` / `*perf_*`), `kern_test.cpp`
and `kern_pi*.out`, `neon_eq.cpp` (with `kern_proto.inc`, the prototype's
kernel copied from `eq-fast`) and `neon_pi*.out`, `run_perf.sh`, `build_pi.sh`.

**What the short design gives up, and what was seen on the way**

- The band beyond +-0.42 fs. Today's design corrects out to its gate
  (~0.47 fs); the 12-tap fit does not try. At -0.45 fs the stream showed
  3-9° with 12 taps where today's EQ leaves ~0-1° (fork's setting; EQ
  off: 3-10°), and at 868 MHz the 12-tap fit was worse than no EQ there
  on three channels (7-8° against 2-4°); at +0.45 fs both leave 0-6°. With
  16 taps and the fit band widened to 0.44 fs (test mode `WLS16E`) the edge
  is back to 1-3.5° at a cost of 0.1-0.2° inside - that value was chosen
  after seeing all the data. Out there the level is already 4-5 dB down
  and the alias term that follows the trim sits in the phase
  (`librtlsdr-fork.md`), so it is poor ground for a bearing either way.
- The fit's gain outside the band: up to +0.4 dB at 12 taps (+0.64 dB at
  16); in-band ripple <= 0.04 dB. Phase-only, like today's: the level
  difference between channels is left as it is (a complex target was tried
  offline and dropped: worse phase at 12 taps on a steep corner, +4-6 dB
  gain outside the band).
- With ANY per-bin EQ active, today's included, Check Calibration reads a
  band-average phase of 0.3-1.6° at gain 49.6 (0.05-0.4° with the EQ off;
  at gain 0 today's 0.06-0.11°, least squares 0.24-0.40°). The scalar
  phasor is settled before the EQ is designed and nothing re-centres the
  mean afterwards; taken to be the clipped noise source for every design and
  the uncorrected band edge for the short one (not proven). Far inside the
  10° recalibration threshold; rotating the taps by the fit's own mean
  phase at design time should remove it (not tried).
- `eq_tail` is not reset at a flush in today's loop (the trim's tail is):
  the first 31 samples after a flush carry the samples from before it. The
  prototype's kernel refills its carry; today's loop was left alone.

**Considered and dropped**

| candidate | why not |
|---|---|
| today's design with fewer taps | 16 taps leave 3.2° (fork's setting) to 9.6°, 12 taps 4.8-13.4° (offline) |
| literal multi-bin: one correction per group, L-point inverse FFT | worse than the fit at every L <= 16; at 32 it fails on a steep corner |
| correcting the level too (complex target) | see above |
| main tap off-centre | no gain (a phase-only correction is close to Hermitian-symmetric) |
| fusing the trim and the EQ into one pass or one filter | one pass: no faster (measured); one filter: the trim alone needs 16 taps, so the sum would be longer than 16 real + 12 complex |
| FFT-domain or filter-bank application, an all-pass IIR, 16-bit fixed point | not built: at 12 taps a vectorized FIR is ~12 multiply-adds per sample and lane, below the cost of two transforms per block, and an IIR's feedback does not vectorize over samples (reasoned, not measured) |
| a phasor per VFO in the client instead of filtering the stream (heimdall publishes the curve, 32-64 points per channel) | zero cost per sample and 0.1-0.7° for a 12.5-25 kHz VFO with the fork's setting (offline), but a 200 kHz VFO at the band edge keeps a 5-13° spread inside it, and the spectrum, gr-krakensdr and other clients get nothing; a possible addition, needs a wire field |
| moving the tuner's IF centre so the corner leaves the band | no CPU at all and 1-2° left without any EQ - the fork's decision, `librtlsdr-fork.md` *Alternatives* |

**Where it is.** Branch `eq-fast` (cut from this branch's docs commit):
`src/dsp/per_bin_wls.{hpp,cpp}` (the design, a pure function),
`eq_fast_stage` in `sdr_pipeline.cpp` (the kernel), test commands on the
8070 web socket `PER_BIN_TEST_LEGACY` (default, today's) / `FAST32` /
`WLS12` / `WLS16` / `WLS16E` / `WLS12S8` (not saved, not in the page),
`tools/eq_bench/` (`eq_bench.cpp`: one file, build line in its header, the
one-minute measurement run on both Pis above; `wls_design_test.cpp`: the
C++ design against reference taps, agreement 3e-9; README with the full
tables). Data: job scratch 14951841 `rtlfilt/eq/` - `w_acc/out/` (offline
tables, `summary.txt`), `w_cpu/out/`, `w_cpu/analysis/` and `w_cpu/runs/`
(benchmarks, hardware A/B), `lead/` (the lead's own re-checks).

Not tested: the accuracy on a Pi (only CPU was measured there; the taps
are designed the same way); the `K1c planar` kernel in a running heimdall;
MinGW and Apple clang builds; a bit-for-bit
comparison of the branch's default mode with main; a signal with a known
bearing; the wideband and Kerberos variants; another KrakenSDR unit; 4
elements with a capture; long runs.

### Phase Compensation (phase-only)

Uses eigenvalue decomposition of the spatial correlation matrix. The dominant
eigenvector of the noise-source data is proportional to the per-channel complex
gains; its phase gives each channel's correction, a UNIT phasor per channel.
Gain is measured (for display) but deliberately not corrected - see below.

1. **WAITING_FOR_LAG_COMPLETION**: Wait for all channels to reach lag convergence
2. **MEASURING_INITIAL_PHASE**: Collect phase measurements (readings are ignored
   until `NOISE_SETTLE_MS` after noise-on; only sets whose phases came from the
   eigen solve count - `CorrelationResult::phases_measured` - so placeholder
   zeros published outside the measuring states never read as a calibration)
3. **APPLYING_COMPENSATION**: Average `required_stable_readings` (3) snapshots as
   unit phasors (circular mean), apply to all future samples
4. **VERIFYING_CONVERGENCE**: Measured on the compensated stream; requires phases
   within ±1°. A failing check triggers the retry pass, which multiplies the
   remaining phase into the vector.
5. **CONVERGED**: Phase drift within threshold (±1°)

**Retune recalibration timing** (re-measured 2026-10-02, 8091 stream): a
retune/gain/side/ring change runs the `STABILITY_DELAY_MS` (3000 ms) cooldown
with the noise OFF -> noise on -> `NOISE_SETTLE_MS` (250 ms, was 1000) gate ->
3 snapshots -> verify: ~3.5 s total (was 4.3 s). The cooldown is a DEBOUNCE
kept at 3 s by choice (each retune restarts it, so a drag calibrates once at
the end and slow tuning doesn't flash the noise source between steps) -
nothing physical needs it: with the noise source on during the cooldown, the
tuner phases sit within 0.2-0.4° of final from the first 50 ms after 1 / 30 /
400 MHz steps, and a 500 ms cooldown recalibrates in ~1.0 s with the same
Check Calibration residual (0.25° vs 0.20° - the check's rounding). The noise
reaches the stream in the first packet after the flag flips, at full power,
and the phases settle within ~20 ms. Runtime override: control port
`set_stability_delay` (100-10000 ms; the continuous scanner uses 500 while it
runs). The DoA client holds DoA on heimdall's WAITING_FOR_STABILITY state (min
500 ms) and the noise flag (+250 ms) instead of a fixed 4 s.

**Lag check during the phase recal** (`check_lag_during_phase_recal()` in
compensation.cpp, added 2026-11-12): a retune / gain change recalibrates
phase only and keeps every lag machine CONVERGED, and a CONVERGED lag machine
ignores its readings - so a sample slip since the lag was acquired survived
the recal, which then reached CONVERGED on a misaligned array (seen as lag
jumps of exact multiples of 94 samples: the RTL2832U FIFO drops whole
188-byte TS packets when its bulk reads run dry, see
`../../../docs/dev/librtlsdr-fork.md`; reported from the field with no
tuner operation in between, CONVERGED shown, fixed by a manual lag reset).
Now, while a phase stage runs on the settled noise source (same
`NOISE_SETTLE_MS` gate), a channel whose |lag| exceeds
`PERIODIC_RECAL_LAG_THRESHOLD` (1 sample) on 2 consecutive readings goes back
to MEASURING (servo) and the phase stage to WAITING_FOR_LAG_COMPLETION; channels
within tolerance are untouched, so an aligned array recalibrates exactly as
before. The servo pulls a few hundred samples back in ~1-3 s (up to ~858
samples/s). Between retunes the 5-minute check (|lag| > 1 sample -> full
recal) remains the only lag check. Not yet verified on hardware with a real
slip.

*No peak at all* (2026-11-21, the Pi round of the sweep work): when both bad
readings have no correlation peak (peak / mean < `LAG_NO_PEAK_RATIO` 8,
against ~150-190 for a real lag on the noise source), the channel is more
than the correlator's window off - a whole packet, from a pairing broken
after the re-pairing flush (a lost USB transfer, or the flush cut while a
reader ran a packet late). The servo can't pull that in. Seen on the Pi 5:
1 of 10 returns from sweep mode left channel 1 with peak / mean ~5, its
servo chased noise for 60 s until `lag_no_peak`, and the array stayed
uncalibrated until the next mode change. Nothing called the coherence
watchdog: it acts only on `coherence_lost` (overflow, stall, a failed
re-pairing) and `force_recalibration`, and neither this check (which assumed
a slip inside the window) nor the no-peak warning (warning only) raised
either. Now such a channel raises `signal_coherence_lost` (one coherence
event; the phase stage waits so nothing latches before the watchdog's next
100 ms tick), and the recovery's flush re-pairs the streams and runs the full
lag + phase recalibration (`--kerberos`: uncalibrated, as for any coherence
loss). Behind it, a channel this check handed to its servo
(`reacquire_from_recal`) that ends in the no-peak warning also raises it.
Fake (one packet lost on channel 1 as the noise comes on, FAKE_DROP1_FILE):
before, `lag_no_peak` [1] and no recovery; now recovered on its own,
CONVERGED 5.4-5.5 s after the switch, check |lag| 0.0. Hardware and the
cause: `operating-modes.md`, *The failed return on the Pi*.
No loop with a dead noise source (no correlation on any channel: a failed
internal source, an unplugged `--ext_noise` source; the fake's
`FAKE_NOISE_DEAD_FILE`, `t_deadnoise.py`). A retune's phase recalibration
raises ONE coherence event. In the recovery that follows, the lags are not
CONVERGED, so this check returns early: `lag_no_peak` on every channel
after 60 s, the recovery given up after 120 s, and the array stays
uncalibrated, as before the net (no further event in 3.5 min). A return
from sweep mode and a Full Recalibration in that state raise none. With
`--kerberos` none at all: no automatic recalibration runs, and an automatic
coherence loss there only enters the uncalibrated state. With the source
back, a Full Recalibration converges.

**Calibration hold** (control port `{"command":"hold_calibration","hold_ms":N}`,
`set_calibration_hold()` / `calibration_held()` in compensation.cpp; added
2026-10-22 for the DoA client's saved-frequency scan, which searches its bands
with the array uncalibrated and calibrates only where it finds a signal): a
retune / gain change still enters the cooldown (noise off,
WAITING_FOR_STABILITY - clients hold DoA, identity compensation) but the lag
driver doesn't let it expire into `handle_settings_change()` while held, and
the periodic check stands down (`checkable`, `preempted`). A LEASE: N > 0
holds for N ms from now (<= 30000; the client renews it every second for
3 s), so a client that stops renewing - stopped, crashed - lets it lapse and
the pending calibration runs as usual; N = 0 releases at once and, if a hold
was in effect, backdates a pending cooldown's `last_frequency_change` by the
stability delay, so the calibration starts on the lag driver's next tick (~1 s
to CONVERGED instead of 3.5 s) - a 0 with no hold in effect (lapsed, never
sent) leaves an ordinary cooldown alone. A coherence recovery is NOT held (it needs the full recal). Status
JSON `calibration_hold`. Unlike the discrete scanner's deferral (which keeps
the old compensation and CONVERGED across its hops - fine in wideband mode,
no DoA) the held state is WAITING_FOR_STABILITY: a coherent client would
otherwise compute DoA on a calibration from another band. Tested only
against a simulated heimdall in kraken_doa's e2e test (the real dongles were
in use) - the paths are the existing cooldown's, plus one gate.

**Why phase-only**: introduced 2026-09-29 while the IF VGA was under the RTL2832
AGC, when each dongle's gain hunted by up to ~2.5 dB between 16k-sample
snapshots and the former gain correction (averaged from 3 snapshots) locked in
up to ~1.6 dB of error, making the match worse. With the VGA now fixed (below)
the gains are stable to ~0.1 dB, but the noise source clips at that VGA step,
which biases a gain estimate. Re-tested 2026-09-30 with the VGA fixed: the noise-source gain estimate is not
trustworthy - it shifts by 0.7-1.0 dB between VGA step 8 (ADC clipping) and
step 0, and disagrees with the receiver noise-floor ratio by up to 2.4 dB (the
noise source compresses the tuner front end, so it measures saturation levels,
not small-signal gain). Gain correction stays off.
The `NOISE_SETTLE_MS` gate still keeps the USB-ring backlog and noise-source
turn-on out of the estimate.

**IF VGA fixed, as stock osmocom librtlsdr (decided 2026-09-30)**: the fork
leaves the R820T IF VGA under the RTL2832 AGC even in manual gain mode
(`r82xx_set_if_mode()` with if_mode 0 sets VGA register 0x0c bit 4 and
`rtlsdr_vga_control()` enables the demod AGC loop). `set_fixed_if_vga()`
(sdr_init.cpp) re-fixes it after every gain setup - device open and each
`update_sdr_settings()` gain change - via `rtlsdr_set_tuner_if_mode(10000 +
index)`: `R820T_IF_VGA_MANUAL_IDX` 8 (16.3 dB) / `R820T_IF_VGA_AUTO_IDX` 11
(26.5 dB) in config.h. Measured on hardware:
- gain stable to ~0.1 dB between snapshots; no noise-on ramp
- the noise source clips ~11-14% of samples at step 8 regardless of RF gain
  (it compresses the tuner front end, so lowering the RF gain doesn't help);
  simulated phase bias from that clipping is <= ~0.26° - accepted
- strong antenna signals clip at 49.6 dB RF gain, clean at ~42 dB
- switching the VGA to a low step only while the noise source is on was
  tested and REJECTED: the VGA step itself shifts each dongle's phase by a
  different amount (~1° at step 1, up to ~8° at step 0, vs step 8), so
  calibration and operation must run at the same step
- `set_if_vga` control command `{"command":"set_if_vga","index":0-15}` fixes
  the step on all open tuners at runtime (`set_if_vga_all()`), for tuning /
  diagnostics; not persisted, the next gain change re-applies the configured step

**Application**: One complex multiply per sample by
`phase_compensation->compensation_vector[channel]` (unit magnitude; the S2P
forward compensation, when enabled, is folded into the same scalar and may
carry gain) in `samples_to_complex_with_compensation()`. Each element
(`AtomicComplex`, core/atomic_complex.hpp) packs real+imag into ONE 64-bit
atomic, so the hot loop can never read a torn vector mid-calibration.

**Visibility**: the control-port status JSON (port 8092, 2 Hz) reports the live
applied vector per channel as `"channel_comp":[{"amp_db":..,"phase_deg":..},..]`
(`amp_db` is always 0). The web UI channel table shows the applied phase in
its **Correction** column next to the live measured **Phase** / **Amp** (gain vs
the reference, EWMA-smoothed against single-snapshot jitter); both ride the
8070 binary correlation message after the phases array
(`build_correlation_message` in correlation.cpp ↔ `processData` in index.html).

### Calibration Buttons (web UI, Calibration card - "Compensation Status" until 2026-11-18, `web-ui.md`)

- **Check Calibration** (`CHECK_CAL`; control port `check_calibration`): the
  periodic monitor runs its non-destructive check at the next tick - briefly
  pulses the noise source, measures the residual lag/phase on the compensated
  stream, and sets `force_recalibration` only if out of tolerance. Works with
  the periodic check disabled; not in plain `--kerberos` (it pulses the noise).
  Result in the web STATE `cal_check` (`ok` / `drifted` / `no_verdict`, worst
  lag + phase), shown under the buttons ("Last check")
- **Full Recalibration** (`FORCE_RECAL`): the watchdog's full lag + phase
  recalibration from scratch (confirm dialog; KerberosSDR: disconnect antennas).
  `RESET_LAG_COMPENSATION` / `reset_lag_compensation` remain as aliases

