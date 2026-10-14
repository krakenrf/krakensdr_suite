#include "track.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ssr {

namespace {

constexpr double PEND_S = 4.0;            // unattributed interrogations kept for discovery
constexpr double PRI_MIN = 1.5e-3, PRI_MAX = 25e-3;
constexpr int CHAIN_MIN = 10;             // all-calls in a chain before it is an SSR
constexpr double JIT = 0.035;             // PRI jitter allowed (SSRs jitter their PRF a few %)
constexpr double BURST_GAP = 0.025;       // a fixed interrogator's burst: gaps up to this
constexpr double SEQ_GAP = 0.060;         // ACAS whisper-shout: steps up to this apart
constexpr double DOA_HOLD_S = 0.3;
constexpr double DOA_RATE = 60;           // packets per second per interrogator
constexpr float DOA_MIN_DB = 12;

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
const char* kind_short(int k) {
    static const char* N[] = {"A", "A", "A", "B", "B", "B", "C", "C", "C", "D", "D", "D", "S", "S"};
    return k >= 0 && k < 14 ? N[k] : "?";
}

}  // namespace

void Tracker::reset() {
    src_.clear();
    pend_.clear();
    seqs_.clear();
    orphan_bursts_.clear();
    serial_ = 0;
    unattr_ = total_ = 0;
}

Source* Tracker::make(int type, double t, Out& out) {
    auto s = std::make_unique<Source>();
    s->type = type;
    s->serial = ++serial_;
    s->first_t = s->last_t = t;
    s->tid = type_name(type) + " #" + std::to_string(s->serial);
    src_.push_back(std::move(s));
    (void)out;
    return src_.back().get();
}

// the talker id: by the code / address it gives itself once known
void Tracker::name(Source& S, Out& out) {
    std::string id;
    if (S.type == SRC_ACAS && S.acas_addr) id = "ACAS " + hex6(S.acas_addr);
    else if (S.type != SRC_ACAS && S.ii >= 0 && (S.ii_checked || S.type == SRC_SSR)) id = type_name(S.type) + " II " + std::to_string(S.ii);
    else if (S.type != SRC_ACAS && S.si > 0 && (S.ii_checked || S.type == SRC_SSR)) id = type_name(S.type) + " SI " + std::to_string(S.si);
    if (id.empty() || id == S.tid) return;
    // another interrogator with that name already (two FIXED with II 0 ...): keep the number
    for (auto& o : src_)
        if (o.get() != &S && o->tid == id) id += " #" + std::to_string(S.serial);
    if (id == S.tid) return;
    out.renamed.push_back({S.tid, id});
    S.tid = id;
}

void Tracker::attach(Source& S, const Rec& r, Out& out) {
    S.n++;
    S.last_t = std::max(S.last_t, r.t);
    S.rate_t.push_back(r.t);
    if (r.uf >= 0) {
        S.ufs[r.uf]++;
        if (!r.verified && r.addr != ALL_CALL && r.uf != 11) {
            Target& tg = S.targets[r.addr];
            tg.n++;
            tg.last_t = r.t;
            tg.ufs[r.uf]++;
        }
    } else {
        S.kinds[r.kind]++;
    }
    S.peak_db = std::max(S.peak_db, r.db);
    if (r.clipped) S.clipped = true;
    S.lv.add(r.t, r.db, r.clipped);
    if (S.lost) {
        S.lost = false;
        if (S.announced) out.events.push_back("🛰 " + S.tid + " heard again");
    }
    if (S.type == SRC_SSR && r.uf == 11) {
        S.s11_t.push_back(r.t);
        while (S.s11_t.size() > 64) S.s11_t.pop_front();
    }
    // what it says about its interrogator: a checked all-call's code names the
    // train; roll-calls' IIS / SIS (not checkable) are votes; ACAS: UF16's
    // sender address (MID)
    if (S.type == SRC_SSR && r.uf == 11 && r.verified && !S.ii_checked && (r.ii >= 0 || r.si > 0)) {
        S.ii = r.ii;
        S.si = r.si;
        S.ii_checked = true;
        out.events.push_back("🛰 " + S.tid + ": interrogator code " + (S.ii >= 0 ? "II " + std::to_string(S.ii) : "SI " + std::to_string(S.si)) +
                             " (its Mode S-only all-calls, parity checked)");
        name(S, out);
    } else if (r.uf >= 0 && r.uf != 11 && (r.ii >= 0 || r.si > 0) && S.type != SRC_ACAS) {
        if (r.ii >= 0) S.ii_votes[r.ii]++;
        if (r.si > 0) S.si_votes[r.si]++;
    }
    if (S.type == SRC_ACAS && r.uf == 16 && r.acas && S.acas_addr != r.acas && (r.verified || ++S.acas_votes[r.acas] >= 2)) {
        S.acas_addr = r.acas;
        out.events.push_back("✈ " + S.tid + " is the ACAS of aircraft " + hex6(S.acas_addr) +
                             (r.verified ? " (UF16 broadcast, parity checked)" : " (UF16 coordination messages)"));
        name(S, out);
    }
    if (doa_ && !r.clipped && r.db >= DOA_MIN_DB) {
        S.cand.push_back(r);
        if (S.cand.size() > 3000) S.cand.pop_front();
    }
}

