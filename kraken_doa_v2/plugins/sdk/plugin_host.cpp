// main() of every decoder plugin executable (linked with the plugin's own
// sources by plugins/Makefile). Three modes:
//
//   decoder --info                 plugin description as JSON (kraken_doa
//                                  reads it when it scans plugins/)
//   decoder --serve                live: kraken_doa streams samples over
//                                  stdin, results go back over stdout
//                                  (binary protocol, kraken_plugin.hpp)
//   decoder --file REC [options]   offline test on a recording - prints the
//                                  events, a frame count and the final facts
//
// Offline options:
//   --rate HZ        sample rate of the file (default: REC.json "rate" from
//                    ai/sigtool.py captures, or the WAV header)
//   --format F       cf32 | cu8 | cs16 | wav (default: from the extension)
//   --offset HZ      the signal sits HZ away from the file's centre
//   --start S        skip S seconds;  --seconds S  decode only S seconds
//   --verbose        verbose option on;  --invert  conjugate (swap I/Q)
//   --opt key=value  pass an option to the decoder (repeatable)
//   --audio OUT.wav  write the decoded 8 kHz audio
//   --channels M     a recording of every antenna of an array, sample by
//                    sample (default: REC.json "channels"): decoded with
//                    process_multi (Info::multichannel); --channel K: only
//                    antenna K, as a single-channel decoder gets it
//   --station LAT,LON  the receiver's location (Host::station)
//   --rf HZ          RF of the file's centre (Host::rf_hz; default: the
//                    REC.json "rf_hz" of ai/sigtool.py captures)
//   --raw            raw frames on (Host::raw_wanted) and printed
//   --quiet          only the summary
//   --uca R          with --channels M: the antennas form kraken_doa's UCA of
//                    radius R metres (clockwise, ANT0 on +x); antenna 0 is
//                    decoded (multichannel plugins: every antenna) and the
//                    summary gives each TALKER's bearing from its own samples
//                    of every antenna, as kraken_doa's DoA per talker
//                    computes it (needs --rf) - the ID-separation test
// Array plugins (Info::array) read a MULTICHANNEL recording: every sample
// holds the antennas one after the other (cf32 / cs16 / cu8 pairs):
//   --channels M     antennas (default: REC.json "channels")
//   --all            every sample (default: bursts of Info::array_burst_s
//                    every array_period_s, like live)
//   --uca R          the antennas form kraken_doa's UCA of radius R metres
//                    (clockwise, ANT0 on +x): the summary gives each spatial
//                    report's bearing (MUSIC, one source; needs --rf)

#include "kraken_plugin.hpp"
#include "kraken_dsp.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#include <liquid/liquid.h>
#include <unistd.h>

namespace {

std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

int print_info() {
    kp::Info i = kp_plugin_info();
    std::cout << "{\"api\":1,\"id\":\"" << jesc(i.id) << "\",\"name\":\"" << jesc(i.name)
              << "\",\"description\":\"" << jesc(i.description) << "\",\"version\":\"" << jesc(i.version)
              << "\",\"author\":\"" << jesc(i.author) << "\",\"sample_rate\":" << i.sample_rate
              << ",\"min_vfo_rate\":" << i.min_vfo_rate << ",\"map\":" << (i.map ? "true" : "false")
              << ",\"manual_only\":" << (i.manual_only ? "true" : "false") << ",\"fixed_freq_hz\":" << std::fixed
              << std::setprecision(0) << i.fixed_freq_hz << std::defaultfloat << std::setprecision(6)
              << ",\"voice\":" << (i.voice ? "true" : "false") << ",\"messages\":" << (i.messages ? "true" : "false")
              << ",\"talkers\":" << (i.talkers ? "true" : "false") << ",\"lock_vfo\":" << (i.lock_vfo ? "true" : "false")
              << ",\"multichannel\":" << (i.multichannel ? "true" : "false") << ",\"array\":" << (i.array ? "true" : "false")
              << ",\"array_burst_s\":" << i.array_burst_s << ",\"array_period_s\":" << i.array_period_s
              << ",\"tunes\":" << (i.tunes ? "true" : "false") << ",\"squelch_hold_s\":" << i.squelch_hold_s;
    // options twice: as JSON for people, and tab-separated for kraken_doa's
    // flat reader (key, label, default, choices, help per line)
    std::string tsv;
    std::cout << ",\"options\":[";
    for (size_t k = 0; k < i.options.size(); k++) {
        const kp::Option& o = i.options[k];
        std::cout << (k ? "," : "") << "{\"key\":\"" << jesc(o.key) << "\",\"label\":\"" << jesc(o.label)
                  << "\",\"default\":\"" << jesc(o.def) << "\",\"choices\":\"" << jesc(o.choices)
                  << "\",\"help\":\"" << jesc(o.help) << "\"}";
        for (const char* f : {o.key, o.label, o.def, o.choices, o.help}) {
            for (const char* c = f; *c; c++) tsv += (*c == '\t' || *c == '\n') ? ' ' : *c;
            tsv += f == o.help ? '\n' : '\t';
        }
    }
    std::cout << "],\"options_tsv\":\"" << jesc(tsv) << "\"}" << std::endl;
    return 0;
}

// decimal text for the wire, "" = NAN
std::string wnum(double v, int prec = 6) {
    if (!std::isfinite(v)) return "";
    char b[48];
    snprintf(b, sizeof b, "%.*g", prec, v);
    return b;
}

// SPATIAL / PLOT payloads (kraken_plugin.hpp kp::wire)
std::string spatial_payload(const kp::Spatial& s, double start, double end) {
    std::string p = s.id.substr(0, 32);
    for (const std::string& f : {s.label.substr(0, 64), wnum(start, 12), wnum(end, 12), wnum(s.delay_us), wnum(s.power_db),
                                 wnum(s.snr_db), std::string(s.ref ? "1" : "0"), wnum(s.avg_s), std::to_string(s.m)}) {
        p.push_back('\0');
        p += f;
    }
    p.push_back('\0');
    for (size_t i = 0; i < s.cov.size(); i++) {
        if (i) p.push_back(',');
        p += wnum(s.cov[i].real(), 7) + "," + wnum(s.cov[i].imag(), 7);
    }
    return p;
}
std::string plot_payload(const kp::Plot& pl) {
    std::string p = pl.title.substr(0, 80);
    for (const std::string& f : {pl.x_unit.substr(0, 16), pl.y_unit.substr(0, 16), wnum(pl.x0, 9), wnum(pl.dx, 9),
                                 wnum(pl.y_min), wnum(pl.y_max), wnum(pl.threshold)}) {
        p.push_back('\0');
        p += f;
    }
    p.push_back('\0');
    for (size_t i = 0; i < pl.y.size() && i < 1000; i++) {
        if (i) p.push_back(',');
        p += wnum(pl.y[i], 4);
    }
    for (size_t i = 0; i < pl.markers.size() && i < 64; i++) {
        const kp::PlotMarker& m = pl.markers[i];
        for (const std::string& f : {wnum(m.x, 9), m.label.substr(0, 16), m.talker.substr(0, 32)}) {
            p.push_back('\0');
            p += f;
        }
    }
    return p;
}

// --------------------------------------------------------------------------
// Live mode
// --------------------------------------------------------------------------
class WireHost : public kp::Host {
public:
    WireHost(int out_fd, double rate) : fd_(out_fd), rate_(rate) {}
    void fact(const std::string& k, const std::string& v) override {
        std::string p = k;
        p.push_back('\0');
        p += v;
        send(kp::wire::FACT, p.data(), p.size());
    }
    void event(const std::string& text, double dedup_s) override {
        std::string p(4, '\0');
        float d = static_cast<float>(dedup_s);
        memcpy(p.data(), &d, 4);
        p += text;
        send(kp::wire::EVENT, p.data(), p.size());
    }
    void valid() override { send(kp::wire::VALID, nullptr, 0); }
    void valid(double start_s, double end_s) override {
        if (!std::isfinite(start_s) || !std::isfinite(end_s) || end_s < start_s) {
            valid();
            return;
        }
        std::string p = secs(start_s);
        p.push_back('\0');
        p += secs(end_s);
        send(kp::wire::VALID, p.data(), p.size());
    }
    void audio(const float* pcm, size_t n) override {
        while (n > 0) {
            size_t k = std::min<size_t>(n, 8000);
            send(kp::wire::AUDIO, pcm, k * sizeof(float));
            pcm += k;
            n -= k;
        }
    }
    void audio_channel(int channel) override {
        if (channel == audio_channel_) return;   // sent when it changes
        audio_channel_ = channel;
        const int32_t c = channel;
        send(kp::wire::AUDIO_CHANNEL, &c, 4);
    }
    void voice_state(const std::string& s) override { send(kp::wire::VOICE_STATE, s.data(), s.size()); }
    void freq_error(float hz) override { send(kp::wire::FREQ_ERROR, &hz, 4); }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return voice_wanted_; }
    double time() const override { return array_t_ >= 0 ? array_t_ : samples_ / rate_; }
    void log(const std::string& t) override { std::cerr << t << std::endl; }
    void spatial(const kp::Spatial& sp) override {
        if (sp.id.empty() || sp.m < 1 || sp.m > 8 || sp.cov.size() != static_cast<size_t>(sp.m * sp.m)) return;
        const std::string p = spatial_payload(sp, std::isfinite(sp.start_s) ? sp.start_s : time(),
                                              std::isfinite(sp.end_s) ? sp.end_s : time());
        send(kp::wire::SPATIAL, p.data(), p.size());
    }
    void spatial_remove(const std::string& id) override { send(kp::wire::SPATIAL_REMOVE, id.data(), std::min<size_t>(id.size(), 32)); }
    void plot(const kp::Plot& pl) override {
        const std::string p = plot_payload(pl);
        send(kp::wire::PLOT, p.data(), p.size());
    }
    bool tune(double rf_hz) override {
        if (!std::isfinite(rf_hz) || rf_hz <= 0) return false;
        char b[32];
        snprintf(b, sizeof b, "%.0f", rf_hz);
        send(kp::wire::TUNE, b, strlen(b));
        return true;
    }
    double array_t_ = -1;   // array plugins: Host::time() = the block being processed
    void map_point(const kp::MapPoint& m) override {
        if (!std::isfinite(m.lat) || !std::isfinite(m.lon) || m.id.empty()) return;
        auto num = [](double v, const char* f) {
            if (!std::isfinite(v)) return std::string();
            char b[32];
            snprintf(b, sizeof b, f, v);
            return std::string(b);
        };
        std::string p;
        for (const std::string& f : {m.id.substr(0, 32), num(m.lat, "%.6f"), num(m.lon, "%.6f"), m.label.substr(0, 64),
                                     m.kind.substr(0, 16), num(m.heading, "%.1f"), num(m.altitude_m, "%.0f"),
                                     num(m.speed_kmh, "%.1f"), num(m.ttl_s, "%.0f"), m.info.substr(0, 1000)}) {
            p += f;
            p.push_back('\0');
        }
        p.pop_back();
        send(kp::wire::MAP_POINT, p.data(), p.size());
    }
    void map_remove(const std::string& id) override { send(kp::wire::MAP_REMOVE, id.data(), std::min<size_t>(id.size(), 32)); }
    void table_columns(const std::vector<std::string>& cols) override {
        std::string p;
        for (size_t i = 0; i < cols.size() && i < 40; i++) {
            if (i) p.push_back('\0');
            p += cols[i].substr(0, 40);
        }
        send(kp::wire::TABLE_COLUMNS, p.data(), p.size());
    }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override {
        std::string p = key.substr(0, 64);
        for (size_t i = 0; i < cells.size() && i < 40; i++) {
            p.push_back('\0');
            p += cells[i].substr(0, 200);
        }
        send(kp::wire::TABLE_ROW, p.data(), p.size());
    }
    void table_remove(const std::string& key) override {
        send(kp::wire::TABLE_REMOVE, key.data(), std::min<size_t>(key.size(), 64));
    }
    void table_keep(double seconds) override {
        if (!std::isfinite(seconds)) return;
        const std::string p = std::to_string(static_cast<long long>(std::llround(seconds)));
        send(kp::wire::TABLE_KEEP, p.data(), p.size());
    }
    void table_detail(const std::string& key, const std::vector<std::pair<std::string, std::string>>& lines) override {
        std::string p = key.substr(0, 64);
        for (size_t i = 0; i < lines.size() && i < 200; i++) {
            p.push_back('\0');
            p += lines[i].first.substr(0, 80);
            p.push_back('\0');
            p += lines[i].second.substr(0, 400);
        }
        send(kp::wire::TABLE_DETAIL, p.data(), p.size());
    }
    bool raw_wanted() const override { return raw_wanted_; }
    void raw(const std::string& data) override {
        if (raw_wanted_) send(kp::wire::RAW, data.data(), std::min<size_t>(data.size(), 2000));
    }
    void message(const std::string& from, const std::string& text) override {
        std::string p = from.substr(0, 64);
        p.push_back('\0');
        p += text.substr(0, 2000);
        send(kp::wire::MESSAGE, p.data(), p.size());
    }
    void talker(const kp::Talker& t) override {
        if (t.id.empty()) return;
        std::string p = t.id.substr(0, 32);
        p.push_back('\0');
        p += t.label.substr(0, 64);
        p.push_back('\0');
        p += secs(t.start_s);
        p.push_back('\0');
        p += secs(t.end_s);
        if (t.channel != 0 || t.packet) {
            p.push_back('\0');
            p += std::to_string(std::clamp(t.channel, 0, 7));
        }
        if (t.packet) {
            p.push_back('\0');
            p += "p";
            p.push_back('\0');
            p += std::isfinite(t.freq_hz) ? fmt_hz(t.freq_hz) : "";
            p.push_back('\0');
            p += fmt_hz(std::isfinite(t.bw_hz) ? std::max(0.0, t.bw_hz) : 0.0);
            p.push_back('\0');
            p += fmt_hz(std::isfinite(t.avg_s) ? std::max(0.0, t.avg_s) : 0.0);
            const bool keep = std::isfinite(t.keep_s) && t.keep_s > 0;
            if (std::isfinite(t.lat) && std::isfinite(t.lon) && std::isfinite(t.alt_m) && std::fabs(t.lat) <= 90 &&
                std::fabs(t.lon) <= 180) {
                char b[40];
                snprintf(b, sizeof b, "%.7f", t.lat);
                p.push_back('\0');
                p += b;
                snprintf(b, sizeof b, "%.7f", t.lon);
                p.push_back('\0');
                p += b;
                snprintf(b, sizeof b, "%.1f", t.alt_m);
                p.push_back('\0');
                p += b;
            } else if (keep) {
                p.append(3, '\0');   // no position (empty fields), keep_s follows
            }
            if (keep) {
                p.push_back('\0');
                p += std::to_string(static_cast<long long>(std::llround(t.keep_s)));
            }
        }
        send(kp::wire::TALKER, p.data(), p.size());
    }
    void talker_end(double at_s, int channel) override {
        std::string p = secs(at_s);
        if (channel != 0) {
            p.push_back('\0');
            p += std::to_string(std::clamp(channel, 0, 7));
        }
        send(kp::wire::TALKER_END, p.data(), p.size());
    }
    void talker_forget(const std::string& id) override {
        send(kp::wire::TALKER_FORGET, id.data(), std::min<size_t>(id.size(), 32));
    }
    bool station(double* lat, double* lon) const override {
        if (!have_station_) return false;
        *lat = st_lat_;
        *lon = st_lon_;
        return true;
    }
    double rf_hz() const override { return rf_hz_; }
    bool rf_inverted() const override { return rf_inv_; }

