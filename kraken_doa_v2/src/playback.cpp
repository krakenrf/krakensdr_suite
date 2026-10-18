// ============================================
// src/playback.cpp - playback through the real pipeline: the decoders' test
// transmissions (🧪 Debug Playback) and 📼 IQ recordings (SigMF)
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
//
// Recordings (RecSource): a SigMF recording / collection / archive
// (sigmf.hpp) from the recorder's folder - every channel memory-mapped in
// place (an archive's tar members too). cu8 at 2.4 MHz goes out as it is
// (the recorder's full band = heimdall's own bytes); any other complex
// datatype is scaled to the receiver's units (krakensdr:iq_scale, else the
// type's full scale; foreign data far off that range is brought into it),
// a rate of 2.4 MHz / L is interpolated like the test transmissions, a VFO-band
// recording is put back where its VFO was (krakensdr:band), with dither of
// ~0.6 LSB so a weak band survives the 8 bits. The packet header follows the
// capture segments (frequency, gain, heimdall's phase state, calibration
// bursts = the noise-source flag, so the DoA holds exactly as it did live).
// Pause, seek, loop. Its session: the recording's array settings
// (krakensdr:array_commands, else spatial:element_geometry as custom
// positions), its VFOs (krakensdr:vfos) when asked, else the user's; the
// station where the recording was made (StationInfo playback override, moved
// along krakensdr:track).

#include "ai_sandbox.hpp"
#include "playback.hpp"

#include "config.hpp"
#include "recorder.hpp"
#include "sigmf.hpp"
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

// the packet header (heimdall's 8091 format)
void write_header(uint8_t* h, int M, uint32_t phase, uint32_t noise, uint32_t counter, const float* freq, const float* gain) {
    auto be32 = [](uint8_t* p, uint32_t v) {
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
    };
    be32(h, TCP_MAGIC);
    be32(h + 4, static_cast<uint32_t>(M));
    be32(h + 8, NSAMP);
    be32(h + 12, phase);
    be32(h + 16, noise);
    be32(h + 20, counter);   // frequency change counter
    be32(h + 24, 0);         // scanner group
    be32(h + 28, 0);         // not retuning
    for (int c = 0; c < M; c++) {
        memcpy(h + 32 + 8 * c, &freq[c], 4);       // little-endian floats (aarch64 / x86)
        memcpy(h + 32 + 8 * c + 4, &gain[c], 4);
    }
}

// polyphase interpolation x L: prototype low-pass 6 dB at the input's
// Nyquist, Kaiser 60 dB (beta 5.65) over TAPS x L - passes 0.35 R, stops from
// 0.65 R. hr: L branches x TAPS, each reversed
void design_interp(int L, std::vector<float>* hr) {
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
    hr->assign(static_cast<size_t>(L) * K, 0.f);
    for (int p = 0; p < L; p++)
        for (int k = 0; k < K; k++) (*hr)[static_cast<size_t>(p) * K + (K - 1 - k)] = static_cast<float>(h[p + k * L] * L / sum);
}

