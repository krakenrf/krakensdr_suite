#!/usr/bin/env python3
"""Narrow source audit for actual Heimdall calibration handoffs."""
from pathlib import Path
p = Path(__file__).resolve().parents[1]
c = (p / "src/dsp/compensation.cpp").read_text()
h = (p / "src/core/calibration_evidence_guard.hpp").read_text()
def section(start, end):
    a = c.index(start)
    return c[a:c.index(end, a)]
checks = []
def check(msg, valid):
    checks.append(bool(valid))
    print(("PASS " if valid else "FAIL ") + msg)

check("production compensation includes standalone evidence header",
      '#include "../core/calibration_evidence_guard.hpp"' in c)
check("C++ helper requires >= 2 active elements",
      'num_channels < 2' in h)
check("helper requires every active non-reference channel",
      "it == readings.end() || !accept(it->second)" in h)
check("helper refuses out-of-range/stale channel keys",
      "entry.first < 0 || entry.first >= num_channels" in h)
check("helper validates phase finiteness, including supplied reference",
      h.count("std::isfinite(") == 2)
initial = section("void apply_phase_compensation_once(", "bool check_phase_convergence(")
check("initial phase accumulation guards required channel coverage",
      initial.index("complete_finite_phase_evidence(") <
      initial.index("phase_compensation->phase_accum_count++"))
check("invalid initial phase resets averaging streak",
      initial.index("stable_nonzero_count = 0;\n        phase_compensation->phase_accum_count = 0;") <
      initial.index("phase_compensation->phase_accum_count++"))
verify = section("bool check_phase_convergence(", "std::optional<PhaseCompensatorState> get_phase_compensation_state()")
check("phase convergence guards coverage before incrementing retry budget",
      verify.index("complete_finite_phase_evidence(") <
      verify.index("checks_since_compensation++"))
check("invalid verification evidence resets success streak and refuses success",
      "phase_compensation->convergence_count = 0;\n        return false;" in verify)
proc = c[c.index("void channel_lag_compensation_processor("):]
check("freshness-gate lag snapshot requires all active state keys",
      "lag_all_converged = kraken_calibration::all_lag_states_converged(" in proc)
check("WAITING_FOR_LAG_COMPLETION now requires complete lag state map",
      "all_lag_converged = kraken_calibration::all_lag_states_converged(" in proc)
check("no vacuous lag all_of remains in processor",
      "std::all_of(correlation_result.channel_states.begin()" not in proc)
check("source still gates phase processing on fresh/lag/noise prerequisites",
      "const bool phase_reading = fresh && lag_all_converged && phases_measured &&" in proc)
check("max retry fallback remains upstream behavior and is NOT addressed",
      "Phase: max convergence attempts reached" in verify)
print(f"CALIBRATION_REAL_SOURCE_AUDIT {sum(checks)}/{len(checks)} "
      + ("PASS" if all(checks) else "FAIL"))
raise SystemExit(0 if all(checks) else 1)