// --- SSR: a periodic all-call train ------------------------------------------
bool Tracker::try_ssr(const Rec& r, Out& out) {
    if (!r.allcall() || std::isfinite(r.s1)) return false;
    Source* best = nullptr;
    double be = 1e9;
    int bn = 0;
    for (auto& sp : src_) {
        Source& S = *sp;
        if (S.type != SRC_SSR || S.pri <= 0) continue;
        const double d = r.t - S.t_last;
        // a UF11 all-call rides in a period of the train (here ~120 us before its A / C)
        if (r.uf == 11 && std::fabs(d) < 0.6e-3 && std::fabs(r.db - S.lvl_last) <= 8) {
            if (std::fabs(d) < be) {
                be = std::fabs(d);
                best = &S;
                bn = 0;
            }
            continue;
        }
        if (r.uf == 11) {
            // ... or ahead of the next A / C
            const double d2 = d - S.pri;
            if (std::fabs(d2) < 0.6e-3 + JIT * S.pri && std::fabs(r.db - S.lvl_last) <= 8 && std::fabs(d2) < be) {
                be = std::fabs(d2);
                best = &S;
                bn = 0;
            }
            continue;
        }
        const int n = static_cast<int>(std::lround(d / S.pri));
        if (n < 1 || n > 5) continue;
        const double e = std::fabs(d - n * S.pri);
        // its own PRI spread sets the window (4x the mean deviation, 20 us ..
        // JIT), growing like a random walk over missed slots
        const double tol = std::clamp(4 * S.jit_e, 20e-6, JIT * S.pri);
        if (e > std::sqrt(static_cast<double>(n)) * tol + 5e-6) continue;
        if (std::fabs(r.db - S.lvl_last) > (n == 1 ? 12 : 9)) continue;
        const double sc = e / (n * S.pri) + 0.01 * n;
        if (sc < be) {
            be = sc;
            best = &S;
            bn = n;
        }
    }
    if (!best) return false;
    Source& S = *best;
    if (bn > 0) {
        S.jit_e = 0.95 * S.jit_e + 0.05 * (std::fabs(r.t - S.t_last - bn * S.pri) / bn);
        S.pri = 0.95 * S.pri + 0.05 * ((r.t - S.t_last) / bn);
        S.t_last = r.t;
        S.lvl_last = r.db;
        S.chain.push_back(r.t);
        if (!S.slot_open.empty()) S.slots.push_back(S.slot_open);
        for (int m = 1; m < bn; m++) S.slots.push_back("?");
        while (S.slots.size() > 60) S.slots.pop_front();
        // a UF11 all-call just before it (the same PRI slot)
        bool s11 = false;
        for (double u : S.s11_t)
            if (r.t - u >= 0 && r.t - u < 0.6e-3) s11 = true;
        S.slot_open = (s11 ? "S+" : "") + std::string(kind_short(r.kind));
        S.slot_t = r.t;
    }
    if (std::isfinite(r.p2) && r.uf < 0 && r.kind < 12) {
        // the control antenna is omni: P2 arrives at one level all round
        S.p2abs.push_back(r.db + r.p2);
        if (S.p2abs.size() > 200) S.p2abs.pop_front();
        // main beam: within 6 dB of the train's strongest of the last 15 s
        const float ref = S.lv.max_over(r.t - 15, r.t);
        auto& v = r.db >= ref - 6 ? S.p2_main : S.p2_side;
        v.push_back(r.p2 - 0);
        if (v.size() > 200) v.pop_front();
    }
    attach(S, r, out);
    return true;
}

// --- an SSR's sidelobes: P2 (SLS) as strong as P1 or stronger -----------------
// Its P2 comes from the omni control antenna, so it has the same level all
// round: that level names the SSR (no timing needed - between its main
// beams the train is lost)
bool Tracker::try_sidelobe(const Rec& r, Out& out) {
    if (r.uf >= 0 || r.kind >= 12 || !std::isfinite(r.p2) || r.p2 < 2) return false;
    Source* best = nullptr;
    float be = 3.1f;
    for (auto& sp : src_) {
        Source& S = *sp;
        if (S.type != SRC_SSR || S.p2abs.size() < 5) continue;
        const float m = median(std::vector<float>(S.p2abs.begin(), S.p2abs.end()));
        const float d = std::fabs(r.db + r.p2 - m);
        if (d < be) {
            be = d;
            best = &S;
        }
    }
    if (!best) return false;
    best->p2_side.push_back(r.p2);
    if (best->p2_side.size() > 200) best->p2_side.pop_front();
    attach(*best, r, out);
    return true;
}

