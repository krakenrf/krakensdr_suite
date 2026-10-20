#include "lte_rrc.hpp"

#include <cstdio>

namespace cells::lte {

namespace {

// unaligned PER reader; reading past the end sets bad
struct Bits {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    bool bad = false;
    explicit Bits(const std::vector<uint8_t>& v) : b(v) {}
    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            if (p >= b.size()) {
                bad = true;
                return v;
            }
            v = (v << 1) | (b[p++] & 1u);
        }
        return v;
    }
    bool bit() { return u(1) != 0; }
    // constrained whole number lb..ub
    int range(int lb, int ub) {
        const unsigned n = static_cast<unsigned>(ub - lb + 1);
        int bits = 0;
        while ((1u << bits) < n) bits++;
        return lb + static_cast<int>(u(bits));
    }
    // ENUMERATED of n root values (+ extension marker): the index (-1 = an extension value)
    int en(int n, bool ext = false) {
        if (ext && bit()) {
            small();
            return -1;
        }
        return range(0, n - 1);
    }
    // normally small non-negative whole number
    int small() {
        if (!bit()) return static_cast<int>(u(6));
        return static_cast<int>(length());
    }
    // length determinant (unconstrained)
    uint32_t length() {
        if (!bit()) return u(7);
        if (!bit()) return u(14);
        bad = true;   // fragmented: not in broadcast messages
        return 0;
    }
    void skip(size_t n) {
        p += n;
        if (p > b.size()) bad = true;
    }
    // the extension additions of an extensible SEQUENCE whose extension bit was set: skipped
    void skip_additions() {
        const int n = small() + 1;
        std::vector<bool> present(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) present[static_cast<size_t>(i)] = bit();
        for (int i = 0; i < n && !bad; i++)
            if (present[static_cast<size_t>(i)]) skip(8u * length());
    }
    // OCTET STRING (unconstrained) -> its bits
    std::vector<uint8_t> octets() {
        const uint32_t n = length();
        std::vector<uint8_t> v;
        for (uint32_t i = 0; i < 8 * n && !bad; i++) v.push_back(static_cast<uint8_t>(u(1)));
        return v;
    }
};

