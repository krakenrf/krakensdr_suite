#include "lte_meta.hpp"

#include "lte_info.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cells::lte {

namespace {

constexpr uint16_t SI_RNTI = 0xFFFF;
constexpr double MIN_AGREE = 0.8;       // a DCI's re-encoded bits must match this share of the received ones
constexpr int MAX_TX = 64;              // transmissions combined before an accumulator starts over
constexpr double CFI_SURE = 20;         // a PCFICH decision this far ahead of the next codeword: the other CFIs aren't tried

// TDD: downlink subframes (D) per UL/DL configuration (36.211 Table 4.2-2)
const char* TDD_SF[7] = {"DSUUUDSUUU", "DSUUDDSUUD", "DSUDDDSUDD", "DSUUUDDDDD", "DSUUDDDDDD", "DSUDDDDDDD", "DSUUUDSUUD"};
// PHICH factor m_i (36.211 Table 6.9-1)
const int TDD_MI[7][10] = {{2, 1, -1, -1, -1, 2, 1, -1, -1, -1}, {0, 1, -1, -1, 1, 0, 1, -1, -1, 1}, {0, 0, -1, 1, 0, 0, 0, -1, 1, 0},
                           {1, 0, -1, -1, -1, 0, 0, 0, 1, 1},   {0, 0, -1, -1, 0, 0, 0, 0, 1, 1}, {0, 0, -1, 0, 0, 0, 0, 0, 1, 0},
                           {1, 1, -1, -1, -1, 1, 1, -1, -1, 1}};

std::string F(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}

bool same_mib(const Mib& a, const Mib& b) {
    return a.nrb == b.nrb && a.ports == b.ports && a.phich_ext == b.phich_ext && a.phich_ng == b.phich_ng;
}

std::string mib_text(const Mib& m) {
    return F("%g MHz", m.nrb == 6 ? 1.4 : m.nrb / 5.0) + " (" + std::to_string(m.nrb) + " RB), " + std::to_string(m.ports) + " antenna port" +
           (m.ports > 1 ? "s" : "") + ", PHICH " + (m.phich_ext ? "extended" : "normal") + " Ng " + phich_ng_text(m.phich_ng) +
           (m.sib1_br ? ", LTE-M SIB1-BR " + std::to_string(m.sib1_br) : "");
}

// the SI-RNTI DCI of the loaded subframe: the PCFICH's CFI first, then the others
bool find_dci(const CellRx& rx, CellMeta& m, const MetaParams& p, const Sib1* sib1, Grant* g, int* nsym, std::string* text) {
    double conf = 0;
    const int cfi = decode_pcfich(rx, m.mib, &conf);
    m.cfi_seen[cfi]++;
    int order[3] = {cfi > 0 ? cfi : 2, 0, 0};
    for (int c = 1, i = 1; c <= 3; c++)
        if (c != order[0]) order[i++] = c;
    std::vector<int> mis = {1};
    if (p.tdd) {
        if (sib1 && sib1->tdd_sa >= 0) mis = {std::max(0, TDD_MI[sib1->tdd_sa][rx.sf()])};
        else mis = {1, 0, 2};
    }
    const std::vector<int> sizes = {dci1a_size(m.mib.nrb, p.tdd), dci1c_size(m.mib.nrb)};
    const int ncfi = cfi > 0 && conf >= CFI_SURE ? 1 : 3;
    std::vector<float> llr;
    for (int ci = 0; ci < ncfi; ci++)
        for (int mi : mis) {
            const int c = order[ci];
            auto lit = m.layouts.find(c * 4 + mi);
            if (lit == m.layouts.end()) lit = m.layouts.emplace(c * 4 + mi, control_layout(rx.pci(), m.mib, c, mi)).first;
            const Control& ctl = lit->second;
            if (ctl.ncce <= 0) continue;
            pdcch_llr(rx, ctl, &llr);
            const auto d = dci_search(llr, ctl.ncce, sizes, SI_RNTI);
            const Dci* best = nullptr;
            for (const auto& x : d)
                if (x.agree >= MIN_AGREE && (!best || x.agree > best->agree)) best = &x;
            if (!best) continue;
            const Grant gg = parse_dci(*best, m.mib.nrb, p.tdd);
            if (!gg.ok) continue;
            *g = gg;
            *nsym = ctl.nsym;
            *text = gg.text() + ", CFI " + std::to_string(c) + ", aggregation " + std::to_string(best->L) + " at CCE " + std::to_string(best->cce);
            return true;
        }
    return false;
}

// one transmission into an accumulator; true when the block decoded (bits in *tb)
bool add_tx(const CellRx& rx, const CellMeta& m, const MetaParams& p, const Grant& g, int nsym, int rv, TbAcc& acc, std::vector<uint8_t>* tb) {
    if (acc.tbs() != g.tbs || acc.count() >= MAX_TX) acc.reset(g.tbs);
    const auto res = pdsch_res(g, m.mib.nrb, m.mib.ports, nsym, rx.sf(), rx.pci(), p.tdd);
    std::vector<float> llr;
    pdsch_llr(rx, res, m.mib.ports, SI_RNTI, &llr);
    acc.add(llr, rv);
    return acc.decode(tb);
}

std::string sibs_text(const std::vector<int>& sibs) {
    std::string s;
    for (int x : sibs) s += (s.empty() ? "SIB" : " + SIB") + (x ? std::to_string(x) : std::string("?"));
    return s;
}

}  // namespace

