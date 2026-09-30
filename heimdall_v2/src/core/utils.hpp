#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>

// Endian utilities
void append_big_endian(std::vector<uint8_t>& vec, uint32_t value);

// Thread scheduling (best-effort). Names the calling thread, optionally pins it
// to a CPU (cpu >= 0), and raises it to SCHED_RR at the given realtime priority
// (round-robin, NOT FIFO: several equal-priority USB readers must time-slice on
// a 4-core Pi rather than be able to lock one another out). Returns true only on
// full success; if the process lacks privilege (no CAP_SYS_NICE / RLIMIT_RTPRIO)
// it logs once and continues at normal priority rather than failing - the
// time-critical USB reader and sample drain threads use this so they resist
// preemption under high CPU load, but the app must still run unprivileged. Pass
// rt_priority <= 0 to set only name/affinity (used for the conversion worker).
bool set_thread_realtime(const char* name, int rt_priority, int cpu = -1);

// Math utilities
template<typename T>
constexpr T clamp(T value, T min_val, T max_val) {
    return std::min(std::max(value, min_val), max_val);
}

// Time utilities
#include <chrono>
using steady_clock = std::chrono::steady_clock;
using milliseconds = std::chrono::milliseconds;

// IQ conversion lookup table
const float* iq_lut();

// UTF-8. Browsers close a WebSocket that receives a TEXT frame with invalid
// UTF-8, so every client-supplied string that is echoed back out must be
// valid. utf8_valid: strict (no overlongs, surrogates or > U+10FFFF).
// utf8_sanitize: replaces each invalid byte with U+FFFD.
bool utf8_valid(std::string_view s);
std::string utf8_sanitize(std::string_view s);
