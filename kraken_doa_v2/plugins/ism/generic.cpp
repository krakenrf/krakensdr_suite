// Unknown signals: a package no protocol decoded is sliced by its own
// timing (pulse widths -> PWM, gap widths -> PPM, else NRZ at the shortest
// width). A row of >= 12 bits that the package sends at least twice is a
// device code - "unknown device" with the code as its id, so it gets a DoA
// of its own. Without a repeat, a clean burst of >= 32 bits is grouped by
// its signal class (modulation, coding, bit rate, frequency) - weaker: two
// devices of one kind side by side share it, and it is not counted as a
// valid frame.

#include "ism.hpp"

#include <algorithm>

namespace ism {

namespace {

// two-means of widths: (short, long, share of short)
bool split2(std::vector<int> v, double* s, double* l, double* share) {
    if (v.size() < 4) return false;
    std::sort(v.begin(), v.end());
    double c1 = v[v.size() / 5], c2 = v[v.size() * 4 / 5];
    size_t n1 = 0;
    for (int it = 0; it < 10; it++) {
        const double mid = (c1 + c2) / 2;
        double a = 0, b = 0;
        size_t na = 0, nb = 0;
        for (int x : v) {
            if (x < mid) { a += x; na++; }
            else { b += x; nb++; }
        }
        if (!na || !nb) return false;
        c1 = a / na;
        c2 = b / nb;
        n1 = na;
    }
    *s = c1;
    *l = c2;
    *share = static_cast<double>(n1) / v.size();
    return true;
}

// a row that is only a preamble / carrier: no information
bool trivial(const Row& r) {
    int ones = 0, trans = 0;
    for (int i = 0; i < r.n; i++) {
        ones += r.bit(i);
        if (i && r.bit(i) != r.bit(i - 1)) trans++;
    }
    if (ones <= 1 || ones >= r.n - 1) return true;
    if (trans < 4) return true;   // a carrier switching on / off, no code
    return trans >= r.n - 3;      // 0101...
}

}  // namespace

bool analyse_unknown(Pkt& pk, Msg* m) {
    const Burst& b = pk.burst;
    // FSK keeps its carrier on (an OOK signal's keying can look like two
    // tones to the discriminator, but its envelope comes in many pulses)
    const bool fsk = b.fsk_sep > 0 && b.fsk.size() >= 12 && b.ook.size() <= 4;
    const Mod mod = fsk ? Mod::FSK : Mod::OOK;
    const auto& ps = pk.pulses(mod);
    if (ps.size() < 8 || b.snr_db < 10) return false;
    std::vector<int> on, off;
    for (const auto& p : ps) {
        if (p.on > 0) on.push_back(p.on);
        if (p.off < (1 << 27)) off.push_back(p.off);
    }
    if (on.size() < 8) return false;
    std::vector<int> all = on;
    all.insert(all.end(), off.begin(), off.end());
    std::sort(all.begin(), all.end());
    const double base = all[all.size() / 10];
    if (base < us2s(20)) return false;
    // typical data gaps (not the long ones between frames)
    std::vector<int> offd;
    for (int g : off)
        if (g < 8 * base) offd.push_back(g);
    double ps_s = 0, ps_l = 0, ps_share = 0, gs_s = 0, gs_l = 0, gs_share = 0;
    const bool on2 = split2(on, &ps_s, &ps_l, &ps_share) && ps_l / ps_s > 1.6 && ps_l / ps_s < 5 && ps_share > 0.12 &&
                     ps_share < 0.88;
    const bool off2 = split2(offd, &gs_s, &gs_l, &gs_share) && gs_l / gs_s > 1.6 && gs_l / gs_s < 5 &&
                      gs_share > 0.12 && gs_share < 0.88;
    // PWM keeps the period: (short pulse, long gap) and (long pulse, short
    // gap) both last ~P, and so does most of the pulse + gap pairs. NRZ whose
    // runs are multiples of one bit has short + long gap = long + short gap
    // too (1T + 2T = 2T + 1T) - but its pairs come in every length (1T + 1T
    // preambles, 3T runs): live, a 20 kbit/s OOK NRZ device was sliced as PWM
    // and never made the same code twice
    auto pwm_period = [&]() {
        const double P = (ps_s + gs_l + ps_l + gs_s) / 2;
        int n = 0, in = 0;
        for (const auto& p : ps) {
            if (p.on <= 0 || p.off >= (1 << 27) || p.off >= 8 * base) continue;
            n++;
            if (std::fabs(p.on + p.off - P) < 0.25 * P) in++;
        }
        return n >= 8 && in >= 0.7 * n;
    };
    const char* coding;
    const Rows* rows;
    double bit_us;
    if (!fsk && on2 && (!off2 || (std::fabs(ps_l + gs_s - ps_s - gs_l) < 0.3 * (ps_l + gs_s) && pwm_period()))) {
        coding = "PWM";
        const double gmax = std::max(gs_l, ps_l) * 2.5;
        rows = &pk.pwm(mod, static_cast<int>(s2us(ps_s)), static_cast<int>(s2us(ps_l)), static_cast<int>(s2us(gmax)),
                       100000);
        bit_us = s2us(ps_s + ps_l);
    } else if (!fsk && !on2 && off2) {
        coding = "PPM";
        rows = &pk.ppm(mod, static_cast<int>(s2us(gs_s)), static_cast<int>(s2us(gs_l)),
                       static_cast<int>(s2us(gs_l * 2.5)), 100000);
        bit_us = s2us(on.empty() ? 0 : on[on.size() / 2] + (gs_s + gs_l) / 2);
    } else {
        coding = "NRZ";
        const int bu = static_cast<int>(s2us(base));
        rows = &pk.pcm(mod, bu, bu, 16 * bu, 100000);
        bit_us = s2us(base);
    }
    if (rows->empty()) return false;
    const char* ms = fsk ? "FSK" : "OOK";
    // a repeated code
    int ri = -1;
    for (size_t i = 0; i < rows->size(); i++) {
        const Row& r = (*rows)[i];
        if (r.n < 12 || trivial(r)) continue;
        if (rows->count_same(i) >= 2 && (ri < 0 || r.n > (*rows)[static_cast<size_t>(ri)].n)) ri = static_cast<int>(i);
    }
    m->model = "Unknown";
    m->kind = Kind::UNKNOWN;
    m->str("modulation", ms);
    m->str("coding", coding);
    m->num("bit_us", std::round(bit_us), "%.0f");
    if (coding[1] == 'W' || coding[1] == 'P') {
        m->num("short_us", std::round(s2us(coding[1] == 'W' ? ps_s : gs_s)), "%.0f");
        m->num("long_us", std::round(s2us(coding[1] == 'W' ? ps_l : gs_l)), "%.0f");
    }
    if (ri >= 0) {
        const Row& r = (*rows)[static_cast<size_t>(ri)];
        int64_t t0 = r.t0, t1 = r.t1;
        for (const auto& x : rows->r)
            if (x.same(r)) {
                t0 = std::min(t0, x.t0);
                t1 = std::max(t1, x.t1);
            }
        std::string hex = r.hex();
        std::string id = std::string(fsk ? "F" : "O") + coding + std::to_string(r.n) + " " + hex;
        if (id.size() > 26) id = id.substr(0, 26);
        m->id = id;
        m->num("bits", r.n, "%.0f");
        m->str("code", hex.size() > 40 ? hex.substr(0, 40) + "..." : hex);
        m->num("repeats", rows->count_same(static_cast<size_t>(ri)), "%.0f");
        m->t0 = t0;
        m->t1 = t1;
        m->check = "REPEAT";
        m->event = "code " + (hex.size() > 16 ? hex.substr(0, 16) + "..." : hex);
        return true;
    }
    // no repeat: the class of a clean burst with a long row (bits of glitch
    // length are no code: the splatter of another signal). 12 dB (was 15):
    // a class still has to be heard again (decoder.cpp) before it shows
    if (b.snr_db < 12 || bit_us < 40) return false;
    const Row* best = nullptr;
    for (const auto& r : rows->r)
        if (r.n >= 32 && !trivial(r) && (!best || r.n > best->n)) best = &r;
    if (!best) return false;
    const double kbps = bit_us > 0 ? 1e3 / bit_us : 0;
    char id[40];
    // the name of the class as first heard - decoder.cpp matches later bursts
    // to it loosely (bit rate, frequency) and keeps this name
    snprintf(id, sizeof id, "%s-%s %.1fk %+.0fkHz", ms, coding, kbps, std::round(b.fc / 25e3) * 25);
    m->id = id;
    m->num("bits", best->n, "%.0f");
    m->str("code", best->hex().substr(0, 40));
    m->t0 = best->t0;
    m->t1 = best->t1;
    m->check = "";
    return true;
}

}  // namespace ism
