// ============================================
// src/playback.cpp - debug playback of the decoders' test transmissions
// ============================================
// See include/playback.hpp. The catalogue lives in the plugins
// (plugins/<id>/tools/testsignals.json, relayed to the page as text - the
// page parses it); the cache (testsignal_cache/, cwd-relative) holds what
// plugins/sdk/tools/make_testsignal.py built: <id>.dat + the flat sidecar
// <id>.json this file reads. Two formats:
//   cf32  every antenna sample by sample at a narrow rate R (2.4 MHz / R
//         whole), NO noise: interpolated to 2.4 MHz here (polyphase,
//         12 taps per phase, Kaiser 60 dB), shifted to the band's offset,
//         noise of the scene's SNR added over the whole 2.4 MHz, 8 bits
//   cu8   every antenna sample by sample at 2.4 MHz, as heimdall's bytes
// Both looped, paced to real time, as heimdall's 8091 packets: 16384
// samples per channel, phase state CONVERGED, coherent, no noise source.

#include "playback.hpp"

#include "config.hpp"
#include "control_handler.hpp"
#include "continuous_scanner.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "digital/dig_plugin.hpp"
#include "globals.hpp"
#include "networking/websocket_server.hpp"
#include "rdf_mapper.hpp"
#include "settings_store.hpp"
#include "station_info.hpp"
#include "utils/parse_num.hpp"
#include "utils/json_escape.hpp"
#include "utils/spawn_fds.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <signal.h>
#include <sstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utime.h>
#include <vector>

namespace playback {

std::atomic<bool> g_active{false};

namespace {

using cf = std::complex<float>;
using clk = std::chrono::steady_clock;

constexpr const char* CACHE_DIR = "testsignal_cache";
constexpr double FS = 2.4e6;
constexpr uint32_t NSAMP = 16384;
constexpr uint32_t PHASE_CONVERGED = 4;   // heimdall PhaseCompensatorState
constexpr float HEADER_GAIN_DB = 30.0f;
constexpr int TAPS = 12;                  // per polyphase branch

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return id[0] != '-';
}

std::string read_file(const std::string& path, size_t max = 1 << 20) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > max) s.clear();
    return s;
}

double num(const std::string& json, const char* key, double def = 0) {
    std::string v;
    if (!json_find(json, key, v)) return def;
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    return (end && *end == '\0' && is_finite_value(d)) ? d : def;
}

std::string str(const std::string& json, const char* key) {
    std::string v;
    return json_find(json, key, v) ? v : std::string();
}

double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

// --- the source -----------------------------------------------------------------
struct Source {
    std::string id, title, decoder;
    bool narrow = true;               // cf32 at `rate` (else cu8 at 2.4 MHz)
    int M = 5;
    double rate = FS;
    int L = 1;                        // 2.4 MHz / rate
    uint64_t tuner_hz = 0;
    double rf_hz = 0, band_off_hz = 0, radius_m = 0.12;
    float amp = 1, sigma = 0;         // output scale; noise rms per I / Q (output units)
    const uint8_t* map = nullptr;
    size_t map_len = 0;
    size_t N = 0;                     // samples per antenna in the file
    std::vector<float> hr;            // L x TAPS, each branch reversed
    std::vector<cf> rot;              // e^(j 2 pi f n / FS), one period
    std::vector<float> gauss;         // 65536 N(0, 1) values
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    uint64_t out_pos = 0;             // output samples produced (per antenna)
    clk::time_point t0{};
    uint64_t packets = 0;
    uint64_t fill_ns = 0, fill_n = 0;   // the cost of making the samples (logged)
    std::vector<cf> xin;
    std::vector<uint8_t> pkt;

    ~Source() {
        if (map) munmap(const_cast<uint8_t*>(map), map_len);
    }

