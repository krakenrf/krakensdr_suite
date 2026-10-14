#include "ofdm.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace sfn {

namespace {

constexpr double DETECT_S = 0.12;        // samples a detection looks at
constexpr double RHO_MIN = 0.12;         // CP correlation of a locked signal at least
constexpr int MAX_PATTERN_SYMBOLS = 48;
constexpr int OFFSET_SYMBOLS = 8;
constexpr int MAX_TAPS = 4096;
constexpr int LOST_BURSTS = 4;
constexpr double PROCESS_S = 0.2;        // of each burst analysed (CPU: ~12 % of a Pi 5 core at 2.4 MHz)

int pow2_at_least(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

std::string fmtd(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace


void OfdmEstimator::reset() {
    locked_ = false;
    fails_ = 0;
    lf_ = 0;
    step_ = 0;
    centre_hz_ = NAN;
    sig_mode_.clear();
    std_->reset_signalling();
}


// CP correlation folded modulo the symbol length: the CP start o, the
// normalised peak rho and the fractional carrier offset eps (carriers)
bool OfdmEstimator::cp_fold(const std::vector<kp::cf>& x, int N, int ncp, int* o, double* rho, double* eps) {
    const int L = N + ncp;
    const long n = static_cast<long>(x.size());
    if (ncp < 1 || n < 2L * L + N) return false;
    CpProducts cp;
    cp_products(x, N, &cp);
    return cp_fold(cp, N, ncp, o, rho, eps);
}

// per sample the product x(i) x*(i + N) and the energy (real arithmetic:
// std::complex products go through the NaN-checking __muldc3 at -O2) - shared
// by every guard length of one FFT size
void OfdmEstimator::cp_products(const std::vector<kp::cf>& x, int N, CpProducts* cp) {
    const long np = static_cast<long>(x.size()) - N;
    cp->pr.resize(static_cast<size_t>(std::max(0L, np)));
    cp->pi.resize(cp->pr.size());
    cp->pe.resize(cp->pr.size());
    const float* xf = reinterpret_cast<const float*>(x.data());
    for (long i = 0; i < np; i++) {
        const float ar = xf[2 * i], ai = xf[2 * i + 1], br = xf[2 * (i + N)], bi = xf[2 * (i + N) + 1];
        cp->pr[static_cast<size_t>(i)] = ar * br + ai * bi;
        cp->pi[static_cast<size_t>(i)] = ai * br - ar * bi;
        cp->pe[static_cast<size_t>(i)] = 0.5f * (ar * ar + ai * ai + br * br + bi * bi);
    }
}

bool OfdmEstimator::cp_fold(const CpProducts& cp, int N, int ncp, int* o, double* rho, double* eps) {
    const int L = N + ncp;
    const long np = static_cast<long>(cp.pr.size());
    if (ncp < 1 || np < 2L * L) return false;
    const auto& pr = cp.pr;
    const auto& pi = cp.pi;
    const auto& pe = cp.pe;
    std::vector<double> Cr(static_cast<size_t>(L), 0.0), Ci(static_cast<size_t>(L), 0.0), E(static_cast<size_t>(L), 0.0);
    // sliding sums over ncp, folded modulo the symbol length
    double sr = 0, si = 0, e = 0;
    for (long i = 0; i < ncp && i < np; i++) {
        sr += pr[static_cast<size_t>(i)];
        si += pi[static_cast<size_t>(i)];
        e += pe[static_cast<size_t>(i)];
    }
    long k = 0;
    for (long i = 0; i + ncp <= np; i++) {
        Cr[static_cast<size_t>(k)] += sr;
        Ci[static_cast<size_t>(k)] += si;
        E[static_cast<size_t>(k)] += e;
        if (++k == L) k = 0;
        if (i + ncp < np) {
            const size_t a = static_cast<size_t>(i + ncp), b = static_cast<size_t>(i);
            sr += pr[a] - pr[b];
            si += pi[a] - pi[b];
            e += pe[a] - pe[b];
        }
    }
    int best = 0;
    double bv = -1;
    for (int j = 0; j < L; j++) {
        const double v = E[static_cast<size_t>(j)] > 0 ? std::hypot(Cr[static_cast<size_t>(j)], Ci[static_cast<size_t>(j)]) / E[static_cast<size_t>(j)] : 0;
        if (v > bv) { bv = v; best = j; }
    }
    *o = best;
    *rho = bv;
    *eps = -std::atan2(Ci[static_cast<size_t>(best)], Cr[static_cast<size_t>(best)]) / (2 * M_PI);
    return true;
}

bool OfdmEstimator::prepare(const Burst& b, const OfdmMode& m, int antennas, double max_s, Prep* p,
                            const std::vector<double>* frame_starts) {
    int D = 1;
    while (m.fs / D > b.rate * 1.0001) D *= 2;
    if (m.nfft % D != 0 || D > 64) return false;
    p->N = m.nfft / D;
    p->ncp = std::max(1, static_cast<int>(std::lround(static_cast<double>(m.ncp) / D)));
    p->rate = m.fs / D;
    const size_t n = std::min(b.n, static_cast<size_t>(max_s * b.rate));
    Resampler rs(b.rate, p->rate);
    p->x.assign(static_cast<size_t>(antennas), {});
    for (int a = 0; a < antennas && a < b.m; a++) rs.run(b.ch[static_cast<size_t>(a)].data(), n, &p->x[static_cast<size_t>(a)]);
    const long nx = static_cast<long>(p->x[0].size());
    // the symbols: guard starts at the resampled rate
    std::vector<double> sym;
    std::vector<char> fol;
    std::vector<int> lid;
    if (std_->framed()) {
        std::vector<double> own;
        if (!frame_starts) {
            own = std_->frame_starts(b.ch[0], b.rate, m.fs);
            frame_starts = &own;
        }
        if (frame_starts->empty()) return false;
        // the frame length from two preambles (kept: it doesn't change)
        if (frame_starts->size() >= 2) {
            const double L = static_cast<double>(m.nfft + m.ncp) / m.nfft * p->N;
            const double pre = static_cast<double>(std_->preamble_T()) * p->N / m.nfft;
            const double d = ((*frame_starts)[1] - (*frame_starts)[0]) * p->rate / b.rate - pre;
            const int lf = static_cast<int>(std::lround(d / L));
            if (lf > 0 && std::fabs(d - lf * L) < 3) lf_ = lf;
        }
        frame_grid(*frame_starts, p->rate / b.rate, nx, m, p->N, p->ncp, &sym, &fol, &lid);
        p->frames = static_cast<int>(frame_starts->size());
        p->rho = cp_at(p->x[0], sym, p->N, p->ncp, &p->eps);
    } else if (std_->tracked()) {
        CpProducts cp;
        cp_products(p->x[0], p->N, &cp);
        double period;
        track(cp, p->N, p->ncp, std_->max_excess_T() / D, &sym, &fol, &period);
        p->rho = cp_at(p->x[0], sym, p->N, p->ncp, &p->eps);
        lid.assign(sym.size(), -1);
    } else {
        int o;
        if (!cp_fold(p->x[0], p->N, p->ncp, &o, &p->rho, &p->eps)) return false;
        const int L = p->N + p->ncp;
        for (long s = o % L; s + L <= nx; s += L) {
            sym.push_back(static_cast<double>(s));
            fol.push_back(sym.size() > 1);
            lid.push_back(-1);
        }
    }
    // fractional carrier offset out (every antenna, the same phase)
    const double df = p->rate / p->N;
    for (auto& v : p->x) {
        double ph = 0;
        shift_freq(v.data(), v.size(), -p->eps * df, p->rate, &ph);
    }
    // FFT windows from the middle of the guard (echoes on both sides)
    p->starts.clear();
    p->frac.clear();
    p->follows.clear();
    p->lidx.clear();
    for (size_t i = 0; i < sym.size(); i++) {
        // a fractional start (a symbol period with excess samples, a preamble
        // position): the window starts at the sample before, the fraction is
        // taken out of the pilots' phases
        const double sx = sym[i] + p->ncp / 2;
        const long s = static_cast<long>(std::floor(sx));
        if (s < 0 || s + p->N > nx) continue;
        p->follows.push_back(i > 0 && fol[i] && !p->starts.empty());
        p->starts.push_back(s);
        p->frac.push_back(sx - static_cast<double>(s));
        p->lidx.push_back(i < lid.size() ? lid[i] : -1);
    }
    D_ = D;
    return true;
}

void OfdmEstimator::frame_grid(const std::vector<double>& fstarts, double scale, long n, const OfdmMode& m, int N, int ncp,
                               std::vector<double>* sym, std::vector<char>* follows, std::vector<int>* lidx) const {
    const double L = static_cast<double>(m.nfft + m.ncp) * N / m.nfft;   // symbol (resampled samples)
    const double pre = static_cast<double>(std_->preamble_T()) * N / m.nfft;
    const int skip = std_->preamble_symbols(m);
    const bool fc = std_->closing_symbol();
    (void)ncp;
    sym->clear();
    follows->clear();
    if (lidx) lidx->clear();
    if (fstarts.empty()) return;
    // before the first preamble: the previous frame's symbols end at it
    // (their index from the frame length, when known)
    const double f0 = fstarts[0] * scale;
    std::vector<double> before;
    std::vector<int> bl;
    for (int j = fc ? 1 : 0;; j++) {
        const double s = f0 - (j + 1) * L;
        if (s < 0) break;
        const int l = lf_ > 0 ? lf_ - 1 - j : -1;
        if (lf_ > 0 && l < skip) break;   // the previous frame's P2 symbols / preamble
        before.push_back(s);
        bl.push_back(l);
    }
    for (size_t i = before.size(); i-- > 0;) {
        sym->push_back(before[i]);
        follows->push_back(i + 1 < before.size());
        if (lidx) lidx->push_back(bl[i]);
    }
    for (size_t f = 0; f < fstarts.size(); f++) {
        const double a = fstarts[f] * scale + pre;
        const double end = f + 1 < fstarts.size() ? fstarts[f + 1] * scale : static_cast<double>(n);
        bool first = true;
        for (int j = skip;; j++) {
            const double s = a + j * L;
            if (s + L > end + 0.5) break;
            // the frame's last symbol (the next preamble right after it): frame closing
            if (fc && f + 1 < fstarts.size() && s + 2 * L > end + 0.5) break;
            sym->push_back(s);
            follows->push_back(!first);
            if (lidx) lidx->push_back(j);
            first = false;
        }
    }
}

// CP correlation at given symbol (guard) starts: rho, and eps
double OfdmEstimator::cp_at(const std::vector<kp::cf>& x, const std::vector<double>& sym, int N, int ncp, double* eps) {
    double cr = 0, ci = 0, e = 0;
    const long n = static_cast<long>(x.size());
    const float* xf = reinterpret_cast<const float*>(x.data());
    for (double sd : sym) {
        const long s = static_cast<long>(std::lround(sd));
        if (s < 0 || s + ncp + N > n) continue;
        for (long i = s; i < s + ncp; i++) {
            const float ar = xf[2 * i], ai = xf[2 * i + 1], br = xf[2 * (i + N)], bi = xf[2 * (i + N) + 1];
            cr += ar * br + ai * bi;
            ci += ai * br - ar * bi;
            e += 0.5 * (ar * ar + ai * ai + br * br + bi * bi);
        }
    }
    if (eps) *eps = -std::atan2(ci, cr) / (2 * M_PI);
    return e > 0 ? std::hypot(cr, ci) / e : 0;
}

// two passes: the guard correlation over the nominal guard finds the symbol
// period; with excess samples (a longer real guard) the window is a plateau
// as wide as the excess, so a second pass correlates over the whole measured
// guard - a sharp peak per symbol again
double OfdmEstimator::track(const CpProducts& cp, int N, int ncp, int max_excess, std::vector<double>* sym, std::vector<char>* follows,
                            double* period) {
    const double rho = track_pass(cp, N, ncp, max_excess, sym, follows, period);
    if (rho <= 0 || *period <= N + ncp + 2) return rho;
    const int ncp2 = static_cast<int>(std::lround(*period - N));
    std::vector<double> s2;
    std::vector<char> f2;
    double p2 = 0;
    const double rho2 = track_pass(cp, N, ncp2, 4, &s2, &f2, &p2);
    if (rho2 <= 0 || s2.size() < sym->size() / 2) return rho;
    sym->swap(s2);
    follows->swap(f2);
    *period = p2;
    return rho2;
}

double OfdmEstimator::track_pass(const CpProducts& cp, int N, int ncp, int max_excess, std::vector<double>* sym, std::vector<char>* follows,
                                 double* period) {
    sym->clear();
    follows->clear();
    *period = 0;
    const long np = static_cast<long>(cp.pr.size());
    const long nm = np - ncp;
    const long L0 = N + ncp;
    if (ncp < 1 || nm < 2 * L0) return 0;
    // normalised guard correlation at every start
    std::vector<float> mt(static_cast<size_t>(nm));
    double sr = 0, si = 0, e = 0;
    for (long i = 0; i < ncp; i++) {
        sr += cp.pr[static_cast<size_t>(i)];
        si += cp.pi[static_cast<size_t>(i)];
        e += cp.pe[static_cast<size_t>(i)];
    }
    for (long i = 0; i < nm; i++) {
        mt[static_cast<size_t>(i)] = e > 0 ? static_cast<float>(std::hypot(sr, si) / e) : 0.0f;
        const size_t a = static_cast<size_t>(i + ncp), b = static_cast<size_t>(i);
        if (i + ncp < np) {
            sr += cp.pr[a] - cp.pr[b];
            si += cp.pi[a] - cp.pi[b];
            e += cp.pe[a] - cp.pe[b];
        }
    }
    long top = 0;
    for (long i = 1; i < nm; i++)
        if (mt[static_cast<size_t>(i)] > mt[static_cast<size_t>(top)]) top = i;
    const double ref = mt[static_cast<size_t>(top)];
    if (ref < RHO_MIN) return 0;
    const float thr = static_cast<float>(std::max(0.5 * ref, 0.7 * RHO_MIN));
    auto argmax = [&](long a, long b) {
        long q = a;
        for (long k = a; k <= b; k++)
            if (mt[static_cast<size_t>(k)] > mt[static_cast<size_t>(q)]) q = k;
        return q;
    };
    // from the strongest symbol forwards and backwards: the next guard within
    // one symbol + the excess; none there (a bootstrap, another subframe): the
    // next strong one further on starts a new run
    std::vector<std::pair<long, char>> pts = {{top, 0}};
    for (int dir : {1, -1}) {
        long prev = top;
        while (true) {
            long a = dir > 0 ? prev + L0 - 4 : prev - L0 - max_excess - 4;
            long b = dir > 0 ? prev + L0 + max_excess + 4 : prev - L0 + 4;
            a = std::max(0L, a);
            b = std::min(nm - 1, b);
            if (a > b) break;
            const long q = argmax(a, b);
            if (mt[static_cast<size_t>(q)] >= thr) {
                pts.push_back({q, 1});
                prev = q;
                continue;
            }
            long found = -1;
            if (dir > 0) {
                for (long k = prev + L0; k < nm; k++)
                    if (mt[static_cast<size_t>(k)] >= thr) {
                        found = argmax(k, std::min(nm - 1, k + ncp));
                        break;
                    }
            } else {
                for (long k = prev - L0; k >= 0; k--)
                    if (mt[static_cast<size_t>(k)] >= thr) {
                        found = argmax(std::max(0L, k - ncp), k);
                        break;
                    }
            }
            if (found < 0) break;
            pts.push_back({found, 2});   // a break before (forwards) / after (backwards) it
            prev = found;
        }
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(), [](const auto& x, const auto& y) { return x.first == y.first; }), pts.end());
    // the usual spacing (symbols with their excess samples); preamble symbols
    // have none, so a change of spacing starts another run
    std::vector<long> sp;
    for (size_t k = 1; k < pts.size(); k++) {
        const long d = pts[k].first - pts[k - 1].first;
        if (d >= L0 - 4 && d <= L0 + max_excess + 4) sp.push_back(d);
    }
    long Lm = L0;
    if (!sp.empty()) {
        std::nth_element(sp.begin(), sp.begin() + static_cast<long>(sp.size() / 2), sp.end());
        Lm = sp[sp.size() / 2];
    }
    // the first symbol after a gap (a bootstrap): a preamble symbol with
    // other carriers (ATSC 3.0: the first one always at the fewest) - left out
    {
        std::vector<std::pair<long, char>> kept;
        for (size_t k = 0; k < pts.size(); k++) {
            const bool after_gap = k > 0 && pts[k].first - pts[k - 1].first > L0 + max_excess + 4;
            if (!after_gap) kept.push_back(pts[k]);
        }
        pts.swap(kept);
    }
    // runs of contiguous symbols (split where a symbol is missing - a
    // bootstrap, a gap): each one's start a and period L folded from the
    // guard correlation along the line, max over a, L of sum m(a + k L). The
    // peaks of single symbols wander (multipath, a guard longer than the
    // window); the fold doesn't. Preamble symbols (no excess samples) at the
    // start of a run land off the line - the scattered-pilot test drops them.
    auto m_at = [&](double x) -> double {
        const long i0 = static_cast<long>(std::floor(x));
        if (i0 < 0 || i0 + 1 >= nm) return 0;
        const double f = x - static_cast<double>(i0);
        return (1 - f) * mt[static_cast<size_t>(i0)] + f * mt[static_cast<size_t>(i0 + 1)];
    };
    double rho = 0;
    size_t i = 0;
    while (i < pts.size()) {
        size_t j = i + 1;
        while (j < pts.size()) {
            const long d = pts[j].first - pts[j - 1].first;
            if (d < Lm / 2 || d > Lm + Lm / 2) break;
            j++;
        }
        const size_t n = j - i;
        double a = static_cast<double>(pts[i].first), slope = static_cast<double>(Lm);
        if (n >= 3) {
            double s0 = 0, s1 = 0, s00 = 0, s01 = 0;
            for (size_t k = 0; k < n; k++) {
                const double xk = static_cast<double>(k), yk = static_cast<double>(pts[i + k].first);
                s0 += xk;
                s1 += yk;
                s00 += xk * xk;
                s01 += xk * yk;
            }
            slope = (n * s01 - s0 * s1) / (n * s00 - s0 * s0);
            a = (s1 - slope * s0) / n;
            // the fold, around the least-squares line
            double best = -1, ba = a, bl = slope;
            for (double dl = -1.5; dl <= 1.5001; dl += 0.05)
                for (double da = -6; da <= 6.001; da += 0.25) {
                    double sum = 0;
                    for (size_t k = 0; k < n; k++) sum += m_at(a + da + (slope + dl) * static_cast<double>(k));
                    if (sum > best) {
                        best = sum;
                        ba = a + da;
                        bl = slope + dl;
                    }
                }
            a = ba;
            slope = bl;
        }
        for (size_t k = 0; k < n; k++) {
            sym->push_back(n >= 3 ? a + slope * static_cast<double>(k) : static_cast<double>(pts[i + k].first));
            follows->push_back(k > 0);
            rho += mt[static_cast<size_t>(pts[i + k].first)];
        }
        i = j;
    }
    // the period: the median spacing (the line fits need clean runs; a guard
    // longer than the correlation window makes the peaks wander)
    *period = static_cast<double>(Lm);
    return sym->empty() ? 0 : rho / static_cast<double>(sym->size());
}

int OfdmEstimator::valid_bins(const Prep& p, double rate_in) const {
    const double df = p.rate / p.N;
    return std::max(8, static_cast<int>(std::floor(0.42 * std::min(rate_in, p.rate) / df)));
}

// FFT of N samples of antenna a from start; Y[(b mod N)] = bin b
void OfdmEstimator::symbol_fft(const Prep& p, int a, size_t start, std::vector<kp::cf>* Y) {
    Fft& f = fft_.get(p.N, -1);
    std::copy_n(p.x[static_cast<size_t>(a)].data() + start, p.N, f.data());
    f.run();
    Y->assign(f.data(), f.data() + p.N);
}

// The scattered-pilot comb: per symbol the residue class (mod dx dy) of bins
// with the boosted power, advancing by dx per symbol
bool OfdmEstimator::find_pattern(const Prep& p, int B, PilotPattern* out, std::vector<int>* residues, double* score) {
    const int N = p.N;
    std::vector<std::vector<double>> pw;   // per symbol: |Y(b)|^2 for b = -B..B
    std::vector<char> fol;
    std::vector<kp::cf> Y;
    for (size_t j = 0; j < p.starts.size() && static_cast<int>(pw.size()) < MAX_PATTERN_SYMBOLS; j++) {
        std::vector<double> q(static_cast<size_t>(2 * B + 1), 0.0);
        for (size_t a = 0; a < p.x.size(); a++) {
            symbol_fft(p, static_cast<int>(a), static_cast<size_t>(p.starts[j]), &Y);
            for (int bb = -B; bb <= B; bb++) q[static_cast<size_t>(bb + B)] += std::norm(Y[static_cast<size_t>(((bb % N) + N) % N)]);
        }
        pw.push_back(std::move(q));
        fol.push_back(pw.size() > 1 && p.follows[j]);
    }
    if (pw.size() < 8) return false;
    // each bin against its own average over the symbols: the channel's
    // fading stays put while the pilots move (two paths 37 us apart fade
    // with a period of ~12 carriers in 2k mode - the classes' powers showed
    // the fading, not the pilots)
    {
        std::vector<double> mean(static_cast<size_t>(2 * B + 1), 0.0);
        for (const auto& q : pw)
            for (size_t i = 0; i < q.size(); i++) mean[i] += q[i];
        for (auto& q : pw)
            for (size_t i = 0; i < q.size(); i++) q[i] = mean[i] > 0 ? q[i] * static_cast<double>(pw.size()) / mean[i] : 0.0;
    }
    double best = 0;
    for (const auto& pat : std_->patterns(mode_)) {
        const int M = pat.dx * pat.dy;
        std::vector<int> res;
        double ratio = 0;
        for (const auto& q : pw) {
            std::vector<double> cls(static_cast<size_t>(M), 0.0);
            std::vector<int> cnt(static_cast<size_t>(M), 0);
            for (int bb = -B; bb <= B; bb++) {
                const int r = ((bb % M) + M) % M;
                cls[static_cast<size_t>(r)] += q[static_cast<size_t>(bb + B)];
                cnt[static_cast<size_t>(r)]++;
            }
            int rb = 0;
            double tot = 0;
            for (int r = 0; r < M; r++) {
                cls[static_cast<size_t>(r)] /= std::max(1, cnt[static_cast<size_t>(r)]);
                tot += cls[static_cast<size_t>(r)];
                if (cls[static_cast<size_t>(r)] > cls[static_cast<size_t>(rb)]) rb = r;
            }
            res.push_back(rb);
            ratio += cls[static_cast<size_t>(rb)] / std::max(1e-30, tot / M);
        }
        ratio /= pw.size();
        int good = 0, pairs = 0;
        for (size_t j = 1; j < res.size(); j++) {
            if (!fol[j]) continue;
            pairs++;
            good += ((res[j] - res[j - 1] - pat.dx) % M + M) % M == 0;
        }
        if (pairs < 4) continue;
        const double cons = static_cast<double>(good) / pairs;
        const double sc = cons * (ratio - 1);
        if (cons > 0.6 && ratio > 1.15 && sc > best) {
            best = sc;
            *out = pat;
            *residues = res;
        }
    }
    *score = best;
    return best > 0;
}

// Which absolute carrier is bin 0: the candidate whose pilots / their values
// give the sparsest impulse response. Each symbol's pattern phase follows
// from its own boosted residue (res[j]) and the candidate.
bool OfdmEstimator::find_offset(const Prep& p, int B, const PilotPattern& pat, const std::vector<int>& res, int* k_off) {
    const int N = p.N, M = pat.dx * pat.dy;
    const int ns = std::min<int>(OFFSET_SYMBOLS, static_cast<int>(std::min(res.size(), p.starts.size())));
    std::vector<std::vector<kp::cf>> Ys(static_cast<size_t>(ns));
    for (int j = 0; j < ns; j++) symbol_fft(p, 0, static_cast<size_t>(p.starts[static_cast<size_t>(j)]), &Ys[static_cast<size_t>(j)]);
    const int J = (2 * B) / M + 2;
    const int P = pow2_at_least(2 * J);
    Fft& ifft = fft_.get(P, +1);
    std::vector<double> prof(static_cast<size_t>(P));
    double best = 0;
    int best_k = 0;
    std::vector<double> metrics;
    // candidates: bin 0 = carrier k with res[0] + k - kmin a multiple of dx
    int k0 = mode_.kmin - B - M;
    k0 += (((mode_.kmin - res[0] - k0) % pat.dx) + pat.dx) % pat.dx;
    for (int k = k0; k <= mode_.kmax + B; k += pat.dx) {
        std::fill(prof.begin(), prof.end(), 0.0);
        int used = 0;
        for (int j = 0; j < ns; j++) {
            const int r = (((res[static_cast<size_t>(j)] + k - mode_.kmin) % M) + M) % M;
            if (r % pat.dx != 0) continue;
            const int ph = r / pat.dx;
            const auto& Y = Ys[static_cast<size_t>(j)];
            std::fill(ifft.data(), ifft.data() + P, kp::cf(0, 0));
            int i = 0;
            for (int bb = -B; bb <= B; bb++) {
                const int kk = bb + k;
                if (!std_->is_pilot(mode_, pat, kk, ph)) continue;
                const kp::cf v = std_->pilot_value(mode_, pat, kk);
                if (std::norm(v) < 1e-12 || i >= P) continue;
                ifft.data()[i++] = Y[static_cast<size_t>(((bb % N) + N) % N)] / v;
            }
            if (i < 8) continue;
            for (int q = 0; q < i; q++) ifft.data()[q] *= static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (q + 0.5) / i));
            ifft.run();
            for (int q = 0; q < P; q++) prof[static_cast<size_t>(q)] += std::norm(ifft.data()[q]);
            used++;
        }
        if (used == 0) continue;
        double mx = 0, sum = 0;
        for (double v : prof) {
            mx = std::max(mx, v);
            sum += v;
        }
        const double metric = mx / (sum / P);
        metrics.push_back(metric);
        if (metric > best) {
            best = metric;
            best_k = k;
        }
    }
    if (metrics.empty()) return false;
    std::nth_element(metrics.begin(), metrics.begin() + static_cast<long>(metrics.size() / 2), metrics.end());
    const double med = metrics[metrics.size() / 2];
    if (!(best > 3 * med)) return false;
    *k_off = best_k;
    return true;
}

