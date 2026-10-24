# heimdall configuration (config.h, ingest robustness, realtime scheduling)

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

## Configuration

All runtime configuration is in `config.h` at the project root:

**Key Settings:**
- `NUM_DEVICES`: compile-time CEILING on device count (default 8). The runtime
  count is `active_num_elements`: `-n` flag > persisted `num_elements` setting
  > count of expected serials attached on USB (`--serials s0,s1,...`, default
  "1000".."1007" — Kraken convention extended to the ceiling, so a stock
  KrakenSDR auto-detects 5 and a KerberosSDR 4). Change
  at runtime via the web UI "Array Elements" selector / `NUM_ELEMENTS:<n>` WS
  command / `set_num_elements` control command (devices reopen + full recal;
  see `src/sdr/pipeline_control.cpp`). Unopened dongles stay free on USB for
  other programs
- `REF_CHANNEL`: Reference channel index (default 0)
- `CENTER_FREQ`: RF center frequency in Hz (default 100 MHz)
- `SAMPLE_RATE`: Sample rate in Hz (default 2.4 MSPS)
- `NUM_SAMPLES`: Samples per FFT (must be power of 2, default 16384)
- `GAIN`: Gain in tenths of dB (default 496 = 49.6 dB)
- `WEB_PORT`: Web interface port (default 8070)
- `RTL_TCP_PORT`: RTL-TCP server port (defined in types.hpp, default 1234)
- `TCP_DATA_PORT`: Multi-channel data streaming port (defined in types.hpp, default 8091)
- `TCP_CONTROL_PORT`: JSON control interface port (defined in types.hpp, default 8092)

**Optimization Flags:**
- `ENABLE_BIAS_TEE`: Enable bias-tee on all devices (default 1)
- `USB_RESET_ON_INIT`: Reset USB on initialization (default 1)
- Uses ARM NEON vectorization on aarch64, automatically falls back to scalar on x86_64
- Compiled `-O3 -fno-math-errno -fcx-fortran-rules` + `-mcpu=native` on
  aarch64 (`-march=native` x86_64; docs/dev/compiler-flags.md). The
  correlation's whole-array |z| loops use `std::sqrt(std::norm(z))` / a
  norm compare - `std::abs` of a complex is a `hypotf` call per element

**Coherence / Ingest Robustness:**
- `RTL_USB_BUF_COUNT`: librtlsdr async USB transfer buffers (default 32 ≈ 218 ms cushion @ 2.4 MSPS; 0 = library default 15). Wider ring = more slack before the RTL2832 FIFO overflows if the reader thread stalls.
- `L2_RAW_MAX`: L2-raw staging depth cap (default 16). Bounds added latency under conversion overload; the per-device sample pool is sized from this.
- `STUCK_DEVICE_MAX_TIMEOUTS`: consecutive 100 ms drain timeouts on one device before declaring coherence lost (default 30 ≈ 3 s).
- `RT_PRIO_USB_READER`, `RT_PRIO_SAMPLE_DRAIN`: SCHED_RR realtime priorities (best-effort). The conversion worker deliberately runs at normal priority.
- `ENABLE_COHERENCE_MONITOR`: low-rate streaming coherence backstop (default **0/OFF**). A differential cross-correlation heuristic; needs ≥3 elements and on-hardware threshold tuning before enabling (a false positive triggers a disruptive ~30–60 s recalibration). The application-level detectors (L1 overflow / pool exhaustion / stuck device) are always on and cover the high-CPU-load case.

**Realtime scheduling (optional):** the USB readers and sample drain use SCHED_RR if the process is granted it. Without privilege they run at normal priority (safe default). To enable: add `<user> - rtprio 30` to `/etc/security/limits.conf` (or grant `CAP_SYS_NICE`). limits.conf only covers PAM logins: the boot service (`install-pi-service.sh`) sets `LimitRTPRIO=30` in its unit instead.