    bool open(const std::string& ident, std::string* err) {
        const std::string side = read_file(std::string(CACHE_DIR) + "/" + ident + ".json");
        if (side.empty()) return *err = "not generated yet", false;
        id = ident;
        title = str(side, "title");
        decoder = str(side, "decoder");
        narrow = str(side, "format") == "cf32";
        M = static_cast<int>(num(side, "channels", 0));
        rate = num(side, "rate", 0);
        N = static_cast<size_t>(num(side, "samples", 0));
        rf_hz = num(side, "rf_hz", 0);
        tuner_hz = static_cast<uint64_t>(llround(num(side, "tuner_hz", 0)));
        band_off_hz = num(side, "band_offset_hz", 0);
        radius_m = num(side, "radius_m", 0.12);
        if (M < 2 || M > MAX_CHANNELS) return *err = "bad channel count", false;
        if (!(rate > 0) || N < 1000 || tuner_hz < 1000000) return *err = "bad sidecar", false;
        L = static_cast<int>(llround(FS / rate));
        if (L < 1 || std::fabs(L * rate - FS) > 1) return *err = "the rate must divide 2.4 MHz", false;
        if (!narrow && L != 1) return *err = "a cu8 test transmission must be at 2.4 MHz", false;
        const std::string path = std::string(CACHE_DIR) + "/" + ident + ".dat";
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return *err = "can't open " + path, false;
        struct stat st{};
        fstat(fd, &st);
        const size_t bps = static_cast<size_t>(M) * (narrow ? 8 : 2);
        if (static_cast<size_t>(st.st_size) < N * bps) {
            ::close(fd);
            return *err = "the file is shorter than its sidecar says", false;
        }
        map_len = N * bps;
        void* p = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return map = nullptr, *err = "mmap failed", false;
        map = static_cast<const uint8_t*>(p);
        madvise(p, map_len, MADV_SEQUENTIAL);
        if (narrow) setup_narrow(num(side, "noise_rms", 0));
        pkt.resize(32 + 8 * M + static_cast<size_t>(M) * NSAMP * 2);
        return true;
    }