std::string S(int v) { return std::to_string(v); }
std::string F(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

using Lines = std::vector<std::pair<std::string, std::string>>;

const int RESEL_DB = 2;   // ReselectionThreshold: value x 2 dB
const int QOFF[31] = {-24, -22, -20, -18, -16, -14, -12, -10, -8, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
const int NRB6[6] = {6, 15, 25, 50, 75, 100};

int q_offset(Bits& r) {
    const int i = r.range(0, 30);
    return QOFF[i];
}

std::string speed_sf(Bits& r) {
    static const char* t[4] = {"0.25", "0.5", "0.75", "1.0"};
    const int m = r.range(0, 3), h = r.range(0, 3);
    return std::string("medium x") + t[m] + ", high x" + t[h];
}

// PhysCellIdRange -> "start" or "start..end"
std::string pci_range(Bits& r) {
    static const int RNG[14] = {4, 8, 12, 16, 24, 32, 48, 64, 84, 96, 128, 168, 252, 504};
    const bool has = r.bit();
    const int start = r.range(0, 503);
    if (!has) return S(start);
    const int i = r.range(0, 15);
    return i < 14 ? S(start) + "-" + S(start + RNG[i] - 1) : S(start);
}

void ac_barring(Bits& r, const std::string& what, Lines* L) {
    static const char* FAC[16] = {"0", "5", "10", "15", "20", "25", "30", "40", "50", "60", "70", "75", "80", "85", "90", "95"};
    static const int TIME[8] = {4, 8, 16, 32, 64, 128, 256, 512};
    const int f = r.range(0, 15), t = r.range(0, 7);
    const int sp = static_cast<int>(r.u(5));
    char hx[8];
    snprintf(hx, sizeof hx, "%02x", sp);   // not F(): an integer format
    L->push_back({what, std::string("factor ") + FAC[f] + " %, " + S(TIME[t]) + " s" + (sp ? ", special AC 0x" + std::string(hx) : "")});
}

// --- SIB1 ---

bool sib1(Bits& r, Sib1* s) {
    const bool has_pmax = r.bit(), has_tdd = r.bit(), has_nce = r.bit();
    // cellAccessRelatedInfo
    const bool has_csg = r.bit();
    const int nplmn = r.range(1, 6);
    int mcc = -1;
    for (int i = 0; i < nplmn; i++) {
        Plmn p;
        if (r.bit()) {
            mcc = 0;
            for (int d = 0; d < 3; d++) mcc = mcc * 10 + r.range(0, 9);   // 4 bits each, 0..9 (values 10..15 never sent)
        }
        p.mcc = mcc;
        p.mnc_digits = r.range(2, 3);
        p.mnc = 0;
        for (int d = 0; d < p.mnc_digits; d++) p.mnc = p.mnc * 10 + static_cast<int>(r.u(4));
        p.reserved = r.u(1) == 0;
        s->plmn.push_back(p);
    }
    s->tac = static_cast<int>(r.u(16));
    s->cell_id = r.u(28);
    s->barred = r.u(1) == 0;
    s->intra_resel_allowed = r.u(1) == 0;
    s->csg = r.bit();
    if (has_csg) s->csg_id = static_cast<long>(r.u(27));
    // cellSelectionInfo
    const bool has_qoff = r.bit();
    s->q_rxlevmin_dbm = 2 * r.range(-70, -22);
    if (has_qoff) s->q_rxlevmin_offset_db = 2 * r.range(1, 8);
    if (has_pmax) s->p_max_dbm = r.range(-30, 33);
    s->band = r.range(1, 64);
    const int nsched = r.range(1, 32);
    static const int PER[7] = {8, 16, 32, 64, 128, 256, 512};
    static const int EXT_SIB[13] = {19, 20, 21, 24, 25, 26, 26, 27, 28, 29, 30, 31, 32};
    for (int i = 0; i < nsched && !r.bad; i++) {
        SchedInfo si;
        si.period_rf = PER[r.range(0, 6)];
        if (i == 0) si.sibs.push_back(2);
        const int nm = r.range(0, 31);
        for (int j = 0; j < nm && !r.bad; j++) {
            if (r.bit()) {
                const int e = r.small();
                si.sibs.push_back(e < 13 ? EXT_SIB[e] : 0);
            } else {
                si.sibs.push_back(3 + r.range(0, 15));
            }
        }
        s->sched.push_back(si);
    }
    if (has_tdd) {
        s->tdd_sa = r.range(0, 6);
        s->tdd_ssp = r.range(0, 8);
    }
    static const int WIN[7] = {1, 2, 5, 10, 15, 20, 40};
    s->si_window_ms = WIN[r.range(0, 6)];
    s->value_tag = r.range(0, 31);
    if (has_nce && !r.bad) {
        // v890: lateNonCriticalExtension (v8h0 / v9e0: multi-band), v920
        const bool has_late = r.bit(), has_v920 = r.bit();
        if (has_late) {
            const auto late = r.octets();
            Bits q(late);
            const bool has_mb = q.bit(), has_v9e0 = q.bit();
            if (has_mb) {
                const int n = q.range(1, 8);
                for (int i = 0; i < n; i++) s->multi_band.push_back(q.range(1, 64));
            }
            if (has_v9e0) {
                const bool has_fb = q.bit(), has_mb2 = q.bit();
                q.bit();   // nonCriticalExtension (v10j0)
                if (has_fb) s->band = q.range(65, 256);
                if (has_mb2) {
                    const int n = q.range(1, 8);
                    for (int i = 0; i < n && !q.bad; i++) {
                        // MultiBandInfo-v9e0 ::= SEQUENCE { freqBandIndicator-v9e0 OPTIONAL }
                        if (q.bit()) {
                            const int b = q.range(65, 256);
                            if (static_cast<size_t>(i) < s->multi_band.size()) s->multi_band[static_cast<size_t>(i)] = b;
                        }
                    }
                }
            }
        }
        if (has_v920 && !r.bad) {
            const bool has_ims = r.bit(), has_csi = r.bit();
            r.bit();   // nonCriticalExtension (v1130): not decoded
            s->ims_emergency = has_ims;
            if (has_csi) {
                const bool has_off = r.bit();
                s->q_qualmin_db = r.range(-34, -3);
                if (has_off) r.range(1, 8);
            }
        }
    }
    if (r.bad) return false;
    // display lines
    Lines& L = s->lines;
    std::string pl;
    for (const auto& p : s->plmn) pl += (pl.empty() ? "" : ", ") + p.text() + (p.reserved ? " (reserved for operator use)" : "");
    L.push_back({"PLMN", pl});
    char tachx[16];
    snprintf(tachx, sizeof tachx, " (0x%04X)", static_cast<unsigned>(s->tac));   // not F(): an integer format
    L.push_back({"Tracking area code", S(s->tac) + tachx});
    char cid[96];
    snprintf(cid, sizeof cid, "%u (0x%07X): eNB %u, cell %u", s->cell_id, s->cell_id, s->cell_id >> 8, s->cell_id & 0xFF);
    L.push_back({"Cell identity", cid});
    L.push_back({"Cell barred", s->barred ? "yes" : "no"});
    L.push_back({"Intra-frequency reselection", s->intra_resel_allowed ? "allowed" : "not allowed"});
    if (s->csg) L.push_back({"Closed subscriber group", s->csg_id >= 0 ? "yes, ID " + std::to_string(s->csg_id) : "yes"});
    L.push_back({"Min. RX level (q-RxLevMin)", S(s->q_rxlevmin_dbm) + " dBm" + (s->q_rxlevmin_offset_db ? ", offset " + S(s->q_rxlevmin_offset_db) + " dB" : "")});
    if (s->q_qualmin_db > -999) L.push_back({"Min. quality (q-QualMin)", S(s->q_qualmin_db) + " dB"});
    if (s->p_max_dbm > -999) L.push_back({"Max. UE power (p-Max)", S(s->p_max_dbm) + " dBm"});
    std::string band = S(s->band);
    for (int b : s->multi_band) band += ", " + S(b);
    L.push_back({s->multi_band.empty() ? "Band" : "Bands", band});
    if (s->tdd_sa >= 0) L.push_back({"TDD configuration", "UL/DL " + S(s->tdd_sa) + ", special subframe " + S(s->tdd_ssp)});
    std::string sch;
    for (size_t i = 0; i < s->sched.size(); i++) {
        std::string sibs;
        for (int x : s->sched[i].sibs) sibs += (sibs.empty() ? "SIB" : "+") + (x ? S(x) : std::string("?"));
        sch += (sch.empty() ? "" : ", ") + (sibs.empty() ? std::string("none") : sibs) + " every " + S(s->sched[i].period_rf * 10) + " ms";
    }
    L.push_back({"SI schedule", sch + "; window " + S(s->si_window_ms) + " ms"});
    L.push_back({"SI value tag", S(s->value_tag)});
    if (s->ims_emergency) L.push_back({"IMS emergency calls", "supported"});
    return true;
}

// --- SIB2 ---

bool sib2(Bits& r, SiMsg* m) {
    Lines& L = m->lines;
    const bool ext = r.bit();
    const bool has_ac = r.bit(), has_mbsfn = r.bit();
    if (has_ac) {
        const bool s = r.bit(), d = r.bit();
        const bool emerg = r.bit();
        L.push_back({"Access barring: emergency", emerg ? "barred" : "allowed"});
        if (s) ac_barring(r, "Access barring: signalling", &L);
        if (d) ac_barring(r, "Access barring: data", &L);
    }
    // radioResourceConfigCommon
    const bool rext = r.bit();
    {
        // rach-ConfigCommon
        const bool kext = r.bit();
        const bool has_ga = r.bit();
        const int npre = 4 * (1 + r.range(0, 15));
        std::string rach = S(npre) + " preambles";
        if (has_ga) {
            const bool gext = r.bit();
            const int ga = 4 * (1 + r.range(0, 14));
            r.range(0, 3);
            r.range(0, 7);
            rach += " (group A " + S(ga) + ")";
            if (gext) r.skip_additions();
        }
        const int step = 2 * r.range(0, 3);
        const int target = -120 + 2 * r.range(0, 15);
        static const int TMAX[11] = {3, 4, 5, 6, 7, 8, 10, 20, 50, 100, 200};
        const int tmax = TMAX[r.range(0, 10) % 11];
        static const int WIN[8] = {2, 3, 4, 5, 6, 7, 8, 10};
        const int win = WIN[r.range(0, 7)];
        const int crt = 8 * (1 + r.range(0, 7));
        const int msg3 = r.range(1, 8);
        rach += ", target " + S(target) + " dBm, ramping " + S(step) + " dB, max " + S(tmax) + " tries, response window " + S(win) +
                " ms, contention timer " + S(crt) + " ms, msg3 max " + S(msg3) + " tx";
        if (kext) r.skip_additions();
        L.push_back({"RACH", rach});
        // bcch-Config, pcch-Config
        const int mod = 2 << r.range(0, 3);
        static const char* NB[8] = {"4T", "2T", "T", "T/2", "T/4", "T/8", "T/16", "T/32"};
        const int cyc = 32 << r.range(0, 3);
        const int nb = r.range(0, 7);
        L.push_back({"Paging", "cycle " + S(cyc * 10) + " ms, nB " + NB[nb] + "; SI modification period " + S(mod) + " x paging cycle"});
        // prach-Config
        const int root = r.range(0, 837);
        const int pidx = r.range(0, 63);
        const bool hs = r.bit();
        const int zcz = r.range(0, 15);
        const int foff = r.range(0, 94);
        L.push_back({"PRACH", "root sequence " + S(root) + ", config index " + S(pidx) + ", zero correlation zone " + S(zcz) + ", frequency offset " +
                                  S(foff) + " PRB" + (hs ? ", high speed" : "")});
        // pdsch-ConfigCommon
        const int rsp = r.range(-60, 50);
        const int pb = r.range(0, 3);
        L.push_back({"Reference signal power", S(rsp) + " dBm per RE (p-b " + S(pb) + ")"});
        // pusch-ConfigCommon
        const int nsb = r.range(1, 4);
        const bool hm = r.bit();
        const int hoff = r.range(0, 98);
        const bool q64 = r.bit();
        const bool gh = r.bit();
        const int ga = r.range(0, 29);
        const bool sh = r.bit();
        const int cs = r.range(0, 7);
        L.push_back({"PUSCH", S(nsb) + " sub-band" + (nsb > 1 ? "s" : "") + ", hopping " + (hm ? "intra+inter" : "inter") + " offset " + S(hoff) +
                                  (q64 ? ", 64QAM" : "") + (gh ? ", group hopping" : "") + (sh ? ", sequence hopping" : "") + ", group " + S(ga) +
                                  ", cyclic shift " + S(cs)});
        // pucch-ConfigCommon
        const int ds = 1 + r.range(0, 2);
        const int cqi = r.range(0, 98);
        const int ncs = r.range(0, 7);
        const int n1 = r.range(0, 2047);
        L.push_back({"PUCCH", "delta shift " + S(ds) + ", CQI RBs " + S(cqi) + ", nCS-AN " + S(ncs) + ", n1PUCCH-AN " + S(n1)});
        // soundingRS-UL-ConfigCommon
        if (r.bit()) {
            const bool up = r.bit();
            const int bw = r.range(0, 7), sc = r.range(0, 15);
            const bool an = r.bit();
            L.push_back({"SRS", "bandwidth config " + S(bw) + ", subframe config " + S(sc) + (an ? ", ACK/NACK simultaneous" : "") + (up ? ", max UpPTS" : "")});
        } else {
            L.push_back({"SRS", "off"});
        }
        // uplinkPowerControlCommon
        const int p0 = r.range(-126, 24);
        static const char* AL[8] = {"0", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9", "1"};
        const int al = r.range(0, 7);
        const int p0c = r.range(-127, -96);
        for (int i = 0; i < 5; i++) r.range(0, i == 2 ? 3 : 2);
        const int dmsg3 = r.range(-1, 6);
        L.push_back({"UL power control", "P0 PUSCH " + S(p0) + " dBm, alpha " + AL[al] + ", P0 PUCCH " + S(p0c) + " dBm, msg3 delta " + S(2 * dmsg3) + " dB"});
        const int cp = r.range(0, 1);
        if (cp) L.push_back({"UL cyclic prefix", "extended"});
    }
    if (rext) r.skip_additions();
    // ue-TimersAndConstants
    {
        const bool text = r.bit();
        static const int T300[8] = {100, 200, 300, 400, 600, 1000, 1500, 2000};
        static const int T310[7] = {0, 50, 100, 200, 500, 1000, 2000};
        static const int N310[8] = {1, 2, 3, 4, 6, 8, 10, 20};
        static const int T311[7] = {1000, 3000, 5000, 10000, 15000, 20000, 30000};
        static const int N311[8] = {1, 2, 3, 4, 5, 6, 8, 10};
        const int t300 = T300[r.range(0, 7)], t301 = T300[r.range(0, 7)], t310 = T310[r.range(0, 6) % 7];
        const int n310 = N310[r.range(0, 7)], t311 = T311[r.range(0, 6) % 7], n311 = N311[r.range(0, 7)];
        L.push_back({"UE timers", "T300 " + S(t300) + " ms, T301 " + S(t301) + " ms, T310 " + S(t310) + " ms, N310 " + S(n310) + ", T311 " +
                                      S(t311) + " ms, N311 " + S(n311)});
        if (text) r.skip_additions();
    }
    // freqInfo
    {
        const bool has_ul = r.bit(), has_bw = r.bit();
        if (has_ul) m->ul_earfcn = static_cast<int>(r.u(16));
        if (has_bw) m->ul_nrb = NRB6[r.range(0, 5) % 6];
        const int ase = r.range(1, 32);
        L.push_back({"Uplink", (has_ul ? "EARFCN " + S(m->ul_earfcn) : std::string("default duplex spacing")) +
                                   (has_bw ? ", " + S(m->ul_nrb) + " RB" : std::string(", same bandwidth")) + ", additional emission NS_" +
                                   F("%02.0f", ase)});
    }
    if (has_mbsfn) {
        const int n = r.range(1, 8);
        std::string mb;
        static const int PER[6] = {1, 2, 4, 8, 16, 32};
        for (int i = 0; i < n && !r.bad; i++) {
            const int per = PER[r.range(0, 5) % 6], off = r.range(0, 7);
            const bool four = r.bit();
            const uint32_t sfs = r.u(four ? 24 : 6);
            char b[80];
            snprintf(b, sizeof b, "every %d frame%s offset %d, subframes 0x%X", per, per > 1 ? "s" : "", off, sfs);
            mb += (mb.empty() ? "" : "; ") + std::string(b);
        }
        L.push_back({"MBSFN subframes", mb});
    }
    static const char* TAT[8] = {"500 ms", "750 ms", "1280 ms", "1920 ms", "2560 ms", "5120 ms", "10240 ms", "infinity"};
    L.push_back({"Time alignment timer", TAT[r.range(0, 7)]});
    if (ext) r.skip_additions();
    return !r.bad;
}

// --- SIB3 ---

bool sib3(Bits& r, SiMsg* m) {
    Lines& L = m->lines;
    const bool ext = r.bit();
    static const int QH[16] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24};
    const bool has_speed = r.bit();
    const int qh = QH[r.range(0, 15)];
    std::string sp;
    if (has_speed) {
        static const int TE[8] = {30, 60, 120, 180, 240, 0, 0, 0};
        const int te = TE[r.range(0, 7)], th = TE[r.range(0, 7)];
        const int ncm = r.range(1, 16), nch = r.range(1, 16);
        static const int SF[4] = {-6, -4, -2, 0};
        const int sm = SF[r.range(0, 3)], sh = SF[r.range(0, 3)];
        sp = ", speed states: " + S(ncm) + " / " + S(nch) + " changes in " + S(te) + " s (hyst. " + S(th) + " s), q-Hyst " + S(sm) + " / " + S(sh) + " dB";
    }
    L.push_back({"Reselection hysteresis", S(qh) + " dB" + sp});
    const bool has_sni = r.bit();
    const int sni = has_sni ? RESEL_DB * r.range(0, 31) : -1;
    const int tsl = RESEL_DB * r.range(0, 31);
    const int prio = r.range(0, 7);
    L.push_back({"Serving frequency", "priority " + S(prio) + ", thresh. serving low " + S(tsl) + " dB" +
                                          (has_sni ? ", inter-freq. search below " + S(sni) + " dB" : "")});
    const bool has_pm = r.bit(), has_sis = r.bit(), has_mbw = r.bit(), has_tsf = r.bit();
    const int qrx = 2 * r.range(-70, -22);
    const int pm = has_pm ? r.range(-30, 33) : 0;
    const int sis = has_sis ? RESEL_DB * r.range(0, 31) : -1;
    const int mbw = has_mbw ? NRB6[r.range(0, 5) % 6] : 0;
    const bool ap1 = r.bit();
    const int ncc = static_cast<int>(r.u(2));
    const int tre = r.range(0, 7);
    std::string tsf;
    if (has_tsf) tsf = " (" + speed_sf(r) + ")";
    L.push_back({"Intra-frequency reselection", "q-RxLevMin " + S(qrx) + " dBm" + (has_pm ? ", p-Max " + S(pm) + " dBm" : "") +
                                                    (has_sis ? ", search below " + S(sis) + " dB" : "") + (has_mbw ? ", meas. bandwidth " + S(mbw) + " RB" : "") +
                                                    ", T-reselection " + S(tre) + " s" + tsf + (ap1 ? ", port 1 on neighbours" : "") +
                                                    ", neighbour config " + S(ncc)});
    if (ext) r.skip_additions();
    return !r.bad;
}

// --- SIB4 ---

bool sib4(Bits& r, SiMsg* m) {
    Lines& L = m->lines;
    const bool ext = r.bit();
    const bool has_n = r.bit(), has_b = r.bit(), has_csg = r.bit();
    if (has_n) {
        const int n = r.range(1, 16);
        std::string s;
        for (int i = 0; i < n && !r.bad; i++) {
            const bool e = r.bit();
            const int pci = r.range(0, 503);
            const int off = q_offset(r);
            if (e) r.skip_additions();
            m->intra_pci.push_back(pci);
            s += (s.empty() ? "" : ", ") + S(pci) + (off ? " (" + F("%+.0f", off) + " dB)" : "");
        }
        L.push_back({"Neighbour cells (same carrier)", "PCI " + s});
    }
    if (has_b) {
        const int n = r.range(1, 16);
        std::string s;
        for (int i = 0; i < n && !r.bad; i++) s += (s.empty() ? "" : ", ") + pci_range(r);
        L.push_back({"Blacklisted cells", "PCI " + s});
    }
    if (has_csg) L.push_back({"CSG cells", "PCI " + pci_range(r)});
    if (ext) r.skip_additions();
    return !r.bad;
}

// --- SIB5 ---

bool sib5(Bits& r, SiMsg* m) {
    Lines& L = m->lines;
    const bool ext = r.bit();
    const int n = r.range(1, 8);
    for (int i = 0; i < n && !r.bad; i++) {
        const bool e = r.bit();
        const bool has_pm = r.bit(), has_tsf = r.bit(), has_prio = r.bit(), has_qo = r.bit(), has_nl = r.bit(), has_bl = r.bit();
        const int earfcn = static_cast<int>(r.u(16));
        const int qrx = 2 * r.range(-70, -22);
        const int pm = has_pm ? r.range(-30, 33) : 0;
        const int tre = r.range(0, 7);
        if (has_tsf) speed_sf(r);
        const int thi = RESEL_DB * r.range(0, 31), tlo = RESEL_DB * r.range(0, 31);
        const int mbw = NRB6[r.range(0, 5) % 6];
        r.bit();
        const int prio = has_prio ? r.range(0, 7) : -1;
        r.u(2);
        const int qo = has_qo ? q_offset(r) : 0;
        std::string s = "EARFCN " + S(earfcn) + ", " + S(mbw) + " RB, priority " + (prio >= 0 ? S(prio) : std::string("-")) + ", q-RxLevMin " + S(qrx) +
                        " dBm" + (has_pm ? ", p-Max " + S(pm) + " dBm" : "") + ", thresholds " + S(thi) + " / " + S(tlo) + " dB, T-reselection " + S(tre) +
                        " s" + (qo ? ", offset " + F("%+.0f", qo) + " dB" : "");
        if (has_nl) {
            const int k = r.range(1, 16);
            std::string c;
            for (int j = 0; j < k && !r.bad; j++) {
                const int pci = r.range(0, 503);
                const int off = q_offset(r);
                c += (c.empty() ? "" : ", ") + S(pci) + (off ? " (" + F("%+.0f", off) + " dB)" : "");
            }
            s += "; cells " + c;
        }
        if (has_bl) {
            const int k = r.range(1, 16);
            std::string c;
            for (int j = 0; j < k && !r.bad; j++) c += (c.empty() ? "" : ", ") + pci_range(r);
            s += "; blacklisted " + c;
        }
        if (e) r.skip_additions();
        m->inter_earfcn.push_back(earfcn);
        L.push_back({"Other LTE carrier " + S(i + 1), s});
    }
    if (ext) r.skip_additions();
    return !r.bad;
}

bool si_message(Bits& r, SiMsg* m) {
    if (r.bit()) return false;   // criticalExtensionsFuture
    r.bit();                     // nonCriticalExtension present (after the list; not decoded)
    const int n = r.range(1, 32);
    static const int EXT_SIB[20] = {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 24, 25, 26, 26, 27, 28, 29, 30, 31, 32};
    for (int i = 0; i < n && !r.bad; i++) {
        if (r.bit()) {
            // an extension alternative: an open type - skipped
            const int e = r.small();
            m->sibs.push_back(e < 20 ? EXT_SIB[e] : 0);
            r.skip(8u * r.length());
            continue;
        }
        const int sib = 2 + r.range(0, 9);
        m->sibs.push_back(sib);
        const size_t before = m->lines.size();
        bool ok = false;
        switch (sib) {
        case 2: ok = sib2(r, m); break;
        case 3: ok = sib3(r, m); break;
        case 4: ok = sib4(r, m); break;
        case 5: ok = sib5(r, m); break;
        default:
            // not decoded: nothing after it can be found
            m->complete = false;
            return true;
        }
        if (!ok) return false;
        for (size_t j = before; j < m->lines.size(); j++) m->lines[j].first = "SIB" + std::to_string(sib) + " \xC2\xB7 " + m->lines[j].first;
    }
    return !r.bad;
}

}  // namespace

std::string Plmn::text() const {
    char b[32];
    if (mnc_digits == 3) snprintf(b, sizeof b, "%03d-%03d", mcc, mnc);
    else snprintf(b, sizeof b, "%03d-%02d", mcc, mnc);
    return b;
}

int parse_bcch(const std::vector<uint8_t>& bits, Sib1* s1, SiMsg* si) {
    Bits r(bits);
    if (r.bit()) return 0;   // messageClassExtension
    if (r.bit()) {
        Sib1 s;
        if (!sib1(r, &s)) return 0;
        *s1 = std::move(s);
        return 1;
    }
    SiMsg m;
    if (!si_message(r, &m)) return 0;
    *si = std::move(m);
    return 2;
}

}  // namespace cells::lte
