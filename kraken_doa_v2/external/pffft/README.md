# pffft (vendored)

kraken_doa's FFT library (and the decoder plugins'): PFFFT, "a pretty fast
FFT" by Julien Pommier, maintained fork by Hayati Ayguen / Dario Mambro /
marton78 - https://github.com/marton78/pffft, commit
`aa16fd3db58de4ba5dae8b0438440bb9da46b6fa` (2026-09-29).

Licence: FFTPACK (BSD-style, `LICENSE.txt` - keep it with the files and in
any binary distribution's notices). It replaced FFTW (GPL-2.0+) on
2026-10-17: on a Raspberry Pi 5 it ties FFTW at 16384-65536 points and is
10-25 % slower at <= 4096 (one core, NEON).

Only the single-precision core is copied, unmodified:

| file | from |
|---|---|
| `pffft.h` | `include/pffft/pffft.h` |
| `pffft.c`, `pffft_common.c`, `pffft_priv_impl.h` | `src/` |
| `simd/pf_float.h`, `pf_sse1_float.h`, `pf_fma_x86.h`, `pf_neon_float.h`, `pf_scalar_float.h`, `pf_altivec_float.h`, `pf_wasm_float.h` | `src/simd/` |

Build: plain C (`gcc -O3 -c pffft.c pffft_common.c`). **On ARM add
`-DPFFFT_ENABLE_NEON`** - without it pffft silently builds its scalar
code (several times slower); x86 picks SSE by itself. The build prints a
`#pragma message` naming the SIMD it chose.

Use it through `plugins/sdk/kraken_fft.hpp` (FFTW-like plans, any size,
FFTW's output order and scaling) - not directly: pffft only takes sizes
N = 2^a 3^b 5^c that are multiples of 16 (complex) / 32 (real), 16-byte
aligned buffers, and orders its output its own way.

To update: copy the same files from a newer commit, then run the checks in
`kraken_doa_v2/docs/dev/fft-pffft.md`.