    void setup_narrow(double noise_rms) {
        // prototype low-pass: 6 dB at the input's Nyquist, Kaiser 60 dB (beta
        // 5.65) over TAPS x L - passes 0.35 R, stops from 0.65 R
        const int K = L == 1 ? 1 : TAPS;
        const int n = K * L;
        std::vector<double> h(n);
        const double c = (n - 1) / 2.0, beta = 5.65;
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double x = (i - c) / L;   // in input samples
            const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            const double r = (i - c) / (c > 0 ? c : 1);
            const double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel_i0(beta);
            h[i] = L == 1 ? 1.0 : s * w;
            sum += h[i];
        }
        hr.assign(static_cast<size_t>(L) * K, 0.f);
        for (int p = 0; p < L; p++)
            for (int k = 0; k < K; k++) hr[static_cast<size_t>(p) * K + (K - 1 - k)] = static_cast<float>(h[p + k * L] * L / sum);
        // the band's place: rounded to 100 Hz -> a short phasor table
        const long long f = llround(band_off_hz / 100.0) * 100;
        const long long period = static_cast<long long>(FS) / std::gcd(std::llabs(f) ? std::llabs(f) : static_cast<long long>(FS),
                                                                      static_cast<long long>(FS));
        rot.resize(static_cast<size_t>(period));
        for (long long i = 0; i < period; i++) {
            const double ph = 2 * M_PI * static_cast<double>((f * i) % static_cast<long long>(FS)) / FS;
            rot[static_cast<size_t>(i)] = cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        }
        // levels as uca_scene.py --wide: the noise (scene SNR in its band,
        // the same density over 2.4 MHz), 8 bits with headroom
        const double sigma_w = noise_rms * std::sqrt(static_cast<double>(L));
        amp = static_cast<float>(0.25 / std::max(sigma_w * 3, 0.3));
        sigma = static_cast<float>(sigma_w * amp / std::sqrt(2.0));
        gauss.resize(65536);
        uint64_t s = 0x2545F4914F6CDD1Dull;
        for (size_t i = 0; i < gauss.size(); i += 2) {
            auto u = [&] {
                s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                return ((s * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
            };
            const double u1 = std::max(u(), 1e-300), u2 = u();
            const double r = std::sqrt(-2 * std::log(u1));
            gauss[i] = static_cast<float>(r * std::cos(2 * M_PI * u2));
            gauss[i + 1] = static_cast<float>(r * std::sin(2 * M_PI * u2));
        }
    }

    inline uint64_t next() {
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        return rng * 0x2545F4914F6CDD1Dull;
    }

    static inline uint8_t q8(float v) {
        const float x = v * 127.5f + 128.0f;   // +0.5 rounding folded in
        return static_cast<uint8_t>(x <= 0.f ? 0 : x >= 255.f ? 255 : static_cast<int>(x));
    }

    // one packet's samples of every antenna (channel-major) into out
    void fill(uint8_t* out) {
        if (!narrow) {
            const size_t step = static_cast<size_t>(M) * 2;
            for (int c = 0; c < M; c++) {
                uint8_t* o = out + static_cast<size_t>(c) * NSAMP * 2;
                size_t pos = out_pos % N;
                for (uint32_t i = 0; i < NSAMP; i++) {
                    const uint8_t* s = map + pos * step + static_cast<size_t>(c) * 2;
                    o[2 * i] = s[0];
                    o[2 * i + 1] = s[1];
                    if (++pos == N) pos = 0;
                }
            }
            out_pos += NSAMP;
            return;
        }
        const int K = L == 1 ? 1 : TAPS;   // == the taps hr holds per branch
        const uint64_t n0 = out_pos, n1 = out_pos + NSAMP - 1;
        const uint64_t m0 = n0 / L, m1 = n1 / L;
        const size_t nin = static_cast<size_t>(m1 - m0) + K;
        xin.resize(nin);
        const float* x = reinterpret_cast<const float*>(map);
        const size_t T = rot.size();
        for (int c = 0; c < M; c++) {
            // input m0 - (K - 1) .. m1 of this antenna, wrapped
            for (size_t j = 0; j < nin; j++) {
                const uint64_t m = m0 + j + N * 4 - (K - 1);
                const size_t idx = static_cast<size_t>(m % N);
                const float* s = x + (idx * M + c) * 2;
                xin[j] = cf(s[0], s[1]);
            }
            uint8_t* o = out + static_cast<size_t>(c) * NSAMP * 2;
            uint64_t n = n0;
            size_t p = static_cast<size_t>(n0 % L), mi = 0, ti = static_cast<size_t>(n0 % T);
            for (uint32_t i = 0; i < NSAMP; i++, n++) {
                const cf* xs = xin.data() + mi;
                float ar, ai;
                if (L == 1) {
                    ar = xs[0].real();
                    ai = xs[0].imag();
                } else {
                    const float* h = hr.data() + p * TAPS;
                    ar = 0, ai = 0;
                    for (int k = 0; k < TAPS; k++) {
                        ar += h[k] * xs[k].real();
                        ai += h[k] * xs[k].imag();
                    }
                }
                const cf r = rot[ti];
                float yr = (ar * r.real() - ai * r.imag()) * amp;
                float yi = (ar * r.imag() + ai * r.real()) * amp;
                if (sigma > 0) {
                    const uint64_t g = next();
                    yr += sigma * gauss[g & 0xFFFF];
                    yi += sigma * gauss[(g >> 16) & 0xFFFF];
                }
                o[2 * i] = q8(yr);
                o[2 * i + 1] = q8(yi);
                if (++p == static_cast<size_t>(L)) { p = 0; mi++; }
                if (++ti == T) ti = 0;
            }
        }
        out_pos += NSAMP;
    }

    // the next packet, paced
    ssize_t packet(uint8_t* buf, size_t cap) {
        const size_t len = pkt.size();
        if (cap < len) return 0;
        const auto now = clk::now();
        if (packets == 0) t0 = now;
        auto due = t0 + std::chrono::nanoseconds(static_cast<int64_t>(packets * (NSAMP * 1e9 / FS)));
        if (now - due > std::chrono::seconds(1)) {   // fell behind (CPU, debugger): don't burst
            t0 = now;
            packets = 0;
            due = now;
        }
        // sleep in short steps so a stop is seen
        while (clk::now() < due && g_active.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::min<clk::duration>(due - clk::now(), std::chrono::milliseconds(20)));
        auto be32 = [](uint8_t* p, uint32_t v) {
            p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
        };
        uint8_t* h = buf;
        be32(h, TCP_MAGIC);
        be32(h + 4, static_cast<uint32_t>(M));
        be32(h + 8, NSAMP);
        be32(h + 12, PHASE_CONVERGED);
        be32(h + 16, 0);   // noise source off
        be32(h + 20, 0);   // frequency change counter
        be32(h + 24, 0);   // scanner group
        be32(h + 28, 0);   // not retuning
        const float f = static_cast<float>(tuner_hz), g = HEADER_GAIN_DB;
        for (int c = 0; c < M; c++) {
            memcpy(h + 32 + 8 * c, &f, 4);       // little-endian floats (aarch64 / x86)
            memcpy(h + 32 + 8 * c + 4, &g, 4);
        }
        const auto f0 = clk::now();
        fill(h + 32 + 8 * M);
        fill_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - f0).count());
        if (++fill_n == 4096) {   // ~28 s
            const double per = fill_ns / 1e6 / fill_n;
            std::cout << "Playback " << id << ": " << std::fixed << std::setprecision(2) << per << " ms per packet = "
                      << std::setprecision(1) << per / (NSAMP * 1e3 / FS) * 100 << " % of a core" << std::defaultfloat << std::setprecision(6) << std::endl;
            fill_ns = fill_n = 0;
        }
        packets++;
        return static_cast<ssize_t>(len);
    }
};