void meta_burst(CellRx& rx, CellMeta& m, const MetaParams& p) {
    const double t0 = now_s();
    auto over = [&] { return now_s() - t0 > p.budget_s; };
    // 1. the MIB: the frame numbers of this burst (two frames must agree the
    //    first time - a CRC16 passes by chance once in 65536 tries)
    long kref = 0;
    int sfn_ref = -1, agree = 0;
    Mib mib;
    m.mib_frames = 0;
    rx.set_ports(m.mib_ok ? m.mib.ports : 4);
    for (long k = rx.first_frame(); k <= rx.last_frame() && !over(); k++) {
        if (!rx.load(k, 0)) continue;
        m.mib_frames++;
        Mib x;
        if (!decode_pbch(rx, m.mib_ok ? m.mib.ports : p.ports_hint, &x)) continue;
        if (sfn_ref >= 0 && same_mib(x, mib) && ((x.sfn - sfn_ref - (k - kref)) % 1024 + 1024) % 1024 == 0) {
            agree++;
        } else {
            mib = x;
            sfn_ref = x.sfn;
            kref = k;
            agree = 1;
        }
        if (agree >= 2 || (agree == 1 && m.mib_ok && same_mib(x, m.mib))) break;
    }
    const bool mib_now = agree >= 2 || (agree == 1 && m.mib_ok && same_mib(mib, m.mib));
    if (!mib_now) {
        if (!m.mib_ok) m.status = "reading the MIB";
        return;
    }
    if (!m.mib_ok || !same_mib(mib, m.mib)) {
        m.news.push_back(std::string(m.mib_ok ? "MIB changed: " : "MIB: ") + mib_text(mib));
        m.version++;
        if (m.mib_ok) {   // a different carrier configuration: start over
            m.sib1_acc = TbAcc{};
            for (auto& [n, s] : m.si) s.acc = TbAcc{};
        }
        m.layouts.clear();
    }
    m.mib = mib;
    m.mib_ok = true;
    m.mib_t = p.now;
    rx.set_ports(mib.ports);
    auto sfn_of = [&](long k) { return static_cast<int>(((sfn_ref + (k - kref)) % 1024 + 1024) % 1024); };
    m.mib.sfn = sfn_of(rx.last_frame());
    // 2. SIB1: subframe 5 of even frames
    const bool sib1_due = !m.sib1_ok || p.now - m.sib1_t > p.refresh_s;
    if (sib1_due) {
        m.status = "SIB1: " + std::to_string(m.sib1_acc.count()) + " transmissions combined" +
                   (m.sib1_acc.count() ? F(", %.0f %% of its code received", 100 * m.sib1_acc.coverage()) : "");
        for (long k = rx.first_frame(); k <= rx.last_frame() && !over(); k++) {
            const int sfn = sfn_of(k);
            if (sfn % 2 || !rx.load(k, 5)) continue;
            Grant g;
            int nsym = 0;
            std::string dt;
            if (!find_dci(rx, m, p, m.sib1_ok ? &m.sib1 : nullptr, &g, &nsym, &dt)) continue;
            const int rv = g.rv >= 0 ? g.rv : rv_of((sfn / 2) % 4);
            std::vector<uint8_t> tb;
            const bool got = add_tx(rx, m, p, g, nsym, rv, m.sib1_acc, &tb);
            if (p.verbose)
                m.debug.push_back("SIB1 SFN " + std::to_string(sfn) + " RV " + std::to_string(rv) + ": " + dt + F(", coverage %.0f %%", 100 * m.sib1_acc.coverage()) +
                                  (got ? " - decoded" : ""));
            if (!got) continue;
            Sib1 s;
            SiMsg dummy;
            const int tx = m.sib1_acc.count();
            m.sib1_acc.reset(g.tbs);
            if (parse_bcch(tb, &s, &dummy) != 1) continue;
            const bool fresh = !m.sib1_ok;
            const bool changed = m.sib1_ok && s.value_tag != m.sib1.value_tag;
            if (changed) {
                // the system information changed: the SI messages are read again
                for (auto& [n, si] : m.si) si = CellMeta::Si{};
            }
            if (fresh || changed) {
                std::string pl;
                for (const auto& x : s.plmn) {
                    const std::string c = mcc_country(x.mcc);
                    pl += (pl.empty() ? "" : ", ") + x.text() + (c.empty() ? "" : " (" + c + ")");
                }
                char cid[64];
                snprintf(cid, sizeof cid, "eNB %u cell %u", s.cell_id >> 8, s.cell_id & 0xFF);
                m.news.push_back(std::string(changed ? "SIB1 changed: " : "SIB1: ") + "PLMN " + pl + ", TAC " + std::to_string(s.tac) + ", " + cid +
                                 ", band " + std::to_string(s.band) + (s.barred ? ", BARRED" : "") + " (" + std::to_string(tx) + " transmissions combined)");
                m.version++;
            }
            m.sib1 = std::move(s);
            m.sib1_ok = true;
            m.sib1_t = p.now;
            m.sib1_dci = dt;
            m.sib1_tx = tx;
            break;
        }
    }
    if (!m.sib1_ok) return;
    // 3. the SI messages in their windows
    const int w = m.sib1.si_window_ms;
    std::string waiting;
    const size_t nsi = m.sib1.sched.size();
    m.si_rr++;
    for (size_t j = 0; j < nsi && w > 0; j++) {
        // a different SI message first in each burst: none starves when the budget runs out
        const size_t n = 1 + (m.si_rr + j) % nsi;
        auto& si = m.si[static_cast<int>(n)];
        if (si.ok && p.now - si.t <= p.refresh_s) continue;
        const int T = m.sib1.sched[n - 1].period_rf;
        const int x = static_cast<int>(n - 1) * w;
        waiting += (waiting.empty() ? "" : ", ") + sibs_text(m.sib1.sched[n - 1].sibs) + " (" + std::to_string(si.acc.count()) + " tx" +
                   (si.acc.count() ? F(", %.0f %%", 100 * si.acc.coverage()) : "") + ")";
        bool done = false;
        for (long k = rx.first_frame(); k <= rx.last_frame() && !over() && !done; k++) {
            const int sfn = sfn_of(k);
            for (int sf = 0; sf < 10 && !over() && !done; sf++) {
                const int t = 10 * sfn + sf;
                const int i = ((t - x) % (10 * T) + 10 * T) % (10 * T);
                if (i >= w) continue;
                if (sf == 5 && sfn % 2 == 0) continue;   // SIB1
                if (p.tdd && m.sib1.tdd_sa >= 0 && TDD_SF[m.sib1.tdd_sa][sf] != 'D') continue;
                if (!rx.load(k, sf)) continue;
                Grant g;
                int nsym = 0;
                std::string dt;
                if (!find_dci(rx, m, p, &m.sib1, &g, &nsym, &dt)) continue;
                const int rv = g.rv >= 0 ? g.rv : rv_of(i % 4);
                std::vector<uint8_t> tb;
                const bool got = add_tx(rx, m, p, g, nsym, rv, si.acc, &tb);
                if (p.verbose)
                    m.debug.push_back("SI " + std::to_string(n) + " SFN " + std::to_string(sfn) + " sf " + std::to_string(sf) + " (window index " +
                                      std::to_string(i) + ") RV " + std::to_string(rv) + ": " + dt + F(", coverage %.0f %%", 100 * si.acc.coverage()) +
                                      (got ? " - decoded" : ""));
                if (!got) continue;
                Sib1 d1;
                SiMsg msg;
                const int tx = si.acc.count();
                si.acc.reset(g.tbs);
                if (parse_bcch(tb, &d1, &msg) != 2) continue;
                if (!si.ok) {
                    std::string first;
                    for (const auto& [k2, v] : msg.lines) {
                        if (k2.find("Neighbour") != std::string::npos || k2.find("Other LTE") != std::string::npos || k2.find("Uplink") != std::string::npos)
                            first += (first.empty() ? "" : "; ") + k2 + ": " + v;
                    }
                    m.news.push_back(sibs_text(msg.sibs) + " decoded (" + std::to_string(tx) + " transmissions combined)" + (first.empty() ? "" : ": " + first));
                    m.version++;
                }
                si.msg = std::move(msg);
                si.ok = true;
                si.t = p.now;
                si.dci = dt;
                si.tx = tx;
                done = true;
            }
        }
    }
    m.status = waiting.empty() ? "" : "waiting for " + waiting;
}

