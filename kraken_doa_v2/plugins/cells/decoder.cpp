// LTE / 5G NR Cell Decoder: the public broadcast information of every cell on
// a mobile network carrier (LTE: MIB, SIB1, SIB2 - SIB5 - operator, tracking
// area, cell identity, band, uplink, neighbours ...; NR: cell ID, beams,
// SS/PBCH block frequency), and each cell's own bearing: the cells that
// share the carrier are told apart by CODE - every cell scrambles its
// signals with its physical cell ID (PCI) - like the transmitters of the sfn
// plugin (which tells them apart by delay). Only broadcast system
// information - nothing addressed to a phone is looked for.
//
// An ARRAY plugin: every antenna (coherent mode) in bursts of 0.3 s every
// second, at the VFO's 2.4 MHz. Per burst and technology:
//   carrier: the 100 kHz LTE raster / the NR sync raster (GSCN) around the
//     VFO centre (or the "carrier_mhz" option), fine offset from the cyclic
//     prefixes; locked once cells are found. Nothing within reach: the
//     receiver is retuned step by step (kp::Host::tune, option "scan") until
//     a carrier is found, then centred on it
//   search.cpp: PSS fold -> timing + N_ID2, SSS -> PCI (+ LTE FDD / TDD and
//     frame timing), several cells per PSS peak in synchronised networks
//   chest.cpp: the cell's own reference signals (LTE CRS, NR SSS) -> every
//     antenna's impulse response -> the path's spatial covariance
//   lte_meta.cpp (LTE, option "decode"): MIB every burst, SIB1 and the SI
//     messages SIB1 schedules - PBCH / PCFICH / PDCCH / PDSCH in lte_dl.cpp
//     + lte_pdsch.cpp, RRC in lte_rrc.cpp - combined over bursts until each
//     block's CRC passes (a carrier wider than the VFO is seen in part)
// Each cell goes to kraken_doa as a kp::Spatial talker "LTE-<pci>" /
// "NR-<pci>" (MUSIC with one source: its bearing, history, heat map on the
// 🗺 Map's 📡 DF panel), plus a table row (+ its details: everything
// decoded), a sync-correlation plot, facts and events. See README.md.

#include "kraken_plugin.hpp"

#include "chest.hpp"
#include "lte_info.hpp"
#include "lte_meta.hpp"
#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace cells;

constexpr double BURST_S = 0.3;
constexpr double PERIOD_S = 1.0;
constexpr double VIS_FRAC = 0.4375;        // the VFO's usable half-width as a share of its rate (2.4 MHz: +-1.05 MHz)
constexpr int SEARCH_PER_BURST = 4;        // raster positions tried per burst while searching
constexpr int LOCK_MISSES = 5;             // bursts without a cell before searching again
constexpr double REMIX_HZ = 300;           // a carrier offset left over beyond this: mixed again
constexpr double REMIX_MIN_Z = 8;          // ... when a convincing cell measured it (not the images of a wrong raster position)
constexpr int SEARCH_EVERY = 4;            // after a whole raster sweep without a cell: search every 4th burst
constexpr int SEARCH_RARE = 16;            // ... and after 12 sweeps every 16th (a search costs ~0.4 s of CPU)
constexpr int CONFIRM_HITS = 2;            // bursts a new cell must be seen in
constexpr double TRACK_TTL_S = 30;         // a cell is forgotten this long after the last sighting
constexpr double SPATIAL_AVG_S = 4;        // the host averages each cell's covariance over this
constexpr int QUICK_SLOTS = 40;            // LTE: slots of the first look at a candidate's CRS
constexpr double MIN_SSS_Z = 6;            // the best SSS hypothesis must stand out this far from the others
constexpr double SYNC_HALF_HZ = 0.45e6;    // the LTE PSS / SSS half-width (+ margin): a raster position must have them inside the VFO
constexpr double CENTRE_MIN_HZ = 5e3;      // a found carrier further off the tuning than this is centred
constexpr double TUNE_TIMEOUT_S = 15;      // a retune not seen in the samples by then: given up
constexpr double SAME_RF_HZ = 2e3;         // RFs this close are the same tuning
constexpr double META_BURST_S = 0.2;       // processing time per burst for the broadcast information (MIB / SIBs) of all cells
constexpr double META_CELL_S = 0.1;        // ... and per cell

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

struct Burst {
    int m = 0;
    double rate = 0, t0 = 0, rf_hz = NAN;
    size_t n = 0;
    std::vector<std::vector<cf>> ch;
};

// How far from the VFO centre a raster position is searched: its sync
// signals (LTE's 0.93 MHz; NR's 1.9 MHz at least partly) must be inside the
// VFO's usable band - +-0.6 MHz at 2.4 MHz, more on a wider VFO
inline double search_span(double vis_hz) { return std::max(0.6e6, vis_hz - SYNC_HALF_HZ); }

// The cell search needs only the band of the sync signals (LTE 1.08 MHz, NR
// 15 kHz 1.9 MHz, NR 30 kHz 3.8 MHz): an input wider than that (a wide-band
// SDR) is searched on a copy decimated by D - the largest D whose rate fs / D
// is at least `need` (its +-0.4 x rate holds the sync signals: LTE 1.92 MHz,
// NR 15 kHz 2.4 MHz as on a 2.4 MHz VFO, NR 30 kHz 4.8 MHz) with a whole
// number of samples per subcarrier - while the channel estimation and the
// broadcast channels use the whole rate. 2.4 MHz: D = 1 (the input itself).
inline int search_decim(double fs, int mu, bool lte) {
    const double need = lte ? 1.92e6 : (mu ? 4.8e6 : 2.4e6);
    int best = 1;
    for (int D = 2; fs / D >= need - 1; D++)
        if (Numerology{fs / D, mu}.usable()) best = D;
    return best;
}

// carrier (LTE) / SSB (NR) centres on the raster within `span` of the VFO
// centre, as offsets from it, nearest first
std::vector<double> raster(Tech t, double rf, double span) {
    std::vector<double> v;
    if (!std::isfinite(rf)) return {0.0};
    if (t == Tech::LTE) {
        for (long k = static_cast<long>(std::ceil((rf - span) / 100e3)); k * 100e3 <= rf + span; k++) v.push_back(k * 100e3 - rf);
    } else if (rf < 3e9) {
        // GSCN below 3 GHz: N x 1200 kHz + M x 50 kHz, M = 1, 3, 5
        for (long N = static_cast<long>(std::floor((rf - span) / 1.2e6)); N * 1.2e6 <= rf + span + 1.2e6; N++)
            for (int M : {1, 3, 5}) {
                const double f = N * 1.2e6 + M * 50e3;
                if (std::fabs(f - rf) <= span) v.push_back(f - rf);
            }
    } else {
        for (long N = static_cast<long>(std::floor((rf - span - 3e9) / 1.44e6)); 3e9 + N * 1.44e6 <= rf + span; N++) {
            const double f = 3e9 + N * 1.44e6;
            if (std::fabs(f - rf) <= span) v.push_back(f - rf);
        }
    }
    std::sort(v.begin(), v.end(), [](double a, double b) { return std::fabs(a) < std::fabs(b); });
    return v;
}

