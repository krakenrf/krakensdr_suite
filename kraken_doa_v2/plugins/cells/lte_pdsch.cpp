// LTE PDSCH of broadcast transport blocks (SIB1 / SI messages): the DCI's
// grant -> its PRBs (type 2 resource allocation, localized or distributed),
// the REs in mapping order, soft bits, and soft combining of repeats over
// the turbo code's circular buffer.

#include "lte_dl.hpp"

#include "fec.hpp"
#include "lte_tables.hpp"
#include "turbo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cells::lte {

namespace {

uint32_t field(const std::vector<uint8_t>& b, int* pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 1) | (*pos < static_cast<int>(b.size()) ? b[static_cast<size_t>(*pos)] : 0u);
        (*pos)++;
    }
    return v;
}

int bits_for(int n) { return n <= 1 ? 0 : static_cast<int>(std::ceil(std::log2(static_cast<double>(n)) - 1e-9)); }

// RIV -> start, length over N (36.213 7.1.6.3)
bool riv_decode(int riv, int N, int* start, int* len) {
    if (N <= 0) return false;
    int L = riv / N + 1, s = riv % N;
    if (L + s > N) {
        L = N - L + 2;
        s = N - 1 - s;
    }
    if (L < 1 || s < 0 || s + L > N) return false;
    *start = s;
    *len = L;
    return true;
}

// distributed VRB -> PRB of the even (slot 0) / odd (slot 1) slot (36.211 6.2.3.2)
void dvrb_to_prb(int nrb, int gap, int nvrb, int* p0, int* p1) {
    const int Ng = gap == 2 ? ngap2(nrb) : ngap1(nrb);
    const int Nt = gap == 2 ? 2 * Ng : 2 * std::min(Ng, nrb - Ng);
    const int P = rbg_size(nrb);
    const int Nrow = ((Nt + 4 * P - 1) / (4 * P)) * P;
    const int Nnull = 4 * Nrow - Nt;
    const int vt = nvrb % Nt, base = Nt * (nvrb / Nt);
    const int t1 = 2 * Nrow * (vt % 2) + vt / 2 + base;
    const int t2 = Nrow * (vt % 4) + vt / 4 + base;
    int even;
    if (Nnull != 0 && vt >= Nt - Nnull && vt % 2 == 1) even = t1 - Nrow;
    else if (Nnull != 0 && vt >= Nt - Nnull && vt % 2 == 0) even = t1 - Nrow + Nnull / 2;
    else if (Nnull != 0 && vt < Nt - Nnull && vt % 4 >= 2) even = t2 - Nnull / 2;
    else even = t2;
    const int odd = (even + Nt / 2) % Nt + base;
    auto prb = [&](int t) { return t < Nt / 2 ? t : t + Ng - Nt / 2; };
    *p0 = prb(even);
    *p1 = prb(odd);
}

}  // namespace

std::string Grant::text() const {
    char b[200];
    std::string prbs;
    for (int p : prb[0]) prbs += (prbs.empty() ? "" : ",") + std::to_string(p);
    snprintf(b, sizeof b, "DCI %s, %s VRB %d-%d (PRB %s), %s %d, TBS %d bits%s", f1c ? "1C" : "1A", distributed ? "distributed" : "localized", vrb0,
             vrb0 + nvrb - 1, prbs.c_str(), f1c ? "I_TBS" : "MCS", mcs, tbs, rv >= 0 ? (", RV " + std::to_string(rv)).c_str() : "");
    return b;
}

Grant parse_dci(const Dci& d, int nrb, bool tdd) {
    Grant g;
    int pos = 0;
    int start = 0, len = 0;
    if (d.size == dci1c_size(nrb) && d.size != dci1a_size(nrb, tdd)) {
        g.f1c = true;
        g.distributed = true;
        g.gap = nrb >= 50 && field(d.bits, &pos, 1) ? 2 : 1;
        const int step = nrb < 50 ? 2 : 4;
        const int g1 = ngap1(nrb);
        const int nv = 2 * std::min(g1, nrb - g1) / step;
        const int riv = static_cast<int>(field(d.bits, &pos, bits_for(nv * (nv + 1) / 2)));
        if (!riv_decode(riv, nv, &start, &len)) return g;
        g.vrb0 = start * step;
        g.nvrb = len * step;
        g.mcs = static_cast<int>(field(d.bits, &pos, 5));
        g.tbs = TBS_1C[g.mcs];
    } else {
        if (!field(d.bits, &pos, 1)) return g;   // format 0 (uplink), not 1A
        g.distributed = field(d.bits, &pos, 1) != 0;
        const int riv = static_cast<int>(field(d.bits, &pos, bits_for(nrb * (nrb + 1) / 2)));
        g.mcs = static_cast<int>(field(d.bits, &pos, 5));
        field(d.bits, &pos, tdd ? 4 : 3);   // HARQ process (reserved)
        const int ndi = static_cast<int>(field(d.bits, &pos, 1));
        g.rv = static_cast<int>(field(d.bits, &pos, 2));
        const int tpc = static_cast<int>(field(d.bits, &pos, 2));
        g.nprb1a = (tpc & 1) ? 3 : 2;
        if (g.distributed && nrb >= 50) g.gap = ndi ? 2 : 1;
        int nv = nrb;
        if (g.distributed) {
            const int Ng = g.gap == 2 ? ngap2(nrb) : ngap1(nrb);
            nv = g.gap == 2 ? (nrb / (2 * Ng)) * 2 * Ng : 2 * std::min(Ng, nrb - Ng);
        }
        if (!riv_decode(riv, nrb, &start, &len) || start + len > nv) return g;
        g.vrb0 = start;
        g.nvrb = len;
        if (g.mcs > 26) return g;
        g.tbs = static_cast<int>(TBS[g.mcs][g.nprb1a - 1]);
    }
    for (int v = g.vrb0; v < g.vrb0 + g.nvrb; v++) {
        int p0 = v, p1 = v;
        if (g.distributed) dvrb_to_prb(nrb, g.gap, v, &p0, &p1);
        if (p0 < 0 || p0 >= nrb || p1 < 0 || p1 >= nrb) return Grant{};
        g.prb[0].push_back(p0);
        g.prb[1].push_back(p1);
    }
    for (auto& s : g.prb) std::sort(s.begin(), s.end());
    g.ok = g.tbs > 0;
    return g;
}