// --- FIXED: a non-rotating ground interrogator --------------------------------
// Not rotating, so it arrives at ONE level (+-3 dB): everything at its
// level that isn't an SSR's (P2 as strong as P1) or an ACAS's (S1) is its.
// Its bursts (gaps <= BURST_GAP) are counted for the table; their timing
// (here 1 s apart, live it varied 0.7-3 s with the traffic) is not needed
// what a fixed interrogator may have sent: not an ACAS step (S1) or
// broadcast (UF16), not an SSR's all-call (UF11) or sidelobe interrogation
// (P2 as strong as P1), not Mode B / D (out of use - near the threshold
// those are pulses of different interrogations put together)
static bool fixed_ok(const Rec& r) {
    const int mode = r.kind < 12 ? r.kind / 3 : -1;
    return !std::isfinite(r.s1) && !(r.uf < 0 && r.p2 >= -6) && r.uf != 16 && r.uf != 11 && mode != MODE_B && mode != MODE_D;
}

bool Tracker::try_fixed(const Rec& r, Out& out) {
    if (!fixed_ok(r)) return false;
    Source* best = nullptr;
    float be = 3.01f;
    for (auto& sp : src_) {
        Source& S = *sp;
        if (S.type != SRC_FIXED || r.t - S.last_t > 30) continue;
        const float d = std::fabs(r.db - S.burst_lvl);
        if (d < be) {
            be = d;
            best = &S;
        }
    }
    if (!best) return false;
    fixed_add(*best, r);
    attach(*best, r, out);
    return true;
}

void Tracker::fixed_add(Source& S, const Rec& r) {
    if (S.burst_last < 0 || r.t - S.burst_last > BURST_GAP) {
        // a new burst: the previous one's make-up and its spacing
        if (S.cur_n) {
            S.burst_ac = S.cur_ac;
            S.burst_s = S.cur_s;
            S.burst_n = S.cur_n;
        }
        S.bursts.push_back(r.t);
        while (S.bursts.size() > 64) S.bursts.pop_front();
        S.burst_start = r.t;
        S.cur_n = S.cur_ac = S.cur_s = 0;
        if (S.bursts.size() >= 3) {
            std::vector<float> d;
            for (size_t i = 1; i < S.bursts.size(); i++) d.push_back(static_cast<float>(S.bursts[i] - S.bursts[i - 1]));
            S.burst_period = median(d);
            float dev = 0;
            for (float v : d) dev += std::fabs(v - static_cast<float>(S.burst_period));
            S.burst_regular = dev / static_cast<float>(d.size()) < 0.03f * static_cast<float>(S.burst_period);
        }
    } else if (S.grid_us <= 0 || (r.t - S.burst_last) * 1e6 < S.grid_us - 5) {
        S.grid_us = (r.t - S.burst_last) * 1e6;   // the smallest spacing inside a burst
    }
    S.burst_last = r.t;
    S.cur_n++;
    (r.uf >= 0 && r.uf != 11 ? S.cur_s : S.cur_ac)++;
    S.burst_lvl = 0.98f * S.burst_lvl + 0.02f * r.db;
}

// --- ACAS whisper-shout -------------------------------------------------------
bool Tracker::try_acas(const Rec& r, Out& out) {
    (void)out;
    // a step: a Mode C (only) all-call with its S1 pulse, or the next step of an open sequence
    const bool c_mode = r.kind >= 6 && r.kind <= 8 && r.uf < 0;
    if (!c_mode) return false;
    for (Seq& q : seqs_) {
        if (r.t - q.last <= SEQ_GAP && r.t > q.last && r.db >= q.lvl - 4 && std::isfinite(r.s1)) {
            q.last = r.t;
            q.lvl = r.db;
            q.steps++;
            if (std::isfinite(r.s1)) q.s1 += r.s1;
            q.recs.push_back(r);
            return true;
        }
    }
    if (!std::isfinite(r.s1)) return false;
    Seq q;
    q.start = q.last = r.t;
    q.lvl0 = q.lvl = r.db;
    q.steps = 1;
    q.s1 = r.s1;
    q.recs.push_back(r);
    seqs_.push_back(q);
    return true;
}

