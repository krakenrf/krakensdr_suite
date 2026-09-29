#include "sdr_init.hpp"
#include "downconverter.hpp"
#include "kerberos_gpio.hpp"
#include "../core/config.hpp"
#include "../dsp/compensation.hpp"
#include "../core/logging.hpp"
#include "../core/forward_comp.hpp"
#include <rtl-sdr.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <vector>
#include <array>
#include <iomanip>
#include <optional>

// Serializes every librtlsdr handle operation outside the USB reader threads
// (tuning, gain, bias tee / noise source, GPIO switch banks) against each
// other and against the element-count reconfiguration's close/reopen. Without
// it a noise-source switch from the watchdog or periodic check could run on a
// handle the reconfiguration just closed (use-after-free), and two threads
// doing read-modify-write on the channel-0 GPIO register could lose each
// other's bits. Recursive: these helpers call one another (device open
// re-applies the antenna bias tees, a retune can throw the RF switches).
// Lock order: device_io_mutex is INNERMOST - settings_mutex and the phase
// machine's state_mutex may be held when taking it (noise-source switches run
// under state_mutex), but never take either while holding it.
std::recursive_mutex device_io_mutex;

// Forward declarations for functions defined elsewhere
extern void clear_l2_buffer();
extern std::atomic<size_t> l2_raw_cap;  // C5: runtime L2-raw depth cap (defined in sdr_pipeline.cpp)

std::string get_device_serial(int device_index) {
    char manufacturer[256], product[256], serial[256];
    if (rtlsdr_get_device_usb_strings(device_index, manufacturer, product, serial) == 0) {
        return std::string(serial);
    }
    return "";
}

int count_expected_devices_present() {
    const int device_count = rtlsdr_get_device_count();
    int found = 0;
    for (int i = 0; i < device_count; i++) {
        const std::string serial = get_device_serial(i);
        if (!serial.empty() &&
            std::find(expected_serials.begin(), expected_serials.end(), serial) !=
                expected_serials.end()) {
            found++;
        }
    }
    return found;
}

std::vector<DeviceMapping> enumerate_devices_by_serial() {
    std::cout << "Enumerating RTL-SDR devices by serial number..." << std::endl;
    
    std::vector<DeviceMapping> mappings;
    for (size_t i = 0; i < expected_serials.size(); i++) {
        mappings.emplace_back(expected_serials[i], static_cast<int>(i));
    }
    
    int device_count = rtlsdr_get_device_count();
    std::cout << "Found " << device_count << " RTL-SDR devices" << std::endl;
    
    if (device_count == 0) {
        std::cerr << "No RTL-SDR devices found!" << std::endl;
        return mappings;
    }
    
    // Enumerate all devices and match by serial
    for (int dev_idx = 0; dev_idx < device_count; dev_idx++) {
        std::string device_serial = get_device_serial(dev_idx);
        std::string device_name = rtlsdr_get_device_name(dev_idx);
        
        std::cout << "Device " << dev_idx << ": " << device_name;
        if (!device_serial.empty()) {
            std::cout << " (Serial: " << device_serial << ")";
        } else {
            std::cout << " (No serial)";
        }
        
        // Find matching expected serial
        auto mapping_it = std::find_if(mappings.begin(), mappings.end(),
            [&device_serial](const DeviceMapping& mapping) {
                return mapping.serial == device_serial;
            });
        
        if (mapping_it != mappings.end()) {
            mapping_it->physical_device_id = dev_idx;
            mapping_it->found = true;
            std::cout << " -> Channel " << mapping_it->channel_index;
        } else if (!device_serial.empty()) {
            std::cout << " -> NOT MAPPED (unexpected serial)";
        }
        std::cout << std::endl;
    }
    
    // Check for missing devices
    std::cout << "\nDevice mapping summary:" << std::endl;
    bool all_found = true;
    for (const auto& mapping : mappings) {
        std::cout << "Channel " << mapping.channel_index << " (Serial " << mapping.serial << "): ";
        if (mapping.found) {
            std::cout << "✓ Found as device " << mapping.physical_device_id << std::endl;
        } else {
            std::cout << "✗ NOT FOUND" << std::endl;
            all_found = false;
        }
    }
    
    if (!all_found) {
        std::cerr << "\nERROR: Not all expected devices found!" << std::endl;
        std::cerr << "Expected serials: ";
        for (size_t i = 0; i < expected_serials.size(); i++) {
            if (i > 0) std::cerr << ", ";
            std::cerr << expected_serials[i];
        }
        std::cerr << std::endl;
    }
    
    return mappings;
}

// Fix the R820T IF VGA (see R820T_IF_VGA_* in config.h). if_mode 10000 + index
// writes that VGA step and switches the RTL2832's AGC loop off
// (rtlsdr_vga_control in the fork); later gain calls keep it. Call after every
// gain-mode / gain change - an auto-gain switch would otherwise keep the manual
// index. Non-R820T tuners return -1 (no IF VGA to fix).
static int set_fixed_if_vga(rtlsdr_dev_t* dev, bool auto_gain) {
    return rtlsdr_set_tuner_if_mode(dev, 10000 + (auto_gain ? R820T_IF_VGA_AUTO_IDX
                                                            : R820T_IF_VGA_MANUAL_IDX));
}

