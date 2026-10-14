#include "lte_dl.hpp"

#include "fec.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace cells::lte {

namespace {

constexpr int PBCH_E = 1920;        // PBCH bits over the 4 frames of a MIB (normal CP)
constexpr int PBCH_PER_FRAME = 480;

}  // namespace

CellRx::CellRx(const std::vector<const cf*>& x, size_t n, const Numerology& num, int pci, double frame0, double cfo_hz, double carrier_off,
               double vis_hz)
    : x_(x), n_(n), num_(num), pci_(pci), frame0_(frame0), cfo_(cfo_hz), m_(static_cast<int>(x.size())), N_(num.nfft()), K_(N_ / 2 - 1),
      W_(2 * K_ + 1), sym_(N_, K_) {
    vis_.assign(static_cast<size_t>(W_), 0);
    for (int f = -K_; f <= K_; f++) vis_[static_cast<size_t>(f + K_)] = f != 0 && visible(f, num.scs(), carrier_off, vis_hz);
    Y_.assign(static_cast<size_t>(14 * m_ * W_), cf(0, 0));
    H_.assign(static_cast<size_t>(4 * 14 * m_ * W_), cf(0, 0));
    const int ls[3] = {0, 1, 4};
    for (int i = 0; i < 3; i++) {
        crs_[i].resize(20);
        for (int ns = 0; ns < 20; ns++) crs_[i][static_cast<size_t>(ns)] = crs_seq(pci, ns, ls[i]);
    }
    // the pilots' subcarriers of each port (both slots of a subframe: every
    // 3rd subcarrier) and every subcarrier's interpolation between them
    for (int p = 0; p < 4; p++) {
        std::vector<int> pf;
        for (int f = -K_; f <= K_; f++) {
            if (!vis(f)) continue;
            bool pilot = false;
            for (int ns = 0; ns < 2 && !pilot; ns++)
                for (int l = 0; l < 7 && !pilot; l++) {
                    int mp = 0;
                    pilot = crs_at(f, crs_v(p, l, ns), pci, &mp);
                }
            if (pilot) pf.push_back(f);
        }
        pf_[p] = pf;
        ip_[p].assign(static_cast<size_t>(W_), Interp{});
        if (pf.empty()) continue;
        for (int f = -K_; f <= K_; f++) {
            Interp& q = ip_[p][static_cast<size_t>(f + K_)];
            const auto it = std::lower_bound(pf.begin(), pf.end(), f);
            if (it == pf.end()) {
                q.i0 = q.i1 = static_cast<int>(pf.size()) - 1;
            } else if (*it == f || it == pf.begin()) {
                q.i0 = q.i1 = static_cast<int>(it - pf.begin());
            } else {
                q.i1 = static_cast<int>(it - pf.begin());
                q.i0 = q.i1 - 1;
                q.w = static_cast<float>(f - pf[static_cast<size_t>(q.i0)]) / static_cast<float>(pf[static_cast<size_t>(q.i1)] - pf[static_cast<size_t>(q.i0)]);
            }
        }
    }
}

long CellRx::first_frame() const { return static_cast<long>(std::floor(-frame0_ / (0.01 * num_.fs))); }
long CellRx::last_frame() const { return static_cast<long>(std::floor((static_cast<double>(n_) - frame0_) / (0.01 * num_.fs))); }

