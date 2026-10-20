#include "ai_capture.hpp"

#include <algorithm>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <vector>

#include <liquid/liquid.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.hpp"
#include "decimator_manager.hpp"
#include "globals.hpp"
#include "playback.hpp"
#include "signal_processing/music_processor.hpp"
#include "utils/disk_util.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"

namespace aicap {

std::atomic<bool> g_active{false};

namespace {

constexpr size_t MAX_QUEUED = 64u << 20;       // bytes waiting for the worker (~2.7 s of 5 antennas)
constexpr uint64_t DISK_RESERVE = 200ull << 20;
constexpr double RETUNE_HZ = 2000;             // the header's float32 is ~64 Hz exact near 1 GHz

struct Chunk {
    std::vector<uint8_t> iq;       // count channels x n samples, channel-major (empty: skipped)
    uint32_t n = 0;
    uint32_t channels = 0;         // the packet's channel count
    bool skip = false;             // noise source / retuning
    float freq_hz = 0, gain_db = 0;   // of the selected channel
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Chunk> g_q;
size_t g_q_bytes = 0;
uint64_t g_dropped = 0;
std::atomic<bool> g_busy{false};
// what the tap copies: channels [first, first + count), sel = the one whose
// header frequency / gain is reported (set before g_active)
std::atomic<int> g_first{0}, g_count{1}, g_sel{0};

// ---- the decimator: the VFO decimator's cascade (SharedDecimator::buildStages),
// real taps normalised to unity gain, on liquid's firdecim_crcf
std::vector<int> stage_factors(int d) {
    std::vector<int> f;
    while (d > 1) {
        int p = 0;
        for (int k = 8; k >= 2; k--)
            if (d % k == 0) {
                p = k;
                break;
            }
        if (!p) p = d;
        f.push_back(p);
        d /= p;
    }
    return f;
}

struct Cascade {
    std::vector<firdecim_crcf> st;
    std::vector<int> fac;
    int D = 1;
    std::vector<std::complex<float>> pend, a, b;

