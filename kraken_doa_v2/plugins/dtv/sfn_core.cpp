#include "sfn_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sfn {

namespace {

constexpr int CONFIRM_HITS = 3;          // cycles a new transmitter must be seen in
constexpr int TENTATIVE_MISSES = 3;      // an unconfirmed one is dropped after these
constexpr double TRACK_TTL_S = 60;       // a confirmed one is forgotten this long after the last sighting
constexpr double AVG_S = 4.0;            // a transmitter's covariance average (the host averages again)
constexpr double SIG_SAME = 0.8;
constexpr double ECHO_REL = 0.02;        // -17 dB: a same-direction peak this much weaker is an echo         // |u^H v|^2 above: about the same direction
constexpr double C_US_KM = 0.299792458;  // km of path per us

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(h), v.end());
    return v[h];
}

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

void SfnCore::reset() {
    C_.clear();
    m_ = ntap_ = 0;
    snaps_ = 0;
    for (const auto& t : tracks_)
        if (t.confirmed) host_.table_remove(tid(t.id));
    tracks_.clear();
    prof_hist_.clear();
    main_id_ = -1;
    // the host's talkers go too (the 📡 DF panel's list): separation off,
    // retune, another standard
    host_.spatial_remove("");
}

void SfnCore::set_profile_only(bool on) {
    if (on == profile_only_) return;
    reset();
    profile_only_ = on;
    // no path table without separation (report() sends its columns)
    if (on) host_.table_columns({});
}

int SfnCore::strongest() {
    int best = -1, cur = -1;
    for (size_t i = 0; i < tracks_.size(); i++) {
        if (!tracks_[i].confirmed) continue;
        if (best < 0 || tracks_[i].pw > tracks_[static_cast<size_t>(best)].pw) best = static_cast<int>(i);
        if (tracks_[i].id == main_id_) cur = static_cast<int>(i);
    }
    // the main path stays the main path unless another is 1 dB stronger
    // (two paths of about the same power would swap it every burst)
    if (cur >= 0 && best >= 0 && tracks_[static_cast<size_t>(best)].pw < 1.26 * tracks_[static_cast<size_t>(cur)].pw) best = cur;
    main_id_ = best >= 0 ? tracks_[static_cast<size_t>(best)].id : -1;
    return best;
}

int SfnCore::confirmed() const {
    int n = 0;
    for (const auto& t : tracks_) n += t.confirmed;
    return n;
}

void SfnCore::add(const CirSnapshot& s) {
    if (s.m < 1 || s.ntap < 8 || s.h.size() != static_cast<size_t>(s.m * s.ntap)) return;
    if (s.m != m_ || s.ntap != ntap_ || std::fabs(s.tap_us - tap_us_) > 1e-9) {
        // another grid: what was summed doesn't fit (and neither do the tracks' delays)
        if (s.ntap != last_ntap_ || std::fabs(s.tap_us - last_tap_us_) > 1e-9) {
            for (const auto& t : tracks_)
                if (t.confirmed) host_.table_remove(tid(t.id));
            if (confirmed() > 0) host_.spatial_remove("");
            tracks_.clear();
            prof_hist_.clear();
            main_id_ = -1;
        }
        m_ = s.m;
        ntap_ = s.ntap;
        tap_us_ = s.tap_us;
        C_.assign(static_cast<size_t>(ntap_), mat_zero(m_));
        snaps_ = 0;
    }
    if (snaps_ == 0) t0_ = s.t0;
    t1_ = s.t1;
    std::vector<cd> v(static_cast<size_t>(m_));
    for (int t = 0; t < ntap_; t++) {
        for (int k = 0; k < m_; k++) v[static_cast<size_t>(k)] = cd(s.h[static_cast<size_t>(k * ntap_ + t)]);
        add_outer(C_[static_cast<size_t>(t)], v.data(), m_);
    }
    snaps_++;
    snaps_total_++;
}

