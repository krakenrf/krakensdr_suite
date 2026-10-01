#pragma once

#include "../core/types.hpp"
#include "sdr_device.hpp"
#include <vector>
#include <memory>
#include <string>
#include <mutex>

// Device enumeration functions
std::string get_device_serial(int device_index);
std::vector<DeviceMapping> enumerate_devices_by_serial();
// How many of the expected serials are attached right now. Used by main() to
// pick the startup element count when neither -n nor a persisted setting says
// otherwise (the default serial list covers the full 8-channel ceiling, but a
// KrakenSDR only has 5 dongles and a KerberosSDR 4).
// Counts the expected serials attached consecutively from the first (the ones
// an N-element open needs), so a gap ends the count.
int count_expected_devices_present();

// Device initialization functions
bool init_rtlsdr_device(SDRDevice* sdr);
// Startup entry point: creates one SDRDevice object per expected serial (the
// vector is never resized afterwards - see pipeline_control.hpp) and opens
// the first active_num_elements of them.
bool init_all_rtlsdr_devices(std::vector<std::unique_ptr<SDRDevice>>& devices);
// (Re)open librtlsdr handles for the first active_num_elements devices; the
// objects must already exist. Re-enumerates USB (indices shift between opens),
// applies the RUNTIME frequency/gain, and cleans up after a partial failure.
bool open_active_devices(std::vector<std::unique_ptr<SDRDevice>>& devices);
// Close every open handle. The per-device USB reader threads must be joined
// before calling this.
void close_active_devices(std::vector<std::unique_ptr<SDRDevice>>& devices);

// Device management functions
// frequency is the user-facing RF in Hz (uint64_t - the wideband variant's
// low-side injection reaches past uint32); in downconverter mode it programs
// the LO while the tuners stay parked at the IF.
void set_bias_tee_all_devices(bool enable, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// Per-port antenna bias tees (standard KrakenSDR): bit N of mask powers the
// bias tee on channel N via GPIO N+1 of the channel-0 chip (GPIO0 is the
// noise source; cf. rtl_biast -g). Updates antenna_bias_tee_mask, which is
// persisted by the caller via settings::save() and restored at device init.
// Refused on the Wideband variant, where GPIO1-6 drive the RF path switches.
void apply_antenna_bias_tees(uint32_t mask, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// Wideband (downconverter) variant only: throw the on-board RF switches
// (GPIO1-6 on the channel-0 chip) so the tuners see either the calibration
// noise source or the currently selected antenna ring. Called automatically
// by set_bias_tee_all_devices; no-op when the variant is not active.
void wideband_set_noise_path(bool noise_on, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// Wideband (downconverter) variant retune: auto-selects the mixer side when
// the current one can't reach rf_hz, programs the LO, and throws the RF
// switches when the frequency crosses into a different antenna ring
// (0 = outer < 1 GHz, 1 = center < 2.5 GHz, 2 = inner above). Used by every
// retune path (control/web/scanner). Returns false if the LO programming
// failed or rf_hz is outside every side's reach.
bool wideband_retune_rf(uint64_t rf_hz, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// RF range the current hardware mode can tune: the tuner range normally,
// the union of every mixer side's reach on the Wideband variant (the retune
// auto-selects the side). Every retune entry point checks against this.
void rf_frequency_range(uint64_t& min_hz, uint64_t& max_hz);
bool rf_frequency_valid(uint64_t rf_hz);
// Set the R820T IF VGA to a fixed step (0-15, gain = -12 dB + 3.5 dB * index)
// on every open tuner, RTL2832 AGC loop off. Diagnostic/tuning aid (control
// port set_if_vga); the next gain change re-applies the configured step.
bool set_if_vga_all(int index, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// Rejects (logs, ignores) a frequency outside rf_frequency_range(); the gain
// part of the request still applies. Values equal to the current ones are
// no-ops. Returns true when the hardware was written (the caller then
// recalibrates) - including a partial failure, after which every tuner is put
// back on the previous settings (logged; a failed restore signals coherence
// loss). frequency 0 / gain -999 = leave unchanged.
bool update_sdr_settings(uint64_t frequency = 0, int gain = -999, const std::vector<std::unique_ptr<SDRDevice>>& devices = {});
void handle_settings_change();

// Wideband scan (tuner spread) mode functions - unavailable in the
// downconverter variant, which needs every tuner parked at the IF
bool set_wideband_mode(bool enable, const std::vector<std::unique_ptr<SDRDevice>>& devices);
bool set_tuner_frequency(int tuner_index, uint32_t frequency, const std::vector<std::unique_ptr<SDRDevice>>& devices);
void setup_wideband_frequencies(uint64_t base_frequency, const std::vector<std::unique_ptr<SDRDevice>>& devices);

// Serializes librtlsdr handle operations (see sdr_init.cpp). Innermost lock.
extern std::recursive_mutex device_io_mutex;
