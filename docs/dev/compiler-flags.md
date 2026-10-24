# Compiler flags

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

### Compiler flags (2026-10-22, the user's request: full optimisation on the Pi 5)

- kraken_doa `-O3 -ffast-math -fno-finite-math-only -march=native
  -mtune=native` (2026-10-23; it was `-Ofast` - below); heimdall
  `-O3 -mcpu=native` on aarch64 (was `-march=armv8-a -mtune=cortex-a72` -
  the Pi 4's core), `-march=native` x86_64; the plugins + libkrakendig
  `-O3` (was `-O2`) + `-mcpu=native` on aarch64 (`-march=native` alone
  tunes for a generic ARMv8 core); liquid-dsp / pffft / librtlsdr /
  whisper.cpp as before (Release / -O3, native). Windows keeps the portable
  `WIN_MARCH` everywhere
- heimdall and the plugins: `-fno-math-errno` (sqrt & co. without an errno
  path - such loops vectorize) and `-fcx-fortran-rules` (std::complex * and /
  without C99's inf / NaN recovery: GCC keeps a `__mulsc3` / `__divsc3` call
  in every complex multiply otherwise, and the loop doesn't vectorize -
  "unsupported control flow"; division keeps its range scaling). NOT
  `-ffast-math` there: its `-ffinite-math-only` folds `std::isfinite` to
  true (NaN = unknown in `kp::MapPoint`, see `is_finite_value()`), and its
  reassociation would change results. `std::abs` of a complex stays a
  `hypotf` call without fast-math: heimdall's correlation uses
  `std::sqrt(std::norm(z))` / a norm compare for its whole-array loops.
  Measured 2026-10-23 (the user's question): the plugins with `-ffast-math
  -fno-finite-math-only` decoded 34 / 35 test transmissions identically (one
  printed time 1 ms off) for 1.8 % less CPU, with `-ffinite-math-only` too
  32 / 35 changed (adsb altitudes `nan m`, cells found no cell); heimdall's
  correlation benchmark -1 % - not taken
- kraken_doa: `-Ofast` (= `-O3 -ffast-math` + `-ffinite-math-only` +
  `-fallow-store-data-races`) replaced by `-O3 -ffast-math
  -fno-finite-math-only` (2026-10-23, the user's call): std::isfinite /
  std::isnan work again - under -Ofast they were constants, and 7 checks
  were dead (a NaN in `SF_SET` / `SF_LOC_SET` JSON - geo::Json's reader takes
  `NaN` - passed the range tests, likewise a SigMF's station / track,
  ai_capture's JSON numbers, rdf_engine's normalisation) - and GCC may no
  longer add stores other threads race with. `is_finite_value` stays the
  way (right under any flags). Measured on the Pi 5 (test build + fake
  heimdall, user-space cycles, % of a core at 2.4 GHz, 3 rounds each;
  -Ofast / the new flags / strict `-O3 -fno-math-errno -fcx-fortran-rules`):
  1 VFO 12.5 kHz 14.6 / 14.3 / 18.1, 3 VFOs 20.2 / 20.0 / 23.5, 5 VFOs
  26.3 / 26.6 / 29.7, 5 x 240 kHz 85.5 / 85.0 / 91.6, 1 x 240 kHz 21.9 /
  22.0 / 25.1, + beamforming 15.2 / 15.3 / 18.4; the bearings the same in
  all three. Strict costs ~3.3 points whatever the VFOs (per-packet work).
  End to end (test build): `SF_LOC_SET` / `SF_SET` with `"lat": NaN` /
  `"hz": NaN` - the -Ofast build took both (the broadcast list and
  saved_frequencies.json then held `nan`: invalid JSON), the new one refuses
  them ("bad position" / "bad frequency"). A build from before recompiles
  every object once (`obj/.flags`), on the Pi and on Windows
- A stamp of the compiler flags (`build/.flags`, `plugins/lib/build/.flags`,
  kraken_doa's `obj/.flags` since 2026-10-23; every platform - it was
  `WIN_MARCH` on Windows only) recompiles everything when the flags change,
  so an existing build picks new flags up
- `-mcpu=native` / `-march=native` builds run only on the kind of CPU they
  were built on (a Pi 5 build on a Pi 4: illegal instruction) - build on
  the device (install.sh, the Dockerfile do)
- Measured on the Pi 5 (GCC 14.2, Debian 13; job scratch 63520c26, results
  in ~/flagtest_527fd2/out on the Pi; A = main, B = these flags):
  heimdall's correlation per 5-channel set (micro-benchmark, one core)
  4.25 -> 2.63 ms (-38 %): peak search 1.02 -> 0.49 ms and display
  magnitudes 1.04 -> 0.10 ms (the norm code), cross-spectrum 1.08 -> 0.94
  ms (the flags), forward FFTs and the conversion worker's loops unchanged;
  the same peaks, display values within 3e-8. On the KrakenSDR (2 rounds A /
  B): the correlation thread while calibrating 46 -> 41-43 % of a core (a
  pass ~8.6 -> ~7.2 ms with its 10 ms sleep), heimdall 58.5 -> 54-56 %;
  CONVERGED in 23-28 s either way (the lag servo's slew), forced recal
  4.6-6.1 s, calibration check OK (|lag| 0.0, |phase| 0.2-0.3 deg), 0
  coherence events; converged 13.8 vs 14.0 % of a core (3 rounds, ranges
  overlapping: unchanged). Plugins: all 34 debug-playback test
  transmissions decoded offline by A and B - outputs identical 34 / 34 (bar
  timing lines), CPU 41.0 -> 38.1 s (-7 %): lora -12..-23 %, p25 -19 %,
  adsb -13 %, cells -7 %, dtv -6 %, the rest within +-5 %. Build on the Pi:
  heimdall 51 -> 41 s, plugins 151 -> 158 s. -O3 found two snprintf
  truncations (`fmt()` 64-byte buffers in dme and cells: the "not a DME
  frequency" fact and the cells "the VFO sees N % ..." note were cut off) -
  fixed