void gauss_table(std::vector<float>* g) {
    g->resize(65536);
    uint64_t s = 0x2545F4914F6CDD1Dull;
    for (size_t i = 0; i < g->size(); i += 2) {
        auto u = [&] {
            s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
            return ((s * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
        };
        const double u1 = std::max(u(), 1e-300), u2 = u();
        const double r = std::sqrt(-2 * std::log(u1));
        (*g)[i] = static_cast<float>(r * std::cos(2 * M_PI * u2));
        (*g)[i + 1] = static_cast<float>(r * std::sin(2 * M_PI * u2));
    }
}

inline uint8_t q8(float v) {
    const float x = v * 127.5f + 128.0f;   // +0.5 rounding folded in
    return static_cast<uint8_t>(x <= 0.f ? 0 : x >= 255.f ? 255 : static_cast<int>(x));
}

// real-time pacing of the packets (a playback never bursts after a stall)
struct Pacer {
    clk::time_point t0{};
    uint64_t packets = 0;
    void reset() { packets = 0; }
    void wait() {
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
        packets++;
    }
};

// --- the test transmission source ------------------------------------------------------
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
        design_interp(L, &hr);
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
        gauss_table(&gauss);
    }

    inline uint64_t next() {
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        return rng * 0x2545F4914F6CDD1Dull;
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
    Pacer pace;
    ssize_t packet(uint8_t* buf, size_t cap) {
        const size_t len = pkt.size();
        if (cap < len) return 0;
        pace.wait();
        float f[MAX_CHANNELS], g[MAX_CHANNELS];
        for (int c = 0; c < M; c++) {
            f[c] = static_cast<float>(tuner_hz);
            g[c] = HEADER_GAIN_DB;
        }
        write_header(buf, M, PHASE_CONVERGED, 0, 0, f, g);
        const auto f0 = clk::now();
        fill(buf + 32 + 8 * M);
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

// --- a recording -------------------------------------------------------------------
struct RecSource {
    sigmf::Recording r;
    std::string path, name;
    int M = 0;
    int L = 1;                        // 2.4 MHz / the recording's rate
    bool fast = false;                // cu8 at 2.4 MHz: the bytes as they are
    uint64_t N = 0;                   // samples per antenna
    std::vector<const uint8_t*> chp;  // channel c: its first sample
    std::vector<size_t> stride;       // bytes from one sample of channel c to the next
    std::vector<std::pair<uint8_t*, size_t>> maps;
    double scale = 1;                 // the datatype -> the receiver's units (+ the level fix)
    float sigma = 0;                  // dither (output units)
    double shift_hz = 0;              // where the band goes in the 2.4 MHz (VFO-band recordings)
    std::vector<float> hr, gauss;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    std::atomic<uint64_t> out_pos{0}; // output samples (2.4 MHz) from the start
    std::atomic<bool> paused{false}, loop{true}, ended{false};
    std::atomic<int64_t> seek_to{-1}; // output sample to jump to (uWS loop -> receiver thread)
    Pacer pace;
    uint32_t counter = 0;
    float last_f = 0;
    std::atomic<uint64_t> cur_freq{0};// the header frequency now (Hz)
    std::atomic<int64_t> cur_t_ms{0}; // the recording's time at out_pos
    size_t track_i = 0;
    int64_t station_ms = 0;
    std::vector<cf> xin;
    std::vector<uint8_t> pkt;

    ~RecSource() {
        for (auto& m : maps) munmap(m.first, m.second);
    }

    double seconds() const { return static_cast<double>(N) / r.rate; }
    double pos_s() const { return static_cast<double>(out_pos.load()) / FS; }

    bool open(const std::string& p, std::string* err) {
        if (!sigmf::open_recording(p, &r, err)) return false;
        path = p;
        name = r.name;
        M = r.channels;
        if (M < 1 || M > MAX_CHANNELS) return *err = "the receiver takes 1 to " + std::to_string(MAX_CHANNELS) + " channels, this has " + std::to_string(M), false;
        L = static_cast<int>(llround(FS / r.rate));
        if (L < 1 || std::fabs(L * r.rate - FS) > 0.5 || L > 2400)
            return *err = "sample rate " + std::to_string(r.rate / 1e3) + " kHz: the receiver runs at 2.4 MHz - recordings at 2.4 MHz or 2.4 MHz / N (an integer) play", false;
        N = r.samples;
        if (N < 1000) return *err = "the recording is too short", false;
        // map every data file once (archives: one file for all)
        std::map<std::string, const uint8_t*> base;
        for (const auto& st : r.streams) {
            if (base.count(st.data_path)) continue;
            const int fd = ::open(st.data_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) return *err = "can't open " + st.data_path, false;
            struct stat sb{};
            fstat(fd, &sb);
            void* m = sb.st_size > 0 ? mmap(nullptr, static_cast<size_t>(sb.st_size), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
            ::close(fd);
            if (m == MAP_FAILED) return *err = "can't map " + st.data_path, false;
            madvise(m, static_cast<size_t>(sb.st_size), MADV_SEQUENTIAL);
            maps.emplace_back(static_cast<uint8_t*>(m), static_cast<size_t>(sb.st_size));
            base[st.data_path] = static_cast<const uint8_t*>(m);
        }
        for (const auto& st : r.streams)
            for (int j = 0; j < st.nch; j++) {
                chp.push_back(base[st.data_path] + st.data_offset + static_cast<size_t>(j) * st.dt.bytes());
                stride.push_back(static_cast<size_t>(st.dt.bytes()) * st.nch);
            }
        const sigmf::Datatype& dt = r.dt;
        // the datatype's full scale -> the receiver's (+-1)
        if (dt.kind == 'i' && dt.bits == 16 && r.streams[0].iq_scale > 0) scale = 1.0 / r.streams[0].iq_scale;
        else if (dt.kind == 'i') scale = 1.0 / std::ldexp(1.0, dt.bits - 1);
        else if (dt.kind == 'u') scale = 1.0 / (std::ldexp(1.0, dt.bits - 1) - 0.5);
        else scale = 1.0;
        fast = dt.kind == 'u' && dt.bits == 8 && L == 1;
        // data from elsewhere far off the receiver's range: brought into it
        if (r.format.empty() && !fast) {
            std::vector<float> mags;
            const uint64_t step = std::max<uint64_t>(1, N / 65536);
            for (uint64_t m = 0; m < N && mags.size() < 65536; m += step) {
                const cf x = raw(0, m);
                mags.push_back(std::max(std::fabs(x.real()), std::fabs(x.imag())));
            }
            std::sort(mags.begin(), mags.end());
            const float p = mags.empty() ? 0.0f : mags[mags.size() * 999 / 1000];
            if (p > 0 && (p > 0.95f || p < 0.02f)) scale *= 0.5 / p;
        }
        if (!fast) {
            design_interp(L, &hr);
            gauss_table(&gauss);
            sigma = 0.6f / 127.5f;   // ~0.6 LSB of dither per I / Q
        }
        if (r.format == "band") shift_hz = r.band_offset_hz;
        pkt.resize(32 + 8 * M + static_cast<size_t>(M) * NSAMP * 2);
        return true;
    }

    // channel c's sample m in the receiver's units
    cf raw(int c, uint64_t m) const {
        const uint8_t* p = chp[static_cast<size_t>(c)] + m * stride[static_cast<size_t>(c)];
        const sigmf::Datatype& dt = r.dt;
        double re = 0, im = 0;
        auto rd = [&](const uint8_t* q, int bytes) {
            uint64_t v = 0;
            if (dt.big) for (int i = 0; i < bytes; i++) v = (v << 8) | q[i];
            else for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | q[i];
            return v;
        };
        const int b = dt.bits / 8;
        if (dt.kind == 'f') {
            if (b == 4) {
                const uint32_t a = static_cast<uint32_t>(rd(p, 4)), d = static_cast<uint32_t>(rd(p + 4, 4));
                float fa, fd;
                memcpy(&fa, &a, 4);
                memcpy(&fd, &d, 4);
                re = fa, im = fd;
            } else {
                const uint64_t a = rd(p, 8), d = rd(p + 8, 8);
                double da, dd;
                memcpy(&da, &a, 8);
                memcpy(&dd, &d, 8);
                re = da, im = dd;
            }
            if (!is_finite_value(re)) re = 0;
            if (!is_finite_value(im)) im = 0;
        } else if (dt.kind == 'i') {
            auto sx = [&](uint64_t v) { return static_cast<double>(static_cast<int64_t>(v << (64 - dt.bits)) >> (64 - dt.bits)); };
            re = sx(rd(p, b));
            im = sx(rd(p + b, b));
        } else {
            const double mid = std::ldexp(1.0, dt.bits - 1) - 0.5;
            re = static_cast<double>(rd(p, b)) - mid;
            im = static_cast<double>(rd(p + b, b)) - mid;
        }
        return cf(static_cast<float>(re * scale), static_cast<float>(im * scale));
    }

    inline uint64_t next() {
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        return rng * 0x2545F4914F6CDD1Dull;
    }

    // input sample m (looped, or silence past the end)
    bool index(uint64_t& m) const {
        if (m < N) return true;
        if (!loop.load(std::memory_order_relaxed)) return false;
        m %= N;
        return true;
    }

    // the capture segment of stream s holding input sample m
    const sigmf::Capture& cap(size_t s, uint64_t m) const {
        const auto& caps = r.streams[s].captures;
        size_t lo = 0, hi = caps.size();
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (caps[mid].sample_start <= m) lo = mid;
            else hi = mid;
        }
        return caps[lo];
    }

    void fill(uint8_t* out, uint64_t n0) {
        if (fast) {
            for (int c = 0; c < M; c++) {
                uint8_t* o = out + static_cast<size_t>(c) * NSAMP * 2;
                const size_t sd = stride[static_cast<size_t>(c)];
                for (uint32_t i = 0; i < NSAMP;) {
                    uint64_t m = n0 + i;
                    if (!index(m)) {
                        memset(o + 2 * i, 128, (NSAMP - i) * 2);
                        break;
                    }
                    const uint32_t run = static_cast<uint32_t>(std::min<uint64_t>(NSAMP - i, N - m));
                    const uint8_t* p = chp[static_cast<size_t>(c)] + m * sd;
                    if (sd == 2) memcpy(o + 2 * i, p, run * 2);
                    else
                        for (uint32_t k = 0; k < run; k++) {
                            o[2 * (i + k)] = p[k * sd];
                            o[2 * (i + k) + 1] = p[k * sd + 1];
                        }
                    i += run;
                }
            }
            return;
        }
        const int K = L == 1 ? 1 : TAPS;
        const uint64_t n1 = n0 + NSAMP - 1;
        const uint64_t m0 = n0 / L, m1 = n1 / L;
        const size_t nin = static_cast<size_t>(m1 - m0) + K;
        xin.resize(nin);
        // the band's place: a phasor stepped in double precision
        const double w = 2 * M_PI * shift_hz / FS;
        for (int c = 0; c < M; c++) {
            for (size_t j = 0; j < nin; j++) {
                const int64_t mm = static_cast<int64_t>(m0 + j) - (K - 1);
                uint64_t m = mm < 0 ? (loop.load() ? static_cast<uint64_t>(mm + static_cast<int64_t>(N)) : N) : static_cast<uint64_t>(mm);
                xin[j] = index(m) ? raw(c, m) : cf(0, 0);
            }
            uint8_t* o = out + static_cast<size_t>(c) * NSAMP * 2;
            size_t p = static_cast<size_t>(n0 % L), mi = 0;
            double ph = std::fmod(w * static_cast<double>(n0), 2 * M_PI);
            std::complex<double> rot(std::cos(ph), std::sin(ph));
            const std::complex<double> step(std::cos(w), std::sin(w));
            for (uint32_t i = 0; i < NSAMP; i++) {
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
                float yr = ar, yi = ai;
                if (w != 0) {
                    yr = static_cast<float>(ar * rot.real() - ai * rot.imag());
                    yi = static_cast<float>(ar * rot.imag() + ai * rot.real());
                    rot *= step;
                }
                const uint64_t g = next();
                yr += sigma * gauss[g & 0xFFFF];
                yi += sigma * gauss[(g >> 16) & 0xFFFF];
                o[2 * i] = q8(yr);
                o[2 * i + 1] = q8(yi);
                if (++p == static_cast<size_t>(L)) { p = 0; mi++; }
            }
        }
    }

    // where the station was at recording time t (s): its track, else static
    void place_station(double t, bool force) {
        if (!r.station_valid && r.track.empty()) return;
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count();
        if (!force && now - station_ms < 1000) return;
        station_ms = now;
        if (r.track.empty()) {
            station_info.setPlaybackOverride(r.station_lat, r.station_lon, r.station_heading);
            return;
        }
        if (force || track_i >= r.track.size() || r.track[track_i][0] > t) track_i = 0;
        while (track_i + 1 < r.track.size() && r.track[track_i + 1][0] <= t) track_i++;
        const auto& a = r.track[track_i];
        double speed = 0;
        if (track_i + 1 < r.track.size()) {
            const auto& b = r.track[track_i + 1];
            const double dt = b[0] - a[0];
            if (dt > 0)
                speed = std::hypot((b[1] - a[1]) * 111320.0, (b[2] - a[2]) * 111320.0 * std::cos(a[1] * M_PI / 180)) / dt;
        }
        station_info.setPlaybackOverride(a[1], a[2], a[3], true, speed);
    }

    ssize_t packet(uint8_t* buf, size_t cap_bytes) {
        const size_t len = pkt.size();
        if (cap_bytes < len) return 0;
        const int64_t sk = seek_to.exchange(-1);
        if (sk >= 0) {
            out_pos.store(static_cast<uint64_t>(sk));
            ended.store(false);
            pace.reset();
            place_station(static_cast<double>(sk) / FS, true);
        }
        uint64_t pos = out_pos.load();
        if (pos >= N * static_cast<uint64_t>(L)) {
            if (loop.load()) {
                pos %= N * static_cast<uint64_t>(L);
                out_pos.store(pos);
            } else {
                ended.store(true);
            }
        }
        if (paused.load() || ended.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            pace.reset();
            return 0;
        }
        pace.wait();
        const uint64_t m = pos / L;   // input sample at the packet's start
        const sigmf::Capture& c0 = cap(0, std::min<uint64_t>(m, N - 1));
        float f[MAX_CHANNELS], g[MAX_CHANNELS];
        const double tuner = c0.freq_hz - shift_hz;
        int c = 0;
        for (size_t s = 0; s < r.streams.size(); s++) {
            const sigmf::Capture& cs = cap(s, std::min<uint64_t>(m, N - 1));
            for (int j = 0; j < r.streams[s].nch && c < M; j++, c++) {
                f[c] = static_cast<float>(tuner);
                g[c] = cs.gain_db > -900 ? static_cast<float>(cs.gain_db) : HEADER_GAIN_DB;
            }
        }
        if (f[0] != last_f) {
            if (last_f != 0) counter++;
            last_f = f[0];
            requested_freq_hz.store(static_cast<uint64_t>(llround(tuner)), std::memory_order_relaxed);
        }
        cur_freq.store(static_cast<uint64_t>(llround(tuner)));
        cur_t_ms.store(c0.t_ms ? c0.t_ms + static_cast<int64_t>((m - c0.sample_start) * 1000.0 / r.rate) : 0);
        const uint32_t phase = c0.phase_state >= 0 ? static_cast<uint32_t>(c0.phase_state) & 0xFF : PHASE_CONVERGED;
        write_header(buf, M, phase, c0.calibration ? 1 : 0, counter, f, g);
        fill(buf + 32 + 8 * M, pos);
        out_pos.store(pos + NSAMP);
        place_station(static_cast<double>(m) / r.rate, false);
        return static_cast<ssize_t>(len);
    }
};

std::mutex g_mu;                       // g_src, g_rec, g_gen, g_notice
std::shared_ptr<Source> g_src;         // a test transmission plays, or
std::shared_ptr<RecSource> g_rec;      // a recording
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
        o << ",\"kind\":\"" << (g_rec ? "rec" : g_src ? "test" : "") << "\"";
        o << ",\"id\":\"" << json_escape(g_active_id) << "\",\"title\":\"" << json_escape(g_active_title) << "\"";
        if (g_rec) {
            const RecSource& r = *g_rec;
            o << ",\"rf_hz\":" << r.cur_freq.load() << ",\"elapsed_s\":" << std::fixed << std::setprecision(2)
              << (now_ms() - g_started_ms) / 1000.0 << ",\"rec\":{\"name\":\"" << json_escape(r.name) << "\",\"channels\":" << r.M
              << ",\"rate\":" << r.r.rate << ",\"datatype\":\"" << json_escape(r.r.datatype) << "\",\"format\":\""
              << json_escape(r.r.format) << "\",\"dur_s\":" << r.seconds() << ",\"pos_s\":" << r.pos_s()
              << ",\"paused\":" << (r.paused.load() ? "true" : "false") << ",\"loop\":" << (r.loop.load() ? "true" : "false")
              << ",\"ended\":" << (r.ended.load() ? "true" : "false") << ",\"t_ms\":" << r.cur_t_ms.load()
              << ",\"start_ms\":" << r.r.start_ms << ",\"station\":" << (r.r.station_valid || !r.r.track.empty() ? "true" : "false")
              << ",\"moving\":" << (r.r.track.empty() ? "false" : "true") << ",\"vfos\":" << (r.r.decimators.empty() ? "false" : "true")
              << ",\"description\":\"" << json_escape(r.r.description) << "\"}" << std::defaultfloat << std::setprecision(6);
        }
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
    // the generators are the plugins' own code (plugins/<id>/tools/), which
    // the AI assistant may have written: sandboxed when bwrap is there
    // (only the cache writable), refused without it while any plugin is one
    // the assistant changed (ai_sandbox.hpp)
    if (aisandbox::available()) {
        args = aisandbox::wrap(args, {CACHE_DIR});
    } else if (aisandbox::any_marked()) {
        return "the AI assistant changed plugins here: test signals are generated only in a sandbox - install bubblewrap (bwrap)";
    }
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
        execvp(av[0], av.data());
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

// a recording's session (uWS loop): coherent, the user's VFOs (or the
// recording's), the recording's array, DoA on, the station where it was made
void apply_rec_session(std::shared_ptr<RecSource> src, bool use_vfos) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_active.load() || g_rec != src) return;   // stopped / another one meanwhile
    }
    const sigmf::Recording& r = src->r;
    run_internal("OPERATING_MODE:coherent");
    std::string dec;
    if (use_vfos && r.decimators.rfind("DECIMATORS:", 0) == 0) dec = r.decimators;
    else
        for (const auto& c : SettingsStore::apply_commands())
            if (c.rfind("DECIMATORS:", 0) == 0) dec = c;
    if (!dec.empty()) run_internal(dec);
    const char* array = "no array settings in it - yours stay";
    if (!r.commands.empty()) {
        array = "its array settings";
        for (const auto& c : r.commands) {
            static const char* ok[] = {"TOPOLOGY:", "CUSTOM_POSITIONS:", "ARRAY_LAYOUT:", "RADIUS:", "SPACING:", "ULA_MODE:",
                                       "CUSTOM_MODE:", "ARRAY_OFFSET:"};
            for (const char* k : ok)
                if (c.rfind(k, 0) == 0 && c.size() < 2000 && c.find('\n') == std::string::npos) run_internal(c);
        }
    } else if (static_cast<int>(r.geometry.size()) == src->M && src->M >= 2) {
        // a recording from elsewhere: its element positions as a custom array
        std::ostringstream pos;
        for (size_t k = 0; k < r.geometry.size(); k++)
            pos << (k ? ";" : "") << std::fixed << std::setprecision(2) << r.geometry[k][0] * 1000 << "," << r.geometry[k][1] * 1000
                << "," << r.geometry[k][2] * 1000;
        run_internal("CUSTOM_POSITIONS:" + pos.str());
        run_internal("TOPOLOGY:CUSTOM");
        array = "its element positions as a custom array";
    }
    if (src->M >= 2) run_internal("DOA:1");
    src->place_station(src->pos_s(), true);
    rdfmap::request_full();
    std::cout << "Playback " << src->name << ": session set up (" << (use_vfos && !r.decimators.empty() ? "the recording's" : "your")
              << " VFOs, " << array << ")" << std::endl;
    bump();
}