// part[g][i]: symbol g's contribution to comb entry i (have[g][i] = set);
// the signs (signs[0] = +1) whose combined comb gives antenna 0 the most
// peaked impulse response
double OfdmEstimator::best_signs(const std::vector<std::vector<kp::cf>>& part, const std::vector<std::vector<char>>& have, int J,
                                 std::vector<int>* signs) {
    const int G = static_cast<int>(part.size());
    const int P = std::min(MAX_TAPS, pow2_at_least(2 * J));
    Fft& ifft = fft_.get(P, +1);
    signs->assign(static_cast<size_t>(G), 1);
    double best = -1;
    int bm = 0;
    for (int mask = 0; mask < (1 << (G - 1)); mask++) {
        std::fill(ifft.data(), ifft.data() + P, kp::cf(0, 0));
        for (int g = 0; g < G; g++) {
            const float s = (g > 0 && (mask >> (g - 1)) & 1) ? -1.0f : 1.0f;
            for (int i = 0; i < J; i++)
                if (have[static_cast<size_t>(g)][static_cast<size_t>(i)])
                    ifft.data()[i] += part[static_cast<size_t>(g)][static_cast<size_t>(i)] * s *
                                      static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / J));
        }
        ifft.run();
        double mx = 0, sum = 0;
        for (int q = 0; q < P; q++) {
            const double v = std::norm(ifft.data()[q]);
            mx = std::max(mx, v);
            sum += v;
        }
        const double pk = sum > 0 ? mx / (sum / P) : 0;
        if (pk > best) {
            best = pk;
            bm = mask;
        }
    }
    for (int g = 1; g < G; g++) (*signs)[static_cast<size_t>(g)] = ((bm >> (g - 1)) & 1) ? -1 : 1;
    return best;
}