    Cascade() = default;
    Cascade(const Cascade&) = delete;
    Cascade& operator=(const Cascade&) = delete;
    ~Cascade() {
        for (auto q : st) firdecim_crcf_destroy(q);
    }
    bool build(int d) {
        D = d;
        fac = stage_factors(d);
        const double fs = SAMPLE_RATE, pass = 0.4 * fs / d;
        double in = fs;
        for (int f : fac) {
            const double out = in / f, stop = out - pass;
            const float fc = static_cast<float>(0.5 * (pass + stop) / in), tr = static_cast<float>((stop - pass) / in);
            unsigned taps = std::clamp(static_cast<unsigned>(std::ceil(52.0f / (14.36f * tr))), 9u, 2047u);
            if (taps % 2 == 0) taps++;
            std::vector<float> h(taps);
            liquid_firdes_kaiser(taps, fc, 60.0f, 0.0f, h.data());
            double s = 0;
            for (float v : h) s += v;
            for (float& v : h) v = static_cast<float>(v / s);   // unity gain in the passband
            firdecim_crcf q = firdecim_crcf_create(f, h.data(), taps);
            if (!q) return false;
            st.push_back(q);
            in = out;
        }
        return true;
    }
    // x (mixed, input rate) -> out (appended)
    void run(const std::complex<float>* x, size_t n, std::vector<std::complex<float>>* out) {
        if (st.empty()) {
            out->insert(out->end(), x, x + n);
            return;
        }
        pend.insert(pend.end(), x, x + n);
        const size_t use = pend.size() / static_cast<size_t>(D) * static_cast<size_t>(D);
        if (!use) return;
        const std::complex<float>* src = pend.data();
        size_t cur = use;
        for (size_t s = 0; s < st.size(); s++) {
            const size_t on = cur / static_cast<size_t>(fac[s]);
            std::complex<float>* dst;
            if (s + 1 == st.size()) {
                const size_t base = out->size();
                out->resize(base + on);
                dst = out->data() + base;
            } else {
                auto& buf = (s % 2 == 0) ? a : b;
                buf.resize(on);
                dst = buf.data();
            }
            firdecim_crcf_execute_block(st[s], const_cast<liquid_float_complex*>(reinterpret_cast<const liquid_float_complex*>(src)),
                                        static_cast<unsigned>(on), reinterpret_cast<liquid_float_complex*>(dst));
            src = dst;
            cur = on;
        }
        pend.erase(pend.begin(), pend.begin() + static_cast<long>(use));
    }
};

// one antenna's path: uint8 -> float (DC tracked like the receiver's
// converter), mixed by -offset (phase exact per block), decimated
struct Path {
    Cascade c;
    bool dc_set = false;
    std::complex<float> dc{0, 0};
    std::vector<std::complex<float>> x, y;
};

float g_lut[256];

void convert(const uint8_t* in, uint32_t n, Path* p) {
    p->x.resize(n);
    std::complex<float>* x = p->x.data();
    float si = 0, sq = 0;
    for (uint32_t i = 0; i < n; i++) {
        const float re = g_lut[in[2 * i]], im = g_lut[in[2 * i + 1]];
        x[i] = {re, im};
        si += re;
        sq += im;
    }
    const std::complex<float> m(si / static_cast<float>(n), sq / static_cast<float>(n));
    if (!p->dc_set) {
        p->dc = m;
        p->dc_set = true;
    } else {
        p->dc += 0.05f * (m - p->dc);   // ~1 Hz corner, as the receiver's DC tracking
    }
    for (uint32_t i = 0; i < n; i++) x[i] -= p->dc;
}

void mix(Path* p, double step, uint64_t n0) {
    if (step == 0) return;
    // the phasor at this block's first sample from the exact phase, then a
    // recursion (renormalised) - no drift over a long capture
    const double ph0 = -2.0 * M_PI * std::fmod(step * static_cast<double>(n0), 1.0);
    std::complex<float> ph(static_cast<float>(std::cos(ph0)), static_cast<float>(std::sin(ph0)));
    const std::complex<float> inc(static_cast<float>(std::cos(-2.0 * M_PI * step)), static_cast<float>(std::sin(-2.0 * M_PI * step)));
    std::complex<float>* x = p->x.data();
    const size_t n = p->x.size();
    for (size_t i = 0; i < n; i++) {
        x[i] *= ph;
        ph *= inc;
        if ((i & 1023) == 1023) ph /= std::abs(ph);
    }
}

std::string fmtd(double v, int prec) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

// the array's geometry from any VFO's MUSIC (UCA radius for `decoder --uca`)
std::string array_json(int M, double* radius_m) {
    *radius_m = 0;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst || !inst->music_processor) continue;
        auto* mp = inst->music_processor.get();
        std::ostringstream o;
        const ArrayTopology t = mp->getArrayTopology();
        o << "{\"topology\":\""
          << (t == ArrayTopology::UCA ? "UCA" : t == ArrayTopology::ULA ? "ULA" : "CUSTOM") << "\"";
        if (t == ArrayTopology::UCA) {
            *radius_m = mp->getArrayRadius() / 1000.0;
            o << ",\"radius_m\":" << fmtd(*radius_m, 4);
        }
        std::vector<double> x, y, z;
        if (mp->nominalPositions(&x, &y, &z) && static_cast<int>(x.size()) == M) {
            o << ",\"positions_m\":[";
            for (int k = 0; k < M; k++)
                o << (k ? "," : "") << "[" << fmtd(x[static_cast<size_t>(k)], 4) << "," << fmtd(y[static_cast<size_t>(k)], 4) << ","
                  << fmtd(z[static_cast<size_t>(k)], 4) << "]";
            o << "]";
        }
        o << "}";
        return o.str();
    }
    return "null";
}

bool write_sidecar(const std::string& cf32, const Result& r, uint64_t samples, int channels, const std::string& array_info) {
    const std::string side = cf32.substr(0, cf32.size() - 5) + ".json";
    char utc[32];
    const time_t t = time(nullptr);
    strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    std::ostringstream o;
    o << "{\n \"rf_hz\": " << fmtd(r.center_hz + r.offset_hz, 1) << ",\n \"center_hz\": " << fmtd(r.center_hz, 1)
      << ",\n \"offset_hz\": " << fmtd(r.offset_hz, 1) << ",\n \"rate\": " << fmtd(r.rate_hz, 4)
      << ",\n \"bandwidth_hz\": " << fmtd(r.bandwidth_hz, 1) << ",\n \"gain_db\": " << fmtd(r.gain_db, 1)
      << ",\n \"captured_utc\": \"" << utc << "\",\n \"source\": \"" << json_escape(r.source) << "\",\n \"channel\": "
      << r.channel << ",\n \"calibration_gaps\": " << r.gaps << ",\n \"skipped_packets\": " << r.skipped
      << ",\n \"dropped_packets\": " << r.dropped << ",\n \"samples\": " << samples << ",\n \"format\": \"cf32\"";
    if (!r.stopped.empty()) o << ",\n \"stopped\": \"" << json_escape(r.stopped) << "\"";
    if (channels > 1)
        o << ",\n \"channels\": " << channels
          << ",\n \"layout\": \"sample-major: antenna 0 .. " << channels - 1 << " of sample 0, then sample 1 ...\""
          << ",\n \"array\": " << array_info;
    o << ",\n \"captured_by\": \"kraken_doa (AI assistant capture_iq)\"\n}\n";
    const std::string tmp = side + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");   // (b: no \r\n on Windows)
    if (!f) return false;
    const std::string s = o.str();
    const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    return ok && fs_replace(tmp, side) == 0;   // rename(); Windows: also over an older one
}

void clear_queue() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_q.clear();
    g_q_bytes = 0;
}

}  // namespace

