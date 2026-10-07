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
//   --station LAT,LON  the receiver's location (Host::station)
//   --raw            raw frames on (Host::raw_wanted) and printed
//   --quiet          only the summary

#include "kraken_plugin.hpp"
#include "kraken_dsp.hpp"

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
              << ",\"talkers\":" << (i.talkers ? "true" : "false");
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
    void audio(const float* pcm, size_t n) override {
        while (n > 0) {
            size_t k = std::min<size_t>(n, 8000);
            send(kp::wire::AUDIO, pcm, k * sizeof(float));
            pcm += k;
            n -= k;
        }
    }
    void voice_state(const std::string& s) override { send(kp::wire::VOICE_STATE, s.data(), s.size()); }
    void freq_error(float hz) override { send(kp::wire::FREQ_ERROR, &hz, 4); }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return voice_wanted_; }
    double time() const override { return samples_ / rate_; }
    void log(const std::string& t) override { std::cerr << t << std::endl; }
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
        send(kp::wire::TALKER, p.data(), p.size());
    }
    void talker_end(double at_s) override {
        std::string p = secs(at_s);
        send(kp::wire::TALKER_END, p.data(), p.size());
    }
    bool station(double* lat, double* lon) const override {
        if (!have_station_) return false;
        *lat = st_lat_;
        *lon = st_lon_;
        return true;
    }

    bool verbose_ = false, voice_wanted_ = false, raw_wanted_ = false;
    bool have_station_ = false;
    double st_lat_ = 0, st_lon_ = 0;
    double samples_ = 0;
    void sync_done(const void* id, size_t len) { send(kp::wire::SYNC_DONE, id, len); }

private:
    int fd_;
    double rate_;
    // a Host::time() value for the wire; NAN = now
    std::string secs(double t) const {
        if (!std::isfinite(t)) t = time();
        char b[32];
        snprintf(b, sizeof b, "%.4f", t);
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
    void audio(const float* pcm, size_t n) override { audio_.insert(audio_.end(), pcm, pcm + n); }
    void voice_state(const std::string& s) override {
        if (s != vstate_ && !quiet_ && !s.empty()) printf("[%9.3f s] (voice) %s\n", time(), s.c_str());
        vstate_ = s;
    }
    void freq_error(float hz) override { ferr_ = hz; nferr_++; }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return want_audio_; }
    double time() const override { return samples_ / rate_; }
    void log(const std::string& t) override {
        if (!quiet_) fprintf(stderr, "[%9.3f s] log: %s\n", time(), t.c_str());
    }
    void map_point(const kp::MapPoint& m) override {
        if (!std::isfinite(m.lat) || !std::isfinite(m.lon) || m.id.empty()) return;
        map_updates_++;
        map_[m.id] = m;
    }
    void map_remove(const std::string& id) override { map_.erase(id); }
    void table_columns(const std::vector<std::string>& cols) override { cols_ = cols; }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override { rows_[key] = cells; }
    void table_remove(const std::string& key) override { rows_.erase(key); }
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
    // talker spans as the live host cuts them: a new id ends the previous one
    struct Span { std::string id, label; double start, end; bool open; };
    std::vector<Span> spans_;
    void talker(const kp::Talker& t) override {
        if (t.id.empty()) return;
        const double st = std::isfinite(t.start_s) ? t.start_s : time();
        const double en = std::isfinite(t.end_s) ? t.end_s : time();
        if (spans_.empty() || !spans_.back().open || spans_.back().id != t.id) {
            if (!spans_.empty() && spans_.back().open) talker_end(st);
            spans_.push_back({t.id, t.label, st, en, true});
            if (!quiet_) printf("[%9.3f s] (talker) %s %s: from %.3f s\n", time(), t.id.c_str(), t.label.c_str(), st);
        }
        spans_.back().end = std::max(spans_.back().end, en);
        if (!t.label.empty()) spans_.back().label = t.label;
    }
    void talker_end(double at_s) override {
        if (spans_.empty() || !spans_.back().open) return;
        Span& s = spans_.back();
        s.open = false;
        if (std::isfinite(at_s)) s.end = std::max(s.start, at_s);
        if (!quiet_) printf("[%9.3f s] (talker) %s ended: %.3f .. %.3f s\n", time(), s.id.c_str(), s.start, s.end);
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

bool read_sidecar_rate(const std::string& path, double* rate) {
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
    size_t p = s.find("\"rate\"");
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    *rate = atof(s.c_str() + p + 1);
    return *rate > 0;
}

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

void write_wav8k(const std::string& path, const std::vector<float>& a) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + a.size() * 2); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    f.write("data", 4); u32(a.size() * 2);
    for (float v : a) u16(static_cast<uint16_t>(static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767)));
}

int test_file(int argc, char** argv) {
    std::string path, fmt, audio_path;
    double rate = 0, offset = 0, start = 0, seconds = 0;
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
        else if (a == "--start") start = atof(next().c_str());
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--audio") audio_path = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--invert") invert = true;
        else if (a == "--raw") raw = true;
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
    std::vector<kp::cf> x;
    std::string err;
    if (!load_file(path, fmt, &rate, start, seconds, x, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    kp::Info info = kp_plugin_info();
    TestHost host(info.sample_rate, quiet);
    host.verbose_ = verbose;
    host.want_audio_ = !audio_path.empty();
    host.have_station_ = have_station;
    host.raw_ = raw;
    host.st_lat_ = st_lat;
    host.st_lon_ = st_lon;
    auto dec = kp_plugin_create(host);
    for (const auto& o : info.options) dec->option(o.key, o.def);
    if (verbose) dec->option("verbose", "1");
    for (auto& o : opts) dec->option(o.first, o.second);

    if (!quiet)
        printf("%s %s: %.2f s of %s at %.0f Hz -> %.0f Hz%s\n", info.name, info.version,
               x.size() / rate, path.c_str(), rate, info.sample_rate,
               offset != 0 ? (" (offset " + std::to_string(static_cast<long>(offset)) + " Hz)").c_str() : "");
    // mix + resample to the plugin's rate, in 10 ms blocks like live
    kp::Nco nco(static_cast<float>(offset), static_cast<float>(rate));
    msresamp_crcf rs = msresamp_crcf_create(static_cast<float>(info.sample_rate / rate), 60.0f);
    const size_t blk = std::max<size_t>(1, static_cast<size_t>(rate / 100));
    std::vector<kp::cf> in(blk), outb(static_cast<size_t>(blk * info.sample_rate / rate) + 64);
    auto t0 = std::chrono::steady_clock::now();
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
            printf("  %-10s %-24s %8.3f .. %8.3f s%s\n", s.id.c_str(), s.label.c_str(), s.start, s.end,
                   s.open ? " (open)" : "");
    }
    if (!host.rows_.empty()) {
        printf("table: %zu rows\n", host.rows_.size());
        std::string h = "  ";
        for (const auto& c : host.cols_) h += c + " | ";
        printf("%s\n", h.c_str());
        size_t n = 0;
        for (const auto& kv : host.rows_) {
            if (++n > 30) { printf("  ...\n"); break; }
            std::string r = "  ";
            for (const auto& c : kv.second) r += c + " | ";
            printf("%s\n", r.c_str());
        }
    }
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
            "          [--opt key=value] [--audio out.wav] [--station LAT,LON] [--raw] [--quiet]\n",
            info.name, info.description, argv[0]);
    return 2;
}
