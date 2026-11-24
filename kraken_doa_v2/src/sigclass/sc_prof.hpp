#pragma once

// sigclass internals: profiling scopes for the stage-by-stage cost table
// (`sigclass_tool bench --profile`). Compiled in only with -DSIGCLASS_PROF;
// otherwise SC_PROF expands to nothing (the app's build is unaffected).
// Not thread-safe (one Analyzer on one thread, as in the tool).

#ifdef SIGCLASS_PROF
#include <chrono>
#include <string>
#include <vector>

namespace sigclass::prof {
struct Slot {
    const char* name;
    double ms = 0.0;
    long long n = 0;
};
std::vector<Slot>& slots();
int reg(const char* name);
void reset();
struct Scope {
    int id;
    std::chrono::steady_clock::time_point t0;
    explicit Scope(int i) : id(i), t0(std::chrono::steady_clock::now()) {}
    ~Scope() {
        Slot& s = slots()[size_t(id)];
        s.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        s.n++;
    }
};
}  // namespace sigclass::prof

#define SC_PROF_CAT2(a, b) a##b
#define SC_PROF_CAT(a, b) SC_PROF_CAT2(a, b)
#define SC_PROF(name)                                                              \
    static const int SC_PROF_CAT(sc_pid_, __LINE__) = ::sigclass::prof::reg(name); \
    ::sigclass::prof::Scope SC_PROF_CAT(sc_ps_, __LINE__)(SC_PROF_CAT(sc_pid_, __LINE__))
#define SC_PROF_ID(id) ::sigclass::prof::Scope SC_PROF_CAT(sc_ps_, __LINE__)(id)
#else
#define SC_PROF(name) static_assert(true, "")
#endif