class CellsPlugin : public kp::Decoder {
public:
    explicit CellsPlugin(kp::Host& h) : Decoder(h) {
        tech_[0].tech = Tech::LTE;
        tech_[1].tech = Tech::NR;
    }

    // array plugin (process_array); kraken_doa still calls process() with a
    // tick every 100 ms - the carrier scan's clock
    void process(const kp::cf*, size_t) override { scan_timeout(); }

    void process_array(const kp::ArrayBlock& b) override {
        if (b.m < 1 || b.n == 0) return;
        const bool shape = !burst_.ch.empty() && (b.m != burst_.m || b.rate != burst_.rate);
        if ((b.gap || shape) && burst_.n > 0) finish();
        if (burst_.n == 0) {
            burst_.m = b.m;
            burst_.rate = b.rate;
            burst_.t0 = b.t0;
            burst_.rf_hz = host.rf_hz();
            burst_.ch.assign(static_cast<size_t>(b.m), {});
        }
        for (int k = 0; k < b.m; k++) burst_.ch[static_cast<size_t>(k)].insert(burst_.ch[static_cast<size_t>(k)].end(), b.ch[k], b.ch[k] + b.n);
        burst_.n += b.n;
        if (static_cast<double>(burst_.n) >= BURST_S * b.rate - 1) finish();
    }

    void reset() override {
        burst_ = Burst{};
        for (auto& t : tech_) {
            t.locked = false;
            t.rr = 0;
            t.misses = 0;
            t.tried = 0;
            t.sweeps = 0;
            t.idle = 0;
            t.duplex = -1;
            t.f_abs = NAN;
            t.kstep = 0;
        }
        for (const auto& [id, t] : tracks_)
            if (t.confirmed) host.table_remove(id);
        tracks_.clear();
        meta_.clear();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "tech") {
            tech_opt_ = value.empty() ? "auto" : value;
            reset();
            scan_ = Scan{};
        } else if (key == "carrier_mhz") {
            const double v = atof(value.c_str());
            carrier_hz_ = v > 0 ? v * 1e6 : NAN;
            reset();
            scan_ = Scan{};
        } else if (key == "scan") {
            const double v = value.empty() ? 10 : atof(value.c_str());
            scan_range_ = std::clamp(v, 0.0, 100.0) * 1e6;
            scan_ = Scan{};
        } else if (key == "min_snr") {
            const double v = atof(value.c_str());
            min_snr_ = v > 0 ? v : 8;
        } else if (key == "path") {
            first_path_ = value == "first";
        } else if (key == "max_cells") {
            const int v = atoi(value.c_str());
            max_cells_ = v > 0 ? std::min(v, 32) : 12;
        } else if (key == "decode") {
            decode_ = value != "0";
        } else if (key == "verbose") {
            verbose_ = value == "1";
        }
    }

