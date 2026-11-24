// Signal classifier: the Analyzer - one snapshot through detector, views,
// features and the model (see include/sigclass/sigclass.hpp).

#include "sc_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace sigclass {

#ifdef SIGCLASS_PROF
namespace prof {
std::vector<Slot>& slots() {
    static std::vector<Slot> v;
    return v;
}
int reg(const char* name) {
    std::vector<Slot>& v = slots();
    for (size_t i = 0; i < v.size(); i++)
        if (std::string(v[i].name) == name) return int(i);
    v.push_back({name});
    return int(v.size()) - 1;
}
void reset() {
    for (Slot& s : slots()) s.ms = 0.0, s.n = 0;
}
}  // namespace prof
#endif

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
    std::vector<cf> img_s;
    std::vector<float> img_p;
    std::vector<Cand> cands;
    std::vector<float> fv;
    bool have = false;

    std::vector<Scored> run(const Model* model, std::vector<float>* fo, double centre_hz, const std::vector<KnownBand>* known);
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
                                      std::vector<float>* fo, double centre_hz, const std::vector<KnownBand>* known) {
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
    return m.run(model, fo, centre_hz, known);
}

std::vector<Scored> Analyzer::analyze(const std::complex<float>* iq, size_t n, double fs_hz, const Model* model,
                                      std::vector<float>* fo, double centre_hz, const std::vector<KnownBand>* known) {
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
    return m.run(model, fo, centre_hz, known);
}

