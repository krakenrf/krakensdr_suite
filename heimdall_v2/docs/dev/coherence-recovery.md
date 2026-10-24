# Coherence-loss detection and recovery

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

### Coherence-Loss Detection and Recovery

**Invariant**: coherence requires that the N-th sample set from every device is the same instant. The dongles are sample-locked (shared clock); calibration only removes a small sub-sample/few-sample offset. A single dropped USB packet shifts one channel by a full `NUM_SAMPLES` — far outside what lag/phase compensation can represent — and silently destroys coherence. All buffer dropping must therefore happen at the **aligned** stage (whole sets, all channels) and **never per-device**.

**Detection** (`signal_coherence_lost()` in sdr_pipeline.cpp sets the `coherence_lost` flag, bumps `coherence_event_count`):
- RTL callback: L1 overflow / sample-pool exhaustion (sdr_device.cpp)
- Sample drain: a device stalled past `STUCK_DEVICE_MAX_TIMEOUTS`
- Optional streaming monitor (`maybe_run_coherence_monitor`, default OFF — see config)

**Recovery** (`coherence_watchdog` thread → `recover_coherence()` in compensation.cpp):
- Re-engages the noise source, resets lag to MEASURING + flushes L1/L2/L2-raw, resets phase to WAITING_FOR_LAG_COMPLETION (identity vector, per-bin dropped), re-enables FFT — i.e. a full startup-equivalent recalibration (the only recovery robust to an arbitrary full-packet slip).
- `recovery_in_progress` spans the whole async recal (cleared in `complete_phase_calibration_locked`), and is surfaced in the control-port status (`coherence_events`, `recovering`).
- A frequency / gain / mixer-side / ring change during a recovery (or a KerberosSDR manual recal) restarts its phase stage (`recovery_settings_changed`: MEASURING_INITIAL_PHASE, settle gate re-armed, pre-retune sets flushed) once lag has converged - a measurement past that point was taken on the old settings and could verify on pre-retune data and latch it as CONVERGED. A discrete-scanner hop marks the calibration for a recal when the scan stops (`scanner_cal_deferred`; `--kerberos`: STALE).
- Debounce: an event that fires **during** a recovery is peeked (not consumed) and acted on once the current recovery completes; a recovery that never converges is aborted after 120 s with the noise source switched off and FFT disabled so it can't latch a no-noise-source calibration.
- Flush-only (no recal) in wideband scan or while the coherent discrete scanner is active (a recal can't converge while hopping). The same rule covers every other recal trigger during a discrete scan: a retune/gain cooldown, `handle_settings_change()` and a calibration already in flight when `start_scanner` arrives (its noise source is switched off) are deferred via `scanner_cal_deferred`, and the lag driver runs one `begin_retune_cooldown()` when the scan stops; the periodic check stands down for a scan started mid-check.
- **Calibration only progresses on the noise source**: lag acquisition (MEASURING/SERVOING) and the phase measuring stages ignore readings while `bias_tee_enabled` is false. The parked states (kerberos uncalibrated idle, the 120 s abort, a calibration stood down for a scan) idle the machines by switching FFT off, so turning the FFT on there used to acquire on the antenna signal and latch a "calibration"; a servo caught with the noise off is parked (register zeroed), and a lag reset zeroes a live correction register too. Likewise `begin_retune_cooldown()` and `handle_settings_change()` are no-ops in wideband scan (the phase stages skip scan mode, so a recal would leave the noise source on across the scan; leaving the scan recalibrates anyway), and a scan-mode retune from the web UI (`SDR_SETTINGS`) or control port (`set_frequency`) re-spreads the tuners via `setup_wideband_frequencies()` instead of tuning them all to one frequency. The retune/gain cooldown handlers (web_server.cpp, tcp_control_server.cpp) and `handle_settings_change` skip their phase-only override while `recovery_in_progress` so they don't clobber the recovery.

