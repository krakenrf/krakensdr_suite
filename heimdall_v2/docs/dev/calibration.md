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

**Why not timed "bursts" on the PLL path** (`rtlsdr_set_freq_correction`,
the historical approach — code removed July 2026): that call retunes the
tuner LO by ppm×RF (10 kHz at 100 ppm/100 MHz), which destroys the
correlation peak while active (mid-burst readings are garbage), each
set/reset pair slips a random ~2–4 samples, and open-loop timing makes the
landing point dice on a loaded machine (landing = lag −
ppm×2.4×(duration + timing jitter) − slip). The register-only servo has
none of these failure modes.

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

### Calibration Buttons (web UI, Compensation Status card)

- **Check Calibration** (`CHECK_CAL`; control port `check_calibration`): the
  periodic monitor runs its non-destructive check at the next tick - briefly
  pulses the noise source, measures the residual lag/phase on the compensated
  stream, and sets `force_recalibration` only if out of tolerance. Works with
  the periodic check disabled; not in plain `--kerberos` (it pulses the noise).
  Result in the web STATE `cal_check` (`ok` / `drifted` / `no_verdict`, worst
  lag + phase), shown under the buttons
- **Full Recalibration** (`FORCE_RECAL`): the watchdog's full lag + phase
  recalibration from scratch (confirm dialog; KerberosSDR: disconnect antennas).
  `RESET_LAG_COMPENSATION` / `reset_lag_compensation` remain as aliases

