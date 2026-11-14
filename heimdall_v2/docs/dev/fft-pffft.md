# FFT (pffft)

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

### FFT (pffft)

- The user's request (2026-10-17, after kraken_doa_v2's swap): FFTW (GPL)
  replaced by pffft (FFTPACK / BSD-style) so the suite needs no FFTW at all -
  `external/pffft` = the same files as kraken_doa_v2's (each app builds on its
  own; keep the copies identical). Built as C with `-DPFFFT_ENABLE_NEON` on
  ARM (Makefile `PFFFT_CC` / `PFFFT_CFLAGS`, CMake target `pffft`); without it
  pffft silently builds its scalar code. `config.h`'s `FFTW_PLANNER_EFFORT`
  was dead (the plans were always FFTW_ESTIMATE) and is gone
- `src/dsp/fft_plan.cpp`: `FftPlan` keeps its interface; pffft directly
  (`pffft_transform_ordered` = FFTW's natural order, unscaled both ways, e^-
  forward) - `CORRELATION_SIZE` (2 x NUM_SAMPLES) is a power of two, which
  pffft takes as it is (static_assert). The forward transform zero-pads
  straight into `output` and runs IN PLACE; pffft's scratch is ONE buffer per
  thread (`thread_work()`), not per plan. Both matter: a correlation pass
  walks 5-9 plans of 256 KB buffers, more than the Pi 5's L2 - the first
  port (separate input + per-plan scratch, as FFTW had) was only 4 % faster
  per pass although each transform alone was 25-28 % faster.
  `correlation_freq` stays a separate buffer: correlation.cpp reads the
  cross-spectrum after `cross_correlate`
- Benchmark (2026-10-17, job scratch 46eef211 hp/: bench_fft.cpp drives the
  old and the new `FftPlan` with correlation.cpp's per-set sequence on a
  5-channel noise-source scene; heimdall's own -O3 -march=armv8-a flags, Pi
  5): forward (copy + zero-pad + FFT, 32768 points) 305-320 -> 200-210 us,
  cross_correlate 365-375 -> 264-275 us, inverse 310-320 -> 220-235 us; a
  whole 5-channel set (5 forwards, 4 cross-correlations, 4 peak searches)
  3.65-3.67 -> 2.22-2.29 ms (-39 %); correlation outputs identical to 1.9e-7
  of the peak (same lags, phases to 0.0001 deg). On the KrakenSDR (hb.py:
  416.839 MHz, 5 channels, the user's settings copied, old HEAD build vs this
  one, alternately): every run converged (5 + 5), 0 coherence events; during
  the calibration heimdall 60.8-61.1 -> 57.8-57.9 % of a core, the
  correlation thread 47-49 -> 44-45 % - with its 10 ms loop sleep that is
  ~9.2 -> ~8.0 ms per correlation pass (the FFTs' 1.2 ms; the rest is the
  eigen solve, peak interpolation and the WebSocket message), so ~7 % more
  lag / phase readings per second for less CPU; startup to CONVERGED 22-36 s
  either way (the lag servo's slew, not the FFT). Once CONVERGED the
  correlation FFT is off: 12.5-14.2 % of a core for both (conversion worker +
  USB readers) - pffft changes nothing there