bool set_if_vga_all(int index, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    if (index < 0 || index > 15) return false;
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    bool ok = true;
    for (const auto& d : devices) {
        if (d && d->dev && rtlsdr_set_tuner_if_mode(d->dev, 10000 + index) < 0) ok = false;
    }
    std::cout << "IF VGA set to index " << index << " (" << (-12.0 + 3.5 * index)
              << " dB) on all open tuners" << (ok ? "" : " - some FAILED") << std::endl;
    return ok;
}

bool init_rtlsdr_device(SDRDevice* sdr) {
    std::cout << "Initializing Channel " << sdr->index << " using RTL-SDR device " << sdr->device_id 
         << " (Serial: " << sdr->serial_number << ")" << std::endl;
    
    if (rtlsdr_open(&sdr->dev, sdr->device_id) < 0) {
        std::cerr << "Failed to open RTL-SDR device " << sdr->device_id << " for channel " << sdr->index << std::endl;
        return false;
    }
    
    // Verify serial number matches what we expect
    std::string actual_serial = get_device_serial(sdr->device_id);
    if (actual_serial != sdr->serial_number) {
        std::cerr << "WARNING: Serial mismatch for device " << sdr->device_id 
             << " - expected " << sdr->serial_number << ", got " << actual_serial << std::endl;
    }
    
    // Return codes: a dongle left on the wrong sample rate or frequency breaks
    // coherence with no error anywhere else, so those (and the buffer reset
    // the async stream starts from) fail the init - the caller then closes
    // every handle it opened. The rest only warn: a gain, dithering or
    // bias-tee miss is visible, and calibration absorbs a gain mismatch.
    bool ok = true;
    auto check = [&](int rc, const char* what, bool fatal) {
        if (rc >= 0) return;
        std::cerr << "Channel " << sdr->index << " (Serial " << sdr->serial_number << "): "
                  << what << " failed (" << rc << ")" << (fatal ? "" : " - continuing") << std::endl;
        if (fatal) ok = false;
    };

    check(rtlsdr_set_dithering(sdr->dev, 0), "disabling dithering", false);
    check(rtlsdr_set_sample_rate(sdr->dev, SAMPLE_RATE), "setting the sample rate", true);
    // Wideband variant: every tuner is parked at the IF for good - the RF is
    // reached by programming the downconverter LO (current_frequency stays the
    // user-facing RF). Normal variant: tuners follow the RF directly.
    // Uses the RUNTIME frequency/gain (not the compile-time defaults): at
    // startup they are equal, and a runtime element-count reconfiguration
    // reopens devices at whatever the user had tuned.
    check(rtlsdr_set_center_freq(sdr->dev, downconverter.enabled.load()
                                               ? static_cast<uint32_t>(WB_VARIANT_IF_HZ)
                                               : static_cast<uint32_t>(current_frequency.load())),
          "setting the center frequency", true);
    const int gain_now = current_gain.load();
    check(rtlsdr_set_tuner_gain_mode(sdr->dev, gain_now < 0 ? 0 : 1), "setting the gain mode", false);
    if (gain_now >= 0) {
        check(rtlsdr_set_tuner_gain(sdr->dev, gain_now), "setting the gain", false);
    }
    check(set_fixed_if_vga(sdr->dev, gain_now < 0), "fixing the IF VGA gain", false);
    // -2 = already at this value (a freshly opened handle starts at 0)
    if (const int rc = rtlsdr_set_freq_correction(sdr->dev, 0); rc != -2)
        check(rc, "clearing the frequency correction", false);

#if ENABLE_BIAS_TEE
    // --kerberos: the noise source couples straight into the antenna path, so
    // it must stay off until an explicit manual calibration.
    if (!kerberos_manual_cal_only()) check(rtlsdr_set_bias_tee(sdr->dev, 1), "enabling the noise source", false);
#endif

#if USB_RESET_ON_INIT
    check(rtlsdr_reset_buffer(sdr->dev), "resetting the USB buffer", true);
#endif

    if (!ok) {
        std::cerr << "Failed to initialize Channel " << sdr->index << " (Serial: "
                  << sdr->serial_number << ")" << std::endl;
        return false;
    }

    sdr->init_success = true;
    std::cout << "Successfully initialized Channel " << sdr->index << " (Serial: " << sdr->serial_number << ")" << std::endl;
    return true;
}