private:
    struct TechState {
        Tech tech = Tech::LTE;
        bool locked = false;
        double carrier_abs = 0;          // locked carrier / SSB centre (Hz; the offset when the RF is unknown)
        size_t rr = 0;                   // raster round robin while searching
        int misses = 0;
        int tried = 0, total = 0;        // raster positions tried in this sweep / in all
        int sweeps = 0;                  // whole raster sweeps without a cell
        int idle = 0;                    // bursts since the last search (after a sweep)
        int duplex = -1;                 // LTE: the locked carrier's (0 FDD, 1 TDD)
        double f_abs = NAN;              // locked: where the carrier really is (RF + offset, Hz) - tracked
        int kstep = 0;                   // searching: whole subcarriers added to the prefixes' estimate (0, +1, -1 by sweep)
    };
    // one cell seen in this burst
    struct Seen {
        std::string id;
        CellHit hit;
        CellEst est;
        int mu = 0;
        double carrier_hz = NAN;         // the carrier (LTE) / SSB (NR) centre
    };
    struct Track {
        Tech tech = Tech::LTE;
        int pci = 0;
        int hits = 0;
        bool confirmed = false;
        double last_t = 0;
        std::string mode;
    };

    Burst burst_;
    TechState tech_[2];
    std::map<std::string, Track> tracks_;
    std::string tech_opt_ = "auto";
    double carrier_hz_ = NAN;
    double min_snr_ = 8;
    bool first_path_ = false;
    int max_cells_ = 12;
    bool verbose_ = false;
    std::vector<std::vector<cf>> mix_;   // mixed copies of the antennas
    bool decode_ = true;                 // read the cells' broadcast information
    std::map<std::string, lte::CellMeta> meta_;   // by cell id

    // Carrier scan (option "scan" = +-range): when a whole raster sweep where
    // the receiver is tuned finds no cell, it asks kraken_doa to retune
    // (kp::Host::tune) to f0 +1.2, -1.2, +2.4 ... MHz - a position's raster
    // in one burst -, and once a carrier is found centres it (also one found
    // without scanning). Survives reset() (every retune resets the plugin); a
    // retune it didn't ask for (the user) starts over from there.
    double scan_range_ = 10e6;
    double scan_step_ = 1.2e6;           // 2 x the raster search's reach (search_span) at the VFO's rate
    double vis_hz_ = 1.05e6;             // the VFO's usable half-width (VIS_FRAC x its rate)
    std::vector<std::vector<cf>> dec_;   // decimated copies of the antennas for the cell search
    std::map<int, std::vector<float>> dec_taps_;

    // search() at the rate the sync signals need (search_decim): the hits'
    // sample positions come back in the input's samples; the fold (plot) stays
    // at its own rate (SearchOut::rate)
    SearchOut search_at(const std::vector<const cf*>& x, size_t n, SearchParams sp, double fs) {
        const int D = search_decim(fs, sp.num.mu, sp.tech == Tech::LTE);
        if (D <= 1) return search(x, n, sp);
        auto it = dec_taps_.find(D);
        if (it == dec_taps_.end()) it = dec_taps_.emplace(D, decim_taps(D)).first;
        if (dec_.size() < x.size()) dec_.resize(x.size());
        std::vector<const cf*> xd;
        for (size_t a = 0; a < x.size(); a++) {
            decimate(x[a], n, D, it->second, &dec_[a]);
            xd.push_back(dec_[a].data());
        }
        const double fsd = fs / D;
        sp.num = Numerology{fsd, sp.num.mu};
        sp.band_hz = 0.4 * fsd;   // decim_taps: within ~1 dB, aliases from 0.6 x rate up attenuated
        SearchOut so = search(xd, dec_[0].size(), sp);
        for (auto& c : so.cells) {
            c.frame0 *= D;
            c.pss_mod *= D;
            for (auto& p : c.pss) p *= D;
        }
        return so;
    }
    struct Scan {
        double f0 = NAN;                 // the RF it started from
        int step = 0;                    // 0 = f0, k = the k-th position away from it
        double target = NAN;             // the RF asked for, not in the samples yet
        std::chrono::steady_clock::time_point asked;
        int timeouts = 0;
        bool done = false;               // swept without a carrier (or can't tune): stay
        bool centred = false;            // asked to centre the found carrier
        double last_rf = NAN;            // the RF of the last burst
    } scan_;

    bool ask_tune(double rf, const std::string& why) {
        if (!host.tune(rf)) return false;
        scan_.target = rf;
        scan_.asked = std::chrono::steady_clock::now();
        host.event(why + ": retuning to " + fmt("%.3f MHz", rf / 1e6), 0.0);
        return true;
    }

    // a retune that never shows up in the samples (refused - the event log
    // says why -, or the receiver is stuck): give up after the second
    void scan_timeout() {
        if (!std::isfinite(scan_.target)) return;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - scan_.asked).count() < TUNE_TIMEOUT_S) return;
        scan_.target = NAN;
        if (++scan_.timeouts >= 2) {
            scan_.done = true;
            host.event("The receiver didn't retune - carrier scan stopped", 0.0);
        }
    }

    // the burst's RF against what was asked: arrived / the user retuned
    void scan_follow(double rf) {
        if (!std::isfinite(rf)) return;
        if (std::isfinite(scan_.target) && std::fabs(rf - scan_.target) < SAME_RF_HZ) {
            scan_.target = NAN;
            scan_.timeouts = 0;
        } else if (std::isfinite(scan_.last_rf) && std::fabs(rf - scan_.last_rf) > SAME_RF_HZ) {
            scan_ = Scan{};   // not ours: start over from the new tuning
        }
        scan_.last_rf = rf;
    }

    // after a burst's search: centre a found carrier, or move on
    void scan_after(double rf, bool found, bool swept) {
        if (!(scan_range_ > 0) || std::isfinite(carrier_hz_) || !std::isfinite(rf) || std::isfinite(scan_.target)) return;
        if (found) {
            if (!scan_.centred) {
                scan_.centred = true;
                scan_.done = true;
                const TechState& t = tech_[0].locked ? tech_[0] : tech_[1];
                if (t.locked && std::fabs(t.carrier_abs - rf) > CENTRE_MIN_HZ)
                    ask_tune(t.carrier_abs, std::string(t.tech == Tech::LTE ? "Centring the LTE carrier" : "Centring the NR SS/PBCH block"));
            }
            return;
        }
        if (scan_.done || !swept) return;
        if (!std::isfinite(scan_.f0)) scan_.f0 = rf;
        // positions f0 + 1.2, - 1.2, + 2.4, - 2.4 ... MHz
        const int k = ++scan_.step;
        const double off = (k % 2 ? 1 : -1) * ((k + 1) / 2) * scan_step_;
        const int total = 2 * static_cast<int>(scan_range_ / scan_step_);
        if (std::fabs(off) > scan_range_ + 1) {
            scan_.done = true;
            host.event("No LTE / NR carrier within " + fmt("%.0f MHz", scan_range_ / 1e6) + " of " + fmt("%.3f MHz", scan_.f0 / 1e6), 0.0);
            if (std::fabs(rf - scan_.f0) > SAME_RF_HZ) ask_tune(scan_.f0, "Back where the scan started");
            return;
        }
        if (!ask_tune(scan_.f0 + off, "Carrier scan " + std::to_string(k) + " of " + std::to_string(total))) {
            scan_.done = true;   // this host can't retune (offline test)
            return;
        }
    }

    bool tech_on(Tech t) const {
        if (t == Tech::LTE) return tech_opt_ == "auto" || tech_opt_ == "lte";
        return tech_opt_ == "auto" || tech_opt_ == "nr15" || tech_opt_ == "nr30";
    }
    int nr_mu() const { return tech_opt_ == "nr30" ? 1 : 0; }

    // the carrier offset the cells measured (score-weighted median of their
    // SSS-against-PSS phases)
    static double residual_hz(const std::vector<CellHit>& cells) {
        if (cells.empty()) return 0;
        std::vector<std::pair<double, double>> v;
        double tot = 0;
        for (const auto& h : cells) {
            v.push_back({h.df_hz, h.score});
            tot += h.score;
        }
        std::sort(v.begin(), v.end());
        double acc = 0;
        for (const auto& [f, w] : v) {
            acc += w;
            if (acc >= tot / 2) return f;
        }
        return v.back().first;
    }

    // antennas 0..k-1 of the burst mixed by -f (the carrier to 0 Hz)
    std::vector<const cf*> mixed(const Burst& b, int k, double f) {
        if (static_cast<int>(mix_.size()) < k) mix_.resize(static_cast<size_t>(k));
        std::vector<const cf*> out;
        for (int a = 0; a < k; a++) {
            auto& v = mix_[static_cast<size_t>(a)];
            v.assign(b.ch[static_cast<size_t>(a)].begin(), b.ch[static_cast<size_t>(a)].end());
            double ph = 0;
            shift_freq(v.data(), v.size(), -f, b.rate, &ph);
            out.push_back(v.data());
        }
        return out;
    }

    void finish() {
        Burst b;
        std::swap(b, burst_);
        burst_ = Burst{};
        // a fragment (a burst cut short by a gap / retune) is not enough for the 5 / 20 ms periods
        if (static_cast<double>(b.n) < 0.5 * BURST_S * b.rate) return;
        const auto c0 = std::chrono::steady_clock::now();
        Numerology num0{b.rate, 0}, num1{b.rate, 1};
        if (!num0.usable()) {
            host.fact("Signal", "needs the 2.4 MHz VFO bandwidth (no decimation) - a rate with a whole number of samples per 15 kHz");
            return;
        }
        // the VFO's usable band, and the raster search's reach in it
        const double vis = VIS_FRAC * b.rate;
        vis_hz_ = vis;
        scan_step_ = 2 * search_span(vis);
        scan_follow(b.rf_hz);
        if (std::isfinite(scan_.target)) {   // the samples are still from before the retune
            host.fact("Signal", "retuning to " + fmt("%.3f MHz", scan_.target / 1e6));
            return;
        }
        const int m = b.m;
        const int A = std::min(m, 2);
        std::vector<const cf*> raw;
        for (int a = 0; a < A; a++) raw.push_back(b.ch[static_cast<size_t>(a)].data());
        // the carrier offset modulo the subcarrier spacing (cyclic prefixes)
        double st0 = 0, st1 = 0;
        prof() = Prof{};
        double tp = now_s();
        const double eps0 = cp_offset(raw, b.n, num0, &st0);
        const double eps1 = tech_on(Tech::NR) && nr_mu() == 1 ? cp_offset(raw, b.n, num1, &st1) : 0.0;

        std::vector<Seen> seen;
        double meta_left = META_BURST_S;
        std::vector<float> plot_fold;
        int plot_period = 0, plot_cells = -1;
        double plot_ref = 0, plot_rate = 0;
        std::string signal;
        double freq_err = NAN;
        for (auto& ts : tech_) {
            if (!tech_on(ts.tech)) continue;
            const bool lte = ts.tech == Tech::LTE;
            const int mu = lte ? 0 : nr_mu();
            const Numerology& num = mu == 0 ? num0 : num1;
            const double eps = mu == 0 ? eps0 : eps1;
            const double scs = num.scs();
            // carrier candidates. The prefixes only know the offset modulo
            // the subcarrier spacing: a receiver more than half a subcarrier
            // off (7.5 kHz: 9 ppm at 800 MHz) needs the next whole subcarrier
            // - the search sweeps the raster with 0, +1 and -1 of them
            static const int KSEQ[3] = {0, 1, -1};
            const int ks = ts.locked || std::isfinite(carrier_hz_) ? 0 : KSEQ[ts.kstep % 3];
            std::vector<double> cand;
            // the PSS / SSS must be (mostly) inside the VFO: LTE's 0.93 MHz
            // within +-0.6 MHz at 2.4 MHz, NR's 1.9 MHz partly there
            const double span = search_span(vis);
            if (std::isfinite(carrier_hz_)) {
                cand.push_back(std::isfinite(b.rf_hz) ? carrier_hz_ - b.rf_hz : 0.0);
            } else if (ts.locked) {
                cand.push_back(std::isfinite(b.rf_hz) ? ts.carrier_abs - b.rf_hz : ts.carrier_abs);
            } else {
                // nothing found in the sweeps of every subcarrier step: search
                // every SEARCH_EVERY-th (later SEARCH_RARE-th) burst only
                if (ts.sweeps >= 3 && ++ts.idle % (ts.sweeps >= 12 ? SEARCH_RARE : SEARCH_EVERY) != 0) continue;
                const auto all = raster(ts.tech, b.rf_hz, span);
                ts.total = static_cast<int>(all.size());
                if (all.empty()) {   // nothing of this technology can be here
                    ts.sweeps++;
                    continue;
                }
                // a scan position gets its whole raster in one burst
                const int per_burst = scan_.step > 0 ? static_cast<int>(all.size()) : SEARCH_PER_BURST;
                for (int i = 0; i < per_burst && i < static_cast<int>(all.size()); i++) {
                    cand.push_back(all[(ts.rr + static_cast<size_t>(i)) % all.size()]);
                }
                const size_t next = ts.rr + std::min<size_t>(static_cast<size_t>(per_burst), all.size());
                if (next >= all.size()) {
                    ts.sweeps++;
                    ts.kstep++;
                }
                ts.rr = next % std::max<size_t>(1, all.size());
                ts.tried = std::min(ts.total, ts.tried + static_cast<int>(cand.size()));
            }
            EstParams ep;
            ep.num = num;
            ep.vis_hz = vis;
            ep.min_snr_db = min_snr_;
            ep.first_path = first_path_;
            // a candidate counts only the cells whose own reference signals
            // are there (a wrong raster position still shows PSS peaks - a ZC
            // sequence shifted in frequency correlates at another time - and
            // the SSS hypotheses always have a best one)
            SearchOut best;
            double best_c = 0, best_score = 0, best_f = 0;
            const bool single = cand.size() == 1;
            std::vector<const cf*> xs;
            for (size_t ci = 0; ci < cand.size(); ci++) {
                const double c = cand[ci];
                // the carrier's offset: the raster position + the cyclic
                // prefixes' estimate (modulo the subcarrier spacing), or once
                // locked the frequency tracked from the cells themselves (two
                // systems on one carrier - DSS - pull the prefixes' estimate)
                double ftot = ts.locked && single && std::isfinite(ts.f_abs) && std::isfinite(b.rf_hz) ? ts.f_abs - b.rf_hz
                                                                                                         : c + std::remainder(eps - c, scs) + ks * scs;
                SearchParams sp;
                sp.tech = ts.tech;
                sp.num = num;
                sp.carrier_off = c;
                sp.vis_hz = vis;
                sp.fold_ants = A;
                sp.min_z = MIN_SSS_Z;
                if (single && ts.locked) sp.duplex = ts.duplex;
                // one candidate: every antenna mixed now (the SSS test and
                // the channel estimation use them all)
                prof().cp += now_s() - tp;
                tp = now_s();
                auto x = mixed(b, single ? m : A, ftot);
                prof().mix += now_s() - tp;
                SearchOut so = search_at(x, b.n, sp, b.rate);
                // the cells' own measure of the offset left over (their
                // SSS against their PSS): a large one is fixed and the burst
                // searched again - the CRS check needs it within ~0.5 kHz
                const double df = residual_hz(so.cells);
                if (verbose_) host.log(std::string(lte ? "LTE" : "NR") + " candidate " + fmt("%+.1f kHz", c / 1e3) + " k " + std::to_string(ks) + ": mixed at " + fmt("%+.0f Hz", ftot) + ", " + std::to_string(so.cells.size()) + " SSS hits, residual " + fmt("%+.0f Hz", df));
                double zmax = 0;
                for (const auto& h : so.cells) zmax = std::max(zmax, h.z);
                if (std::fabs(df) > REMIX_HZ && zmax >= REMIX_MIN_Z) {
                    ftot += df;
                    x = mixed(b, single ? m : A, ftot);
                    so = search_at(x, b.n, sp, b.rate);
                    if (verbose_) host.log(std::string(lte ? "LTE" : "NR") + ": carrier offset corrected by " + fmt("%+.0f Hz", df));
                }
                tp = now_s();
                std::sort(so.cells.begin(), so.cells.end(), [](const CellHit& p1, const CellHit& p2) { return p1.score > p2.score; });
                EstParams q = ep;
                q.carrier_off = c;
                q.quick = true;
                q.max_snaps = QUICK_SLOTS;
                std::vector<CellHit> ok;
                for (const auto& h : so.cells) {
                    if (static_cast<int>(ok.size()) >= max_cells_) break;
                    prof().quick_n++;
                    if (estimate(x, b.n, h, q).ok) ok.push_back(h);
                    else if (verbose_)
                        host.log(std::string(lte ? "LTE" : "NR") + " PCI " + std::to_string(h.pci) + ": SSS score " + fmt("%.1f", h.score) + " z " +
                                 fmt("%.1f", h.z) + " at " + fmt("%+.1f kHz", c / 1e3) + ": not confirmed by its reference signals");
                }
                so.cells = std::move(ok);
                prof().quick += now_s() - tp;
                tp = now_s();
                double sc = 0;
                for (const auto& h : so.cells) sc += h.score;
                if (sc > best_score) {
                    best_score = sc;
                    best = std::move(so);
                    best_c = c;
                    best_f = ftot;
                    if (single) xs = x;
                }
            }
            const double ftot = best_f;
            if (best.cells.empty()) {
                if (ts.locked && ++ts.misses >= LOCK_MISSES) {
                    ts.locked = false;
                    ts.tried = 0;
                    ts.duplex = -1;
                    ts.f_abs = NAN;
                    host.event(std::string(lte ? "LTE" : "NR") + ": no cells - searching again", 0.0);
                }
                continue;
            }
            ts.misses = 0;
            ts.sweeps = 0;
            if (lte) {
                int tdd = 0;
                for (const auto& h : best.cells) tdd += h.tdd ? 1 : -1;
                ts.duplex = tdd > 0 ? 1 : 0;
            }
            const double abs_c = std::isfinite(b.rf_hz) ? b.rf_hz + best_c : best_c;
            if (!ts.locked || std::fabs(abs_c - ts.carrier_abs) > 1) {
                ts.locked = true;
                ts.carrier_abs = abs_c;
                host.event(std::string(lte ? "LTE carrier" : "NR SS/PBCH block") +
                               (std::isfinite(b.rf_hz) ? " at " + fmt("%.3f MHz", abs_c / 1e6) : "") + " (VFO " + fmt("%+.1f kHz", best_c / 1e3) + ")",
                           0.0);
            }
            if (xs.empty()) xs = mixed(b, m, ftot);
            if (std::isfinite(b.rf_hz)) ts.f_abs = b.rf_hz + ftot + residual_hz(best.cells);
            // the network's frequency against the receiver's
            if (!std::isfinite(freq_err)) freq_err = ftot + residual_hz(best.cells) - best_c;
            ep.carrier_off = best_c;
            int nc = 0;
            for (const auto& h : best.cells) {
                const double te = now_s();
                CellEst e = estimate(xs, b.n, h, ep);
                prof().full += now_s() - te;
                prof().full_n++;
                if (verbose_) {
                    char buf[240];
                    snprintf(buf, sizeof buf, "%s PCI %d: SSS score %.1f z %.1f (peak %.0f r%d)%s, est %s snr %.1f dB, power %.3g, delay %.2f us, %d/%d snaps, ports %d, purity %.2f",
                             lte ? "LTE" : "NR", h.pci, h.score, h.z, h.pss_mod, h.round, lte ? (h.tdd ? " TDD" : " FDD") : "", e.ok ? "ok" : "--", e.snr_db, e.power,
                             e.delay_us, e.used, e.snaps, e.ports, e.purity);
                    host.log(buf);
                }
                if (!e.ok) continue;
                const std::string id = std::string(lte ? "LTE-" : "NR-") + std::to_string(h.pci);
                seen.push_back({id, h, std::move(e), mu, abs_c});
                nc++;
            }
            // what the cells broadcast (MIB, SIB1, SI messages), within this
            // burst's time budget - the cell served longest ago first (while
            // xs still holds this technology's mix)
            if (lte && decode_) {
                std::vector<const Seen*> order;
                for (const auto& s : seen)
                    if (s.hit.tech == Tech::LTE) order.push_back(&s);
                std::sort(order.begin(), order.end(), [&](const Seen* x, const Seen* y) { return meta_[x->id].served < meta_[y->id].served; });
                for (const Seen* s : order) {
                    if (meta_left <= 0.005) break;
                    const double tm = now_s();
                    lte::CellRx rx(xs, b.n, num, s->hit.pci, s->hit.frame0, std::isfinite(s->est.cfo_hz) ? s->est.cfo_hz : 0.0, best_c, vis);
                    lte::MetaParams mp;
                    mp.now = b.t0;
                    mp.tdd = s->hit.tdd;
                    mp.ports_hint = s->est.ports;
                    mp.budget_s = std::min(META_CELL_S, meta_left);
                    mp.verbose = verbose_;
                    auto& cm = meta_[s->id];
                    cm.served = b.t0;
                    lte::meta_burst(rx, cm, mp);
                    for (const auto& d : cm.debug) host.log("LTE PCI " + std::to_string(s->hit.pci) + " " + d);
                    cm.debug.clear();
                    meta_left -= now_s() - tm;
                }
            }
            if (nc > plot_cells && !best.fold.empty()) {
                plot_cells = nc;
                plot_fold = best.fold;
                plot_period = best.period;
                plot_ref = best.cells.front().pss_mod;
                plot_rate = best.rate;
            }
            signal += (signal.empty() ? "" : "; ") + std::string(lte ? "LTE" : (mu ? "NR 30 kHz" : "NR 15 kHz")) +
                      (std::isfinite(b.rf_hz) ? " " + fmt("%.3f MHz", abs_c / 1e6) : "") + " (VFO " + fmt("%+.1f kHz", best_c / 1e3) + ")";
        }
        report(b, seen, plot_fold, plot_period, plot_ref, plot_rate);
        bool found = false, swept = true;
        for (const auto& ts : tech_) {
            if (!tech_on(ts.tech)) continue;
            found = found || ts.locked;
            swept = swept && ts.sweeps >= 1;
        }
        scan_after(b.rf_hz, found, swept);
        if (std::isfinite(scan_.target) && !found) {
            host.fact("Signal", "scanning: retuning to " + fmt("%.3f MHz", scan_.target / 1e6) + " (from " + fmt("%.3f MHz", scan_.f0 / 1e6) +
                                    ", +-" + fmt("%.0f MHz", scan_range_ / 1e6) + ")");
        } else if (signal.empty()) {
            std::string s;
            for (const auto& ts : tech_)
                if (tech_on(ts.tech))
                    s += (s.empty() ? "" : ", ") + std::string(ts.tech == Tech::LTE ? "LTE" : "NR") + " " + std::to_string(ts.tried) + " of " +
                         std::to_string(ts.total) + " raster positions";
            host.fact("Signal", "searching (" + s + ")");
        } else {
            host.fact("Signal", signal);
        }
        if (std::isfinite(freq_err))
            host.fact("Frequency offset", fmt("%+.0f Hz", freq_err) +
                                              (std::isfinite(b.rf_hz) && b.rf_hz > 0 ? " (" + fmt("%+.2f ppm", freq_err / b.rf_hz * 1e6) + ")" : "") +
                                              ": the network's carrier against the receiver's tuning");
        host.fact("Antennas", std::to_string(m) + (m < 2 ? " (no bearings outside coherent mode)" : ""));
        if (verbose_) {
            char buf[200];
            const Prof& pf = prof();
            snprintf(buf, sizeof buf, "burst: %.3f s of processing (cp+mix %.3f, fold %.3f, sss %.3f / %d peaks, quick %.3f / %d, full %.3f / %d, broadcast info %.3f)",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count(), pf.cp + pf.mix, pf.fold, pf.sss, pf.peaks,
                     pf.quick, pf.quick_n, pf.full, pf.full_n, META_BURST_S - meta_left);
            host.log(buf);
        }
    }

    // the table's broadcast-information columns of a cell
    struct Bcast {
        std::string op, tac, enb, band, arfcn, bw;
    };
    static Bcast bcast(const Seen& s, const lte::CellMeta* cm) {
        Bcast c;
        const double f = s.carrier_hz;
        if (s.hit.tech == Tech::NR) {
            if (std::isfinite(f) && f > 0) {
                for (int band : nr_bands_at(f)) c.band += (c.band.empty() ? "n" : "/n") + std::to_string(band);
                c.arfcn = std::to_string(nr_arfcn(f));
            }
            return c;
        }
        if (cm && cm->mib_ok) c.bw = fmt("%g MHz", cm->mib.nrb == 6 ? 1.4 : cm->mib.nrb / 5.0);
        std::vector<int> bands;
        if (cm && cm->sib1_ok) {
            const auto& s1 = cm->sib1;
            bands.push_back(s1.band);
            if (!s1.plmn.empty()) {
                const std::string ctry = mcc_country(s1.plmn[0].mcc);
                c.op = s1.plmn[0].text() + (ctry.empty() ? "" : " (" + ctry + ")") + (s1.plmn.size() > 1 ? " +" + std::to_string(s1.plmn.size() - 1) : "");
            }
            c.tac = std::to_string(s1.tac);
            c.enb = std::to_string(s1.cell_id >> 8) + " / " + std::to_string(s1.cell_id & 0xFF);
        } else if (std::isfinite(f) && f > 0) {
            bands = lte_bands_at(f);
        }
        for (int band : bands) {
            c.band += (c.band.empty() ? "" : "/") + std::to_string(band);
            const LteBand* lb = lte_band(band);
            const int n = lb && std::isfinite(f) ? lte_earfcn(*lb, f) : -1;
            if (n >= 0 && c.arfcn.empty()) c.arfcn = std::to_string(n);
        }
        return c;
    }

    // everything known about a cell, for its table row's details
    static std::vector<std::pair<std::string, std::string>> details(const Seen& s, const lte::CellMeta* cm, double db, double tim) {
        std::vector<std::pair<std::string, std::string>> L;
        const bool lte = s.hit.tech == Tech::LTE;
        const int pci = s.hit.pci;
        L.push_back({"Physical cell ID", std::to_string(pci) + " (group " + std::to_string(pci / 3) + ", sector " + std::to_string(pci % 3) + ")"});
        L.push_back({"Technology", lte ? std::string(s.hit.tdd ? "LTE TDD" : "LTE FDD") + ", normal cyclic prefix"
                                       : std::string("5G NR, SS/PBCH block ") + (s.mu ? "30" : "15") + " kHz, " + std::to_string(s.hit.beams) +
                                             (s.hit.beams > 1 ? " beams" : " beam") + " seen"});
        if (!lte && std::isfinite(s.carrier_hz) && s.carrier_hz > 0) {
            const double f = s.carrier_hz;
            long gscn = -1;
            if (f < 3e9) {
                const long N = static_cast<long>(std::floor(f / 1.2e6));
                const long M = std::lround((f - N * 1.2e6) / 50e3);
                if (M == 1 || M == 3 || M == 5) gscn = 3 * N + (M - 3) / 2;
            } else {
                gscn = 7499 + std::lround((f - 3e9) / 1.44e6);
            }
            std::string bands;
            for (int band : nr_bands_at(f)) bands += (bands.empty() ? "n" : ", n") + std::to_string(band);
            L.push_back({"SS/PBCH block", fmt("%.3f MHz", f / 1e6) + ", NR-ARFCN " + std::to_string(nr_arfcn(f)) +
                                              (gscn >= 0 ? ", GSCN " + std::to_string(gscn) : "") + (bands.empty() ? "" : ", band " + bands)});
        }
        L.push_back({"Signal", fmt("%.1f dB", db) + " against the strongest cell, " + fmt("%.0f dB above the estimation floor", s.est.snr_db)});
        L.push_back({"Timing", fmt("%+.2f \xC2\xB5s", tim) + " against the strongest cell" +
                                   (std::fabs(tim) < 0.5 ? " (same frame timing: synchronised network / same site)" : "")});
        L.push_back({"Paths", std::to_string(s.est.paths) + " above the threshold, delay spread " + fmt("%.2f \xC2\xB5s", s.est.spread_us) +
                                  ", first " + fmt("%.2f \xC2\xB5s", s.est.first_us) + ", bearing from " + fmt("%.2f \xC2\xB5s", s.est.delay_us)});
        if (lte && std::isfinite(s.est.cfo_hz)) L.push_back({"Carrier offset (cell)", fmt("%+.0f Hz", s.est.cfo_hz) + " after the receiver's correction"});
        if (cm) {
            const auto ml = lte::meta_lines(*cm, s.carrier_hz);
            L.insert(L.end(), ml.begin(), ml.end());
            if (!cm->mib_ok && !cm->status.empty()) L.push_back({"Decoding", cm->status});
        }
        return L;
    }

    // facts: the carrier(s) and the networks heard
    void carrier_facts(const std::vector<Seen>& seen) {
        std::string carrier, nets, si;
        int n_mib = 0, n_sib1 = 0, n_all = 0, n_lte = 0;
        std::vector<std::string> plmns;
        for (const auto& [id, cm] : meta_) {
            n_lte++;
            n_mib += cm.mib_ok;
            n_sib1 += cm.sib1_ok;
            bool all = cm.sib1_ok;
            for (size_t i = 1; cm.sib1_ok && i <= cm.sib1.sched.size(); i++) {
                const auto it = cm.si.find(static_cast<int>(i));
                all = all && it != cm.si.end() && it->second.ok;
            }
            n_all += all;
            if (cm.sib1_ok)
                for (const auto& p : cm.sib1.plmn) {
                    const std::string c = mcc_country(p.mcc);
                    const std::string t = p.text() + (c.empty() ? "" : " (" + c + ")");
                    if (std::find(plmns.begin(), plmns.end(), t) == plmns.end()) plmns.push_back(t);
                }
        }
        // the carrier, from the strongest LTE cell whose MIB is known
        const Seen* best = nullptr;
        for (const auto& s : seen) {
            if (s.hit.tech != Tech::LTE) continue;
            const auto it = meta_.find(s.id);
            if (it == meta_.end() || !it->second.mib_ok) continue;
            if (!best || s.est.power > best->est.power) best = &s;
        }
        if (best) {
            const auto& cm = meta_.at(best->id);
            const Bcast bc = bcast(*best, &cm);
            carrier = (std::isfinite(best->carrier_hz) ? fmt("%.1f MHz", best->carrier_hz / 1e6) + ", " : "") + (best->hit.tdd ? "TDD" : "FDD") +
                      (bc.band.empty() ? "" : ", band " + bc.band) + (bc.arfcn.empty() ? "" : ", EARFCN " + bc.arfcn) + ", " + bc.bw + " (" +
                      std::to_string(cm.mib.nrb) + " RB)" + ", SFN " + std::to_string(cm.mib.sfn);
            if (cm.mib.nrb > 12)
                carrier += fmt("; the VFO sees %.0f %% of it - the rest of SIB1 / SI comes from combining repeats", 100.0 * std::min(1.0, 2 * vis_hz_ / (cm.mib.nrb * 180e3)));
        }
        for (const auto& p : plmns) nets += (nets.empty() ? "" : ", ") + p;
        if (n_lte) si = "MIB " + std::to_string(n_mib) + ", SIB1 " + std::to_string(n_sib1) + ", all SI messages " + std::to_string(n_all) + " of " + std::to_string(n_lte) + " LTE cells";
        host.fact("LTE carrier", carrier);
        host.fact("Network (PLMN)", nets);
        host.fact("System information", decode_ ? si : std::string("not decoded (option off)"));
    }

    // fold / period at fold_rate (the cell search's rate, search_at); ref_pos in input samples
    void report(const Burst& b, const std::vector<Seen>& seen, const std::vector<float>& fold, int period, double ref_pos, double fold_rate) {
        const double t1 = b.t0 + static_cast<double>(b.n) / b.rate;
        const double now = t1;
        // power relative to the strongest cell of the same technology
        double pmax[2] = {0, 0};
        const Seen* ref[2] = {nullptr, nullptr};
        for (const auto& s : seen) {
            const int k = s.hit.tech == Tech::LTE ? 0 : 1;
            if (s.est.power > pmax[k]) {
                pmax[k] = s.est.power;
                ref[k] = &s;
            }
        }
        std::vector<std::string> cols = {"Cell", "PCI", "Mode", "Operator", "TAC", "eNB / cell", "Band", "(E)ARFCN", "Bandwidth", "Ports / beams",
                                         "Power (dB)", "Above noise (dB)", "Timing (\xC2\xB5s)", "Spread (\xC2\xB5s)"};
        host.table_columns(cols);
        int confirmed = 0;
        for (const auto& s : seen) {
            const bool lte = s.hit.tech == Tech::LTE;
            const int k = lte ? 0 : 1;
            const double db = pmax[k] > 0 ? 10 * std::log10(std::max(s.est.power / pmax[k], 1e-9)) : 0;
            // timing against the strongest cell (LTE: radio frames, NR: SS bursts)
            double tim = 0;
            if (ref[k] && ref[k] != &s) {
                const double per = lte ? 0.01 * b.rate : 0.02 * b.rate;
                const double a = lte ? s.hit.frame0 : s.hit.pss_mod;
                const double r = lte ? ref[k]->hit.frame0 : ref[k]->hit.pss_mod;
                tim = std::remainder(a - r, per) / b.rate * 1e6 + (s.est.delay_us - ref[k]->est.delay_us);
            }
            const std::string mode = lte ? (s.hit.tdd ? "LTE TDD" : "LTE FDD") : (s.mu ? "NR 30 kHz" : "NR 15 kHz");
            Track& t = tracks_[s.id];
            t.tech = s.hit.tech;
            t.pci = s.hit.pci;
            t.mode = mode;
            t.hits++;
            t.last_t = now;
            if (!t.confirmed && t.hits >= CONFIRM_HITS) {
                t.confirmed = true;
                host.event("New cell " + std::string(lte ? "LTE" : "NR") + " PCI " + std::to_string(s.hit.pci) + " (" + mode +
                               (lte ? ", " + std::to_string(s.est.ports) + (s.est.ports > 1 ? " ports" : " port") : "") + "): " + fmt("%.1f dB", db) +
                               ", " + fmt("%.0f dB above the noise", s.est.snr_db),
                           0.0);
            }
            lte::CellMeta* cm = nullptr;
            if (lte) {
                const auto it = meta_.find(s.id);
                if (it != meta_.end()) cm = &it->second;
            }
            // what the cell's broadcast information brought in this burst
            if (cm) {
                for (const auto& n : cm->news) host.event("LTE PCI " + std::to_string(s.hit.pci) + " " + n, 0.0);
                cm->news.clear();
            }
            if (!t.confirmed) continue;
            confirmed++;
            const Bcast bc = bcast(s, cm);
            const int ports = cm && cm->mib_ok ? cm->mib.ports : s.est.ports;
            const std::string pb = lte ? std::to_string(ports) : std::to_string(s.hit.beams) + (s.hit.beams > 1 ? " beams" : " beam");
            host.table_row(s.id, {s.id, std::to_string(s.hit.pci), mode, bc.op, bc.tac, bc.enb, bc.band, bc.arfcn, bc.bw, pb, fmt("%.1f", db),
                                  fmt("%.0f", s.est.snr_db), fmt("%+.2f", tim), fmt("%.2f", s.est.spread_us)});
            host.table_detail(s.id, details(s, cm, db, tim));
            if (b.m < 2) continue;
            kp::Spatial sp;
            sp.id = s.id;
            sp.label = std::string(lte ? "LTE" : "NR") + " PCI " + std::to_string(s.hit.pci) + (bc.enb.empty() ? "" : " \xC2\xB7 eNB " + bc.enb) +
                       " \xC2\xB7 " + fmt("%.1f dB", db);
            sp.start_s = b.t0;
            sp.end_s = t1;
            sp.m = b.m;
            sp.power_db = db;
            sp.snr_db = s.est.snr_db;
            sp.ref = ref[k] == &s;
            sp.avg_s = SPATIAL_AVG_S;
            sp.cov.resize(s.est.cov.size());
            for (size_t i = 0; i < s.est.cov.size(); i++)
                sp.cov[i] = kp::cf(static_cast<float>(s.est.cov[i].real()), static_cast<float>(s.est.cov[i].imag()));
            host.spatial(sp);
        }
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            if (now - it->second.last_t > TRACK_TTL_S || (!it->second.confirmed && now - it->second.last_t > 5 * PERIOD_S)) {
                if (it->second.confirmed) {
                    host.event("Cell " + std::string(it->second.tech == Tech::LTE ? "LTE" : "NR") + " PCI " + std::to_string(it->second.pci) + " lost", 0.0);
                    host.table_remove(it->first);
                }
                meta_.erase(it->first);
                it = tracks_.erase(it);
            } else {
                ++it;
            }
        }
        if (!seen.empty()) host.valid(b.t0, t1);
        int all = 0;
        for (const auto& [id, t] : tracks_) all += t.confirmed;
        host.fact("Cells", std::to_string(all) + (all != confirmed ? " (" + std::to_string(confirmed) + " in the last burst)" : ""));
        carrier_facts(seen);
        // the sync correlation (PSS fold) around the strongest cell, with a
        // marker per cell
        if (period > 0 && !fold.empty() && fold_rate > 0) {
            const double r = fold_rate / b.rate;   // input samples -> fold samples
            const int pool = std::max(1, (period + 999) / 1000);
            const int s0 = static_cast<int>(std::lround(ref_pos * r)) - period / 2;
            kp::Plot pl;
            pl.title = "Sync correlation (PSS)";
            pl.x_unit = "\xC2\xB5s";
            pl.y_unit = "dB";
            pl.dx = pool / fold_rate * 1e6;
            pl.x0 = -(period / 2) / fold_rate * 1e6;
            for (int i = 0; i < period; i += pool) {
                float mx = 0;
                for (int j = i; j < std::min(period, i + pool); j++) mx = std::max(mx, fold[static_cast<size_t>(((s0 + j) % period + period) % period)]);
                pl.y.push_back(static_cast<float>(10 * std::log10(std::max(mx, 1.0f))));
            }
            pl.y_min = 0;
            for (const auto& s : seen) {
                const auto it = tracks_.find(s.id);
                if (it == tracks_.end() || !it->second.confirmed) continue;
                if (std::lround((s.hit.tech == Tech::LTE ? 5e-3 : 20e-3) * fold_rate) != period) continue;   // the other technology's fold
                const double x = std::remainder((s.hit.pss_mod - ref_pos) * r, static_cast<double>(period)) / fold_rate * 1e6;
                pl.markers.push_back({x, "PCI " + std::to_string(s.hit.pci), s.id});
            }
            host.plot(pl);
        }
    }
};

}  // namespace