bool CellRx::load(long k, int sf) {
    sf_ = -1;
    const double base = frame0_ + static_cast<double>(k) * 0.01 * num_.fs + sf * 1e-3 * num_.fs;
    const double early = std::min(4.0, 0.4 * num_.cp());
    double t[14];
    for (int slot = 0; slot < 2; slot++)
        for (int l = 0; l < 7; l++) {
            const int L = slot * 7 + l;
            const double s = base + slot * num_.half_ms() + num_.useful(l);
            t[L] = (s - base) / num_.fs;
            for (int a = 0; a < m_; a++)
                if (!sym_.run(x_[static_cast<size_t>(a)], n_, s, early, Y_.data() + (static_cast<size_t>(L) * m_ + static_cast<size_t>(a)) * W_))
                    return false;
        }
    // channel of every port: the pilots' LS estimates turned back to the
    // subframe's start (the cell's carrier offset), averaged over the
    // subframe per subcarrier, interpolated across the subcarriers, turned
    // forward to each symbol
    std::vector<cd> acc(static_cast<size_t>(W_));
    std::vector<int> cnt(static_cast<size_t>(W_));
    std::vector<cf> hbase(static_cast<size_t>(W_));
    cf rot_fwd[14];
    for (int L = 0; L < 14; L++) rot_fwd[L] = std::polar(1.0f, static_cast<float>(2 * M_PI * cfo_ * t[L]));
    double nacc = 0;
    int ncnt = 0;
    for (int p = 0; p < 4; p++) {
        if (p >= nports_) {
            pp_[static_cast<size_t>(p)] = 0;
            continue;
        }
        double pw = 0;
        int pwn = 0;
        for (int a = 0; a < m_; a++) {
            std::fill(acc.begin(), acc.end(), cd(0, 0));
            std::fill(cnt.begin(), cnt.end(), 0);
            // port 0, symbol 0 of both slots (the same subcarriers): their
            // difference measures the noise
            std::vector<cd> s0(static_cast<size_t>(W_), cd(0, 0));
            for (int slot = 0; slot < 2; slot++) {
                const int ns = 2 * sf + slot;
                for (int l = 0; l < 7; l++) {
                    const int v = crs_v(p, l, ns);
                    if (v < 0) continue;
                    const int L = slot * 7 + l;
                    const auto& r = crs_[l == 0 ? 0 : l == 1 ? 1 : 2][static_cast<size_t>(ns)];
                    const cd rot = std::polar(1.0, -2 * M_PI * cfo_ * t[L]);
                    const cf* Y = y(L, a);
                    for (int f : pf_[p]) {
                        int mp = 0;
                        if (!crs_at(f, v, pci_, &mp)) continue;
                        const cd ls = cd(Y[f + K_]) * std::conj(r[static_cast<size_t>(mp)]) * rot;
                        acc[static_cast<size_t>(f + K_)] += ls;
                        cnt[static_cast<size_t>(f + K_)]++;
                        if (p == 0 && l == 0) {
                            if (slot == 0) {
                                s0[static_cast<size_t>(f + K_)] = ls;
                            } else {
                                nacc += std::norm(ls - s0[static_cast<size_t>(f + K_)]) / 2;
                                ncnt++;
                            }
                        }
                    }
                }
            }
            std::vector<cd> hp(pf_[p].size());
            for (size_t i = 0; i < pf_[p].size(); i++) {
                const int f = pf_[p][i];
                const int c = cnt[static_cast<size_t>(f + K_)];
                hp[i] = c > 0 ? acc[static_cast<size_t>(f + K_)] / static_cast<double>(c) : cd(0, 0);
                pw += std::norm(hp[i]);
                pwn++;
            }
            // interpolated once, turned per symbol
            for (int f = -K_; f <= K_; f++) {
                if (hp.empty()) {
                    hbase[static_cast<size_t>(f + K_)] = cf(0, 0);
                    continue;
                }
                const Interp& q = ip_[p][static_cast<size_t>(f + K_)];
                const cd v = q.i0 == q.i1 ? hp[static_cast<size_t>(q.i0)]
                                          : hp[static_cast<size_t>(q.i0)] * static_cast<double>(1 - q.w) + hp[static_cast<size_t>(q.i1)] * static_cast<double>(q.w);
                hbase[static_cast<size_t>(f + K_)] = cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
            }
            for (int L = 0; L < 14; L++) {
                cf* H = H_.data() + ((static_cast<size_t>(p) * 14 + static_cast<size_t>(L)) * m_ + static_cast<size_t>(a)) * W_;
                const cf rot = rot_fwd[L];
                for (int i = 0; i < W_; i++) H[i] = hbase[static_cast<size_t>(i)] * rot;
            }
        }
        pp_[static_cast<size_t>(p)] = pwn > 0 ? static_cast<float>(pw / pwn) : 0.0f;
    }
    noise_ = ncnt > 0 ? static_cast<float>(std::max(nacc / ncnt, 1e-12)) : 1.0f;
    sf_ = sf;
    k_ = k;
    return true;
}