    bool verbose_ = false, voice_wanted_ = false, raw_wanted_ = false;
    int audio_channel_ = -1;   // last AUDIO_CHANNEL sent
    bool have_station_ = false;
    double st_lat_ = 0, st_lon_ = 0;
    double rf_hz_ = NAN;
    bool rf_inv_ = false;
    double samples_ = 0;
    void sync_done(const void* id, size_t len) { send(kp::wire::SYNC_DONE, id, len); }

private:
    int fd_;
    double rate_;
    static std::string fmt_hz(double v) {
        char b[40];
        snprintf(b, sizeof b, "%.3f", v);
        return b;
    }
    // a Host::time() value for the wire; NAN = now
    std::string secs(double t) const {
        if (!std::isfinite(t)) t = time();
        char b[40];
        snprintf(b, sizeof b, "%.7f", t);
        return b;
    }
    void send(uint32_t type, const void* data, size_t len) {
        if (len > kp::wire::MAX_PAYLOAD) len = kp::wire::MAX_PAYLOAD;
        uint32_t hdr[2] = {type, static_cast<uint32_t>(len)};
        write_all(hdr, 8);
        if (len) write_all(data, len);
    }
    void write_all(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n > 0) {
            ssize_t w = ::write(fd_, c, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                _exit(0);   // kraken_doa went away
            }
            c += w;
            n -= static_cast<size_t>(w);
        }
    }
};

