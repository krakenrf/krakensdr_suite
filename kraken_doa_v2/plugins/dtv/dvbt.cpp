// DVB-T (ETSI EN 300 744): scattered pilots every 12 carriers, moving 3 per
// symbol (cycle of 4), value +-4/3 from the reference PRBS w_k of the
// carrier - the same every symbol, so 4 symbols fill a comb of every 3rd
// carrier (CIR range Tu / 3: 2k 74.7 us, 8k 298.7 us at 8 MHz).
// TPS (clause 4.6): 17 / 68 carriers spread over the band (a 2.4 MHz slice
// of an 8k signal holds ~19 of them), every one carrying the same bit per
// symbol, DBPSK; 68 bits per frame: sync word, length, frame number,
// constellation, hierarchy, code rates, guard, mode, cell ID, DVB-H bits,
// BCH(67,53) parity.
// Tables: std/dvbt_tables.hpp (extracted from the standard, clause refs there).

#include "ofdm.hpp"
#include "signalling.hpp"
#include "std/dvbt_tables.hpp"

#include <cstdio>

namespace sfn {

namespace {

constexpr int TPS_BITS = 68;
// BCH(67,53), shortened from BCH(127,113): g(x) = x^14 + x^9 + x^8 + x^6 + x^5 + x^4 + x^2 + x + 1 (clause 4.6.2.12)
constexpr uint32_t TPS_BCH_POLY = 0x0377;

std::string fmtd(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

// the remainder of the 67 bits s1..s67 (info + parity) mod g(x): 0 = a codeword
uint32_t tps_bch_syndrome(const std::vector<uint8_t>& s) {
    uint32_t r = 0;
    for (int i = 1; i < TPS_BITS; i++) {
        const uint32_t fb = ((r >> 13) & 1u) ^ s[static_cast<size_t>(i)];
        r = (r << 1) & 0x3FFF;
        if (fb) r ^= TPS_BCH_POLY;
    }
    return r;
}

const char* CODE_RATE[8] = {"1/2", "2/3", "3/4", "5/6", "7/8", "?", "?", "?"};
const double CODE_RATE_V[8] = {0.5, 2.0 / 3, 0.75, 5.0 / 6, 7.0 / 8, 0, 0, 0};

struct Tps {
    int length = 0, frame = 0, constellation = 0, hierarchy = 0, cr_hp = 0, cr_lp = 0, guard = 0, mode = 0, cell_byte = 0, dvbh = 0;
    bool operator==(const Tps& o) const {
        return length == o.length && constellation == o.constellation && hierarchy == o.hierarchy && cr_hp == o.cr_hp && cr_lp == o.cr_lp &&
               guard == o.guard && mode == o.mode && dvbh == o.dvbh;
    }
};

class DvbtStd : public OfdmStandard {
public:
    std::string name() const override { return "DVB-T"; }
    std::vector<OfdmMode> modes(double bw_mhz) const override {
        std::vector<OfdmMode> v;
        for (int bw : {8, 7, 6, 5}) {
            if (bw_mhz > 0 && std::lround(bw_mhz) != bw) continue;
            for (int m8k : {0, 1}) {
                const int nfft = m8k ? dvbt_std::FFT_SIZE_8K : dvbt_std::FFT_SIZE_2K;
                for (int den : dvbt_std::GUARD_DENOMINATORS) {
                    OfdmMode m;
                    m.label = std::string(m8k ? "8k" : "2k") + " GI 1/" + std::to_string(den);
                    m.bw_mhz = bw;
                    m.fs = dvbt_std::sample_rate_hz(bw);
                    m.nfft = nfft;
                    m.ncp = dvbt_std::guard_in_T(nfft, den);
                    m.kmin = dvbt_std::KMIN;
                    m.kmax = m8k ? dvbt_std::KMAX_8K : dvbt_std::KMAX_2K;
                    m.kc = m8k ? dvbt_std::KCENTER_8K : dvbt_std::KCENTER_2K;
                    v.push_back(m);
                }
            }
        }
        return v;
    }
    std::vector<PilotPattern> patterns(const OfdmMode&) const override { return {{"", 3, 4}}; }
    kp::cf pilot_value(const OfdmMode&, const PilotPattern&, int k) const override {
        const auto v = dvbt_std::pilot_value(k);
        return kp::cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }

    // --- TPS ---
    void signal_carriers(const OfdmMode& m, std::vector<int>* sig, std::vector<int>* ref) const override {
        const bool m8k = m.nfft == dvbt_std::FFT_SIZE_8K;
        sig->clear();
        ref->clear();
        if (m8k) {
            sig->assign(dvbt_std::TPS_8K.begin(), dvbt_std::TPS_8K.end());
            ref->assign(dvbt_std::CONTINUAL_8K.begin(), dvbt_std::CONTINUAL_8K.end());
        } else {
            sig->assign(dvbt_std::TPS_2K.begin(), dvbt_std::TPS_2K.end());
            ref->assign(dvbt_std::CONTINUAL_2K.begin(), dvbt_std::CONTINUAL_2K.end());
        }
    }
    void reset_signalling() override {
        have_ = false;
        frames_ok_ = frames_bad_ = 0;
        cell_hi_ = cell_lo_ = -1;
        cell_reported_ = -2;
    }
    void signalling(const OfdmMode& m, const SigBurst& s) override {
        std::vector<float> soft;
        std::vector<char> valid;
        dbpsk_soft(s, &soft, &valid);
        // frames: the sync word on s1..s16 (either polarity: frames 1 / 3 and
        // 2 / 4), all 67 differential bits s1..s67 of the frame seen
        for (size_t j = 1; j + (TPS_BITS - 1) <= soft.size(); j++) {
            if (!all_valid(valid, j, 16)) continue;
            const uint32_t w = hard_bits(soft, j, 16);
            const int e0 = __builtin_popcount(w ^ dvbt_std::TPS_SYNC_FRAMES_1_3), e1 = __builtin_popcount(w ^ dvbt_std::TPS_SYNC_FRAMES_2_4);
            if (std::min(e0, e1) > 1) continue;
            if (!all_valid(valid, j, TPS_BITS - 1)) continue;
            std::vector<uint8_t> b(TPS_BITS, 0);
            for (int i = 1; i < TPS_BITS; i++) b[static_cast<size_t>(i)] = soft[j + static_cast<size_t>(i - 1)] > 0;
            if (tps_bch_syndrome(b) != 0) {
                frames_bad_++;
                continue;
            }
            auto field = [&](int a, int n) {
                int v = 0;
                for (int i = 0; i < n; i++) v = (v << 1) | b[static_cast<size_t>(a + i)];
                return v;
            };
            Tps t;
            t.length = field(17, 6);
            t.frame = field(23, 2);
            t.constellation = field(25, 2);
            t.hierarchy = field(27, 3);
            t.cr_hp = field(30, 3);
            t.cr_lp = field(33, 3);
            t.guard = field(36, 2);
            t.mode = field(38, 2);
            t.cell_byte = field(40, 8);
            t.dvbh = field(48, 2);
            frames_ok_++;
            // the cell ID: frames 1 and 3 carry its high byte, 2 and 4 its low byte
            if (t.length >= 31) {
                if (t.frame % 2 == 0) cell_hi_ = t.cell_byte;
                else cell_lo_ = t.cell_byte;
            }
            if (!have_ || !(t == tps_)) news_.push_back("TPS: " + summary(m, t));
            tps_ = t;
            have_ = true;
            j += TPS_BITS - 2;   // the next frame
        }
        const int cell = cell_id();
        if (have_ && cell != cell_reported_ && cell >= 0) {
            char c[48];
            snprintf(c, sizeof c, "Cell ID 0x%04X (%d)", cell, cell);
            news_.push_back(c);
        }
        cell_reported_ = cell;
    }
    std::vector<std::pair<std::string, std::string>> info(const OfdmMode& m) const override {
        std::vector<std::pair<std::string, std::string>> L;
        if (!have_) {
            L.push_back({"TPS", frames_bad_ ? "BCH check failed in " + std::to_string(frames_bad_) + " frames" : "reading"});
            return L;
        }
        const Tps& t = tps_;
        static const char* CONST[4] = {"QPSK", "16-QAM", "64-QAM", "reserved"};
        static const char* GI[4] = {"1/32", "1/16", "1/8", "1/4"};
        static const char* MODE[4] = {"2k", "8k", "4k", "reserved"};
        L.push_back({"TPS", "decoded, BCH ok (" + std::to_string(frames_ok_) + " frames)"});
        L.push_back({"Constellation", CONST[t.constellation & 3]});
        std::string h;
        const int a = t.hierarchy & 3;
        if (a == 0) h = "non-hierarchical";
        else h = "hierarchical, alpha = " + std::to_string(a == 1 ? 1 : a == 2 ? 2 : 4);
        if (t.hierarchy & 4) h += ", in-depth interleaver";
        L.push_back({"Hierarchy", h});
        L.push_back({"Code rate", std::string(CODE_RATE[t.cr_hp & 7]) + (a ? std::string(" (HP), ") + CODE_RATE[t.cr_lp & 7] + " (LP)" : "")});
        L.push_back({"Guard interval", GI[t.guard & 3]});
        L.push_back({"Transmission mode", MODE[t.mode & 3]});
        const int cell = cell_id();
        char cs[48];
        snprintf(cs, sizeof cs, "0x%04X (%d)", cell, cell);
        L.push_back({"Cell ID", t.length < 31 ? "not transmitted" : cell >= 0 ? cs : "reading (both halves needed)"});
        if (t.length >= 33) L.push_back({"DVB-H", std::string("time slicing ") + (t.dvbh & 2 ? "on" : "off") + ", MPE-FEC " + (t.dvbh & 1 ? "on" : "off")});
        L.push_back({"Net bit rate", bitrate(m, t)});
        return L;
    }
    std::vector<std::string> take_news() override {
        auto n = std::move(news_);
        news_.clear();
        return n;
    }

private:
    bool have_ = false;
    Tps tps_;
    int frames_ok_ = 0, frames_bad_ = 0;
    int cell_hi_ = -1, cell_lo_ = -1, cell_reported_ = -2;
    std::vector<std::string> news_;

    int cell_id() const { return cell_hi_ >= 0 && cell_lo_ >= 0 ? (cell_hi_ << 8) | cell_lo_ : -1; }

    // net bit rate: data carriers x bits x code rate x 188 / 204 / symbol duration
    static std::string bitrate(const OfdmMode& m, const Tps& t) {
        const int nd = (t.mode & 3) == 1 ? 6048 : (t.mode & 3) == 2 ? 3024 : 1512;
        static const int GDEN[4] = {32, 16, 8, 4};
        const double tu = static_cast<double>(m.nfft) / m.fs;   // the mode found (2k / 8k at the channel width)
        const double ts = tu * (1 + 1.0 / GDEN[t.guard & 3]);
        const int bpc = 2 * (1 + (t.constellation & 3));
        if (bpc > 6) return "?";
        auto rate = [&](int bits, int cr) { return nd * bits * CODE_RATE_V[cr & 7] * 188.0 / 204.0 / ts / 1e6; };
        if ((t.hierarchy & 3) == 0) return fmtd("%.2f Mbit/s", rate(bpc, t.cr_hp));
        return fmtd("%.2f Mbit/s (HP)", rate(2, t.cr_hp)) + fmtd(" + %.2f Mbit/s (LP)", rate(bpc - 2, t.cr_lp));
    }
    static std::string summary(const OfdmMode& m, const Tps& t) {
        static const char* CONST[4] = {"QPSK", "16-QAM", "64-QAM", "reserved"};
        static const char* GI[4] = {"1/32", "1/16", "1/8", "1/4"};
        static const char* MODE[4] = {"2k", "8k", "4k", "reserved"};
        return std::string(CONST[t.constellation & 3]) + " " + CODE_RATE[t.cr_hp & 7] + ((t.hierarchy & 3) ? " (hierarchical)" : "") + ", GI " +
               GI[t.guard & 3] + ", " + MODE[t.mode & 3] + ", " + bitrate(m, t);
    }
};

}  // namespace

std::unique_ptr<Estimator> make_dvbt(kp::Host& h) { return std::make_unique<OfdmEstimator>(h, std::make_unique<DvbtStd>()); }

}  // namespace sfn