std::mutex g_mu;                       // g_src, g_gen, g_notice
std::shared_ptr<Source> g_src;
std::string g_active_id, g_active_title;
std::atomic<bool> g_internal{false};
int64_t g_started_ms = 0;

struct Gen {
    std::string id;
    pid_t pid = -1;
    int pct = 0;
    std::string text, error, done_id;
    bool running = false;
};
Gen g_gen;
std::atomic<uint64_t> g_version{1};    // bumped on every change the page should see
std::string g_notice;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void bump() { g_version.fetch_add(1); }

void broadcast(const std::string& s) { WebSocketServer::broadcast_json_message(s); }

std::string notice_json(const std::string& text) {
    return "{\"playback\":{\"notice\":\"" + json_escape(text) + "\"}}";
}

// the catalogues as text (the page parses them) + the cache's state
std::string status_json() {
    std::ostringstream o;
    o << "{\"playback\":{\"active\":" << (g_active.load() ? "true" : "false");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        o << ",\"id\":\"" << json_escape(g_active_id) << "\",\"title\":\"" << json_escape(g_active_title) << "\"";
        if (g_src) {
            const double secs = static_cast<double>(g_src->N) / g_src->rate;
            const double pos = std::fmod(static_cast<double>(g_src->out_pos) / FS, secs);
            o << ",\"rf_hz\":" << g_src->tuner_hz << ",\"loop_s\":" << std::fixed << std::setprecision(2) << secs
              << ",\"pos_s\":" << pos << ",\"elapsed_s\":" << (now_ms() - g_started_ms) / 1000.0
              << ",\"decoder\":\"" << json_escape(g_src->decoder) << "\"" << std::defaultfloat << std::setprecision(6);
        }
        o << ",\"gen\":{\"id\":\"" << json_escape(g_gen.id) << "\",\"running\":" << (g_gen.running ? "true" : "false")
          << ",\"pct\":" << g_gen.pct << ",\"text\":\"" << json_escape(g_gen.text) << "\",\"error\":\""
          << json_escape(g_gen.error) << "\"}";
    }
    // catalogues
    o << ",\"catalog\":[";
    const std::string pdir = dig::PluginRegistry::dir();
    bool first = true;
    if (DIR* d = opendir(pdir.c_str())) {
        std::vector<std::string> ids;
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;
            ids.push_back(n);
        }
        closedir(d);
        std::sort(ids.begin(), ids.end());
        for (const auto& n : ids) {
            const std::string text = read_file(pdir + "/" + n + "/tools/testsignals.json", 256 << 10);
            if (text.empty()) continue;
            o << (first ? "" : ",") << "{\"plugin\":\"" << json_escape(n) << "\",\"text\":\"" << json_escape(text) << "\"}";
            first = false;
        }
    }
    o << "],\"cache\":{";
    first = true;
    if (DIR* d = opendir(CACHE_DIR)) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
            const std::string id = n.substr(0, n.size() - 5);
            if (!valid_id(id)) continue;
            struct stat st{};
            if (stat((std::string(CACHE_DIR) + "/" + id + ".dat").c_str(), &st) != 0) continue;
            const std::string side = read_file(std::string(CACHE_DIR) + "/" + n);
            o << (first ? "" : ",") << "\"" << id << "\":{\"mb\":" << std::fixed << std::setprecision(1)
              << st.st_size / 1e6 << std::defaultfloat << std::setprecision(6) << ",\"generated\":" << static_cast<long long>(num(side, "generated"))
              << ",\"seconds\":" << num(side, "seconds") << "}";
            first = false;
        }
        closedir(d);
    }
    o << "}}}";
    return o.str();
}

