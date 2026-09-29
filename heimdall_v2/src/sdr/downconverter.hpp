#pragma once

#include <atomic>
#include <cstdint>

// ---- KrakenSDR Wideband (downconverter) variant ----
// The Wideband hardware puts a mixer in front of every tuner, all fed by ONE
// shared LO: an Othernet moRFeus box running in signal-generator mode,
// controlled over USB HID (Silicon Labs USBXpress, VID 0x10C4 / PID 0xEAC9).
// The tuners are parked at the fixed IF (WB_VARIANT_IF_HZ, the hardware IF
// filter's centre) and a "retune" only reprograms the LO, which the mixer
// side (Mix High/Low/Below in the web UI) places around the IF:
//
//   high:  LO = IF + RF  (difference product LO - RF lands at the IF,
//          spectrum arrives mirrored; corrected in software), RF 24-4132 MHz
//   low:   LO = IF - RF  (sum product LO + RF lands at the IF, upright),
//          RF 24-1183 MHz
//   below: LO = RF - IF  (LO below the RF; difference product RF - LO lands
//          at the IF, upright), RF 1353-6668 MHz - the only side that
//          reaches past the synthesizer ceiling, up to IF + LO_MAX
//
// For any LO both RF = LO - IF and RF = LO + IF fold into the IF; the side
// choice decides which is the wanted signal and which the image (2*IF away).
//
// This is a distinct feature from OperatingMode::WIDEBAND_SCAN (which spreads
// the tuners across different frequencies for spectrum scanning) - the two
// are mutually exclusive because the variant requires all tuners at the IF.

enum class MixerSide : int {
    HIGH  = 0,  // LO = IF + RF
    LOW   = 1,  // LO = IF - RF
    BELOW = 2,  // LO = RF - IF
};

// Wire/UI name: "high" / "low" / "below" (set_mixer_side command, status JSON)
const char* mixer_side_name(MixerSide side);

struct Downconverter {
    std::atomic<bool> enabled{false};     // set once at startup by --wideband
    std::atomic<MixerSide> side{MixerSide::HIGH};
    std::atomic<uint64_t> lo_hz{0};       // last LO actually programmed
    std::atomic<int> lo_current{3};       // LO output drive current 0-7 (WB_LO_CURRENT
                                          // default, runtime set_lo_current command)

    // The Wideband front end has three concentric antenna rings selected by
    // on-board RF switches (GPIO1-6 of the channel-0 RTL2832U); the same
    // switch bank routes the calibration noise source into the receive path
    // (see wideband_set_noise_path in sdr_init.cpp).
    std::atomic<int> array_select{0};     // 0 = outer, 1 = center, 2 = inner

    // High-side injection (LO above the RF) conjugates the complex baseband
    // (mirrored spectrum, negated inter-channel phases). The conversion hot
    // loop un-mirrors it by negating Q when this is true, so everything
    // downstream - our own correlation/phase cal, the TCP broadcast, the DoA
    // client - sees a normally-oriented signal with the true RF steering
    // phases. Low (sum product) and below (LO under the RF) arrive upright.
    bool spectral_inversion_active() const {
        return enabled.load(std::memory_order_relaxed) &&
               side.load(std::memory_order_relaxed) == MixerSide::HIGH;
    }
};

extern Downconverter downconverter;

// Open the LO synthesizer and program it for initial_rf_hz on the current
// side (frequency only - the WB firmware's output is always on and it does
// not implement the stock-moRFeus mode/current functions). Call once at
// startup (after --wideband set downconverter.enabled). Returns false if the
// device is missing/unopenable - wideband mode cannot run without its LO.
bool downconverter_init(uint64_t initial_rf_hz);
void downconverter_close();

// Achievable RF range for a given injection side, derived from the moRFeus LO
// limits and the fixed IF.
void downconverter_rf_range(MixerSide side, uint64_t& min_hz, uint64_t& max_hz);
bool downconverter_rf_valid(uint64_t rf_hz, MixerSide side);

// RF span reachable on SOME side (union of the three) - the valid range for
// retunes now that the side auto-switches when the current one can't reach
// the requested RF.
void downconverter_rf_union_range(uint64_t& min_hz, uint64_t& max_hz);

// Preferred side for an RF the current side can't reach: high covers
// everything up to its ceiling (low is a strict subset of high's range),
// below covers the rest.
MixerSide downconverter_auto_side(uint64_t rf_hz);

// Antenna ring dictated by the RF: 0 = outer (< WB_RING_CENTER_MIN_HZ),
// 1 = center (< WB_RING_INNER_MIN_HZ), 2 = inner.
int wb_ring_for_rf(uint64_t rf_hz);

// Program the LO so rf_hz lands on the IF using the current side. Returns
// false (and leaves the LO untouched) if rf_hz is outside the side's range or
// the HID write fails.
bool downconverter_apply_rf(uint64_t rf_hz);

// Switch injection side and reprogram the LO for rf_hz on the new side. The
// side only changes if the LO write succeeds (false = nothing changed). May flip the spectral-inversion correction; the caller is responsible for
// triggering a phase recalibration (the LO distribution phases move).
bool downconverter_set_side(MixerSide side, uint64_t rf_hz);

// Set the LO output drive current (0-7). Shared by all mixers (one LO), so a
// change is common-mode across channels and needs no phase recalibration.
bool downconverter_set_current(int current);