void Tracker::close_seq(Seq& q, Out& out) {
    int s1n = 0;
    for (const Rec& r : q.recs) s1n += std::isfinite(r.s1);
    if (q.steps < 3 || s1n * 2 < q.steps) {
        for (const Rec& r : q.recs) pend_.push_back(r);
        return;
    }
    // the same aircraft: about a second after one of its sequences, similar level
    Source* best = nullptr;
    double be = 1e9;
    for (auto& sp : src_) {
        Source& S = *sp;
        if (S.type != SRC_ACAS) continue;
        if (S.seqs.empty()) {
            // known from its broadcasts only: a sequence around one of them, at its level
            if (S.bc_t >= 0 && std::fabs(q.start - S.bc_t) <= 1.5 && std::fabs(q.lvl - S.seq_lvl) <= 8 && 0.5 < be) {
                be = 0.5;
                best = &S;
            }
            continue;
        }
        const double d = q.start - S.seq_start;
        const int k = static_cast<int>(std::lround(d));
        if (k < 1 || k > 6) continue;
        const double e = std::fabs(d - k) / k;
        if (e > 0.12) continue;
        const double sc = e + 0.02 * std::fabs(q.lvl - S.seq_lvl);
        if (std::fabs(q.lvl - S.seq_lvl) > 8) continue;
        if (sc < be) {
            be = sc;
            best = &S;
        }
    }
    Source& S = best ? *best : *make(SRC_ACAS, q.start, out);
    S.seq_start = q.start;
    S.seq_lvl = q.lvl;
    S.seqs.push_back(q.start);
    if (S.seqs.size() > 30) S.seqs.pop_front();
    S.last_steps = q.steps;
    S.last_span_db = q.lvl - q.lvl0;
    S.last_step_db = q.steps > 1 ? (q.lvl - q.lvl0) / (q.steps - 1) : 0;
    S.last_step_ms = q.steps > 1 ? (q.last - q.start) / (q.steps - 1) * 1e3 : 0;
    S.seq_s1 = s1n ? q.s1 / s1n : NAN;
    for (const Rec& r : q.recs) attach(S, r, out);
}

// --- roll-calls --------------------------------------------------------------
bool Tracker::try_rollcall(const Rec& r, Out& out) {
    (void)out;
    if (r.uf < 0 || r.uf == 11) return false;
    Source* best = nullptr;
    double be = 1e9;
    for (auto& sp : src_) {
        Source& S = *sp;
        double sc = 1e9;
        if (S.type == SRC_FIXED && r.uf != 16 && r.t - S.last_t <= 30 && std::fabs(r.db - S.burst_lvl) <= 3) {
            sc = std::fabs(r.db - S.burst_lvl);
        } else if (S.type == SRC_SSR && S.pri > 0 && r.t - S.t_last <= 4 * S.pri && r.t - S.t_last >= -S.pri &&
                   std::fabs(r.db - S.lvl_last) <= 8) {
            sc = 1 + std::fabs(r.db - S.lvl_last);
        } else if (S.type == SRC_ACAS && (r.uf == 0 || r.uf == 16) && r.t - std::max(S.seq_start, S.bc_t) <= 1.5 &&
                   std::fabs(r.db - S.seq_lvl) <= 6 && !(r.uf == 16 && r.verified && r.acas && S.acas_addr && S.acas_addr != r.acas)) {
            sc = std::fabs(r.db - S.seq_lvl) - 1;   // UF0 / UF16 come from an ACAS first
        }
        if (sc < be) {
            be = sc;
            best = &S;
        }
    }
    if (!best) return false;
    if (best->type == SRC_FIXED) fixed_add(*best, r);
    attach(*best, r, out);
    return true;
}

void Tracker::add(const Interr& it, double t_a, double t_b, Out& out) {
    total_++;
    Rec r;
    r.t = it.t;
    r.t_a = t_a;
    r.t_b = t_b;
    r.kind = it.kind();
    r.db = it.db;
    r.p2 = it.p2_db;
    r.s1 = it.s1_db;
    r.clipped = it.clipped;
    if (it.decoded) {
        r.uf = it.up.uf;
        r.addr = it.up.address;
        r.verified = it.up.verified;
        r.ii = it.up.ii;
        r.si = it.up.si;
        r.acas = it.up.acas_sender;
    }
    // what it says about its interrogator: a checked all-call's code, or a
    // roll-call's IIS / SIS equal to the code an SSR's checked all-calls gave
    // (roll-calls aren't checkable, but a bit error landing exactly on that
    // code is rare) - an SSR's roll-calls come between its all-calls, when
    // its train may not be picked up yet
    Source* by_code = nullptr;
    const bool rollcall = it.decoded && !it.up.verified && (it.up.uf == 4 || it.up.uf == 5 || it.up.uf == 20 || it.up.uf == 21);
    if (it.decoded && ((it.up.verified && it.up.uf == 11) || rollcall) && (it.up.ii >= 0 || it.up.si > 0)) {
        for (auto& sp : src_)
            if (sp->type == SRC_SSR && (!rollcall || sp->ii_checked) &&
                ((it.up.ii >= 0 && sp->ii == it.up.ii) || (it.up.si > 0 && sp->si == it.up.si)))
                by_code = sp.get();
    }
    bool done = false;
    if (by_code) {
        attach(*by_code, r, out);
        done = true;
    }
    if (!done && r.uf == 16 && r.verified && r.acas) {
        for (auto& sp : src_)
            if (sp->type == SRC_ACAS && sp->acas_addr == r.acas) {
                sp->bc_t = r.t;
                if (sp->seqs.empty()) sp->seq_lvl = r.db;
                sp->bcasts++;
                attach(*sp, r, out);
                done = true;
                break;
            }
        if (!done) done = try_rollcall(r, out);
        if (!done) {
            Source* S = make(SRC_ACAS, r.t, out);
            S->bc_t = r.t;
            S->seq_lvl = r.db;
            S->bcasts = 1;
            attach(*S, r, out);
            done = true;
        }
    }
    if (!done) done = try_acas(r, out);
    if (!done) done = try_ssr(r, out);
    if (!done) done = try_sidelobe(r, out);
    if (!done && r.uf >= 0) done = try_rollcall(r, out);   // Mode S: by its interrogator's timing / level
    if (!done) done = try_fixed(r, out);
    if (!done) pend_.push_back(r);
}