void SfnCore::cycle(const std::string& what) {
    if (snaps_ == 0 || m_ < 1) return;
    const int m = m_, N = ntap_;
    const int S = snaps_;
    snaps_ = 0;
    last_ntap_ = N;
    last_tap_us_ = tap_us_;
    std::vector<Mat> C;
    C.swap(C_);
    C_.assign(static_cast<size_t>(N), mat_zero(m));
    // the estimation floor: the taps without a transmitter (low trace)
    std::vector<double> tr(static_cast<size_t>(N));
    for (int t = 0; t < N; t++) tr[static_cast<size_t>(t)] = trace_re(C[static_cast<size_t>(t)], m);
    const double tmed = median(tr);
    Mat F = mat_zero(m);
    int fn = 0;
    for (int t = 0; t < N; t++)
        if (tr[static_cast<size_t>(t)] < 2 * tmed) {
            for (size_t i = 0; i < F.size(); i++) F[i] += C[static_cast<size_t>(t)][i];
            fn++;
        }
    if (fn == 0 || !(tmed > 0)) return;
    for (auto& x : F) x /= fn;
    Mat Fi;
    if (!inv_herm(F, m, &Fi, 1e-6 * trace_re(F, m))) return;
    std::vector<double> D(static_cast<size_t>(N));
    for (int t = 0; t < N; t++) {
        double d = 0;
        const Mat& Ct = C[static_cast<size_t>(t)];
        for (int i = 0; i < m; i++)
            for (int k = 0; k < m; k++) d += (Fi[static_cast<size_t>(i * m + k)] * Ct[static_cast<size_t>(k * m + i)]).real();
        D[static_cast<size_t>(t)] = d / m;
    }
    const double dmed = std::max(median(D), 1e-12);
    std::vector<double> dev(D.size());
    for (size_t i = 0; i < D.size(); i++) dev[i] = std::fabs(D[i] - dmed);
    const double mad = median(dev);
    const double thr = std::max(dmed * std::pow(10.0, min_snr_db_ / 10), dmed + 8 * mad);
    auto at = [&](int t) { return D[static_cast<size_t>(((t % N) + N) % N)]; };

    // --- peaks (circular), strongest first, >= 2 taps apart ---
    std::vector<int> cand;
    for (int t = 0; t < N; t++)
        if (D[static_cast<size_t>(t)] >= thr && D[static_cast<size_t>(t)] >= at(t - 1) && D[static_cast<size_t>(t)] > at(t + 1))
            cand.push_back(t);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return D[static_cast<size_t>(a)] > D[static_cast<size_t>(b)]; });
    auto cdist = [N](double a, double b) {   // circular tap distance
        double d = std::fmod(std::fabs(a - b), N);
        return std::min(d, N - d);
    };
    std::vector<int> pj;
    for (int t : cand) {
        if (static_cast<int>(pj.size()) >= 2 * max_tx_) break;
        bool close = false;
        for (int q : pj)
            if (cdist(q, t) < 2) close = true;
        if (!close) pj.push_back(t);
    }
    // main-lobe half width of the strongest peak
    int mlw = 1;
    if (!pj.empty()) {
        const int j0 = pj[0];
        const double half = dmed + 0.5 * (D[static_cast<size_t>(j0)] - dmed);
        int k = 1;
        while (k < N / 4 && (at(j0 + k) > half || at(j0 - k) > half)) k++;
        mlw = std::max(1, k);
    }
    struct Peak {
        double d;
        std::vector<cd> u;
        Mat Cs;          // unit trace
        double pw, purity, snr_db;
        bool used = false;
    };
    std::vector<Peak> peaks;
    for (int j : pj) {
        const double a = at(j - 1), b = at(j), c = at(j + 1);
        const double den = a - 2 * b + c;
        const double frac = den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        Mat Cs = mat_zero(m);
        for (int q = j - mlw; q <= j + mlw; q++) {
            const Mat& Cq = C[static_cast<size_t>(((q % N) + N) % N)];
            for (size_t i = 0; i < Cs.size(); i++) Cs[i] += Cq[i] - F[i];
        }
        std::vector<double> ev;
        std::vector<std::vector<cd>> vecs;
        eig_herm(Cs, m, &ev, &vecs);
        double pos = 0;
        for (double e : ev) pos += std::max(0.0, e);
        const double l1 = ev.back();
        if (!(l1 > 0) || !(pos > 0)) continue;
        Peak p;
        p.d = j + frac;
        p.u = vecs.back();
        // positive semidefinite, unit trace
        p.Cs = mat_zero(m);
        for (int r = 0; r < m; r++) {
            const double lam = std::max(0.0, ev[static_cast<size_t>(r)]);
            if (lam > 0) add_outer(p.Cs, vecs[static_cast<size_t>(r)].data(), m, lam);
        }
        const double ptr = trace_re(p.Cs, m);
        for (auto& x : p.Cs) x /= ptr;
        p.pw = l1 / S;
        p.purity = l1 / pos;
        p.snr_db = 10 * std::log10(std::max(b / dmed, 1.0));
        peaks.push_back(std::move(p));
    }
    // sidelobes of a stronger peak (the slice's pulse shape): its direction, close, much weaker
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.pw > b.pw; });
    std::vector<Peak> keep;
    const double rel_floor = !peaks.empty() && max_rel_db_ > 0 ? peaks.front().pw * std::pow(10.0, -max_rel_db_ / 10) : 0.0;
    for (auto& p : peaks) {
        if (m > 1 && p.purity < 0.5) continue;   // two transmitters in one lobe
        if (p.pw < rel_floor) continue;          // a weak reflection
        bool side = false;
        for (const auto& q : keep)
            if ((m == 1 || sig_corr(p.u, q.u) > SIG_SAME) && cdist(p.d, q.d) < 4 * mlw && p.pw < 0.05 * q.pw) side = true;
            // anywhere: an echo of the estimator (pilot comb / repeated
            // reference) - the same direction far weaker; a real transmitter
            // there would show the same bearing anyway
            else if (m > 1 && sig_corr(p.u, q.u) > SIG_SAME && p.pw < ECHO_REL * q.pw) side = true;
        if (!side) keep.push_back(std::move(p));
        if (static_cast<int>(keep.size()) >= max_tx_) break;
    }

    if (profile_only_) {
        std::vector<std::pair<double, double>> paths;
        const double pfloor = !keep.empty() && profile_floor_db_ > 0 ? keep.front().pw * std::pow(10.0, -profile_floor_db_ / 10) : 0.0;
        for (const auto& p : keep)
            if (p.pw >= pfloor) paths.push_back({p.d, p.pw});
        if (!keep.empty()) host_.valid(t0_, t1_);
        report_profile(D, dmed, thr, paths, what);
        return;
    }

    // --- tracks: the timing reference may have moved since the last cycle:
    // the common offset that matches most peaks to tracks ---
    const double now = t1_;
    if (!tracks_.empty() && !keep.empty()) {
        double best_off = 0;
        double best_score = -1;
        auto score = [&](double off) {
            double sc = 0;
            for (const auto& p : keep) {
                double bs = 0;
                for (const auto& t : tracks_) {
                    if (cdist(p.d, t.d + off) > 1.5 + 0.5 * t.misses) continue;
                    const double c = m > 1 ? sig_corr(p.u, t.sig) : 1.0;
                    if (c > 0.3) bs = std::max(bs, 1 + c);
                }
                sc += bs;
            }
            return sc;
        };
        for (size_t a = 0; a < keep.size() && a < 4; a++)
            for (size_t b = 0; b < tracks_.size() && b < 6; b++) {
                double off = keep[a].d - tracks_[b].d;
                off = std::remainder(off, static_cast<double>(N));
                const double sc = score(off) - 0.01 * std::fabs(off);   // ties: the smallest move
                if (sc > best_score) { best_score = sc; best_off = off; }
            }
        if (score(0) >= best_score - 1e-9) best_off = 0;
        for (auto& t : tracks_) t.d = std::fmod(t.d + best_off + N, static_cast<double>(N));
    }
    for (auto& t : tracks_) t.misses++;
    struct Pair { double score; size_t p, k; };
    std::vector<Pair> pairs;
    for (size_t p = 0; p < keep.size(); p++)
        for (size_t k = 0; k < tracks_.size(); k++) {
            const double dd = cdist(keep[p].d, tracks_[k].d), tol = 1.5 + 0.5 * (tracks_[k].misses - 1);
            const double c = m > 1 ? sig_corr(keep[p].u, tracks_[k].sig) : 1.0;
            if (dd < tol && c > 0.3) pairs.push_back({dd / tol + (1 - c), p, k});
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score < b.score; });
    std::vector<char> tk_used(tracks_.size(), 0);
    std::vector<size_t> seen;   // tracks updated this cycle
    for (const auto& pr : pairs) {
        if (keep[pr.p].used || tk_used[pr.k]) continue;
        keep[pr.p].used = true;
        tk_used[pr.k] = 1;
        Track& tk = tracks_[pr.k];
        const Peak& p = keep[pr.p];
        // delay: follows the peak (circular average)
        tk.d = std::fmod(tk.d + 0.5 * std::remainder(p.d - tk.d, static_cast<double>(N)) + N, static_cast<double>(N));
        const double decay = tk.last_t > 0 ? std::exp(-(now - tk.last_t) / AVG_S) : 0.0;
        for (size_t i = 0; i < tk.Cavg.size(); i++) tk.Cavg[i] = decay * tk.Cavg[i] + (1 - decay) * p.Cs[i];
        std::vector<double> ev;
        std::vector<std::vector<cd>> vecs;
        eig_herm(tk.Cavg, m, &ev, &vecs);
        tk.sig = vecs.back();
        tk.pw = p.pw;
        tk.snr_db = p.snr_db;
        tk.hits++;
        tk.misses = 0;
        tk.last_t = now;
        if (!tk.confirmed && tk.hits >= CONFIRM_HITS) {
            tk.confirmed = true;
            tk.first_t = now;
        }
        seen.push_back(pr.k);
    }
    for (auto& p : keep) {
        if (p.used || static_cast<int>(tracks_.size()) >= 2 * max_tx_) continue;
        Track tk;
        tk.id = next_id_++;
        tk.d = p.d;
        tk.sig = p.u;
        tk.Cavg = p.Cs;
        tk.pw = p.pw;
        tk.snr_db = p.snr_db;
        tk.hits = 1;
        tk.last_t = now;
        tracks_.push_back(std::move(tk));
        seen.push_back(tracks_.size() - 1);
    }
    // which tracks were seen (indices change when erasing: mark them)
    std::vector<char> was_seen(tracks_.size(), 0);
    for (size_t i : seen) was_seen[i] = 1;
    // forget: unconfirmed after a few misses, confirmed after TRACK_TTL_S
    for (size_t i = 0; i < tracks_.size();) {
        Track& t = tracks_[i];
        const bool drop = (!t.confirmed && t.misses >= TENTATIVE_MISSES) || (t.confirmed && now - t.last_t > TRACK_TTL_S);
        if (drop) {
            if (t.confirmed) {
                host_.event("Path " + tid(t.id) + " gone", 0.0);
                host_.table_remove(tid(t.id));
                host_.spatial_remove(tid(t.id));
            }
            tracks_.erase(tracks_.begin() + static_cast<long>(i));
            was_seen.erase(was_seen.begin() + static_cast<long>(i));
        } else {
            i++;
        }
    }

    match_ids();

    // --- report: spatial covariances of the confirmed paths seen now ---
    // Delays from the MAIN (strongest) path - a path can arrive before it;
    // the order on the circular delay axis comes from the first to arrive
    // (after the largest gap), whose delay is the table's "From first" column
    double pmax = 0;
    const int first = earliest(), mainp = strongest();
    const double dfirst = first >= 0 ? tracks_[static_cast<size_t>(first)].d : 0;
    auto from_first = [&](double d) { return std::fmod(d - dfirst + N, static_cast<double>(N)); };
    const double main_ff = mainp >= 0 ? from_first(tracks_[static_cast<size_t>(mainp)].d) : 0;
    for (const auto& t : tracks_)
        if (t.confirmed) pmax = std::max(pmax, t.pw);
    for (size_t i = 0; i < tracks_.size(); i++) {
        const Track& t = tracks_[i];
        if (!t.confirmed) continue;
        const std::string id = tid(t.id);
        const double ff = from_first(t.d) * tap_us_;    // after the first to arrive
        const double us = ff - main_ff * tap_us_;       // from the main path (signed)
        const bool is_main = static_cast<int>(i) == mainp;
        const double db = pmax > 0 ? 10 * std::log10(std::max(t.pw / pmax, 1e-9)) : 0;
        const std::string dtxt = fmt(std::fabs(us) < 10 ? "%+.2f" : "%+.1f", us) + " \xC2\xB5s";
        if (t.hits == CONFIRM_HITS && was_seen[i])
            host_.event("New path " + id + ": " + (is_main ? std::string("the main path") : dtxt + " from the main path") + ", " +
                            fmt("%.1f", db) + " dB",
                        0.0);
        // a row only when the path was seen: the page's "Seen" column
        // is the time since its row's last update
        std::vector<std::string> row = {id, fmt("%+.2f", us), fmt("%.2f", ff), fmt("%+.2f", us * C_US_KM), fmt("%.1f", db), fmt("%.0f", t.snr_db)};
        if (has_ids_) row.insert(row.begin() + 1, t.ident);
        if (was_seen[i]) host_.table_row(id, row);
        if (!was_seen[i] || m < 2) continue;
        // this cycle's own covariance of the path (the host averages them)
        kp::Spatial sp;
        sp.id = id;
        sp.label = (is_main ? std::string("main") : dtxt) + " \xC2\xB7 " + fmt("%.1f", db) + " dB";
        if (!t.ident.empty()) sp.label += " \xC2\xB7 " + t.ident;
        sp.start_s = t0_;
        sp.end_s = t1_;
        sp.m = m;
        sp.delay_us = us;
        sp.power_db = db;
        sp.snr_db = t.snr_db;
        sp.ref = is_main;
        // the peak of this cycle that the track took
        const Peak* pk = nullptr;
        for (const auto& p : keep)
            if (p.used && cdist(p.d, t.d) < 2 && (!pk || cdist(p.d, t.d) < cdist(pk->d, t.d))) pk = &p;
        const Mat& cov = pk ? pk->Cs : t.Cavg;
        sp.cov.resize(cov.size());
        for (size_t k = 0; k < cov.size(); k++) sp.cov[k] = kp::cf(static_cast<float>(cov[k].real()), static_cast<float>(cov[k].imag()));
        host_.spatial(sp);
    }
    if (!keep.empty()) host_.valid(t0_, t1_);
    report(D, dmed, thr, what, first, mainp);
}