std::vector<Re> pdsch_res(const Grant& g, int nrb, int ports, int nsym, int sf, int pci, bool tdd) {
    std::vector<Re> v;
    const int vs = pci % 6;
    for (int L = nsym; L < 14; L++) {
        const int slot = L / 7, l = L % 7, ns = 2 * sf + slot;
        for (int prb : g.prb[slot])
            for (int kk = 0; kk < 12; kk++) {
                const int k = 12 * prb + kk;
                const int kappa = k - 6 * nrb;
                const bool central = kappa >= -36 && kappa < 36;
                // the cell's CRS
                bool crs = false;
                for (int p = 0; p < std::max(1, ports) && !crs; p++) {
                    const int vv = crs_v(p, l, ns);
                    crs = vv >= 0 && (((kappa - (vv + vs)) % 6) + 6) % 6 == 0;
                }
                if (crs) continue;
                if (central) {
                    if (sf == 0 && slot == 1 && l <= 3) continue;                       // PBCH
                    if (!tdd && (sf == 0 || sf == 5) && slot == 0 && l >= 5) continue;  // FDD SSS + PSS
                    if (tdd && (sf == 0 || sf == 5) && L == 13) continue;               // TDD SSS
                    if (tdd && (sf == 1 || sf == 6) && L == 2) continue;                // TDD PSS
                }
                v.push_back({L, f_of_k(k, nrb)});
            }
    }
    return v;
}

void pdsch_llr(const CellRx& rx, const std::vector<Re>& re, int ports, uint16_t rnti, std::vector<float>* llr) {
    txd_llr(rx, re, ports, llr);
    const uint32_t cinit = (static_cast<uint32_t>(rnti) << 14) + (static_cast<uint32_t>(rx.sf()) << 9) + static_cast<uint32_t>(rx.pci());
    const auto c = gold(cinit, static_cast<int>(llr->size()));
    for (size_t i = 0; i < llr->size(); i++)
        if (c[i]) (*llr)[i] = -(*llr)[i];
}

void TbAcc::reset(int tbs) {
    tbs_ = tbs;
    n_ = 0;
    const int B = tbs + 24;
    K_ = fec::turbo_k(B);
    F_ = K_ > 0 ? K_ - B : 0;
    for (int s = 0; s < 3; s++) {
        s_[s].assign(static_cast<size_t>(K_ > 0 ? K_ + 4 : 0), 0.0f);
        hit_[s].assign(s_[s].size(), 0);
    }
}

void TbAcc::add(const std::vector<float>& llr, int rv) {
    if (K_ <= 0 || llr.empty()) return;
    const auto sel = fec::turbo_rate_match(K_, F_, static_cast<int>(llr.size()), rv & 3);
    for (size_t i = 0; i < sel.size() && i < llr.size(); i++) {
        if (llr[i] == 0.0f) continue;
        s_[sel[i].first][static_cast<size_t>(sel[i].second)] += llr[i];
        hit_[sel[i].first][static_cast<size_t>(sel[i].second)] = 1;
    }
    n_++;
}

double TbAcc::coverage() const {
    size_t h = 0, t = 0;
    for (int s = 0; s < 3; s++)
        for (size_t i = 0; i < hit_[s].size(); i++) {
            if (s < 2 && static_cast<int>(i) < F_) continue;
            t++;
            h += hit_[s][i];
        }
    return t ? static_cast<double>(h) / static_cast<double>(t) : 0;
}

bool TbAcc::decode(std::vector<uint8_t>* tb, int* iters) {
    if (K_ <= 0 || n_ == 0) return false;
    std::vector<float> s[3] = {s_[0], s_[1], s_[2]};
    // keep the numbers in a sane range for the max-log decoder
    float mx = 0;
    for (auto& v : s)
        for (float x : v) mx = std::max(mx, std::fabs(x));
    if (mx > 0)
        for (auto& v : s)
            for (float& x : v) x *= 50.0f / mx;
    // filler bits: known zeros
    for (int i = 0; i < F_; i++) s[0][static_cast<size_t>(i)] = s[1][static_cast<size_t>(i)] = 100.0f;
    const int B = tbs_ + 24;
    auto crc_ok = [&](const std::vector<uint8_t>& b) { return fec::crc(b.data() + F_, B, fec::CRC24A, 24) == 0; };
    const auto bits = fec::turbo_decode(s, K_, 10, crc_ok, iters);
    if (static_cast<int>(bits.size()) != K_ || !crc_ok(bits)) return false;
    tb->assign(bits.begin() + F_, bits.begin() + F_ + tbs_);
    return true;
}

}  // namespace cells::lte