void txd_llr(const CellRx& rx, const std::vector<Re>& re, int ports, std::vector<float>* llr) {
    const int m = rx.m(), K = rx.K();
    const size_t n = re.size();
    llr->assign(2 * n, 0.0f);
    const float s2 = std::max(rx.noise(), 1e-12f);
    auto ok = [&](size_t i) { return rx.vis(re[i].f); };
    if (ports <= 1) {
        for (size_t i = 0; i < n; i++) {
            if (!ok(i)) continue;
            cd z = 0;
            for (int a = 0; a < m; a++) z += std::conj(cd(rx.h(0, re[i].l, a)[re[i].f + K])) * cd(rx.y(re[i].l, a)[re[i].f + K]);
            const float sc = static_cast<float>(2 * std::sqrt(2.0) / s2);
            (*llr)[2 * i] = static_cast<float>(z.real()) * sc;
            (*llr)[2 * i + 1] = static_cast<float>(z.imag()) * sc;
        }
        return;
    }
    // Alamouti over ports (p, q) on the REs (i, i + 1): x0 = d(i), x1 = d(i + 1)
    auto pair = [&](size_t i, int p, int q) {
        if (!ok(i) || !ok(i + 1)) return;
        cd z0 = 0, z1 = 0;
        for (int a = 0; a < m; a++) {
            const Re& e0 = re[i];
            const Re& e1 = re[i + 1];
            const cd h0 = (cd(rx.h(p, e0.l, a)[e0.f + K]) + cd(rx.h(p, e1.l, a)[e1.f + K])) * 0.5;
            const cd h1 = (cd(rx.h(q, e0.l, a)[e0.f + K]) + cd(rx.h(q, e1.l, a)[e1.f + K])) * 0.5;
            const cd r0 = rx.y(e0.l, a)[e0.f + K], r1 = rx.y(e1.l, a)[e1.f + K];
            // r0 = (h0 x0 - h1 x1*) / sqrt2, r1 = (h0 x1 + h1 x0*) / sqrt2
            z0 += std::conj(h0) * r0 + h1 * std::conj(r1);
            z1 += std::conj(h0) * r1 - h1 * std::conj(r0);
        }
        const float sc = 2 / s2;
        (*llr)[2 * i] = static_cast<float>(z0.real()) * sc;
        (*llr)[2 * i + 1] = static_cast<float>(z0.imag()) * sc;
        (*llr)[2 * i + 2] = static_cast<float>(z1.real()) * sc;
        (*llr)[2 * i + 3] = static_cast<float>(z1.imag()) * sc;
    };
    if (ports == 2) {
        for (size_t i = 0; i + 1 < n; i += 2) pair(i, 0, 1);
    } else {
        for (size_t i = 0; i + 3 < n; i += 4) {
            pair(i, 0, 2);
            pair(i + 2, 1, 3);
        }
    }
}

namespace {

// the PBCH's REs in mapping order: subframe 0 slot 1 symbols 0..3, the 72
// central subcarriers, without the CRS positions of 4 ports
std::vector<Re> pbch_res(int pci) {
    std::vector<Re> v;
    for (int l = 7; l <= 10; l++)
        for (int kp = 0; kp < 72; kp++) {
            const int f = kp < 36 ? kp - 36 : kp - 35;
            if ((l == 7 || l == 8) && ((kappa_of(f) % 3) + 3) % 3 == (pci % 6) % 3) continue;
            v.push_back({l, f});
        }
    return v;
}

}  // namespace

