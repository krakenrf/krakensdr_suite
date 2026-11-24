// sigclass_tool - the signal classifier core (src/sigclass/) offline: feature
// extraction for training, classification of recordings, benchmarks, self-test.
// The training pipeline (tools/sigclass/) runs THIS binary on its synthetic
// captures, so training features and runtime features come from the same code.
//
// Build (from kraken_doa_v2), either
//   make sigclass-tool         (the app's own objects and flags -> tools/build/sigclass_tool)
// or one line (no make, no liquid-dsp / uWebSockets needed):
//   mkdir -p tools/build && g++ -std=c++20 -O3 -ffast-math -fno-finite-math-only -march=native -Wall -Wextra -Iinclude -Iexternal/pffft tools/sigclass_tool.cpp src/sigclass/*.cpp -x c external/pffft/pffft.c external/pffft/pffft_common.c -x none -o tools/build/sigclass_tool
//   (-x c compiles pffft as C; g++ then warns that -std=c++20 does not apply
//   to C - harmless. On Windows add -DPFFFT_STATIC_DEFINE.)
//
// Modes:
//   layout                                    the feature layout JSON
//   features --out PREFIX [opts] FILE...      PREFIX.f32 (rows x D float32) + PREFIX.jsonl
//   classify --model M [--track] [--rf HZ] [opts] FILE...   JSON lines: detections + top-3 classes
//                                             (--rf: the capture's RF centre - the model's RF mask)
//   bench [--reps N] [--model M] [opts] FILE  timing per stage
//   view --offset HZ --view V --out X.cf32 [opts] FILE   one view of the first snapshot
//   forward --model M --view V --in X.f32 --out P.f32 [--rf F --offset BW]   the model alone (parity tests;
//                                             --rf: the RF mask for a detection at F with BW)
//   selftest                                  checks that need no data
// opts: --rate HZ (2400000)  --format cu8|cs16|cf32 (from the extension, else cu8)
//       --snap S (0.5)  --hop S (= snap)  --list FILELIST  --centre HZ (classify)
//       --requant (cs16 / cf32: quantise to 8 bits like an RTL-SDR first)
//       --spec-hop N (the short spectrogram's hop, Config::spectrogram_hop)
//       rule parameters (experiments): --second-view-max HZ (Config::second_view_max_hz, 0 = off)
//       --image-share X (Config::image_share; 0 = every image candidate dropped)
//       --tone-merge 0|1 (Config::tone_merge), --lobe-widths X (Config::sidelobe_widths; 0 = off)
//       --min-channel HZ, --core-ratio X (classify --track: TrackerConfig::min_channel_hz /
//       core_energy_ratio; a huge --min-channel turns the core rule off), --hump-guard 0|1
//       (TrackerConfig::hump_guard)
//       --settled 0|1 (classify --track: the service's S1 emulated - a settled track's detections,
//       >= 3 hits and p >= its class's sure level, are not classified except in one look of 4:
//       analyze()'s known bands; each snapshot's line then carries "known", the count)
// Input scale: cu8 = (u - 127.5) / 127.5 (the runtime path); cs16 = v / 32768;
// cf32 as is (full scale 1.0). --requant turns cs16 / cf32 into cu8 bytes
// (round(x * 127.5 + 127.5), clipped) and takes the cu8 path.

#include "sigclass/sigclass.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cf = std::complex<float>;

namespace {

constexpr double PI = 3.14159265358979323846;

struct Opts {
    std::string mode, out, model, list, format, in;
    double rate = 2400000.0, snap = 0.5, hop = 0.0, centre = 0.0, offset = 0.0, rf = 0.0;
    int view = 0, reps = 20, spec_hop = 0;
    // (rule parameters for experiments; < 0 = the core's default)
    double second_view_max = -1.0, image_share = -1.0, min_channel = -1.0, core_ratio = -1.0, lobe_widths = -1.0;
    int hump_guard = -1;
    int settled = 0;
    int tone_merge = -1;
    bool track = false, requant = false;
    std::vector<std::string> files;
};

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}

std::string num(double v, int prec = 6) {
    if (!is_finite_value(v)) return "null";
    char b[40];
    std::snprintf(b, sizeof b, "%.*g", prec, v);
    return b;
}

std::string fmt_of(const Opts& o, const std::string& path) {
    if (!o.format.empty()) return o.format;
    auto ends = [&](const char* e) {
        const size_t n = std::strlen(e);
        return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
    };
    if (ends(".cs16") || ends(".s16")) return "cs16";
    if (ends(".cf32") || ends(".fc32") || ends(".cfile")) return "cf32";
    return "cu8";
}

int bytes_per_sample(const std::string& f) { return f == "cs16" ? 4 : f == "cf32" ? 8 : 2; }

// one snapshot of a file in the analyzer's two input forms
struct Snap {
    std::vector<uint8_t> u8;
    std::vector<cf> c;
    bool is_u8 = true;
};

