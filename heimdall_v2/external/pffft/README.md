# pffft (vendored)

heimdall's FFT library (the correlation FFTs, `src/dsp/fft_plan.cpp`):
PFFFT, "a pretty fast FFT" by Julien Pommier, maintained fork by Hayati
Ayguen / Dario Mambro / marton78 - https://github.com/marton78/pffft, commit
`aa16fd3db58de4ba5dae8b0438440bb9da46b6fa` (2026-09-29). The SAME files as
`kraken_doa_v2/external/pffft` (each app builds on its own) - keep the two
copies identical.

Licence: FFTPACK (BSD-style, `LICENSE.txt` - keep it with the files and in
any binary distribution's notices). It replaced FFTW (GPL-2.0+) here on
2026-10-17, after kraken_doa_v2.

Only the single-precision core is copied, unmodified:

| file | from |
|---|---|
| `pffft.h` | `include/pffft/pffft.h` |
| `pffft.c`, `pffft_common.c`, `pffft_priv_impl.h` | `src/` |
| `simd/pf_float.h`, `pf_sse1_float.h`, `pf_fma_x86.h`, `pf_neon_float.h`, `pf_scalar_float.h`, `pf_altivec_float.h`, `pf_wasm_float.h` | `src/simd/` |

Build: plain C (`gcc -O3 -c pffft.c pffft_common.c`). **On ARM add
`-DPFFFT_ENABLE_NEON`** - without it pffft silently builds its scalar
code (several times slower); x86 picks SSE by itself. heimdall's Makefile
and CMakeLists.txt add it.

heimdall calls pffft directly (`pffft_transform_ordered`): its only size is
`CORRELATION_SIZE` = 2 x `NUM_SAMPLES`, a power of two, which pffft takes
as it is (complex: a multiple of 16 of the form 2^a 3^b 5^c). Output order
and scaling are FFTW's: natural bin order, unscaled in both directions.

To update: copy the same files from a newer commit into BOTH copies, then
run the checks in `kraken_doa_v2/docs/dev/fft-pffft.md` and
`heimdall_v2/docs/dev/fft-pffft.md`.
