// SSR / Mode S / ACAS interrogations on 1030 MHz: the UPLINK that the 1090
// MHz replies (the adsb plugin) answer. Ground radars (SSR, Mode S), fixed
// interrogators (multilateration / WAM) and aircraft TCAS / ACAS interrogate
// here.
//
//   front.cpp    pulses at 2.4 MHz (raw power: 0.8 us pulses 2 us apart) and
//                the long Mode S P6
//   interr.cpp   pulses -> interrogations: Mode A / B / C / D (+ P2 SLS, P4
//                all-call, S1 whisper-shout), Mode S (P1 P2 P6, 56 / 112 bits)
//   dpsk.cpp     the Mode S chips: 4 Mchip/s DPSK seen through a ~2.2 MHz
//                receiver, read by a sequence estimator with the receiver's
//                step response (learned from frames whose parity checks)
//   uplink.cpp   UF formats, parity, the uplink address encoding, II / SI
//   track.cpp    which interrogator sent each one (ID separation): SSR trains,
//                fixed interrogators' bursts, ACAS whisper-shout; PRI / scan /
//                main beam (radar_pri / radar_scan from plugins/lib)
//
// Every interrogator is a talker: its own DoA from packets of exactly its
// interrogations' samples - for a rotating SSR only those of its main beam.

#include "kraken_plugin.hpp"
#include "dpsk.hpp"
#include "front.hpp"
#include "interr.hpp"
#include "track.hpp"
#include "uplink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace ssr;

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string hex6(uint32_t a) {
    char b[16];
    snprintf(b, sizeof b, "%06X", static_cast<unsigned>(a & 0xFFFFFF));
    return b;
}
float median(std::vector<float> v) {
    if (v.empty()) return NAN;
    auto m = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), m, v.end());
    return *m;
}