bool read_all(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        ssize_t r = ::read(fd, c, n);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

int serve() {
    // stdout carries the protocol: keep a private copy of it and point fd 1
    // at stderr, so a stray printf / std::cout in a plugin can't corrupt it
    int out_fd = dup(1);
    dup2(2, 1);
    kp::Info info = kp_plugin_info();
    WireHost host(out_fd, info.sample_rate);
    auto dec = kp_plugin_create(host);
    for (const auto& o : info.options) dec->option(o.key, o.def);
    std::vector<char> buf;
    std::vector<std::vector<kp::cf>> mc;   // SAMPLES_MC, per antenna
    std::vector<kp::cf> arr;   // array plugins: the block, antenna-major
    for (;;) {
        uint32_t hdr[2];
        if (!read_all(0, hdr, 8)) return 0;
        if (hdr[1] > kp::wire::MAX_PAYLOAD) return 2;
        buf.resize(hdr[1]);
        if (hdr[1] && !read_all(0, buf.data(), hdr[1])) return 0;
        try {
            switch (hdr[0]) {
                case kp::wire::SAMPLES: {
                    size_t n = hdr[1] / sizeof(kp::cf);
                    dec->process(reinterpret_cast<const kp::cf*>(buf.data()), n);
                    host.samples_ += n;
                    break;
                }
                case kp::wire::SAMPLES_MC: {
                    uint32_t m = 0;
                    if (hdr[1] >= 4) memcpy(&m, buf.data(), 4);
                    if (m < 2 || m > 8) break;
                    const size_t n = (hdr[1] - 4) / (4 * static_cast<size_t>(m));
                    if (!n) break;
                    mc.resize(m);
                    std::vector<const kp::cf*> ptrs(m);
                    const int16_t* q = reinterpret_cast<const int16_t*>(buf.data() + 4);
                    const float sc = 1.0f / kp::wire::MC_SCALE;
                    for (uint32_t k = 0; k < m; k++) {
                        mc[k].resize(n);
                        float* __restrict d = reinterpret_cast<float*>(mc[k].data());
                        for (size_t i = 0; i < n; i++) {
                            d[2 * i] = q[2 * (i * m + k)] * sc;
                            d[2 * i + 1] = q[2 * (i * m + k) + 1] * sc;
                        }
                        ptrs[k] = mc[k].data();
                    }
                    dec->process_multi(ptrs.data(), static_cast<int>(m), n);
                    host.samples_ += n;
                    break;
                }
                case kp::wire::SAMPLES_ARRAY: {
                    // uint32 m, n, flags; float32 scale; float64 rate, t0; int16 I/Q, sample-major
                    if (buf.size() < 32) break;
                    uint32_t m, n, flags;
                    float scale;
                    double rate, t0;
                    memcpy(&m, buf.data(), 4);
                    memcpy(&n, buf.data() + 4, 4);
                    memcpy(&flags, buf.data() + 8, 4);
                    memcpy(&scale, buf.data() + 12, 4);
                    memcpy(&rate, buf.data() + 16, 8);
                    memcpy(&t0, buf.data() + 24, 8);
                    if (m < 1 || m > 8 || buf.size() < 32 + static_cast<size_t>(m) * n * 4) break;
                    arr.resize(static_cast<size_t>(m) * n);
                    const int16_t* q = reinterpret_cast<const int16_t*>(buf.data() + 32);
                    for (uint32_t i = 0; i < n; i++)
                        for (uint32_t k = 0; k < m; k++, q += 2) arr[k * n + i] = kp::cf(q[0] * scale, q[1] * scale);
                    kp::ArrayBlock b;
                    b.m = static_cast<int>(m);
                    b.n = n;
                    b.rate = rate;
                    b.t0 = t0;
                    b.gap = flags & 1;
                    for (uint32_t k = 0; k < m; k++) b.ch[k] = arr.data() + static_cast<size_t>(k) * n;
                    host.array_t_ = t0;
                    dec->process_array(b);
                    break;
                }
                case kp::wire::OPTION: {
                    std::string s(buf.begin(), buf.end());
                    size_t eq = s.find('=');
                    std::string k = s.substr(0, eq), v = eq == std::string::npos ? "" : s.substr(eq + 1);
                    if (k == "verbose") host.verbose_ = v == "1";
                    if (k == "log_raw") { host.raw_wanted_ = v == "1"; break; }   // host-side only
                    // "" = back to the declared default
                    if (v.empty())
                        for (const auto& o : info.options)
                            if (k == o.key) v = o.def;
                    dec->option(k, v);
                    break;
                }
                case kp::wire::RESET:
                    dec->reset();
                    break;
                case kp::wire::VOICE_WANTED:
                    host.voice_wanted_ = !buf.empty() && buf[0];
                    break;
                case kp::wire::SYNC:
                    host.sync_done(buf.data(), buf.size());
                    break;
                case kp::wire::STATION: {
                    std::string s(buf.begin(), buf.end());
                    double la = 0, lo = 0;
                    host.have_station_ = sscanf(s.c_str(), "%lf,%lf", &la, &lo) == 2 && std::isfinite(la) &&
                                         std::isfinite(lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180;
                    host.st_lat_ = la;
                    host.st_lon_ = lo;
                    break;
                }
                case kp::wire::RF: {
                    std::string s(buf.begin(), buf.end());
                    double f = NAN;
                    int inv = 0;
                    const int got = sscanf(s.c_str(), "%lf,%d", &f, &inv);
                    host.rf_hz_ = got >= 1 && std::isfinite(f) && f > 0 ? f : NAN;
                    host.rf_inv_ = got == 2 && inv != 0;
                    break;
                }
                default:
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "decoder exception: " << e.what() << std::endl;
        }
    }
}

// --------------------------------------------------------------------------
// Offline test mode
// --------------------------------------------------------------------------
class TestHost : public kp::Host {
public:
    TestHost(double rate, bool quiet) : rate_(rate), quiet_(quiet) {}
    void fact(const std::string& k, const std::string& v) override {
        for (auto& f : facts_)
            if (f.first == k) {
                if (v.empty()) { facts_.erase(facts_.begin() + (&f - &facts_[0])); return; }
                f.second = v;
                return;
            }
        if (!v.empty()) facts_.emplace_back(k, v);
    }
    void event(const std::string& text, double dedup_s) override {
        double t = time();
        auto it = last_.find(text);
        if (it != last_.end() && t - it->second < dedup_s) return;
        last_[text] = t;
        events_++;
        if (!quiet_) printf("[%9.3f s] %s\n", t, text.c_str());
    }
    void valid() override {
        valid_++;
        if (first_valid_ < 0) first_valid_ = time();
    }
    // valid frames with their samples (Host::valid(start, end)): counted, the
    // time they cover, and each one with --verbose
    uint64_t valid_spans_ = 0;
    double valid_span_s_ = 0, valid_span_last_ = -1;
    void valid(double start_s, double end_s) override {
        valid();
        if (!std::isfinite(start_s) || !std::isfinite(end_s) || end_s < start_s) return;
        valid_spans_++;
        // covered time without counting overlaps twice
        valid_span_s_ += end_s - std::max(start_s, std::min(end_s, valid_span_last_));
        valid_span_last_ = std::max(valid_span_last_, end_s);
        if (verbose_ && !quiet_) printf("[%9.3f s] (valid) %.4f .. %.4f s\n", time(), start_s, end_s);
    }
    void audio(const float* pcm, size_t n) override { audio_.insert(audio_.end(), pcm, pcm + n); }
    void voice_state(const std::string& s) override {
        if (s != vstate_ && !quiet_ && !s.empty()) printf("[%9.3f s] (voice) %s\n", time(), s.c_str());
        vstate_ = s;
    }
    void freq_error(float hz) override { ferr_ = hz; nferr_++; }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return want_audio_; }
    double time() const override { return array_t_ >= 0 ? array_t_ : samples_ / rate_; }
    double array_t_ = -1;
    void log(const std::string& t) override {
        if (!quiet_) fprintf(stderr, "[%9.3f s] log: %s\n", time(), t.c_str());
    }
    // spatial reports (array plugins): per id, the trace-normalised covariances summed
    struct Sp {
        std::string label;
        uint64_t n = 0;
        int m = 0;
        double delay = NAN, power = NAN, snr = NAN, first = 0, last = 0;
        bool ref = false;
        std::vector<std::complex<double>> sum;
    };
    std::map<std::string, Sp> spatial_;
    void spatial(const kp::Spatial& s) override {
        if (s.id.empty() || s.m < 1 || s.m > 8 || s.cov.size() != static_cast<size_t>(s.m * s.m)) return;
        Sp& k = spatial_[s.id];
        if (k.m != s.m) {
            k.m = s.m;
            k.sum.assign(s.cov.size(), {});
        }
        double tr = 0;
        for (int i = 0; i < s.m; i++) tr += s.cov[static_cast<size_t>(i * s.m + i)].real();
        if (!(tr > 0)) return;
        for (size_t i = 0; i < s.cov.size(); i++) k.sum[i] += std::complex<double>(s.cov[i]) / tr;
        if (!k.n) k.first = time();
        k.n++;
        k.last = time();
        k.label = s.label;
        k.delay = s.delay_us;
        k.power = s.power_db;
        k.snr = s.snr_db;
        k.ref = s.ref;
        if (verbose_ && !quiet_)
            printf("[%9.3f s] (spatial) %s %s delay %.2f us %.1f dB snr %.1f dB%s\n", time(), s.id.c_str(), s.label.c_str(),
                   s.delay_us, s.power_db, s.snr_db, s.ref ? " ref" : "");
    }
    // a removed one leaves the summary (as kraken_doa forgets it)
    uint64_t spatial_removed_ = 0;
    void spatial_remove(const std::string& id) override {
        size_t n = 0;
        if (id.empty()) {
            n = spatial_.size();
            spatial_.clear();
        } else {
            n = spatial_.erase(id);
        }
        spatial_removed_ += n;
        if (n && !quiet_) printf("[%9.3f s] (spatial removed) %s\n", time(), id.empty() ? "all" : id.c_str());
    }
    kp::Plot plot_;
    uint64_t plots_ = 0;
    void plot(const kp::Plot& p) override {
        plot_ = p;
        plots_++;
    }
    // a recording can't be retuned: shown, refused
    bool tune(double rf_hz) override {
        if (!quiet_) printf("[%9.3f s] (tune) %.6f MHz asked - a recording can't be retuned\n", time(), rf_hz / 1e6);
        return false;
    }
    void map_point(const kp::MapPoint& m) override {
        if (!std::isfinite(m.lat) || !std::isfinite(m.lon) || m.id.empty()) return;
        map_updates_++;
        map_[m.id] = m;
    }
    void map_remove(const std::string& id) override { map_.erase(id); }
    void table_columns(const std::vector<std::string>& cols) override { cols_ = cols; }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override { rows_[key] = cells; }
    void table_remove(const std::string& key) override {
        rows_.erase(key);
        details_.erase(key);
    }
    void table_detail(const std::string& key, const std::vector<std::pair<std::string, std::string>>& lines) override { details_[key] = lines; }
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> details_;
    // the rows + their details
    void print_table(size_t max_rows) const {
        if (rows_.empty()) return;
        printf("table: %zu rows\n", rows_.size());
        std::string h = "  ";
        for (const auto& c : cols_) h += c + " | ";
        printf("%s\n", h.c_str());
        size_t n = 0;
        for (const auto& kv : rows_) {
            if (++n > max_rows) {
                printf("  ...\n");
                break;
            }
            std::string r = "  ";
            for (const auto& c : kv.second) r += c + " | ";
            printf("%s\n", r.c_str());
            const auto it = details_.find(kv.first);
            if (it == details_.end()) continue;
            for (const auto& [k, v] : it->second) printf("      %-34s %s\n", (k + ":").c_str(), v.c_str());
        }
    }
    bool raw_wanted() const override { return raw_; }
    void raw(const std::string& data) override {
        raws_++;
        if (raw_ && !quiet_) printf("[%9.3f s] (raw) %s\n", time(), data.c_str());
    }
    bool raw_ = false;
    uint64_t raws_ = 0;
    void message(const std::string& from, const std::string& text) override {
        messages_++;
        if (!quiet_) printf("[%9.3f s] (message) %s: %s\n", time(), from.c_str(), text.c_str());
    }
    uint64_t messages_ = 0;
    // talker spans as the live host cuts them: a new id ends the channel's previous one
    struct Span { std::string id, label; double start, end; bool open; int channel; };
    std::vector<Span> spans_;
    Span* open_span(int ch) {
        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it)
            if (it->open && it->channel == ch) return &*it;
        return nullptr;
    }
    // packets (Talker::packet): counted per talker
    struct Pk {
        std::string label;
        uint64_t n = 0, npos = 0;   // packets, those with a position (Talker::lat / lon / alt_m)
        double first = 0, last = 0, len_us = 0;
        double lat = NAN, lon = NAN, alt = NAN;   // the latest position
    };
    std::map<std::string, Pk> packets_;
    uint64_t packets_total_ = 0;
    // every packet with its channel, for the per-talker bearings (--uca on a
    // multichannel recording)
    struct PkRec { std::string id; double start, end, freq, bw; };
    std::vector<PkRec> pk_recs_;
    bool keep_pk_ = false;
    void talker(const kp::Talker& t) override {
        if (t.id.empty()) return;
        const double st = std::isfinite(t.start_s) ? t.start_s : time();
        const double en = std::isfinite(t.end_s) ? t.end_s : time();
        if (t.packet) {
            if (keep_pk_) pk_recs_.push_back({t.id, st, en, t.freq_hz, t.bw_hz});
            Pk& k = packets_[t.id];
            if (!k.n) k.first = st;
            k.n++;
            k.last = st;
            k.len_us += (en - st) * 1e6;
            if (!t.label.empty()) k.label = t.label;
            const bool pos = std::isfinite(t.lat) && std::isfinite(t.lon) && std::isfinite(t.alt_m);
            if (pos) {
                k.npos++;
                k.lat = t.lat;
                k.lon = t.lon;
                k.alt = t.alt_m;
            }
            packets_total_++;
            if (verbose_ && !quiet_) {
                char where[96] = "";
                if (pos) snprintf(where, sizeof where, " at %.5f, %.5f, %.0f m", t.lat, t.lon, t.alt_m);
                printf("[%9.3f s] (packet) %s %.7f .. %.7f s%s%s\n", time(), t.id.c_str(), st, en,
                       std::isfinite(t.freq_hz) ? (" at " + std::to_string(static_cast<long>(std::lround(t.freq_hz))) + " Hz").c_str() : "",
                       where);
            }
            return;
        }
        Span* s = open_span(t.channel);
        if (!s || s->id != t.id) {
            if (s) talker_end(st, t.channel);
            spans_.push_back({t.id, t.label, st, en, true, t.channel});
            s = &spans_.back();
            if (!quiet_)
                printf("[%9.3f s] (talker) %s %s: from %.3f s%s\n", time(), t.id.c_str(), t.label.c_str(), st,
                       t.channel ? (" (channel " + std::to_string(t.channel) + ")").c_str() : "");
        }
        s->end = std::max(s->end, en);
        if (!t.label.empty()) s->label = t.label;
    }
    void talker_end(double at_s, int channel) override {
        Span* s = open_span(channel);
        if (!s) return;
        s->open = false;
        if (std::isfinite(at_s)) s->end = std::max(s->start, at_s);
        if (!quiet_) printf("[%9.3f s] (talker) %s ended: %.3f .. %.3f s\n", time(), s->id.c_str(), s->start, s->end);
    }
    std::vector<std::string> cols_;
    std::map<std::string, std::vector<std::string>> rows_;
    bool station(double* lat, double* lon) const override {
        if (!have_station_) return false;
        *lat = st_lat_;
        *lon = st_lon_;
        return true;
    }
    bool have_station_ = false;
    double st_lat_ = 0, st_lon_ = 0;
    double rf_hz() const override { return rf_hz_; }
    bool rf_inverted() const override { return rf_inv_; }
    double rf_hz_ = NAN;
    bool rf_inv_ = false;
    uint64_t map_updates_ = 0;
    std::map<std::string, kp::MapPoint> map_;

    double rate_;
    bool quiet_, verbose_ = false, want_audio_ = false;
    double samples_ = 0;
    uint64_t valid_ = 0, events_ = 0, nferr_ = 0;
    double first_valid_ = -1;
    float ferr_ = 0;
    std::string vstate_;
    std::vector<std::pair<std::string, std::string>> facts_;
    std::map<std::string, double> last_;
    std::vector<float> audio_;
};

// a number from the recording's JSON sidecar (ai/sigtool.py captures:
// "rate", "rf_hz")
bool read_sidecar(const std::string& path, const char* key, double* out) {
    std::ifstream f(path + ".json");
    if (!f) {
        // also accept capture.json next to capture.cf32
        size_t dot = path.rfind('.');
        if (dot == std::string::npos) return false;
        f.open(path.substr(0, dot) + ".json");
        if (!f) return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t p = s.find("\"" + std::string(key) + "\"");
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    *out = atof(s.c_str() + p + 1);
    return *out > 0;
}
bool read_sidecar_rate(const std::string& path, double* rate) { return read_sidecar(path, "rate", rate); }

// Loads the whole recording as complex float
bool load_file(const std::string& path, std::string fmt, double* rate, double start, double seconds,
               std::vector<kp::cf>& out, std::string& err) {
    if (fmt.empty()) {
        std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
        if (ext == "cu8" || ext == "u8") fmt = "cu8";
        else if (ext == "cs16" || ext == "s16") fmt = "cs16";
        else if (ext == "wav") fmt = "wav";
        else fmt = "cf32";
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t off = 0;
    int bits = 0;
    bool wav_float = false;
    if (fmt == "wav") {
        // minimal RIFF parser: 2 channels (I, Q), int16 or float32
        if (raw.size() < 44 || memcmp(raw.data(), "RIFF", 4) || memcmp(raw.data() + 8, "WAVE", 4)) {
            err = "not a WAV file";
            return false;
        }
        size_t p = 12;
        int ch = 0;
        while (p + 8 <= raw.size()) {
            uint32_t len;
            memcpy(&len, raw.data() + p + 4, 4);
            if (!memcmp(raw.data() + p, "fmt ", 4)) {
                uint16_t afmt, nch, bps;
                uint32_t sr;
                memcpy(&afmt, raw.data() + p + 8, 2);
                memcpy(&nch, raw.data() + p + 10, 2);
                memcpy(&sr, raw.data() + p + 12, 4);
                memcpy(&bps, raw.data() + p + 22, 2);
                ch = nch;
                bits = bps;
                wav_float = afmt == 3;
                if (*rate <= 0) *rate = sr;
            } else if (!memcmp(raw.data() + p, "data", 4)) {
                off = p + 8;
                break;
            }
            p += 8 + len + (len & 1);
        }
        if (ch != 2 || off == 0) { err = "WAV must be 2-channel (I/Q)"; return false; }
        fmt = wav_float ? "cf32" : (bits == 16 ? "cs16" : (bits == 24 ? "cs24" : ""));
        if (fmt.empty()) { err = "unsupported WAV sample format"; return false; }
    }
    if (*rate <= 0 && !read_sidecar_rate(path, rate)) {
        err = "sample rate unknown: pass --rate";
        return false;
    }
    size_t bps = fmt == "cf32" ? 8 : (fmt == "cs16" ? 4 : (fmt == "cs24" ? 6 : 2));
    size_t total = (raw.size() - off) / bps;
    size_t s0 = std::min(total, static_cast<size_t>(std::max(0.0, start) * *rate));
    size_t s1 = seconds > 0 ? std::min(total, s0 + static_cast<size_t>(seconds * *rate)) : total;
    out.resize(s1 - s0);
    const char* d = raw.data() + off;
    for (size_t i = s0; i < s1; i++) {
        kp::cf v;
        if (fmt == "cf32") {
            float iq[2];
            memcpy(iq, d + i * 8, 8);
            v = {iq[0], iq[1]};
        } else if (fmt == "cs16") {
            int16_t iq[2];
            memcpy(iq, d + i * 4, 4);
            v = {iq[0] / 32768.0f, iq[1] / 32768.0f};
        } else if (fmt == "cs24") {
            auto q = reinterpret_cast<const uint8_t*>(d + i * 6);
            int32_t a = static_cast<int32_t>(static_cast<uint32_t>(q[0]) << 8 | static_cast<uint32_t>(q[1]) << 16 |
                                             static_cast<uint32_t>(q[2]) << 24) >> 8;
            int32_t b = static_cast<int32_t>(static_cast<uint32_t>(q[3]) << 8 | static_cast<uint32_t>(q[4]) << 16 |
                                             static_cast<uint32_t>(q[5]) << 24) >> 8;
            v = {a / 8388608.0f, b / 8388608.0f};
        } else {
            auto u = reinterpret_cast<const uint8_t*>(d + i * 2);
            v = {(u[0] - 127.5f) / 127.5f, (u[1] - 127.5f) / 127.5f};
        }
        out[i - s0] = v;
    }
    return true;
}

// A recording of every antenna (--channels M / the sidecar's "channels"):
// sample by sample, antenna 0 first, cf32 / cs16 / cu8. Read a block at a
// time (5 antennas at 2.4 MHz are far too big to load whole)
struct McReader {
    FILE* f = nullptr;
    std::string fmt;
    int m = 1;
    size_t bps = 8;
    std::vector<char> raw;
    bool open(const std::string& path, std::string fm, int channels, double rate, double start, std::string& err) {
        if (fm.empty()) {
            std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
            fm = ext == "cu8" || ext == "u8" ? "cu8" : ext == "cs16" || ext == "s16" ? "cs16" : "cf32";
        }
        if (fm != "cf32" && fm != "cs16" && fm != "cu8") { err = "multichannel recordings: cf32, cs16 or cu8"; return false; }
        fmt = fm;
        m = channels;
        bps = fmt == "cf32" ? 8 : fmt == "cs16" ? 4 : 2;
        f = fopen(path.c_str(), "rb");
        if (!f) { err = "cannot open " + path; return false; }
        const long long skip = static_cast<long long>(std::max(0.0, start) * rate) * m * static_cast<long long>(bps);
        if (skip && fseeko(f, skip, SEEK_SET) != 0) { err = "cannot seek"; return false; }
        return true;
    }
    // up to n samples of every antenna -> out[k]; returns the samples read
    size_t read(size_t n, std::vector<std::vector<kp::cf>>& out) {
        raw.resize(n * m * bps);
        const size_t got = fread(raw.data(), m * bps, n, f);
        out.resize(static_cast<size_t>(m));
        for (int k = 0; k < m; k++) out[static_cast<size_t>(k)].resize(got);
        const int kind = fmt == "cf32" ? 0 : fmt == "cs16" ? 1 : 2;
        for (int k = 0; k < m; k++) {
            kp::cf* o = out[static_cast<size_t>(k)].data();
            for (size_t i = 0; i < got; i++) {
                const char* d = raw.data() + (i * m + static_cast<size_t>(k)) * bps;
                if (kind == 0) {
                    float iq[2];
                    memcpy(iq, d, 8);
                    o[i] = {iq[0], iq[1]};
                } else if (kind == 1) {
                    int16_t iq[2];
                    memcpy(iq, d, 4);
                    o[i] = {iq[0] * (1.0f / 32768.0f), iq[1] * (1.0f / 32768.0f)};
                } else {
                    auto u = reinterpret_cast<const uint8_t*>(d);
                    o[i] = {(u[0] - 127.5f) * (1.0f / 127.5f), (u[1] - 127.5f) * (1.0f / 127.5f)};
                }
            }
        }
        return got;
    }
    ~McReader() { if (f) fclose(f); }
};

void write_wav8k(const std::string& path, const std::vector<float>& a) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + a.size() * 2); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    f.write("data", 4); u32(a.size() * 2);
    for (float v : a) u16(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767)));
}