// --- discovery ----------------------------------------------------------------
// Sequence search: the PRI that the most all-call pairs share, then chains of
// all-calls at multiples of it (+-JIT) through the unattributed ones
void Tracker::find_trains(double now, Out& out) {
    std::vector<size_t> idx;
    for (size_t i = 0; i < pend_.size(); i++)
        if (pend_[i].allcall() && pend_[i].uf != 11 && !std::isfinite(pend_[i].s1) && pend_[i].t > now - 3.0) idx.push_back(i);
    if (idx.size() < static_cast<size_t>(CHAIN_MIN)) return;
    constexpr double BIN = 20e-6;
    const int NB = static_cast<int>(PRI_MAX / BIN) + 2;
    std::vector<int> h(static_cast<size_t>(NB), 0);
    for (size_t a = 0; a < idx.size(); a++)
        for (size_t b = a + 1; b < idx.size() && b <= a + 4; b++) {
            const Rec &x = pend_[idx[a]], &y = pend_[idx[b]];
            const double d = y.t - x.t;
            if (d < PRI_MIN || d > PRI_MAX || std::fabs(x.db - y.db) > 8) continue;
            h[static_cast<size_t>(d / BIN)]++;
        }
    // the histogram smoothed over +-2 % of the PRI; its local maxima, the
    // shortest PRIs first (a train's 2nd / 3rd neighbours make peaks at 2 / 3 PRI)
    std::vector<int> sm(static_cast<size_t>(NB), 0);
    for (int i = 0; i < NB; i++) {
        const int w = std::max(2, static_cast<int>(0.02 * (i + 0.5)));
        for (int j = std::max(0, i - w); j <= std::min(NB - 1, i + w); j++) sm[static_cast<size_t>(i)] += h[static_cast<size_t>(j)];
    }
    std::vector<std::pair<int, double>> peaks;
    for (int i = 1; i < NB - 1; i++) {
        const int v = sm[static_cast<size_t>(i)];
        if (v < CHAIN_MIN - 2 || v < sm[static_cast<size_t>(i - 1)] || v <= sm[static_cast<size_t>(i + 1)]) continue;
        peaks.push_back({v, (i + 0.5) * BIN});
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<uint8_t> taken(pend_.size(), 0);
    int tried = 0;
    for (auto [cnt, P] : peaks) {
        if (++tried > 6) break;
        (void)cnt;
        for (size_t s0 = 0; s0 < idx.size(); s0++) {
            if (taken[idx[s0]]) continue;
            std::vector<size_t> ch = {idx[s0]};
            std::vector<double> pris;
            size_t cur = s0;
            for (;;) {
                const Rec& c = pend_[idx[cur]];
                size_t nx = 0;
                double ne = 1e9;
                int nn = 0;
                for (size_t q = cur + 1; q < idx.size(); q++) {
                    const Rec& y = pend_[idx[q]];
                    const double d = y.t - c.t;
                    if (d > 3.2 * P) break;
                    if (taken[idx[q]] || std::fabs(y.db - c.db) > 10) continue;
                    const int n = static_cast<int>(std::lround(d / P));
                    if (n < 1 || n > 3) continue;
                    const double e = std::fabs(d - n * P);
                    if (e > n * (JIT * P + 5e-6)) continue;
                    const double sc = e / (n * P) + 0.01 * n;
                    if (sc < ne) {
                        ne = sc;
                        nx = q;
                        nn = n;
                    }
                }
                if (!nn) break;
                pris.push_back((pend_[idx[nx]].t - c.t) / nn);
                ch.push_back(idx[nx]);
                cur = nx;
            }
            if (ch.size() < static_cast<size_t>(CHAIN_MIN)) continue;
            const double span = pend_[ch.back()].t - pend_[ch.front()].t;
            if (static_cast<double>(ch.size()) < 0.5 * (span / P + 1)) continue;
            std::sort(pris.begin(), pris.end());
            const double pri = pris[pris.size() / 2];
            // an SSR we know with this PRI (or a multiple of it: every 2nd / 3rd
            // all-call of a train) that went quiet between its main beams: the same one
            Source* S = nullptr;
            int mult = 1;
            for (auto& sp : src_) {
                if (sp->type != SRC_SSR || sp->pri <= 0) continue;
                const int m = static_cast<int>(std::lround(pri / sp->pri));
                if (m < 1 || m > 4 || std::fabs(pri - m * sp->pri) > 0.02 * pri) continue;
                if (m == 1 && now - sp->t_last <= 8 * sp->pri) continue;   // a second train beside a live one
                if (!S || m < mult) {
                    S = sp.get();
                    mult = m;
                }
            }
            const bool fresh = !S;
            if (!S) S = make(SRC_SSR, pend_[ch.front()].t, out);
            if (dbg_)
                out.events.push_back(fmt("DBG chain P %.1f us", P * 1e6) + fmt(" pri %.1f", pri * 1e6) + fmt(" n %.0f", ch.size()) +
                                     fmt(" from %.4f", pend_[ch.front()].t) + fmt(" to %.4f", pend_[ch.back()].t) + " -> " + S->tid +
                                     fmt(" mult %.0f", mult));
            if (mult == 1) S->pri = pri;
            {   // the chain's own PRI spread
                double dev = 0;
                for (double v : pris) dev += std::fabs(v - pri);
                const double je = pris.empty() ? JIT * pri / 2 : dev / static_cast<double>(pris.size());
                S->jit_e = fresh ? je : std::max(S->jit_e, 0.5 * je);
            }
            for (size_t k : ch) taken[k] = 1;
            // replay the chain's members and the all-calls / roll-calls between them
            std::vector<size_t> all;
            for (size_t i = 0; i < pend_.size(); i++)
                if (!taken[i] || std::find(ch.begin(), ch.end(), i) != ch.end())
                    if (pend_[i].t >= pend_[ch.front()].t - 1e-3) all.push_back(i);
            S->t_last = pend_[ch.front()].t - S->pri;
            S->lvl_last = pend_[ch.front()].db;
            S->slots.push_back("|");   // the chain was broken: no slot pattern across it
            S->slot_open.clear();
            bool first = true;
            for (size_t i : all) {
                const Rec& r = pend_[i];
                if (taken[i]) {
                    if (first) {   // the chain's first member starts the train again
                        S->t_last = r.t - S->pri;
                        S->lvl_last = r.db;
                        first = false;
                    }
                    if (try_ssr(r, out)) taken[i] = 2;
                } else if (r.uf == 11 || (r.uf >= 0 && r.t <= pend_[ch.back()].t + 0.01)) {
                    if (try_ssr(r, out) || try_rollcall(r, out)) taken[i] = 2;
                }
            }
            if (fresh) S->first_t = pend_[ch.front()].t;
        }
    }
    std::deque<Rec> keep;
    for (size_t i = 0; i < pend_.size(); i++)
        if (taken[i] != 2) keep.push_back(pend_[i]);
    pend_.swap(keep);
}

// Bursts of what's left (not an SSR's: no P2 as strong as P1; not an
// ACAS's: no S1): three bursts at one level (+-3 dB) within 15 s are a
// fixed interrogator - or more of one we know at that level
void Tracker::find_bursts(double now, Out& out) {
    std::vector<size_t> ord;
    for (size_t i = 0; i < pend_.size(); i++)
        if (pend_[i].t < now - 0.1 && fixed_ok(pend_[i])) ord.push_back(i);
    std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return pend_[a].t < pend_[b].t; });
    std::vector<uint8_t> used(pend_.size(), 0);
    auto take = [&](Source& S, const Burst& bu) {
        for (size_t i = 0; i < pend_.size(); i++) {
            const Rec& r = pend_[i];
            if (used[i] || r.t < bu.start - 1e-6 || r.t > bu.last + 1e-6 || std::fabs(r.db - bu.lvl) > 3) continue;
            fixed_add(S, r);
            attach(S, r, out);
            used[i] = 1;
        }
    };
    for (size_t a = 0; a < ord.size();) {
        size_t b = a + 1;
        while (b < ord.size() && pend_[ord[b]].t - pend_[ord[b - 1]].t <= BURST_GAP) b++;
        if (b - a >= 3) {
            std::vector<float> lv;
            for (size_t k = a; k < b; k++) lv.push_back(pend_[ord[k]].db);
            Burst bu;
            bu.lvl = median(lv);
            for (size_t k = a; k < b; k++) {
                const Rec& r = pend_[ord[k]];
                if (std::fabs(r.db - bu.lvl) > 3) continue;
                if (bu.n == 0) bu.start = r.t;
                bu.last = r.t;
                bu.n++;
                (r.uf >= 0 && r.uf != 11 ? bu.s : bu.ac)++;
                if (r.uf < 0) bu.ac_t.push_back(r.t);
            }
            Source* S = nullptr;
            for (auto& sp : src_)
                if (sp->type == SRC_FIXED && std::fabs(sp->burst_lvl - bu.lvl) <= 3 &&
                    (!S || std::fabs(sp->burst_lvl - bu.lvl) < std::fabs(S->burst_lvl - bu.lvl)))
                    S = sp.get();
            if (bu.n >= 3 && S) {
                take(*S, bu);
            } else if (bu.n >= 3 && bu.lvl >= 13) {   // a new one: clear of the pulse threshold
                const bool known = std::any_of(orphan_bursts_.begin(), orphan_bursts_.end(),
                                               [&](const Burst& o) { return std::fabs(o.start - bu.start) < 1e-6; });
                if (!known) orphan_bursts_.push_back(bu);
            }
        }
        a = b;
    }
    while (!orphan_bursts_.empty() && orphan_bursts_.front().start < now - 15) orphan_bursts_.pop_front();
    // three orphan bursts at one level -> a new fixed interrogator - unless
    // they come seconds apart and their all-calls keep an SSR's PRI: that
    // SSR's main beam passing (its train not picked up again yet)
    for (size_t c = 0; c < orphan_bursts_.size(); c++) {
        std::vector<size_t> same;
        for (size_t k = 0; k < orphan_bursts_.size(); k++)
            if (std::fabs(orphan_bursts_[k].lvl - orphan_bursts_[c].lvl) <= 3) same.push_back(k);
        if (same.size() < 3) continue;
        const double span = orphan_bursts_[same.back()].start - orphan_bursts_[same.front()].start;
        const bool slow = span > 3.0 * static_cast<double>(same.size() - 1);
        int on_grid = 0, all = 0;
        for (size_t k : same) {
            const std::vector<double>& ts = orphan_bursts_[k].ac_t;
            for (size_t i = 1; i < ts.size(); i++) {
                all++;
                const double d = ts[i] - ts[i - 1];
                for (auto& sp : src_)
                    if (sp->type == SRC_SSR && sp->pri > 0) {
                        const int m = static_cast<int>(std::lround(d / sp->pri));
                        if (m >= 1 && m <= 3 && std::fabs(d - m * sp->pri) < 0.03 * sp->pri) {
                            on_grid++;
                            break;
                        }
                    }
            }
        }
        if (slow && all >= 4 && on_grid * 2 > all) continue;
        Source* S = make(SRC_FIXED, orphan_bursts_[same.front()].start, out);
        S->burst_lvl = orphan_bursts_[c].lvl;
        for (size_t k : same) take(*S, orphan_bursts_[k]);
        std::deque<Burst> rest;
        for (size_t k = 0; k < orphan_bursts_.size(); k++)
            if (std::find(same.begin(), same.end(), k) == same.end()) rest.push_back(orphan_bursts_[k]);
        orphan_bursts_.swap(rest);
        break;
    }
    std::deque<Rec> keep;
    for (size_t i = 0; i < pend_.size(); i++)
        if (!used[i]) keep.push_back(pend_[i]);
    pend_.swap(keep);
}

