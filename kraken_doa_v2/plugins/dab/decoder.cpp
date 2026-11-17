// DAB / DAB+ digital radio (ETSI EN 300 401, transmission mode I) at 2.4 MHz.
//
//  dab_ofdm   the OFDM front end without a resampler (2400-point FFT = the
//             1 kHz carrier spacing; sub-sample window offsets as phase ramps),
//             null symbol / cyclic prefix / phase reference synchronisation,
//             CFO +-410 kHz, D-QPSK soft bits
//  dab_fic    the FIC: de-puncturing, Viterbi, energy dispersal, FIB CRC, the
//             FIGs -> the ensemble database (services, sub-channels, labels,
//             programme types, languages, date / time, announcements, TII database)
//  dab_msc    the selected programme's sub-channel: time de-interleaving, UEP / EEP
//             de-puncturing, Viterbi, energy dispersal -> logical frames
//  dab_audio  (the audio layer: MPEG Layer II / DAB+ superframes, PAD, the dynamic label)
//  dab_tii    transmitter identification in the null symbol: the codes, their
//             level and delay, and (every antenna) each transmitter's own spatial
//             covariance -> a talker packet per transmitter (ID separation in a
//             single frequency network)

#include "dab_audio.hpp"
#include "dab_fic.hpp"
#include "dab_msc.hpp"
#include "dab_ofdm.hpp"
#include "dab_tables.hpp"
#include "dab_tii.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using dab::cf;

// v rounded to the decimals it is printed with, a negative zero made +0 ("-0 Hz" otherwise)
double no_neg_zero(double v, int decimals) {
    const double k = std::pow(10.0, decimals);
    const double r = std::round(v * k) / k;
    return r == 0 ? 0.0 : r;
}
std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string hex(uint32_t v, int digits) {
    char b[16];
    snprintf(b, sizeof b, "%0*X", digits, v);
    return b;
}
std::string talker_id(int eid, int p, int c) {
    char b[32];
    snprintf(b, sizeof b, "%04X-%02d-%02d", eid & 0xFFFF, p, c);
    return b;
}
// civil date from a Modified Julian Date
void mjd_to_date(int mjd, int* y, int* m, int* d) {
    // days since 1970-01-01, then the proleptic Gregorian calendar
    long long z = static_cast<long long>(mjd) - 40587 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yoe + era * 400 + (*m <= 2));
}

class DabDecoder : public kp::Decoder, public dab::AudioSink {
public:
    explicit DabDecoder(kp::Host& h) : Decoder(h), figs_(ens_), audio_(*this) {
        const std::string st = dab::self_test();
        if (!st.empty()) host.log("DAB tables self test FAILED: " + st);
        rx_.on_frame = [this](const dab::Frame& f) { on_frame(f); };
        rx_.on_sync = [this](bool locked) { on_sync(locked); };
        rx_.log = [this](const std::string& s) {
            if (verbose_) host.log(s);
        };
        figs_.ev.event = [this](const std::string& s) { host.event(s, 30); };
        // (▶: the programme being decoded - after Service, so the panel's default sort, by its
        // first column, stays by name)
        host.table_columns({"Service", "▶", "SId", "Type", "kbit/s", "Protection", "Sub-ch", "Programme type", "Language"});
        host.table_keep(3600);
        reset();
    }

    // a retune (the host clears the table, the map and the talkers itself)
    void reset() override {
        rx_.reset();
        clear_state();
        frames_ = 0;
        fic_bits_ = fic_errs_ = 0;
        pk_sent_ = 0;
        snr_ = mer_ = 0;
        host.fact("Sync", "searching for a DAB ensemble");
        // (the host forgets the list on a retune itself; said anyway, and sent again once
        // the services are known)
        if (!choices_sent_.empty()) host.option_choices("service", {});
        choices_sent_.clear();
    }

    // everything learnt from the ensemble, and the programme's decoding
    void clear_state() {
        ens_.clear();
        figs_.clear();
        msc_.clear();
        audio_.configure(dab::SubchannelConfig{});   // nothing: the next service starts from scratch
        pend_cfg_valid_ = false;
        tii_.reset();
        fib_hist_.clear();
        frame_no_ = -1;
        last_start_ = 0;
        cif_abs_ = -1;
        cif_anchored_ = false;
        prev_timing_ok_ = false;
        tx_off_ = 0;
        step_d_ = 0;
        step_n_ = 0;
        msc_next_cif_ = -1;
        held_ = false;
        pv_ = false;
        found_ = false;
        found_at_ = 0;
        country_settled_ = false;
        ecc_at_ = label_at_ = -1;
        ens_country_ = dab::EnsembleCountry();
        announced_.clear();
        rows_.clear();
        tx_announced_.clear();
        talkers_sent_.clear();
        map_ids_.clear();
        sel_sid_ = 0;
        sel_subch_ = -1;
        prog_announced_ = 0xFFFFFFFFu;
        sel_valid_ = false;
        sel_label_.clear();
        last_ui_ = -1e9;
        last_good_fic_ = -1e9;
        lf_ok_ = lf_lost_ = 0;
        msc_bits_ = msc_errs_ = 0;
        reconf_pending_ = false;
        reconf_cif_ = 0;
        reconf_done_cif_ = INT64_MIN / 2;
        cand_eid_ = -1;
        cand_n_ = 0;
        want_.assign(72, 0);
        rx_.want_symbols(want_);
    }

    // another ensemble on the block (FIG 0/0 with a new EId, confirmed):
    // what the old one put on the screen goes, then a cold start of the
    // ensemble (the OFDM sync is kept)
    void drop_ensemble(int new_eid) {
        const std::string old = ens_label();
        for (const auto& kv : rows_) host.table_remove(kv.first);
        for (const auto& id : map_ids_) host.map_remove(id);
        for (const auto& id : talkers_sent_) host.talker_forget(id);
        for (const char* k : {"Ensemble", "Country", "Date / time", "Services", "Alarm announcements", "Programme", "Sub-channel", "Codec",
                              "Voice codec", "Audio frames", "RS corrections", "Text", "Transmitters (TII)"})
            host.fact(k, "");
        host.voice_state("");
        if (ens_.eid >= 0)
            host.event("Ensemble lost: " + old + " (another ensemble, EId " + hex(static_cast<uint32_t>(new_eid), 4) + ", on the block)", 5);
        clear_state();
        update_choices();   // the old ensemble's programmes go from the list
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "service") {
            service_opt_ = value;
            sel_valid_ = false;
            select_service();
            update_choices();
        } else if (key == "verbose") {
            verbose_ = value == "1";
        } else if (key == "log_raw") {
            // host.raw_wanted() covers it
        }
    }

    void process(const cf* x, size_t n) override {
        t0_ = host.time() - static_cast<double>(rx_.samples_in()) / dab::FS;
        const cf* p[1] = {x};
        const auto a = Clock::now();
        rx_.push(p, 1, n);
        cpu_total_ += secs(a);
        m_ = 1;
        idle_ui();
    }

    void process_multi(const cf* const* x, int m, size_t n) override {
        t0_ = host.time() - static_cast<double>(rx_.samples_in()) / dab::FS;
        const auto a = Clock::now();
        rx_.push(x, m, n);
        cpu_total_ += secs(a);
        m_ = m;
        idle_ui();
    }

    // facts / events while no frames come (searching)
    void idle_ui() {
        const double now = host.time();
        if (rx_.locked() || now - last_ui_ < 1.0) return;
        last_ui_ = now;
        update_ui(now);
    }

    // ---- dab::AudioSink
    void pcm(const float* mono, size_t n) override { host.audio(mono, n); }
    void dynamic_label(const std::string& utf8) override {
        if (utf8.empty()) {   // the broadcaster cleared the display
            host.fact("Text", "");
            return;
        }
        const std::string t = dab::printable_utf8(utf8);
        host.fact("Text", t);
        host.event("💬 " + (sel_label_.empty() ? std::string("") : dab::printable_utf8(sel_label_) + ": ") + t, 900);
    }
    void log(const std::string& text) override { host.log(text); }