// --- generation (a child process) ----------------------------------------------
void gen_reader(int fd, pid_t pid, std::string id) {
    std::string buf;
    char tmp[512];
    std::string last_err;
    for (;;) {
        const ssize_t n = ::read(fd, tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            std::lock_guard<std::mutex> lk(g_mu);
            if (line.rfind("PROGRESS ", 0) == 0) {
                g_gen.pct = std::clamp(atoi(line.c_str() + 9), 0, 100);
                const size_t sp = line.find(' ', 9);
                g_gen.text = sp == std::string::npos ? "" : line.substr(sp + 1, 200);
            } else if (line.rfind("ERROR ", 0) == 0) {
                last_err = line.substr(6, 300);
            }
            bump();
        }
    }
    ::close(fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    std::lock_guard<std::mutex> lk(g_mu);
    g_gen.running = false;
    g_gen.pid = -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        g_gen.pct = 100;
        g_gen.error.clear();
        g_gen.done_id = id;
    } else {
        g_gen.error = !last_err.empty() ? last_err
                    : WIFSIGNALED(status) ? "stopped"
                    : WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "python3 (with numpy + scipy) is needed to generate test transmissions"
                    : "the generator failed (see " + std::string(CACHE_DIR) + "/" + id + ".log)";
    }
    std::cout << "Playback: generating " << id << (g_gen.error.empty() ? " done" : " failed: " + g_gen.error) << std::endl;
    bump();
}

std::string start_gen(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_gen.running) return "already generating " + g_gen.id;
    }
    if (mkdir(CACHE_DIR, 0755) != 0 && errno != EEXIST) return std::string("can't create ") + CACHE_DIR;
    const std::string tool = dig::PluginRegistry::dir() + "/sdk/tools/make_testsignal.py";
    if (access(tool.c_str(), R_OK) != 0) return "missing " + tool;
    const StationLocation st = station_info.resolve();
    char lat[32], lon[32];
    snprintf(lat, sizeof lat, "%.6f", st.lat);
    snprintf(lon, sizeof lon, "%.6f", st.lon);
    std::vector<std::string> args = {"python3", "-I", tool, id, "--plugins", dig::PluginRegistry::dir(), "--cache", CACHE_DIR,
                                     "--lat", lat, "--lon", lon};
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return "pipe failed";
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(pfd[0]);
        ::close(pfd[1]);
        return "fork failed";
    }
    if (pid == 0) {
        setpgid(0, 0);
        setpriority(PRIO_PROCESS, 0, 10);   // nice 10: the receiver comes first
        dup2(pfd[1], 1);
        const int dn = ::open("/dev/null", O_RDONLY);
        if (dn >= 0) dup2(dn, 0);
        spawn_fds::close_above_stdio_in_child();
        std::vector<char*> av;
        for (auto& a : args) av.push_back(a.data());
        av.push_back(nullptr);
        execvp("python3", av.data());
        _exit(127);
    }
    ::close(pfd[1]);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_gen = Gen{};
        g_gen.id = id;
        g_gen.pid = pid;
        g_gen.running = true;
        g_gen.text = "starting";
    }
    bump();
    std::cout << "Playback: generating " << id << " (pid " << pid << ")" << std::endl;
    std::thread(gen_reader, pfd[0], pid, id).detach();
    return "";
}