class SsrDecoder : public kp::Decoder {
public:
    explicit SsrDecoder(kp::Host& h) : Decoder(h) {
        fe_.set_threshold_db(sens_db_);
        host.table_columns({"Interrogator", "Type", "Interrogations", "PRF Hz", "PRI", "Scan s", "RPM", "Beam °", "SLS P2 dB",
                            "Mode S", "Targets", "Level dBFS", "SNR dB", "Per s", "Main beam"});
        host.table_keep(keep_s_);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "sens") {
            sens_db_ = std::clamp(v.empty() ? 10.0 : atof(v.c_str()), 6.0, 25.0);
            fe_.set_threshold_db(sens_db_);
        } else if (k == "doa") {
            doa_ = v != "0";
            trk_.set_doa(doa_, doa_db_);
        } else if (k == "doa_db") {
            doa_db_ = std::clamp(v.empty() ? 10.0 : atof(v.c_str()), 3.0, 30.0);
            trk_.set_doa(doa_, doa_db_);
        } else if (k == "doa_avg") {
            avg_s_ = std::clamp(v.empty() ? 30.0 : atof(v.c_str()), 3.0, 3600.0);
        } else if (k == "keep") {
            keep_s_ = std::clamp(v.empty() ? 3600.0 : atof(v.c_str()), 60.0, 7 * 86400.0);
            host.table_keep(keep_s_);
        } else if (k == "decode") {
            decode_ = v != "0";
        }
    }

    void reset() override {
        for (auto& s : trk_.sources()) host.table_remove(s->tid);
        fe_.reset();
        fe_.set_threshold_db(sens_db_);
        asm_.reset();
        trk_.reset();
        pending_.clear();
        ready_.clear();
        fast_t_ = slow_t_ = -1;
        rate_t_.clear();
        for (auto& c : kind_n_) c = 0;
        s_n_ = s_dec_ = s_ok_ = 0;
    }

    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        t_ref_ = t0 - static_cast<double>(fe_.count()) / FS;
        pulses_.clear();
        fe_.push(x, n, pulses_);
        if (pdump_)
            for (const Pulse& p : pulses_)
                host.log(fmt("PUL %.7f", t_ref_ + p.le / FS) + fmt(" w %.2f", p.width_us()) + fmt(" %.1f dB", p.db) +
                         (p.longp ? " LONG" : "") + (p.clipped ? " clip" : ""));
        its_.clear();
        asm_.push(pulses_, fe_.count(), its_);
        for (Interr& it : its_) {
            it.t = t_ref_ + it.p1 / FS;
            if (it.mode == MODE_S) pending_.push_back(it);
            else ready_.push_back(it);
        }
        // Mode S: decoded once the samples after P6 are in
        for (size_t i = 0; i < pending_.size();) {
            Interr& it = pending_[i];
            if (static_cast<double>(fe_.count()) < it.p1 + (3.5 + (it.nbits == 112 ? 30.25 : 16.25) + 5.0) / US) {
                i++;
                continue;
            }
            if (decode_) decode_s(it);
            ready_.push_back(it);
            pending_.erase(pending_.begin() + static_cast<long>(i));
        }
        // to the tracker in time order (Mode S come out ~40 us later than an A / C)
        const double now = t0 + static_cast<double>(n) / FS;
        std::sort(ready_.begin(), ready_.end(), [](const Interr& a, const Interr& b) { return a.t < b.t; });
        size_t k = 0;
        for (; k < ready_.size() && ready_[k].t < now - 2e-3; k++) feed(ready_[k]);
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<long>(k));
        if (fast_t_ < 0) fast_t_ = slow_t_ = start_t_ = t0;
        if (now - fast_t_ >= 0.05) {
            fast_t_ = now;
            Tracker::Out o;
            trk_.tick_fast(now, o);
            emit(o, now);
        }
        if (now - slow_t_ >= 0.5) {
            slow_t_ = now;
            Tracker::Out o;
            trk_.tick_slow(now, o);
            emit(o, now);
            outputs(now);
        }
    }

    ~SsrDecoder() override {
        if (!dump_) return;
        host.log(fmt("MODE S %.0f", s_n_) + fmt(" decoded %.0f", s_dec_) + fmt(" checked %.0f", s_ok_) +
                 fmt(" learned %.0f", dpsk_.learned()) + fmt(" refits %.0f", dpsk_.refits()));
        for (auto& s : trk_.sources()) {
            std::string t = "SRC " + s->tid + fmt(" n %.0f", static_cast<double>(s->n)) + fmt(" pkts %.0f", static_cast<double>(s->packets));
            if (s->pri > 0) t += fmt(" pri %.1f us", s->pri * 1e6);
            if (s->scan_s > 0) t += fmt(" scan %.3f", s->scan_s);
            if (s->burst_period > 0) t += fmt(" burst %.4f s", s->burst_period);
            host.log(t);
        }
        host.log(fmt("UNATTRIBUTED %.0f", static_cast<double>(trk_.unattributed())) + fmt(" of %.0f", static_cast<double>(trk_.total())));
        std::string sr = "RESPONSE";
        for (auto& v : dpsk_.response()) sr += fmt(" %.4f", v.real()) + fmt(",%.4f", v.imag());
        host.log(sr);
    }