// the user's state is sandboxed once, at the first playback of a run
void enter_sandbox() {
    SettingsStore::begin_sandbox();
    rdfmap::set_sandbox(true);
    if (scanner_manager.isRunning()) scanner_manager.stop();
    if (continuous_scanner.isRunning()) continuous_scanner.stop();
}

std::string start_rec(const std::string& name, bool use_vfos, bool loop_on) {
    if (rec::iq_recording()) return "an IQ recording is running - stop it first";
    const std::string path = rec::item_path(name);
    if (path.empty()) return "no recording " + name;
    auto src = std::make_shared<RecSource>();
    std::string err;
    if (!src->open(path, &err)) return err;
    src->loop.store(loop_on);
    const bool was = g_active.load();
    if (!was) enter_sandbox();
    for (const auto& d : decimator_manager.getDecimatorInfoList())
        decimator_manager.setDigitalMode(d.id, dig::Mode::OFF, "");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_src.reset();
        g_rec = src;
        g_active_id = name;
        g_active_title = name;
        g_started_ms = now_ms();
    }
    g_active.store(true);
    std::cout << "Playback: recording " << name << " (" << src->M << (src->M == 1 ? " channel, " : " channels, ") << src->r.datatype << " at "
              << src->r.rate / 1e3 << " kHz, " << src->seconds() << " s" << (src->L > 1 ? ", interpolated x" + std::to_string(src->L) : "")
              << ") - heimdall disconnected, settings sandboxed" << std::endl;
    std::thread([src, use_vfos] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        if (loop) loop->defer([src, use_vfos] { apply_rec_session(src, use_vfos); });
    }).detach();
    rdfmap::request_full();
    bump();
    return "";
}