void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, bool noise, bool retuning, const float* freq_hz,
               const float* gain_db) {
    if (!active()) return;
    const int first = g_first.load(std::memory_order_relaxed), count = g_count.load(std::memory_order_relaxed);
    const int sel = g_sel.load(std::memory_order_relaxed);
    Chunk c;
    c.n = samples;
    c.channels = channels;
    c.skip = noise || retuning;
    if (sel >= 0 && static_cast<uint32_t>(sel) < channels && sel < MAX_CHANNELS) {
        c.freq_hz = freq_hz[sel];
        c.gain_db = gain_db[sel];
    }
    const bool fits = first >= 0 && static_cast<uint32_t>(first + count) <= channels;
    if (!c.skip && fits) {
        const size_t per = static_cast<size_t>(samples) * 2;
        c.iq.assign(iq + static_cast<size_t>(first) * per, iq + static_cast<size_t>(first + count) * per);
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_q_bytes + c.iq.size() > MAX_QUEUED) {
            g_dropped++;
            c.iq.clear();
            c.skip = true;   // the worker fell behind: a gap
        }
        g_q_bytes += c.iq.size();
        g_q.push_back(std::move(c));
    }
    g_cv.notify_one();
}

Result run(const Request& req) {
    Result r;
    if (g_busy.exchange(true)) {
        r.error = "Another capture is running - wait for it";
        return r;
    }
    struct Busy {
        ~Busy() { g_busy.store(false); }
    } busy_guard;
    static std::once_flag lut_once;
    std::call_once(lut_once, [] {
        for (int i = 0; i < 256; i++) g_lut[i] = static_cast<float>(i) / 127.5f - 1.0f;
    });

    const double fs = SAMPLE_RATE;
    const int D = std::max(1, static_cast<int>(std::lround(fs / std::max(req.rate_hz, 1.0))));
    r.rate_hz = fs / D;
    r.bandwidth_hz = 0.8 * r.rate_hz;
    const int M = std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
    const bool multi = multi_tuner_mode();
    int sel = 0, first = 0, count = 1;
    if (multi) {
        if (req.all_antennas) {
            r.error = "all_antennas needs coherent mode (outside it every tuner is a receiver of its own)";
            return r;
        }
        // the tuner whose band holds the frequency (nearest centre)
        double best = 1e300;
        for (int ch = 0; ch < M; ch++) {
            const double c = static_cast<double>(exact_tuner_hz(ch));
            if (c > 0 && std::abs(req.rf_hz - c) < best) {
                best = std::abs(req.rf_hz - c);
                sel = ch;
            }
        }
        r.center_hz = static_cast<double>(exact_tuner_hz(sel));
        first = sel;
        r.source = "tuner " + std::to_string(sel);
    } else {
        sel = req.antenna >= 0 ? req.antenna : active_channel.load();
        if (sel < 0 || sel >= M) {
            r.error = "No antenna " + std::to_string(sel) + " (the array has " + std::to_string(M) + ")";
            return r;
        }
        r.center_hz = static_cast<double>(coherent_tuner_hz());
        if (req.all_antennas) {
            first = 0;
            count = M;
        } else {
            first = sel;
        }
        r.source = "antenna " + std::to_string(sel);
    }
    r.source += playback::active() ? " of a playback (" + playback::playing_path() + ") - not live"
                                   : " of heimdall's stream";
    r.channel = sel;
    r.antennas = count;
    if (r.center_hz <= 0) {
        r.error = "No data from the receiver yet";
        return r;
    }
    r.offset_hz = req.rf_hz - r.center_hz;
    if (std::abs(r.offset_hz) > fs / 2 - r.rate_hz / 2) {
        r.error = "The frequency is outside the band the receiver takes in (" + fmtd((r.center_hz - fs / 2) / 1e6, 4) + " - " +
                  fmtd((r.center_hz + fs / 2) / 1e6, 4) + " MHz at this rate) - tune there first";
        return r;
    }
    const double secs = std::clamp(req.seconds, 0.1, 600.0);
    const uint64_t want = static_cast<uint64_t>(std::llround(secs * r.rate_hz));
    // disk: the files + a reserve
    const std::string dir = req.path.substr(0, req.path.rfind('/') == std::string::npos ? 0 : req.path.rfind('/'));
    uint64_t free_b = 0, total_b = 0;
    const uint64_t need = want * 8 * static_cast<uint64_t>(1 + (count > 1 ? count : 0));
    if (diskutil::disk_space(dir.empty() ? "." : dir, &free_b, &total_b) && need + DISK_RESERVE > free_b) {
        r.error = "The capture needs " + std::to_string(need >> 20) + " MB, the disk has " + std::to_string(free_b >> 20) +
                  " MB free - fewer seconds or a lower rate";
        return r;
    }
    if (req.path.size() < 6 || req.path.compare(req.path.size() - 5, 5, ".cf32") != 0) {
        r.error = "bad file name";
        return r;
    }
    const std::string apath = req.path.substr(0, req.path.size() - 5) + "_array.cf32";
    FILE* f1 = fopen((req.path + ".part").c_str(), "wb");
    FILE* fa = count > 1 ? fopen((apath + ".part").c_str(), "wb") : nullptr;
    if (!f1 || (count > 1 && !fa)) {
        if (f1) fclose(f1);
        if (fa) fclose(fa);
        unlink((req.path + ".part").c_str());
        unlink((apath + ".part").c_str());
        r.error = "Cannot write in " + dir;
        return r;
    }
    std::vector<Path> paths(static_cast<size_t>(count));
    for (auto& p : paths)
        if (!p.c.build(D)) {
            fclose(f1);
            if (fa) fclose(fa);
            r.error = "decimator setup failed";
            return r;
        }
    const int sel_k = count > 1 ? sel : 0;   // the selected antenna among the paths
    const double step = r.offset_hz / fs;

    clear_queue();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_dropped = 0;
    }
    g_first.store(first);
    g_count.store(count);
    g_sel.store(sel);
    g_active.store(true, std::memory_order_release);

    using clk = std::chrono::steady_clock;
    const auto t_end = clk::now() + std::chrono::milliseconds(static_cast<int64_t>(secs * 1000) + 30000);
    auto t_data = clk::now();
    uint64_t got = 0, n_in = 0;
    bool in_gap = false, wrote_ok = true;
    double gain_sum = 0;
    uint64_t gain_n = 0;
    std::vector<std::complex<float>> inter;
    while (got < want) {
        Chunk c;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_q.empty(); });
            if (g_q.empty()) {
                lk.unlock();
                if (clk::now() - t_data > std::chrono::seconds(5)) {
                    r.stopped = "no data from the receiver for 5 s (heimdall stopped / reconnecting?)";
                    break;
                }
                if (clk::now() > t_end) {
                    r.stopped = "timed out (heimdall calibrating the whole time?)";
                    break;
                }
                continue;
            }
            c = std::move(g_q.front());
            g_q.pop_front();
            g_q_bytes -= c.iq.size();
        }
        t_data = clk::now();
        if (static_cast<int>(c.channels) < first + count) {
            r.stopped = "the antenna / tuner count changed";
            break;
        }
        if (!c.skip && std::abs(static_cast<double>(c.freq_hz) - r.center_hz) > RETUNE_HZ) {
            r.stopped = "the receiver retuned to " + fmtd(c.freq_hz / 1e6, 4) + " MHz";
            break;
        }
        if (c.skip || c.iq.empty()) {
            r.skipped++;
            if (got && !in_gap) r.gaps++;
            in_gap = true;
            if (clk::now() > t_end) {
                r.stopped = "timed out (heimdall calibrating)";
                break;
            }
            continue;
        }
        in_gap = false;
        gain_sum += c.gain_db;
        gain_n++;
        const size_t per = static_cast<size_t>(c.n) * 2;
        for (int k = 0; k < count; k++) {
            Path& p = paths[static_cast<size_t>(k)];
            convert(c.iq.data() + static_cast<size_t>(k) * per, c.n, &p);
            mix(&p, step, n_in);
            p.y.clear();
            p.c.run(p.x.data(), p.x.size(), &p.y);
        }
        n_in += c.n;
        size_t n_out = paths[static_cast<size_t>(sel_k)].y.size();
        n_out = static_cast<size_t>(std::min<uint64_t>(n_out, want - got));
        if (!n_out) continue;
        wrote_ok &= fwrite(paths[static_cast<size_t>(sel_k)].y.data(), sizeof(std::complex<float>), n_out, f1) == n_out;
        if (fa) {
            inter.resize(n_out * static_cast<size_t>(count));
            for (size_t i = 0; i < n_out; i++)
                for (int k = 0; k < count; k++) inter[i * static_cast<size_t>(count) + static_cast<size_t>(k)] = paths[static_cast<size_t>(k)].y[i];
            wrote_ok &= fwrite(inter.data(), sizeof(std::complex<float>), inter.size(), fa) == inter.size();
        }
        got += n_out;
        if (!wrote_ok) {
            r.stopped = "a write failed (disk full?)";
            break;
        }
    }
    g_active.store(false, std::memory_order_release);
    clear_queue();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        r.dropped = g_dropped;
    }
    const bool closed = fclose(f1) == 0 && (!fa || fclose(fa) == 0);
    if (!got || !closed) {
        unlink((req.path + ".part").c_str());
        unlink((apath + ".part").c_str());
        r.error = !got ? "Nothing captured: " + (r.stopped.empty() ? std::string("no data") : r.stopped) : "a write failed";
        return r;
    }
    r.seconds = static_cast<double>(got) / r.rate_hz;
    r.gain_db = gain_n ? gain_sum / static_cast<double>(gain_n) : NAN;
    fs_replace(req.path + ".part", req.path);
    write_sidecar(req.path, r, got, 1, "");
    r.path = req.path;
    if (fa) {
        double rad = 0;
        const std::string ai = array_json(count, &rad);
        r.radius_m = rad;
        Result ra = r;
        ra.channel = 0;
        ra.source = "every antenna (0 .. " + std::to_string(count - 1) + ")" +
                    (playback::active() ? std::string(" of a playback - not live") : std::string(" of heimdall's stream"));
        fs_replace(apath + ".part", apath);
        write_sidecar(apath, ra, got, count, ai);
        r.array_path = apath;
    }
    r.ok = true;
    return r;
}

}  // namespace aicap