bool init_all_rtlsdr_devices(std::vector<std::unique_ptr<SDRDevice>>& devices) {
    // One SDRDevice object per expected serial, created ONCE and kept for the
    // life of the process: the vector is iterated lock-free all over the
    // codebase (status dashboard, TCP status builders, correlation), so it is
    // never resized after this. Channels beyond the active element count keep
    // dev == nullptr; the runtime reconfiguration only closes/reopens handles
    // inside these objects.
    if (devices.empty()) {
        devices.reserve(expected_serials.size());
        for (size_t i = 0; i < expected_serials.size(); i++) {
            devices.emplace_back(std::make_unique<SDRDevice>(
                -1, static_cast<int>(i), expected_serials[i]));
        }
    }
    return open_active_devices(devices);
}

bool open_active_devices(std::vector<std::unique_ptr<SDRDevice>>& devices) {
    // Defensive clamp: callers validate against the serial list, but an OOB
    // read of expected_serials here would be memory corruption, not an error.
    const int num_to_open = std::min({active_num_elements.load(),
                                      static_cast<int>(expected_serials.size()),
                                      static_cast<int>(devices.size())});

    std::cout << "Initializing " << num_to_open << " RTL-SDR devices using serial number mapping..." << std::endl;
    std::cout << "Expected serials for channels 0-" << (num_to_open - 1) << ": ";
    for (int i = 0; i < num_to_open; i++) {
        if (i > 0) std::cout << ", ";
        std::cout << expected_serials[i];
    }
    std::cout << std::endl << std::endl;

    // Get device mappings based on serial numbers (USB indices shift between
    // opens, so this runs on every open, not just at startup)
    auto mappings = enumerate_devices_by_serial();

    // Check if we have all required devices (only check the ones we need)
    bool all_devices_found = true;
    for (int i = 0; i < num_to_open; i++) {
        if (!mappings[i].found) {
            all_devices_found = false;
            break;
        }
    }

    if (!all_devices_found) {
        std::cerr << "ERROR: Not all required devices found. Cannot proceed." << std::endl;
        return false;
    }

    for (int i = 0; i < num_to_open; i++) {
        devices[i]->device_id = mappings[i].physical_device_id;
        std::cout << "Channel " << i << " -> Physical device " << mappings[i].physical_device_id
             << " (Serial: " << mappings[i].serial << ")" << std::endl;
    }

    // Initialize devices in parallel
    std::vector<std::thread> init_threads;
    // char, not bool: vector<bool> packs results into shared words, so the
    // init threads' concurrent writes would race and could lose a result.
    std::vector<char> init_results(num_to_open, 0);

    for (int i = 0; i < num_to_open; i++) {
        init_threads.emplace_back([i, &init_results, &devices]() {
            init_results[i] = init_rtlsdr_device(devices[i].get());
        });
    }

    for (auto& thread : init_threads) thread.join();

    bool all_success = std::all_of(init_results.begin(), init_results.end(), [](bool result) { return result; });

#if ENABLE_BIAS_TEE
    // Startup calibration runs with the noise source on (GPIO0 was set per
    // device in init_rtlsdr_device); on the Wideband variant the RF path
    // switches must also route it to the tuners, and on --kerberos_sw the
    // CKOVAL switches must disconnect the antennas. In manual --kerberos
    // mode the noise stayed off (see init_rtlsdr_device) so neither applies.
    if (all_success && !kerberos_manual_cal_only()) {
        wideband_set_noise_path(true, devices);
        kerberos_gpio_set_noise_path(true);
    }
#endif

    // Restore the persisted per-port antenna bias tee state (settings::load()
    // ran before device init and filled antenna_bias_tee_mask; the GPIOs can
    // only be driven once the channel-0 device is open).
    if (all_success && antenna_bias_tee_mask.load(std::memory_order_acquire) != 0)
        apply_antenna_bias_tees(antenna_bias_tee_mask.load(std::memory_order_acquire), devices);

    std::cout << "\nDevice initialization summary:" << std::endl;
    for (int i = 0; i < num_to_open; i++) {
        std::cout << (init_results[i] ? "✓" : "✗") << " Channel " << i
             << " (Serial: " << devices[i]->serial_number
             << ", Physical RTL-SDR #" << devices[i]->device_id << ")" << std::endl;
    }

    // A partial open must not leave stray handles behind (the caller may retry
    // with a different count or roll back).
    if (!all_success) close_active_devices(devices);

    return all_success;
}

void close_active_devices(std::vector<std::unique_ptr<SDRDevice>>& devices) {
    // Callers must have joined the USB reader threads first (the handle is
    // freed here; a concurrent rtlsdr_read_async on it would be use-after-free).
    for (auto& device : devices) {
        if (device && device->dev) {
            rtlsdr_close(device->dev);
            device->dev = nullptr;
            device->init_success = false;
            std::cout << "Closed Channel " << device->index
                 << " (Serial: " << device->serial_number << ")" << std::endl;
        }
    }
}

