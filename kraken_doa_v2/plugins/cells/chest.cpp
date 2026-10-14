#include "chest.hpp"

#include <algorithm>
#include <cmath>

namespace cells {

namespace {

constexpr double GATE_REL = 0.1;        // a snapshot with < -10 dB of the usual energy at the main path is dropped (TDD uplink, missing SSB)
constexpr double FIRST_REL_DB = 15;     // "first path": the earliest peak within this of the strongest
constexpr double PORT1_REL = 0.1;       // LTE port 1 counts from -10 dB of port 0

double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(h), v.end());
    return v[h];
}

// snapshots of every antenna's impulse response on a window of taps
struct Snaps {
    int m = 0, ntap = 0, count = 0;
    std::vector<cf> h;                  // count x m x ntap
    const cf* at(int s, int a) const { return h.data() + (static_cast<size_t>(s) * m + static_cast<size_t>(a)) * static_cast<size_t>(ntap); }
};

// channel values at subcarriers -> impulse responses (Hann over the visible
// band, normalised so that one path of gain g gives |h| = |g| at its tap)
class CirMaker {
public:
    CirMaker(int nc, int t0, int ntap, int fmin, int fmax) : nc_(nc), t0_(t0), ntap_(ntap), fmin_(fmin), fmax_(fmax), ifft_(nc, +1) {}
    // f[i] subcarrier of value H[a * nre + i]; appends m x ntap to out
    void make(const std::vector<int>& f, const std::vector<cf>& H, int m, std::vector<cf>* out) {
        const int nre = static_cast<int>(f.size());
        double ws = 0;
        w_.resize(static_cast<size_t>(nre));
        for (int i = 0; i < nre; i++) {
            const double x = static_cast<double>(f[static_cast<size_t>(i)] - fmin_ + 1) / (fmax_ - fmin_ + 2);
            w_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * x));
            ws += w_[static_cast<size_t>(i)];
        }
        const float sc = ws > 0 ? static_cast<float>(1 / ws) : 0.0f;
        for (int a = 0; a < m; a++) {
            cf* b = ifft_.data();
            std::fill(b, b + nc_, cf(0, 0));
            for (int i = 0; i < nre; i++)
                b[(f[static_cast<size_t>(i)] + nc_) % nc_] = H[static_cast<size_t>(a * nre + i)] * (w_[static_cast<size_t>(i)] * sc);
            ifft_.run();
            for (int t = 0; t < ntap_; t++) out->push_back(b[((t0_ + t) % nc_ + nc_) % nc_]);
        }
    }

private:
    int nc_, t0_, ntap_, fmin_, fmax_;
    Fft ifft_;
    std::vector<float> w_;
};