// MUSIC with one source on a covariance, kraken_doa's UCA (clockwise, ANT0
// on +x, radius r metres, wavelength lambda): the bearing (degrees, CCW from
// ANT0 as the MUSIC DoA plot shows it) and the peak's sharpness
double uca_bearing(const std::vector<std::complex<double>>& C, int m, double r, double lambda, double* conf) {
    // principal eigenvector by power iteration
    std::vector<std::complex<double>> u(static_cast<size_t>(m), 1.0), v(static_cast<size_t>(m));
    for (int it = 0; it < 200; it++) {
        double nn = 0;
        for (int i = 0; i < m; i++) {
            v[static_cast<size_t>(i)] = 0;
            for (int j = 0; j < m; j++) v[static_cast<size_t>(i)] += C[static_cast<size_t>(i * m + j)] * u[static_cast<size_t>(j)];
            nn += std::norm(v[static_cast<size_t>(i)]);
        }
        nn = std::sqrt(nn);
        if (!(nn > 0)) return NAN;
        for (int i = 0; i < m; i++) u[static_cast<size_t>(i)] = v[static_cast<size_t>(i)] / nn;
    }
    double best = -1, bdeg = 0, sum = 0;
    int cnt = 0;
    for (int a = 0; a < 3600; a++) {
        const double th = a * M_PI / 1800;
        std::complex<double> dot = 0;
        for (int k = 0; k < m; k++) {
            const double ea = -2 * M_PI * k / m;
            const double ph = 2 * M_PI * r / lambda * (std::cos(ea) * std::cos(th) + std::sin(ea) * std::sin(th));
            dot += std::conj(std::polar(1.0, ph)) * u[static_cast<size_t>(k)];
        }
        const double c = std::norm(dot) / m;    // |a^H u|^2 / |a|^2
        const double pm = 1 / std::max(1e-9, 1 - c);
        sum += pm;
        cnt++;
        if (pm > best) { best = pm; bdeg = a / 10.0; }
    }
    if (conf) *conf = best / (sum / cnt);
    return bdeg;
}

