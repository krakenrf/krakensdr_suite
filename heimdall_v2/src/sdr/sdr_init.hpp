#pragma once

#include "../core/types.hpp"
#include "sdr_device.hpp"
#include <vector>
#include <memory>
#include <string>
#include <mutex>

// Device enumeration functions
// rc (optional): librtlsdr's result, 0 = read; a libusb error code otherwise
// (Windows: -12 = no WinUSB driver, -3 = opened by another program).
std::string get_device_serial(int device_index, int* rc = nullptr);
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
#ifdef _WIN32
// Windows only, called by a device's own USB reader thread once its stream
// died (see pipeline_control.cpp): close the handle, find the dongle again by
// serial, open + initialize it and put its tuning, noise source and (channel
// 0) GPIOs back as the rest of the array has them. false while it can't be
// opened (unplugged - dev is then nullptr), or once a pipeline stop has begun
// (running cleared - the handle is left to the stop). Takes device_io_mutex.
bool reopen_device_after_stream_loss(SDRDevice* sdr);
#endif

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
// What update_sdr_settings did with a request, for a caller that answers
// someone (the control port): its return value only says whether the hardware
// was written. Filled on every return; frequency / gain are the settings in
// place afterwards (what the 8091 header and the status report).
struct SdrUpdateResult {
    bool frequency_out_of_range = false;  // outside rf_frequency_range(): frequency unchanged
    bool lo_retune_failed = false;        // Wideband variant: the LO retune failed, frequency unchanged
    // Coherent / wideband scan: the change failed on failed_channels and every
    // tuner was put back on the previous settings (rolled_back) - or that
    // failed too on restore_failed_channels (restore_failed: tuners
    // inconsistent, a coherence recovery was forced). Independent mode
    // records per tuner instead: failed_channels kept their previous
    // settings, the others took the new ones (no rollback).
    bool rolled_back = false;
    bool restore_failed = false;
    std::string failed_channels;          // channel indexes, "0, 3"
    std::string restore_failed_channels;
    uint64_t frequency = 0;               // Hz
    int gain = -999;                      // tenths of dB, -1 = AGC
    bool ok() const { return !frequency_out_of_range && !lo_retune_failed && failed_channels.empty(); }
};
// Rejects (logs, ignores) a frequency outside rf_frequency_range(); the gain
// part of the request still applies. Values equal to the current ones are
// no-ops. Returns true when the hardware was written (the caller then
// recalibrates) - including a partial failure, after which every tuner is put
// back on the previous settings (logged; a failed restore signals coherence
// loss). frequency 0 / gain -999 = leave unchanged. result (optional): what
// was applied, see SdrUpdateResult.
bool update_sdr_settings(uint64_t frequency = 0, int gain = -999, const std::vector<std::unique_ptr<SDRDevice>>& devices = {},
                         SdrUpdateResult* result = nullptr);
// The end of a drag: update_sdr_settings (coherent) and
// setup_wideband_frequencies (wideband scan) retunes that follow one another
// within 400 ms (or before this finished the previous one, up to 1 s) take
// the fork's fast path (rtlsdr_set_center_freq_fast: the
// R820T's PLL autotune clock stays at 128 kHz); this puts those tuners back
// in the state a full tune leaves (rtlsdr_finish_tune: lock read, 8 kHz). A
// tuner found unlocked is retuned on the full path (twice), else coherence is
// lost. No-op when nothing is pending. only_if_quiet: only once 400 ms have
// passed since the last retune (the coherence watchdog's call; the noise
// source switch-on calls it unconditionally). Takes device_io_mutex.
void finish_fast_retunes(const std::vector<std::unique_ptr<SDRDevice>>& devices, const char* why, bool only_if_quiet);
void handle_settings_change();

// Operating mode (COHERENT / WIDEBAND_SCAN / INDEPENDENT, see types.hpp).
// Leaving COHERENT parks the calibration (noise off, FFT off); returning to it
// retunes every tuner to current_frequency on the common gain and runs the
// phase recalibration. WIDEBAND_SCAN: the caller then spreads the tuners
// (setup_wideband_frequencies). Non-coherent modes are unavailable in the
// downconverter variant, which needs every tuner parked at the IF. Returns
// false if nothing changed / refused.
bool set_operating_mode(OperatingMode target, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// enable -> WIDEBAND_SCAN; !enable -> COHERENT, only when in WIDEBAND_SCAN
bool set_wideband_mode(bool enable, const std::vector<std::unique_ptr<SDRDevice>>& devices);
// Independent mode: one tuner's frequency (Hz, 0 = keep) and/or gain (tenths
// of dB, -1 = AGC, -999 = keep). false + *err on failure.
bool set_independent_tuner(int tuner_index, uint64_t frequency, int gain,
                           const std::vector<std::unique_ptr<SDRDevice>>& devices, std::string* err);
// Startup: re-enter the saved operating mode (settings::persisted_mode) and
// independent mode's per-tuner tuning, once the devices are open
void restore_operating_mode(const std::vector<std::unique_ptr<SDRDevice>>& devices);
void setup_wideband_frequencies(uint64_t base_frequency, const std::vector<std::unique_ptr<SDRDevice>>& devices);

// Serializes librtlsdr handle operations (see sdr_init.cpp). Innermost lock.
extern std::recursive_mutex device_io_mutex;