std::string start(const std::string& id) {
    if (!valid_id(id)) return "bad id";
    if (rec::iq_recording()) return "an IQ recording is running - stop it first";
    auto src = std::make_shared<Source>();
    std::string err;
    if (!src->open(id, &err)) return err;
    utime((std::string(CACHE_DIR) + "/" + id + ".json").c_str(), nullptr);   // the cache keeps what is played
    const bool was = g_active.load();
    if (!was) enter_sandbox();   // the user's state: written now, then nothing until stop
    // every VFO's decoder off first: their talkers (the user's, or the entry
    // played before) are forgotten, the session starts clean
    for (const auto& d : decimator_manager.getDecimatorInfoList())
        decimator_manager.setDigitalMode(d.id, dig::Mode::OFF, "");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_rec.reset();
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
    station_info.clearPlaybackOverride();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_src.reset();
        g_rec.reset();
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
    return g_src ? g_src->tuner_hz : g_rec ? g_rec->cur_freq.load() : 0;
}

std::string playing_path() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_rec ? g_rec->path : "";
}

ssize_t read(uint8_t* buf, size_t cap) {
    std::shared_ptr<Source> s;
    std::shared_ptr<RecSource> r;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_src;
        r = g_rec;
    }
    if (!g_active.load(std::memory_order_relaxed)) return 0;
    if (r) return r->packet(buf, cap);
    if (s) return s->packet(buf, cap);
    return 0;
}