// The first transmitter on the circular delay axis: the one after the
// largest gap between confirmed transmitters (their spread can exceed half
// the estimator's range - DVB-T2 PP2 with a long guard)
int SfnCore::earliest() const {
    std::vector<std::pair<double, int>> v;
    for (size_t i = 0; i < tracks_.size(); i++)
        if (tracks_[i].confirmed) v.push_back({tracks_[i].d, static_cast<int>(i)});
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const double N = static_cast<double>(ntap_);
    double gap = -1;
    int after = v[0].second;
    for (size_t i = 0; i < v.size(); i++) {
        const double a = v[i].first, b = i + 1 < v.size() ? v[i + 1].first : v[0].first + N;
        if (b - a > gap) {
            gap = b - a;
            after = v[(i + 1) % v.size()].second;
        }
    }
    return after;
}

// identifiers -> confirmed transmitters by spatial signature: the best pairs
// first, each used once; a track keeps its identifier until another matches
// it better (identifiers come and go with the TII frames)
void SfnCore::match_ids() {
    if (m_ < 2 || ids_.empty()) return;
    struct Pair { double c; size_t t, i; };
    std::vector<Pair> pairs;
    for (size_t t = 0; t < tracks_.size(); t++) {
        if (!tracks_[t].confirmed) continue;
        for (size_t i = 0; i < ids_.size(); i++) {
            if (ids_[i].sig.size() != tracks_[t].sig.size()) continue;
            const double c = sig_corr(tracks_[t].sig, ids_[i].sig);
            if (c >= SIG_SAME) pairs.push_back({c, t, i});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.c > b.c; });
    std::vector<char> tu(tracks_.size(), 0), iu(ids_.size(), 0);
    for (const auto& p : pairs) {
        if (tu[p.t] || iu[p.i]) continue;
        tu[p.t] = iu[p.i] = 1;
        Track& tk = tracks_[p.t];
        if (tk.ident != ids_[p.i].id) {
            // the identifier moves here: off any other track
            for (auto& o : tracks_)
                if (&o != &tk && o.ident == ids_[p.i].id) o.ident.clear();
            if (tk.ident.empty())
                host_.event(tid(tk.id) + " is " + ids_[p.i].id, 0.0);
            tk.ident = ids_[p.i].id;
        }
    }
}