private:
    dab::OfdmRx rx_;
    dab::FicDecoder fic_;
    dab::Ensemble ens_;
    dab::FigParser figs_;
    dab::MscDecoder msc_;
    dab::SubchannelDecoder audio_;
    dab::Tii tii_;
    std::vector<dab::TiiPacket> pk_;
    std::vector<uint8_t> want_;   // the MSC symbols asked of the front end

    double t0_ = 0;   // host time of input sample 0
    int m_ = 1;
    bool verbose_ = false;
    std::string service_opt_;
    std::string choices_sent_;   // the Programme list as last sent (update_choices), serialised
    // FIC statistics: (frame time, good, total) of the last 10 s
    std::deque<std::pair<double, int>> fib_hist_;
    int64_t frame_no_ = -1;
    int64_t last_start_ = 0;
    uint64_t frames_ = 0;
    bool found_ = false;
    double found_at_ = 0, last_ui_ = -1e9, last_good_fic_ = -1e9;
    // the countries (update_country)
    bool country_settled_ = false;
    double ecc_at_ = -1, label_at_ = -1;   // first update with the ECC / the ensemble label
    dab::EnsembleCountry ens_country_;
    static constexpr double COUNTRY_QUIET_S = 0.5, COUNTRY_WAIT_S = 3;
    std::set<uint32_t> announced_;
    // the services table as sent: key -> its text, label, when sent
    struct Row {
        std::string text, label;
        double sent = 0;
    };
    std::map<std::string, Row> rows_;
    std::set<int> tx_announced_;
    std::set<std::string> map_ids_, talkers_sent_;
    uint32_t sel_sid_ = 0, prog_announced_ = 0xFFFFFFFFu;
    bool sel_valid_ = false;
    std::string sel_label_;
    int sel_subch_ = -1;
    uint64_t lf_ok_ = 0, lf_lost_ = 0, pk_sent_ = 0;
    uint64_t msc_bits_ = 0, msc_errs_ = 0, fic_bits_ = 0, fic_errs_ = 0;
    double snr_ = 0, mer_ = 0, cfo_ = 0, ppm_ = 0;
    // CIF numbering: the absolute index of the latest frame's first CIF (from
    // the FIG 0/0 CIF count, else the frame's distance in samples)
    int64_t cif_abs_ = -1;
    bool cif_anchored_ = false;    // tx_off_ is known (a FIG 0/0 CIF count was seen)
    int64_t tx_off_ = 0;           // the FIG 0/0 CIF count = cif_abs_ + tx_off_ (mod 5000)
    bool prev_timing_ok_ = false;  // the previous frame's start was where the tracking expected it
    // a step of the transmitted count with contiguous frames: the new offset (relative) and the
    // frames in a row it held for
    int64_t step_d_ = 0;
    int step_n_ = 0;
    static constexpr int COUNT_CONFIRM = 3;
    int64_t msc_next_cif_ = -1;    // the next CIF the MSC decoder expects
    // the latest frame's MSC soft bits, held until the next frame (see on_frame)
    bool held_ = false;
    int64_t held_cif_ = 0;
    std::vector<int8_t> held_msc_;
    std::vector<uint8_t> held_ok_;
    // the previous frame's valid span, sent once the next frame shows the input stayed contiguous
    bool pv_ = false;
    double pv_a_ = 0, pv_b_ = 0;
    // multiplex reconfiguration: the absolute CIF of the next configuration
    bool reconf_pending_ = false;
    int64_t reconf_cif_ = 0, reconf_done_cif_ = 0;
    // a new configuration's bit rate reaches the audio layer with its first logical frame
    dab::SubchannelConfig pend_cfg_;
    bool pend_cfg_valid_ = false;
    // a FIG 0/0 with another EId: switch after EID_CONFIRM frames in a row
    int cand_eid_ = -1, cand_n_ = 0;
    static constexpr int EID_CONFIRM = 3;
    static constexpr double CONTIG_ERR = 400;     // samples: a frame start further off = the input jumped
    static constexpr int64_t MAX_GAP_CIFS = 250;  // CIFs lost in an outage beyond this (6 s): start the sub-channel over
    static constexpr int ROW_BUDGET = 32;         // table rows sent per second at most
    static constexpr double ROW_REFRESH_S = 15;   // an unchanged row is sent again after this long
    static constexpr int SERVICE_EVENT_BUDGET = 8;   // service found / gone events per second at most

    // CPU time per stage (verbose fact)
    using Clock = std::chrono::steady_clock;
    static double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }
    double cpu_total_ = 0, cpu_fic_ = 0, cpu_msc_ = 0, cpu_tii_ = 0, cpu_ui_ = 0;

    double t_of(double sample) const { return t0_ + sample / dab::FS; }
    double now_s() const { return host.time(); }

    void on_sync(bool locked) {
        if (locked) {
            host.fact("Sync", "locked (frame timing, carrier offset)");
            return;
        }
        // a gap in the input or a fade: the receiver searches again; the
        // "lost" event comes from update_ui when the FIC stays away for 3 s
        host.fact("Sync", "lost - searching");
        prev_timing_ok_ = false;   // (the next frame comes from a new search)
    }

    std::string ens_label() const {
        std::string s = ens_.label.empty() ? std::string("(no label yet)") : dab::printable_utf8(ens_.label);
        if (ens_.eid >= 0) s += " (EId " + hex(static_cast<uint32_t>(ens_.eid), 4) + ")";
        return s;
    }

    double fic_ratio(int* good, int* total) const {
        int g = 0, t = 0;
        for (const auto& h : fib_hist_) {
            g += h.second;
            t += 12;
        }
        if (good) *good = g;
        if (total) *total = t;
        return t ? static_cast<double>(g) / t : 0;
    }

    bool fic_decodes() const { return ens_.eid >= 0 && now_s() - last_good_fic_ < 1.0; }
    // the services table's ▶: the service whose sub-channel the MSC decodes now, 0 = none
    // (nothing matches, conditional access, no coding profile, no FIC for 3 s - the
    // "Ensemble lost" limit). What is decoded, not the option's text ("" or a part of a name)
    uint32_t decoded_sid() const { return msc_.active() && now_s() - last_good_fic_ <= 3 ? sel_sid_ : 0; }

    void on_frame(const dab::Frame& f) {
        const double tf = t_of(static_cast<double>(f.start));
        frames_++;
        // frames since the previous one, by the sample count (the FIG 0/0 CIF count corrects it below)
        const int64_t nf = frame_no_ < 0 ? 0 : std::max<int64_t>(1, std::llround(static_cast<double>(f.start - last_start_) / dab::FRAME));
        frame_no_ = frame_no_ < 0 ? 0 : frame_no_ + nf;
        last_start_ = f.start;
        snr_ = f.snr_db;
        mer_ = f.mer_db;
        cfo_ = f.cfo_hz;
        ppm_ = f.clock_ppm;

        // ---- FIC
        auto ta = Clock::now();
        uint8_t fibs[12][32];
        bool ok[12];
        const int good = fic_.decode(f.fic, fibs, ok);
        fic_bits_ += static_cast<uint64_t>(fic_.last_bits);
        fic_errs_ += static_cast<uint64_t>(fic_.last_bit_errors);
        fib_hist_.push_back({tf, good});
        while (!fib_hist_.empty() && tf - fib_hist_.front().first > 10) fib_hist_.pop_front();
        const int cif_before = ens_.cif_count;
        figs_.begin_frame();
        for (int i = 0; i < 12; i++) {
            if (!ok[i]) continue;
            figs_.fib(fibs[i], i / 3, tf);
            if (host.raw_wanted()) {
                std::string s = "FIB ";
                s += (ens_.cif_count >= 0 ? std::to_string((ens_.cif_count + i / 3) % 5000) : std::string("?")) + "." +
                     std::to_string(i % 3) + " ";
                for (int b = 0; b < 30; b++) s += hex(fibs[i][b], 2);
                host.raw(s);
            }
            if (figs_.foreign_eid >= 0) break;   // the rest of the frame is the other ensemble's
        }
        // ---- another ensemble? (a FIG 0/0 with a new EId in EID_CONFIRM frames in a row: one
        // corrupted FIG that passes its CRC must not throw the ensemble away)
        if (figs_.foreign_eid >= 0) {
            if (figs_.foreign_eid == cand_eid_) cand_n_++;
            else {
                cand_eid_ = figs_.foreign_eid;
                cand_n_ = 1;
            }
            if (cand_n_ >= EID_CONFIRM) {
                drop_ensemble(cand_eid_);
                cpu_fic_ += secs(ta);
                return;   // the next frame's FIG 0/0 starts the new ensemble
            }
        } else if (figs_.frame_cif >= 0) {
            cand_n_ = 0;
        }
        if (ens_.cif_count == cif_before && cif_before >= 0) ens_.cif_count = (cif_before + 4) % 5000;   // no FIG 0/0 this time

        // ---- CIF numbering. cif_abs_ counts the CIFs that passed; the FIG 0/0 CIF count is
        // cif_abs_ + tx_off_ (mod 5000). While the frames follow each other in the samples the
        // sample count is the authority for how many CIFs passed: a transmitted count that
        // steps (a looped recording, a multiplexer restart) only moves tx_off_ - once the new
        // offset holds for COUNT_CONFIRM frames in a row (one corrupted count is ignored); the
        // CIFs stay contiguous for the de-interleaver. Across a gap in the input (the frame
        // timing not contiguous) the count says how many CIFs passed.
        int64_t cif = cif_abs_ < 0 ? 0 : cif_abs_ + 4 * nf;
        // the frame start where the tracking expected it, and the previous frame's too (no
        // failed phase reference, no new search in between): the samples say "the next frame"
        const bool timing_ok = f.prs_ok && std::isfinite(f.timing_err) && std::fabs(f.timing_err) < CONTIG_ERR;
        const bool next_frame = cif_abs_ >= 0 && nf == 1 && timing_ok && prev_timing_ok_;
        prev_timing_ok_ = timing_ok;
        bool count_agrees = true;   // (no count in this frame: nothing against it)
        if (figs_.frame_cif >= 0) {
            const int64_t c = figs_.frame_cif;
            if (cif_abs_ < 0) {
                cif = c;
                tx_off_ = 0;
                step_n_ = 0;
            } else {
                int64_t d = ((c - (cif + tx_off_)) % 5000 + 5000) % 5000;   // the count nearest the expected one
                if (d >= 2500) d -= 5000;
                if (!cif_anchored_) {
                    tx_off_ += d;   // the first count (the first frames had none): just the offset
                    step_n_ = 0;
                } else if (d == 0) {
                    step_n_ = 0;
                } else if (next_frame) {
                    if (step_n_ > 0 && d == step_d_) {
                        step_n_++;
                    } else {
                        step_d_ = d;
                        step_n_ = 1;
                    }
                    if (step_n_ >= COUNT_CONFIRM) count_step(d);
                    // (until it is confirmed the frame's position is in doubt - an input that
                    // dropped exactly whole frames looks the same: no valid span, no talker)
                    else count_agrees = false;
                } else if (cif + d > cif_abs_) {
                    cif += d;   // a gap in the input: these CIFs were lost
                    step_n_ = 0;
                }
            }
            cif_anchored_ = true;
        }
        const bool consecutive = cif_abs_ >= 0 && cif == cif_abs_ + 4;
        cif_abs_ = cif;
        // the input is contiguous from the previous frame's phase reference to this one's
        const bool contig = consecutive && timing_ok && count_agrees;
        // the previous frame's span: valid when this one shows that no samples went missing in it
        if (pv_ && contig) host.valid(pv_a_, pv_b_);
        pv_ = false;
        if (good >= 2) {
            last_good_fic_ = tf;
            if (contig) {   // (and its null symbol is contiguous with the previous frame)
                pv_ = true;
                pv_a_ = tf;
                pv_b_ = t_of(static_cast<double>(f.start + dab::FRAME));
            }
        }
        if (verbose_) {
            char b[200];
            snprintf(b, sizeof b, "frame %lld (CIF %lld): PRS %.2f%s, timing %+.1f, SNR %.1f dB, MER %.1f dB, CFO %+.1f Hz, %d/12 FIBs ok",
                     static_cast<long long>(frame_no_), static_cast<long long>(cif), f.prs_metric, f.prs_ok ? "" : " (failed)",
                     std::isfinite(f.timing_err) ? f.timing_err : 0.0, f.snr_db, f.mer_db, f.cfo_hz, good);
            host.log(b);
        }

        // ---- reconfiguration announced (FIG 0/0 change flags + occurrence change, clause 6.5):
        // the next configuration applies from the earliest CIF after the announcing one whose
        // CIF count's lower part equals the occurrence change
        if (figs_.frame_cif >= 0) {
            const int64_t at = cif + figs_.frame_fig00_at;   // the announcing CIF
            // (a pending one that is due by now is applied below, not moved 250 CIFs on)
            if (figs_.frame_flags && figs_.frame_occ >= 0 && !(reconf_pending_ && at >= reconf_cif_)) {
                const int lower = static_cast<int>((figs_.frame_cif + figs_.frame_fig00_at) % 5000 % 250);
                int d = (figs_.frame_occ - lower + 250) % 250;
                if (d == 0) d = 250;
                // (a distance from the announcing CIF: right whatever the transmitted count's offset)
                const int64_t target = at + d;
                // (a configuration stays for at least 250 CIFs: an announcement for less after the last change is stale)
                if (target - reconf_done_cif_ >= 250 && (!reconf_pending_ || target != reconf_cif_)) {
                    if (!reconf_pending_)
                        host.event("Multiplex reconfiguration announced, in " + fmt("%.2f s", static_cast<double>(target - at) * 0.024) +
                                       " (CIF count " + std::to_string(((target + tx_off_) % 5000 + 5000) % 5000) + ")",
                                   30);
                    reconf_pending_ = true;
                    reconf_cif_ = target;
                }
            } else if (!figs_.frame_flags && reconf_pending_ && at < reconf_cif_) {
                reconf_pending_ = false;   // withdrawn before it happened
                host.event("Multiplex reconfiguration withdrawn", 30);
            }
        }

        cpu_fic_ += secs(ta);
        ta = Clock::now();
        // ---- the selected programme's sub-channel. A frame's CIFs go to the de-interleaver one
        // frame late: only the next frame shows whether the input stayed contiguous through this
        // one (a frame that straddles a jump in the input - samples dropped by the host - has the
        // wrong samples after the jump: its CIFs are erasures then, not data)
        if (held_) {
            held_ = false;
            const bool jumped = cif != held_cif_ + 4 || (std::isfinite(f.timing_err) && std::fabs(f.timing_err) >= CONTIG_ERR);
            for (int c = 0; c < 4; c++) step_cif(held_cif_ + c, jumped ? nullptr : held_msc_.data(), held_ok_.data(), c);
            msc_next_cif_ = held_cif_ + 4;
        }
        if (msc_.active() && msc_next_cif_ >= 0 && cif > msc_next_cif_) {
            // CIFs skipped (frames lost in an outage): erasures, so that the audio layer keeps its
            // timeline - after a long outage the sub-channel starts over instead
            if (cif - msc_next_cif_ > MAX_GAP_CIFS) {
                msc_.reset();
                audio_.reset();
                lf_lost_ += static_cast<uint64_t>(cif - msc_next_cif_);
            } else {
                for (int64_t k = msc_next_cif_; k < cif; k++) step_cif(k, nullptr, nullptr, 0);
            }
        }
        if (msc_.active()) {
            // hold this frame's soft bits (the symbols computed) until the next frame
            held_msc_.resize(static_cast<size_t>(72) * dab::SYM_BITS);
            held_ok_.assign(72, 0);
            if (f.prs_ok)
                for (int s = 0; s < 72; s++)
                    if (f.msc_ok[s]) {
                        held_ok_[static_cast<size_t>(s)] = 1;
                        std::memcpy(&held_msc_[static_cast<size_t>(s) * dab::SYM_BITS], f.msc + static_cast<size_t>(s) * dab::SYM_BITS, dab::SYM_BITS);
                    }
            held_ = true;
            held_cif_ = cif;
            msc_next_cif_ = cif;
        } else {
            for (int c = 0; c < 4; c++) step_cif(cif + c, nullptr, nullptr, c);   // (a reconfiguration due)
        }
        update_want();

        cpu_msc_ += secs(ta);
        ta = Clock::now();
        // ---- TII
        if (f.null_m >= 1) {
            // (the null symbol lies between the previous frame's phase reference and this one's:
            // with a jump in the input there its samples are not where the time says)
            const bool talk = f.null_m >= 2 && fic_decodes() && contig;
            tii_.process(f.null_car, f.null_m, tf, talk, pk_);
            if (talk) {
                const double ts = t_of(static_cast<double>(f.null_w0)), te = t_of(static_cast<double>(f.null_w0 + dab::NFFT));
                for (const auto& p : pk_) {
                    kp::Talker t;
                    t.id = talker_id(ens_.eid, p.p, p.c);
                    t.label = tx_label(p.p, p.c);
                    t.start_s = ts;
                    t.end_s = te;
                    t.packet = true;
                    t.avg_s = 10;
                    t.keep_s = 1800;
                    t.m = p.m;
                    t.cov = p.cov;
                    host.talker(t);
                    talkers_sent_.insert(t.id);
                    pk_sent_++;
                }
            }
            for (int key : tii_.take_dropped()) {
                if (tx_announced_.erase(key) && ens_.eid >= 0)
                    host.event("📡 Transmitter " + talker_id(ens_.eid, key / 32, key % 32) + " no longer received", 60);
            }
        }

        cpu_tii_ += secs(ta);
        if (tf - last_ui_ >= 1.0) {
            ta = Clock::now();
            last_ui_ = tf;
            update_ui(tf);
            cpu_ui_ += secs(ta);
            if (verbose_ && tf > t0_) {
                const double el = static_cast<double>(rx_.samples_in()) / dab::FS;
                char b[200];
                snprintf(b, sizeof b, "%.1f %% of real time: front end %.1f, FIC %.1f, MSC %.1f, TII %.1f, facts %.1f", 100 * cpu_total_ / el,
                         100 * (cpu_total_ - cpu_fic_ - cpu_msc_ - cpu_tii_ - cpu_ui_) / el, 100 * cpu_fic_ / el, 100 * cpu_msc_ / el,
                         100 * cpu_tii_ / el, 100 * cpu_ui_ / el);
                host.fact("CPU", b);
            }
        }
    }

    std::string tx_label(int p, int c) const {
        std::string s = ens_.label.empty() ? std::string("DAB") : dab::printable_utf8(ens_.label);
        double lat, lon;
        if (tx_position(p, c, &lat, &lon)) {
            double slat, slon;
            if (host.station(&slat, &slon)) {
                const double d = distance_km(slat, slon, lat, lon);
                s += fmt(" · %.0f km", d);
            }
        }
        return dab::utf8_truncate(s, 64);
    }

    bool tx_position(int p, int c, double* lat, double* lon) const {
        auto m = ens_.tii_main.find(p);
        auto s = ens_.tii_sub.find(p * 32 + c);
        if (m == ens_.tii_main.end() || s == ens_.tii_sub.end()) return false;
        *lat = m->second.lat + s->second.lat;
        *lon = m->second.lon + s->second.lon;
        return std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180;
    }

    static double distance_km(double la1, double lo1, double la2, double lo2) {
        const double r = 6371.0, d2r = 0.017453292519943295;
        const double dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
        const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                         std::cos(la1 * d2r) * std::cos(la2 * d2r) * std::sin(dlo / 2) * std::sin(dlo / 2);
        return 2 * r * std::asin(std::min(1.0, std::sqrt(a)));
    }

    // The transmitted CIF count stepped by d with the input contiguous, confirmed: re-anchor
    // (nothing lost, nothing reset - the CIFs went on without a break).
    void count_step(int64_t d) {
        tx_off_ += d;
        step_n_ = 0;
        // (a pending reconfiguration needs nothing: its target is a distance from the
        // announcing CIF, and the transmitter announces in every frame's FIG 0/0 up to the
        // change - the first announcement under the new count sets the target by the new
        // count's lower part, as clause 6.5 defines it; flags 00 under the new count withdraw it)
        if (verbose_)
            host.log("CIF count stepped by " + std::to_string(static_cast<long long>(d)) +
                     " with the frames contiguous: renumbered (nothing lost)");
    }

    // ---------------------------------------------------------------- the selected service
    // CIF k (CIF index c of a held frame's soft bits msc / symbols computed ok; nullptr = not
    // received): a reconfiguration due from it on, then the sub-channel's part of it
    void step_cif(int64_t k, const int8_t* msc, const uint8_t* ok, int c) {
        if (reconf_pending_ && k >= reconf_cif_) apply_reconfiguration(k);
        if (!msc_.active()) return;
        int start = 0, size = 0;
        msc_.alloc(k, &start, &size);
        const int8_t* bits = nullptr;
        if (msc && ok && size > 0) {
            const int o0 = 64 * start, o1 = 64 * (start + size) - 1;
            bool have = true;
            for (int s = o0 / dab::SYM_BITS; have && s <= o1 / dab::SYM_BITS; s++) have = ok[18 * c + s] != 0;
            if (have) bits = msc + static_cast<size_t>(18 * c) * dab::SYM_BITS + o0;
        }
        push_cif(bits, k);
    }

    // One CIF of the sub-channel to the MSC decoder, the logical frame it completes to the audio layer
    void push_cif(const int8_t* bits, int64_t k) {
        const int r = msc_.push_cif(bits, k);
        if (r > 0) {
            lf_ok_++;
            msc_bits_ += static_cast<uint64_t>(msc_.last_bits);
            msc_errs_ += static_cast<uint64_t>(msc_.last_bit_errors);
            // a reconfiguration's new bit rate: from its first logical frame on
            if (pend_cfg_valid_ && msc_.frame_bytes() == pend_cfg_.bitrate_kbps * 3) {
                audio_.configure(pend_cfg_);
                pend_cfg_valid_ = false;
            }
            audio_.push_frame(msc_.frame(), static_cast<size_t>(msc_.frame_bytes()), host.voice_wanted());
        } else if (r < 0) {
            lf_lost_++;
            audio_.push_lost();
        }
    }

    // the OFDM symbols the sub-channel needs next frame: its allocation, and the
    // next one while a reconfiguration is pending
    void update_want() {
        std::vector<uint8_t> want72(72, 0);
        auto add = [&want72](int start, int size) {
            if (size <= 0) return;
            const int s0 = 64 * start / dab::SYM_BITS, s1 = (64 * (start + size) - 1) / dab::SYM_BITS;
            for (int cc = 0; cc < 4; cc++)
                for (int s = s0; s <= s1 && s < 18; s++) want72[static_cast<size_t>(18 * cc + s)] = 1;
        };
        if (msc_.active()) {
            int st = 0, sz = 0;
            msc_.alloc(-1, &st, &sz);   // the current allocation
            add(st, sz);
            msc_.alloc(INT64_MAX, &st, &sz);   // a scheduled one
            add(st, sz);
            if (reconf_pending_ && sel_subch_ >= 0) {
                auto it = ens_.next_subch.find(sel_subch_);
                if (it != ens_.next_subch.end()) add(it->second.start, it->second.prot.size_cu);
            }
        }
        if (want72 != want_) {
            want_ = want72;
            rx_.want_symbols(want72);
        }
    }

    // the next configuration becomes the current one at CIF k (the first CIF of the new configuration)
    void apply_reconfiguration(int64_t k) {
        reconf_pending_ = false;
        reconf_done_cif_ = reconf_cif_;
        for (const auto& kv : ens_.next_subch) ens_.subch[kv.first] = kv.second;
        for (const auto& kv : ens_.next_services) {
            auto it = ens_.services.find(kv.first);
            if (it == ens_.services.end()) {
                if (ens_.services.size() < dab::MAX_SERVICES) ens_.services[kv.first] = kv.second;
            } else {
                it->second.comps = kv.second.comps;
                it->second.last_seen = std::max(it->second.last_seen, kv.second.last_seen);
            }
        }
        const bool had = !ens_.next_subch.empty() || !ens_.next_services.empty();
        ens_.next_subch.clear();
        ens_.next_services.clear();
        host.event(std::string("Multiplex reconfiguration") + (had ? "" : " (the next configuration was not received)"), 30);
        select_service(k);
    }

    // a service's audio component, if it has one with a known sub-channel (the services the
    // Programme option can select)
    const dab::Component* audio_comp(const dab::Service& s) const {
        const dab::Component* c = s.primary();
        if (!c || c->tmid != 0 || !ens_.subch.count(c->subch)) return nullptr;
        return c;
    }

    // The Programme option's run-time choices (kp::Host::option_choices): "" = the first
    // audio service, then every audio service by name, its SId as the value (stable, what the
    // option matches first, and the key of the service's table row - so a click on the row
    // selects it; the option has no control of its own, kp::Option::hidden). Only the
    // services: a setting that is none of them (a part of a name, a SId in lower case or of
    // another ensemble) has no entry - the Programme fact says what it selects or that
    // nothing matches, and a kraken_doa page from before hidden options, which still draws
    // the list, adds the setting as an entry itself. Sent on a change.
    void update_choices() {
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> by_name;   // sort key, (value, label)
        const dab::Service* first = nullptr;
        auto name_of = [](const dab::Service& s) {
            return s.label.empty() ? "SId " + hex(s.sid, s.data ? 8 : 4) : dab::printable_utf8(s.label);
        };
        for (const auto& kv : ens_.services) {   // (by SId: the first is the lowest)
            const dab::Service& s = kv.second;
            const dab::Component* c = audio_comp(s);
            if (!c) continue;
            if (!first) first = &s;
            const std::string name = name_of(s);
            std::string l = name + " · " + (c->type == 63 ? "DAB+" : c->type == 0 ? "DAB" : "audio") + " " +
                            std::to_string(ens_.subch.at(c->subch).prot.kbps) + "k";
            if (c->ca || s.ca_id) l += " · conditional access";
            std::string key = name;
            for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            by_name.push_back({key + '\x1f' + hex(s.sid, 8), {hex(s.sid, s.data ? 8 : 4), dab::utf8_truncate(l, 80)}});
        }
        std::vector<std::pair<std::string, std::string>> list;
        if (first) {
            std::sort(by_name.begin(), by_name.end());
            list.push_back({"", "First audio service (" + dab::utf8_truncate(name_of(*first), 48) + ")"});
            for (auto& x : by_name) list.push_back(std::move(x.second));
        }
        std::string all;
        for (const auto& [v, l] : list) all += v + '\x1e' + l + '\x1f';
        if (all == choices_sent_) return;
        choices_sent_ = all;
        host.option_choices("service", list);
    }

    // at_cif >= 0: called for a multiplex reconfiguration taking effect at that CIF - the same
    // service on the same sub-channel follows it without a break
    void select_service(int64_t at_cif = -1) {
        // audio services with a known sub-channel, by SId
        const dab::Service* pick = nullptr;
        std::string want = service_opt_;
        while (!want.empty() && want.back() == ' ') want.pop_back();
        while (!want.empty() && want.front() == ' ') want.erase(want.begin());
        auto lower = [](std::string s) {
            for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        bool is_hex = !want.empty() && want.size() <= 8;
        for (char ch : want) is_hex = is_hex && std::isxdigit(static_cast<unsigned char>(ch));
        for (const auto& kv : ens_.services) {
            const dab::Service& s = kv.second;
            if (!audio_comp(s)) continue;
            if (want.empty()) {
                pick = &s;
                break;
            }
            if (is_hex && std::strtoul(want.c_str(), nullptr, 16) == s.sid) {
                pick = &s;
                break;
            }
        }
        if (!pick && !want.empty())
            for (const auto& kv : ens_.services)
                if (audio_comp(kv.second) && lower(kv.second.label).find(lower(want)) != std::string::npos) {
                    pick = &kv.second;
                    break;
                }
        if (!pick) {
            if (msc_.active() || sel_subch_ >= 0 || sel_sid_) new_programme();
            sel_sid_ = 0;
            sel_label_.clear();
            sel_subch_ = -1;
            return;
        }
        const dab::Component* c = audio_comp(*pick);
        const dab::SubChannel& sc = ens_.subch.at(c->subch);
        sel_label_ = pick->label.empty() ? hex(pick->sid, 4) : pick->label;
        if (c->ca) {   // conditional access: not decoded
            if (msc_.active() || sel_sid_ != pick->sid) new_programme();
            sel_sid_ = pick->sid;
            sel_subch_ = -1;
            return;
        }
        dab::SubchannelConfig cfg;
        cfg.dabplus = c->type == 63;
        cfg.bitrate_kbps = sc.prot.kbps;
        cfg.sid = pick->sid;
        cfg.subch = sc.id;
        const bool same_stream = msc_.active() && sel_sid_ == pick->sid && sel_subch_ == sc.id;
        if (same_stream && msc_.start_cu() == sc.start && msc_.protection() == sc.prot) return;
        // (a reconfiguration on its way: the database may show the new configuration a little
        // before the MSC reaches its first CIF - it follows then, without a break)
        if (same_stream && at_cif < 0 && reconf_pending_) return;
        if (same_stream && at_cif >= 0) {
            // the reconfiguration moves / resizes / re-protects this sub-channel (annex D): the
            // de-interleaver and the audio layer carry on
            if (msc_.reconfigure(sc.start, sc.prot, at_cif)) {
                pend_cfg_ = cfg;
                pend_cfg_valid_ = true;
                update_want();
                return;
            }
            host.log("sub-channel " + std::to_string(sc.id) + ": no coding profile");
            new_programme();
            return;
        }
        // another service or sub-channel (or an unannounced change): start from scratch
        new_programme();
        sel_sid_ = pick->sid;
        sel_subch_ = sc.id;
        if (!msc_.configure(sc.start, sc.prot)) {
            host.log("sub-channel " + std::to_string(sc.id) + ": no coding profile");
            return;
        }
        audio_.configure(cfg);
        update_want();
    }

    // the programme changes: nothing of the previous one's decoding or facts stays
    void new_programme() {
        msc_.clear();
        msc_next_cif_ = -1;
        held_ = false;
        audio_.configure(dab::SubchannelConfig{});
        pend_cfg_valid_ = false;
        lf_ok_ = lf_lost_ = msc_bits_ = msc_errs_ = 0;
        clear_programme_facts();
        update_want();
    }

    // what describes the programme being decoded (not the Programme fact itself)
    void clear_programme_facts() {
        for (const char* k : {"Text", "Codec", "Voice codec", "Audio frames", "RS corrections", "Sub-channel"}) host.fact(k, "");
    }

    std::string type_of(const dab::Service& s, const dab::Component* c) const {
        if (!c) return "";
        std::string t;
        if (c->tmid == 0) t = c->type == 63 ? "DAB+" : c->type == 0 ? "DAB" : "audio type " + std::to_string(c->type);
        else if (c->tmid == 1) t = "stream data (" + dab::dscty_name(c->type) + ")";
        else if (c->tmid == 3) t = "packet data (" + dab::dscty_name(c->type) + ")";
        else t = "reserved transport";
        for (int ua : c->uas)
            if (ua == 0x009) t = "DMB (not decoded)";
        if (c->ca || s.ca_id) t += ", conditional access";
        return t;
    }

    // ---------------------------------------------------------------- facts, table, events
    // The ensemble's country (dab::ensemble_country) is decided once the service list
    // is complete by FIG 0/7, or has stopped growing for COUNTRY_QUIET_S with at least
    // MIN_COUNTRY_SERVICES services (or COUNTRY_WAIT_S after the ECC came),
    // so that it doesn't change while the FIGs arrive; then it follows the database (a
    // change needs other services). Before that, no Country fact or service country.
    void update_country(double now) {
        if (ens_.ecc < 0 || ens_.eid < 0) return;
        if (ecc_at_ < 0) ecc_at_ = now;
        if (!country_settled_) {
            int n = 0;
            double newest = -1e9;
            for (const auto& kv : ens_.services)
                if (!kv.second.comps.empty()) {
                    n++;
                    newest = std::max(newest, kv.second.first_seen);
                }
            // (with fewer services than the ensemble verdict needs, a quiet list isn't enough:
            // services trickling in through a poor FIC would change the verdict later)
            const bool all = ens_.services_total > 0 && n >= ens_.services_total;
            country_settled_ = (n > 0 && (all || (now - newest >= COUNTRY_QUIET_S && n >= dab::MIN_COUNTRY_SERVICES))) ||
                               now - ecc_at_ >= COUNTRY_WAIT_S;
            if (!country_settled_) return;
        }
        ens_country_ = dab::ensemble_country(ens_, ens_country_.from_services ? ens_country_.base : std::string());
    }

    void update_ui(double now) {
        // purge what the MCI stopped describing (the current and the next configuration)
        for (auto* m : {&ens_.subch, &ens_.next_subch})
            for (auto it = m->begin(); it != m->end();) it = now - it->second.seen > 30 ? m->erase(it) : std::next(it);
        for (auto* m : {&ens_.services, &ens_.next_services})
            for (auto it = m->begin(); it != m->end();) {
                const bool stale = it->second.last_seen > 0 ? now - it->second.last_seen > 30 : now - it->second.first_seen > 30;
                it = stale ? m->erase(it) : std::next(it);
            }
        // the table and the announced set follow the database (services also go when a full
        // database makes room for new ones)
        int ev_budget = SERVICE_EVENT_BUDGET;
        std::set<std::string> keys;
        for (const auto& kv : ens_.services)
            if (!kv.second.comps.empty()) keys.insert(hex(kv.second.sid, kv.second.data ? 8 : 4));
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (keys.count(it->first)) {
                ++it;
                continue;
            }
            host.table_remove(it->first);
            if (!it->second.label.empty() && ev_budget > 0) {
                ev_budget--;
                host.event("Service gone: " + it->second.label + " (SId " + it->first + ")", 60);
            }
            it = rows_.erase(it);
        }
        for (auto it = announced_.begin(); it != announced_.end();) it = ens_.services.count(*it) ? std::next(it) : announced_.erase(it);
        const bool fic_ok = fic_decodes();
        int g = 0, tot = 0;
        const double q = fic_ratio(&g, &tot);
        // signal facts
        if (rx_.locked()) {
            host.fact("SNR", fmt("%.1f dB", snr_) + fmt(" (MER of the FIC symbols %.1f dB)", mer_));
            const double rf = host.rf_hz();
            std::string fo = fmt("%+.0f Hz", no_neg_zero(cfo_, 0));
            if (std::fabs(cfo_) >= 1000) fo += fmt(" (the ensemble is %.0f kHz off the VFO centre)", cfo_ / 1000);
            fo += fmt(", sample clock %+.1f ppm", no_neg_zero(ppm_, 1));
            host.fact("Frequency offset", fo);
            if (std::isfinite(rf)) host.fact("Block", dab::band_block(host.rf_inverted() ? rf - cfo_ : rf + cfo_));
            std::string fq = fmt("%.0f %% of FIBs pass their CRC (last 10 s)", 100 * q);
            if (fic_bits_ > 0) fq += fmt(", %.2f %% channel bit errors", 100.0 * static_cast<double>(fic_errs_) / static_cast<double>(fic_bits_));
            host.fact("FIC quality", fq);
        }
        if (!fic_ok && found_ && now - last_good_fic_ > 3) {
            host.event("Ensemble lost: " + ens_label() + " (no FIC for 3 s)", 5);
            found_ = false;
        }
        if (ens_.eid < 0) {
            update_choices();   // (none)
            return;
        }
        update_country(now);
        if (!ens_.label.empty() && label_at_ < 0) label_at_ = now;
        // (found once the country is decided, or COUNTRY_WAIT_S after the label without it)
        if (!found_ && fic_ok && !ens_.label.empty() && (country_settled_ || now - label_at_ >= COUNTRY_WAIT_S)) {
            found_ = true;
            if (found_at_ == 0) found_at_ = now;
            std::string where;
            const double rf = host.rf_hz();
            if (std::isfinite(rf)) where = ", " + dab::band_block(rf + cfo_);
            const std::string ctry = country_settled_ ? ens_country_.name : std::string();
            host.event("📻 Ensemble found: " + ens_label() + (ctry.empty() ? "" : ", " + ctry) + where, 60);
        }
        host.fact("Ensemble", ens_label() + (ens_.short_label.empty() ? "" : " - short: " + dab::printable_utf8(ens_.short_label)));
        if (country_settled_) host.fact("Country", ens_country_.text);
        if (ens_.mjd >= 0) {
            int y, mo, d;
            mjd_to_date(ens_.mjd, &y, &mo, &d);
            char b[128];
            // the time moves on with the input from when it was received
            const double el = now - ens_.dt_at;
            double secs = ens_.hh * 3600.0 + ens_.mm * 60.0 + ens_.ss + ens_.ms / 1000.0 + el;
            int day_add = 0;
            while (secs >= 86400) { secs -= 86400; day_add++; }
            if (day_add) mjd_to_date(ens_.mjd + day_add, &y, &mo, &d);
            const int hh = static_cast<int>(secs / 3600), mm = static_cast<int>(secs / 60) % 60, ss = static_cast<int>(secs) % 60;
            snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d UTC", y, mo, d, hh, mm, ss);
            std::string s = b;
            if (ens_.lto > -999) {
                const double lsecs = std::fmod(secs + ens_.lto * 1800.0 + 86400.0, 86400.0);
                snprintf(b, sizeof b, " (local %02d:%02d, UTC%+.1f h)", static_cast<int>(lsecs / 3600), static_cast<int>(lsecs / 60) % 60,
                         ens_.lto / 2.0);
                s += b;
            }
            if (!ens_.utc_long) s += " (minutes only)";
            host.fact("Date / time", s);
        }
        // services
        int naudio = 0, nall = 0;
        int row_budget = ROW_BUDGET;
        const uint32_t dec_before = decoded_sid();
        std::vector<std::string> names;
        for (const auto& kv : ens_.services) {
            const dab::Service& s = kv.second;
            if (s.comps.empty()) continue;
            nall++;
            const dab::Component* c = s.primary();
            if (c && c->tmid == 0) naudio++;
            update_row(s, row_budget);
            // (announced with the ensemble: its event waits for the country)
            if ((found_ || ens_.label.empty()) && !announced_.count(s.sid) && !s.label.empty() && (now - found_at_ <= 3 || ev_budget > 0)) {
                announced_.insert(s.sid);
                if (now - found_at_ > 3 && ev_budget-- > 0)
                    host.event("Service: " + dab::printable_utf8(s.label) + " (SId " + hex(s.sid, s.data ? 8 : 4) + ", " + type_of(s, c) +
                                   (c && ens_.subch.count(c->subch) ? ", " + std::to_string(ens_.subch.at(c->subch).prot.kbps) + " kbit/s" : "") + ")",
                               600);
                else
                    names.push_back(dab::printable_utf8(s.label));
            }
        }
        if (!names.empty()) {
            std::string s;
            for (size_t i = 0; i < names.size() && i < 20; i++) s += (i ? ", " : "") + names[i];
            host.event(std::to_string(names.size()) + " services: " + s + (names.size() > 20 ? " ..." : ""), 60);
        }
        std::string sv = std::to_string(nall) + " (" + std::to_string(naudio) + " audio)";
        if (ens_.services_total >= 0 && ens_.services_total != nall)
            sv += ", " + std::to_string(ens_.services_total) + " announced by FIG 0/7";
        host.fact("Services", sv);
        if (ens_.alarm) host.fact("Alarm announcements", "enabled for the ensemble");
        // the programme
        if (!sel_valid_ || now - found_at_ > 0) select_service();
        sel_valid_ = true;
        // the ▶ moved with this selection: its two rows now, not a second later (a change made
        // in between - option(), a reconfiguration - the rows above have already sent)
        const uint32_t dec = decoded_sid();
        if (dec != dec_before)
            for (uint32_t sid : {dec_before, dec}) {
                auto it = ens_.services.find(sid);
                if (sid && it != ens_.services.end() && !it->second.comps.empty()) update_row(it->second, row_budget);
            }
        update_choices();
        update_programme_facts();
        update_tii_facts(now);
        update_map();
    }

    void update_row(const dab::Service& s, int& budget) {
        const std::string key = hex(s.sid, s.data ? 8 : 4);
        const dab::Component* c = s.primary();
        const dab::SubChannel* sc = nullptr;
        if (c && ens_.subch.count(c->subch)) sc = &ens_.subch.at(c->subch);
        std::string lang;
        if (c && c->language >= 0) lang = dab::language_name(c->language);
        const std::vector<std::string> cells = {
            s.label.empty() ? std::string("?") : dab::printable_utf8(s.label),
            !s.data && s.sid == decoded_sid() ? "▶" : "",
            key,
            type_of(s, c),
            sc ? std::to_string(sc->prot.kbps) : "",
            sc ? dab::protection_name(sc->prot) : "",
            c && c->subch >= 0 ? std::to_string(c->subch) : "",
            s.pty >= 0 ? dab::pty_name(s.pty, ens_.inter_table) : "",
            lang};
        std::string all;
        for (const auto& x : cells) all += x + '\x1f';
        std::vector<std::pair<std::string, std::string>> det;
        det.push_back({"Service", (s.label.empty() ? std::string("?") : dab::printable_utf8(s.label)) +
                                      (s.short_label.empty() ? "" : " (short: " + dab::printable_utf8(s.short_label) + ")") +
                                      (s.label2 ? " [extended label]" : "")});
        det.push_back({"SId", key + (s.data ? " (data service)" : " (programme service)")});
        // (a country that depends on the ensemble's waits until that is decided)
        if (country_settled_ || dab::own_ecc(ens_, s) >= 0) {
            const std::string cn = dab::service_country(ens_, s, ens_country_);
            if (!cn.empty()) det.push_back({"Country", cn});
        }
        if (s.pty >= 0)
            det.push_back({"Programme type", dab::pty_name(s.pty, ens_.inter_table) + " (" + std::to_string(s.pty) +
                                                 (s.pty_dynamic ? ", dynamic" : ", static") + ")"});
        if (s.asu) det.push_back({"Announcements", dab::announcement_names(s.asu)});
        if (!s.clusters.empty()) {
            std::string cl;
            for (int x : s.clusters) cl += (cl.empty() ? "" : ", ") + std::to_string(x);
            det.push_back({"Announcement clusters", cl});
        }
        if (s.ca_id) det.push_back({"Conditional access", "CAId " + std::to_string(s.ca_id)});
        int n = 0;
        for (const auto& k : s.comps) {
            n++;
            std::string d = k.primary ? "primary, " : "secondary, ";
            d += type_of(s, &k);
            if (k.subch >= 0) {
                d += ", sub-channel " + std::to_string(k.subch);
                auto it = ens_.subch.find(k.subch);
                if (it != ens_.subch.end())
                    d += " (CU " + std::to_string(it->second.start) + "+" + std::to_string(it->second.prot.size_cu) + ", " +
                         std::to_string(it->second.prot.kbps) + " kbit/s, " + dab::protection_name(it->second.prot) +
                         (it->second.fec == 1 ? ", RS FEC" : "") + ")";
            }
            if (k.tmid == 3 && k.packet_addr >= 0) d += ", packet address " + std::to_string(k.packet_addr);
            if (k.scids >= 0) d += ", SCIdS " + std::to_string(k.scids);
            if (k.language >= 0) {
                const std::string ln = dab::language_name(k.language);
                if (!ln.empty()) d += ", " + ln;
            }
            for (int ua : k.uas) d += ", " + dab::user_app_name(ua);
            if (!k.label.empty()) d += ", \"" + dab::printable_utf8(k.label) + "\"";
            det.push_back({"Component " + std::to_string(n), d});
        }
        for (const auto& x : det) all += x.first + '\x1e' + x.second + '\x1f';
        auto it = rows_.find(key);
        const double now = now_s();
        const bool same = it != rows_.end() && it->second.text == all;
        // an unchanged row is refreshed every ROW_REFRESH_S (the table's "Seen" column)
        if (same && now - it->second.sent < ROW_REFRESH_S) return;
        if (budget <= 0) return;   // (sent in a later second: the host traffic stays bounded)
        budget--;
        Row& r = rows_[key];
        if (!same) {
            // first sent at a phase of its own within the refresh period, so that the rows'
            // refreshes spread over the seconds instead of all going out in one status
            unsigned h = 0;
            for (char ch : key) h = h * 31u + static_cast<unsigned char>(ch);
            r.sent = it == rows_.end() ? now - static_cast<double>(h % static_cast<unsigned>(ROW_REFRESH_S)) : now;
            r.text = all;
            r.label = s.label.empty() ? std::string() : dab::printable_utf8(s.label);
            host.table_row(key, cells);
            host.table_detail(key, det);
            return;
        }
        r.sent = now;
        host.table_row(key, cells);   // (the detail is unchanged: not sent again)
    }

    void update_programme_facts() {
        const dab::AudioStatus& st = audio_.status();
        if (sel_label_.empty()) {
            host.fact("Programme", ens_.services.empty() ? ""
                                                         : "no audio service matches \"" + kp::printable(service_opt_) +
                                                               "\" - click a service in the table");
            clear_programme_facts();
            host.voice_state("");
            return;
        }
        auto it = ens_.services.find(sel_sid_);
        std::string p = dab::printable_utf8(sel_label_) + " (SId " + hex(sel_sid_, 4) + ")";
        if (it != ens_.services.end()) {
            const dab::Component* c = it->second.primary();
            if (c && c->ca) p += " - conditional access, not decoded";
            else if (msc_.active())
                p += ", " + std::string(c && c->type == 63 ? "DAB+" : "DAB") + " " + std::to_string(msc_.protection().kbps) + " kbit/s, " +
                     dab::protection_name(msc_.protection()) + ", sub-channel " + std::to_string(sel_subch_);
        }
        host.fact("Programme", p);
        if (it != ens_.services.end() && !it->second.label.empty() && prog_announced_ != sel_sid_ && msc_.active()) {
            prog_announced_ = sel_sid_;
            const dab::Component* c = it->second.primary();
            host.event("▶ Programme: " + dab::printable_utf8(sel_label_) + " (SId " + hex(sel_sid_, 4) + ", " +
                           (c && c->type == 63 ? "DAB+" : "DAB") + " " + std::to_string(msc_.protection().kbps) + " kbit/s)",
                       5);
        }
        // (each fact only once this programme provides it: "" = no fact)
        std::string fr;
        if (msc_.active()) {
            fr = std::to_string(lf_ok_) + " logical frames decoded";
            if (lf_lost_) fr += ", " + std::to_string(lf_lost_) + " lost";
            if (msc_bits_) fr += fmt(", %.2f %% channel bit errors", 100.0 * static_cast<double>(msc_errs_) / static_cast<double>(msc_bits_));
        }
        host.fact("Sub-channel", fr);
        host.fact("Codec", st.codec);
        host.fact("Voice codec", st.decoder);
        host.fact("Audio frames", st.frames_ok || st.frames_bad ? std::to_string(st.frames_ok) + " ok, " + std::to_string(st.frames_bad) + " bad" : "");
        host.fact("RS corrections", st.rs_corrected || st.rs_failed ? std::to_string(st.rs_corrected) + " code words corrected, " +
                                                                          std::to_string(st.rs_failed) + " failed"
                                                                    : "");
        // (a DAB+ programme without a usable AAC library - "AAC codec not installed (libfdk-aac2)",
        // "... disabled (KRAKEN_AAC_LIB=none)", "... not usable: <why>": the panel shows it in
        // red. No decoder named = no stream configured yet (conditional access, a sub-channel
        // without a coding profile): not a codec problem, nothing added)
        std::string vs = dab::printable_utf8(sel_label_);
        if (host.voice_wanted() && !st.can_decode && !st.decoder.empty()) {
            const bool missing = st.decoder.rfind("not installed", 0) == 0 || st.decoder.rfind("disabled", 0) == 0;
            vs += (missing ? ": AAC codec " : ": AAC codec not usable: ") + st.decoder;
        }
        host.voice_state(dab::utf8_truncate(vs, 120));
    }

    void update_tii_facts(double now) {
        const auto& tx = tii_.txs();
        const int ref = tii_.strongest();
        std::vector<std::pair<double, std::string>> list;
        for (const auto& kv : tx) {
            const dab::Tii::Tx& t = kv.second;
            if (!t.confirmed) continue;
            const std::string id = talker_id(ens_.eid, t.p, t.c);
            std::string s = id;
            const double rel = ref >= 0 && tx.at(ref).snr_lin > 0 && t.snr_lin > 0 ? 10 * std::log10(t.snr_lin / tx.at(ref).snr_lin) : 0;
            s += kv.first == ref ? " (strongest" : fmt(" (%+.1f dB", no_neg_zero(rel, 1));
            double us;
            if (kv.first != ref && tii_.delay_us(t, &us)) s += fmt(", %+.1f us", no_neg_zero(us, 1));
            s += fmt(", SNR %.0f dB", 10 * std::log10(std::max(1e-3, t.snr_lin)));
            if (verbose_)
                s += fmt(", coarse %.1f", t.tau_coarse) + fmt(" coh %.2f", t.coherence) + fmt(" fine %.1f samples", t.tau_fine) +
                     (t.convention ? ", pairs phi k/k+1" : ", pairs phi k/k");
            double lat, lon, slat, slon;
            if (tx_position(t.p, t.c, &lat, &lon) && host.station(&slat, &slon)) s += fmt(", %.0f km", distance_km(slat, slon, lat, lon));
            s += ")";
            list.push_back({-t.snr_lin, s});
            if (!tx_announced_.count(kv.first) && fic_decodes()) {
                tx_announced_.insert(kv.first);
                host.event("📡 Transmitter " + id + " (MainId " + std::to_string(t.p) + ", SubId " + std::to_string(t.c) + ")" +
                               (kv.first == ref ? " - the strongest" : fmt(", %+.1f dB against the strongest", no_neg_zero(rel, 1))),
                           600);
            }
        }
        std::sort(list.begin(), list.end());
        std::string s;
        for (const auto& x : list) s += (s.empty() ? "" : "; ") + x.second;
        if (list.empty()) s = tii_.tii_symbols ? "none confirmed" : "no TII in the null symbols";
        host.fact("Transmitters (TII)", list.empty() ? s : std::to_string(list.size()) + ": " + s);
        host.fact("Antennas", m_ >= 2 ? std::to_string(m_) + " (a bearing per transmitter from its TII carriers; " + std::to_string(pk_sent_) +
                                            " packets)"
                                      : std::string("1 (transmitter bearings need coherent mode with DoA on)"));
        (void)now;
    }

    void update_map() {
        if (ens_.tii_sub.empty() || ens_.eid < 0) return;
        for (const auto& kv : ens_.tii_sub) {
            const int p = kv.first / 32, c = kv.first % 32;
            double lat, lon;
            if (!tx_position(p, c, &lat, &lon)) continue;
            kp::MapPoint mp;
            mp.id = talker_id(ens_.eid, p, c);
            mp.lat = lat;
            mp.lon = lon;
            mp.kind = "station";
            mp.label = "TII " + std::to_string(p) + "/" + std::to_string(c);
            mp.ttl_s = 3600;
            std::string info = "Ensemble: " + ens_label() + "\nMainId: " + std::to_string(p) + "\nSubId: " + std::to_string(c);
            if (kv.second.td_us >= 0) info += "\nTime delay: " + std::to_string(kv.second.td_us) + " us (FIG 0/22)";
            auto it = tii_.txs().find(p * 32 + c);
            info += it != tii_.txs().end() && it->second.confirmed ? "\nReceived: yes" : "\nReceived: no";
            mp.info = info;
            host.map_point(mp);
            map_ids_.insert(mp.id);
        }
    }
};

}   // namespace

KRAKEN_PLUGIN(DabDecoder,
              {.id = "dab",
               .name = "DAB / DAB+ (digital radio)",
               .category = "Broadcast & cellular",
               .description = "Digital radio ensembles (Eureka-147, transmission mode I, Band III / L-band): the ensemble and its "
                              "services (labels, programme types, languages, bit rates, protection), date and time, the selected "
                              "programme's audio and dynamic label, and the transmitters of the single frequency network from "
                              "their TII codes - each with its own bearing. Click a service's row in the table to listen to it",
               .version = "1.0",
               .sample_rate = 2400000,
               .min_vfo_rate = 2000000,
               .author = "KrakenSDR",
               // (hidden: no control in the panel - a click on a service's row sets it to its SId)
               .options = {{"service", "Programme", "", "",
                            "The service to decode, set by clicking its row in the services table: empty = the first audio "
                            "service (lowest SId); a SId in hex (e.g. C221) or part of its label (case doesn't matter)",
                            true}},
               .map = true,
               .manual_only = true,
               .voice = true,
               .talkers = true,
               .lock_vfo = true,
               .multichannel = true})
