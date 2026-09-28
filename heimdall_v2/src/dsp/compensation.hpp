#pragma once

#include "../core/types.hpp"
#include <map>
#include <optional>

// Phase and amplitude compensation functions
void apply_phase_compensation_once(const std::map<int, float>& measured_phases,
                                   const std::map<int, float>& measured_amplitudes);
bool check_phase_convergence(const std::map<int, float>& current_phases,
                             const std::map<int, float>& current_amplitudes);
std::optional<PhaseCompensatorState> get_phase_compensation_state();

// Reset the phase machine for a fresh calibration pass: enter `state`, zero the
// convergence counters, set the compensation vector to identity and (unless
// drop_per_bin_eq is false) drop the per-bin equalizer so it is re-measured.
// Gating policy (kerberos / recovery / scan checks), the noise source and FFT
// stay with the caller. Caller holds phase_compensation->state_mutex and has
// checked phase_compensation.
void reset_phase_state_locked(PhaseCompensatorState state, bool drop_per_bin_eq = true);

// Retune / gain / mixer-side / antenna-ring cooldown: noise source off, then
// reset the phase machine into WAITING_FOR_STABILITY with the cooldown timer
// restarted. The phase-driver lag thread starts the phase-only recal
// (handle_settings_change) once the stability delay elapses. `what` prefixes
// the log line. The caller applies its own gating first.
void begin_retune_cooldown(const char* what);

// Lag compensation functions
bool process_channel_lag_compensation(int channel, float lag);
void reset_lag_compensation_all_channels();

// Coherence recovery: full flush + recalibration triggered after a detected
// desync (see signal_coherence_lost). Re-engages the noise source and re-runs
// lag+phase calibration from the startup state - the only recovery robust to an
// arbitrary full-packet offset. coherence_watchdog() is the thread that watches
// the coherence_lost flag and invokes recover_coherence() with debounce.
// `manual` marks a user-requested recalibration (FORCE_RECAL): in --kerberos
// mode that is the ONLY trigger allowed to run the noise-source calibration
// (the user has confirmed the antennas are disconnected); an automatic trigger
// there flushes and drops to the uncalibrated idle state instead.
void recover_coherence(bool manual = false);
void coherence_watchdog();

// --kerberos: current calibration state for status reporting:
// "calibrating" (manual recal running), "calibrated", "stale" (calibrated but
// settings changed since), or "uncalibrated". Only meaningful when
// kerberos_mode is set.
const char* kerberos_calibration_state();

// --kerberos: park the system in the uncalibrated idle state - noise source
// off, FFT off (which idles the lag/phase machines), lag+phase reset to their
// startup states, identity compensation. Data keeps streaming; DF output is
// meaningless until a manual recalibration. Used at startup, after a
// coherence loss, and after an element-count reconfiguration.
void kerberos_enter_uncalibrated(const char* reason);

// Periodic calibration monitor: every periodic_recal_minutes (while enabled and
// the system is in steady CONVERGED coherent operation) briefly engages the
// noise source, measures the residual lag/phase, restores the prior noise/FFT
// state, and requests a full recalibration via force_recalibration if the
// calibration has drifted out of tolerance. Runs as its own thread.
void periodic_calibration_monitor(CorrelationResult& correlation_result);

// Compensation processor thread function
void channel_lag_compensation_processor(int channel, CorrelationResult& correlation_result);