private:
    FrontEnd fe_;
    Assembler asm_;
    Dpsk dpsk_;
    Tracker trk_;
    std::vector<Pulse> pulses_;
    std::vector<Interr> its_, pending_, ready_;
    std::vector<kp::cf> win_;
    double t_ref_ = 0, fast_t_ = -1, slow_t_ = -1, start_t_ = 0;
    double sens_db_ = 10, doa_db_ = 10, avg_s_ = 30, keep_s_ = 3600;
    bool doa_ = true, decode_ = true;
    const bool dump_ = getenv("SSR_DUMP") != nullptr;
    const bool pdump_ = getenv("SSR_PULSES") != nullptr;
    std::deque<std::pair<double, int>> rate_t_;   // (time, kind) of the last 2 s
    uint64_t kind_n_[14] = {};
    uint64_t s_n_ = 0, s_dec_ = 0, s_ok_ = 0;
    double pre_t_ = -1;

    void decode_s(Interr& it) {
        s_n_++;
        const int64_t a = static_cast<int64_t>(std::floor(it.p1)) - 5;
        const int n = static_cast<int>((3.5 + (it.nbits == 112 ? 30.25 : 16.25) + 4.5) / US) + 8;
        if (a + n > fe_.count() || a < fe_.count() - FrontEnd::RING + 16) return;
        win_.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) win_[static_cast<size_t>(i)] = fe_.at(a + i);
        const double p1 = it.p1 - static_cast<double>(a);
        dpsk_.decode(win_.data(), n, p1, it.nbits, it.dr);
        it.up.nbits = it.nbits;
        std::copy(it.dr.bits, it.dr.bits + it.nbits, it.up.b);
        parse_uplink(it.up);
        it.decoded = true;
        if (std::isnan(it.p2_db)) it.p2_db = static_cast<float>(it.dr.p2_db);
        s_dec_++;
        if (!it.clipped && !it.up.verified && it.p6_db >= 20 && it.t - pre_t_ >= 0.2) {
            pre_t_ = it.t;   // the known part of a strong one, 5 a second at most
            dpsk_.learn(win_.data(), n, p1, it.dr, false);
        }
        if (it.up.verified) {
            s_ok_++;
            if (!it.clipped) dpsk_.learn(win_.data(), n, p1, it.dr, true);
            const double ta = it.t - 1e-6, tb = it.t + (3.5 + it.nbits * 0.25 + 2.25 + 1) * 1e-6;
            host.valid(ta, tb);
            if (host.verbose()) host.event(it.up.kind() + fmt(" (parity checked) %.1f dB", it.db) + " · " + it.up.hex(), 0.0);
        }
        if (host.raw_wanted())
            host.raw(it.up.hex() + " UF" + std::to_string(it.up.uf) + " " + hex6(it.up.address) + (it.up.verified ? " checked" : "") +
                     fmt(" %.1f dB", it.db));
    }

    void feed(const Interr& it) {
        kind_n_[std::clamp(it.kind(), 0, 13)]++;
        rate_t_.push_back({it.t, it.kind()});
        const double len_us = it.mode == MODE_S ? 3.5 + (it.nbits == 112 ? 30.25 : 16.25) : (it.end - it.p1) * US + 0.8;
        // the DoA span: the interrogation +-10 us (>= 2 whole 16-sample covariance chunks)
        const double ta = it.t - 10e-6, tb = it.t + (len_us + 10) * 1e-6;
        Tracker::Out o;
        trk_.add(it, ta, tb, o);
        emit(o, it.t);
        if (dump_) {
            std::string s = fmt("IT %.7f ", it.t) + it.name() + fmt(" %.1f dB", it.db);
            if (std::isfinite(it.p2_db)) s += fmt(" P2 %+.1f", it.p2_db);
            if (std::isfinite(it.s1_db)) s += fmt(" S1 %+.1f", it.s1_db);
            if (it.clipped) s += " clip";
            if (it.decoded)
                s += " UF" + std::to_string(it.up.uf) + " " + hex6(it.up.address) + (it.up.verified ? " OK" : "") +
                     fmt(" df %.0f", it.dr.df) + fmt(" err %.3f", it.dr.rel_err) + " " + it.up.hex();
            for (auto& sp : trk_.sources())
                if (sp->last_t == it.t) s += " -> " + sp->tid;
            host.log(s);
        }
    }

    std::string label(const Source& S) const {
        std::string t = Tracker::type_name(S.type);
        if (S.type == SRC_SSR) {
            if (S.scan_s > 0) t += fmt(" · %.2f s scan", S.scan_s);
            if (S.pri > 0) t += fmt(" · PRF %.0f Hz", 1 / S.pri);
        } else if (S.type == SRC_FIXED) {
            t += S.burst_period > 0 ? fmt(" · bursts %.2f s", S.burst_period) : " · not rotating";
        } else if (!S.seqs.empty()) {
            t += fmt(" · %.0f-step whisper-shout", S.last_steps);
        } else {
            t += " · broadcasts";
        }
        return t.substr(0, 64);
    }

    void emit(Tracker::Out& o, double now) {
        (void)now;
        for (auto& [oldid, newid] : o.renamed) {
            host.table_remove(oldid);
            host.talker_forget(oldid);
        }
        for (const std::string& e : o.events) host.event(e, 5);
        for (auto& [S, r] : o.packets) {
            kp::Talker t;
            t.id = S->tid;
            t.label = label(*S);
            t.packet = true;
            t.start_s = r.t_a;
            t.end_s = r.t_b;
            t.avg_s = S->type == SRC_ACAS ? 5 : S->type == SRC_FIXED ? 10 : avg_s_;
            t.keep_s = S->type == SRC_ACAS ? std::min(keep_s_, 600.0) : keep_s_;
            host.talker(t);
            host.valid(r.t_a, r.t_b);
        }
    }

    // --- outputs ---------------------------------------------------------------
    static std::string interlace(const Source& S) {
        if (S.type == SRC_FIXED) {
            std::string t = fmt("bursts of %.0f", S.burst_n) + fmt(" (%.0f all-calls", S.burst_ac) + fmt(" + %.0f Mode S)", S.burst_s);
            if (S.grid_us > 0) t += fmt(", %.0f µs apart", S.grid_us);
            return t;
        }
        if (S.type == SRC_ACAS) {
            std::string t = S.seqs.empty() ? "" : fmt("whisper-shout %.0f steps", S.last_steps) + fmt(" %+.1f dB", S.last_step_db) +
                                                  fmt(" / %.1f ms", S.last_step_ms) + (std::isfinite(S.seq_s1) ? fmt(", S1 %+.1f dB", S.seq_s1) : "");
            if (S.bcasts) t += (t.empty() ? "" : ", ") + fmt("%.0f broadcasts", S.bcasts);
            return t;
        }
        // SSR: the Mode A / B / C / D sequence over its PRI slots ("?" = a
        // slot missed, "|" = the train lost in between) - its shortest cycle -
        // and how often a UF11 all-call opens a slot
        const std::vector<std::string> v(S.slots.begin(), S.slots.end());
        auto letter = [](const std::string& x) { return x.empty() ? '?' : x.back(); };
        std::string t;
        for (size_t c = 1; c <= 8 && 2 * c <= v.size(); c++) {
            int eq = 0, cmp = 0;
            for (size_t i = c; i < v.size(); i++) {
                bool brk = false;
                for (size_t j = i - c; j <= i; j++) brk = brk || v[j] == "|";
                if (brk || v[i] == "?" || v[i - c] == "?") continue;
                cmp++;
                eq += letter(v[i]) == letter(v[i - c]);
            }
            if (cmp < static_cast<int>(3 * c) || eq < 0.85 * cmp) continue;
            std::string cyc;
            for (size_t i = v.size(); i-- > 0 && cyc.size() < c;) {
                if (v[i] == "|") break;
                if (v[i] == "?") cyc.clear();
                else cyc.insert(cyc.begin(), letter(v[i]));
            }
            if (cyc.size() < c) continue;
            std::sort(cyc.begin(), cyc.end());   // a canonical start: A first
            if (c == 1) t = std::string(1, cyc[0]) + " only";
            else
                for (char ch : cyc) t += (t.empty() ? "" : " · ") + std::string(1, ch);
            break;
        }
        // UF11: the spacing (in slots) between the slots it opens
        std::map<int, int> gaps;
        int last = -1;
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == "|") last = -1;
            else if (v[i].rfind("S+", 0) == 0) {
                if (last >= 0) gaps[static_cast<int>(i) - last]++;
                last = static_cast<int>(i);
            }
        }
        int bk = 0, bv = 0, tot = 0;
        for (auto& [k, n] : gaps) {
            tot += n;
            if (n > bv) {
                bv = n;
                bk = k;
            }
        }
        if (t.empty()) {
            std::map<char, int> cnt;
            for (auto& x : v)
                if (x != "?" && x != "|") cnt[letter(x)]++;
            for (auto& [k, n] : cnt) t += (t.empty() ? "" : ", ") + std::string(1, k) + fmt(" %.0f", n);
            if (!t.empty()) t += " (no fixed order)";
        }
        if (bv >= 2 && bv >= 0.6 * tot) t += bk == 1 ? " + UF11 every period" : fmt(" + UF11 every %.0f periods", bk);
        else if (tot) t += " + UF11";
        int p4s = 0, p4l = 0, p4n = 0;
        for (auto& [k, n] : S.kinds) (k % 3 == 1 ? p4s : k % 3 == 2 ? p4l : p4n) += static_cast<int>(n);
        if (p4s > 2 * (p4l + p4n)) t += " (P4 short: A/C-only all-calls)";
        else if (p4l > 2 * (p4s + p4n)) t += " (P4 long: A/C/S all-calls)";
        return t;
    }
    static std::string ufs_text(const Source& S) {
        std::string t;
        for (auto& [uf, n] : S.ufs) t += (t.empty() ? "" : " · ") + ("UF" + std::to_string(uf)) + fmt(" %.0f", static_cast<double>(n));
        return t;
    }
    static int confirmed(const Source& S) {
        int c = 0;
        for (auto& [a, tg] : S.targets) c += tg.n >= 3;
        return c;
    }
    double noise_dbfs() const { return fe_.noise() > 0 ? 10 * std::log10(fe_.noise()) : NAN; }

    void row(Source& S, double now) {
        const double rate = static_cast<double>(S.rate_t.size()) / std::clamp(now - S.first_t, 0.5, 2.0);
        const double nf = noise_dbfs();
        const double beam_deg = [&] {
            std::vector<float> w;
            for (double v : S.beam_w3)
                if (std::isfinite(v)) w.push_back(static_cast<float>(v));
            return S.scan_s > 0 && !w.empty() ? median(w) / S.scan_s * 360 : 0.0;
        }();
        const float p2m = median(std::vector<float>(S.p2_main.begin(), S.p2_main.end()));
        std::string pri;
        if (S.type == SRC_SSR) pri = S.pri_info.ok ? S.pri_info.text() : fmt("%.0f µs", S.pri * 1e6);
        else if (S.type == SRC_FIXED && S.burst_period > 0) pri = fmt("bursts every %.2f s", S.burst_period) + (S.burst_regular ? "" : " (irregular)");
        else if (S.seqs.size() >= 2) pri = fmt("sequences every %.2f s", (S.seqs.back() - S.seqs.front()) / std::max<double>(1, std::lround((S.seqs.back() - S.seqs.front()))));
        const std::string mb = S.type == SRC_SSR && !S.beams.empty() ? fmt("%.1f s ago", now - S.beams.back()) : "";
        host.table_row(S.tid, {S.tid, Tracker::type_name(S.type), interlace(S), S.type == SRC_SSR && S.pri > 0 ? fmt("%.1f", 1 / S.pri) : "",
                               pri, S.scan_s > 0 ? fmt("%.2f", S.scan_s) : "", S.scan_s > 0 ? fmt("%.1f", 60 / S.scan_s) : "",
                               beam_deg > 0 ? fmt("%.1f", beam_deg) : "", std::isfinite(p2m) ? fmt("%+.0f", p2m) : "", ufs_text(S),
                               confirmed(S) ? fmt("%.0f", confirmed(S)) : "",
                               std::isfinite(nf) ? fmt("%.1f", S.peak_db + nf) + (S.clipped ? " (clipped)" : "") : "", fmt("%.0f", S.peak_db),
                               fmt("%.0f", rate), mb});
        std::vector<std::pair<std::string, std::string>> d;
        if (S.type == SRC_SSR)
            d.push_back({"Type", "SSR - a rotating secondary surveillance radar (periodic all-call train, main beam once per turn)"});
        else if (S.type == SRC_FIXED)
            d.push_back({"Type", "fixed interrogator - not rotating, bursts at a fixed period (multilateration / WAM, test transponder)"});
        else
            d.push_back({"Type", "ACAS / TCAS on an aircraft - whisper-shout sequences of Mode C-only all-calls with S1 pulses"});
        if (S.ii >= 0) d.push_back({"Interrogator code", "II " + std::to_string(S.ii) + (S.ii_checked ? " (UF11 all-calls, parity checked)" : " (IIS of its roll-calls)")});
        if (S.si > 0) d.push_back({"Interrogator code", "SI " + std::to_string(S.si) + (S.ii_checked ? " (UF11 all-calls, parity checked)" : " (SIS of its roll-calls)")});
        if (S.acas_addr) d.push_back({"Aircraft", hex6(S.acas_addr) + " (its own address, from UF16)"});
        if (S.type == SRC_SSR) {
            if (S.pri > 0) d.push_back({"PRF", fmt("%.1f Hz", 1 / S.pri) + fmt(" (mean PRI %.1f µs)", S.pri * 1e6)});
            if (S.pri_info.ok) d.push_back({"PRI", S.pri_info.text() + fmt(" (%.0f %% of the intervals explained)", 100 * S.pri_info.coverage)});
            const std::string il = interlace(S);
            if (!il.empty()) d.push_back({"Interlace", il});
            if (S.scan_s > 0) d.push_back({"Antenna scan", fmt("%.3f s", S.scan_s) + fmt(" = %.2f rpm", 60 / S.scan_s) + fmt(" (from %.0f main-beam passes)", S.scan_support)});
            else d.push_back({"Antenna scan", fmt("measuring (%.0f main-beam passes so far)", static_cast<double>(S.beams.size()))});
            if (beam_deg > 0) d.push_back({"Main beam", fmt("%.2f° at -3 dB", beam_deg)});
            if (std::isfinite(p2m))
                d.push_back({"SLS (P2)", fmt("%+.1f dB against P1 in the main beam", p2m) +
                                             (S.p2_side.empty() ? "" : fmt(", %+.1f dB elsewhere", median(std::vector<float>(S.p2_side.begin(), S.p2_side.end()))))});
        } else if (S.type == SRC_FIXED) {
            d.push_back({"Bursts", (S.burst_period > 0 ? fmt("every %.3f s", S.burst_period) + (S.burst_regular ? "" : " (median, irregular)") + ", " : "") +
                                       interlace(S)});
            d.push_back({"Level", fmt("steady at %.1f dB over the noise (it doesn't rotate)", S.burst_lvl)});
        } else {
            d.push_back({"Whisper-shout", interlace(S)});
            if (S.last_span_db != 0) d.push_back({"Power steps", fmt("%.1f dB over the sequence", S.last_span_db)});
        }
        std::string kinds;
        static const char* KN[] = {"A", "A+P4s", "A+P4L", "B", "B+P4s", "B+P4L", "C", "C+P4s", "C+P4L", "D", "D+P4s", "D+P4L", "S56", "S112"};
        for (auto& [k, n] : S.kinds) kinds += (kinds.empty() ? "" : ", ") + std::string(KN[k]) + fmt(" %.0f", static_cast<double>(n));
        if (!kinds.empty()) d.push_back({"Mode A / C", kinds + " (P4s = A/C-only all-call, P4L = A/C/S all-call)"});
        if (!S.ufs.empty()) d.push_back({"Mode S", ufs_text(S)});
        // targets: addresses seen 3+ times (one-off ones are mostly bit errors)
        std::vector<std::pair<int, uint32_t>> tg;
        for (auto& [a, t] : S.targets)
            if (t.n >= 3) tg.push_back({t.n, a});
        std::sort(tg.rbegin(), tg.rend());
        std::string tl;
        for (size_t i = 0; i < tg.size() && i < 20; i++) tl += (tl.empty() ? "" : ", ") + hex6(tg[i].second) + fmt(" ×%.0f", tg[i].first);
        if (!tl.empty()) d.push_back({"Targets", tl + " (addresses it interrogates - from the parity, not checkable)"});
        d.push_back({"Level", fmt("%.1f dB over the noise", S.peak_db) + (S.clipped ? " - clipped" : "")});
        d.push_back({"DoA", doa_ ? fmt("%.0f interrogations reported", static_cast<double>(S.packets)) +
                                       (S.type == SRC_SSR ? fmt(" (its main beam: within %.0f dB of the scan's peak, unclipped)", doa_db_) : " (unclipped)")
                                 : "off"});
        d.push_back({"Heard", fmt("%.0f interrogations", static_cast<double>(S.n)) + fmt(", for %.0f s", now - S.first_t)});
        host.table_detail(S.tid, d);
    }

    void outputs(double now) {
        while (!rate_t_.empty() && rate_t_.front().first < now - 2) rate_t_.pop_front();
        const double span = std::clamp(now - start_t_, 0.5, 2.0);
        int ac = 0, s = 0;
        for (auto& [t, k] : rate_t_) (k >= 12 ? s : ac)++;
        host.fact("Interrogations", fmt("%.0f /s", rate_t_.size() / span) + fmt(" (Mode A / C %.0f", ac / span) + fmt(", Mode S %.0f)", s / span));
        if (fe_.noise() > 0) host.fact("Noise floor", fmt("%.1f dBFS", noise_dbfs()));
        if (s_n_)
            host.fact("Mode S decoded", fmt("%.0f", static_cast<double>(s_dec_)) + fmt(", %.0f parity-checked all-calls / broadcasts", static_cast<double>(s_ok_)) +
                                            fmt(" (receiver model re-fitted %.0f×)", dpsk_.refits()));
        int n = 0;
        std::string list;
        Source* top = nullptr;
        for (auto& sp : trk_.sources()) {
            Source& S = *sp;
            if (!S.announced || now - S.last_t > keep_s_) continue;
            n++;
            list += (list.empty() ? "" : ", ") + S.tid;
            row(S, now);
            if (S.type == SRC_SSR && (!top || S.peak_db > top->peak_db)) top = &S;
        }
        host.fact("Interrogators", n ? std::to_string(n) + " (" + list + ")" : "none yet");
        if (top) {
            host.fact("Strongest SSR", top->tid + (top->scan_s > 0 ? fmt(", scan %.2f s", top->scan_s) : "") +
                                           (top->pri > 0 ? fmt(", PRF %.1f Hz", 1 / top->pri) : ""));
            plot(*top, now);
        }
    }

    // the strongest SSR's level over one turn of its antenna (as the radar plugin)
    void plot(Source& S, double now) {
        if (now - plot_t_ < 2) return;
        plot_t_ = now;
        std::vector<std::pair<double, float>> b;
        kp::Plot p;
        p.y_unit = "dB";
        const float ref = S.lv.max_over(now - 15, now);
        p.threshold = ref - doa_db_;
        if (S.scan_s > 0 && !S.beams.empty()) {
            const double P = S.scan_s, tb = S.beams.back();
            S.lv.bins(now - 2 * P, now, b);
            p.title = S.tid + ": interrogation level over one turn (0° = main beam here)";
            p.x_unit = "°";
            p.x0 = -180;
            p.dx = 2;
            p.y.assign(180, NAN);
            for (auto& [t, v] : b) {
                const double az = std::remainder((t - tb) / P * 360, 360);
                const int i = std::clamp(static_cast<int>((az + 180) / 2), 0, 179);
                if (!(p.y[static_cast<size_t>(i)] >= v)) p.y[static_cast<size_t>(i)] = v;
            }
            p.markers.push_back({0, "main beam", S.tid});
        } else {
            S.lv.bins(now - 20, now, b);
            p.title = S.tid + ": interrogation level, last 20 s";
            p.x_unit = "s";
            p.x0 = -20;
            p.dx = 0.1;
            p.y.assign(200, NAN);
            for (auto& [t, v] : b) {
                const int i = std::clamp(static_cast<int>((t - (now - 20)) / 0.1), 0, 199);
                if (!(p.y[static_cast<size_t>(i)] >= v)) p.y[static_cast<size_t>(i)] = v;
            }
        }
        float lo = 1e9f;
        for (float v : p.y)
            if (std::isfinite(v)) lo = std::min(lo, v);
        if (lo > 1e8f) return;
        for (float& v : p.y)
            if (!std::isfinite(v)) v = std::max(0.0f, lo - 3);
        p.y_min = 0;
        host.plot(p);
    }
    double plot_t_ = -1e9;
};

}  // namespace

