#include "interr.hpp"

#include <algorithm>

namespace ssr {

namespace {
constexpr double WAIT_US = 40;     // a pulse waits this long before it is tried as P1
constexpr double KEEP_US = 50;     // pulses kept this long (S1 look-back)
constexpr double TOL_US = 0.3;     // spacing tolerance (2.4 MHz edges: ~0.1 us)
const double AC_SPACING[4] = {8.0, 17.0, 21.0, 25.0};   // A, B, C, D
bool short_ok(const Pulse& p) { return !p.longp && p.width_us() >= 0.35 && p.width_us() <= 1.45; }
}  // namespace

int Interr::kind() const {
    if (mode == MODE_S) return nbits == 112 ? 13 : 12;
    return mode * 3 + p4;   // 0..11
}

const char* Interr::name() const {
    static const char* N[] = {"A", "A+P4s", "A+P4L", "B", "B+P4s", "B+P4L", "C", "C+P4s", "C+P4L", "D", "D+P4s", "D+P4L", "S56", "S112"};
    const int k = kind();
    return k >= 0 && k < 14 ? N[k] : "?";
}

// the best unused pulse at sample position `at` +-tol (us), of the given kind,
// within max_ddb of ref_db (max_ddb < 0: any level)
int Assembler::find(size_t from, double at, double tol, bool longp, float ref_db, float max_ddb) const {
    int best = -1;
    double bd = 1e9;
    const double tl = tol / US;
    for (size_t i = from; i < q_.size(); i++) {
        const Pulse& p = q_[i];
        if (p.le > at + tl) break;
        if (used_[i] || p.longp != longp || p.le < at - tl) continue;
        if (max_ddb >= 0 && std::fabs(p.db - ref_db) > max_ddb) continue;
        const double d = std::fabs(p.le - at);
        if (d < bd) {
            bd = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void Assembler::push(const std::vector<Pulse>& ps, int64_t now, std::vector<Interr>& out) {
    for (const Pulse& p : ps) {
        q_.push_back(p);
        used_.push_back(0);
    }
    const double wait = WAIT_US / US, keep = KEEP_US / US;
    while (head_ < q_.size() && q_[head_].le < static_cast<double>(now) - wait) {
        const size_t k = head_++;
        const Pulse& p1 = q_[k];
        if (used_[k] || !short_ok(p1)) continue;
        const double a = p1.le;
        Interr it;
        it.p1 = a;
        it.db = p1.db;
        it.clipped = p1.clipped;
        // Mode S: a long pulse from P2 (merged) or P6 on
        int l6 = -1;
        for (size_t i = k + 1; i < q_.size(); i++) {
            const Pulse& q = q_[i];
            if (q.le > a + 3.9 / US) break;
            if (used_[i] || !q.longp || q.le < a + 1.7 / US) continue;
            const double total = (q.te - a) * US;
            if ((total >= 17.0 && total <= 23.0) || (total >= 30.0 && total <= 37.0)) {
                if (std::fabs(q.db - p1.db) <= 8) l6 = static_cast<int>(i);
                break;
            }
        }
        if (l6 >= 0) {
            const Pulse& q = q_[static_cast<size_t>(l6)];
            it.mode = MODE_S;
            it.nbits = (q.te - a) * US > 27 ? 112 : 56;
            it.p6_db = q.db;
            it.end = q.te;
            it.clipped = it.clipped || q.clipped;
            used_[k] = used_[static_cast<size_t>(l6)] = 1;
            const int i2 = find(k + 1, a + 2.0 / US, 0.35, false, p1.db, -1);
            if (i2 >= 0) {
                it.p2_db = q_[static_cast<size_t>(i2)].db - p1.db;
                used_[static_cast<size_t>(i2)] = 1;
            }
            out.push_back(it);
            continue;
        }
        // Mode A / B / C / D: P3 at the mode's spacing, the same level (3 dB)
        int i3 = -1, mode = -1;
        double bd = 1e9;
        for (int m = 0; m < 4; m++) {
            const int c = find(k + 1, a + AC_SPACING[m] / US, TOL_US, false, p1.db, 3.0f);
            if (c < 0 || !short_ok(q_[static_cast<size_t>(c)])) continue;
            const double d = std::fabs(q_[static_cast<size_t>(c)].le - a - AC_SPACING[m] / US);
            if (d < bd) {
                bd = d;
                i3 = c;
                mode = m;
            }
        }
        if (i3 < 0) continue;
        // Mode B / D (rare) - or an ACAS S1 2 us before a Mode A / C P1 (S1 +
        // P3 / P4 look like B / D): the pulse 2 us later with an A / C of its own wins
        if (mode == MODE_B || mode == MODE_D) {
            const int ip = find(k + 1, a + 2.0 / US, 0.35, false, p1.db, -1);
            if (ip >= 0 && short_ok(q_[static_cast<size_t>(ip)])) {
                const Pulse& pp = q_[static_cast<size_t>(ip)];
                bool ac = false;
                for (int m : {MODE_A, MODE_C}) {
                    const int c = find(static_cast<size_t>(ip) + 1, pp.le + AC_SPACING[m] / US, TOL_US, false, pp.db, 3.0f);
                    if (c >= 0 && short_ok(q_[static_cast<size_t>(c)])) ac = true;
                }
                if (ac) continue;
            }
        }
        const Pulse& p3 = q_[static_cast<size_t>(i3)];
        it.mode = mode;
        it.end = p3.te;
        it.clipped = it.clipped || p3.clipped;
        used_[k] = used_[static_cast<size_t>(i3)] = 1;
        const int i2 = find(k + 1, a + 2.0 / US, 0.35, false, p1.db, -1);
        if (i2 >= 0 && i2 != i3) {
            it.p2_db = q_[static_cast<size_t>(i2)].db - p1.db;
            used_[static_cast<size_t>(i2)] = 1;
        }
        const int i4 = find(static_cast<size_t>(i3) + 1, p3.le + 2.0 / US, 0.35, false, p1.db, 6.0f);
        if (i4 >= 0) {
            const Pulse& p4 = q_[static_cast<size_t>(i4)];
            it.p4 = p4.width_us() >= 1.2 ? 2 : 1;
            it.end = p4.te;
            used_[static_cast<size_t>(i4)] = 1;
        }
        // S1 (whisper-shout): an unused pulse 2 us before P1, not stronger
        for (size_t i = k; i-- > 0;) {
            const Pulse& s = q_[i];
            if (s.le < a - 2.4 / US) break;
            if (!used_[i] && !s.longp && std::fabs(s.le - (a - 2.0 / US)) <= 0.35 / US && s.db <= p1.db + 1) {
                it.s1_db = s.db - p1.db;
                used_[i] = 1;
                break;
            }
        }
        out.push_back(it);
    }
    // forget old pulses
    while (!q_.empty() && head_ > 0 && q_.front().le < static_cast<double>(now) - keep) {
        q_.pop_front();
        used_.pop_front();
        head_--;
    }
}

}  // namespace ssr