bool decode_pbch(const CellRx& rx, int ports_hint, Mib* out) {
    static const std::vector<std::pair<int, int>> sel = fec::conv_rate_match(40, PBCH_E);
    const auto re = pbch_res(rx.pci());
    if (re.size() * 2 != PBCH_PER_FRAME) return false;
    const auto c = gold(static_cast<uint32_t>(rx.pci()), PBCH_E);
    int order[3] = {ports_hint == 4 ? 4 : ports_hint == 1 ? 1 : 2, 0, 0};
    {
        int i = 1;
        for (int p : {2, 1, 4})
            if (p != order[0]) order[i++] = p;
    }
    std::vector<float> llr;
    for (int ports : order) {
        if (ports > rx.nports()) continue;   // no channel estimate for those ports
        txd_llr(rx, re, ports, &llr);
        for (int j = 0; j < 4; j++) {
            std::vector<float> s[3];
            for (auto& v : s) v.assign(40, 0.0f);
            for (int i = 0; i < PBCH_PER_FRAME; i++) {
                const float v = llr[static_cast<size_t>(i)] * (c[static_cast<size_t>(PBCH_PER_FRAME * j + i)] ? -1.0f : 1.0f);
                const auto& q = sel[static_cast<size_t>(PBCH_PER_FRAME * j + i)];
                s[q.first][static_cast<size_t>(q.second)] += v;
            }
            const auto bits = fec::viterbi_tb(s);
            if (bits.size() != 40) continue;
            uint32_t par = 0;
            for (int i = 24; i < 40; i++) par = (par << 1) | bits[static_cast<size_t>(i)];
            const uint32_t mask = fec::crc(bits.data(), 24, fec::CRC16, 16) ^ par;
            const int got = mask == 0x0000 ? 1 : mask == 0xFFFF ? 2 : mask == 0x5555 ? 4 : 0;
            if (!got) continue;
            uint32_t mib = 0;
            for (int i = 0; i < 24; i++) mib = (mib << 1) | bits[static_cast<size_t>(i)];
            static const int NRB[8] = {6, 15, 25, 50, 75, 100, 0, 0};
            Mib m;
            m.bits = mib;
            m.nrb = NRB[(mib >> 21) & 7];
            if (!m.nrb) continue;
            m.ports = got;
            m.phich_ext = static_cast<int>((mib >> 20) & 1);
            m.phich_ng = static_cast<int>((mib >> 18) & 3);
            m.sfn = static_cast<int>(((mib >> 10) & 0xFF) << 2) | j;
            m.sib1_br = static_cast<int>((mib >> 5) & 31);
            *out = m;
            return true;
        }
    }
    return false;
}

std::string phich_ng_text(int ng) {
    static const char* t[4] = {"1/6", "1/2", "1", "2"};
    return t[ng & 3];
}

namespace {

// the REs of the REG starting at subcarrier k0 (k index) in symbol l
std::array<Re, 4> reg_res(int k0, int l, int span, int nrb, int pci) {
    std::array<Re, 4> r{};
    int j = 0;
    for (int k = k0; k < k0 + span && j < 4; k++) {
        if (span == 6 && (k % 3) == (pci % 6) % 3) continue;   // CRS positions (k mod 6 = kappa mod 6)
        r[static_cast<size_t>(j++)] = {l, f_of_k(k, nrb)};
    }
    return r;
}

// PCFICH REG starts (k) in symbol 0
std::array<int, 4> pcfich_k(int pci, int nrb) {
    const int kbar = 6 * (pci % (2 * nrb));
    std::array<int, 4> k{};
    for (int i = 0; i < 4; i++) k[static_cast<size_t>(i)] = (kbar + (i * nrb / 2) * 6) % (12 * nrb);
    return k;
}

}  // namespace