// Per-talker bearings of a decoder run on a multichannel recording (--uca R
// with --channels M, non-array plugins): every talker's own samples of every
// antenna - transmissions (Host::talker spans) 40 ms inside both ends, packets
// exactly, each through the channel it named (Talker::freq_hz / bw_hz; else
// the band the plugin receives) - summed as trace-normalised covariances ->
// MUSIC with one source on kraken_doa's UCA. The same rules as kraken_doa's
// TalkerDoa: packets longer than 0.25 s and packets overlapping another
// talker's on the same channel are dropped (counted). Times: plugin time ->
// file time through the resampler's delay.
struct BearingIn {
    std::string path, fmt;
    int m = 1;
    double rate = 0, start = 0, offset = 0, band = 0, delay_s = 0, uca_r = 0, lambda = NAN;
};
void talker_bearings(const BearingIn& in, const TestHost& host) {
    constexpr double GUARD_S = 0.04, MAX_PACKET_S = 0.25;
    struct Seg { std::string id, label; double a, b, freq, bw; bool packet; };
    std::vector<Seg> segs;
    std::map<std::string, std::string> labels;
    for (const auto& s : host.spans_) {
        if (s.id == "?") continue;
        if (s.end - s.start > 2 * GUARD_S) segs.push_back({s.id, s.label, s.start + GUARD_S, s.end - GUARD_S, NAN, 0, false});
        labels[s.id] = s.label;
    }
    size_t too_long = 0, overlap = 0;
    std::vector<bool> bad(host.pk_recs_.size(), false);
    for (size_t i = 0; i < host.pk_recs_.size(); i++) {
        const auto& p = host.pk_recs_[i];
        if (p.end - p.start > MAX_PACKET_S) { bad[i] = true; too_long++; continue; }
        for (size_t j = i + 1; j < host.pk_recs_.size() && j < i + 64; j++) {
            const auto& q = host.pk_recs_[j];
            const bool same = !std::isfinite(p.freq) || !std::isfinite(q.freq) || std::fabs(p.freq - q.freq) < 0.5 * std::max(p.bw, q.bw) + 1;
            if (q.id != p.id && q.start < p.end && p.start < q.end && same) bad[i] = bad[j] = true;
        }
    }
    for (size_t i = 0; i < host.pk_recs_.size(); i++) {
        const auto& p = host.pk_recs_[i];
        if (bad[i]) { if (p.end - p.start <= MAX_PACKET_S) overlap++; continue; }
        auto it = host.packets_.find(p.id);
        segs.push_back({p.id, it != host.packets_.end() ? it->second.label : "", p.start, p.end, p.freq, p.bw, true});
    }
    printf("talker bearings (UCA r = %.3f m, %d antennas, %.4f MHz):\n", in.uca_r, in.m, 3e8 / in.lambda / 1e6);
    if (too_long) printf("  %zu packets longer than %.2f s dropped (kraken_doa drops them too: report <= 0.24 s of a longer frame)\n", too_long, MAX_PACKET_S);
    if (overlap) printf("  %zu packets dropped: they overlap another talker's packet on the same channel\n", overlap);
    FILE* f = fopen(in.path.c_str(), "rb");
    if (!f) { printf("  cannot open %s\n", in.path.c_str()); return; }
    const size_t bps = in.fmt == "cf32" ? 8 : in.fmt == "cs16" ? 4 : 2;
    const int m = in.m;
    // samples [i0, i0 + n) of every antenna (zeros outside the file)
    std::vector<char> raw;
    auto read = [&](int64_t i0, size_t n, std::vector<std::vector<std::complex<double>>>& out) {
        out.assign(static_cast<size_t>(m), std::vector<std::complex<double>>(n));
        const int64_t s0 = std::max<int64_t>(0, i0);
        if (static_cast<int64_t>(n) + i0 <= s0) return;
        const size_t skip = static_cast<size_t>(s0 - i0), want = n - skip;
        raw.resize(want * static_cast<size_t>(m) * bps);
        if (fseeko(f, static_cast<off_t>(s0) * m * static_cast<off_t>(bps), SEEK_SET) != 0) return;
        const size_t got = fread(raw.data(), static_cast<size_t>(m) * bps, want, f);
        for (size_t i = 0; i < got; i++)
            for (int k = 0; k < m; k++) {
                const char* d = raw.data() + (i * static_cast<size_t>(m) + static_cast<size_t>(k)) * bps;
                std::complex<double> v;
                if (bps == 8) { float iq[2]; memcpy(iq, d, 8); v = {iq[0], iq[1]}; }
                else if (bps == 4) { int16_t iq[2]; memcpy(iq, d, 4); v = {iq[0] / 32768.0, iq[1] / 32768.0}; }
                else { auto u = reinterpret_cast<const uint8_t*>(d); v = {(u[0] - 127.5) / 127.5, (u[1] - 127.5) / 127.5}; }
                out[static_cast<size_t>(k)][skip + i] = v;
            }
    };
    struct Acc { std::vector<std::complex<double>> sum; uint64_t n = 0; double secs = 0; bool packet = false; };
    std::map<std::string, Acc> acc;
    std::vector<std::vector<std::complex<double>>> x;
    for (const auto& s : segs) {
        // the channel in the file: the plugin's 0 Hz is --offset; a packet's own
        // channel, else the plugin's band
        const double fc = in.offset + (std::isfinite(s.freq) ? s.freq : 0);
        const double bw = s.bw > 0 ? s.bw : in.band;
        const bool filt = bw < 0.98 * in.rate || fc != 0;
        std::vector<double> h;
        if (filt) {
            int nt = static_cast<int>(std::clamp(4.0 * in.rate / std::max(1.0, bw), 15.0, 2047.0)) | 1;
            h.resize(static_cast<size_t>(nt));
            const double fcut = 0.5 * std::min(bw, in.rate) / in.rate;
            double sum = 0;
            for (int i = 0; i < nt; i++) {
                const double t = i - (nt - 1) / 2.0;
                const double sinc = t == 0 ? 2 * fcut : std::sin(2 * M_PI * fcut * t) / (M_PI * t);
                h[static_cast<size_t>(i)] = sinc * (0.54 - 0.46 * std::cos(2 * M_PI * i / (nt - 1)));
                sum += h[static_cast<size_t>(i)];
            }
            for (auto& v : h) v /= sum;
        }
        const int nt = static_cast<int>(h.size());
        const int64_t ia = std::llround((s.a - in.delay_s + in.start) * in.rate), ib = std::llround((s.b - in.delay_s + in.start) * in.rate);
        if (ib - ia < 2) continue;
        std::vector<std::complex<double>> R(static_cast<size_t>(m * m));
        std::vector<std::complex<double>> y(static_cast<size_t>(m));
        constexpr int64_t CH = 65536;
        for (int64_t c0 = ia; c0 < ib; c0 += CH) {
            const int64_t c1 = std::min(ib, c0 + CH);
            const int64_t r0 = c0 - (nt ? nt - 1 : 0);
            read(r0, static_cast<size_t>(c1 - r0), x);
            if (filt) {
                // mix the channel to 0 Hz (the phase reference is common to all antennas)
                for (int64_t i = r0; i < c1; i++) {
                    const std::complex<double> rot = std::polar(1.0, -2 * M_PI * std::fmod(fc * static_cast<double>(i) / in.rate, 1.0));
                    for (int k = 0; k < m; k++) x[static_cast<size_t>(k)][static_cast<size_t>(i - r0)] *= rot;
                }
            }
            for (int64_t i = c0; i < c1; i++) {
                const size_t o = static_cast<size_t>(i - r0);
                for (int k = 0; k < m; k++) {
                    if (!filt) { y[static_cast<size_t>(k)] = x[static_cast<size_t>(k)][o]; continue; }
                    std::complex<double> v = 0;
                    const auto& xk = x[static_cast<size_t>(k)];
                    for (int j = 0; j < nt; j++) v += h[static_cast<size_t>(j)] * xk[o - static_cast<size_t>(j)];
                    y[static_cast<size_t>(k)] = v;
                }
                for (int a = 0; a < m; a++)
                    for (int b = 0; b < m; b++) R[static_cast<size_t>(a * m + b)] += y[static_cast<size_t>(a)] * std::conj(y[static_cast<size_t>(b)]);
            }
        }
        double tr = 0;
        for (int k = 0; k < m; k++) tr += R[static_cast<size_t>(k * m + k)].real();
        if (!(tr > 0)) continue;
        Acc& A = acc[s.id];
        if (A.sum.empty()) A.sum.assign(static_cast<size_t>(m * m), 0);
        for (size_t i = 0; i < R.size(); i++) A.sum[i] += R[i] / tr;
        A.n++;
        A.secs += s.b - s.a;
        A.packet = s.packet;
        if (!s.label.empty()) labels[s.id] = s.label;
    }
    fclose(f);
    for (const auto& [id, A] : acc) {
        double conf = 0;
        const double b = uca_bearing(A.sum, m, in.uca_r, in.lambda, &conf);
        printf("  %-14s %-30s %5llu %s %8.2f s  bearing %6.1f deg  (peak/mean %.0f)\n", id.c_str(), labels[id].substr(0, 30).c_str(),
               static_cast<unsigned long long>(A.n), A.packet ? "packets" : "spans  ", A.secs, b, conf);
    }
}

