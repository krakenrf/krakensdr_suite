#pragma once

// Checked GPIO0 OFF receipt for the standard internal KrakenSDR noise source.
// This deliberately does not model external-noise, Wideband or Kerberos GPIOs.
// A successful librtlsdr call acknowledges a driver write, NOT measured voltage.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

enum class NoiseOffOutcome : std::uint8_t {
    ALL_ACKNOWLEDGED,
    UNSUPPORTED_PATH,
    INVALID_ACTIVE_COUNT,
    MISSING_RECEIVER,
    DRIVER_WRITE_FAILED
};

struct NoiseOffChannelReceipt {
    std::size_t slot = 0;
    bool present = false;
    int driver_rc = 0;  // defined only for present == true
};

struct NoiseOffReceipt {
    NoiseOffOutcome outcome = NoiseOffOutcome::INVALID_ACTIVE_COUNT;
    int active_count = 0;
    std::vector<NoiseOffChannelReceipt> channels;

    bool all_acknowledged() const noexcept {
        return outcome == NoiseOffOutcome::ALL_ACKNOWLEDGED &&
               static_cast<int>(channels.size()) == active_count;
    }
};

inline const char* noise_off_outcome_name(NoiseOffOutcome value) noexcept {
    switch (value) {
        case NoiseOffOutcome::ALL_ACKNOWLEDGED: return "all_gpio0_off_writes_acknowledged";
        case NoiseOffOutcome::UNSUPPORTED_PATH: return "unsupported_noise_source_path";
        case NoiseOffOutcome::INVALID_ACTIVE_COUNT: return "invalid_active_receiver_count";
        case NoiseOffOutcome::MISSING_RECEIVER: return "missing_or_unexpected_receiver";
        case NoiseOffOutcome::DRIVER_WRITE_FAILED: return "gpio0_off_write_failed";
    }
    return "unknown";
}

// The caller owns device_io_mutex and verifies the board path is the STANDARD
// internal-noise path. WriteOff(slot) returns nullopt for an unavailable or
// wrong-identity handle; otherwise it performs exactly one GPIO0 OFF write
// and returns the raw librtlsdr return code.
//
// All eligible channels are attempted, even after a failure. A missing slot
// never counts as success. max_expected is the configured/compiled ceiling;
// a nonsense active count cannot cause an unbounded sweep.
//
// VisitSlots(n, fn) MUST run fn(i) exactly once per available slot and join all
// its worker threads before returning. Distinct vector elements are written
// concurrently; only the caller thread aggregates them after the join. The
// real driver passes for_each_tuner, preserving upstream's parallel Linux GPIO
// operations and sequential WinUSB operations.
template <class VisitSlots, class WriteOff>
NoiseOffReceipt checked_standard_noise_off(
        int active_count, std::size_t max_expected, std::size_t available_slots,
        VisitSlots&& visit_slots, WriteOff&& write_off) {
    NoiseOffReceipt report;
    report.active_count = active_count;
    if (active_count < 2 || static_cast<std::size_t>(active_count) > max_expected)
        return report;

    const auto count = static_cast<std::size_t>(active_count);
    std::vector<std::optional<int>> results(count);
    std::forward<VisitSlots>(visit_slots)(count, [&](std::size_t slot) {
        if (slot < count && slot < available_slots)
            results[slot] = write_off(slot);
    });

    report.channels.reserve(count);
    bool missing = false, failed = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& rc = results[i];
        if (!rc) missing = true;
        else if (*rc != 0) failed = true;
        report.channels.push_back({i, rc.has_value(), rc.value_or(0)});
    }
    report.outcome = missing ? NoiseOffOutcome::MISSING_RECEIVER :
                     failed ? NoiseOffOutcome::DRIVER_WRITE_FAILED :
                              NoiseOffOutcome::ALL_ACKNOWLEDGED;
    return report;
}

// Sequential mock/test default; the production caller supplies its
// existing platform-aware fan-out instead.
template <class WriteOff>
NoiseOffReceipt checked_standard_noise_off(
        int active_count, std::size_t max_expected, std::size_t available_slots,
        WriteOff&& write_off) {
    return checked_standard_noise_off(
        active_count, max_expected, available_slots,
        [](std::size_t n, auto&& each) {
            for (std::size_t i = 0; i < n; ++i) each(i);
        }, std::forward<WriteOff>(write_off));
}