// main-beam passes of an SSR -> its scan period and beam width (radar_scan)
void Tracker::scan_update(Source& S, double now, Out& out) {
    const std::vector<radar::Burst> bs = S.lv.bursts(6, 0.05);
    double maxdw = 0;
    for (const radar::Burst& b : bs) maxdw = std::max(maxdw, b.dwell);
    float top = -1e9f;
    for (const radar::Burst& b : bs) top = std::max(top, b.peak_db);
    for (const radar::Burst& b : bs) {
        if (b.open || b.dwell < 0.5 * maxdw || b.peak_db < top - 6) continue;
        bool have = false;
        for (double t : S.beams)
            if (std::fabs(t - b.t) < 0.5) have = true;
        if (have) continue;
        S.beams.push_back(b.t);
        S.beam_db.push_back(b.peak_db);
        // -3 dB width from the unclipped bins around it
        std::vector<std::pair<double, float>> pts;
        S.lv.bins(b.from - 0.05, b.to + 0.05, pts);
        std::vector<std::pair<double, float>> top10;
        for (auto& p : pts)
            if (p.second >= b.peak_db - 10) top10.push_back(p);
        double t0, pk, w3;
        S.beam_w3.push_back(!b.clipped && radar::fit_lobe(top10, &t0, &pk, &w3) ? w3 : NAN);
        if (S.beams.size() > 12) {
            S.beams.pop_front();
            S.beam_db.pop_front();
            S.beam_w3.pop_front();
        }
    }
    std::sort(S.beams.begin(), S.beams.end());
    int sup = 0;
    const double P = radar::scan_period(S.beams, &sup);
    if (P > 0) {
        S.scan_s = P;
        S.scan_support = sup;
        if (std::fabs(P - S.scan_told) > 0.03 * P) {
            S.scan_told = P;
            if (S.announced) out.events.push_back("🛰 " + S.tid + fmt(": antenna scan %.2f s", P) + fmt(" (%.1f rpm)", 60 / P));
        }
    }
    S.mb_dwell = maxdw;
    if (!S.beam_db.empty()) S.mb_db = *std::max_element(S.beam_db.begin(), S.beam_db.end());
    (void)now;
}