// the shift (taps) of profile b against a within +-L, parabola-refined
double OfdmEstimator::pdp_shift(const std::vector<double>& a, const std::vector<double>& b, int L) {
    const int P = static_cast<int>(a.size());
    std::vector<double> c(static_cast<size_t>(2 * L + 1));
    for (int l = -L; l <= L; l++) {
        double s = 0;
        for (int t = 0; t < P; t++) s += a[static_cast<size_t>(t)] * b[static_cast<size_t>(((t + l) % P + P) % P)];
        c[static_cast<size_t>(l + L)] = s;
    }
    int bl = 0;
    for (int l = -L; l <= L; l++)
        if (c[static_cast<size_t>(l + L)] > c[static_cast<size_t>(bl + L)]) bl = l;
    if (bl == -L || bl == L) return bl;
    const double y0 = c[static_cast<size_t>(bl + L - 1)], y1 = c[static_cast<size_t>(bl + L)], y2 = c[static_cast<size_t>(bl + L + 1)];
    const double den = y0 - 2 * y1 + y2;
    return bl + (den < 0 ? 0.5 * (y0 - y2) / den : 0);
}

std::string OfdmEstimator::describe() const {
    if (!locked_) return "";
    return std_->name() + " " + std_->describe(mode_, pat_);
}