void wideband_set_noise_path(bool noise_on, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    if (!downconverter.enabled.load()) return;

    // Unlike the standard KrakenSDR, whose directional-coupler network keeps
    // the noise source permanently coupled into every channel (GPIO0 merely
    // powers it), the Wideband board sits RF switches between the antenna
    // rings / noise source and the mixers, driven by GPIO1-6 of the channel-0
    // RTL2832U (v1's ctr_channel_serial_no = 1000). Powering the noise source
    // without also switching the RF path leaves the tuners looking at the
    // antennas - the channels then share no common signal and lag/phase
    // calibration can never converge. Patterns ported verbatim from the v1
    // heimdall_daq_fw wideband branch (rtl_daq.c): GPIO1/2 pick noise vs the
    // selected ring, GPIO3-6 are per-ring switch-tree constants.
    rtlsdr_dev_t* ctrl = nullptr;
    for (const auto& device : devices) {
        if (device && device->index == 0 && device->dev) {
            ctrl = device->dev;
            break;
        }
    }
    if (!ctrl) {
        std::cerr << "Wideband RF path: channel 0 device unavailable - cannot drive switch GPIOs" << std::endl;
        return;
    }

    static constexpr int NOISE_PATTERN[3][6] = {   // [array][gpio1..gpio6]
        {1, 0, 0, 0, 1, 0},   // outer ring
        {1, 0, 1, 1, 1, 1},   // center ring
        {1, 0, 0, 1, 0, 0},   // inner ring
    };
    static constexpr int ANTENNA_PATTERN[3][6] = {
        {0, 0, 0, 0, 1, 0},   // outer ring
        {1, 1, 1, 1, 1, 1},   // center ring
        {0, 1, 0, 1, 0, 0},   // inner ring
    };
    static constexpr const char* ARRAY_NAMES[3] = {"outer", "center", "inner"};

    const int array = std::clamp(downconverter.array_select.load(), 0, 2);
    const int* pattern = noise_on ? NOISE_PATTERN[array] : ANTENNA_PATTERN[array];

    bool ok = true;
    for (int gpio = 1; gpio <= 6; gpio++) {
        if (rtlsdr_set_bias_tee_gpio(ctrl, gpio, pattern[gpio - 1]) != 0) ok = false;
    }
    std::cout << "Wideband RF path: switched to " << (noise_on ? "noise source" : "antennas")
              << " (" << ARRAY_NAMES[array] << " ring)" << (ok ? "" : " - some GPIO writes FAILED")
              << std::endl;
}

void set_bias_tee_all_devices(bool enable, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
#if ENABLE_BIAS_TEE
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    std::cout << "Bias Tee: " << (enable ? "Enabling" : "Disabling") << " on all devices..." << std::endl;
    for (const auto& device : devices) {
        if (device && device->dev) {
            bool success = (rtlsdr_set_bias_tee(device->dev, enable ? 1 : 0) == 0);
            std::cout << "Bias Tee: Ch" << device->index << " (Serial " << device->serial_number << ") "
                 << (success ? (enable ? "ON" : "OFF") : "FAILED") << std::endl;
        }
    }
    // Wideband variant: GPIO0 only powers the noise source; the RF switches
    // must also be thrown or the tuners keep looking at the antennas.
    wideband_set_noise_path(enable, devices);
    // --kerberos_sw: the CKOVAL antenna switches (Pi GPIOs) must disconnect
    // the antennas while the noise source is on - the KerberosSDR couples the
    // noise in via a directional coupler, not an RF switch. No-op unless the
    // switch GPIOs were claimed at startup.
    kerberos_gpio_set_noise_path(enable);
    bias_tee_enabled = enable;
    // Anchor the phase-cal settle gate to the moment the noise source comes ON. Only on
    // ENABLE: a retune turns it on just before measuring, so the gate then waits out the
    // in-flight pre-noise USB samples and the tuner AGC ramp. Startup turns the bias tee on per-device in
    // init_rtlsdr_device() (not via this function), so noise_on_ns stays 0 there and the
    // startup calibration is never gated (the noise has been on through lag cal already).
    if (enable && phase_compensation) {
        phase_compensation->noise_on_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
    }
#else
    std::cout << "Bias Tee: Control disabled in config" << std::endl;
    bias_tee_enabled = false;
#endif
}

void apply_antenna_bias_tees(uint32_t mask, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    // One GPIO per OPEN channel (GPIO ch+1 on the channel-0 chip) - the RTL2832
    // only has GPIO0-7, so never sweep the compile-time ceiling here.
    // devices holds one slot per EXPECTED serial (8 by default); only the
    // first active_num_elements are open.
    const int nch = std::min(active_num_elements.load(), static_cast<int>(devices.size()));
    mask &= (nch >= 32) ? ~0u : ((1u << nch) - 1);

    if (downconverter.enabled.load()) {
        // On the Wideband board GPIO1-6 of the channel-0 chip drive the RF
        // path switches (see wideband_set_noise_path); there are no per-port
        // antenna bias tees to control.
        if (mask) std::cerr << "Bias tees: not available on the Wideband variant "
                               "(GPIO1-6 drive the RF path switches)" << std::endl;
        antenna_bias_tee_mask.store(0, std::memory_order_release);
        return;
    }

    rtlsdr_dev_t* ctrl = nullptr;
    for (const auto& device : devices) {
        if (device && device->index == 0 && device->dev) {
            ctrl = device->dev;
            break;
        }
    }
    if (!ctrl) {
        std::cerr << "Bias tees: channel 0 device unavailable - cannot drive GPIOs" << std::endl;
        return;
    }

    bool ok = true;
    for (int ch = 0; ch < nch && ch + 1 <= 7; ch++) {
        if (rtlsdr_set_bias_tee_gpio(ctrl, ch + 1, (mask >> ch) & 1) != 0) ok = false;
    }
    antenna_bias_tee_mask.store(mask, std::memory_order_release);

    std::cout << "Bias tees:";
    for (int ch = 0; ch < nch; ch++)
        std::cout << " Ch" << ch << "=" << (((mask >> ch) & 1) ? "ON" : "off");
    std::cout << std::endl;
    if (!ok) std::cerr << "Bias tees: some GPIO writes FAILED" << std::endl;
}