// facts + the CIR plot (dB above the floor, x = us from the main path; the
// axis starts a tenth of the window before the first path to arrive)
void SfnCore::report(const std::vector<double>& D, double floor, double thr, const std::string& what, int first, int mainp) {
    const int N = static_cast<int>(D.size());
    double dfirst = first >= 0 ? tracks_[static_cast<size_t>(first)].d : NAN;
    // without paths: from the strongest tap
    if (!std::isfinite(dfirst)) {
        int best = 0;
        for (int t = 1; t < N; t++)
            if (D[static_cast<size_t>(t)] > D[static_cast<size_t>(best)]) best = t;
        dfirst = best;
    }
    auto from_first = [&](double d) { return std::fmod(d - dfirst + N, static_cast<double>(N)); };
    const double main_ff = mainp >= 0 ? from_first(tracks_[static_cast<size_t>(mainp)].d) : 0;
    const int s0 = static_cast<int>(std::floor(dfirst - 0.1 * N));
    const int pool = std::max(1, (N + 999) / 1000);
    kp::Plot pl;
    pl.title = "Channel impulse response";
    pl.x_unit = "\xC2\xB5s";
    pl.y_unit = "dB";
    pl.dx = pool * tap_us_;
    pl.x0 = (s0 - dfirst - main_ff) * tap_us_;
    for (int i = 0; i < N; i += pool) {
        double mx = 0;
        for (int j = i; j < std::min(N, i + pool); j++) mx = std::max(mx, D[static_cast<size_t>(((s0 + j) % N + N) % N)]);
        pl.y.push_back(static_cast<float>(10 * std::log10(std::max(mx / floor, 1.0))));
    }
    pl.y_min = 0;
    pl.threshold = 10 * std::log10(std::max(thr / floor, 1.0));
    double lo = 0, hi = 0;
    for (size_t i = 0; i < tracks_.size(); i++) {
        const Track& t = tracks_[i];
        if (!t.confirmed) continue;
        const double ff = from_first(t.d);
        lo = std::min(lo, ff);
        hi = std::max(hi, ff);
        pl.markers.push_back({(ff - main_ff) * tap_us_,
                              (static_cast<int>(i) == mainp ? std::string("main") : "#" + std::to_string(t.id)) + (t.ident.empty() ? "" : " " + t.ident),
                              tid(t.id)});
    }
    host_.plot(pl);
    std::vector<std::string> cols = {"Path", "From main (\xC2\xB5s)", "From first (\xC2\xB5s)", "Extra path (km)", "Power (dB)", "Above noise (dB)"};
    if (has_ids_) cols.insert(cols.begin() + 1, "ID");
    host_.table_columns(cols);
    host_.fact("Signal", what);
    const int n = confirmed();
    host_.fact("Paths", n == 0 ? std::string("none confirmed yet")
                               : std::to_string(n) + (n == 1 ? " path" : " paths") + ", each with its own bearing" +
                                     (n > 1 ? ", spread " + fmt("%.1f", (hi - lo) * tap_us_) + " \xC2\xB5s" : "") +
                                     (main_ff > 0.5 ? ", the first " + fmt("%.1f", main_ff * tap_us_) + " \xC2\xB5s before the main" : ""));
    host_.fact("Delay window", fmt("%.1f us", N * tap_us_) + " (taps of " + fmt("%.2f us", tap_us_) + ")");
    host_.fact("CIR snapshots", std::to_string(snaps_total_));
}

