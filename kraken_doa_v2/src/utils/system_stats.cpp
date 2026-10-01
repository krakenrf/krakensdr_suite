#include "utils/system_stats.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <mutex>

using namespace std;
using namespace chrono;

// Read CPU temperature from thermal zone
static float read_cpu_temp() {
    ifstream f("/sys/class/thermal/thermal_zone0/temp");
    if (f.is_open()) {
        int millidegrees;
        if (f >> millidegrees) {
            return millidegrees / 1000.0f;
        }
    }
    return -1.0f;
}

// Read CPU usage from /proc/stat (delta since the previous sample).
// Two threads ask for it - the 500 ms status broadcast (uWS loop) and the TUI
// dashboard - and the previous totals were plain statics: a data race, and
// each caller's "since my last call" window was cut short by the other's.
// Now one mutex-guarded sample at most every 400 ms, shared by both.
static float read_cpu_usage() {
    static std::mutex mtx;
    static std::chrono::steady_clock::time_point last_sample{};
    static float cached = 0.0f;
    static long long prev_total = 0;
    static long long prev_idle = 0;

    std::lock_guard<std::mutex> lock(mtx);
    const auto now = std::chrono::steady_clock::now();
    if (last_sample.time_since_epoch().count() != 0 &&
        now - last_sample < std::chrono::milliseconds(400)) {
        return cached;
    }
    last_sample = now;

    ifstream f("/proc/stat");
    if (!f.is_open()) return 0.0f;

    string line;
    getline(f, line);
    // Format: cpu  user nice system idle iowait irq softirq steal
    if (line.substr(0, 3) != "cpu") return 0.0f;

    istringstream iss(line.substr(3));
    long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (!(iss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal))
        return 0.0f;

    long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    long long idle_total = idle + iowait;

    long long total_delta = total - prev_total;
    long long idle_delta = idle_total - prev_idle;

    prev_total = total;
    prev_idle = idle_total;

    if (total_delta <= 0) return cached;
    cached = 100.0f * (1.0f - static_cast<float>(idle_delta) / static_cast<float>(total_delta));
    return cached;
}

// Read RAM usage from /proc/meminfo
static void read_ram_usage(float& used_mb, float& total_mb) {
    ifstream f("/proc/meminfo");
    if (!f.is_open()) { used_mb = 0; total_mb = 0; return; }

    long long mem_total_kb = 0, mem_available_kb = 0;
    string line;
    while (getline(f, line)) {
        if (line.compare(0, 9, "MemTotal:") == 0) {
            istringstream iss(line.substr(9));
            iss >> mem_total_kb;
        } else if (line.compare(0, 13, "MemAvailable:") == 0) {
            istringstream iss(line.substr(13));
            iss >> mem_available_kb;
        }
        if (mem_total_kb && mem_available_kb) break;
    }
    total_mb = mem_total_kb / 1024.0f;
    used_mb = (mem_total_kb - mem_available_kb) / 1024.0f;
}

HardwareStats get_hardware_stats() {
    HardwareStats stats;
    stats.cpu_temp_c = read_cpu_temp();
    stats.cpu_usage_pct = read_cpu_usage();
    read_ram_usage(stats.ram_used_mb, stats.ram_total_mb);
    return stats;
}

// OPTIMIZATION: Returns thread index for lock-free stats access
size_t SystemStats::register_thread(const string& name) {
    lock_guard<mutex> lock(registration_mutex);

    // Check if thread already registered
    for (size_t i = 0; i < thread_count.load(std::memory_order_relaxed); i++) {
        if (thread_stats[i].active.load(std::memory_order_relaxed) && thread_stats[i].name == name) {
            return i;  // Already registered, return existing index
        }
    }

    // Allocate new slot
    size_t idx = thread_count.fetch_add(1, std::memory_order_relaxed);
    if (idx >= MAX_THREADS) {
        cerr << "ERROR: Too many threads registered (max " << MAX_THREADS << ")" << endl;
        return 0;  // Fallback to slot 0
    }

    thread_stats[idx].name = name;
    thread_stats[idx].active.store(true, std::memory_order_release);

    return idx;
}

// OPTIMIZATION: Lock-free atomic increment (no mutex, no string lookup!)
void SystemStats::increment_operations(size_t thread_idx, size_t count) {
    if (thread_idx < MAX_THREADS && thread_stats[thread_idx].active.load(std::memory_order_relaxed)) {
        thread_stats[thread_idx].operations.fetch_add(count, std::memory_order_relaxed);
    }
}

void SystemStats::increment_errors(size_t thread_idx, size_t count) {
    if (thread_idx < MAX_THREADS && thread_stats[thread_idx].active.load(std::memory_order_relaxed)) {
        thread_stats[thread_idx].errors.fetch_add(count, std::memory_order_relaxed);
    }
}

void SystemStats::add_bytes_processed(size_t thread_idx, size_t bytes) {
    if (thread_idx < MAX_THREADS && thread_stats[thread_idx].active.load(std::memory_order_relaxed)) {
        thread_stats[thread_idx].bytes_processed.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void SystemStats::print_stats_if_time(size_t thread_idx, seconds interval) {
    if (thread_idx >= MAX_THREADS || !thread_stats[thread_idx].active.load(std::memory_order_relaxed)) {
        return;
    }

    auto& stats = thread_stats[thread_idx];
    auto now = steady_clock::now();

    if (duration_cast<seconds>(now - stats.last_report) >= interval) {
        auto ops = stats.operations.load(std::memory_order_relaxed);
        auto errors = stats.errors.load(std::memory_order_relaxed);
        auto bytes = stats.bytes_processed.load(std::memory_order_relaxed);

        cout << stats.name << " Stats: Ops=" << ops
             << ", Errors=" << errors
             << ", MB=" << fixed << setprecision(2) << (bytes / 1024.0 / 1024.0) << endl;

        stats.operations.store(0, std::memory_order_relaxed);
        stats.errors.store(0, std::memory_order_relaxed);
        stats.bytes_processed.store(0, std::memory_order_relaxed);
        stats.last_report = now;
    }
}

std::vector<SystemStats::ThreadSnapshot> SystemStats::snapshot() const {
    std::vector<ThreadSnapshot> out;
    size_t count = thread_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count && i < MAX_THREADS; i++) {
        if (thread_stats[i].active.load(std::memory_order_relaxed)) {
            out.push_back({thread_stats[i].name,
                           thread_stats[i].operations.load(std::memory_order_relaxed),
                           thread_stats[i].errors.load(std::memory_order_relaxed),
                           thread_stats[i].bytes_processed.load(std::memory_order_relaxed)});
        }
    }
    return out;
}

void SystemStats::print_summary() {
    cout << "\n=== SYSTEM STATISTICS SUMMARY ===" << endl;
    size_t count = thread_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count && i < MAX_THREADS; i++) {
        if (thread_stats[i].active.load(std::memory_order_relaxed)) {
            const auto& stats = thread_stats[i];
            cout << stats.name << ": Ops=" << stats.operations.load(std::memory_order_relaxed)
                 << ", Errors=" << stats.errors.load(std::memory_order_relaxed)
                 << ", MB=" << fixed << setprecision(2)
                 << (stats.bytes_processed.load(std::memory_order_relaxed) / 1024.0 / 1024.0) << endl;
        }
    }
    cout << "================================\n" << endl;
}