// --- start / stop ------------------------------------------------------------------
int bw_index_for(double khz) {
    int best = 0;
    double bd = 1e18;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        const double d = std::fabs(BANDWIDTH_OPTIONS[i].bandwidth_mhz * 1000.0 - khz);
        if (d < bd) bd = d, best = i;
    }
    return best;
}

void run_internal(const std::string& cmd) {
    g_internal.store(true);
    ControlHandler::handle_websocket_message(cmd);
    g_internal.store(false);
}

// the session (uWS loop): coherent, one VFO with the entry's decoder, DoA on
// the scene's array, the entry's extra commands - all into the sandbox
void apply_session(const std::string& id) {
    double radius_m;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_active.load() || g_active_id != id || !g_src) return;   // stopped / another one meanwhile
        radius_m = g_src->radius_m;
    }
    const std::string side = read_file(std::string(CACHE_DIR) + "/" + id + ".json");
    char dec[256];
    const double off_hz = num(side, "vfo_offset_khz", 0) * 1000.0;
    std::string demod = str(side, "vfo_demod");
    if (demod.empty()) demod = "NBFM";
    std::string mode = str(side, "vfo_mode");
    if (mode.find(',') != std::string::npos || mode.find(';') != std::string::npos) mode = "OFF";
    snprintf(dec, sizeof dec, "DECIMATORS:0|%.2f,%d,%s,0,-60.00,0,0,%s,,0,0", off_hz, bw_index_for(num(side, "vfo_bw_khz", 2400)),
             demod.c_str(), mode.c_str());
    run_internal("OPERATING_MODE:coherent");
    run_internal(dec);
    run_internal("DOA:1");
    run_internal("TOPOLOGY:UCA");
    run_internal("RADIUS:" + std::to_string(static_cast<int>(llround(radius_m * 1000))));
    std::istringstream extra(str(side, "commands"));
    for (std::string line; std::getline(extra, line);)
        if (!line.empty() && !line.starts_with("PLAYBACK_")) run_internal(line);
    rdfmap::request_full();
    std::cout << "Playback " << id << ": VFO 0 set up (" << dec << ")" << std::endl;
    bump();
}

std::string start(const std::string& id) {
    if (!valid_id(id)) return "bad id";
    auto src = std::make_shared<Source>();
    std::string err;
    if (!src->open(id, &err)) return err;
    utime((std::string(CACHE_DIR) + "/" + id + ".json").c_str(), nullptr);   // the cache keeps what is played
    const bool was = g_active.load();
    if (!was) {
        // the user's state: written now, then nothing until stop
        SettingsStore::begin_sandbox();
        rdfmap::set_sandbox(true);
        if (scanner_manager.isRunning()) scanner_manager.stop();
        if (continuous_scanner.isRunning()) continuous_scanner.stop();
    }
    // every VFO's decoder off first: their talkers (the user's, or the entry
    // played before) are forgotten, the session starts clean
    for (const auto& d : decimator_manager.getDecimatorInfoList())
        decimator_manager.setDigitalMode(d.id, dig::Mode::OFF, "");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_src = src;
        g_active_id = id;
        g_active_title = src->title;
        g_started_ms = now_ms();
    }
    g_active.store(true);
    std::cout << "Playback: " << id << " (" << src->M << " antennas, " << src->tuner_hz / 1e6 << " MHz, "
              << src->N / src->rate << " s loop" << (src->narrow ? ", interpolated x" + std::to_string(src->L) : "")
              << ") - heimdall disconnected, settings sandboxed" << std::endl;
    // the session a moment later: the decoders just switched off may still
    // report talkers from their queues - they are ignored while off
    std::thread([id] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (loop) loop->defer([id] { apply_session(id); });
    }).detach();
    rdfmap::request_full();
    bump();
    return "";
}

void stop() {
    if (!g_active.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_src.reset();
        g_active_id.clear();
        g_active_title.clear();
    }
    // the playback's decoders and their talkers go first (a VFO whose saved
    // mode is the same plugin would keep the playback's talkers otherwise)
    for (const auto& d : decimator_manager.getDecimatorInfoList())
        decimator_manager.setDigitalMode(d.id, dig::Mode::OFF, "");
    SettingsStore::end_sandbox();
    rdfmap::set_sandbox(false);
    std::cout << "Playback stopped - restoring the settings, heimdall reconnects" << std::endl;
    ControlHandler::apply_persisted_settings();
    rdfmap::request_full();
    bump();
}

}  // namespace

