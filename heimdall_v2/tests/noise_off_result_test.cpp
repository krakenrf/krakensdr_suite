// No RTL hardware: exercise the same GPIO0 OFF receipt used by calibration.
#include "../src/sdr/noise_off_result.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

static unsigned checks = 0;
static void expect(bool yes, const char* name) {
    ++checks;
    if (!yes) { std::cerr << "FAIL " << name << '\n'; std::abort(); }
}
int main() {
    using O = NoiseOffOutcome;
    for (int n = 2; n <= 8; ++n) {
        std::vector<int> calls;
        auto r = checked_standard_noise_off(n, 8, 8,
            [&](std::size_t i) -> std::optional<int> {
                calls.push_back(static_cast<int>(i)); return 0;
            });
        expect(r.all_acknowledged() && r.outcome == O::ALL_ACKNOWLEDGED, "valid all-ACK");
        expect(r.active_count == n && static_cast<int>(calls.size()) == n, "active write count");
        for (int i = 0; i < n; ++i) {
            expect(calls[i] == i, "write order");
            expect(r.channels[i].present && r.channels[i].driver_rc == 0, "per-slot ACK");
        }
    }
    for (int n : {-2, 0, 1, 9, 1000000}) {
        unsigned calls = 0;
        auto r = checked_standard_noise_off(n, 8, 8,
            [&](std::size_t) -> std::optional<int> { ++calls; return 0; });
        expect(r.outcome == O::INVALID_ACTIVE_COUNT && !r.all_acknowledged(),
               "invalid count refuses convergence");
        expect(calls == 0, "invalid count avoids GPIO writes");
    }
    {
        std::vector<std::size_t> calls;
        auto r = checked_standard_noise_off(5, 8, 4,
            [&](std::size_t i) -> std::optional<int> { calls.push_back(i); return 0; });
        expect(r.outcome == O::MISSING_RECEIVER && !r.all_acknowledged(),
               "vector missing active channel");
        expect(calls.size() == 4 && r.channels.size() == 5 &&
               !r.channels[4].present, "attempt available and record missing");
    }
    {
        auto r = checked_standard_noise_off(5, 8, 5,
            [](std::size_t i) -> std::optional<int> {
                return i == 2 ? std::nullopt : std::optional<int>{0};
            });
        expect(r.outcome == O::MISSING_RECEIVER && !r.all_acknowledged(),
               "missing handle or wrong receiver identity");
    }
    {
        auto r = checked_standard_noise_off(5, 8, 5,
            [](std::size_t i) -> std::optional<int> { return i == 2 ? -7 : 0; });
        expect(r.outcome == O::DRIVER_WRITE_FAILED && !r.all_acknowledged() &&
               r.channels[2].driver_rc == -7, "driver error preserved");
    }
    // 3^5 = 243 possible ACK / failed write / missing receiver patterns.
    for (int pattern = 0; pattern < 243; ++pattern) {
        std::array<int,5> state{};
        int v = pattern;
        bool missing = false, failed = false;
        for (int i = 0; i < 5; ++i) {
            state[i] = v % 3; v /= 3;
            missing |= state[i] == 2;
            failed |= state[i] == 1;
        }
        unsigned calls = 0;
        auto r = checked_standard_noise_off(5, 8, 5,
            [&](std::size_t i) -> std::optional<int> {
                ++calls;
                if (state[i] == 2) return std::nullopt;
                return state[i] == 1 ? std::optional<int>{-12} :
                                       std::optional<int>{0};
            });
        expect(calls == 5 && r.channels.size() == 5, "attempt all channels");
        expect(r.all_acknowledged() == (!missing && !failed),
               "no false success under any five-channel fault pattern");
        expect(r.outcome == (missing ? O::MISSING_RECEIVER :
                             failed ? O::DRIVER_WRITE_FAILED :
                                      O::ALL_ACKNOWLEDGED),
               "fault classification");
    }
    // Production-like Linux parallel dispatch: the worker executor must JOIN
    // before the caller classifies the outcomes.
    {
        const std::array<int,5> driver_codes{0, -7, 0, -12, 0};
        auto r = checked_standard_noise_off(5, 8, 5,
            [](std::size_t n, auto&& fn) {
                std::vector<std::thread> jobs;
                for (std::size_t i = 0; i < n; ++i)
                    jobs.emplace_back([&,i]() { fn(i); });
                for (auto& job : jobs) job.join();
            },
            [&](std::size_t i) -> std::optional<int> { return driver_codes[i]; });
        expect(r.outcome == O::DRIVER_WRITE_FAILED && !r.all_acknowledged(),
               "parallel GPIO write failure is not success");
        expect(r.channels[1].driver_rc == -7 && r.channels[3].driver_rc == -12,
               "parallel channel result aggregation");
    }
    {
        auto r = checked_standard_noise_off(5, 8, 5,
            [](std::size_t n, auto&& fn) {
                for (std::size_t i = 0; i + 1 < n; ++i) fn(i);
            },
            [](std::size_t) -> std::optional<int> { return 0; });
        expect(r.outcome == O::MISSING_RECEIVER && !r.all_acknowledged(),
               "incomplete executor is never reported as successful");
    }
    NoiseOffReceipt unsupported;
    unsupported.outcome = O::UNSUPPORTED_PATH;
    unsupported.active_count = 5;
    expect(!unsupported.all_acknowledged(), "unsupported mode not acknowledged");
    std::cout << "PASS noise OFF receipt: " << checks
              << " assertions; 243 exhaustive five-receiver patterns\n";
}