bool wideband_retune_rf(uint64_t rf_hz, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    // Mixer side follows the frequency: keep the current side (it may be a
    // user preference, e.g. low for image dodging) as long as it can reach
    // the RF, otherwise auto-select.
    // The side is committed only once the LO write succeeds, so a failed
    // retune leaves side, LO and spectral-inversion correction consistent.
    if (!downconverter_rf_valid(rf_hz, downconverter.side.load())) {
        const MixerSide side = downconverter_auto_side(rf_hz);
        if (!downconverter_set_side(side, rf_hz)) return false;
        std::cout << "Wideband: mixer side auto-selected " << mixer_side_name(side)
                  << " for RF " << rf_hz / 1e6 << " MHz" << std::endl;
    } else if (!downconverter_apply_rf(rf_hz)) {
        return false;
    }

    // Antenna ring follows the frequency (outer < 1 GHz, center < 2.5 GHz,
    // inner above): throw the RF switches when crossing a boundary. The
    // caller's normal retune cooldown -> phase-recal covers the path change.
    const int ring = wb_ring_for_rf(rf_hz);
    if (ring != downconverter.array_select.load()) {
        downconverter.array_select.store(ring);
        wideband_set_noise_path(bias_tee_enabled.load(), devices);
        static const char* RING_NAMES[3] = {"outer", "center", "inner"};
        std::cout << "Wideband: antenna ring auto-selected " << RING_NAMES[ring]
                  << " (" << ring << ") for RF " << rf_hz / 1e6 << " MHz" << std::endl;
    }
    return true;
}

void rf_frequency_range(uint64_t& min_hz, uint64_t& max_hz) {
    if (downconverter.enabled.load()) {
        downconverter_rf_union_range(min_hz, max_hz);
    } else {
        min_hz = RTL_TUNER_MIN_HZ;
        max_hz = RTL_TUNER_MAX_HZ;
    }
}

bool rf_frequency_valid(uint64_t rf_hz) {
    uint64_t min_hz, max_hz;
    rf_frequency_range(min_hz, max_hz);
    return rf_hz >= min_hz && rf_hz <= max_hz;
}

bool update_sdr_settings(uint64_t frequency, int gain, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    if (frequency > 0 && !rf_frequency_valid(frequency)) {
        uint64_t min_hz, max_hz;
        rf_frequency_range(min_hz, max_hz);
        std::cerr << "SDR: rejecting frequency " << frequency / 1e6 << " MHz (tunable range "
                  << min_hz / 1e6 << " - " << max_hz / 1e6 << " MHz)" << std::endl;
        frequency = 0;
        if (gain == -999) return false;
    }

    std::lock_guard<std::mutex> lock(settings_mutex);
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);

    bool changed = false;
    uint64_t prev_frequency = current_frequency.load();
    if (frequency > 0 && (frequency > prev_frequency ? frequency - prev_frequency
                                                     : prev_frequency - frequency) > 1000) changed = true;
    if (gain != -999 && gain != current_gain.load()) changed = true;

    std::cout << "SDR: Updating settings - ";
    if (frequency > 0) std::cout << "Freq: " << frequency/1e6 << " MHz ";
    if (gain == -1) std::cout << "Gain: AUTO mode";
    else if (gain >= 0) std::cout << "Gain: " << gain/10.0 << " dB";
    if (changed) std::cout << " (CHANGED - will do phase-only recalibration)";
    std::cout << std::endl;

    // Wideband variant: a frequency change is an LO reprogram; the tuners stay
    // parked at the IF (no per-device retune, no PLL resettle). Gain changes
    // still go to the tuners via the parallel loop below.
    const bool wb_variant = downconverter.enabled.load();
    bool lo_ok = true;
    if (wb_variant && frequency > 0) {
        lo_ok = wideband_retune_rf(frequency, devices);
        if (!lo_ok) changed = false;  // nothing retuned - don't trigger a recal
    }

    // Update all devices in parallel - each RTL-SDR has independent PLL settling time
    // Parallel execution reduces total time from N*latency to max(latency)
    std::vector<std::thread> update_threads;
    std::vector<char> results(devices.size(), 1);  // char: see init_results

    for (size_t i = 0; i < devices.size(); i++) {
        if (devices[i] && devices[i]->dev) {
            update_threads.emplace_back([i, frequency, gain, wb_variant, &devices, &results]() {
                bool success = true;

                if (!wb_variant && frequency > 0 &&
                    rtlsdr_set_center_freq(devices[i]->dev, static_cast<uint32_t>(frequency)) < 0) {
                    success = false;
                }

                if (gain == -1) {
                    if (rtlsdr_set_tuner_gain_mode(devices[i]->dev, 0) < 0) success = false;
                } else if (gain >= 0) {
                    if (rtlsdr_set_tuner_gain_mode(devices[i]->dev, 1) < 0 ||
                        rtlsdr_set_tuner_gain(devices[i]->dev, gain) < 0) success = false;
                }
                if (gain != -999 && set_fixed_if_vga(devices[i]->dev, gain == -1) < 0) success = false;

                results[i] = success;
            });
        }
    }

    // Wait for all updates to complete
    for (auto& thread : update_threads) {
        thread.join();
    }

    bool all_success = std::all_of(results.begin(), results.end(), [](bool r) { return r; }) && lo_ok;

    if (all_success) {
        if (frequency > 0) current_frequency = frequency;
        if (gain >= -1) current_gain = gain;
    }

    // Re-interpolate the S2P forward correction at the new center frequency so
    // the differential insertion phase tracks retunes (web UI, control port,
    // scanner hops). Cheap (a handful of interpolations + atomic stores) and
    // only when forward compensation is actually enabled.
    if (all_success && frequency > 0 &&
        forward_comp.enabled.load(std::memory_order_relaxed)) {
        fwdcomp::recompute(static_cast<double>(frequency));
    }

    return changed;
}