// Array plugins: a multichannel recording through process_array()
int test_array(const std::string& path, std::string fmt, double rate, int channels, double offset, double start,
               double seconds, bool all, double uca_r, TestHost& host, kp::Decoder& dec, const kp::Info& info,
               bool quiet) {
    std::vector<kp::cf> x;
    std::string err;
    double frate = rate * std::max(1, channels);   // load_file counts complex values
    if (rate <= 0) {
        if (!read_sidecar_rate(path, &rate)) { fprintf(stderr, "error: sample rate unknown: pass --rate\n"); return 1; }
        frate = rate * std::max(1, channels);
    }
    const int m = channels;
    if (m < 1 || m > 8) { fprintf(stderr, "error: --channels 1..8\n"); return 1; }
    if (fmt.empty()) {
        const std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
        fmt = ext == "cu8" || ext == "u8" ? "cu8" : ext == "cs16" || ext == "s16" ? "cs16" : ext == "wav" ? "wav" : "cf32";
    }
    // cf32 / cs16 / cu8 are read block by block (a multichannel recording can
    // be larger than the RAM: 20 s of 5 antennas at 2.4 MHz is 1.9 GB); a WAV whole
    const bool stream = fmt != "wav";
    const size_t bps = fmt == "cf32" ? 8 : fmt == "cs16" ? 4 : 2;
    std::ifstream sf;
    size_t total = 0;
    if (stream) {
        sf.open(path, std::ios::binary | std::ios::ate);
        if (!sf) { fprintf(stderr, "error: cannot open %s\n", path.c_str()); return 1; }
        total = static_cast<size_t>(sf.tellg()) / bps / static_cast<size_t>(m);
    } else {
        if (!load_file(path, fmt, &frate, 0, 0, x, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        total = x.size() / static_cast<size_t>(m);
    }
    std::vector<char> raw;
    // samples p .. p + n of every antenna (sample-major, antenna-minor) -> x[0 ..]
    auto fetch = [&](size_t p, size_t n) -> const kp::cf* {
        if (!stream) return x.data() + p * static_cast<size_t>(m);
        const size_t cnt = n * static_cast<size_t>(m);
        raw.resize(cnt * bps);
        sf.seekg(static_cast<std::streamoff>(p * static_cast<size_t>(m) * bps));
        sf.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        x.resize(cnt);
        for (size_t i = 0; i < cnt; i++) {
            if (fmt == "cf32") {
                float iq[2];
                memcpy(iq, raw.data() + i * 8, 8);
                x[i] = {iq[0], iq[1]};
            } else if (fmt == "cs16") {
                int16_t iq[2];
                memcpy(iq, raw.data() + i * 4, 4);
                x[i] = {iq[0] / 32768.0f, iq[1] / 32768.0f};
            } else {
                auto u = reinterpret_cast<const uint8_t*>(raw.data() + i * 2);
                x[i] = {(u[0] - 127.5f) / 127.5f, (u[1] - 127.5f) / 127.5f};
            }
        }
        return x.data();
    };
    const size_t s0 = std::min(total, static_cast<size_t>(std::max(0.0, start) * rate));
    const size_t s1 = seconds > 0 ? std::min(total, s0 + static_cast<size_t>(seconds * rate)) : total;
    if (!quiet)
        printf("%s %s: %.2f s of %s, %d antennas at %.0f Hz%s%s\n", info.name, info.version, (s1 - s0) / rate, path.c_str(), m,
               rate, all ? "" : (" (bursts of " + std::to_string(info.array_burst_s).substr(0, 5) + " s every " +
                                 std::to_string(info.array_period_s).substr(0, 5) + " s)").c_str(),
               std::isfinite(host.rf_hz_) ? (", RF " + std::to_string(host.rf_hz_ / 1e6).substr(0, 11) + " MHz").c_str() : "");
    const size_t blk = std::max<size_t>(1, static_cast<size_t>(rate / 100));
    std::vector<std::vector<kp::cf>> ch(static_cast<size_t>(m), std::vector<kp::cf>(blk));
    const double burst = all ? 0 : std::max(0.0, info.array_burst_s), period = std::max(burst, info.array_period_s);
    bool gap = true;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t p = s0; p < s1; p += blk) {
        const size_t n = std::min(blk, s1 - p);
        const double t = static_cast<double>(p) / rate;
        if (!all && burst < period && std::fmod(t, period) >= burst) {   // between bursts
            gap = true;
            continue;
        }
        const kp::cf* xs = fetch(p, n);
        for (size_t i = 0; i < n; i++) {
            const kp::cf rot = offset != 0 ? std::polar(1.0f, static_cast<float>(std::remainder(-2 * M_PI * offset * static_cast<double>(p + i) / rate, 2 * M_PI)))
                                           : kp::cf(1, 0);
            for (int k = 0; k < m; k++) ch[static_cast<size_t>(k)][i] = xs[i * static_cast<size_t>(m) + static_cast<size_t>(k)] * rot;
        }
        kp::ArrayBlock b;
        b.m = m;
        b.n = n;
        b.rate = rate;
        b.t0 = t;
        b.gap = gap;
        gap = false;
        for (int k = 0; k < m; k++) b.ch[k] = ch[static_cast<size_t>(k)].data();
        host.array_t_ = t;
        dec.process_array(b);
    }
    host.array_t_ = static_cast<double>(s1) / rate;
    double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("\n=== %s summary ===\n", info.name);
    printf("input: %.2f s, process time %.2f s (%.1f%% of real time)\n", (s1 - s0) / rate, cpu, 100.0 * cpu / std::max(1e-9, (s1 - s0) / rate));
    printf("events: %llu\n", static_cast<unsigned long long>(host.events_));
    if (!host.facts_.empty()) {
        printf("facts:\n");
        for (auto& f : host.facts_) printf("  %-22s %s\n", f.first.c_str(), f.second.c_str());
    }
    const double lambda = std::isfinite(host.rf_hz_) && host.rf_hz_ > 0 ? 3e8 / host.rf_hz_ : NAN;
    if (host.spatial_removed_) printf("spatial removed: %llu\n", static_cast<unsigned long long>(host.spatial_removed_));
    if (!host.spatial_.empty()) {
        printf("spatial: %zu transmitters\n", host.spatial_.size());
        std::vector<std::pair<std::string, TestHost::Sp>> v(host.spatial_.begin(), host.spatial_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.delay < b.second.delay; });
        for (const auto& [id, k] : v) {
            char brg[64] = "";
            if (uca_r > 0 && std::isfinite(lambda)) {
                double conf = 0;
                const double b = uca_bearing(k.sum, k.m, uca_r, lambda, &conf);
                snprintf(brg, sizeof brg, "  bearing %6.1f deg", b);
            }
            printf("  %-8s %-34s %5llu reports  delay %8.3f us  %6.1f dB  snr %5.1f dB%s%s\n", id.c_str(), k.label.c_str(),
                   static_cast<unsigned long long>(k.n), k.delay, k.power, k.snr, k.ref ? " ref" : "    ", brg);
        }
    }
    if (host.plots_) {
        const kp::Plot& pl = host.plot_;
        printf("plot: %llu sent; last \"%s\" %zu points %s .. %s %s, %zu markers:", static_cast<unsigned long long>(host.plots_),
               pl.title.c_str(), pl.y.size(), wnum(pl.x0).c_str(), wnum(pl.x0 + pl.dx * std::max<double>(0, pl.y.size() - 1.0)).c_str(),
               pl.x_unit.c_str(), pl.markers.size());
        for (const auto& mk : pl.markers) printf(" %s@%.2f", mk.label.c_str(), mk.x);
        printf("\n");
    }
    host.print_table(1000);
    return 0;
}