// the per-tap covariance of the snapshots -> the cell's path and its spatial covariance
CellEst analyse(const Snaps& S, int t0, double tap_us, const EstParams& p) {
    CellEst e;
    e.m = S.m;
    e.tap_us = tap_us;
    e.tap0 = t0;
    e.snaps = S.count;
    const int m = S.m, T = S.ntap;
    if (S.count < 2 || m < 1 || T < 8) return e;
    // the strongest tap over every snapshot
    std::vector<double> P(static_cast<size_t>(T), 0.0);
    for (int s = 0; s < S.count; s++)
        for (int a = 0; a < m; a++) {
            const cf* h = S.at(s, a);
            for (int t = 0; t < T; t++) P[static_cast<size_t>(t)] += std::norm(h[t]);
        }
    const int tp = static_cast<int>(std::max_element(P.begin(), P.end()) - P.begin());
    // each snapshot's energy there: TDD uplink slots / SSB positions not used carry none
    std::vector<double> es(static_cast<size_t>(S.count), 0.0);
    for (int s = 0; s < S.count; s++)
        for (int a = 0; a < m; a++) {
            const cf* h = S.at(s, a);
            for (int t = std::max(0, tp - 2); t <= std::min(T - 1, tp + 2); t++) es[static_cast<size_t>(s)] += std::norm(h[t]);
        }
    std::vector<double> sorted = es;
    std::sort(sorted.begin(), sorted.end());
    const double q75 = sorted[static_cast<size_t>(0.75 * (sorted.size() - 1))];
    std::vector<int> keep;
    for (int s = 0; s < S.count; s++)
        if (es[static_cast<size_t>(s)] >= GATE_REL * q75) keep.push_back(s);
    e.used = static_cast<int>(keep.size());
    if (e.used < 2) return e;
    std::vector<Mat> C(static_cast<size_t>(T), mat_zero(m));
    std::vector<cd> v(static_cast<size_t>(m));
    for (int s : keep)
        for (int t = 0; t < T; t++) {
            for (int a = 0; a < m; a++) v[static_cast<size_t>(a)] = cd(S.at(s, a)[t]);
            add_outer(C[static_cast<size_t>(t)], v.data(), m);
        }
    // estimation floor: the taps without the cell's paths
    std::vector<double> tr(static_cast<size_t>(T));
    for (int t = 0; t < T; t++) tr[static_cast<size_t>(t)] = trace_re(C[static_cast<size_t>(t)], m);
    const double tmed = median_of(tr);
    if (!(tmed > 0)) return e;
    Mat F = mat_zero(m);
    int fn = 0;
    for (int t = 0; t < T; t++)
        if (tr[static_cast<size_t>(t)] < 2 * tmed) {
            for (size_t i = 0; i < F.size(); i++) F[i] += C[static_cast<size_t>(t)][i];
            fn++;
        }
    for (auto& x : F) x /= std::max(1, fn);
    Mat Fi;
    if (!inv_herm(F, m, &Fi, 1e-6 * trace_re(F, m))) return e;
    std::vector<double> D(static_cast<size_t>(T));
    for (int t = 0; t < T; t++) {
        double d = 0;
        const Mat& Ct = C[static_cast<size_t>(t)];
        for (int i = 0; i < m; i++)
            for (int k = 0; k < m; k++) d += (Fi[static_cast<size_t>(i * m + k)] * Ct[static_cast<size_t>(k * m + i)]).real();
        D[static_cast<size_t>(t)] = d / m;
    }
    const double dmed = std::max(median_of(D), 1e-12);
    std::vector<double> dev(D.size());
    for (size_t i = 0; i < D.size(); i++) dev[i] = std::fabs(D[i] - dmed);
    const double mad = median_of(dev);
    const double thr = std::max(dmed * std::pow(10.0, p.min_snr_db / 10), dmed + 8 * mad);
    auto at = [&](int t) { return D[static_cast<size_t>(std::clamp(t, 0, T - 1))]; };
    // paths: peaks above the threshold
    std::vector<int> pk;
    for (int t = 0; t < T; t++)
        if (D[static_cast<size_t>(t)] >= thr && D[static_cast<size_t>(t)] >= at(t - 1) && D[static_cast<size_t>(t)] > at(t + 1)) pk.push_back(t);
    e.profile.resize(static_cast<size_t>(T));
    for (int t = 0; t < T; t++) e.profile[static_cast<size_t>(t)] = static_cast<float>(10 * std::log10(std::max(D[static_cast<size_t>(t)] / dmed, 1e-3)));
    if (pk.empty()) return e;
    int j = pk[0];
    for (int t : pk)
        if (D[static_cast<size_t>(t)] > D[static_cast<size_t>(j)]) j = t;
    const double dmax = D[static_cast<size_t>(j)];
    int first = j;
    for (int t : pk)
        if (D[static_cast<size_t>(t)] >= dmax * std::pow(10.0, -FIRST_REL_DB / 10)) {
            first = t;
            break;
        }
    if (p.first_path) j = first;
    e.paths = static_cast<int>(pk.size());
    // main lobe: down to half the peak (above the median)
    const double half = dmed + 0.5 * (D[static_cast<size_t>(j)] - dmed);
    int lo = j, hi = j;
    while (lo > 0 && at(lo - 1) > half && lo > j - T / 8) lo--;
    while (hi < T - 1 && at(hi + 1) > half && hi < j + T / 8) hi++;
    Mat Cs = mat_zero(m);
    for (int q = lo; q <= hi; q++)
        for (size_t i = 0; i < Cs.size(); i++) Cs[i] += C[static_cast<size_t>(q)][i] - F[i];
    std::vector<double> ev;
    std::vector<std::vector<cd>> vecs;
    eig_herm(Cs, m, &ev, &vecs);
    double pos = 0;
    for (double x : ev) pos += std::max(0.0, x);
    if (!(ev.back() > 0) || !(pos > 0)) return e;
    e.cov = mat_zero(m);
    for (int r = 0; r < m; r++) {
        const double lam = std::max(0.0, ev[static_cast<size_t>(r)]);
        if (lam > 0) add_outer(e.cov, vecs[static_cast<size_t>(r)].data(), m, lam / pos);
    }
    e.purity = ev.back() / pos;
    e.power = std::max(0.0, (tr[static_cast<size_t>(j)] - trace_re(F, m))) / (e.used * m);
    e.snr_db = 10 * std::log10(std::max(D[static_cast<size_t>(j)] / dmed, 1.0));
    const double a = at(j - 1), b = at(j), c = at(j + 1), den = a - 2 * b + c;
    const double frac = den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    e.delay_us = (t0 + j + frac) * tap_us;
    e.first_us = (t0 + first) * tap_us;
    // rms delay spread of everything above the threshold
    double w = 0, m1 = 0, m2 = 0;
    for (int t = 0; t < T; t++) {
        const double d = D[static_cast<size_t>(t)];
        if (d < thr) continue;
        const double x = (t0 + t) * tap_us;
        w += d - dmed;
        m1 += (d - dmed) * x;
        m2 += (d - dmed) * x * x;
    }
    e.spread_us = w > 0 ? std::sqrt(std::max(0.0, m2 / w - (m1 / w) * (m1 / w))) : 0;
    e.ok = true;
    return e;
}

