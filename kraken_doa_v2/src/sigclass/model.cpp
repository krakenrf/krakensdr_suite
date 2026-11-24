// Signal classifier: the model file and the per-view MLP evaluator.
//
// sigclass.model, little-endian (tools/sigclass/model_io.py writes and reads it):
//   char[8]  "KSIGCLS\0"
//   u32      format version (1 - 4; this build reads all)
//   u32      feature layout version (FEATURE_LAYOUT_VERSION)
//   u64      feature layout hash (layout_hash(); must match the code)
//   u32      feature dim D
//   u32      meta_len, then meta_len bytes of UTF-8 JSON (opaque here)
//   u32      number of view entries (0..4), then per entry:
//              u32 view (0..3), u32 classes C, f32 softmax temperature
//                (TEMP_MIN .. TEMP_MAX)
//              C x { u16 id_len, id bytes, u16 name_len, name bytes }
//              f32[D] mean, f32[D] std   (input z = (x - mean) / std)
//              format 2 only: u32 front end (0 = none, 1 = Conv1D, 2 = the sequence
//                not read: u32 seq_off, u32 S, u32 Cin - the MLP takes the rest), for 1:
//                u32 seq_off, u32 S, u32 Cin  (the sequence: features[seq_off ..
//                  seq_off + Cin x S), channel-major, used as they are)
//                u32 K filters, u32 L taps, u32 pools (1 = max, 2 = max + mean)
//                f32[K x Cin x L] W, f32[K] b, f32[pools x K] mean, f32[pools x K] std
//                y[k][t] = ReLU(b[k] + sum_c sum_l W[k][c][l] x[c][t + l]),
//                t = 0 .. S - L; pooled over t; standardised
//                the MLP then reads the standardised features WITHOUT the
//                sequence (in their order) followed by the pools x K pooled values
//              u32 layers, per layer: u32 in, u32 out, u32 activation
//                (0 = none, 1 = ReLU), f32[out x in] W (row-major), f32[out] b
//              (first in = D, or D - Cin x S + pools x K with a front end;
//               in = previous out, last out = C, last activation 0)
//   format 4: a view may have up to MEMBERS_MAX entries - the members of an
//              ensemble (fits of the same data): every entry of a view has the
//              same classes (ids and names, in order); the view's probabilities
//              are the mean of the members' (each with its own temperature,
//              normalisation, front end and layers). Formats 1 - 3: one entry a view
//   formats 3 - 4 - the RF plausibility table (no table = no mask):
//   u32      classes with an entry (<= 4096), per class:
//              u16 id_len, id bytes, u8 flags (bit 0 generic: plausible anywhere),
//              u32 ranges (<= 64), f64[2 x ranges] [lo, hi] Hz (0 = no range: anywhere)
//   u32      exclusive bands (<= 256), per band: f64 lo, f64 hi, u32 only (<= 256),
//              per class allowed there: u16 id_len, id bytes
//   u32      CRC-32 (IEEE 802.3, zlib's) of every byte before it
// probabilities = softmax(last layer / temperature).

#include "sigclass/sigclass.hpp"
#include "sc_prof.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sigclass {

