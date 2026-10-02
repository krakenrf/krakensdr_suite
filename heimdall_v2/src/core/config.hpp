#pragma once

#include <cstdint>
#include <vector>
#include <string>

extern "C" {
#include "config.h"
}

// Constants from config.h
constexpr float SCALE = 1.0f / 127.5f;
constexpr uint32_t TCP_MAGIC = 0x4D434851;  // 'MCHQ'
constexpr int TCP_DATA_PORT = 8091;
constexpr int TCP_CONTROL_PORT = 8092;
constexpr int RTL_TCP_PORT = 1234;  // Standard rtl_tcp port

// Serial number configuration - channel N is the device with serial
// expected_serials[N]. Defaults to "1000".."1007" (the KrakenSDR factory
// serial convention, extended to the NUM_DEVICES ceiling so the element count
// can be raised to 8 without --serials); overridden at startup by
// --serials s0,s1,... (defined in main.cpp, written only during argument
// parsing - treated as immutable once threads start). Its length is the
// runtime device ceiling: 2 <= active_num_elements <= expected_serials.size()
// <= NUM_DEVICES.
extern std::vector<std::string> expected_serials;

// KerberosSDR support modes (set by --kerberos / --kerberos_sw during
// argument parsing; defined in main.cpp - see the comment there).
#include <atomic>
extern std::atomic<bool> kerberos_mode;
extern std::atomic<bool> kerberos_sw_mode;
extern std::atomic<bool> kerberos_cal_stale;

// --ext_noise: a standard KrakenSDR with an add-on antenna array that has its
// own noise source, powered from the CH0 antenna bias tee (GPIO1 of the
// channel-0 chip). Every noise-source switch (set_bias_tee_all_devices and the
// startup open) drives that bias tee instead of the KrakenSDR's internal noise
// source, which stays OFF; CH0's bias tee is then reserved (not a per-port
// antenna bias tee). Calibration is otherwise unchanged. Defined in main.cpp.
extern std::atomic<bool> external_noise_mode;

// True when calibration may ONLY be triggered manually: plain --kerberos has
// no way to isolate the noise source from the antennas, so every automatic
// noise-on path must be suppressed. --kerberos_sw restores automatic
// calibration (the CKOVAL switches isolate the antennas).
inline bool kerberos_manual_cal_only() {
    return kerberos_mode.load(std::memory_order_relaxed) &&
           !kerberos_sw_mode.load(std::memory_order_relaxed);
}