bool read_snap(std::ifstream& f, const std::string& fmt, int64_t start, int64_t n, bool requant, Snap& s) {
    const int bps = bytes_per_sample(fmt);
    std::vector<char> raw(size_t(n) * size_t(bps));
    f.clear();
    f.seekg(std::streamoff(start) * bps);
    f.read(raw.data(), std::streamsize(raw.size()));
    if (f.gcount() != std::streamsize(raw.size())) return false;
    if (fmt == "cu8") {
        s.is_u8 = true;
        s.u8.assign(raw.begin(), raw.end());
        return true;
    }
    s.c.resize(size_t(n));
    for (int64_t i = 0; i < n; i++) {
        if (fmt == "cs16") {
            int16_t a, b;
            std::memcpy(&a, raw.data() + 4 * i, 2);
            std::memcpy(&b, raw.data() + 4 * i + 2, 2);
            s.c[size_t(i)] = cf(a / 32768.0f, b / 32768.0f);
        } else {
            float a, b;
            std::memcpy(&a, raw.data() + 8 * i, 4);
            std::memcpy(&b, raw.data() + 8 * i + 4, 4);
            s.c[size_t(i)] = cf(is_finite_value(a) ? a : 0.0f, is_finite_value(b) ? b : 0.0f);
        }
    }
    if (requant) {
        s.is_u8 = true;
        s.u8.resize(size_t(2 * n));
        auto q = [](float x) { return uint8_t(std::clamp(std::lround(x * 127.5f + 127.5f), 0L, 255L)); };
        for (int64_t i = 0; i < n; i++) {
            s.u8[size_t(2 * i)] = q(s.c[size_t(i)].real());
            s.u8[size_t(2 * i + 1)] = q(s.c[size_t(i)].imag());
        }
    } else {
        s.is_u8 = false;
    }
    return true;
}

std::vector<sigclass::Scored> run(sigclass::Analyzer& an, const Snap& s, double rate, const sigclass::Model* m,
                                  std::vector<float>* fo, double rf = 0.0, const std::vector<sigclass::KnownBand>* known = nullptr) {
    if (s.is_u8) return an.analyze(s.u8.data(), s.u8.size() / 2, rate, m, fo, rf, known);
    return an.analyze(s.c.data(), s.c.size(), rate, m, fo, rf, known);
}

// the classes' sure levels from the model's metadata (its "classes": min_p; else "min_confidence", else 0.7) -
// what the service's S1 reads (sigclass_service.cpp parse_table)
struct SureLevels {
    std::vector<std::pair<std::string, double>> min_p;
    double min_conf = 0.7;
    double of(const std::string& id) const {
        for (const auto& x : min_p)
            if (x.first == id) return x.second;
        return min_conf;
    }
};
SureLevels sure_levels(const std::string& meta) {
    SureLevels r;
    std::string v;
    if (json_find(meta, "min_confidence", v)) {
        const double d = std::atof(v.c_str());
        if (d > 0.0 && d <= 1.0) r.min_conf = d;
    }
    const size_t k = meta.find("\"classes\"");
    size_t p = k == std::string::npos ? std::string::npos : meta.find('[', k);
    if (p == std::string::npos) return r;
    int depth = 0;
    size_t obj = 0;
    for (p = p + 1; p < meta.size(); p++) {
        const char c = meta[p];
        if (c == '"') {
            std::string tmp;
            p = json_read_string(meta, p, tmp) - 1;
            continue;
        }
        if (c == '{' || c == '[') {
            if (depth++ == 0) obj = p;
        } else if (c == '}' || c == ']') {
            if (depth == 0) break;   // (the end of "classes")
            if (--depth == 0 && c == '}') {
                const std::string o = meta.substr(obj, p + 1 - obj);
                std::string id, mp;
                if (json_find(o, "id", id) && json_find(o, "min_p", mp) && mp != "null") {
                    const double d = std::atof(mp.c_str());
                    if (d >= 0.0 && d <= 1.01) r.min_p.push_back({id, d});
                }
            }
        }
    }
    return r;
}

// iterate the snapshots of every input file
template <class F>
int for_each_snapshot(const Opts& o, F&& fn) {
    const int64_t n = std::max<int64_t>(1, std::llround(o.snap * o.rate));
    const int64_t hop = std::max<int64_t>(1, std::llround((o.hop > 0.0 ? o.hop : o.snap) * o.rate));
    int bad = 0;
    for (const std::string& path : o.files) {
        const std::string fmt = fmt_of(o, path);
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", path.c_str());
            bad++;
            continue;
        }
        f.seekg(0, std::ios::end);
        const int64_t total = int64_t(f.tellg()) / bytes_per_sample(fmt);
        Snap s;
        if (total < n) {
            // shorter than a snapshot: the whole file as one (the core decides if it is enough)
            if (total > 0 && read_snap(f, fmt, 0, total, o.requant, s)) fn(path, 0, 0.0, s);
            continue;
        }
        int idx = 0;
        for (int64_t st = 0; st + n <= total; st += hop, idx++) {
            if (!read_snap(f, fmt, st, n, o.requant, s)) break;
            fn(path, idx, double(st) / o.rate, s);
        }
    }
    return bad;
}