double OfdmEstimator::detect(const Burst& b, const EstOptions& o) {
    locked_ = false;
    double best = 0;
    OfdmMode bm;
    std::vector<double> bfs;   // framed: the preambles found for the best mode's bandwidth
    // antenna 0 resampled once per rate (the modes of one bandwidth share it),
    // then only the guard correlation per FFT size / guard
    std::map<long long, std::vector<kp::cf>> rs;
    std::map<std::pair<long long, int>, CpProducts> prods;
    std::map<long long, std::vector<double>> pre;   // framed: preambles per elementary rate
    const bool framed = std_->framed();
    const size_t n = framed ? b.n : std::min(b.n, static_cast<size_t>(DETECT_S * b.rate));
    if (std_->tracked()) {
        // per rate and FFT size: track with the shortest guard (a longer one
        // correlates worse), then the longest allowed guard that fits the
        // measured period
        const auto all = std_->modes(o.bw_mhz);
        std::map<std::pair<long long, int>, std::pair<double, double>> done;   // rho, period
        for (const auto& m : all) {
            int D = 1;
            while (m.fs / D > b.rate * 1.0001) D *= 2;
            if (m.nfft % D != 0 || D > 64) continue;
            const double rate = m.fs / D;
            const long long key = std::llround(rate);
            const int N = m.nfft / D;
            auto it = done.find({key, N});
            if (it == done.end()) {
                int gmin = INT32_MAX;
                for (const auto& q : all)
                    if (std::llround(q.fs) == std::llround(m.fs) && q.nfft == m.nfft) gmin = std::min(gmin, q.ncp);
                auto& x = rs[key];
                if (x.empty()) Resampler(b.rate, rate).run(b.ch[0].data(), n, &x);
                auto& cp = prods[{key, N}];
                if (cp.pr.empty()) cp_products(x, N, &cp);
                std::vector<double> sym;
                std::vector<char> fol;
                double period = 0;
                const double r = track(cp, N, std::max(1, gmin / D), std_->max_excess_T() / D + (m.ncp - gmin) / D, &sym, &fol, &period);
                it = done.emplace(std::make_pair(key, N), std::make_pair(sym.size() >= 8 ? r : 0.0, period)).first;
            }
            const double rho = it->second.first, period = it->second.second;
            // this guard fits the period (+ excess >= 0), and is the longest that does
            const double g = static_cast<double>(m.ncp) / D;
            if (rho <= 0 || N + g > period + 1.5) continue;
            const bool longest = [&] {
                for (const auto& q : all)
                    if (std::llround(q.fs) == std::llround(m.fs) && q.nfft == m.nfft && q.ncp > m.ncp &&
                        N + static_cast<double>(q.ncp) / D <= period + 1.5)
                        return false;
                return true;
            }();
            if (longest && rho > best) {
                best = rho;
                bm = m;
            }
        }
    } else
    for (const auto& m : std_->modes(o.bw_mhz)) {
        int D = 1;
        while (m.fs / D > b.rate * 1.0001) D *= 2;
        if (m.nfft % D != 0 || D > 64) continue;
        const double rate = m.fs / D;
        const long long key = std::llround(rate);
        if (framed) {
            auto it = pre.find(std::llround(m.fs));
            if (it == pre.end()) it = pre.emplace(std::llround(m.fs), std_->frame_starts(b.ch[0], b.rate, m.fs)).first;
            if (it->second.empty()) continue;
        }
        auto& x = rs[key];
        if (x.empty()) Resampler(b.rate, rate).run(b.ch[0].data(), n, &x);
        const int N = m.nfft / D, ncp = std::max(1, static_cast<int>(std::lround(static_cast<double>(m.ncp) / D)));
        double rho;
        if (framed) {
            std::vector<double> sym;
            std::vector<char> fol;
            const auto& fsx = pre[std::llround(m.fs)];
            frame_grid(fsx, rate / b.rate, static_cast<long>(x.size()), m, N, ncp, &sym, &fol);
            if (sym.size() < 8) continue;
            rho = cp_at(x, sym, N, ncp, nullptr);
        } else {
            auto& cp = prods[{key, N}];
            if (cp.pr.empty()) cp_products(x, N, &cp);
            int off;
            double eps;
            if (!cp_fold(cp, N, ncp, &off, &rho, &eps)) continue;
        }
        if (rho > best) {
            best = rho;
            bm = m;
            if (framed) bfs = pre[std::llround(m.fs)];
        }
    }
    if (o.verbose)
        host_.log(std_->name() + ": best CP correlation " + fmtd("%.3f", best) + (best > 0 ? " for " + bm.label + fmtd(" %g MHz", bm.bw_mhz) : "") +
                  (framed ? ", " + std::to_string(bfs.size()) + " preambles" : ""));
    if (best < RHO_MIN) return 0;
    mode_ = bm;
    // pilots: on every antenna (the boosted classes stand out better summed)
    Prep p;
    if (!prepare(b, mode_, b.m, framed ? static_cast<double>(b.n) / b.rate : 2 * DETECT_S, &p, framed ? &bfs : nullptr)) return 0;
    const int B = valid_bins(p, b.rate);
    std::vector<int> res;
    double ps = 0;
    if (!find_pattern(p, B, &pat_, &res, &ps)) {
        if (o.verbose) host_.log(std_->name() + ": no scattered-pilot comb found");
        return 0;
    }
    if (!find_offset(p, B, pat_, res, &k_off_)) {
        if (o.verbose) host_.log(std_->name() + ": pilot comb " + pat_.label + fmtd(" (score %.2f) but no carrier offset", ps));
        return 0;
    }
    // per-symbol pilot signs: a standard without them rejects a signal with them
    if (std_->combine_symbols() && !std_->symbol_signs() && pat_.dy <= 4) {
        int groups = 0, plain = 0;
        sign_check_ = true;
        sign_groups_ = sign_plain_ = 0;
        locked_ = true;
        process(b, o, [](const CirSnapshot&) {});
        locked_ = false;
        sign_check_ = false;
        groups = sign_groups_;
        plain = sign_plain_;
        if (o.verbose) host_.log(std_->name() + ": " + std::to_string(plain) + " of " + std::to_string(groups) + " pilot combs without sign flips");
        if (groups >= 4 && plain < 0.6 * groups) return 0;
    }
    locked_ = true;
    fails_ = 0;
    rho_ = p.rho;
    if (o.verbose)
        host_.log(std_->name() + ": " + describe() + fmtd(", rho %.2f", p.rho) + ", bin 0 = carrier " + std::to_string(k_off_));
    return std::min(1.0, best * 2);
}

