# liquid-dsp

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**liquid-dsp (built from source: static, without FFTW / libfec) - `liquid.mk`:**
- The user's request (2026-10-17, licensing, after the pffft swap): Debian's
  `libliquid1` 1.7.0-1 is built WITH FFTW (GPL-2.0+) and libfec (LGPL-2.1) -
  its Debian patches add both to liquid's CMake build - so kraken_doa and
  every plugin still loaded FFTW at run time. liquid-dsp itself is MIT
  (Expat). The call graph of that libliquid (objdump): of the 38 liquid
  functions kraken_doa / the plugins import, only `fftwf_malloc / _free` were
  reachable (liquid's own FFT allocates with them, inside `msresamp_*_create`'s
  halfband design); FFTW's transforms sit only in fftfilt / spgram /
  firpfbch* / ofdmframe* / qdetector / qpilotsync / fskdem - none used
- `liquid.mk` - ONE recipe: included by the Makefile (`LIQUID_ROOT` .) and
  plugins/Makefile (..), run by CMake (`make -f liquid.mk liquid`, custom
  target `liquid_build`) and by the Dockerfile (a layer of its own, before
  the sources are copied). It fetches `LIQUID_COMMIT` (4dda702 = v1.7.0, the
  version Debian 13 ships: liquid.h byte-identical, Debian's changes are
  build-only) with a shallow fetch into `liquid-dsp/` (gitignored), runs
  liquid's own CMake (examples / autotests / benchmarks off, `-march=native`,
  armv7 `-mfpu=neon`) - v1.7.0's CMake never looks for FFTW or libfec
  (`config.h` has neither: its own FFT, libfec's codes compiled out) - builds
  its shared target `liquid` (1.7.0 has no static target) and archives that
  target's objects (`CMakeFiles/*.dir/*.o`, 159) as
  `liquid-dsp/build-kraken/libliquid.a`, the header at
  `build-kraken/include/liquid/liquid.h`, the commit in `build-kraken/commit`;
  the archive is renamed into place LAST (it existing = a complete build).
  Log: `liquid-dsp/build-kraken.log`. `+` on the build line: liquid's make
  joins the jobserver (-j3 stays -j3). ~22 s on the Pi 5. A build from
  another commit (an older pin) is removed at parse time - the whole
  build-kraken - and made again; `make distclean` removes `liquid-dsp/`
- Linking: kraken_doa `$(TARGET): $(OBJS) $(LIQUID_A)` ($^ ends with the
  archive); every plugin and `lib/build/codecs`: `$(LIB_A) $(LIQUID_A)`.
  Included with `-isystem $(LIQUID_INC)` - as a non-system header liquid.h
  warns about its own deprecated `eqlms` objects in every file; the system
  copy (/usr/include) was silent - so the dependency files don't list it and
  objects depend on `$(LIQUID_H)` explicitly (its rule order-only on the
  archive; `cp -p` keeps the checkout's time: a rebuild of the same commit
  recompiles nothing, a new commit everything that includes it). The
  `plugins` target waits for the archive (two builds of it at once otherwise).
  `ldd` of kraken_doa / every plugin / codecs: no liquid, FFTW or libfec;
  the Docker image's runtime packages (from `ldd`) lose libliquid1 + libfec0
  (and libfftw3-single3 with heimdall on pffft). A system libliquid-dev may stay
  installed: `-isystem` dirs come before /usr/include, the archive is linked
  by path
- What changes in the DSP: liquid's CMake picks the NEON dot products
  (`dotprod_*.neon.c`; SSE / AVX on x86) - Debian's build had the portable C
  ones. Outputs (scratch lq/bench, same input): firdecim / firfilt / msresamp
  differ by float rounding (max 1.5e-7 .. 7.9e-7 of full scale - the sums'
  order), agc / iirfilt bit-identical. Speed (Pi 5, one core): firdecim_cccf
  (kraken_doa's decimator stages, M 5) 21 taps 15.8 -> 10.4 ns per input
  sample, 63: 32.9 -> 16.0, 127: 53.5 -> 21.4, 255: 108 -> 32 (1.5-3.4x);
  firfilt_crcf 63 taps (the plugins' FM front end) 89 -> 37, RRC 161 taps
  207 -> 61; msresamp 1.1-1.4x; agc equal; iirfilt 1.1x. msresamp creation
  unchanged (~74 ms at rate 0.02: liquid's firdespm halfband design, its own
  FFT 10-20 % of it - 1.8.x designs those halfbands with Kaiser windows, not
  taken: other filters)
- Without libfec, liquid's convolutional / Reed-Solomon `fec` schemes are
  compiled out (`fec_create` fails) - plugins/SDK.md sends plugin authors to
  `kp::` / `lib/dig_fec`; liquid's FFT-based objects use its own FFT (~8x
  pffft's time at 1200 points)
- Tests (2026-10-17, job scratch 46eef211 lq/): every catalogue test
  transmission (all 34) decoded offline by the plugins built on Debian's
  liquid and on this one, same arguments: every output identical except
  the decode-time lines; plugin CPU equal or lower (-8 .. -15 % for the FM
  front-end decoders p25 / dmr / nxdn / tetra / mpt1327 / radiosonde). Core
  (t_core_ab.py: fake heimdall, 5-antenna UCA, three VFOs, DoA + then
  beamforming; two runs each): kraken_doa 106.6-107.0 % of a core -> 84.0-
  84.3 % (with beamforming 107.3-107.7 -> 85.8-88.2 %) - the decimators'
  NEON dot products; spectrum mean |difference| 0.010 dB (max 0.055), DoA
  identical every frame, audio / beamformed tones within their run-to-run
  spread. Build: a fresh `make` 22 s for liquid, every object recompiled
  once after the switch, `make` again a no-op; liquid prints no warnings;
  the CMake recipe checked in a scratch project