std::vector<std::pair<std::string, std::string>> meta_lines(const CellMeta& m, double carrier_hz) {
    std::vector<std::pair<std::string, std::string>> L;
    if (!m.mib_ok) return L;
    L.push_back({"MIB", mib_text(m.mib) + ", SFN " + std::to_string(m.mib.sfn)});
    // the carrier: band / EARFCN (SIB1's band, else every band holding the frequency)
    if (std::isfinite(carrier_hz) && carrier_hz > 0) {
        std::vector<int> bands;
        if (m.sib1_ok) bands.push_back(m.sib1.band);
        else bands = lte_bands_at(carrier_hz);
        std::string s;
        for (int b : bands) {
            const LteBand* lb = lte_band(b);
            const int n = lb ? lte_earfcn(*lb, carrier_hz) : -1;
            s += (s.empty() ? "" : ", ") + std::string("band ") + std::to_string(b) + (n >= 0 ? " EARFCN " + std::to_string(n) : "");
        }
        L.push_back({"Downlink", F("%.1f MHz", carrier_hz / 1e6) + (s.empty() ? "" : ", " + s)});
    }
    if (m.sib1_ok) {
        for (const auto& [k, v] : m.sib1.lines) {
            if (k == "PLMN") {
                std::string pl;
                for (const auto& x : m.sib1.plmn) {
                    const std::string c = mcc_country(x.mcc);
                    pl += (pl.empty() ? "" : ", ") + x.text() + (c.empty() ? "" : " (" + c + ")") + (x.reserved ? " reserved" : "");
                }
                L.push_back({"PLMN (MCC-MNC)", pl});
            } else {
                L.push_back({k, v});
            }
        }
        L.push_back({"SIB1 transmission", m.sib1_dci});
        for (const auto& [n, si] : m.si) {
            if (!si.ok) continue;
            for (const auto& [k, v] : si.msg.lines) {
                std::string val = v;
                if (k.find("Uplink") != std::string::npos && si.msg.ul_earfcn >= 0) {
                    const double f = lte_ul_mhz(si.msg.ul_earfcn);
                    if (f > 0) val += F(" (%.1f MHz)", f);
                }
                L.push_back({k, val});
            }
            if (!si.msg.complete) L.push_back({sibs_text(si.msg.sibs), "the SIBs after the first one not decoded here"});
        }
    }
    if (!m.status.empty()) L.push_back({"Decoding", m.status});
    return L;
}

}  // namespace cells::lte