KRAKEN_PLUGIN(SsrDecoder,
              {.id = "ssr1030",
               .name = "SSR / Mode S / TCAS interrogations (1030 MHz)",
               .category = "Aviation",
               .description = "The 1030 MHz uplink: interrogations of secondary radars (SSR, Mode S), fixed "
                              "multilateration interrogators and aircraft TCAS / ACAS - Mode A / C with SLS and "
                              "all-call P4, whisper-shout, Mode S UF formats read from the DPSK (target addresses, "
                              "interrogator codes II / SI, ACAS addresses). Per interrogator: PRF / PRI, interlace, "
                              "antenna scan, main beam - and its own DoA in the 🗺 Map's 📡 DF panel",
               .version = "1.0",
               .sample_rate = 2400000,
               .min_vfo_rate = 2400000,
               .author = "KrakenSDR",
               .options = {{"sens", "Pulse threshold", "10", "8=8 dB|10=10 dB|12=12 dB|15=15 dB",
                            "How far above the noise a pulse must rise. Lower finds weaker interrogators"},
                           {"decode", "Mode S decoding", "1", "1=On|0=Off",
                            "Read the Mode S interrogations' 56 / 112 bits (UF, addresses, interrogator codes) - "
                            "a sequence estimator per interrogation, a few % of a core"},
                           {"doa", "DoA per interrogator", "1", "1=On|0=Off",
                            "Report each interrogator's interrogations for its own bearing on the 🗺 Map (DF panel)"},
                           {"doa_db", "SSR DoA from", "10", "6=Main beam top (6 dB)|10=Main beam (10 dB)|15=Main beam (15 dB)",
                            "A rotating SSR's interrogations within this much of its scan's strongest are used for its "
                            "bearing (unclipped only): its main beam comes straight from it"},
                           {"doa_avg", "SSR DoA averaging", "30", "10=10 s|30=30 s|120=2 min|600=10 min",
                            "How long an SSR's main-beam interrogations are averaged into its bearing (one pass per turn)"},
                           {"keep", "Keep interrogators", "3600", "600=10 min|3600=1 hour|86400=24 hours",
                            "How long an interrogator stays in the table and the DF list after it was last heard"}},
               .manual_only = true,
               .fixed_freq_hz = 1030e6,
               .talkers = true})