bool internal() { return g_internal.load(); }

uint64_t rf_hz() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_src ? g_src->tuner_hz : 0;
}

ssize_t read(uint8_t* buf, size_t cap) {
    std::shared_ptr<Source> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_src;
    }
    if (!s || !g_active.load(std::memory_order_relaxed)) return 0;
    return s->packet(buf, cap);
}

const char* refuse(std::string_view m, uint64_t freq_hz) {
    if (m.starts_with("FREQ:")) {
        const uint64_t rf = rf_hz();
        if (rf && (freq_hz > rf ? freq_hz - rf : rf - freq_hz) < 1000) return nullptr;   // where it is anyway
        return "Debug playback: the receiver frequency is the test transmission's - stop the playback to retune";
    }
    if (m.starts_with("GAIN:") || m.starts_with("TUNER_FREQ:") || m.starts_with("TUNER_GAIN:") || m.starts_with("TUNERS:"))
        return "Debug playback: tuner settings don't apply to a playback";
    if (m.starts_with("OPERATING_MODE:") && m != "OPERATING_MODE:coherent")
        return "Debug playback runs in coherent mode only";
    if (m.starts_with("WIDEBAND_MODE:1") || m.starts_with("WIDEBAND_MODE:true") || m.starts_with("WIDEBAND_FREQ:") ||
        m.starts_with("MIXER_SIDE:") || m.starts_with("LO_CURRENT:") || m.starts_with("FREQ_OFFSET:"))
        return "Debug playback: not available during a playback";
    if (m.starts_with("SCANNER_START") || m.starts_with("CONTINUOUS_SCANNER_START") || m.starts_with("SCANNER_NEXT"))
        return "Debug playback: the scanners need the real receiver";
    if (m == "RESET_SETTINGS") return "Debug playback: stop the playback before resetting the settings";
    if (m.starts_with("AI_INVESTIGATE")) return "Debug playback: the AI Signal Lab captures from heimdall - stop the playback first";
    return nullptr;
}

bool handle_command(std::string_view m) {
    if (!m.starts_with("PLAYBACK_")) return false;
    std::string err;
    if (m == "PLAYBACK_LIST") {
        bump();
    } else if (m.starts_with("PLAYBACK_GEN:")) {
        const std::string id(m.substr(13));
        err = valid_id(id) ? start_gen(id) : "bad id";
    } else if (m == "PLAYBACK_GEN_CANCEL") {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_gen.running && g_gen.pid > 0) kill(-g_gen.pid, SIGTERM);
    } else if (m.starts_with("PLAYBACK_START:")) {
        err = start(std::string(m.substr(15)));
    } else if (m == "PLAYBACK_STOP") {
        stop();
    } else if (m.starts_with("PLAYBACK_DELETE:")) {
        const std::string id(m.substr(16));
        std::lock_guard<std::mutex> lk(g_mu);
        if (!valid_id(id)) err = "bad id";
        else if (id == g_active_id) err = "playing - stop it first";
        else if (g_gen.running && g_gen.id == id) err = "being generated";
        else {
            for (const char* ext : {".dat", ".json", ".log"}) unlink((std::string(CACHE_DIR) + "/" + id + ext).c_str());
            bump();
        }
    } else {
        err = "unknown playback command";
    }
    if (!err.empty()) {
        std::cout << "Playback: " << std::string(m.substr(0, 80)) << " refused: " << err << std::endl;
        broadcast(notice_json(err));
    }
    return true;
}

std::string tick_message() {
    static uint64_t sent_ver = 0;
    static int64_t sent_ms = 0;
    const uint64_t v = g_version.load();
    bool busy;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        busy = g_gen.running;
    }
    const int64_t t = now_ms();
    if (v == sent_ver && !((g_active.load() || busy) && t - sent_ms >= 2000)) return "";
    sent_ver = v;
    sent_ms = t;
    return status_json();
}

}  // namespace playback
