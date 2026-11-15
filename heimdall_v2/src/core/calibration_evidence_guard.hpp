#pragma once

// A calibration state may advance only when every active, non-reference
// channel is represented by a valid reading. std::all_of(empty) is true;
// incomplete lag maps and partial phase maps must not look "CONVERGED".
// This is a host-measurement coverage check, NOT an ADC/USB freshness proof.
#include <cmath>
#include <cstddef>
#include <map>

namespace kraken_calibration {

template <typename Value, typename Accept>
bool complete_nonreference_evidence(
    const std::map<int, Value>& readings, int num_channels,
    int reference_channel, Accept accept) {
    if (num_channels < 2 || reference_channel < 0 ||
        reference_channel >= num_channels) return false;

    const auto required = static_cast<std::size_t>(num_channels - 1);
    if (readings.size() < required ||
        readings.size() > static_cast<std::size_t>(num_channels)) return false;

    for (int ch = 0; ch < num_channels; ++ch) {
        if (ch == reference_channel) continue;
        const auto it = readings.find(ch);
        if (it == readings.end() || !accept(it->second)) return false;
    }
    // A map with a stale/out-of-range entry is not a coherent generation.
    for (const auto& entry : readings) {
        if (entry.first < 0 || entry.first >= num_channels) return false;
    }
    return true;
}

template <typename State>
bool all_lag_states_converged(const std::map<int, State>& states,
                              int num_channels, int reference_channel,
                              State converged) {
    return complete_nonreference_evidence(
        states, num_channels, reference_channel,
        [converged](State state) { return state == converged; });
}

inline bool complete_finite_phase_evidence(
    const std::map<int, float>& phases, int num_channels,
    int reference_channel) {
    const bool valid_nonreference = complete_nonreference_evidence(
        phases, num_channels, reference_channel,
        [](float phase) { return std::isfinite(phase); });
    if (!valid_nonreference) return false;
    // A supplied reference reading should also be finite, even though it
    // is not part of the residual compensation calculation.
    const auto ref = phases.find(reference_channel);
    return ref == phases.end() || std::isfinite(ref->second);
}

} // namespace kraken_calibration
