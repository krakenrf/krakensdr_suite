#include "../src/core/calibration_evidence_guard.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

enum class Lag { MEASURING, SERVOING, CONVERGED };
static int count = 0;
static void require(const char* name, bool result) {
    if (!result) throw std::runtime_error(name);
    std::cout << "PASS " << ++count << " " << name << '\n';
}

int main() {
    using namespace kraken_calibration;
    const auto yes = Lag::CONVERGED;
    const auto no = Lag::MEASURING;
    const std::map<int, Lag> empty;
    const auto valid_lag = [yes](const std::map<int, Lag>& m, int n = 5, int ref = 0) {
        return all_lag_states_converged(m, n, ref, yes);
    };
    const auto valid_phase = [](const std::map<int, float>& m, int n = 5, int ref = 0) {
        return complete_finite_phase_evidence(m, n, ref);
    };
    require("demonstrate vacuous std::all_of", std::all_of(empty.begin(), empty.end(),
                                                     [](const auto&) { return true; }));
    require("empty lag never converged", !valid_lag(empty));
    require("empty lag also rejected for two elements", !valid_lag(empty, 2));
    require("two-channel non-reference lag valid", valid_lag({{1,yes}}, 2));
    require("five-channel non-reference lags valid",
            valid_lag({{1,yes},{2,yes},{3,yes},{4,yes}}));
    require("reference lag may be present without being converged",
            valid_lag({{0,no},{1,yes},{2,yes},{3,yes},{4,yes}}));
    require("missing lag key refused", !valid_lag({{1,yes},{2,yes},{3,yes}}));
    require("reference-only lag refused", !valid_lag({{0,yes}}, 2));
    require("partially converged lag refused", !valid_lag({{1,yes},{2,no},{3,yes},{4,yes}}));
    require("out of range lag refused", !valid_lag({{1,yes},{2,yes},{3,yes},{4,yes},{5,yes}}));
    require("negative lag key refused", !valid_lag({{-1,yes},{1,yes},{2,yes},{3,yes},{4,yes}}));
    require("invalid element count refused", !valid_lag({{1,yes}}, 1));
    require("invalid reference channel refused", !valid_lag({{1,yes}}, 2, 3));
    require("alternate reference index accepted", valid_lag({{0,yes},{2,yes}},3,1));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    require("empty phase refused", !valid_phase({}));
    require("reference-only phase refused", !valid_phase({{0,0.0f}}));
    require("partial phase refused", !valid_phase({{0,0},{1,0},{2,0},{4,0}}));
    require("finite zero phases accepted", valid_phase({{0,0},{1,0},{2,0},{3,0},{4,0}}));
    require("finite nonzero phases accepted", valid_phase({{1,0.8f},{2,-1.4f},{3,90},{4,-180}}));
    require("two-channel without reference accepted", valid_phase({{1,0.2f}},2));
    require("NaN phase refused", !valid_phase({{0,0},{1,0},{2,nan},{3,0},{4,0}}));
    require("infinite phase refused", !valid_phase({{0,0},{1,0},{2,inf},{3,0},{4,0}}));
    require("nonfinite reference refused", !valid_phase({{0,nan},{1,0},{2,0},{3,0},{4,0}}));
    require("out of range phase refused", !valid_phase({{1,0},{2,0},{3,0},{4,0},{5,0}}));
    require("negative phase key refused", !valid_phase({{-1,0},{1,0},{2,0},{3,0},{4,0}}));
    require("invalid phase element count refused", !valid_phase({{1,0}},1));
    require("alternate reference phase index accepted", valid_phase({{0,0},{2,0}},3,1));
    std::cout << "CALIBRATION_EVIDENCE_GUARD " << count << "/" << count
              << " PASS (synthetic; no physical/transport freshness proof)\n";
}