void handle_settings_change() {
    // Wideband scan: no phase calibration exists to redo (the phase stages
    // skip scan mode), so switching the noise source on here would leave it
    // injected across the whole scan. Leaving the scan recalibrates anyway.
    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) return;

    // --kerberos: a settings change may not trigger the noise-source phase
    // recal (antennas are connected). Keep the current compensation applied -
    // approximately valid for small changes - and mark it STALE so both UIs
    // warn the user to recalibrate manually.
    if (kerberos_manual_cal_only()) {
        if (get_phase_compensation_state() == PhaseCompensatorState::CONVERGED) {
            kerberos_cal_stale.store(true, std::memory_order_release);
            std::cerr << "KerberosSDR: settings changed - calibration is STALE. "
                         "Disconnect antennas and press Recalibrate." << std::endl;
        }
        return;
    }

    // If a full coherence recovery is in flight, skip the phase-only recal. It
    // assumes lag is still CONVERGED and jumps straight to MEASURING_INITIAL_PHASE
    // - but recovery has reset lag to MEASURING, so this would measure/apply phase
    // on un-lag-corrected data and could latch a bad calibration as "recovered".
    // The in-flight full recovery already recalibrates lag+phase at the current
    // frequency, so the settings change is covered.
    if (recovery_in_progress.load(std::memory_order_acquire)) {
        std::cout << "System: Settings change during coherence recovery - deferring to the full recal" << std::endl;
        return;
    }

    std::cout << "System: Settings changed - doing phase-only recalibration (skipping lag calibration)" << std::endl;

    // C5: phase recal starting - tighten the L2-raw cap (restored on CONVERGED).
    l2_raw_cap.store(L2_RAW_CAL_MAX, std::memory_order_relaxed);

    // Enable bias tee for phase calibration
    set_bias_tee_all_devices(true, devices);
    
    if (phase_compensation) {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        // Skip directly to phase measurement - lag calibration remains valid for settings
        // changes. The settle gate is anchored by set_bias_tee_all_devices(true) above.
        // Also drops the per-bin equalizer (must be re-measured for the new settings).
        reset_phase_state_locked(PhaseCompensatorState::MEASURING_INITIAL_PHASE);
    }

    // Keep device lag compensation states as CONVERGED - don't reset them for settings changes
    for (const auto& device : devices) {
        if (device) {
            std::lock_guard<std::mutex> lock(device->compensation_mutex);
            // Only reset initial calibration flag to allow bias tee disable after phase convergence
            device->compensation.initial_calibration_complete = false;
            // Keep lag state as CONVERGED - don't reset to MEASURING for settings changes
            if (device->compensation.state == LagCompensatorState::CONVERGED) {
                std::cout << "Keeping Channel " << device->index << " lag state as CONVERGED (settings change)" << std::endl;
            }
        }
    }
    
    clear_l2_buffer();
    
    {
        std::lock_guard<std::mutex> lock(fft_control.control_mutex);
        fft_control.fft_enabled = true;
        fft_control.auto_disabled = false;
        fft_control.user_override = false;
    }
}

