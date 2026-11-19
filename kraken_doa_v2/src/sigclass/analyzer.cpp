// Signal classifier: the Analyzer - one snapshot through detector, views,
// features and the model (see include/sigclass/sigclass.hpp).

#include "sc_internal.hpp"

#include <chrono>

namespace sigclass {

using namespace detail;

namespace {
double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

struct Analyzer::Impl {
    Config cfg;
    FrontEnd fe;
    Detector det;
    Channelizer ch;
    Features feat;
    Timing tm;
    avec<cf> view;
    std::vector<Cand> cands;
    std::vector<float> fv;
    bool have = false;

    std::vector<Scored> run(const Model* model, std::vector<float>* fo);
    void features_of(const Cand& c, float* out);
};

Analyzer::Analyzer(const Config& cfg) : impl_(std::make_unique<Impl>()) { impl_->cfg = cfg; }
Analyzer::~Analyzer() = default;

const Analyzer::Timing& Analyzer::last_timing() const { return impl_->tm; }
const Config& Analyzer::config() const { return impl_->cfg; }
const std::vector<float>& Analyzer::last_floor() const { return impl_->det.floor_fine(); }

int Analyzer::view_for_bandwidth(double bw_hz, double fs_hz, float occupancy) {
    const double occ = occupancy > 0.05f && occupancy <= 1.0f ? occupancy : 0.45;
    for (int v = 0; v < NUM_VIEWS - 1; v++)
        if (fs_hz / VIEW_DECIM[v] * occ >= bw_hz) return v;
    return NUM_VIEWS - 1;
}

std::vector<Scored> Analyzer::analyze(const uint8_t* iq, size_t n, double fs_hz, const Model* model,
                                      std::vector<float>* fo) {
    Impl& m = *impl_;
    m.have = false;
    if (fo) fo->clear();
    if (!iq || n < MIN_SNAPSHOT_SAMPLES || !(fs_hz > 0.0) || !finite_d(fs_hz)) return {};
    const double t0 = now_ms();
    // (u - 127.5) / 127.5 with the snapshot mean removed = (u - mean(u)) / 127.5
    int64_t si = 0, sq = 0;
    for (size_t i = 0; i < n; i++) {
        si += iq[2 * i];
        sq += iq[2 * i + 1];
    }
    const float mi = float(double(si) / double(n)), mq = float(double(sq) / double(n));
    m.fe.x.resize(n);
    const float k = 1.0f / 127.5f;
    cf* x = m.fe.x.data();
    for (size_t i = 0; i < n; i++) x[i] = cf((float(iq[2 * i]) - mi) * k, (float(iq[2 * i + 1]) - mq) * k);
    m.fe.fs = fs_hz;
    m.tm = Timing{};
    m.tm.convert_ms = now_ms() - t0;
    return m.run(model, fo);
}

std::vector<Scored> Analyzer::analyze(const std::complex<float>* iq, size_t n, double fs_hz, const Model* model,
                                      std::vector<float>* fo) {
    Impl& m = *impl_;
    m.have = false;
    if (fo) fo->clear();
    if (!iq || n < MIN_SNAPSHOT_SAMPLES || !(fs_hz > 0.0) || !finite_d(fs_hz)) return {};
    const double t0 = now_ms();
    double si = 0.0, sq = 0.0;
    for (size_t i = 0; i < n; i++) {
        const float a = finite_f(iq[i].real()) ? iq[i].real() : 0.0f, b = finite_f(iq[i].imag()) ? iq[i].imag() : 0.0f;
        si += a;
        sq += b;
    }
    const float mi = float(si / double(n)), mq = float(sq / double(n));
    m.fe.x.resize(n);
    for (size_t i = 0; i < n; i++) {
        const float a = finite_f(iq[i].real()) ? iq[i].real() : 0.0f, b = finite_f(iq[i].imag()) ? iq[i].imag() : 0.0f;
        m.fe.x[i] = cf(a - mi, b - mq);
    }
    m.fe.fs = fs_hz;
    m.tm = Timing{};
    m.tm.convert_ms = now_ms() - t0;
    return m.run(model, fo);
}

void Analyzer::Impl::features_of(const Cand& c, float* out) {
    const int v = std::clamp(c.det.view, 0, NUM_VIEWS - 1);
    double t = now_ms();
    ch.extract(fe, c.centre_bin, v, view);
    ViewInfo vi;
    vi.view = v;
    vi.fs_v = fe.fs / VIEW_DECIM[v];
    vi.noise_var = ch.noise_var(det.floor_fine(), c.centre_bin, v);
    vi.det = c.det;
    const double t1 = now_ms();
    tm.channelize_ms += t1 - t;
    feat.compute(view.data(), view.size(), vi, out);
    tm.features_ms += now_ms() - t1;
}

std::vector<Scored> Analyzer::Impl::run(const Model* model, std::vector<float>* fo) {
    const double t0 = now_ms();
    fe.run(fe.x.size(), fe.fs, cfg.spectrogram_hop);
    const double t1 = now_ms();
    tm.frontend_ms = t1 - t0;            // long FFTs + PSD + spectrogram
    tm.spectrogram_ms = fe.spec_ms;      // (the spectrogram's part of it)
    det.detect(fe, cfg, cands);
    const double t2 = now_ms();
    tm.detect_ms = t2 - t1;
    have = true;
    std::vector<Scored> res(cands.size());
    const bool want = (model && model->loaded()) || fo;
    const int D = feature_dim();
    if (fo) fo->assign(cands.size() * size_t(D), 0.0f);
    fv.resize(size_t(D));
    for (size_t i = 0; i < cands.size(); i++) {
        res[i].det = cands[i].det;
        if (!want) continue;
        float* out = fo ? fo->data() + i * size_t(D) : fv.data();
        features_of(cands[i], out);
        if (model && model->has_view(cands[i].det.view)) {
            const double tm0 = now_ms();
            model->predict(cands[i].det.view, out, res[i].probs);
            for (size_t c = 0; c < res[i].probs.size(); c++)
                if (res[i].probs[c] > res[i].best_p) {
                    res[i].best_p = res[i].probs[c];
                    res[i].best = int(c);
                }
            tm.model_ms += now_ms() - tm0;
        }
    }
    tm.total_ms = tm.convert_ms + (now_ms() - t0);
    return res;
}

bool Analyzer::extract_view(double offset_hz, int view, std::vector<std::complex<float>>& out) {
    Impl& m = *impl_;
    out.clear();
    if (!m.have) return false;
    const int c = int(std::lround(offset_hz / (m.fe.fs / LONG_N)));
    m.ch.extract(m.fe, std::clamp(c, -LONG_N / 2, LONG_N / 2 - 1), std::clamp(view, 0, NUM_VIEWS - 1), m.view);
    out.assign(m.view.begin(), m.view.end());
    return true;
}

bool Analyzer::features_for(const Detection& d, std::vector<float>& out) {
    Impl& m = *impl_;
    out.clear();
    if (!m.have) return false;
    Cand c;
    c.det = d;
    c.det.view = std::clamp(d.view, 0, NUM_VIEWS - 1);
    c.centre_bin = std::clamp(int(std::lround(d.offset_hz / (m.fe.fs / LONG_N))), -LONG_N / 2, LONG_N / 2 - 1);
    out.assign(size_t(feature_dim()), 0.0f);
    m.features_of(c, out.data());
    return true;
}

}  // namespace sigclass