std::string det_json(const sigclass::Detection& d) {
    return "\"offset_hz\":" + num(d.offset_hz, 9) + ",\"bw_hz\":" + num(d.bw_hz, 7) + ",\"snr_db\":" + num(d.snr_db, 4) +
           ",\"peak_db\":" + num(d.peak_db, 4) + ",\"duty\":" + num(d.duty, 4) + ",\"bursty\":" +
           (d.bursty ? "true" : "false") + ",\"fills_band\":" + (d.fills_band ? "true" : "false") +
           ",\"view\":" + std::to_string(d.view);
}

// ---- a model writer (selftest / bench / parity; tools/sigclass/model_io.py is the real one)
struct WLayer {
    int in, out, act;
    std::vector<float> w, b;
};
struct WView {
    int view;
    float temp = 1.0f;
    std::vector<std::pair<std::string, std::string>> classes;
    std::vector<float> mean, sd;
    std::vector<WLayer> layers;
};

void put32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; i++) o.push_back(uint8_t(v >> (8 * i))); }
void put16(std::vector<uint8_t>& o, uint16_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void put64(std::vector<uint8_t>& o, uint64_t v) { put32(o, uint32_t(v)); put32(o, uint32_t(v >> 32)); }
void putf(std::vector<uint8_t>& o, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(o, u); }

std::vector<uint8_t> model_bytes(const std::string& meta, const std::vector<WView>& views, uint64_t hash) {
    std::vector<uint8_t> o;
    const char magic[8] = {'K', 'S', 'I', 'G', 'C', 'L', 'S', '\0'};
    o.insert(o.end(), magic, magic + 8);
    put32(o, 1);
    put32(o, uint32_t(sigclass::FEATURE_LAYOUT_VERSION));
    put64(o, hash);
    put32(o, uint32_t(sigclass::feature_dim()));
    put32(o, uint32_t(meta.size()));
    o.insert(o.end(), meta.begin(), meta.end());
    put32(o, uint32_t(views.size()));
    for (const WView& v : views) {
        put32(o, uint32_t(v.view));
        put32(o, uint32_t(v.classes.size()));
        putf(o, v.temp);
        for (const auto& c : v.classes) {
            put16(o, uint16_t(c.first.size()));
            o.insert(o.end(), c.first.begin(), c.first.end());
            put16(o, uint16_t(c.second.size()));
            o.insert(o.end(), c.second.begin(), c.second.end());
        }
        for (float f : v.mean) putf(o, f);
        for (float f : v.sd) putf(o, f);
        put32(o, uint32_t(v.layers.size()));
        for (const WLayer& L : v.layers) {
            put32(o, uint32_t(L.in));
            put32(o, uint32_t(L.out));
            put32(o, uint32_t(L.act));
            for (float f : L.w) putf(o, f);
            for (float f : L.b) putf(o, f);
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : o) {
        c ^= b;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    put32(o, c ^ 0xFFFFFFFFu);
    return o;
}

std::vector<WView> random_views(int classes, int hidden, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    const int D = sigclass::feature_dim();
    std::vector<WView> vs;
    for (int v = 0; v < sigclass::NUM_VIEWS; v++) {
        WView w;
        w.view = v;
        w.temp = 1.3f;
        for (int c = 0; c < classes; c++) w.classes.push_back({"c" + std::to_string(c), "Class " + std::to_string(c)});
        for (int i = 0; i < D; i++) { w.mean.push_back(0.1f * nd(rng)); w.sd.push_back(0.5f + std::fabs(nd(rng))); }
        const int dims[] = {D, hidden, hidden / 2, classes};
        for (int l = 0; l < 3; l++) {
            WLayer L{dims[l], dims[l + 1], l < 2 ? 1 : 0, {}, {}};
            const float s = 1.0f / std::sqrt(float(L.in));
            for (int i = 0; i < L.in * L.out; i++) L.w.push_back(s * nd(rng));
            for (int i = 0; i < L.out; i++) L.b.push_back(0.1f * nd(rng));
            w.layers.push_back(std::move(L));
        }
        vs.push_back(std::move(w));
    }
    return vs;
}

sigclass::Config cfg_of(const Opts& o) {
    sigclass::Config c;
    if (o.spec_hop > 0) c.spectrogram_hop = o.spec_hop;
    if (o.second_view_max >= 0.0) c.second_view_max_hz = o.second_view_max;
    if (o.image_share >= 0.0) c.image_share = float(o.image_share);
    if (o.tone_merge >= 0) c.tone_merge = o.tone_merge != 0;
    if (o.lobe_widths >= 0.0) c.sidelobe_widths = float(o.lobe_widths);
    return c;
}

bool load_model(const Opts& o, sigclass::Model& m) {
    if (o.model.empty()) return false;
    std::string err;
    if (!m.load(o.model, &err)) {
        std::fprintf(stderr, "model: %s\n", err.c_str());
        std::exit(2);
    }
    return true;
}

// ------------------------------------------------------------------ modes

int mode_features(const Opts& o) {
    if (o.out.empty()) { std::fprintf(stderr, "features: --out PREFIX needed\n"); return 2; }
    FILE* ff = std::fopen((o.out + ".f32").c_str(), "wb");
    FILE* fj = std::fopen((o.out + ".jsonl").c_str(), "wb");
    if (!ff || !fj) { std::fprintf(stderr, "cannot write %s.*\n", o.out.c_str()); return 2; }
    sigclass::Analyzer an(cfg_of(o));
    std::vector<float> fo;
    size_t rows = 0;
    const int D = sigclass::feature_dim();
    const int bad = for_each_snapshot(o, [&](const std::string& path, int idx, double t0, const Snap& s) {
        const auto res = run(an, s, o.rate, nullptr, &fo);
        for (size_t i = 0; i < res.size(); i++) {
            std::fwrite(fo.data() + i * size_t(D), sizeof(float), size_t(D), ff);
            const std::string line = "{\"file\":" + jstr(path) + ",\"snap\":" + std::to_string(idx) + ",\"t0_s\":" +
                                     num(t0, 9) + "," + det_json(res[i].det) + "}\n";
            std::fputs(line.c_str(), fj);
            rows++;
        }
    });
    std::fclose(ff);
    std::fclose(fj);
    std::fprintf(stderr, "%zu rows x %d features -> %s.f32 / .jsonl\n", rows, D, o.out.c_str());
    return bad ? 1 : 0;
}

int mode_classify(const Opts& o) {
    sigclass::Model m;
    if (!load_model(o, m)) { std::fprintf(stderr, "classify: --model needed\n"); return 2; }
    sigclass::Analyzer an(cfg_of(o));
    sigclass::TrackerConfig tcfg;
    if (o.min_channel >= 0.0) tcfg.min_channel_hz = o.min_channel;
    if (o.core_ratio >= 0.0) tcfg.core_energy_ratio = o.core_ratio;
    if (o.hump_guard >= 0) tcfg.hump_guard = o.hump_guard != 0;
    sigclass::Tracker tr(tcfg);
    std::string cur;
    // S1 emulated (--settled 1): the service's known_bands() on this tracker (its looks: the snapshots of the file)
    const SureLevels sure = sure_levels(m.meta_json());
    int looks = 0;
    std::vector<sigclass::KnownBand> known;
    auto top3 = [&](int view, const std::vector<float>& p) {
        std::vector<int> idx(p.size());
        for (size_t i = 0; i < p.size(); i++) idx[i] = int(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return p[size_t(a)] > p[size_t(b)]; });
        std::string s = "[";
        for (size_t k = 0; k < std::min<size_t>(3, idx.size()); k++) {
            const auto& ci = m.class_info(view, idx[k]);
            s += (k ? "," : "") + std::string("{\"id\":") + jstr(ci.id) + ",\"name\":" + jstr(ci.name) +
                 ",\"p\":" + num(p[size_t(idx[k])], 4) + "}";
        }
        return s + "]";
    };
    const int bad = for_each_snapshot(o, [&](const std::string& path, int idx, double t0, const Snap& s) {
        if (path != cur) { tr.clear(); cur = path; looks = 0; }
        known.clear();
        if (o.settled && o.track) {
            // (tracker frame: --centre + offset; the analyzer's: --rf + offset, or the offset)
            const double shift = (o.rf > 0.0 ? o.rf : 0.0) - o.centre;
            for (const auto& k : tr.tracks()) {
                if (k.hits < 3 || k.best < 0 || k.best >= int(k.probs.size())) continue;
                if ((looks + k.id) % 4 == 0) continue;
                if (!(t0 - k.last_s < tcfg.expire_s - 1.0)) continue;   // (it survives this update's expiry)
                if (k.best_p >= sure.of(m.class_info(k.view, k.best).id)) known.push_back({k.lo_hz + shift, k.hi_hz + shift, k.view});
            }
            looks++;
        }
        const auto res = run(an, s, o.rate, &m, nullptr, o.rf, o.settled && o.track ? &known : nullptr);
        std::string line = "{\"file\":" + jstr(path) + ",\"snap\":" + std::to_string(idx) + ",\"t0_s\":" + num(t0, 9) +
                           ",\"detections\":[";
        for (size_t i = 0; i < res.size(); i++)
            line += (i ? "," : "") + std::string("{") + det_json(res[i].det) + ",\"top\":" +
                    top3(res[i].det.view, res[i].probs) + "}";
        line += "]";
        // (the detections also classified in view 0 / those that took its answer)
        line += ",\"second_view\":[" + std::to_string(an.last_timing().second_view) + "," +
                std::to_string(an.last_timing().second_view_taken) + "]";
        if (o.settled) line += ",\"known\":" + std::to_string(an.last_timing().known);
        if (o.track) {
            const auto& ts = tr.update(res, o.centre, t0);   // (offsets relative to the capture unless --centre)
            line += ",\"tracks\":[";
            for (size_t i = 0; i < ts.size(); i++) {
                const auto& t = ts[i];
                line += (i ? "," : "") + std::string("{\"id\":") + std::to_string(t.id) + ",\"centre_hz\":" +
                        num(t.centre_hz, 10) + ",\"bw_hz\":" + num(t.bw_hz, 6) + ",\"lo_hz\":" + num(t.lo_hz, 10) + ",\"hi_hz\":" + num(t.hi_hz, 10) + ",\"hits\":" + std::to_string(t.hits) +
                        ",\"first_s\":" + num(t.first_s) + ",\"last_s\":" + num(t.last_s) + ",\"view\":" +
                        std::to_string(t.view) + ",\"updated\":" + (t.updated ? "true" : "false") + ",\"top\":" +
                        top3(t.view, t.probs) + "}";
            }
            line += "]";
        }
        std::puts((line + "}").c_str());
    });
    return bad ? 1 : 0;
}

int mode_bench(const Opts& o) {
    if (o.files.empty()) { std::fprintf(stderr, "bench FILE\n"); return 2; }
    sigclass::Model m;
    if (!load_model(o, m)) {
        const auto bytes = model_bytes("{}", random_views(48, 256, 7), sigclass::layout_hash());
        std::string err;
        if (!m.load_from_memory(bytes.data(), bytes.size(), &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    Opts one = o;
    one.files.resize(1);
    Snap snap;
    bool got = false;
    for_each_snapshot(one, [&](const std::string&, int idx, double, const Snap& s) {
        if (idx == 0) { snap = s; got = true; }
    });
    if (!got) { std::fprintf(stderr, "no snapshot\n"); return 2; }
    sigclass::Analyzer an(cfg_of(o));
    std::vector<float> fo;
    auto res = run(an, snap, o.rate, &m, &fo);   // warm-up (plans, buffers)
    const int reps = std::max(1, o.reps);
    sigclass::Analyzer::Timing acc;
    double wall = 0.0, det_only = 0.0;
    for (int r = 0; r < reps; r++) {
        const double t0 = now_ms();
        res = run(an, snap, o.rate, &m, nullptr);
        wall += now_ms() - t0;
        const auto& t = an.last_timing();
        acc.convert_ms += t.convert_ms; acc.frontend_ms += t.frontend_ms; acc.detect_ms += t.detect_ms;
        acc.channelize_ms += t.channelize_ms; acc.features_ms += t.features_ms; acc.model_ms += t.model_ms;
        acc.second_view += t.second_view; acc.second_view_taken += t.second_view_taken;
        const double t1 = now_ms();
        run(an, snap, o.rate, nullptr, nullptr);
        det_only += now_ms() - t1;
    }
    const double k = 1.0 / reps;
    const int ns = int(res.size());
    int per_view[sigclass::NUM_VIEWS] = {0, 0, 0, 0};
    for (const auto& s : res) per_view[s.det.view]++;
    std::printf("snapshot %.3f s at %.0f Hz, %d signals (views 24k/96k/480k/2.4M: %d/%d/%d/%d), %d reps\n",
                double(o.snap), o.rate, ns, per_view[0], per_view[1], per_view[2], per_view[3], reps);
    std::printf("  convert     %8.3f ms\n", acc.convert_ms * k);
    std::printf("  frontend    %8.3f ms  (long FFTs + PSD + short spectrogram)\n", acc.frontend_ms * k);
    std::printf("  detect      %8.3f ms\n", acc.detect_ms * k);
    std::printf("  channelize  %8.3f ms  (%.3f ms / signal)\n", acc.channelize_ms * k, ns ? acc.channelize_ms * k / ns : 0.0);
    std::printf("  features    %8.3f ms  (%.3f ms / signal)\n", acc.features_ms * k, ns ? acc.features_ms * k / ns : 0.0);
    std::printf("  model       %8.3f ms  (%.3f ms / signal)\n", acc.model_ms * k, ns ? acc.model_ms * k / ns : 0.0);
    std::printf("  total       %8.3f ms per snapshot (wall); detector only %.3f ms\n", wall * k, det_only * k);
    std::printf("  second view %d of %d detections also classified in view 0, %d took its answer\n", acc.second_view / reps, ns,
                acc.second_view_taken / reps);
    return 0;
}

int mode_view(const Opts& o) {
    if (o.files.empty() || o.out.empty()) { std::fprintf(stderr, "view --offset HZ --view V --out X.cf32 FILE\n"); return 2; }
    Opts one = o;
    one.files.resize(1);
    int rc = 2;
    for_each_snapshot(one, [&](const std::string&, int idx, double, const Snap& s) {
        if (idx != 0) return;
        sigclass::Analyzer an(cfg_of(o));
        const auto res = run(an, s, o.rate, nullptr, nullptr);
        for (const auto& r : res) std::fprintf(stderr, "{%s}\n", det_json(r.det).c_str());
        std::vector<cf> v;
        if (!an.extract_view(o.offset, o.view, v)) return;
        FILE* f = std::fopen(o.out.c_str(), "wb");
        if (!f) return;
        std::fwrite(v.data(), sizeof(cf), v.size(), f);
        std::fclose(f);
        // the detector's noise floor too (fine bins, -fs/2 first): X.cf32.floor.f32
        const auto& fl = an.last_floor();
        if (FILE* g = std::fopen((o.out + ".floor.f32").c_str(), "wb")) {
            std::fwrite(fl.data(), sizeof(float), fl.size(), g);
            std::fclose(g);
        }
        std::fprintf(stderr, "%zu samples at %.1f Hz -> %s\n", v.size(), o.rate / sigclass::VIEW_DECIM[o.view], o.out.c_str());
        rc = 0;
    });
    return rc;
}

int mode_forward(const Opts& o) {
    sigclass::Model m;
    if (!load_model(o, m) || o.in.empty() || o.out.empty()) { std::fprintf(stderr, "forward --model M --view V --in X.f32 --out P.f32\n"); return 2; }
    const int D = sigclass::feature_dim();
    std::ifstream f(o.in, std::ios::binary);
    std::vector<float> x;
    float buf[1];
    while (f.read(reinterpret_cast<char*>(buf), 4)) x.push_back(buf[0]);
    const size_t rows = x.size() / size_t(D);
    FILE* fo = std::fopen(o.out.c_str(), "wb");
    if (!fo) return 2;
    std::vector<float> p;
    for (size_t r = 0; r < rows; r++) {
        if (!m.predict(o.view, x.data() + r * size_t(D), p)) { std::fprintf(stderr, "no model for view %d\n", o.view); return 2; }
        if (o.rf > 0) m.rf_mask(o.view, o.rf - 0.5 * o.offset, o.rf + 0.5 * o.offset, p);   // (parity of the RF mask: --rf F --offset BW)
        std::fwrite(p.data(), sizeof(float), p.size(), fo);
    }
    std::fclose(fo);
    std::fprintf(stderr, "%zu rows -> %s\n", rows, o.out.c_str());
    return 0;
}

// ------------------------------------------------------------------ selftest

int failures = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) failures++;
}

std::vector<uint8_t> to_u8(const std::vector<cf>& x) {
    std::vector<uint8_t> u(2 * x.size());
    for (size_t i = 0; i < x.size(); i++) {
        u[2 * i] = uint8_t(std::clamp(std::lround(x[i].real() * 127.5f + 127.5f), 0L, 255L));
        u[2 * i + 1] = uint8_t(std::clamp(std::lround(x[i].imag() * 127.5f + 127.5f), 0L, 255L));
    }
    return u;
}

// power of x at frequency f (Hz) by correlation, normalised to the amplitude^2 of a unit tone
double tone_power(const std::vector<cf>& x, double f, double fs, size_t skip) {
    std::complex<double> s(0.0, 0.0);
    size_t n = 0;
    for (size_t i = skip; i + skip < x.size(); i++, n++) {
        const double ph = -2.0 * PI * f * double(i) / fs;
        s += std::complex<double>(x[i]) * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return n ? std::norm(s / double(n)) : 0.0;
}

int mode_selftest() {
    const double fs = 2.4e6;
    const size_t N = sigclass::NOMINAL_SNAPSHOT_SAMPLES;
    const int D = sigclass::feature_dim();
    {
        const std::string lj = sigclass::layout_json();
        check(D >= 600 && D <= 4096 && lj.find("\"dim\":" + std::to_string(D)) != std::string::npos, "layout",
              "D=" + std::to_string(D));
    }
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    sigclass::Analyzer an;
    // ---- channelizer: tones through each view (complex float path: no quantisation)
    {
        std::vector<cf> x(N);
        const double offs[3] = {123456.7, -401234.5, 37020.0};
        const double rates[4] = {24000, 96000, 480000, 2400000};
        bool all = true;
        std::string det;
        for (int v = 0; v < 3; v++) {
            const double fv = rates[v];
            const double centre = offs[v];
            // in-band tones at 0, +-0.2, +-0.39 x rate, out-of-band at +-0.52 / 0.8 x rate
            const double rel_in[] = {0.0, 0.2, -0.2, 0.39, -0.39};
            const double rel_out[] = {0.52, -0.52, 0.8, -1.3};
            for (double r : rel_in) {
                for (size_t i = 0; i < N; i++) {
                    const double ph = 2.0 * PI * (centre + r * fv) * double(i) / fs;
                    x[i] = cf(float(0.5 * std::cos(ph)), float(0.5 * std::sin(ph)));
                }
                an.analyze(x.data(), N, fs, nullptr, nullptr);
                std::vector<cf> y;
                an.extract_view(centre, v, y);
                const double p = tone_power(y, r * fv - (std::lround(centre / (fs / 120000)) * (fs / 120000) - centre), fv, 200);
                const double db = 10.0 * std::log10(p / 0.25);
                if (std::fabs(db) > 1.0) all = false;
                char b[96];
                std::snprintf(b, sizeof b, " v%d %+.2f:%+.2fdB", v, r, db);
                det += b;
            }
            for (double r : rel_out) {
                for (size_t i = 0; i < N; i++) {
                    const double ph = 2.0 * PI * (centre + r * fv) * double(i) / fs;
                    x[i] = cf(float(0.5 * std::cos(ph)), float(0.5 * std::sin(ph)));
                }
                an.analyze(x.data(), N, fs, nullptr, nullptr);
                std::vector<cf> y;
                an.extract_view(centre, v, y);
                double e = 0.0;
                for (size_t i = 200; i + 200 < y.size(); i++) e += std::norm(y[i]);
                e /= double(y.size() - 400);
                const double db = 10.0 * std::log10(std::max(e, 1e-30) / 0.25);
                if (db > -50.0) all = false;
                char b[96];
                std::snprintf(b, sizeof b, " v%d %+.2f:%.0fdB", v, r, db);
                det += b;
            }
        }
        check(all, "channelizer passband within 1 dB, out-of-view >= 50 dB down", det);
    }
    // ---- features: finite and deterministic on odd inputs
    {
        struct Case {
            const char* name;
            std::vector<uint8_t> u;
        };
        std::vector<Case> cases;
        cases.push_back({"all-127", std::vector<uint8_t>(2 * N, 127)});
        cases.push_back({"all-0", std::vector<uint8_t>(2 * N, 0)});
        {
            std::vector<uint8_t> u(2 * N);
            for (size_t i = 0; i < 2 * N; i++) u[i] = (i / 2) % 2 ? 255 : 0;
            cases.push_back({"clipped square", u});
        }
        {
            std::vector<cf> x(N);
            for (size_t i = 0; i < N; i++) {
                const double ph = 2.0 * PI * 100e3 * double(i) / fs;
                x[i] = cf(float(0.99 * std::cos(ph)), float(0.99 * std::sin(ph)));
            }
            cases.push_back({"full-scale carrier", to_u8(x)});
        }
        {
            std::vector<cf> x(N);
            for (size_t i = 0; i < N; i++) x[i] = cf(0.05f * nd(rng), 0.05f * nd(rng));
            cases.push_back({"noise", to_u8(x)});
        }
        bool fin = true, det = true;
        std::string info;
        for (auto& c : cases) {
            std::vector<float> f1, f2;
            const auto r1 = an.analyze(c.u.data(), N, fs, nullptr, &f1);
            const auto r2 = an.analyze(c.u.data(), N, fs, nullptr, &f2);
            for (float v : f1) if (!is_finite_value(v) || std::fabs(v) > 16.0f) fin = false;
            if (f1 != f2 || r1.size() != r2.size()) det = false;
            // also features of a detection forced on this input in every view
            for (int v = 0; v < 4; v++) {
                sigclass::Detection d;
                d.offset_hz = 1000.0;
                d.bw_hz = 5000.0;
                d.view = v;
                std::vector<float> fv;
                an.features_for(d, fv);
                for (float x : fv) if (!is_finite_value(x) || std::fabs(x) > 16.0f) fin = false;
            }
            info += std::string(" ") + c.name + ":" + std::to_string(r1.size());
        }
        // very short input
        {
            std::vector<uint8_t> u(2 * 9000, 128);
            std::vector<float> f;
            an.analyze(u.data(), 9000, fs, nullptr, &f);
            for (float v : f) if (!is_finite_value(v)) fin = false;
            std::vector<uint8_t> one(2, 128);
            an.analyze(one.data(), 1, fs, nullptr, &f);
        }
        check(fin, "features finite and bounded (|v| <= 16)", "detections" + info);
        check(det, "repeated calls identical");
    }
    // ---- model: a random model loads, predicts a distribution, a wrong hash fails
    {
        const auto bytes = model_bytes("{\"test\":1}", random_views(5, 64, 3), sigclass::layout_hash());
        sigclass::Model m;
        std::string err;
        const bool ok = m.load_from_memory(bytes.data(), bytes.size(), &err);
        std::vector<float> x(size_t(D), 0.3f), p;
        bool good = ok && m.classes(2) == 5 && m.meta_json() == "{\"test\":1}" && m.class_info(1, 4).id == "c4";
        if (good && m.predict(1, x.data(), p)) {
            double s = 0.0;
            for (float v : p) s += v;
            good = std::fabs(s - 1.0) < 1e-5;
        } else {
            good = false;
        }
        check(good, "model load + predict", err);
        const auto bad = model_bytes("{}", random_views(5, 64, 3), sigclass::layout_hash() ^ 1);
        sigclass::Model m2;
        std::string e2;
        const bool refused = !m2.load_from_memory(bad.data(), bad.size(), &e2) && e2.find("layout") != std::string::npos;
        check(refused, "model with a wrong layout hash refused", e2);
        auto corrupt = bytes;
        corrupt[corrupt.size() / 2] ^= 0x40;
        std::string e3;
        check(!m2.load_from_memory(corrupt.data(), corrupt.size(), &e3), "corrupt model refused", e3);
    }
    // ---- tracker: appear, drift 200 Hz, disappear, another nearby, expiry
    {
        sigclass::TrackerConfig tc;
        tc.expire_s = 3.0;
        sigclass::Tracker tr(tc);
        auto det = [](double off, double bw, std::vector<float> p) {
            sigclass::Scored s;
            s.det.offset_hz = off;
            s.det.bw_hz = bw;
            s.det.view = 0;
            s.probs = p;
            return s;
        };
        bool ok = true;
        int id1 = -1;
        for (int k = 0; k < 5; k++) {
            const auto& ts = tr.update({det(10000.0 + 50.0 * k, 9000.0, {0.7f, 0.2f, 0.1f})}, 446e6, k * 1.0);
            if (ts.size() != 1) ok = false;
            if (k == 0) id1 = ts[0].id;
            else if (ts[0].id != id1) ok = false;
        }
        // gone for 2 s (still kept), another signal 25 kHz up appears
        auto ts = tr.update({det(35000.0, 9000.0, {0.1f, 0.1f, 0.8f})}, 446e6, 6.0);
        int id2 = -1;
        for (const auto& t : ts) if (t.id != id1) id2 = t.id;
        if (ts.size() != 2 || id2 < 0) ok = false;
        // after expiry only the second remains
        ts = tr.update({det(35100.0, 9000.0, {0.1f, 0.1f, 0.8f})}, 446e6, 8.5);
        if (ts.size() != 1 || ts[0].id != id2 || ts[0].best != 2) ok = false;
        // a retune that keeps the signal in band keeps its id
        ts = tr.update({det(35100.0 - 100e3, 9000.0, {0.1f, 0.1f, 0.8f})}, 446e6 + 100e3, 9.0);
        if (ts.size() != 1 || ts[0].id != id2 || ts[0].hits != 3) ok = false;
        check(ok, "tracker ids, drift, expiry, retune");
    }
    // ---- detector: noise only, and a carrier + an FSK-like block
    {
        std::vector<cf> x(N);
        int fa = 0;
        for (int k = 0; k < 4; k++) {
            for (size_t i = 0; i < N; i++) x[i] = cf(0.08f * nd(rng), 0.08f * nd(rng));
            const auto u = to_u8(x);
            fa += int(an.analyze(u.data(), N, fs, nullptr, nullptr).size());
        }
        check(fa <= 4, "noise only: false detections", std::to_string(fa) + " in 4 snapshots");
        for (size_t i = 0; i < N; i++) {
            const double ph = 2.0 * PI * 250e3 * double(i) / fs;
            x[i] = cf(0.08f * nd(rng) + float(0.05 * std::cos(ph)), 0.08f * nd(rng) + float(0.05 * std::sin(ph)));
        }
        const auto u = to_u8(x);
        const auto r = an.analyze(u.data(), N, fs, nullptr, nullptr);
        bool found = false;
        for (const auto& s : r) found |= std::fabs(s.det.offset_hz - 250e3) < 200.0;
        check(found, "carrier detected at +250 kHz", r.empty() ? "none" : det_json(r[0].det));
    }
    std::printf("%s (%d failure%s)\n", failures ? "SELFTEST FAILED" : "selftest passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

void usage() {
    std::fprintf(stderr,
                 "sigclass_tool layout | features --out PREFIX FILE... | classify --model M [--track] FILE... |\n"
                 "              bench FILE | view --offset HZ --view V --out X.cf32 FILE |\n"
                 "              forward --model M --view V --in X.f32 --out P.f32 | selftest\n"
                 "  opts: --rate HZ --format cu8|cs16|cf32 --snap S --hop S --list FILE --centre HZ --rf HZ --requant --reps N\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    Opts o;
    o.mode = argv[1];
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--out") o.out = val();
        else if (a == "--model") o.model = val();
        else if (a == "--list") o.list = val();
        else if (a == "--format") o.format = val();
        else if (a == "--in") o.in = val();
        else if (a == "--rate") o.rate = std::atof(val().c_str());
        else if (a == "--snap") o.snap = std::atof(val().c_str());
        else if (a == "--hop") o.hop = std::atof(val().c_str());
        else if (a == "--centre" || a == "--center") o.centre = std::atof(val().c_str());
        else if (a == "--rf") o.rf = std::atof(val().c_str());
        else if (a == "--offset") o.offset = std::atof(val().c_str());
        else if (a == "--view") o.view = std::clamp(std::atoi(val().c_str()), 0, sigclass::NUM_VIEWS - 1);
        else if (a == "--reps") o.reps = std::atoi(val().c_str());
        else if (a == "--spec-hop") o.spec_hop = std::atoi(val().c_str());
        else if (a == "--second-view-max") o.second_view_max = std::atof(val().c_str());
        else if (a == "--image-share") o.image_share = std::atof(val().c_str());
        else if (a == "--tone-merge") o.tone_merge = std::atoi(val().c_str());
        else if (a == "--lobe-widths") o.lobe_widths = std::atof(val().c_str());
        else if (a == "--min-channel") o.min_channel = std::atof(val().c_str());
        else if (a == "--core-ratio") o.core_ratio = std::atof(val().c_str());
        else if (a == "--hump-guard") o.hump_guard = std::atoi(val().c_str());
        else if (a == "--settled") o.settled = std::atoi(val().c_str());
        else if (a == "--track") o.track = true;
        else if (a == "--requant") o.requant = true;
        else if (a.size() > 1 && a[0] == '-' && a[1] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else o.files.push_back(a);
    }
    if (!o.list.empty()) {
        std::ifstream lf(o.list);
        std::string line;
        while (std::getline(lf, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) o.files.push_back(line);
        }
    }
    if (!(o.rate > 0.0) || !(o.snap > 0.0)) { std::fprintf(stderr, "bad --rate / --snap\n"); return 2; }
    if (o.mode == "layout") { std::puts(sigclass::layout_json().c_str()); return 0; }
    if (o.mode == "features") return mode_features(o);
    if (o.mode == "classify") return mode_classify(o);
    if (o.mode == "bench") return mode_bench(o);
    if (o.mode == "view") return mode_view(o);
    if (o.mode == "forward") return mode_forward(o);
    if (o.mode == "selftest") return mode_selftest();
    usage();
    return 2;
}