// Wideband mode functions
bool set_wideband_mode(bool enable, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    // The tuner-spread scan needs to retune individual tuners, but the
    // Wideband (downconverter) variant requires every tuner parked at the IF -
    // the two are mutually exclusive.
    if (enable && downconverter.enabled.load()) {
        std::cerr << "Wideband scan: unavailable in KrakenSDR Wideband (downconverter) mode"
                  << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lock(settings_mutex);

    if (enable == wideband_config.enabled.load()) {
        // Already in requested mode
        return false;
    }

    std::cout << "System: " << (enable ? "Enabling" : "Disabling") << " wideband scan mode" << std::endl;

    if (enable) {
        // Entering wideband mode
        operating_mode = OperatingMode::WIDEBAND_SCAN;
        wideband_config.enabled = true;

        // Reset phase calibration state to prevent eigenvalue calculations during wideband
        // This prevents race condition where calibration was in progress when wideband enabled.
        // Deliberately NOT reset_phase_state_locked(): this parks the machine but keeps the
        // applied compensation vector (and counters it doesn't list) as they were.
        if (phase_compensation) {
            std::lock_guard<std::mutex> ph_lock(phase_compensation->state_mutex);
            PhaseCompensatorState old_state = phase_compensation->state;
            phase_compensation->state = PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION;
            phase_compensation->compensation_applied = false;
            phase_compensation->convergence_count = 0;
            phase_compensation->stable_nonzero_count = 0;
            // Stop applying the per-bin equalizer: it was designed for the
            // coherent tuning, and during the scan every tuner sits at a
            // different frequency. (It stayed applied to the scan data.)
            phase_compensation->per_bin_measured = false;
            per_bin_cal.ready.store(false, std::memory_order_release);

            // If we interrupted active calibration, turn off bias tee
            if (old_state != PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION &&
                old_state != PhaseCompensatorState::CONVERGED) {
                std::cout << "Wideband: Interrupted phase calibration (was in state "
                         << static_cast<int>(old_state) << "), disabling bias tee" << std::endl;
                set_bias_tee_all_devices(false, devices);
            }
        }

        // Disable FFT processing during wideband mode
        {
            std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
            fft_control.fft_enabled = false;
            fft_control.auto_disabled = true;
        }

        std::cout << "Wideband: Phase calibration reset to idle state" << std::endl;
        std::cout << "Wideband: FFT processing disabled" << std::endl;

    } else {
        // Returning to coherent mode
        operating_mode = OperatingMode::COHERENT;
        wideband_config.enabled = false;

        // Restore all tuners to the same frequency
        uint32_t coherent_freq = static_cast<uint32_t>(current_frequency.load());
        {
            std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
            for (const auto& device : devices) {
                if (device && device->dev) {
                    rtlsdr_set_center_freq(device->dev, coherent_freq);
                    wideband_config.set_tuner_frequency(device->index, coherent_freq);
                }
            }
        }

        // Re-enable phase calibration by resetting to wait for lag convergence state
        if (phase_compensation) {
            std::lock_guard<std::mutex> ph_lock(phase_compensation->state_mutex);
            // Identity vector, counters reset, per-bin equalizer dropped: the
            // re-convergence re-measures it (keeping per_bin_measured set used
            // to skip that, leaving a filter from before the scan applied).
            reset_phase_state_locked(PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION);
        }

        // DO NOT reset lag compensation states when returning from wideband mode
        // This prevents the system from trying to recalibrate lag when the noise source
        // may not have fully activated yet. We only want to do phase compensation.
        // Keep devices in CONVERGED state with lag_compensation_locked = true
        std::cout << "Wideband: Keeping lag compensation in locked/converged state" << std::endl;
        for (const auto& device : devices) {
            if (device) {
                std::lock_guard<std::mutex> comp_lock(device->compensation_mutex);
                // Ensure lag compensation stays locked and converged
                if (device->compensation.state == LagCompensatorState::CONVERGED) {
                    device->compensation.lag_compensation_locked = true;
                    device->compensation.initial_calibration_complete = true;
                    std::cout << "Channel " << device->index << " lag state kept as CONVERGED (locked)" << std::endl;
                } else {
                    // If for some reason it wasn't converged, keep it in current state
                    std::cout << "Channel " << device->index << " lag state: "
                             << static_cast<int>(device->compensation.state) << " (unchanged)" << std::endl;
                }
            }
        }

        // Enable FFT processing
        {
            std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
            fft_control.fft_enabled = true;
            fft_control.auto_disabled = false;
            fft_control.user_override = false;
        }

        // Enable bias tee for phase calibration
        std::cout << "Wideband: Enabling bias tee for phase calibration" << std::endl;
        set_bias_tee_all_devices(true, devices);

        // Add delay to allow noise source to fully activate and stabilize
        // This prevents processing stale IQ data that doesn't see the noise source
        std::cout << "Wideband: Waiting 500ms for noise source stabilization..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Clear buffers AFTER delay to ensure we get fresh IQ data with noise source active
        clear_l2_buffer();
        std::cout << "Wideband: Buffers cleared, ready for phase calibration with fresh data" << std::endl;

        std::cout << "Wideband: Returned to coherent mode" << std::endl;
        std::cout << "Wideband: Phase calibration re-enabled (lag compensation locked)" << std::endl;
        std::cout << "Wideband: All tuners set to " << coherent_freq/1e6 << " MHz" << std::endl;
    }

    return true;
}

bool set_tuner_frequency(int tuner_index, uint32_t frequency, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    if (tuner_index < 0 || tuner_index >= static_cast<int>(devices.size())) {
        std::cerr << "Wideband: Invalid tuner index " << tuner_index << std::endl;
        return false;
    }

    if (frequency < RTL_TUNER_MIN_HZ || frequency > RTL_TUNER_MAX_HZ) {
        std::cerr << "Wideband: Frequency out of range: " << frequency << std::endl;
        return false;
    }

    // Only allow in wideband mode
    if (operating_mode.load() != OperatingMode::WIDEBAND_SCAN) {
        std::cerr << "Wideband: Cannot set individual tuner frequency in coherent mode" << std::endl;
        return false;
    }

    // Set the frequency on the physical device
    bool success = false;
    for (const auto& device : devices) {
        if (device && device->index == tuner_index && device->dev) {
            if (rtlsdr_set_center_freq(device->dev, frequency) == 0) {
                wideband_config.set_tuner_frequency(tuner_index, frequency);
                std::cout << "Wideband: Tuner " << tuner_index << " set to "
                         << frequency/1e6 << " MHz" << std::endl;
                success = true;
            } else {
                std::cerr << "Wideband: Failed to set frequency for tuner " << tuner_index << std::endl;
            }
            break;
        }
    }

    return success;
}

void setup_wideband_frequencies(uint64_t base_frequency, const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    if (operating_mode.load() != OperatingMode::WIDEBAND_SCAN) {
        std::cerr << "Wideband: Not in wideband scan mode" << std::endl;
        return;
    }

    // Use the edge clip setting for tuner spacing (matches client-side FFT stitching)
    const float edge_clip = wideband_config.edge_clip.load();

    std::cout << "Wideband: Setting up frequency plan with base " << base_frequency/1e6
              << " MHz, edge_clip=" << (edge_clip * 100.0f) << "%" << std::endl;

    // For 5 tuners with 2.4 MHz bandwidth, use edge_clip * bandwidth for spacing
    // This ensures tuner coverage matches the FFT stitching on the client
    const uint32_t usable_bandwidth = static_cast<uint32_t>(SAMPLE_RATE * edge_clip);

    // Center the array around the base frequency
    // Tuner 2 (middle) gets base_frequency
    // Tuner 0: base - 2*spacing
    // Tuner 1: base - spacing
    // Tuner 2: base
    // Tuner 3: base + spacing
    // Tuner 4: base + 2*spacing

    // Spread across the tuners actually open (runtime count, not the ceiling).
    // Calculate all target frequencies first (tuner domain, fits uint32)
    // devices holds one slot per EXPECTED serial (8 by default), not per open
    // dongle: spreading over the slots put the requested center on the top
    // open tuner, so the stitched spectrum (centered on the open tuners) sat
    // two spacings below every requested pan and snapped away from it.
    const int num_tuners = std::min(active_num_elements.load(), static_cast<int>(devices.size()));
    std::vector<uint32_t> target_freqs(num_tuners);
    for (int i = 0; i < num_tuners; i++) {
        int offset_from_center = i - (num_tuners / 2);  // e.g. -2..2 for 5 tuners
        target_freqs[i] = static_cast<uint32_t>(base_frequency + (offset_from_center * usable_bandwidth));
    }

    // Tune all devices in PARALLEL - critical for responsive drag-to-tune
    // Each RTL-SDR has independent PLL, so parallel tuning reduces total time
    // from N*latency (~500ms) to max(latency) (~100ms)
    std::vector<std::thread> tune_threads;
    std::vector<char> results(num_tuners, 0);  // char: see init_results

    for (int i = 0; i < num_tuners; i++) {
        tune_threads.emplace_back([i, &target_freqs, &devices, &results]() {
            for (const auto& device : devices) {
                if (device && device->index == i && device->dev) {
                    if (rtlsdr_set_center_freq(device->dev, target_freqs[i]) == 0) {
                        wideband_config.set_tuner_frequency(i, target_freqs[i]);
                        results[i] = true;
                    }
                    break;
                }
            }
        });
    }

    // Wait for all tuners to complete
    for (auto& thread : tune_threads) {
        thread.join();
    }

    // Log results
    int success_count = 0;
    for (int i = 0; i < num_tuners; i++) {
        if (results[i]) success_count++;
    }

    std::cout << "Wideband: " << success_count << "/" << num_tuners << " tuners configured in parallel" << std::endl;
    std::cout << "Wideband: Total coverage = " << (num_tuners * usable_bandwidth) / 1e6 << " MHz" << std::endl;
}