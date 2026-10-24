# FFT (pffft)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**FFT (pffft) - `external/pffft/` + `plugins/sdk/kraken_fft.hpp`, kraken_doa and every plugin:**
- The user's request (2026-10-17): swap FFTW for pffft (FFTW is GPL-2.0+;
  the licensing note of 2026-10-14 picked pffft after a Pi 5 benchmark).
  pffft = the marton78 fork (commit aa16fd3, 2026-09-29), single-precision
  core only, VENDORED unmodified in `external/pffft/` (FFTPACK / BSD-style
  `LICENSE.txt`, README.md with the files + commit) - no system FFT package
  for kraken_doa. Built as C (`gcc -O3`) with **`-DPFFFT_ENABLE_NEON` on ARM
  - without it pffft silently builds its scalar code** (both Makefiles +
  CMake add it; x86 picks SSE itself). kraken_doa links the two objects;
  the plugins get them in `lib/build/libkrakendig.a`; `.dockerignore`
  allows `kraken_doa_v2/external/`
- `plugins/sdk/kraken_fft.hpp` (header-only, used by the core too -
  `-Iplugins/sdk -Iexternal/pffft`): FFTW-shaped plans so the port was
  call-for-call - `kfft_complex` (= `float[2]`, so `x[i][0]` / `[1]` code
  stayed), `kfft_alloc_complex / _real / kfft_malloc / kfft_free` (64-byte
  aligned), `kfft_plan_dft_1d(n, in, out, KFFT_FORWARD | KFFT_BACKWARD)`,
  `kfft_plan_dft_r2c_1d(n, in, out)` (FFTW's n/2 + 1 layout, unpacked from
  pffft's ordered real output), `kfft_execute`, `kfft_execute_dft(_r2c)`
  (other arrays), `kfft_destroy_plan` (null-safe), `kfft_direct(n)`,
  `kfft_simd()`. Results = FFTW's: forward e^-, unscaled, natural bin order
  (`pffft_transform_ordered`). ANY n: pffft takes n = 2^a 3^b 5^c and a
  multiple of 16 (real: 32) directly; any other n (lora's narrow channels,
  dtv / cells OFDM sizes at the VFO rate, primes) goes through BLUESTEIN
  on a pffft of the nearest valid m >= 2n - 1 (chirp angles from k^2 mod 2n
  in integers + double, the filter's FFT kept in pffft's z-domain,
  `pffft_zconvolve_scale`) - ~3 transforms of m. In place and unaligned
  buffers work (unaligned ones are copied through an aligned scratch).
  Plans are thread-safe to make (no planner lock - `fftw_planner_mutex`,
  `FFTProcessor::export_wisdom` and the wisdom file are gone); one plan per
  executing thread (it owns its scratch)
- Ported: fft_processor (per-channel spectra, beamformed FFT), beamformer
  (SNR FFT, FD-DAS 256 forward / inverse), plugins beacon406, cells
  (dsp_util Fft + tools/cell_gen), analog_id (r2c), dme, dtv (dsp_util Fft),
  flarm, ils (incl. r2c), ism, lora (channelizer, phy, search, decoder),
  wildlife; `sdk/tools/make_testsignal.py` compiles .cpp generators with
  `-I../external/pffft` + libkrakendig.a. heimdall_v2 followed the same day
  (its own copy of these files, `FftPlan` on pffft directly - heimdall_v2/
  CLAUDE.md *FFT (pffft)*); gr-krakensdr_v2 keeps FFTW (the user's call: GNU
  Radio needs it anyway). Debian's `libliquid1` still loaded
  `libfftw3-single3`: liquid-dsp is built from source without it now - see
  *liquid-dsp*
- Accuracy (scratch pf/t_kfft.cpp vs FFTW double): every n 1..80 and 96 ..
  99991 (primes, 2150, 24000, 48000, 65536), forward + backward, in place /
  unaligned: worst relative error 4.4e-7 direct, 6.2e-7 Bluestein (FFTW
  float 3.1e-7); r2c 1 .. 48000 <= 4.4e-7. Speed (pf/b_kfft.cpp, one Pi 5
  core): vs FFTW_ESTIMATE (what the plugins used) equal or faster at every
  valid size (4608: 24 vs 48 us, 24000: 150 vs 232), vs FFTW_MEASURE (the
  core) 10-30 % slower at <= 4096, equal at 16384 (89 us), faster above;
  Bluestein 2150 54 vs 44-48 us
- Regression (2026-10-17, job scratch 46eef211 pf/: gen.sh + run_ab.py, the
  catalogue's 19 FFT-plugin test transmissions generated once - 2.5 GB,
  deleted afterwards -, narrow ones with noise added; OLD = the FFTW builds
  of main, NEW = pffft, the same arguments; outputs in pf/ab_out/): every
  plugin's offline output identical to the old one except its decode-time /
  CPU-share lines - the same frames, talkers, bearings, levels; CPU of the
  runs equal or 5-12 % lower. Core (scratch t_core_ab.py, FFTW vs pffft
  test builds alternately, fake heimdall: CW + two FM tones on a 5-element
  UCA): averaged spectrum mean |difference| 0.04 dB (max 0.2 dB = the
  0.47 dB display step), peaks within 0.03 dB; DoA of three VFOs identical
  every frame; audio tones and beamformed (FD-DAS) tones identical; the
  beamformer's SNR-boost estimate the same spread; kraken_doa CPU 106.5-
  106.8 vs 106.6-107.5 % (equal). Debug playback through the pffft build
  (t_pb_pf.py): ism-six-devices 6 / 6, lora-mesh-five 6 / 6, dtv-dvbt-paths
  3 / 3, beacon406-five 5 / 5, cells-two-lte 2 / 2, wildlife-five-survey
  5 / 5 talkers at their bearings

