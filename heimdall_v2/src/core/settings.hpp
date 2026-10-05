#pragma once

// Lightweight persistence for runtime-toggleable settings.
//
// Stores a small key=value text file (heimdall_settings.conf) in the working
// directory so user choices survive restarts. Currently persists:
//   - per_bin_eq             : optional per-bin phase calibration toggle (default OFF)
//   - periodic_recal_enabled : periodic calibration check on/off (default ON)
//   - periodic_recal_minutes : periodic calibration check period in minutes (default 5)
//   - antenna_bias_tee_mask  : per-port antenna bias tee bitmask, bit N = channel N
//                              (default 0 = all OFF; applied once devices are open)
//   - num_elements           : active element/device count chosen via the web UI
//                              (0/absent = unset; an explicit -n flag overrides it)
//   - center_freq / gain     : the last user tuning (Hz / tenths of dB, -1 = auto),
//                              restored before the dongles open - a crash restart
//                              used to come back at the compiled-in 100 MHz
//   - operating_mode         : coherent / wideband / independent, re-entered once
//                              the devices are open (restore_operating_mode)
//   - tuner_freq<N> / tuner_gain<N> : independent mode's per-tuner tuning
//
// load() is called once at startup (before calibration begins) and applies the
// saved values to the global state; save() is called whenever a persisted
// setting changes. Both are thread-safe and degrade gracefully (a missing or
// unreadable file just leaves the compiled-in defaults in place).
#include <atomic>
#include <cstdint>

namespace settings {

// Settings file path (relative to the working directory, like index.html).
constexpr const char* FILE_PATH = "heimdall_settings.conf";

// The user's chosen element count: loaded from the settings file (0 = never
// chosen) and updated when the count is changed at runtime (web UI / control
// port). main() resolves the startup count as: -n flag > --kerberos (4) >
// this > devices detected. save() writes THIS, not active_num_elements, so a
// count given by -n / --kerberos only lasts for that run.
extern std::atomic<int> persisted_num_elements;

// The last user tuning (frequency Hz, gain tenths of dB or -1 = auto; 0 /
// -999 = none). note_tuning() records a successful retune; the tuning is
// written once it has been stable for TUNING_SAVE_DELAY_MS (save_tuning_if_due,
// called periodically) - a continuous scan retunes every few seconds and must
// not rewrite the file each time.
extern std::atomic<uint64_t> persisted_frequency;
extern std::atomic<int> persisted_gain;
constexpr long long TUNING_SAVE_DELAY_MS = 10000;
void note_tuning(uint64_t frequency_hz, int gain);
void save_tuning_if_due();

// The operating mode (0 coherent, 1 wideband, 2 independent) and independent
// mode's per-tuner tuning (Hz / tenths of dB; 0 / -999 = none). note_mode()
// takes them from the live state and saves with the same debounce as
// note_tuning; load() only fills these, main() re-enters the mode.
extern std::atomic<int> persisted_mode;
extern std::atomic<uint32_t> persisted_tuner_freq[8];
extern std::atomic<int> persisted_tuner_gain[8];
void note_mode();

// Read the settings file (if present) and apply known keys to the globals.
void load();

// Write all persisted settings to disk (overwrites the file).
void save();

}  // namespace settings