CellEst estimate_lte(const std::vector<const cf*>& x, size_t n, const CellHit& c, const EstParams& p) {
    const Numerology& num = p.num;
    const int N = num.nfft(), K = N / 2 - 1, m = static_cast<int>(x.size());
    const double scs = num.scs();
    SymFft sym(N, K);
    // visible subcarriers
    int fmin = 0, fmax = 0;
    for (int f = -K; f <= K; f++)
        if (f != 0 && visible(f, scs, p.carrier_off, p.vis_hz)) {
            if (fmin == 0 || f < fmin) fmin = f;
            fmax = std::max(fmax, f);
        }
    if (fmin >= 0 || fmax <= 0) return CellEst{};
    // the REs of each port: symbol 0 and 4, and their CRS index m'
    struct Re { int f, l, mp; };
    std::vector<Re> re[2];
    for (int port = 0; port < 2; port++)
        for (int l : {0, 4})
            for (int f = fmin; f <= fmax; f++) {
                int mp = 0;
                if (lte::crs_re(f, l, port, c.pci, &mp)) re[port].push_back({f, l, mp});
            }
    // CRS sequences of the frame's 20 slots
    std::vector<cd> r[20][2];
    for (int ns = 0; ns < 20; ns++) {
        r[ns][0] = lte::crs_seq(c.pci, ns, 0);
        r[ns][1] = lte::crs_seq(c.pci, ns, 4);
    }
    // slots in the burst (pairs of consecutive slots: the cell's carrier offset)
    const double slot = num.half_ms();
    const double early = std::min(4.0, 0.4 * num.cp());
    std::vector<long> slots;
    for (long k = 0;; k++) {
        const double s0 = c.frame0 + static_cast<double>(k) * slot;
        if (s0 + num.useful(4) + N + 1 > static_cast<double>(n)) break;
        if (s0 + num.useful(0) - early >= 0) slots.push_back(k);
    }
    if (slots.size() < 4) return CellEst{};
    const int npairs = std::max(1, std::min(p.max_snaps / 2, static_cast<int>(slots.size()) / 2));
    std::vector<long> use;
    for (int i = 0; i < npairs; i++) {
        const size_t k = static_cast<size_t>(static_cast<double>(i) * (slots.size() - 1) / npairs);
        if (k + 1 < slots.size() && slots[k + 1] == slots[k] + 1 && (use.empty() || use.back() < slots[k])) {
            use.push_back(slots[k]);
            use.push_back(slots[k + 1]);
        }
    }
    // channel values per slot, port, antenna, RE
    std::vector<std::vector<cf>> Y0(static_cast<size_t>(m), std::vector<cf>(static_cast<size_t>(2 * K + 1))), Y4 = Y0;
    std::vector<std::vector<cf>> H[2];   // [port][slot] -> m x nre
    std::vector<long> got;
    for (long k : use) {
        const double s0 = c.frame0 + static_cast<double>(k) * slot;
        bool ok = true;
        for (int a = 0; a < m && ok; a++)
            ok = sym.run(x[static_cast<size_t>(a)], n, s0 + num.useful(0), early, Y0[static_cast<size_t>(a)].data()) &&
                 sym.run(x[static_cast<size_t>(a)], n, s0 + num.useful(4), early, Y4[static_cast<size_t>(a)].data());
        if (!ok) continue;
        const int ns = static_cast<int>(((k % 20) + 20) % 20);
        for (int port = 0; port < (p.quick ? 1 : 2); port++) {
            const auto& R = re[port];
            std::vector<cf> h(static_cast<size_t>(m) * R.size());
            for (int a = 0; a < m; a++)
                for (size_t i = 0; i < R.size(); i++) {
                    const auto& Y = R[i].l == 0 ? Y0[static_cast<size_t>(a)] : Y4[static_cast<size_t>(a)];
                    const cd ref = r[ns][R[i].l == 0 ? 0 : 1][static_cast<size_t>(R[i].mp)];
                    h[static_cast<size_t>(a) * R.size() + i] = Y[static_cast<size_t>(R[i].f + K)] * cf(static_cast<float>(ref.real()), static_cast<float>(-ref.imag()));
                }
            H[port].push_back(std::move(h));
        }
        got.push_back(k);
    }
    if (got.size() < 4) return CellEst{};
    // the cell's residual carrier offset: symbol 0 of port 0 (the same
    // subcarriers every slot) from one slot to the next
    cd acc = 0;
    for (size_t i = 0; i + 1 < got.size(); i++) {
        if (got[i + 1] != got[i] + 1) continue;
        const auto& a0 = H[0][i];
        const auto& a1 = H[0][i + 1];
        for (size_t q = 0; q < a0.size(); q++)
            if (re[0][q % re[0].size()].l == 0) acc += cd(a1[q]) * std::conj(cd(a0[q]));
    }
    const double cfo = std::abs(acc) > 0 ? std::arg(acc) / (2 * M_PI * 0.5e-3) : 0.0;
    // symbol 4 turned back to symbol 0's time
    const cf rot4 = std::polar(1.0f, static_cast<float>(-2 * M_PI * cfo * (num.useful(4) - num.useful(0)) / num.fs));
    // impulse responses: every 3rd subcarrier (both symbols) -> 22.2 us
    // unambiguous; the window starts 5.4 us before the sync timing
    const int nc = 2 * N;
    const int ntap = nc / 3;
    const int t0 = -ntap / 4;
    const double tap_us = 1e6 / (nc * scs);
    CirMaker cir(nc, t0, ntap, fmin, fmax);
    Snaps S[2];
    for (int port = 0; port < (p.quick ? 1 : 2); port++) {
        S[port].m = m;
        S[port].ntap = ntap;
        std::vector<int> f;
        for (const auto& q : re[port]) f.push_back(q.f);
        for (auto& h : H[port]) {
            for (int a = 0; a < m; a++)
                for (size_t i = 0; i < re[port].size(); i++)
                    if (re[port][i].l == 4) h[static_cast<size_t>(a) * re[port].size() + i] *= rot4;
            cir.make(f, h, m, &S[port].h);
            S[port].count++;
        }
    }
    CellEst e0 = analyse(S[0], t0, tap_us, p);
    e0.cfo_hz = cfo;
    if (!e0.ok || p.quick) return e0;
    CellEst e1 = analyse(S[1], t0, tap_us, p);
    if (!(e1.ok && e1.power >= PORT1_REL * e0.power && std::fabs(e1.delay_us - e0.delay_us) < 1.0)) return e0;
    // two antenna ports at the base station: both are the cell's
    Snaps both = S[0];
    both.h.insert(both.h.end(), S[1].h.begin(), S[1].h.end());
    both.count += S[1].count;
    CellEst e = analyse(both, t0, tap_us, p);
    if (!e.ok) return e0;
    e.ports = 2;
    e.cfo_hz = cfo;
    return e;
}