// profile-only: the plot from a tenth of the window before the strongest
// path, every path marked with its delay and level relative to it
void SfnCore::report_profile(const std::vector<double>& D, double floor, double thr, const std::vector<std::pair<double, double>>& paths,
                             const std::string& what) {
    const int N = static_cast<int>(D.size());
    double d0 = 0, p0 = 0;
    for (const auto& p : paths)
        if (p.second > p0) {
            p0 = p.second;
            d0 = p.first;
        }
    if (paths.empty()) {
        int best = 0;
        for (int t = 1; t < N; t++)
            if (D[static_cast<size_t>(t)] > D[static_cast<size_t>(best)]) best = t;
        d0 = best;
    }
    const double lead = 0.1 * N;
    const int s0 = static_cast<int>(std::floor(d0 - lead));
    const int pool = std::max(1, (N + 999) / 1000);
    kp::Plot pl;
    pl.title = "Echo profile";
    pl.x_unit = "\xC2\xB5s";
    pl.y_unit = "dB";
    pl.dx = pool * tap_us_;
    pl.x0 = (s0 - d0) * tap_us_;
    for (int i = 0; i < N; i += pool) {
        double mx = 0;
        for (int j = i; j < std::min(N, i + pool); j++) mx = std::max(mx, D[static_cast<size_t>(((s0 + j) % N + N) % N)]);
        pl.y.push_back(static_cast<float>(10 * std::log10(std::max(mx / floor, 1.0))));
    }
    pl.y_min = 0;
    pl.threshold = 10 * std::log10(std::max(thr / floor, 1.0));
    // the paths' delays from the strongest (taps, on the plot's circular
    // axis); one is marked when it was seen at about the same delay in one of
    // the two cycles before - estimator artefacts (images beside a path,
    // ghosts) don't stay put, and one antenna can't tell them by direction
    // (separation's tracks need 3 sightings)
    std::vector<double> rels;
    for (const auto& p : paths) rels.push_back(std::fmod(p.first - d0 + lead + 2.0 * N, static_cast<double>(N)) - lead);
    auto seen_before = [&](double r) {
        for (const auto& h : prof_hist_)
            for (double q : h)
                if (std::fabs(q - r) < 1.5) return true;
        return false;
    };
    double lo = 0, hi = 0;
    int shown = 0;
    for (size_t i = 0; i < paths.size(); i++) {
        const bool main = paths[i].first == d0;
        if (!main && !seen_before(rels[i])) continue;
        shown++;
        lo = std::min(lo, rels[i]);
        hi = std::max(hi, rels[i]);
        const double db = 10 * std::log10(std::max(paths[i].second / p0, 1e-9));
        pl.markers.push_back({rels[i] * tap_us_, main ? std::string("main") : fmt("%+.1f", rels[i] * tap_us_) + " \xC2\xB5s, " + fmt("%.0f", db) + " dB", ""});
    }
    prof_hist_.push_back(rels);
    if (prof_hist_.size() > 2) prof_hist_.erase(prof_hist_.begin());
    host_.plot(pl);
    host_.fact("Signal", what);
    host_.fact("Paths", shown == 0 ? std::string("none above the threshold")
                                   : std::to_string(shown) + (shown == 1 ? " path" : " paths") +
                                         (shown > 1 ? ", spread " + fmt("%.1f", (hi - lo) * tap_us_) + " \xC2\xB5s" : "") +
                                         " (path separation off: no bearings)");
    host_.fact("Delay window", fmt("%.1f us", N * tap_us_) + " (taps of " + fmt("%.2f us", tap_us_) + ")");
    host_.fact("CIR snapshots", std::to_string(snaps_total_));
}

}  // namespace sfn
