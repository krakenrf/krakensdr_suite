# heimdall_v2/CLAUDE.md

heimdall: the C++20 server that opens the RTL-SDR dongles, keeps them
sample-coherent (lag servo + phase calibration on the noise source) and
streams the compensated IQ on 8091, with JSON control on 8092, RTL-TCP on
1234 and a web UI on 8070. The suite-wide rules in `../CLAUDE.md` apply
here too; this file adds what heimdall work needs. Details are in
`docs/dev/` (not loaded automatically) - read the file for the area you
are changing:

| Area | Read |
|---|---|
| Modules and key files, data flow, threads and locks, debugging, dependencies, manual tests | `docs/dev/architecture.md` |
| `config.h` settings, ingest robustness knobs, realtime scheduling | `docs/dev/configuration.md` |
| Lag servo, phase calibration, retune timing, calibration hold, why phase-only, fixed IF VGA, calibration buttons | `docs/dev/calibration.md` |
| Drag retunes (fast path), failed-retune replies, measurements | `docs/dev/drag-retunes.md` |
| Operating modes (coherent / wideband scan / independent) | `docs/dev/operating-modes.md` |
| Coherence-loss detection and recovery | `docs/dev/coherence-recovery.md` |
| `--kerberos`, `--kerberos_sw`, `--ext_noise` | `docs/dev/kerberos-and-ext-noise.md` |
| `--wideband` downconverter (moRFeus LO, mixer sides, RF switches) | `docs/dev/wideband-variant.md` |
| FFT (pffft) | `docs/dev/fft-pffft.md` |
| The librtlsdr fork, Windows USB behaviour | `../docs/dev/librtlsdr-fork.md`, `../docs/dev/windows.md` |
| 8091 / 8092 wire protocols | `../gr-krakensdr_v2/docs/heimdall_v2_data_protocol.md`, `..._control_protocol.md` |

## Build

```bash
make            # also clones the pinned uWebSockets and builds ../librtlsdr's static lib if missing
make run | status | debug | clean | rebuild | distclean | deps | deps-fedora | deps-arch
```

CMake works too (`mkdir build && cd build && cmake .. && make -j3`) once
`make check-uws` has fetched uWebSockets and `../librtlsdr/build/src/librtlsdr.a`
exists. `index.html` sits in `heimdall_v2/` (not `src/web/`) and is read at
startup by `html_loader.cpp`.

## Where things are

- `src/core/`: `types.hpp` (state enums, `CorrelationResult`,
  `PhaseCompensationData`, `ChannelCompensation`, `OperatingMode`),
  `config.hpp` (wraps `config.h`; `kerberos_manual_cal_only()`,
  `external_noise_mode`), `buffer_pool.hpp`, `atomic_complex.hpp`,
  `logging.hpp` (`LOG_I/W/E`), `utils` (`set_thread_realtime()`), and the
  copies shared with kraken_doa (`net_compat.hpp`, `platform.*`,
  `fs_compat.*`, `host_check.hpp`, `log_sanitize.hpp`).
- `src/sdr/`: `sdr_device.cpp` (L1 buffers, the RTL callback), `sdr_init.cpp`
  (enumeration by serial, `update_sdr_settings()`,
  `set_bias_tee_all_devices()`, `set_operating_mode()`,
  `wideband_retune_rf()`, `for_each_tuner`), `sdr_pipeline.cpp` (drain,
  conversion worker, `signal_coherence_lost()`), `pipeline_control.cpp`
  (thread lifecycle, `reconfigure_num_elements()`), `downconverter.cpp`,
  `kerberos_gpio.cpp`.
- `src/dsp/`: `fft_plan.cpp` (`FftPlan`, `FftPool`), `correlation.cpp`
  (`process_correlations()`, Gaussian peak interpolation, the 8070
  correlation message), `compensation.cpp` (lag / phase state machines,
  coherence watchdog, `recover_coherence()`, calibration hold, KerberosSDR
  states).
- `src/net/`: `tcp_data_server.cpp` (8091, header flags),
  `tcp_control_server.cpp` (8092, `handle_command()`, status JSON at 2 Hz),
  `rtl_tcp_server.cpp` (1234). `src/web/web_server.cpp`: 8070.

Threads: one USB reader per dongle and the sample drain (SCHED_RR when the
process may), the conversion worker (normal priority), the correlation
processor, one compensation thread per non-reference channel, the coherence
watchdog, the status broadcaster, the TCP servers, and the uWS loop on the
main thread.

## Invariants

Coherence
- The N-th sample set from every dongle must be the same instant. Drop data
  only as whole aligned sets (at L2-raw), never one device's buffer: a
  per-device drop shifts that channel by a full packet, which no
  compensation can represent. When the callback or the drain can't keep up
  (L1 overflow, pool exhaustion, a stuck device), call
  `signal_coherence_lost()` and let the watchdog recover.
- The RTL callback is allocation-free (`BufferPool::try_acquire`).
- The queue size counters (`l1_buffer_size`, `l2_buffer_size`,
  `l2_raw_buffer_size`) change only by matched `fetch_add` / `fetch_sub`.
  Never `store(0)`: it races the producers and a later `fetch_sub`
  underflows to `SIZE_MAX`, wedging the fullness checks.