CellEst estimate_nr(const std::vector<const cf*>& x, size_t n, const CellHit& c, const EstParams& p) {
    const Numerology& num = p.num;
    const int N = num.nfft(), K = N / 2 - 1, m = static_cast<int>(x.size());
    const double scs = num.scs();
    SymFft sym(N, K);
    const auto d = nr::sss(c.pci / 3, c.pci % 3);
    std::vector<int> f, idx;
    int fmin = 1 << 20, fmax = -(1 << 20);
    for (int i = 0; i < 127; i++) {
        const int k = nr::sync_f(i);
        if (std::abs(k) > K || !visible(k, scs, p.carrier_off, p.vis_hz)) continue;
        f.push_back(k);
        idx.push_back(i);
        fmin = std::min(fmin, k);
        fmax = std::max(fmax, k);
    }
    if (f.size() < 40) return CellEst{};
    const double early = std::min(4.0, 0.4 * num.cp());
    const int nc = 2 * N, ntap = nc, t0 = -nc / 5;
    const double tap_us = 1e6 / (nc * scs);
    CirMaker cir(nc, t0, ntap, fmin, fmax);
    Snaps S;
    S.m = m;
    S.ntap = ntap;
    std::vector<std::vector<cf>> Y(static_cast<size_t>(m), std::vector<cf>(static_cast<size_t>(2 * K + 1)));
    std::vector<cf> h(static_cast<size_t>(m) * f.size());
    for (double ps : c.pss) {
        const double ss = ps + 2 * num.sym_len();
        bool ok = true;
        for (int a = 0; a < m && ok; a++) ok = sym.run(x[static_cast<size_t>(a)], n, ss, early, Y[static_cast<size_t>(a)].data());
        if (!ok) continue;
        for (int a = 0; a < m; a++)
            for (size_t i = 0; i < f.size(); i++)
                h[static_cast<size_t>(a) * f.size() + i] =
                    Y[static_cast<size_t>(a)][static_cast<size_t>(f[i] + K)] * static_cast<float>(d[static_cast<size_t>(idx[i])]);
        cir.make(f, h, m, &S.h);
        S.count++;
    }
    return analyse(S, t0, tap_us, p);
}

}  // namespace

CellEst estimate(const std::vector<const cf*>& x, size_t n, const CellHit& c, const EstParams& p) {
    if (x.empty() || !p.num.usable()) return CellEst{};
    return c.tech == Tech::LTE ? estimate_lte(x, n, c, p) : estimate_nr(x, n, c, p);
}

}  // namespace cells