int test_file(int argc, char** argv) {
    std::string path, fmt, audio_path;
    double rate = 0, offset = 0, start = 0, seconds = 0, rf = NAN, uca_r = 0;
    int channels = 0, pick = -1;   // a recording of every antenna: how many / decode only this one
    bool all = false;              // array plugins: every sample, not bursts
    bool quiet = false, verbose = false, invert = false, have_station = false, raw = false;
    double st_lat = 0, st_lon = 0;
    std::vector<std::pair<std::string, std::string>> opts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--file") path = next();
        else if (a == "--rate") rate = atof(next().c_str());
        else if (a == "--format") fmt = next();
        else if (a == "--offset") offset = atof(next().c_str());
        else if (a == "--rf") rf = atof(next().c_str());
        else if (a == "--start") start = atof(next().c_str());
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--audio") audio_path = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--invert") invert = true;
        else if (a == "--raw") raw = true;
        else if (a == "--channels") channels = atoi(next().c_str());
        else if (a == "--channel") pick = atoi(next().c_str());
        else if (a == "--all") all = true;
        else if (a == "--uca") uca_r = atof(next().c_str());
        else if (a == "--station") {
            std::string v = next();
            if (sscanf(v.c_str(), "%lf,%lf", &st_lat, &st_lon) != 2) { fprintf(stderr, "--station needs LAT,LON\n"); return 2; }
            have_station = true;
        }
        else if (a == "--opt") {
            std::string kv = next();
            size_t eq = kv.find('=');
            opts.emplace_back(kv.substr(0, eq), eq == std::string::npos ? "" : kv.substr(eq + 1));
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    kp::Info info = kp_plugin_info();
    if (info.array) {   // a multichannel recording through process_array()
        double ch = channels;
        if (ch <= 0 && !read_sidecar(path, "channels", &ch)) ch = 1;
        TestHost host(rate > 0 ? rate : 1, quiet);
        host.verbose_ = verbose;
        host.have_station_ = have_station;
        host.st_lat_ = st_lat;
        host.st_lon_ = st_lon;
        if (!std::isfinite(rf) && !read_sidecar(path, "rf_hz", &rf)) rf = NAN;
        if (std::isfinite(rf) && rf > 0) host.rf_hz_ = rf + offset;
        auto dec = kp_plugin_create(host);
        for (const auto& o : info.options) dec->option(o.key, o.def);
        if (verbose) dec->option("verbose", "1");
        for (auto& o : opts) dec->option(o.first, o.second);
        return test_array(path, fmt, rate, static_cast<int>(ch), offset, start, seconds, all, uca_r, host, *dec, info, quiet);
    }
    std::vector<kp::cf> x;
    std::string err;
    if (channels <= 0) {
        double c = 0;
        channels = read_sidecar(path, "channels", &c) ? static_cast<int>(c) : 1;
    }
    if (channels > 8) {
        fprintf(stderr, "error: at most 8 channels\n");
        return 1;
    }
    McReader mcr;
    // every antenna through process_multi (Info::multichannel); other plugins
    // decode antenna 0 (or --channel K) like a single-channel VFO
    if (channels > 1 && pick < 0 && !info.multichannel) pick = 0;
    const bool mc_all = channels > 1 && pick < 0;
    if (channels > 1) {
        if (rate <= 0 && !read_sidecar_rate(path, &rate)) {
            fprintf(stderr, "error: sample rate unknown: pass --rate\n");
            return 1;
        }
        if (!mcr.open(path, fmt, channels, rate, start, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (pick >= channels) {
            fprintf(stderr, "error: --channel %d of %d\n", pick, channels);
            return 1;
        }
        if (!mc_all) {   // one antenna, as a single-channel decoder gets it
            std::vector<std::vector<kp::cf>> b;
            const size_t want = seconds > 0 ? static_cast<size_t>(seconds * rate) : SIZE_MAX;
            for (size_t got; x.size() < want && (got = mcr.read(std::min<size_t>(1 << 16, want - x.size()), b)) > 0;)
                x.insert(x.end(), b[static_cast<size_t>(pick)].begin(), b[static_cast<size_t>(pick)].end());
        }
    } else if (!load_file(path, fmt, &rate, start, seconds, x, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (mc_all && (info.sample_rate != rate || offset != 0)) {
        fprintf(stderr, "error: every antenna at once needs the plugin's rate (%.0f Hz) and no --offset\n", info.sample_rate);
        return 1;
    }
    TestHost host(info.sample_rate, quiet);
    host.verbose_ = verbose;
    host.want_audio_ = !audio_path.empty();
    host.have_station_ = have_station;
    host.raw_ = raw;
    host.st_lat_ = st_lat;
    host.st_lon_ = st_lon;
    // RF of the file's centre (--rf, else the sidecar's "rf_hz"): the
    // decoder's 0 Hz is --offset away from it (mirrored with --invert)
    if (!std::isfinite(rf) && !read_sidecar(path, "rf_hz", &rf)) rf = NAN;
    if (std::isfinite(rf) && rf > 0) {
        host.rf_hz_ = invert ? rf - offset : rf + offset;
        host.rf_inv_ = invert;
    }
    host.keep_pk_ = uca_r > 0 && channels > 1;
    auto dec = kp_plugin_create(host);
    for (const auto& o : info.options) dec->option(o.key, o.def);
    if (verbose) dec->option("verbose", "1");
    for (auto& o : opts) dec->option(o.first, o.second);

    if (!quiet && mc_all)
        printf("%s %s: %d antennas of %s at %.0f Hz (process_multi)\n", info.name, info.version, channels, path.c_str(), rate);
    else if (!quiet)
        printf("%s %s: %.2f s of %s at %.0f Hz -> %.0f Hz%s%s\n", info.name, info.version,
               x.size() / rate, path.c_str(), rate, info.sample_rate,
               offset != 0 ? (" (offset " + std::to_string(static_cast<long>(offset)) + " Hz)").c_str() : "",
               std::isfinite(host.rf_hz_) ? (", RF " + std::to_string(host.rf_hz_ / 1e6).substr(0, 11) + " MHz").c_str() : "");
    // mix + resample to the plugin's rate, in 10 ms blocks like live
    kp::Nco nco(static_cast<float>(offset), static_cast<float>(rate));
    msresamp_crcf rs = msresamp_crcf_create(static_cast<float>(info.sample_rate / rate), 60.0f);
    // the plugin's samples lag the file by the resampler's delay (per-talker bearings)
    const double rs_delay_s = info.sample_rate == rate && offset == 0 ? 0.0 : msresamp_crcf_get_delay(rs) / info.sample_rate;
    const size_t blk = std::max<size_t>(1, static_cast<size_t>(rate / 100));
    std::vector<kp::cf> in(blk), outb(static_cast<size_t>(blk * info.sample_rate / rate) + 64);
    auto t0 = std::chrono::steady_clock::now();
    if (mc_all) {   // every antenna, 10 ms blocks like live
        std::vector<std::vector<kp::cf>> b;
        std::vector<const kp::cf*> ptrs(static_cast<size_t>(channels));
        const size_t want = seconds > 0 ? static_cast<size_t>(seconds * rate) : SIZE_MAX;
        size_t done = 0;
        for (size_t got; done < want && (got = mcr.read(std::min(blk, want - done), b)) > 0; done += got) {
            for (int k = 0; k < channels; k++) {
                if (invert)
                    for (auto& v : b[static_cast<size_t>(k)]) v = std::conj(v);
                ptrs[static_cast<size_t>(k)] = b[static_cast<size_t>(k)].data();
            }
            dec->process_multi(ptrs.data(), channels, got);
            host.samples_ += got;
        }
    }
    for (size_t p = 0; p < x.size(); p += blk) {
        size_t n = std::min(blk, x.size() - p);
        for (size_t i = 0; i < n; i++) in[i] = nco.mix(invert ? std::conj(x[p + i]) : x[p + i]);
        unsigned int ny = 0;
        if (info.sample_rate == rate && offset == 0) {
            std::copy(in.begin(), in.begin() + n, outb.begin());
            ny = static_cast<unsigned int>(n);
        } else {
            msresamp_crcf_execute(rs, in.data(), static_cast<unsigned int>(n), outb.data(), &ny);
        }
        if (ny) {
            dec->process(outb.data(), ny);
            host.samples_ += ny;
        }
    }
    msresamp_crcf_destroy(rs);
    double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("\n=== %s summary ===\n", info.name);
    printf("input: %.2f s, decode time %.2f s (%.1f%% of real time)\n", host.time(), cpu,
           host.time() > 0 ? 100.0 * cpu / host.time() : 0.0);
    printf("VALID FRAMES: %llu", static_cast<unsigned long long>(host.valid_));
    if (host.first_valid_ >= 0) printf(" (first at %.3f s)", host.first_valid_);
    if (host.valid_spans_)
        printf("\nvalid frames with their samples: %llu, covering %.2f s", static_cast<unsigned long long>(host.valid_spans_),
               host.valid_span_s_);
    printf("\nevents: %llu\n", static_cast<unsigned long long>(host.events_));
    if (host.nferr_) printf("last frequency error: %.0f Hz\n", host.ferr_);
    if (!host.facts_.empty()) {
        printf("facts:\n");
        for (auto& f : host.facts_) printf("  %-22s %s\n", f.first.c_str(), f.second.c_str());
    }
    if (host.raws_) printf("raw frames: %llu\n", static_cast<unsigned long long>(host.raws_));
    if (!host.spans_.empty()) {
        printf("talkers: %zu transmissions\n", host.spans_.size());
        for (const auto& s : host.spans_)
            printf("  %-10s %-32s %8.3f .. %8.3f s%s%s\n", s.id.c_str(), s.label.c_str(), s.start, s.end,
                   s.channel ? (" ch " + std::to_string(s.channel)).c_str() : "", s.open ? " (open)" : "");
    }
    if (!host.packets_.empty()) {
        printf("talker packets: %llu from %zu talkers\n", static_cast<unsigned long long>(host.packets_total_),
               host.packets_.size());
        std::vector<std::pair<std::string, TestHost::Pk>> v(host.packets_.begin(), host.packets_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
        for (size_t i = 0; i < v.size() && i < 40; i++) {
            const TestHost::Pk& k = v[i].second;
            char pos[128] = "";
            if (k.npos)
                snprintf(pos, sizeof pos, "  %llu with position, last %.5f, %.5f, %.0f m",
                         static_cast<unsigned long long>(k.npos), k.lat, k.lon, k.alt);
            printf("  %-10s %-24s %6llu packets  %8.3f .. %8.3f s  mean %.0f us%s\n", v[i].first.c_str(),
                   k.label.c_str(), static_cast<unsigned long long>(k.n), k.first, k.last,
                   k.len_us / static_cast<double>(k.n), pos);
        }
    }
    host.print_table(30);
    if (host.map_updates_ || !host.map_.empty()) {
        printf("map: %llu updates, %zu points\n", static_cast<unsigned long long>(host.map_updates_), host.map_.size());
        for (const auto& kv : host.map_) {
            const kp::MapPoint& m = kv.second;
            printf("  %-10s %-10s %10.5f %11.5f", m.id.c_str(), m.label.c_str(), m.lat, m.lon);
            if (std::isfinite(m.altitude_m)) printf("  %6.0f m", m.altitude_m);
            if (std::isfinite(m.speed_kmh)) printf("  %5.0f km/h", m.speed_kmh);
            if (std::isfinite(m.heading)) printf("  %5.1f deg", m.heading);
            printf("  [%s]\n", m.kind.c_str());
        }
    }
    if (!audio_path.empty()) {
        write_wav8k(audio_path, host.audio_);
        printf("audio: %.2f s -> %s\n", host.audio_.size() / 8000.0, audio_path.c_str());
    }
    if (uca_r > 0 && channels > 1) {
        if (!std::isfinite(host.rf_hz_) || !(host.rf_hz_ > 0)) {
            printf("talker bearings: need the RF (--rf or the sidecar's rf_hz)\n");
        } else if (invert) {
            printf("talker bearings: not with --invert\n");
        } else {
            BearingIn b;
            b.path = path;
            b.fmt = mcr.fmt;
            b.m = channels;
            b.rate = rate;
            b.start = start;
            b.offset = offset;
            b.band = std::min(info.sample_rate, rate);
            b.delay_s = mc_all ? 0.0 : rs_delay_s;
            b.uca_r = uca_r;
            b.lambda = 3e8 / host.rf_hz_;
            talker_bearings(b, host);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--info")) return print_info();
    if (argc >= 2 && !strcmp(argv[1], "--serve")) return serve();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--file")) return test_file(argc, argv);
    kp::Info info = kp_plugin_info();
    fprintf(stderr,
            "%s (%s) - KrakenSDR decoder plugin\n"
            "usage: %s --info | --serve | --file REC [--rate HZ] [--format cf32|cu8|cs16|wav]\n"
            "          [--offset HZ] [--start S] [--seconds S] [--verbose] [--invert]\n"
            "          [--opt key=value] [--audio out.wav] [--station LAT,LON] [--rf HZ] [--raw] [--quiet]\n"
            "          [--channels M (every antenna, interleaved) [--channel K (only that one)] [--uca RADIUS_M]]\n"
            "          array plugins: [--channels M] [--all] [--uca RADIUS_M]\n",
            info.name, info.description, argv[0]);
    return 2;
}
