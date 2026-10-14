// ISDB-T (ARIB STD-B31): 13 segments of 108 / 216 / 432 carriers (modes 1 /
// 2 / 3) + one continual carrier at the top; scattered pilots every 12
// carriers moving 3 per symbol like DVB-T, value +-4/3 from a PRBS that runs
// continuously over the whole band (from all ones at the lowest carrier: the
// standard's per-segment initial values, Table 3-16, are this sequence's
// states - std/isdbt_tables.hpp checked them). Elementary rate 512/63 MHz for
// 6 MHz channels (scaled for 7 / 8 MHz).
// TMCC (clause 3.15): 1 / 2 / 4 carriers per synchronous segment, DBPSK, one
// bit per symbol, 204 per frame: sync word, segment type, then system,
// emergency warning flag, partial reception and per hierarchical layer A /
// B / C its modulation, code rate, time interleaving and segment count
// (current + next), parity. Every segment is assumed synchronous (the
// pilot engine needs scattered pilots - every broadcaster uses them).

#include "ofdm.hpp"
#include "signalling.hpp"
#include "std/isdbt_tables.hpp"

#include <cstdio>

namespace sfn {

namespace {

constexpr int FRAME = isdbt_std::kSymbolsPerFrame;   // 204
constexpr int INFO_END = 121;                        // B20..B121 = the TMCC information

std::string fmtd(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

uint32_t word16(const char* s) {
    uint32_t v = 0;
    for (int i = 0; i < 16; i++) v = (v << 1) | static_cast<uint32_t>(s[i] - '0');
    return v;
}

struct Layer {
    int mod = 7, cr = 7, il = 7, seg = 15;
    bool used() const { return mod != 7 && seg >= 1 && seg <= 13; }
    bool operator==(const Layer& o) const { return mod == o.mod && cr == o.cr && il == o.il && seg == o.seg; }
};
struct Tmcc {
    int system = 0, countdown = 15;
    bool ews = false, partial = false, next_partial = false;
    Layer cur[3], next[3];
    bool operator==(const Tmcc& o) const {
        for (int i = 0; i < 3; i++)
            if (!(cur[i] == o.cur[i]) || !(next[i] == o.next[i])) return false;
        return system == o.system && countdown == o.countdown && ews == o.ews && partial == o.partial && next_partial == o.next_partial;
    }
};

const char* MOD[8] = {"DQPSK", "QPSK", "16-QAM", "64-QAM", "?", "?", "?", "unused"};
const char* CR[8] = {"1/2", "2/3", "3/4", "5/6", "7/8", "?", "?", "unused"};
const double CR_V[8] = {0.5, 2.0 / 3, 0.75, 5.0 / 6, 7.0 / 8, 0, 0, 0};

class IsdbtStd : public OfdmStandard {
public:
    std::string name() const override { return "ISDB-T"; }
    std::vector<OfdmMode> modes(double bw_mhz) const override {
        std::vector<OfdmMode> v;
        for (int bw : {6, 7, 8}) {
            if (bw_mhz > 0 && std::lround(bw_mhz) != bw) continue;
            for (int mode = 1; mode <= 3; mode++)
                for (int den : isdbt_std::kGuardRatioDenominators) {
                    OfdmMode m;
                    m.label = "mode " + std::to_string(mode) + " GI 1/" + std::to_string(den);
                    m.bw_mhz = bw;
                    m.fs = isdbt_std::kIfftSampleRateHz * bw / 6.0;
                    m.nfft = isdbt_std::fft_size(mode);
                    m.ncp = isdbt_std::guard_samples(mode, den);
                    m.kmin = 0;
                    m.kmax = isdbt_std::total_carriers(mode) - 1;
                    m.kc = isdbt_std::center_carrier(mode);
                    v.push_back(m);
                }
        }
        return v;
    }
    std::vector<PilotPattern> patterns(const OfdmMode&) const override { return {{"", 3, 4}}; }
    kp::cf pilot_value(const OfdmMode& m, const PilotPattern&, int k) const override {
        const auto& w = band(mode_of(m));
        if (k < 0 || k >= static_cast<int>(w.size())) return kp::cf(0, 0);
        return kp::cf(w[static_cast<size_t>(k)] ? -4.0f / 3 : 4.0f / 3, 0);
    }

    // --- TMCC ---
    void signal_carriers(const OfdmMode& m, std::vector<int>* sig, std::vector<int>* ref) const override {
        const int mode = mode_of(m);
        sig->clear();
        ref->clear();
        for (int seg = 0; seg < isdbt_std::kNumSegments; seg++)
            for (int c : isdbt_std::tmcc_carriers(mode, seg, false)) sig->push_back(isdbt_std::segment_first_carrier(mode, seg) + c);
    }
    // the information (B1..B121) from any symbol of a frame on: 204 + 121
    int signalling_symbols(const OfdmMode&) const override { return FRAME + INFO_END + 1; }
    void reset_signalling() override {
        have_ = false;
        frames_ok_ = frames_bad_ = 0;
        ews_reported_ = false;
    }
    void signalling(const OfdmMode& m, const SigBurst& s) override {
        (void)m;
        std::vector<float> soft;
        std::vector<char> valid;
        dbpsk_soft(s, &soft, &valid);
        static const uint32_t W0 = word16(isdbt_std::kTmccSyncW0), W1 = word16(isdbt_std::kTmccSyncW1);
        // soft[j] = bit Bk of the symbol j = frame symbol k: B1 at j. Without
        // reference carriers the polarity is unknown: W0 / W1 are each other's
        // complement, the segment type (000 = synchronous) settles it
        for (size_t j = 1; j + INFO_END <= soft.size(); j++) {
            if (!all_valid(valid, j, 16)) continue;
            const uint32_t w = hard_bits(soft, j, 16);
            if (std::min(__builtin_popcount(w ^ W0), __builtin_popcount(w ^ W1)) > 1) continue;
            if (!all_valid(valid, j, INFO_END)) continue;
            const uint32_t type = hard_bits(soft, j + 16, 3);
            if (type != 0 && type != 7) continue;
            const bool inv = type == 7;
            std::vector<uint8_t> B(INFO_END + 1, 0);
            for (int k = 1; k <= INFO_END; k++) B[static_cast<size_t>(k)] = (soft[j + static_cast<size_t>(k - 1)] > 0) != inv;
            Tmcc t;
            if (!parse(B, &t)) {
                frames_bad_++;
                continue;
            }
            frames_ok_++;
            if (!have_ || !(t == tmcc_)) news_.push_back("TMCC: " + summary(m, t));
            if (t.ews && !ews_reported_) news_.push_back("⚠ Emergency warning broadcast: the TMCC start flag is set");
            if (!t.ews && ews_reported_ && have_) news_.push_back("Emergency warning start flag cleared");
            ews_reported_ = t.ews;
            tmcc_ = t;
            have_ = true;
            j += FRAME - 2;
        }
    }
    std::vector<std::pair<std::string, std::string>> info(const OfdmMode& m) const override {
        std::vector<std::pair<std::string, std::string>> L;
        if (!have_) {
            L.push_back({"TMCC", frames_bad_ ? "check failed in " + std::to_string(frames_bad_) + " frames" : "reading"});
            return L;
        }
        const Tmcc& t = tmcc_;
        L.push_back({"TMCC", "decoded (" + std::to_string(frames_ok_) + " frames)"});
        L.push_back({"System", t.system == 0 ? "ISDB-T (television)" : t.system == 1 ? "ISDB-TSB (sound broadcasting)" : "reserved"});
        L.push_back({"Partial reception", t.partial ? "yes (one-seg service in the centre segment)" : "no"});
        static const char* NAME[3] = {"Layer A", "Layer B", "Layer C"};
        for (int i = 0; i < 3; i++)
            if (t.cur[i].used()) L.push_back({NAME[i], layer_text(m, t.cur[i])});
        double total = 0;
        for (int i = 0; i < 3; i++)
            if (t.cur[i].used()) total += rate(m, t.cur[i]);
        L.push_back({"Net bit rate", fmtd("%.2f Mbit/s", total)});
        L.push_back({"Emergency warning (EWS)", t.ews ? "⚠ start flag SET" : "not active"});
        if (t.countdown != 15) L.push_back({"Parameter change", "in " + std::to_string(t.countdown + 1) + " frames"});
        const bool next_given = t.next[0].used() || t.next[1].used() || t.next[2].used();
        if (next_given && !(t.next[0] == t.cur[0] && t.next[1] == t.cur[1] && t.next[2] == t.cur[2])) {
            std::string n;
            for (int i = 0; i < 3; i++)
                if (t.next[i].used()) n += std::string(n.empty() ? "" : "; ") + "ABC"[i] + ": " + layer_text(m, t.next[i]);
            L.push_back({"Next parameters", n});
        }
        return L;
    }
    std::vector<std::string> take_news() override {
        auto n = std::move(news_);
        news_.clear();
        return n;
    }

private:
    bool have_ = false, ews_reported_ = false;
    Tmcc tmcc_;
    int frames_ok_ = 0, frames_bad_ = 0;
    std::vector<std::string> news_;

    static int mode_of(const OfdmMode& m) { return m.nfft == 2048 ? 1 : m.nfft == 4096 ? 2 : 3; }
    static const std::vector<std::uint8_t>& band(int mode) {
        static const std::vector<std::uint8_t> w[3] = {isdbt_std::prbs_w_band(1), isdbt_std::prbs_w_band(2),
                                                       isdbt_std::prbs_w_band(3)};
        return w[mode - 1];
    }
    // B20..B121 (Table 3-21): the frame's checks - reserved bits all 1, known
    // codes only, the layers' segments add up to 13 (current and next)
    static bool parse(const std::vector<uint8_t>& B, Tmcc* t) {
        auto f = [&](int a, int n) {
            int v = 0;
            for (int i = 0; i < n; i++) v = (v << 1) | B[static_cast<size_t>(a + i)];
            return v;
        };
        if (f(110, 12) != 0xFFF) return false;
        t->system = f(20, 2);
        t->countdown = f(22, 4);
        t->ews = f(26, 1) != 0;
        auto set = [&](int a, bool* partial, Layer* L, bool may_be_empty) {
            *partial = f(a, 1) != 0;
            int segs = 0;
            for (int i = 0; i < 3; i++) {
                const int b = a + 1 + 13 * i;
                L[i] = {f(b, 3), f(b + 3, 3), f(b + 6, 3), f(b + 9, 4)};
                if (!L[i].used()) continue;
                if (L[i].mod > 3 || L[i].cr > 4 || L[i].il > 3) return false;
                segs += L[i].seg;
            }
            return segs == isdbt_std::kNumSegments || (may_be_empty && segs == 0);
        };
        return set(27, &t->partial, t->cur, false) && set(67, &t->next_partial, t->next, true);
    }
    // per layer: segments x data carriers x bits x code rate x 188 / 204 / symbol
    static double rate(const OfdmMode& m, const Layer& L) {
        const int bpc = L.mod <= 1 ? 2 : L.mod == 2 ? 4 : 6;
        const double ts = static_cast<double>(m.nfft + m.ncp) / m.fs;
        return L.seg * isdbt_std::data_carriers_per_segment(mode_of(m)) * bpc * CR_V[L.cr & 7] * 188.0 / 204.0 / ts / 1e6;
    }
    static std::string layer_text(const OfdmMode& m, const Layer& L) {
        static const int IL[4][3] = {{0, 0, 0}, {4, 2, 1}, {8, 4, 2}, {16, 8, 4}};
        return std::to_string(L.seg) + " segment" + (L.seg == 1 ? "" : "s") + ", " + MOD[L.mod & 7] + " " + CR[L.cr & 7] + ", interleave I = " +
               std::to_string(IL[L.il & 3][mode_of(m) - 1]) + ", " + fmtd("%.2f Mbit/s", rate(m, L));
    }
    static std::string summary(const OfdmMode& m, const Tmcc& t) {
        std::string s;
        for (int i = 0; i < 3; i++)
            if (t.cur[i].used()) s += std::string(s.empty() ? "" : "; ") + "ABC"[i] + ": " + layer_text(m, t.cur[i]);
        return s + (t.partial ? " (partial reception)" : "");
    }
};

}  // namespace

std::unique_ptr<Estimator> make_isdbt(kp::Host& h) { return std::make_unique<OfdmEstimator>(h, std::make_unique<IsdbtStd>()); }

}  // namespace sfn