// --- every 50 ms: DoA packets --------------------------------------------------
void Tracker::tick_fast(double now, Out& out) {
    for (auto& sp : src_) {
        Source& S = *sp;
        if (S.cand.empty() || S.cand.front().t >= now - DOA_HOLD_S) continue;
        // an SSR: only its main beam (the samples come straight from it), after its first pass
        const bool gate = S.type != SRC_SSR || S.beams.size() >= 2 || now - S.first_t > 30;
        float ref = S.lv.max_over(now - 15, now);
        if (std::isfinite(S.mb_db)) ref = std::max(ref, S.mb_db);
        while (!S.cand.empty() && S.cand.front().t < now - DOA_HOLD_S) {
            const Rec c = S.cand.front();
            S.cand.pop_front();
            if (!gate || !S.announced || c.t < now - 1.0) continue;   // too late for kraken_doa's 1.5 s ring
            if (S.type == SRC_SSR) {
                const float local = S.lv.max_over(c.t - 0.1, c.t + 0.1);
                if (c.db < ref - doa_db_ || local < ref - 6) continue;
            }
            S.tokens = std::min(DOA_RATE * 0.5, S.tokens + (c.t - S.tok_t) * DOA_RATE);
            S.tok_t = c.t;
            if (S.tokens < 1) continue;
            S.tokens -= 1;
            S.packets++;
            out.packets.push_back({&S, c});
        }
    }
}