void Analyzer::Impl::features_of(const Cand& c, float* out) {
    const int v = std::clamp(c.det.view, 0, NUM_VIEWS - 1);
#ifdef SIGCLASS_PROF
    static const int pid[NUM_VIEWS] = {prof::reg("look.v0"), prof::reg("look.v1"), prof::reg("look.v2"), prof::reg("look.v3")};
    SC_PROF_ID(pid[v]);
#endif
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

std::vector<Scored> Analyzer::Impl::run(const Model* model, std::vector<float>* fo, double centre_hz, const std::vector<KnownBand>* known) {
    const double t0 = now_ms();
    fe.run(fe.x.size(), fe.fs, cfg.spectrogram_hop);
    const double t1 = now_ms();
    tm.frontend_ms = t1 - t0;            // long FFTs + PSD + spectrogram
    tm.spectrogram_ms = fe.spec_ms;      // (the spectrogram's part of it)
    det.detect(fe, cfg, cands);
    // the detector's image candidates: the tuner's IQ imbalance puts eps x conj(x(-f)) at f, so
    // the candidate's view at its centre f holds eps x conj(the view at exactly -f) - the
    // strong signal's part mirrored onto the candidate - plus whatever else is there. The image's energy in it is the
    // projection of the view onto conj(s's view); the candidate is the image when that
    // projection holds at least image_share of the view's energy over its noise (a weak
    // image: nearly all of it; a transmitter of its own near the mirror frequency -
    // independent content - next to nothing). Only the samples where s is on count (a
    // burst's image is there only while the burst is).
    for (size_t i = 0; i < cands.size();) {
        bool image = false;
        if (cands[i].maybe_image) {
            SC_PROF("det.image_test");
            const int v = std::clamp(cands[i].det.view, 0, NUM_VIEWS - 1);
            const int bs = std::clamp(-cands[i].centre_bin, -LONG_N / 2 + 1, LONG_N / 2 - 1);   // its mirror
            ch.extract(fe, bs, v, view);
            img_s.assign(view.begin(), view.end());
            ch.extract(fe, -bs, v, view);
            const double nv = ch.noise_var(det.floor_fine(), -bs, v);
            const size_t n = std::min(img_s.size(), view.size());
            img_p.resize(n);
            for (size_t k = 0; k < n; k++) img_p[k] = std::norm(img_s[k]);
            double gate = 0.0;
            if (n) {
                std::vector<float> q(img_p.begin(), img_p.end());
                std::nth_element(q.begin(), q.begin() + long(0.9 * double(n - 1)), q.end());
                gate = 0.1 * double(q[size_t(0.9 * double(n - 1))]);
            }
            double sxy_r = 0.0, sxy_i = 0.0, ss = 0.0, ew = 0.0;
            size_t ng = 0;
            for (size_t k = 0; k < n; k++) {
                if (double(img_p[k]) < gate) continue;
                const cf a = view[k], b = img_s[k];
                sxy_r += double(a.real()) * b.real() - double(a.imag()) * b.imag();   // a x b (not conj b)
                sxy_i += double(a.real()) * b.imag() + double(a.imag()) * b.real();
                ss += double(img_p[k]);
                ew += std::norm(a);
                ng++;
            }
            const double proj = ss > 0.0 ? (sxy_r * sxy_r + sxy_i * sxy_i) / ss : 0.0;   // the image's energy
            const double excess = ew - double(ng) * nv;                                    // the view's over its noise
            // (only a narrow candidate's view - 24 / 96 kHz - holds little besides it: a wide view
            // holds other signals, whose energy would hide the image's share; there the
            // candidate goes as before)
            image = v > 1 || !(excess > 0.0) || proj >= double(cfg.image_share) * excess;
            static const bool dbg = std::getenv("SIGCLASS_DEBUG_IMAGE") != nullptr;   // (the image test per candidate, stderr)
            if (dbg) std::fprintf(stderr, "image? off %.0f bw %.0f snr %.1f mirror-of %.0f view %d share %.3f\n", cands[i].det.offset_hz, cands[i].det.bw_hz, cands[i].det.snr_db, bs * (fe.fs / LONG_N), v, excess > 0 ? proj / excess : 9.0);
        }
        if (image) cands.erase(cands.begin() + long(i));
        else i++;
    }
    if (int(cands.size()) > cfg.max_candidates) cands.resize(size_t(std::max(0, cfg.max_candidates)));
    const double t2 = now_ms();
    tm.detect_ms = t2 - t1;
    have = true;
    std::vector<Scored> res(cands.size());
    const bool want = (model && model->loaded()) || fo;
    const int D = feature_dim();
    if (fo) fo->assign(cands.size() * size_t(D), 0.0f);
    fv.resize(size_t(D));
    // the model's answer for a detection in a view (+ the RF plausibility mask by the
    // detection's absolute band)
    auto score = [&](const Detection& d, int v, const float* x, std::vector<float>& probs) {
        const double tm0 = now_ms();
        model->predict(v, x, probs);
        if (centre_hz > 0.0 && finite_d(centre_hz)) {
            const double lo = centre_hz + d.offset_hz - 0.5 * d.bw_hz, hi = centre_hz + d.offset_hz + 0.5 * d.bw_hz;
            model->rf_mask(v, lo, hi, probs);
        }
        tm.model_ms += now_ms() - tm0;
    };
    // a second look in view 0 (see below): only with a model, and not when the caller
    // takes the features (training rows stay those of the detection's own view)
    const bool second = !fo && model && model->has_view(0) && model->has_view(1) && cfg.second_view_max_hz > 0.0;
    std::vector<float> fv0, p0;
    std::vector<uint8_t> only0;   // view 0's classes that view 1's model does not have
    for (size_t i = 0; i < cands.size(); i++) {
        res[i].det = cands[i].det;
        if (!want) continue;
        // a known signal (the caller's settled track): reported in its track's view, unclassified
        if (known && !fo) {
            const Detection& d = cands[i].det;
            const double base = centre_hz > 0.0 && finite_d(centre_hz) ? centre_hz : 0.0;
            const double lo = base + d.offset_hz - 0.5 * d.bw_hz, hi = base + d.offset_hz + 0.5 * d.bw_hz;
            const KnownBand* hit = nullptr;
            for (const KnownBand& k : *known) {
                const double inter = std::min(hi, k.hi_hz) - std::max(lo, k.lo_hz);
                const double uni = std::max(hi, k.hi_hz) - std::min(lo, k.lo_hz);
                if (inter > 0.0 && uni > 0.0 && inter >= 0.5 * uni) { hit = &k; break; }
            }
            if (hit) {
                res[i].det.view = std::clamp(hit->view, 0, NUM_VIEWS - 1);
                tm.known++;
                continue;
            }
        }
        float* out = fo ? fo->data() + i * size_t(D) : fv.data();
        features_of(cands[i], out);
        if (model && model->has_view(cands[i].det.view)) {
            score(cands[i].det, cands[i].det.view, out, res[i].probs);
            // A detection a little too wide for view 0 (a 12.5 kHz channel on a shoulder of
            // its own transmitter, beside a neighbour, or a strong one's skirt measured over
            // 10.8 kHz) is also cut and classified in view 0: view 1's model has only the
            // classes it saw there in training - none of the 12.5 kHz channels -, so it cannot
            // name such a channel. View 0's answer is taken when it is a class view 1 does
            // not have (a class native to view 1 - TETRA, AIS, VDL2, wide FSK - cannot be
            // taken from it, and the generic classes exist in both), at least
            // second_view_min_p and more probable than view 1's best. The detection then
            // reports view 0 (its probabilities are view 0's classes) and keeps its band.
            const Detection& d = cands[i].det;
            float m1 = 0.0f;
            for (float p : res[i].probs) m1 = std::max(m1, p);
            if (second && d.view == 1 && !d.fills_band && d.bw_hz <= cfg.second_view_max_hz && m1 < cfg.second_view_skip_p) {
                if (only0.empty()) {
                    only0.assign(size_t(model->classes(0)), 1);
                    for (int c = 0; c < model->classes(0); c++)
                        for (int k = 0; k < model->classes(1); k++)
                            if (model->class_info(1, k).id == model->class_info(0, c).id) only0[size_t(c)] = 0;
                }
                Cand c0 = cands[i];
                c0.det.view = 0;
                // (as view 0 would have seen it: at most its widest band - the meta features and
                // the symbol group's channel filter then take the channel's core)
                c0.det.bw_hz = std::min(c0.det.bw_hz, fe.fs / VIEW_DECIM[0] * cfg.view_occupancy);
                fv0.resize(size_t(D));
                features_of(c0, fv0.data());
                score(c0.det, 0, fv0.data(), p0);
                tm.second_view++;
                int b0 = -1;
                for (int c = 0; c < int(p0.size()) && c < int(only0.size()); c++)
                    if (only0[size_t(c)] && (b0 < 0 || p0[size_t(c)] > p0[size_t(b0)])) b0 = c;
                static const bool dbg_sv = std::getenv("SIGCLASS_DEBUG_SV") != nullptr;   // (the second look per detection, stderr)
                if (dbg_sv) {
                    int b1 = 0;
                    for (int c = 1; c < int(res[i].probs.size()); c++) if (res[i].probs[size_t(c)] > res[i].probs[size_t(b1)]) b1 = c;
                    std::fprintf(stderr, "sv off %.0f bw %.0f m1 %.3f %s p0 %.3f %s take %d\n", d.offset_hz, d.bw_hz, m1, model->class_info(1, b1).id.c_str(),
                                 b0 >= 0 ? p0[size_t(b0)] : 0.0f, b0 >= 0 ? model->class_info(0, b0).id.c_str() : "-",
                                 int(b0 >= 0 && p0[size_t(b0)] >= cfg.second_view_min_p && p0[size_t(b0)] > m1));
                }
                if (b0 >= 0 && p0[size_t(b0)] >= cfg.second_view_min_p && p0[size_t(b0)] > m1) {
                    res[i].det.view = 0;
                    res[i].probs = p0;
                    tm.second_view_taken++;
                }
            }
            for (size_t c = 0; c < res[i].probs.size(); c++)
                if (res[i].probs[c] > res[i].best_p) {
                    res[i].best_p = res[i].probs[c];
                    res[i].best = int(c);
                }
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
