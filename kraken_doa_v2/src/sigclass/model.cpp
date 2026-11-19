// Signal classifier: the model file and the per-view MLP evaluator.
//
// sigclass.model, little-endian (tools/sigclass/model_io.py writes and reads it):
//   char[8]  "KSIGCLS\0"
//   u32      format version (1)
//   u32      feature layout version (FEATURE_LAYOUT_VERSION)
//   u64      feature layout hash (layout_hash(); must match the code)
//   u32      feature dim D
//   u32      meta_len, then meta_len bytes of UTF-8 JSON (opaque here)
//   u32      number of view entries (0..4), then per entry:
//              u32 view (0..3), u32 classes C, f32 softmax temperature
//                (TEMP_MIN .. TEMP_MAX)
//              C x { u16 id_len, id bytes, u16 name_len, name bytes }
//              f32[D] mean, f32[D] std   (input z = (x - mean) / std)
//              u32 layers, per layer: u32 in, u32 out, u32 activation
//                (0 = none, 1 = ReLU), f32[out x in] W (row-major), f32[out] b
//              (first in = D, in = previous out, last out = C, last activation 0)
//   u32      CRC-32 (IEEE 802.3, zlib's) of every byte before it
// probabilities = softmax(last layer / temperature).

#include "sigclass/sigclass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sigclass {

namespace {

constexpr char MAGIC[8] = {'K', 'S', 'I', 'G', 'C', 'L', 'S', '\0'};
constexpr uint32_t FORMAT_VERSION = 1;
// the loader's bounds: a temperature outside them (1e-39 passed the old > 0
// test) made logit / temperature overflow and every probability NaN; the
// scaled logits are clamped to +-LOGIT_MAX as well
constexpr float TEMP_MIN = 1e-3f, TEMP_MAX = 1e3f, LOGIT_MAX = 1e4f;

struct Layer {
    int in = 0, out = 0, act = 0;
    std::vector<float> w, b;
};

struct ViewModel {
    bool present = false;
    float temperature = 1.0f;
    std::vector<ClassInfo> classes;
    std::vector<float> mean, inv_std;
    std::vector<Layer> layers;
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
    if (fv != FORMAT_VERSION) return fail("model format version " + std::to_string(fv) + ", this build reads " +
                                          std::to_string(FORMAT_VERSION));
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
    if (!r.ok || nv > uint32_t(NUM_VIEWS)) return fail("bad view count");
    const int D = feature_dim();
    for (uint32_t q = 0; q < nv; q++) {
        const uint32_t v = r.u32();
        if (!r.ok || v >= uint32_t(NUM_VIEWS)) return fail("bad view index");
        ViewModel& vm = impl->views[v];
        if (vm.present) return fail("view " + std::to_string(v) + " twice");
        const uint32_t C = r.u32();
        vm.temperature = r.f32();
        if (!r.ok || C < 1 || C > 100000) return fail("bad class count");
        if (!finite32(vm.temperature) || !(vm.temperature >= TEMP_MIN && vm.temperature <= TEMP_MAX))
            return fail("bad temperature");
        vm.classes.resize(C);
        for (uint32_t c = 0; c < C; c++) {
            vm.classes[c].id = r.str(r.u16());
            vm.classes[c].name = r.str(r.u16());
        }
        std::vector<float> sd;
        if (!r.floats(vm.mean, size_t(D)) || !r.floats(sd, size_t(D))) return fail("truncated normalisation");
        vm.inv_std.resize(size_t(D));
        for (int i = 0; i < D; i++) {
            if (!finite32(vm.mean[size_t(i)]) || !finite32(sd[size_t(i)])) return fail("non-finite normalisation");
            vm.inv_std[size_t(i)] = std::fabs(sd[size_t(i)]) > 1e-12f ? 1.0f / sd[size_t(i)] : 0.0f;
        }
        const uint32_t nl = r.u32();
        if (!r.ok || nl < 1 || nl > 64) return fail("bad layer count");
        int prev = D;
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
            vm.layers.push_back(std::move(L));
        }
        if (prev != int(C) || vm.layers.back().act != 0) return fail("last layer must be linear with one output per class");
        vm.present = true;
    }
    if (!r.ok || r.i != size - 4) return fail("trailing or missing bytes in model file");
    impl_ = impl;
    return true;
}

bool Model::loaded() const { return impl_ != nullptr; }
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

bool Model::predict(int view, const float* x, std::vector<float>& probs) const {
    probs.clear();
    if (!has_view(view) || !x) return false;
    const ViewModel& vm = impl_->views[view];
    const int D = feature_dim();
    std::vector<float> a(static_cast<size_t>(D)), b;
    for (int i = 0; i < D; i++) {
        const float v = finite32(x[i]) ? x[i] : 0.0f;
        a[size_t(i)] = (v - vm.mean[size_t(i)]) * vm.inv_std[size_t(i)];
    }
    for (const Layer& L : vm.layers) {
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
        v = finite32(v) ? std::clamp(v / vm.temperature, -LOGIT_MAX, LOGIT_MAX) : 0.0f;
        mx = std::max(mx, v);
    }
    double sum = 0.0;
    for (float& v : a) {
        v = float(std::exp(double(v - mx)));
        sum += v;
    }
    probs.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) probs[i] = float(a[i] / sum);
    return true;
}

}  // namespace sigclass