// --- every 0.5 s ---------------------------------------------------------------
void Tracker::tick_slow(double now, Out& out) {
    // close ACAS sequences
    for (size_t i = 0; i < seqs_.size();) {
        if (now - seqs_[i].last > 0.1) {
            Seq q = seqs_[i];
            seqs_.erase(seqs_.begin() + static_cast<long>(i));
            close_seq(q, out);
        } else {
            i++;
        }
    }
    while (!pend_.empty() && pend_.front().t < now - PEND_S) {
        pend_.pop_front();
        unattr_++;
    }
    std::sort(pend_.begin(), pend_.end(), [](const Rec& a, const Rec& b) { return a.t < b.t; });
    find_trains(now, out);
    find_bursts(now, out);
    for (auto& sp : src_) {
        Source& S = *sp;
        while (!S.rate_t.empty() && S.rate_t.front() < now - 2) S.rate_t.pop_front();
        while (!S.chain.empty() && S.chain.front() < now - 4) S.chain.pop_front();
        if (S.type == SRC_SSR) {
            if (S.chain.size() >= 9) {
                std::vector<double> v(S.chain.begin(), S.chain.end());
                const radar::PriInfo pi = radar::analyze_pri(v);
                if (pi.ok) S.pri_info = pi;
            }
            scan_update(S, now, out);
        }
        // the code from roll-calls when no checked all-call gave it
        if (!S.ii_checked && S.type != SRC_ACAS) {
            int tot = 0, bk = -1, bv = 0;
            for (auto& [k, v] : S.ii_votes) {
                tot += v;
                if (v > bv) {
                    bv = v;
                    bk = k;
                }
            }
            if (bv >= 5 && bv >= 0.8 * tot && S.ii != bk) {
                S.ii = bk;
                if (S.announced) out.events.push_back("🛰 " + S.tid + fmt(": its roll-calls carry II %.0f (IIS field)", bk));
                name(S, out);
            }
        }
        // announce: an SSR after its first main-beam pass (or 10 s), a fixed
        // interrogator after its third burst, an ACAS after two sequences
        if (!S.announced) {
            const bool ready = S.type == SRC_SSR ? (!S.beams.empty() || now - S.first_t > 10) && S.n >= 20
                                                 : S.type == SRC_FIXED ? S.bursts.size() >= 3 : S.seqs.size() + S.bcasts >= 2;
            if (ready) {
                S.announced = true;
                std::string t;
                if (S.type == SRC_SSR)
                    t = "🛰 New SSR " + S.tid + fmt(": PRF %.1f Hz", 1 / S.pri) +
                        (S.ii >= 0 ? " · II " + std::to_string(S.ii) : S.si > 0 ? " · SI " + std::to_string(S.si) : "");
                else if (S.type == SRC_FIXED)
                    t = "🛰 New fixed interrogator " + S.tid + fmt(": steady at %.0f dB over the noise", S.burst_lvl) +
                        (S.burst_period > 0 ? fmt(", bursts every %.2f s", S.burst_period) : std::string()) +
                        " (not rotating - multilateration / WAM?)";
                else if (S.seqs.empty())
                    t = "✈ New ACAS (TCAS) " + S.tid + ": its broadcasts (UF16, parity checked) - no whisper-shout heard";
                else
                    t = "✈ New ACAS (TCAS) " + S.tid + fmt(": whisper-shout %.0f steps", S.last_steps) +
                        fmt(" %.1f dB apart", S.last_step_db) + fmt(" every %.1f ms", S.last_step_ms);
                out.events.push_back(t);
            }
        }
        if (S.announced && !S.lost && now - S.last_t > 60) {
            S.lost = true;
            out.events.push_back("🛰 " + S.tid + " not heard for 60 s");
        }
    }
}

}  // namespace ssr