bool OfdmEstimator::process(const Burst& b, const EstOptions& o, const Sink& sink) {
    if (!locked_) return false;
    const auto tp0 = std::chrono::steady_clock::now();
    Prep p;
    const bool prep_ok = prepare(b, mode_, b.m, PROCESS_S, &p);
    if (o.verbose && !sign_check_)
        host_.log(fmtd("prepare %.3f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count()));
    if (!prep_ok || p.rho < RHO_MIN) {
        if (!sign_check_ && ++fails_ >= LOST_BURSTS) locked_ = false;
        return locked_;
    }
    rho_ = p.rho;
    cfo_carriers_ = p.eps;
    frames_ = p.frames;
    const int N = p.N, M = pat_.dx * pat_.dy, m = b.m;
    const int B = valid_bins(p, b.rate);
    const double df = p.rate / N;
    // the standard's signalling carriers (TPS / TMCC) of this mode
    {
        const std::string mk = mode_.label + "/" + std::to_string(mode_.bw_mhz);
        if (mk != sig_mode_) {
            sig_mode_ = mk;
            std_->signal_carriers(mode_, &sig_k_, &ref_k_);
            std_->reset_signalling();
        }
    }
    OfdmStandard::SigBurst sb;
    std::vector<kp::cf> Y;
    // the signalling carriers' values of symbol j (antenna 0; the window's
    // fractional start undone like the pilots')
    auto take_sig = [&](const Prep& q, size_t j) {
        const double fr = j < q.frac.size() ? q.frac[j] : 0.0;
        auto take = [&](const std::vector<int>& ks, std::vector<kp::cf>* out) {
            out->assign(ks.size(), kp::cf(0, 0));
            for (size_t i = 0; i < ks.size(); i++) {
                const int bb = ks[i] - k_off_;
                if (bb < -B || bb > B) continue;
                (*out)[i] = Y[static_cast<size_t>(((bb % N) + N) % N)] * std::polar(1.0f, static_cast<float>(2 * M_PI * bb * fr / N));
            }
        };
        sb.sig.emplace_back();
        take(sig_k_, &sb.sig.back());
        sb.ref.emplace_back();
        take(ref_k_, &sb.ref.back());
        sb.follows.push_back(q.follows[j]);
    };
    // a signalling frame longer than the part analysed (ISDB-T mode 3: 204
    // symbols ~ 0.23 s) is read from the whole burst, antenna 0 only
    const bool sig_on = !sig_k_.empty() && !sign_check_;
    Prep ps;
    const bool sig_own = sig_on && static_cast<int>(p.starts.size()) < std_->signalling_symbols(mode_) &&
                         b.n > static_cast<size_t>(1.05 * PROCESS_S * b.rate) && prepare(b, mode_, 1, static_cast<double>(b.n) / b.rate, &ps) &&
                         ps.N == N;
    if (sig_own)
        for (size_t j = 0; j < ps.starts.size(); j++) {
            symbol_fft(ps, 0, static_cast<size_t>(ps.starts[j]), &Y);
            take_sig(ps, j);
        }
    // per symbol: its pattern phase (the boosted class among the dy expected ones)
    struct Sym { long start; int ph; bool follows; int l; int run; double frac; };
    std::vector<Sym> syms;
    // the bins' powers, each against its average over the symbols (as in
    // find_pattern: the channel's fading out, the moving pilots stay)
    std::vector<std::vector<float>> pwr(p.starts.size(), std::vector<float>(static_cast<size_t>(2 * B + 1)));
    std::vector<double> pmean(static_cast<size_t>(2 * B + 1), 0.0);
    for (size_t j = 0; j < p.starts.size(); j++) {
        symbol_fft(p, 0, static_cast<size_t>(p.starts[j]), &Y);
        if (sig_on && !sig_own) take_sig(p, j);
        for (int bb = -B; bb <= B; bb++) {
            const double v = std::norm(Y[static_cast<size_t>(((bb % N) + N) % N)]);
            pwr[j][static_cast<size_t>(bb + B)] = static_cast<float>(v);
            pmean[static_cast<size_t>(bb + B)] += v;
        }
    }
    for (auto& v : pmean) v = v > 0 ? static_cast<double>(p.starts.size()) / v : 0.0;
    for (size_t j = 0; j < p.starts.size(); j++) {
        std::vector<double> cls(static_cast<size_t>(pat_.dy), 0.0);
        double all = 0;
        for (int bb = -B; bb <= B; bb++) {
            const double v = pwr[j][static_cast<size_t>(bb + B)] * pmean[static_cast<size_t>(bb + B)];
            all += v;
            const int k = bb + k_off_;
            for (int ph = 0; ph < pat_.dy; ph++)
                if (std_->is_pilot(mode_, pat_, k, ph)) cls[static_cast<size_t>(ph)] += v;
        }
        int ph = 0;
        for (int q = 1; q < pat_.dy; q++)
            if (cls[static_cast<size_t>(q)] > cls[static_cast<size_t>(ph)]) ph = q;
        double second = 0;
        for (int q = 0; q < pat_.dy; q++)
            if (q != ph) second = std::max(second, cls[static_cast<size_t>(q)]);
        // a pilot class carries 1 / M of the bins: boosted, it holds clearly
        // more - and clearly more than the other phases (preamble / boundary
        // symbols have pilots on every phase)
        const double share = cls[static_cast<size_t>(ph)] / std::max(1e-30, all) * (cls[static_cast<size_t>(ph)] > 1.3 * second ? 1.0 : 0.0);
        const int run = syms.empty() ? 0 : syms.back().run + (p.follows[j] ? 0 : 1);
        syms.push_back({share > 1.15 / M ? p.starts[j] : -1, ph, p.follows[j] != 0, j < p.lidx.size() ? p.lidx[j] : -1, run,
                        j < p.frac.size() ? p.frac[j] : 0.0});
    }
    if (!sb.sig.empty()) std_->signalling(mode_, sb);
    // the impulse responses: dy symbols with a full cycle of phases combined
    // (pilot values per carrier only), else every symbol alone
    const bool comb_all = std_->combine_symbols() && pat_.dy <= 4;
    const bool signs = comb_all && std_->symbol_signs();
    const int dk = comb_all ? pat_.dx : M;
    // the comb carriers inside the slice (k = kfirst + i dk)
    int kfirst = 0, J = 0;
    {
        int lo = INT32_MAX, hi = INT32_MIN;
        for (int bb = -B; bb <= B; bb++) {
            const int k = bb + k_off_;
            if (k < mode_.kmin || k > mode_.kmax) continue;
            lo = std::min(lo, k);
            hi = std::max(hi, k);
        }
        if (lo > hi) return locked_;
        auto any_phase = [&](int k) {
            for (int ph = 0; ph < pat_.dy; ph++)
                if (std_->is_pilot(mode_, pat_, k, ph)) return true;
            return false;
        };
        kfirst = lo;
        while (kfirst <= hi && !(comb_all ? any_phase(kfirst) : std_->is_pilot(mode_, pat_, kfirst, 0))) kfirst++;
        J = (hi - kfirst) / dk + 1;
    }
    if (J < 8) return locked_;
    const int P = std::min(MAX_TAPS, pow2_at_least(2 * J));
    Fft& ifft = fft_.get(P, +1);
    std::vector<double> win(static_cast<size_t>(J));
    for (int i = 0; i < J; i++) win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / J);
    CirSnapshot cs;
    cs.m = m;
    cs.ntap = P;
    cs.tap_us = 1e6 / (static_cast<double>(P) * dk * df);
    cs.h.assign(static_cast<size_t>(m) * P, kp::cf(0, 0));
    const size_t group = comb_all ? static_cast<size_t>(pat_.dy) : 1;
    // the groups: per symbol and antenna its part of the comb, its window start
    struct Grp {
        size_t first;                                         // index of its first symbol
        int run;
        double t0, t1;
        std::vector<std::vector<std::vector<kp::cf>>> part;   // symbol x antenna x J
        std::vector<std::vector<char>> have;                  // symbol x J
        std::vector<int> sg;                                  // pilot signs
        std::vector<double> w;                                // window starts (samples, fractional)
    };
    std::vector<Grp> grps;
    size_t j = 0;
    while (j + group <= syms.size()) {
        // a group: dy consecutive symbols with every phase once
        bool ok = true;
        std::vector<char> phs(static_cast<size_t>(pat_.dy), 0);
        for (size_t g = 0; g < group; g++) {
            const Sym& s = syms[j + g];
            if (s.start < 0 || (g > 0 && !s.follows)) ok = false;
            else if (signs && s.l < 0) ok = false;   // its pilots' sign unknown
            else if (comb_all) {
                if (phs[static_cast<size_t>(s.ph)]) ok = false;
                phs[static_cast<size_t>(s.ph)] = 1;
            }
        }
        if (!ok) {
            j++;
            continue;
        }
        int kf = kfirst;
        if (!comb_all) {   // this symbol's comb starts at its own phase
            while (!std_->is_pilot(mode_, pat_, kf, syms[j].ph) && kf < kfirst + M) kf++;
        }
        Grp gr;
        gr.first = j;
        gr.run = syms[j].run;
        gr.t0 = b.t0 + static_cast<double>(syms[j].start) / p.rate;
        gr.t1 = b.t0 + static_cast<double>(syms[j + group - 1].start + N) / p.rate;
        gr.part.assign(group, std::vector<std::vector<kp::cf>>(static_cast<size_t>(m), std::vector<kp::cf>(static_cast<size_t>(J), kp::cf(0, 0))));
        gr.have.assign(group, std::vector<char>(static_cast<size_t>(J), 0));
        gr.sg.assign(group, 1);
        int filled = 0;
        for (size_t g = 0; g < group; g++) {
            const Sym& s = syms[j + g];
            gr.w.push_back(static_cast<double>(s.start) + s.frac);
            if (signs) gr.sg[g] = std_->symbol_sign(s.l);
            for (int a = 0; a < m; a++) {
                auto& pa = gr.part[g][static_cast<size_t>(a)];
                symbol_fft(p, a, static_cast<size_t>(s.start), &Y);
                for (int i = 0; i < J; i++) {
                    const int k = kf + i * dk;
                    if (!std_->is_pilot(mode_, pat_, k, s.ph)) continue;
                    const int bb = k - k_off_;
                    if (bb < -B || bb > B) continue;
                    const kp::cf v = std_->pilot_value(mode_, pat_, k);
                    if (std::norm(v) < 1e-12) continue;
                    // the window started frac samples early: the content looks that much
                    // later - undone per bin
                    pa[static_cast<size_t>(i)] = Y[static_cast<size_t>(((bb % N) + N) % N)] / v *
                                                 std::polar(1.0f, static_cast<float>(2 * M_PI * bb * s.frac / N));
                    if (a == 0) {
                        gr.have[g][static_cast<size_t>(i)] = 1;
                        filled++;
                    }
                }
            }
        }
        if (filled >= J * 3 / 4) grps.push_back(std::move(gr));
        j += group;
    }
    // Every symbol carries a common phase that steps from symbol to symbol
    // (the carrier offset's whole-bin part times the symbol period, which
    // the guard - and ATSC 3.0's excess samples - make fractional; a residual
    // fractional offset). The dy symbols of a comb must agree, else ghosts a
    // quarter / half of the range away: the step is measured on the pilots
    // that symbols dy apart share (the ratio gives dy steps; the multiples of
    // 2 pi / dy it can't tell apart only move the whole response circularly)
    // and taken out.
    double step = 0;
    if (comb_all && group > 1) {
        std::complex<double> acc = 0;
        for (size_t q = 0; q + 1 < grps.size(); q++) {
            const Grp& g0 = grps[q];
            const Grp& g1 = grps[q + 1];
            if (g1.run != g0.run || g1.first != g0.first + group) continue;
            for (size_t g = 0; g < group; g++)
                for (int a = 0; a < m; a++)
                    for (int i = 0; i < J; i++)
                        if (g0.have[g][static_cast<size_t>(i)] && g1.have[g][static_cast<size_t>(i)])
                            acc += std::complex<double>(g1.part[g][static_cast<size_t>(a)][static_cast<size_t>(i)]) * static_cast<double>(g1.sg[g]) *
                                   std::conj(std::complex<double>(g0.part[g][static_cast<size_t>(a)][static_cast<size_t>(i)]) * static_cast<double>(g0.sg[g]));
        }
        if (std::abs(acc) > 0) step = std::arg(acc) / static_cast<double>(group);
        else step = step_;
        step_ = step;
    }
    auto comb_of = [&](const Grp& gr, int a, std::vector<kp::cf>* c) {
        c->assign(static_cast<size_t>(J), kp::cf(0, 0));
        for (size_t g = 0; g < gr.part.size(); g++) {
            const kp::cf rot = std::polar(1.0f, static_cast<float>(-step * static_cast<double>(g))) * static_cast<float>(gr.sg[g]);
            for (int i = 0; i < J; i++)
                if (gr.have[g][static_cast<size_t>(i)])
                    (*c)[static_cast<size_t>(i)] += gr.part[g][static_cast<size_t>(a)][static_cast<size_t>(i)] * rot * static_cast<float>(win[static_cast<size_t>(i)]);
        }
    };
    int snaps = static_cast<int>(grps.size());
    struct Comb { int run; double t0, t1; std::vector<std::vector<kp::cf>> c; };
    std::vector<Comb> combs;
    for (const auto& gr : grps) {
        // the symbols' signs: known (DVB-T2: the frame PN), or checked (a
        // standard without them must not need any)
        if (comb_all && sign_check_) {
            std::vector<std::vector<kp::cf>> p0(group);
            for (size_t g = 0; g < group; g++) {
                p0[g] = gr.part[g][0];
                for (auto& v : p0[g]) v *= std::polar(1.0f, static_cast<float>(-step * static_cast<double>(g)));
            }
            std::vector<int> found;
            best_signs(p0, gr.have, J, &found);
            // (+, -, +, -) is the same CIR moved by half its range: counts as plain
            bool plain = true, alt = true;
            for (size_t g = 0; g < found.size(); g++) {
                plain = plain && found[g] > 0;
                alt = alt && found[g] == (g % 2 ? -1 : 1);
            }
            sign_groups_++;
            sign_plain_ += plain || alt;
            continue;
        }
        Comb c;
        c.run = gr.run;
        c.t0 = gr.t0;
        c.t1 = gr.t1;
        c.c.resize(static_cast<size_t>(m));
        for (int a = 0; a < m; a++) comb_of(gr, a, &c.c[static_cast<size_t>(a)]);
        combs.push_back(std::move(c));
    }
    if (sign_check_) return true;
    // runs (frames / stretches between preambles) each have their own timing
    // reference (a preamble position, a guard peak): their responses are
    // aligned to the first run's by the power delay profile
    std::vector<double> run_shift;
    auto cir0 = [&](const std::vector<kp::cf>& c, std::vector<double>* pdp) {
        std::fill(ifft.data(), ifft.data() + P, kp::cf(0, 0));
        std::copy(c.begin(), c.end(), ifft.data());
        ifft.run();
        for (int t = 0; t < P; t++) (*pdp)[static_cast<size_t>(t)] += std::norm(ifft.data()[t]);
    };
    if (!combs.empty() && combs.back().run > combs.front().run) {
        const int r0 = combs.front().run, nr = combs.back().run - r0 + 1;
        std::vector<std::vector<double>> pdp(static_cast<size_t>(nr), std::vector<double>(static_cast<size_t>(P), 0.0));
        for (const auto& c : combs) cir0(c.c[0], &pdp[static_cast<size_t>(c.run - r0)]);
        run_shift.assign(static_cast<size_t>(nr), 0.0);
        for (int r = 1; r < nr; r++) run_shift[static_cast<size_t>(r)] = pdp_shift(pdp[0], pdp[static_cast<size_t>(r)], std::min(64, P / 8));
        if (o.verbose) {
            std::string t = "runs:";
            for (int r = 0; r < nr; r++) {
                int cnt = 0;
                for (const auto& c : combs) cnt += c.run - r0 == r;
                t += fmtd(" %.2f", run_shift[static_cast<size_t>(r)]) + "(" + std::to_string(cnt) + ")";
            }
            host_.log(t);
        }
        for (auto& c : combs) {
            const double s = run_shift[static_cast<size_t>(c.run - r0)];
            if (s == 0) continue;
            for (auto& ca : c.c)
                for (int i = 0; i < J; i++) ca[static_cast<size_t>(i)] *= std::polar(1.0f, static_cast<float>(2 * M_PI * i * s / P));
        }
    }
    for (const auto& c : combs) {
        for (int a = 0; a < m; a++) {
            std::fill(ifft.data(), ifft.data() + P, kp::cf(0, 0));
            std::copy(c.c[static_cast<size_t>(a)].begin(), c.c[static_cast<size_t>(a)].end(), ifft.data());
            ifft.run();
            std::copy_n(ifft.data(), P, cs.h.data() + static_cast<size_t>(a) * P);
        }
        cs.t0 = c.t0;
        cs.t1 = c.t1;
        sink(cs);
    }
    if (o.verbose)
        host_.log(fmtd("burst %.3f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count()) +
                  ", " + std::to_string(snaps) + " snapshots" + (std_->framed() ? ", " + std::to_string(frames_) + " preambles" : ""));
    if (snaps == 0) {
        if (++fails_ >= LOST_BURSTS) locked_ = false;
    } else {
        fails_ = 0;
    }
    // the channel centre: carrier kc at bin kc - k_off, plus the fractional offset
    if (std::isfinite(b.rf_hz)) {
        centre_hz_ = b.rf_hz + std_->centre_bins(mode_, k_off_, cfo_carriers_, b.rf_hz, df) * df;
        host_.fact("Channel centre", fmtd("%.4f MHz", centre_hz_ / 1e6));
    }
    host_.fact("Guard correlation", fmtd("%.2f", rho_));
    return locked_;
}

}  // namespace sfn
