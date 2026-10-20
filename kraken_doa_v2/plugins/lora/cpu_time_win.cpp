// lora/cpu_time_win.cpp - Windows only (empty elsewhere): the calling
// thread's CPU time for the search's CPU budget (decoder.cpp thread_cpu_s).
// winpthreads' clock_gettime(CLOCK_THREAD_CPUTIME_ID) reads GetThreadTimes,
// which only moves at the 15.6 ms clock tick - most of the budget's per-call
// deltas read 0, a few 15.6 ms, and the token bucket ran on noise. The
// thread's cycle count (QueryThreadCycleTime, counted with the TSC at every
// context switch) has sub-microsecond resolution; it is turned into seconds
// with the TSC rate, measured once against the performance counter (as
// Chromium's ThreadTicks does). In a file of its own so <windows.h> and its
// macros stay out of the decoder.
#ifdef _WIN32
#include <windows.h>
#if defined(__x86_64__) || defined(__i386__)
#include <intrin.h>
#endif

namespace {

// TSC ticks per second, 0 = no invariant TSC to count with
double tsc_hz() {
#if defined(__x86_64__) || defined(__i386__)
    static const double hz = [] {
        int r[4] = {};
        __cpuid(r, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(r[0]) < 0x80000007u) return 0.0;
        __cpuid(r, static_cast<int>(0x80000007u));
        if (!(r[3] & (1 << 8))) return 0.0;   // EDX bit 8: invariant TSC
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        const unsigned long long c0 = __rdtsc();
        Sleep(20);   // once per process; the elapsed time is the counter's, not Sleep's
        QueryPerformanceCounter(&q1);
        const unsigned long long c1 = __rdtsc();
        const double s = static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(f.QuadPart);
        return s > 0.0 && c1 > c0 ? static_cast<double>(c1 - c0) / s : 0.0;
    }();
    return hz;
#else
    return 0.0;
#endif
}

double filetime_s(const FILETIME& t) {
    return static_cast<double>((static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7;
}

}  // namespace

double lora_thread_cpu_s_win() {
    const double hz = tsc_hz();
    ULONG64 cycles = 0;
    if (hz > 0.0 && QueryThreadCycleTime(GetCurrentThread(), &cycles)) return static_cast<double>(cycles) / hz;
    FILETIME created, exited, kernel, user;   // coarse fallback: the 15.6 ms tick
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    return filetime_s(kernel) + filetime_s(user);
}
#endif  // _WIN32