- Flushes go through the existing paths: the drain serves L1 flushes
  (`clear_l1_buffer()`, `flush_l1_now()` at a quiet point), and
  `flush_downstream()` bumps `l2_flush_epoch` under `l2_flush_mutex`, which
  the conversion worker re-checks before it broadcasts or pushes a set.
- The conversion worker handles sets in order on one thread (its FIR tails
  depend on it), at normal priority on purpose (realtime would
  priority-invert its malloc / mutex use). Set-wide state is read once per
  set (`SetConversionState`). Compensation vector elements are
  `AtomicComplex` (one 64-bit atomic), so the hot loop never reads a torn
  value.
- The `devices` vector never changes shape after startup (one `SDRDevice`
  per expected serial; inactive channels have `dev == nullptr`). On an
  element-count change, clear and re-initialise the per-channel
  `correlation_result` maps under `data_mutex`; stale entries overflow the
  correlation message (heap corruption).

Calibration
- Calibration advances only while the noise source is on
  (`bias_tee_enabled`); parked states idle the machines by turning the FFT
  off.
- The noise source is switched only through `set_bias_tee_all_devices()`:
  the downconverter's RF switches, the `--kerberos_sw` GPIOs, `--ext_noise`
  and `finish_fast_retunes()` all hook in there. A new calibration entry
  point that skips it (noise already on) must call `finish_fast_retunes()`
  itself.
- Every "calibration stands down" check is `operating_mode != COHERENT`.
  `kerberos_manual_cal_only()` guards every automatic noise-on path - plain
  `--kerberos` must never calibrate on its own.
- A retune / gain / mixer-side / ring change goes through
  `update_sdr_settings()` and the cooldown (`STABILITY_DELAY_MS` 3 s, a
  debounce kept on purpose) -> noise on -> `NOISE_SETTLE_MS` -> snapshots ->
  verify. A change during a recovery restarts its phase stage; the discrete
  scanner defers calibration (`scanner_cal_deferred`); `hold_calibration` is
  a lease, and a coherence recovery is never held.
- The lag servo writes only the sample-clock register
  (`rtlsdr_set_sample_freq_correction_f`). The old PLL "bursts"
  (`rtlsdr_set_freq_correction`) were removed for measured reasons
  (`docs/dev/calibration.md`); don't bring them back.

Hardware and replies
- `init_rtlsdr_device()` fails a channel whose sample rate, centre frequency
  or buffer reset can't be set (a mistuned dongle breaks coherence); other
  setup calls only warn.
- Tune through `for_each_tuner`: one by one on Windows (parallel control
  transfers time out there), parallel threads on Linux - each thread touches
  only its own handle and result slot, and the shared atomics are written
  only under `device_io_mutex`.
- A failed retune or gain change replies an error carrying the previous
  value (`SdrUpdateResult`); the bool `update_sdr_settings()` returns means
  "hardware written, recalibrate".
- 8092: `TCP_NODELAY` on every platform, no auth but it hangs up on HTTP
  requests, and only `get_status` is answered during an element-count
  change. 8091 / 1234 stream bulk data and keep Nagle.
- Downconverter HID reports are exactly 17 bytes with a leading 0
  report-id; a malformed frame wedges the moRFeus until a power cycle.
  High-side injection is undone by negating Q in the conversion loop.

Persistence (`heimdall_settings.conf`)
- The last user tuning (frequency, gain) is saved after 10 s unchanged and
  restored before the dongles open; scanner hops are not saved. The
  operating mode and per-tuner tuning are saved (`settings::note_mode`).
  An element count is saved only when chosen at runtime
  (`persisted_num_elements`), never from `-n` or `--kerberos`.

Performance
- Flags `-O3 -fno-math-errno -fcx-fortran-rules`, no fast-math. For array
  magnitudes use `std::sqrt(std::norm(z))` or compare norms: `std::abs` of a
  complex is a `hypotf` call per element.
- pffft: forward transforms in place, one scratch buffer per thread
  (`thread_work()`), `CORRELATION_SIZE` a power of two. Keep
  `external/pffft` identical to kraken_doa's; it needs
  `-DPFFFT_ENABLE_NEON` on ARM or it silently builds scalar code.

## Adding things

- A control command: `src/net/tcp_control_server.cpp` `handle_command()`;
  document it in `README.md` and the gr protocol doc; test with
  `echo '{"command":"..."}' | nc localhost 8092`. New state belongs in the
  8092 status JSON and the web STATE message.
- A web feature: `src/web/web_server.cpp`, plus `index.html` (read at
  runtime).
- A module: add its sources to the Makefile (`SOURCES`, the build dirs) and
  `CMakeLists.txt`, and keep the dependency order of the root file.

## Checking a change

There is no automated test suite. On hardware: every channel reaches
CONVERGED within ~25-30 s, Check Calibration (`check_calibration`) reports
|lag| 0 and |phase| < 1 deg, and `coherence_events` in the status JSON stays
0 across retunes, gain changes and element-count changes. Hardware
measurements are made by scripts that drive 8092 and read the 8091 headers.