int decode_pcfich(const CellRx& rx, const Mib& mib, double* conf) {
    if (mib.nrb <= 0 || rx.sf() < 0) return 0;
    std::vector<Re> re;
    for (int k0 : pcfich_k(rx.pci(), mib.nrb)) {
        const auto r = reg_res(k0, 0, 6, mib.nrb, rx.pci());
        re.insert(re.end(), r.begin(), r.end());
    }
    std::vector<float> llr;
    txd_llr(rx, re, mib.ports, &llr);
    const uint32_t cinit = static_cast<uint32_t>((rx.sf() + 1) * (2 * rx.pci() + 1)) * 512u + static_cast<uint32_t>(rx.pci());
    const auto c = gold(cinit, 32);
    // codewords: CFI 1 = 011 repeated, 2 = 101, 3 = 110 (36.212 5.3.4)
    static const int CW[3][3] = {{0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
    double sc[3] = {0, 0, 0};
    for (int i = 0; i < 32; i++) {
        const float v = llr[static_cast<size_t>(i)] * (c[static_cast<size_t>(i)] ? -1.0f : 1.0f);
        for (int q = 0; q < 3; q++) sc[q] += CW[q][i % 3] ? -v : v;
    }
    int best = 0;
    for (int q = 1; q < 3; q++)
        if (sc[q] > sc[best]) best = q;
    double second = -1e30;
    for (int q = 0; q < 3; q++)
        if (q != best) second = std::max(second, sc[q]);
    if (conf) *conf = sc[best] - second;
    return sc[best] > 0 ? best + 1 : 0;
}

Control control_layout(int pci, const Mib& mib, int cfi, int phich_m) {
    Control c;
    const int nrb = mib.nrb;
    c.nrb = nrb;
    c.ports = mib.ports;
    c.cfi = cfi;
    c.nsym = cfi + (nrb <= 10 ? 1 : 0);
    if (nrb <= 0 || cfi < 1 || cfi > 3) return c;
    // the REGs of each symbol: start subcarrier + span
    auto span_of = [&](int l) { return l == 0 || (l == 1 && mib.ports == 4) ? 6 : 4; };
    struct Reg {
        int k0, l;
        bool used;
    };
    std::vector<Reg> regs[4];
    for (int l = 0; l < c.nsym && l < 4; l++)
        for (int k0 = 0; k0 < 12 * nrb; k0 += span_of(l)) regs[l].push_back({k0, l, false});
    // PCFICH
    for (int k0 : pcfich_k(pci, nrb))
        for (auto& r : regs[0])
            if (r.k0 == k0) r.used = true;
    // PHICH (normal CP): groups of 3 REGs, in symbol 0 (normal duration) or 0..2 (extended)
    static const double NG[4] = {1.0 / 6, 0.5, 1, 2};
    const int ngroup = static_cast<int>(std::ceil(NG[mib.phich_ng & 3] * nrb / 8.0)) * phich_m;
    // REGs of a symbol not used by the PCFICH, by frequency
    std::vector<int> free_idx[4];
    for (int l = 0; l < c.nsym && l < 4; l++)
        for (size_t i = 0; i < regs[l].size(); i++)
            if (!regs[l][i].used) free_idx[l].push_back(static_cast<int>(i));
    const int n0 = static_cast<int>(free_idx[0].size());
    for (int mp = 0; mp < ngroup; mp++)
        for (int i = 0; i < 3; i++) {
            const int li = mib.phich_ext ? i : 0;
            if (li >= c.nsym) continue;
            const int nl = static_cast<int>(free_idx[li].size());
            if (nl <= 0 || n0 <= 0) continue;
            const int ni = (pci * nl / n0 + mp + i * nl / 3) % nl;
            regs[li][static_cast<size_t>(free_idx[li][static_cast<size_t>(ni)])].used = true;
        }
    // the PDCCH's REGs: by start subcarrier, then symbol (36.211 6.8.5)
    for (int k = 0; k < 12 * nrb; k++)
        for (int l = 0; l < c.nsym && l < 4; l++) {
            const int sp = span_of(l);
            if (k % sp) continue;
            const Reg& r = regs[l][static_cast<size_t>(k / sp)];
            if (r.used) continue;
            c.reg.push_back(reg_res(r.k0, l, sp, nrb, pci));
        }
    const int M = static_cast<int>(c.reg.size());
    c.ncce = M / 9;
    // quadruplet interleaving (the sub-block interleaver of 36.212 5.1.4.2.1)
    // + cyclic shift by the cell ID
    static const int PERM[32] = {1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
                                 0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30};
    const int R = (M + 31) / 32, ND = 32 * R - M;
    std::vector<int> w;   // w(t) = z index
    w.reserve(static_cast<size_t>(M));
    for (int col = 0; col < 32; col++)
        for (int row = 0; row < R; row++) {
            const int y = PERM[col] + 32 * row;
            if (y >= ND) w.push_back(y - ND);
        }
    c.quad.assign(static_cast<size_t>(M), 0);
    for (int i = 0; i < M; i++) c.quad[static_cast<size_t>(w[static_cast<size_t>((i + pci) % M)])] = i;
    return c;
}

void pdcch_llr(const CellRx& rx, const Control& c, std::vector<float>* llr, int max_cce) {
    const int nq = 9 * std::min(c.ncce, max_cce);
    llr->assign(static_cast<size_t>(8 * nq), 0.0f);
    if (nq <= 0 || rx.sf() < 0) return;
    const auto sc = gold(static_cast<uint32_t>(rx.sf()) * 512u + static_cast<uint32_t>(rx.pci()), 8 * static_cast<int>(c.reg.size()));
    std::vector<Re> re(4);
    std::vector<float> v;
    for (int q = 0; q < nq; q++) {
        const auto& g = c.reg[static_cast<size_t>(c.quad[static_cast<size_t>(q)])];
        for (int i = 0; i < 4; i++) re[static_cast<size_t>(i)] = g[static_cast<size_t>(i)];
        txd_llr(rx, re, c.ports, &v);
        for (int j = 0; j < 8; j++) (*llr)[static_cast<size_t>(8 * q + j)] = v[static_cast<size_t>(j)] * (sc[static_cast<size_t>(8 * q + j)] ? -1.0f : 1.0f);
    }
}

int dci1a_size(int nrb, bool tdd) {
    const int rba = static_cast<int>(std::ceil(std::log2(nrb * (nrb + 1) / 2.0) - 1e-9));
    // format 1A: flag, localized / distributed, RBA, MCS 5, HARQ 3 (TDD 4), NDI, RV 2, TPC 2 (+ TDD DAI 2)
    const int a = 1 + 1 + rba + 5 + (tdd ? 4 : 3) + 1 + 2 + 2 + (tdd ? 2 : 0);
    // format 0 (padded to the same size): flag, hopping, RBA, MCS/RV 5, NDI, TPC 2, CS 3 (+ TDD 2), CQI 1
    const int z = 1 + 1 + rba + 5 + 1 + 2 + 3 + (tdd ? 2 : 0) + 1;
    int s = std::max(a, z);
    static const int AMB[] = {12, 14, 16, 20, 24, 26, 32, 40, 44, 56};
    for (int x : AMB)
        if (s == x) s++;
    return s;
}

int dci1c_size(int nrb) {
    const int step = nrb < 50 ? 2 : 4;
    const int g = ngap1(nrb);
    const int nvrb = 2 * std::min(g, nrb - g) / step;
    const int rba = static_cast<int>(std::ceil(std::log2(nvrb * (nvrb + 1) / 2.0) - 1e-9));
    return (nrb >= 50 ? 1 : 0) + rba + 5;
}

std::vector<Dci> dci_search(const std::vector<float>& llr, int ncce, const std::vector<int>& sizes, uint16_t rnti) {
    std::vector<Dci> out;
    for (int L : {8, 4}) {
        const int ncand = L == 4 ? 4 : 2;
        const int slots = ncce / L;
        if (slots <= 0) continue;
        for (int mc = 0; mc < ncand && mc < slots; mc++) {
            const int cce = L * (mc % slots);
            const int E = 72 * L;
            if (72 * cce + E > static_cast<int>(llr.size())) continue;
            const float* e = llr.data() + 72 * cce;
            int seen = 0;
            for (int i = 0; i < E; i++) seen += e[i] != 0.0f;
            if (seen < 40) continue;
            for (int size : sizes) {
                const int D = size + 16;
                static std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> sels;
                auto si = sels.find({D, E});
                if (si == sels.end()) si = sels.emplace(std::make_pair(D, E), fec::conv_rate_match(D, E)).first;
                const auto& sel = si->second;
                std::vector<float> s[3];
                for (auto& v : s) v.assign(static_cast<size_t>(D), 0.0f);
                for (int i = 0; i < E; i++) s[sel[static_cast<size_t>(i)].first][static_cast<size_t>(sel[static_cast<size_t>(i)].second)] += e[i];
                const auto bits = fec::viterbi_tb(s);
                if (static_cast<int>(bits.size()) != D) continue;
                uint32_t par = 0;
                for (int i = size; i < D; i++) par = (par << 1) | bits[static_cast<size_t>(i)];
                if ((fec::crc(bits.data(), size, fec::CRC16, 16) ^ par) != rnti) continue;
                // re-encoded: the share of the received soft bits that agree
                std::vector<uint8_t> d[3];
                fec::conv_encode(bits, d);
                double agree = 0, tot = 0;
                for (int i = 0; i < E; i++) {
                    if (e[i] == 0.0f) continue;
                    const uint8_t b = d[sel[static_cast<size_t>(i)].first][static_cast<size_t>(sel[static_cast<size_t>(i)].second)];
                    const double w = std::fabs(e[i]);
                    tot += w;
                    if ((e[i] < 0) == (b == 1)) agree += w;
                }
                Dci x;
                x.size = size;
                x.L = L;
                x.cce = cce;
                x.bits.assign(bits.begin(), bits.begin() + size);
                x.agree = tot > 0 ? agree / tot : 0;
                x.seen = seen;
                out.push_back(std::move(x));
            }
        }
    }
    return out;
}

}  // namespace cells::lte