namespace {

constexpr char MAGIC[8] = {'K', 'S', 'I', 'G', 'C', 'L', 'S', '\0'};
constexpr uint32_t FORMAT_VERSION = 4, FORMAT_MIN = 1;
constexpr uint32_t MEMBERS_MAX = 16;   // ensemble members a view (format 4)
constexpr uint32_t RF_CLASSES_MAX = 4096, RF_RANGES_MAX = 64, RF_BANDS_MAX = 256, RF_ONLY_MAX = 256, RF_ID_MAX = 256;
// the front end's bounds (a hostile file must not make it allocate or loop
// without limit): taps, filters, channels, sequence length
constexpr uint32_t CONV_K_MAX = 256, CONV_L_MAX = 256, CONV_C_MAX = 8, CONV_S_MAX = 1u << 16;
// the loader's bounds: a temperature outside them (1e-39 passed the old > 0
// test) made logit / temperature overflow and every probability NaN; the
// scaled logits are clamped to +-LOGIT_MAX as well
constexpr float TEMP_MIN = 1e-3f, TEMP_MAX = 1e3f, LOGIT_MAX = 1e4f;

struct Layer {
    int in = 0, out = 0, act = 0;
    std::vector<float> w, b;
};

struct Conv {
    bool on = false, skip = false;   // skip: the sequence slice is not read (no front end)
    int off = 0, S = 0, C = 0, K = 0, L = 0, pools = 1;
    std::vector<float> w, b, pmean, pinv;
};

// per class of a view: where it can exist (from the model's RF table)
struct RfClass {
    bool generic = false;
    std::vector<std::pair<double, double>> ranges;   // empty: anywhere
    std::vector<int> bands;                          // the exclusive bands that allow it
};
struct RfBand {
    double lo = 0, hi = 0;
};

// one network of a view (format 4: one of its ensemble members)
struct Net {
    Conv conv;
    float temperature = 1.0f;
    std::vector<float> mean, inv_std;
    std::vector<Layer> layers;
};

struct ViewModel {
    bool present = false;
    std::vector<RfClass> rf;   // per class (empty: no table)
    std::vector<ClassInfo> classes;
    std::vector<Net> nets;     // 1, or the ensemble's members (format 4)
};

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    bool take(void* dst, size_t k) {
        if (!ok || n - i < k) { ok = false; return false; }
        std::memcpy(dst, p + i, k);
        i += k;
        return true;
    }
    uint32_t u32() {
        uint8_t b[4] = {0, 0, 0, 0};
        take(b, 4);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
    uint16_t u16() {
        uint8_t b[2] = {0, 0};
        take(b, 2);
        return uint16_t(b[0] | b[1] << 8);
    }
    double f64() {
        const uint64_t v = u64();
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    uint8_t u8() {
        uint8_t b = 0;
        take(&b, 1);
        return b;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | uint64_t(u32()) << 32;
    }
    float f32() {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    bool floats(std::vector<float>& v, size_t k) {
        if (!ok || (n - i) / 4 < k) { ok = false; return false; }
        v.resize(k);
        for (size_t q = 0; q < k; q++) v[q] = f32();
        return ok;
    }
    std::string str(size_t k) {
        std::string s;
        if (!ok || n - i < k) { ok = false; return s; }
        s.assign(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
};

uint32_t crc32(const uint8_t* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool finite64(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    return (u & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

bool finite32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return (u & 0x7F800000u) != 0x7F800000u;
}

const ClassInfo& no_class() {
    static const ClassInfo c;
    return c;
}

}  // namespace

struct Model::Impl {
    std::string meta;
    ViewModel views[NUM_VIEWS];
    std::vector<RfBand> bands;
    bool has_rf = false;
};

Model::Model() = default;
Model::~Model() = default;
Model::Model(const Model&) = default;
Model& Model::operator=(const Model&) = default;

bool Model::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string data = ss.str();
    return load_from_memory(data.data(), data.size(), err);
}

bool Model::load_from_memory(const void* data, size_t size, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (!p || size < 40) return fail("model file too short");
    if (std::memcmp(p, MAGIC, 8) != 0) return fail("not a sigclass model (bad magic)");
    const uint32_t want = crc32(p, size - 4);
    Reader tail{p + size - 4, 4};
    if (tail.u32() != want) return fail("model file corrupt (checksum mismatch)");
    Reader r{p, size - 4};
    r.i = 8;
    const uint32_t fv = r.u32();
    if (fv < FORMAT_MIN || fv > FORMAT_VERSION)
        return fail("model format version " + std::to_string(fv) + ", this build reads " + std::to_string(FORMAT_MIN) +
                    ".." + std::to_string(FORMAT_VERSION));
    const uint32_t lv = r.u32();
    const uint64_t lh = r.u64();
    const uint32_t dim = r.u32();
    if (lv != uint32_t(FEATURE_LAYOUT_VERSION) || lh != layout_hash() || dim != uint32_t(feature_dim())) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "model was trained on feature layout v%u hash %016llx dim %u; this build has v%d hash %016llx "
                      "dim %d - retrain or export with the matching layout",
                      lv, (unsigned long long)lh, dim, FEATURE_LAYOUT_VERSION, (unsigned long long)layout_hash(),
                      feature_dim());
        return fail(buf);
    }
    auto impl = std::make_shared<Impl>();
    const uint32_t ml = r.u32();
    impl->meta = r.str(ml);
    const uint32_t nv = r.u32();
    if (!r.ok || nv > uint32_t(NUM_VIEWS) * (fv >= 4 ? MEMBERS_MAX : 1u)) return fail("bad view count");
    const int D = feature_dim();
    for (uint32_t q = 0; q < nv; q++) {
        const uint32_t v = r.u32();
        if (!r.ok || v >= uint32_t(NUM_VIEWS)) return fail("bad view index");
        ViewModel& vm = impl->views[v];
        // (format 4: another entry of a view is one more member of its ensemble)
        if (vm.present && (fv < 4 || vm.nets.size() >= MEMBERS_MAX)) return fail("view " + std::to_string(v) + " twice");
        const uint32_t C = r.u32();
        Net net;
        net.temperature = r.f32();
        if (!r.ok || C < 1 || C > 100000) return fail("bad class count");
        if (!finite32(net.temperature) || !(net.temperature >= TEMP_MIN && net.temperature <= TEMP_MAX))
            return fail("bad temperature");
        std::vector<ClassInfo> cls(C);
        for (uint32_t c = 0; c < C; c++) {
            cls[c].id = r.str(r.u16());
            cls[c].name = r.str(r.u16());
        }
        if (!r.ok) return fail("truncated class list");
        if (vm.present) {
            bool same = cls.size() == vm.classes.size();
            for (size_t c = 0; same && c < cls.size(); c++) same = cls[c].id == vm.classes[c].id && cls[c].name == vm.classes[c].name;
            if (!same) return fail("the members of view " + std::to_string(v) + " have different classes");
        } else {
            vm.classes = std::move(cls);
        }
        std::vector<float> sd;
        if (!r.floats(net.mean, size_t(D)) || !r.floats(sd, size_t(D))) return fail("truncated normalisation");
        net.inv_std.resize(size_t(D));
        for (int i = 0; i < D; i++) {
            if (!finite32(net.mean[size_t(i)]) || !finite32(sd[size_t(i)])) return fail("non-finite normalisation");
            net.inv_std[size_t(i)] = std::fabs(sd[size_t(i)]) > 1e-12f ? 1.0f / sd[size_t(i)] : 0.0f;
        }
        int prev = D;
        if (fv >= 2) {
            const uint32_t fe = r.u32();
            if (!r.ok || fe > 2) return fail("bad front end in view " + std::to_string(v));
            if (fe == 2) {
                Conv& c = net.conv;
                const uint32_t off = r.u32(), S = r.u32(), C = r.u32();
                if (!r.ok || C < 1 || C > CONV_C_MAX || S < 1 || S > CONV_S_MAX || uint64_t(off) + uint64_t(C) * S > uint64_t(D) ||
                    uint64_t(C) * S >= uint64_t(D))
                    return fail("bad sequence slice in view " + std::to_string(v));
                c.skip = true;
                c.off = int(off);
                c.S = int(S);
                c.C = int(C);
                prev = D - int(C * S);
            }
            if (fe == 1) {
                Conv& c = net.conv;
                const uint32_t off = r.u32(), S = r.u32(), C = r.u32(), K = r.u32(), L = r.u32(), pools = r.u32();
                // every product in 64 bits, bounded before anything is allocated
                if (!r.ok || C < 1 || C > CONV_C_MAX || S < 1 || S > CONV_S_MAX || K < 1 || K > CONV_K_MAX || L < 1 ||
                    L > CONV_L_MAX || L > S || pools < 1 || pools > 2 || uint64_t(off) + uint64_t(C) * S > uint64_t(D))
                    return fail("bad front end shape in view " + std::to_string(v));
                const uint64_t nw = uint64_t(K) * C * L;
                if (nw > uint64_t(r.n - r.i) / 4 || !r.floats(c.w, size_t(nw)) || !r.floats(c.b, size_t(K)))
                    return fail("truncated front end");
                std::vector<float> psd;
                if (!r.floats(c.pmean, size_t(pools) * K) || !r.floats(psd, size_t(pools) * K)) return fail("truncated front end");
                for (float x : c.w) if (!finite32(x)) return fail("non-finite front end weight");
                for (float x : c.b) if (!finite32(x)) return fail("non-finite front end bias");
                c.pinv.resize(psd.size());
                for (size_t i = 0; i < psd.size(); i++) {
                    if (!finite32(c.pmean[i]) || !finite32(psd[i])) return fail("non-finite front end normalisation");
                    c.pinv[i] = std::fabs(psd[i]) > 1e-12f ? 1.0f / psd[i] : 0.0f;
                }
                c.on = true;
                c.off = int(off);
                c.S = int(S);
                c.C = int(C);
                c.K = int(K);
                c.L = int(L);
                c.pools = int(pools);
                prev = D - int(C * S) + int(pools * K);
            }
        }
        const uint32_t nl = r.u32();
        if (!r.ok || nl < 1 || nl > 64) return fail("bad layer count");
        for (uint32_t l = 0; l < nl; l++) {
            Layer L;
            L.in = int(r.u32());
            L.out = int(r.u32());
            L.act = int(r.u32());
            if (!r.ok || L.in != prev || L.out < 1 || L.out > 1 << 20 || (L.act != 0 && L.act != 1))
                return fail("bad layer shape in view " + std::to_string(v));
            // in x out up to 2^40: in 64 bits, against the bytes left before
            // it becomes a size_t (32-bit builds - armv7l - would wrap)
            const uint64_t nw = uint64_t(uint32_t(L.in)) * uint64_t(uint32_t(L.out));
            if (nw > uint64_t(r.n - r.i) / 4 || !r.floats(L.w, size_t(nw)) || !r.floats(L.b, size_t(L.out)))
                return fail("truncated weights");
            for (float x : L.w) if (!finite32(x)) return fail("non-finite weight");
            for (float x : L.b) if (!finite32(x)) return fail("non-finite bias");
            prev = L.out;
            net.layers.push_back(std::move(L));
        }
        if (prev != int(C) || net.layers.back().act != 0) return fail("last layer must be linear with one output per class");
        vm.nets.push_back(std::move(net));
        vm.present = true;
    }
    if (fv >= 3) {
        // the RF table: by class id onto every view's classes
        struct Entry {
            std::string id;
            bool generic = false;
            std::vector<std::pair<double, double>> ranges;
        };
        const uint32_t ne = r.u32();
        if (!r.ok || ne > RF_CLASSES_MAX) return fail("bad RF table size");
        std::vector<Entry> ent;
        ent.reserve(ne);
        for (uint32_t q = 0; q < ne; q++) {
            Entry e;
            const uint16_t il = r.u16();
            if (!r.ok || il > RF_ID_MAX) return fail("bad RF class id");
            e.id = r.str(il);
            e.generic = (r.u8() & 1u) != 0;
            const uint32_t nr = r.u32();
            if (!r.ok || nr > RF_RANGES_MAX) return fail("bad RF range count");
            for (uint32_t k = 0; k < nr; k++) {
                const double lo = r.f64(), hi = r.f64();
                if (!r.ok || !finite64(lo) || !finite64(hi) || !(lo >= 0.0) || !(hi > lo)) return fail("bad RF range");
                e.ranges.push_back({lo, hi});
            }
            ent.push_back(std::move(e));
        }
        const uint32_t nb = r.u32();
        if (!r.ok || nb > RF_BANDS_MAX) return fail("bad RF band count");
        std::vector<std::vector<std::string>> only(nb);
        for (uint32_t b = 0; b < nb; b++) {
            RfBand bd;
            bd.lo = r.f64();
            bd.hi = r.f64();
            if (!r.ok || !finite64(bd.lo) || !finite64(bd.hi) || !(bd.lo >= 0.0) || !(bd.hi > bd.lo)) return fail("bad RF band");
            const uint32_t no = r.u32();
            if (!r.ok || no > RF_ONLY_MAX) return fail("bad RF band class count");
            for (uint32_t k = 0; k < no; k++) {
                const uint16_t il = r.u16();
                if (!r.ok || il > RF_ID_MAX) return fail("bad RF band class id");
                only[b].push_back(r.str(il));
            }
            impl->bands.push_back(bd);
        }
        if (!r.ok) return fail("truncated RF table");
        for (ViewModel& vm : impl->views) {
            if (!vm.present) continue;
            vm.rf.assign(vm.classes.size(), RfClass{});
            for (size_t c = 0; c < vm.classes.size(); c++) {
                for (const Entry& e : ent)
                    if (e.id == vm.classes[c].id) {
                        vm.rf[c].generic = e.generic;
                        vm.rf[c].ranges = e.ranges;
                    }
                for (uint32_t b = 0; b < nb; b++)
                    for (const std::string& id : only[b])
                        if (id == vm.classes[c].id) vm.rf[c].bands.push_back(int(b));
            }
        }
        impl->has_rf = true;
    }
    if (!r.ok || r.i != size - 4) return fail("trailing or missing bytes in model file");
    impl_ = impl;
    return true;
}

bool Model::loaded() const { return impl_ != nullptr; }
bool Model::has_rf_table() const { return impl_ && impl_->has_rf; }

bool Model::rf_mask(int view, double lo_hz, double hi_hz, std::vector<float>& probs) const {
    if (!has_view(view) || !impl_->has_rf || !finite64(lo_hz) || !finite64(hi_hz) || !(hi_hz >= lo_hz) || !(lo_hz > 0.0)) return false;
    const ViewModel& vm = impl_->views[view];
    if (vm.rf.size() != probs.size() || probs.empty()) return false;
    const double f = 0.5 * (lo_hz + hi_hz);
    std::vector<uint8_t> ok(probs.size(), 1);
    for (size_t c = 0; c < probs.size(); c++) {
        const RfClass& rc = vm.rf[c];
        if (rc.generic) continue;
        if (!rc.ranges.empty()) {
            bool any = false;
            for (const auto& rg : rc.ranges) any = any || (rg.first <= hi_hz && lo_hz <= rg.second);
            if (!any) ok[c] = 0;
        }
        // an exclusive band holding the detection's centre that does not list the class
        for (size_t b = 0; b < impl_->bands.size() && ok[c]; b++) {
            const RfBand& bd = impl_->bands[b];
            if (f >= bd.lo && f <= bd.hi && std::find(rc.bands.begin(), rc.bands.end(), int(b)) == rc.bands.end()) ok[c] = 0;
        }
    }
    double s = 0.0;
    for (size_t c = 0; c < probs.size(); c++) s += ok[c] ? probs[c] : 0.0f;
    if (!(s > 0.0)) {
        // nothing plausible left: the generic classes keep their share
        for (size_t c = 0; c < probs.size(); c++) ok[c] = vm.rf[c].generic ? 1 : 0;
        s = 0.0;
        for (size_t c = 0; c < probs.size(); c++) s += ok[c] ? probs[c] : 0.0f;
        if (!(s > 0.0)) return false;   // (then unmasked)
    }
    for (size_t c = 0; c < probs.size(); c++) probs[c] = ok[c] ? float(probs[c] / s) : 0.0f;
    return true;
}
bool Model::has_view(int view) const {
    return impl_ && view >= 0 && view < NUM_VIEWS && impl_->views[view].present;
}
int Model::classes(int view) const { return has_view(view) ? int(impl_->views[view].classes.size()) : 0; }
const ClassInfo& Model::class_info(int view, int cls) const {
    if (!has_view(view) || cls < 0 || cls >= classes(view)) return no_class();
    return impl_->views[view].classes[size_t(cls)];
}
const std::string& Model::meta_json() const {
    static const std::string empty;
    return impl_ ? impl_->meta : empty;
}

namespace {

// one network's probabilities (softmax of its last layer / its temperature)
void net_forward(const Net& nt, const float* x, std::vector<float>& probs) {
    const int D = feature_dim();
    std::vector<float> a, b;
    const Conv& cv = nt.conv;
    const bool slice = cv.on || cv.skip;
    const int s0 = slice ? cv.off : D, s1 = slice ? cv.off + cv.C * cv.S : D;
    a.reserve(size_t(D) + (cv.on ? size_t(cv.pools * cv.K) : 0));
    for (int i = 0; i < D; i++) {
        if (i >= s0 && i < s1) continue;   // the sequence goes through the front end
        const float v = finite32(x[i]) ? x[i] : 0.0f;
        a.push_back((v - nt.mean[size_t(i)]) * nt.inv_std[size_t(i)]);
    }
    if (cv.on) {
        // Conv1D (C in, K out, L taps, stride 1) + ReLU, pooled over time: per
        // filter one pass over t with the taps innermost (contiguous: vectorised)
        std::vector<float> seq(static_cast<size_t>(cv.C * cv.S));
        for (int i = 0; i < cv.C * cv.S; i++) {
            const float v = x[s0 + i];
            seq[size_t(i)] = finite32(v) ? std::clamp(v, -16.0f, 16.0f) : 0.0f;
        }
        const int T = cv.S - cv.L + 1;
        std::vector<float> acc(static_cast<size_t>(T));
        std::vector<float> pooled(static_cast<size_t>(cv.pools * cv.K));
        for (int k = 0; k < cv.K; k++) {
            std::fill(acc.begin(), acc.end(), cv.b[size_t(k)]);
            for (int c = 0; c < cv.C; c++) {
                const float* w = cv.w.data() + (size_t(k) * size_t(cv.C) + size_t(c)) * size_t(cv.L);
                const float* xs = seq.data() + size_t(c) * size_t(cv.S);
                for (int l = 0; l < cv.L; l++) {
                    const float wl = w[l];
                    const float* xl = xs + l;
                    float* ac = acc.data();
                    for (int t = 0; t < T; t++) ac[t] += wl * xl[t];
                }
            }
            float mx = 0.0f;
            double sum = 0.0;
            for (int t = 0; t < T; t++) {
                const float y = acc[size_t(t)] > 0.0f ? acc[size_t(t)] : 0.0f;
                mx = std::max(mx, y);
                sum += y;
            }
            pooled[size_t(k)] = mx;
            if (cv.pools == 2) pooled[size_t(cv.K + k)] = float(sum / T);
        }
        // the max of every filter, then (pools 2) the means
        for (int q = 0; q < cv.pools * cv.K; q++) a.push_back((pooled[size_t(q)] - cv.pmean[size_t(q)]) * cv.pinv[size_t(q)]);
    }
    for (const Layer& L : nt.layers) {
        b.assign(size_t(L.out), 0.0f);
        for (int o = 0; o < L.out; o++) {
            const float* w = L.w.data() + size_t(o) * size_t(L.in);
            float s = 0.0f;
            for (int i = 0; i < L.in; i++) s += w[i] * a[size_t(i)];
            s += L.b[size_t(o)];
            b[size_t(o)] = (L.act == 1 && !(s > 0.0f)) ? 0.0f : s;
        }
        a.swap(b);
    }
    float mx = -3.0e38f;
    for (float& v : a) {
        v = finite32(v) ? std::clamp(v / nt.temperature, -LOGIT_MAX, LOGIT_MAX) : 0.0f;
        mx = std::max(mx, v);
    }
    double sum = 0.0;
    for (float& v : a) {
        v = float(std::exp(double(v - mx)));
        sum += v;
    }
    probs.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) probs[i] = float(a[i] / sum);
}

}  // namespace

bool Model::predict(int view, const float* x, std::vector<float>& probs) const {
    SC_PROF("model.predict");
    probs.clear();
    if (!has_view(view) || !x) return false;
    const ViewModel& vm = impl_->views[view];
    if (vm.nets.size() == 1) {
        net_forward(vm.nets[0], x, probs);
        return true;
    }
    // an ensemble (format 4): the mean of its members' probabilities
    std::vector<double> acc(vm.classes.size(), 0.0);
    std::vector<float> p;
    for (const Net& n : vm.nets) {
        net_forward(n, x, p);
        for (size_t i = 0; i < acc.size() && i < p.size(); i++) acc[i] += p[i];
    }
    probs.resize(acc.size());
    for (size_t i = 0; i < acc.size(); i++) probs[i] = float(acc[i] / double(vm.nets.size()));
    return true;
}

}  // namespace sigclass