KRAKEN_PLUGIN(CellsPlugin,
              {.id = "cells",
               .name = "LTE / 5G NR Cell Decoder",
               .description = "The public broadcast information of every LTE / 5G NR cell on a carrier - LTE: MIB (bandwidth, antenna ports, "
                              "frame number), SIB1 (operator MCC-MNC, tracking area, cell identity / eNB, band, access rules), SIB2 - SIB5 "
                              "(uplink, random access, power, neighbour cells and carriers); NR: cell ID and sync block - and each cell's own "
                              "bearing, told apart by the physical cell ID its signals are scrambled with. Open a row (\xE2\x96\xB8) for all of a "
                              "cell's details",
               .version = "2.0",
               .sample_rate = 2400000,
               .min_vfo_rate = 2000000,
               .author = "KrakenSDR",
               .options = {{"tech", "Technology", "auto", "auto=Auto (LTE + NR 15 kHz)|lte=LTE|nr15=5G NR, 15 kHz SSB|nr30=5G NR, 30 kHz SSB",
                            "Which signals to look for"},
                           {"carrier_mhz", "Carrier / SSB centre (MHz)", "", "",
                            "LTE: the carrier's centre, NR: the SS/PBCH block's centre (empty = search the raster around the VFO centre)"},
                           {"min_snr", "Detection threshold (dB)", "8", "",
                            "A cell's path must stand this far above the estimation floor (noise + the other cells' leakage)"},
                           {"path", "Bearing from", "strongest", "strongest=The strongest path|first=The first path (line of sight)",
                            "Which path of each cell's impulse response gives its bearing"},
                           {"max_cells", "Max cells", "12", "", "At most this many cells per technology"},
                           {"decode", "Cell information", "1", "1=Decode (MIB, SIB1, SIB2 - SIB5)|0=Off (cells + bearings only)",
                            "Read what each LTE cell broadcasts to every phone: its operator, tracking area, cell identity, band, "
                            "neighbours... A carrier wider than the VFO is read in parts - repeats are combined until each message's "
                            "checksum passes (seconds to minutes)"},
                           {"scan", "Find the carrier", "10", "0=Off|5=Within 5 MHz|10=Within 10 MHz|20=Within 20 MHz",
                            "No carrier where the receiver is tuned: retune 1.2 MHz at a time, alternately up and down, up to this far "
                            "until one is found, then centre the receiver on it (also a carrier found next to the tuning). Each "
                            "retune makes the receiver recalibrate for a few seconds; other VFOs move along"}},
               .manual_only = true,
               .talkers = true,
               .lock_vfo = true,
               .array = true,
               .array_burst_s = BURST_S,
               .array_period_s = PERIOD_S,
               .tunes = true})
