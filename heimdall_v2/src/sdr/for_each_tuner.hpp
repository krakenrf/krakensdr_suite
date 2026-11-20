#pragma once

#include <cstddef>
#include <system_error>
#include <thread>
#include <vector>

// Runs fn(0) .. fn(n - 1), one call per tuner: in parallel threads on
// Linux - each RTL-SDR has its own PLL settling time, so it costs
// max(latency) rather than n x latency - and one after the other on
// Windows, where concurrent control transfers to the KrakenSDR's dongles
// time out on WinUSB (-7): opened together, 4 of 5 dongles' first register
// write timed out and librtlsdr reset them mid-init; a parallel retune left
// one tuner on the old frequency and forced a coherence recovery.
// Each call may touch only its own tuner's handle and result slot.
template <class F>
static void for_each_tuner(size_t n, F&& fn) {
#ifdef _WIN32
    for (size_t i = 0; i < n; i++) fn(i);
#else
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (size_t i = 0; i < n; i++) {
        // No thread to be had (EAGAIN: thread / memory limits): that tuner
        // on this thread instead. The exception escaping would destroy the
        // joinable threads above - std::terminate, e.g. in the shutdown's
        // noise-source switch-off before the handles are closed.
        try {
            threads.emplace_back([&fn, i]() { fn(i); });
        } catch (const std::system_error&) {
            fn(i);
        }
    }
    for (auto& t : threads) t.join();
#endif
}