const char* refuse(std::string_view m, uint64_t freq_hz) {
    if (m.starts_with("FREQ:")) {
        const uint64_t rf = rf_hz();
        if (rf && (freq_hz > rf ? freq_hz - rf : rf - freq_hz) < 1000) return nullptr;   // where it is anyway
        return "Playback: the receiver frequency is the playback's - stop the playback to retune";
    }
    if (m.starts_with("GAIN:") || m.starts_with("TUNER_FREQ:") || m.starts_with("TUNER_GAIN:") || m.starts_with("TUNERS:"))
        return "Playback: tuner settings don't apply to a playback";
    if (m.starts_with("OPERATING_MODE:") && m != "OPERATING_MODE:coherent")
        return "Playback runs in coherent mode only";
    if (m.starts_with("WIDEBAND_MODE:1") || m.starts_with("WIDEBAND_MODE:true") || m.starts_with("WIDEBAND_FREQ:") ||
        m.starts_with("MIXER_SIDE:") || m.starts_with("LO_CURRENT:") || m.starts_with("FREQ_OFFSET:"))
        return "Playback: not available during a playback";
    if (m.starts_with("SCANNER_START") || m.starts_with("CONTINUOUS_SCANNER_START") || m.starts_with("SCANNER_NEXT"))
        return "Playback: the scanners need the real receiver";
    if (m == "RESET_SETTINGS") return "Playback: stop the playback before resetting the settings";
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
    } else if (m.starts_with("PLAYBACK_OPEN:")) {
        // PLAYBACK_OPEN:<name>[:<its VFOs 0|1>[:<loop 0|1>]]
        std::string rest(m.substr(14));
        bool vfos = false, loop_on = true;
        const size_t c1 = rest.find(':');
        if (c1 != std::string::npos) {
            const std::string flags = rest.substr(c1 + 1);
            rest.resize(c1);
            vfos = !flags.empty() && flags[0] == '1';
            const size_t c2 = flags.find(':');
            if (c2 != std::string::npos) loop_on = flags.substr(c2 + 1) != "0";
        }
        err = start_rec(rest, vfos, loop_on);
    } else if (m.starts_with("PLAYBACK_PAUSE:") || m.starts_with("PLAYBACK_LOOP:") || m.starts_with("PLAYBACK_SEEK:")) {
        std::shared_ptr<RecSource> r;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            r = g_rec;
        }
        if (!r) {
            err = "no recording is playing";
        } else if (m.starts_with("PLAYBACK_PAUSE:")) {
            r->paused.store(m.substr(15) == "1");
        } else if (m.starts_with("PLAYBACK_LOOP:")) {
            r->loop.store(m.substr(14) == "1");
            if (r->loop.load()) r->ended.store(false);
        } else {
            const double sec = atof(std::string(m.substr(14)).c_str());
            if (!(sec >= 0) || sec > r->seconds()) err = "position outside the recording";
            else r->seek_to.store(static_cast<int64_t>(sec * FS) / NSAMP * NSAMP);
        }
        bump();
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
