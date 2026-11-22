#include "aero_all.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>

namespace aero {

namespace {
constexpr double CH_RATE = 24000;
constexpr double SURVEY_S = 0.25;      // the survey's integration and period
constexpr float NARROW_DB = 4.0f;      // a narrow peak (750 Hz smoothing) over the local floor
constexpr float WIDE_DB = 1.5f;        // a wide bump (6.4 kHz smoothing)
constexpr double NARROW_SEARCH_HZ = 1000;   // a trial on a narrow peak searches this near it
constexpr int MAX_TRIALS = 24;         // trials on narrow peaks at once (a searching decoder: ~0.25 % of an x86 core)
constexpr int MAX_WIDE_TRIALS = 16;    // and on wide bumps
constexpr double TRIAL_NOLOCK_S = 4;   // a trial that never locks is closed after this
constexpr double TRIAL_S = 9;          // one that locks passes a CRC within this (600 bit/s: first frame <= 4.2 s)
constexpr double ASSIGNED_S = 20;      // a C channel opened from an assignment
constexpr double QUIET_P_S = 40;       // a channel that stops passing CRCs: closed after
constexpr double QUIET_C_S = 8;
constexpr double BLACKLIST_S = 90;     // a closed trial's frequency is left alone this long
// the part of the VFO channels are taken from (+- this x its rate, less half a channel): a 2.4 MHz VFO
// is the tuner unfiltered (+-1.09 MHz), a narrower one has the decimator's filter (flat to 0.4 W, -6 dB at 0.5 W)
inline double usable(double rate) { return rate >= 2.0e6 ? 0.46 : 0.42; }
constexpr int64_t MIX_LAT = 4800;     // the mixed audio runs 0.6 s behind the input (a C frame is 0.5 s)
constexpr double SEPARATE_HZ = 3000;   // a new carrier this close to a channel is that channel
constexpr double DUP_HZ = 1500;        // a trial locked this close to a validated channel's carrier is that channel

int parse_two(const std::string& s, const char* rx, int& b) {
    std::smatch m;
    std::regex r(rx);
    if (!std::regex_search(s, m, r)) return -1;
    b = std::stoi(m[2].str());
    return std::stoi(m[1].str());
}
}  // namespace

// ---------------------------------------------------------------------------
// one channel's kp::Host
class AllChannels::ChanHost : public kp::Host {
public:
    ChanHost(AllChannels& e, Chan& c) : e_(e), c_(c) {}
    void fact(const std::string& key, const std::string& value) override {
        if (muted()) return;
        if (value.empty()) c_.facts.erase(key);
        else c_.facts[key] = value;
        // the system's facts, the same from every channel: shown once
        if (key == "Voice codec" || key == "Satellites (system table)" || key == "Satellite" || key == "GES time" ||
            key.rfind("GES 0x", 0) == 0) {
            e_.host.fact(key, value);
            e_.global_facts_.insert(key);
        } else if (key == "Last ACARS" && !value.empty()) {
            e_.host.fact(key, c_.name + " · " + value);
            e_.global_facts_.insert(key);
        }
    }
    void event(const std::string& text, double dedup_s) override {
        if (!muted()) e_.host.event(c_.name + " · " + text, dedup_s);
    }
    void valid() override {
        if (muted()) return;
        mark();
        e_.host.valid();
    }
    void valid(double s, double t) override {
        if (muted()) return;
        mark();
        e_.host.valid(s, t);
    }
    void audio(const float* pcm, size_t n) override {
        if (muted()) return;
        if (c_.audio_start < 0) c_.audio_start = e_.now();
        c_.audio.insert(c_.audio.end(), pcm, pcm + n);
    }
    void voice_state(const std::string& s) override {
        if (muted()) return;
        c_.voice = s;
        e_.voice_state();
    }
    void freq_error(float) override {}
    bool verbose() const override { return e_.host.verbose(); }
    bool voice_wanted() const override { return e_.host.voice_wanted(); }
    double time() const override { return c_.t0 + static_cast<double>(c_.nsamp) / CH_RATE; }
    double sample_rate() const override { return CH_RATE; }
    void map_point(const kp::MapPoint& p) override {
        if (!muted()) e_.host.map_point(p);
    }
    void map_remove(const std::string& id) override { e_.host.map_remove(id); }
    void message(const std::string& from, const std::string& text) override {
        if (!muted()) e_.host.message(from, text);
    }
    bool raw_wanted() const override { return e_.host.raw_wanted(); }
    void raw(const std::string& d) override {
        if (!muted()) e_.host.raw(c_.name + " " + d);
    }
    void table_columns(const std::vector<std::string>& cols) override { e_.host.table_columns(cols); }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override {
        if (!muted()) e_.table_row(c_.id, key, cells);
    }
    void table_remove(const std::string& key) override {
        auto it = e_.rows_.find(key);
        if (it == e_.rows_.end()) return;
        it->second.erase(c_.id);
        if (it->second.empty()) {
            e_.rows_.erase(it);
            e_.host.table_remove(key);
        }
    }
    void table_keep(double s) override { e_.host.table_keep(s); }
    void table_detail(const std::string& key, const std::vector<std::pair<std::string, std::string>>& lines) override {
        e_.host.table_detail(key, lines);
    }
    void talker(const kp::Talker& t) override {
        if (!muted()) e_.host.talker(t);
    }
    void talker_end(double at_s, int channel) override { e_.host.talker_end(at_s, channel); }
    void talker_forget(const std::string& id) override { e_.host.talker_forget(id); }
    bool station(double* lat, double* lon) const override { return e_.host.station(lat, lon); }
    double rf_hz() const override {
        const double rf = e_.host.rf_hz();
        return std::isfinite(rf) ? rf + c_.centre_hz : rf;
    }
    void spatial(const kp::Spatial& s) override { e_.host.spatial(s); }
    void spatial_remove(const std::string& id) override { e_.host.spatial_remove(id); }
    void plot(const kp::Plot& p) override { e_.host.plot(p); }
    void log(const std::string& t) override { e_.host.log(c_.name + ": " + t); }

private:
    AllChannels& e_;
    Chan& c_;
    // a channel that has not yet passed a CRC and has locked the carrier of
    // one that has (two survey peaks on one signal) says nothing: it is
    // closed at the next look
    bool muted() {
        if (c_.dup) return true;
        if (c_.validated || !c_.dec || !c_.dec->locked()) return false;
        const double f = e_.carrier_abs(c_);
        for (auto& [id, o] : e_.ch_)
            if (o.get() != &c_ && o->validated && o->dec && std::fabs(e_.carrier_abs(*o) - f) < DUP_HZ) {
                c_.dup = true;
                if (e_.debug_) e_.host.log("duplicate " + c_.name + " of " + o->name);
                return true;
            }
        return false;
    }
    void mark() {
        const double t = e_.now();
        if (c_.first_valid < 0) c_.first_valid = t;
        c_.last_valid = t;
        if (!c_.validated && e_.debug_)
            e_.host.log("valid " + c_.name + " at " + std::to_string(t).substr(0, 6) + " s");
        c_.validated = true;
    }
};

// ---------------------------------------------------------------------------

AllChannels::AllChannels(kp::Host& h, double rate) : host(h), rate_(rate), bank_(rate) {
    char b[160];
    snprintf(b, sizeof b, "all channels in %.0f kHz: %d bins of %.0f Hz, channels at 24 kHz", rate_ / 1e3, bank_.bins(),
             bank_.bin_hz());
    host.log(b);
    host.fact("Channels", "searching the VFO");
    global_facts_.insert("Channels");
}

AllChannels::~AllChannels() {
    // the test tools compare each channel with a single-channel decode of it
    if (!debug_) return;
    for (auto& [id, c] : ch_)
        if (c->validated && !c->dup)
            host.log("final " + c->name + " id " + std::to_string(id) + " opened " + std::to_string(c->opened) + " input " +
                     std::to_string(std::lround(c->centre_hz)) + " carrier " + std::to_string(std::lround(carrier_abs(*c))) + ": " +
                     summary(*c, nullptr, nullptr));
}

double AllChannels::now() const { return t_base_ + static_cast<double>(nin_) / rate_; }

void AllChannels::reset() {
    for (auto& [id, c] : ch_) {
        bank_.close(c->bank_id);
        if (!c->shown_key.empty()) host.fact(c->shown_key, "");
    }
    ch_.clear();
    blacklist_.clear();
    rows_.clear();
    offset_hz_ = NAN;
    offset_n_ = 0;
    closed_ok_ = closed_tot_ = 0;
    for (const auto& k : global_facts_) host.fact(k, "");
    global_facts_.clear();
    host.fact("Channels", "searching the VFO");
    global_facts_.insert("Channels");
    host.voice_state("");
    std::vector<float> dummy;
    bank_.take_spectrum(dummy);
    next_survey_ = now() + SURVEY_S;
}

void AllChannels::option(const std::string& key, const std::string& value) {
    if (key == "listen") {
        listen_ = value == "newest" ? "newest" : "mix";
        voice_state();
    } else if (key == "verbose") {
        verbose_opt_ = value == "1";
        for (auto& [id, c] : ch_) c->dec->option(key, value);
    }
}

void AllChannels::process(const cf* x, size_t n) {
    if (!t_set_) {
        t_base_ = host.time();
        t_set_ = true;
        next_survey_ = now() + SURVEY_S;
        next_show_ = now() + 1;
    }
    std::map<int, Chan*> by_bank;
    for (auto& [id, c] : ch_) by_bank[c->bank_id] = c.get();
    bank_.push(x, n, [&](int bank_id, const cf* y, size_t m) {
        auto it = by_bank.find(bank_id);
        if (it != by_bank.end()) feed(*it->second, y, m);
    });
    nin_ += static_cast<int64_t>(n);
    const double t = now();
    if (t >= next_survey_) {
        next_survey_ = t + SURVEY_S;
        survey();
        housekeeping();
    }
    if (t >= next_show_) {
        next_show_ = t + 1;
        show();
    }
    mix();
}

void AllChannels::feed(Chan& c, const cf* y, size_t n) {
    c.pend.insert(c.pend.end(), y, y + n);
    if (c.pend.size() < 480) return;   // 20 ms at a time
    c.dec->process(c.pend.data(), c.pend.size());
    c.nsamp += static_cast<int64_t>(c.pend.size());
    c.pend.clear();
}

AllChannels::Chan* AllChannels::open(double centre_hz, const std::string& mode, bool assigned, double search_hz) {
    auto c = std::make_unique<Chan>();
    Chan& r = *c;
    r.id = next_id_++;
    const int bin = bank_.bin_of(centre_hz);
    r.bank_id = bank_.open(bin);
    r.centre_hz = bank_.hz_of(bin);
    r.t0 = t_base_ + static_cast<double>(bank_.blocks() * bank_.hop() + bank_.first_offset()) / rate_;
    r.opened = now();
    r.assigned = assigned;
    r.h = std::make_unique<ChanHost>(*this, r);
    r.name = name_of(r);
    r.dec = std::make_unique<AeroChannel>(*r.h);
    r.dec->option("mode", mode);
    if (search_hz < 2900) r.dec->set_search_hz(search_hz + std::fabs(centre_hz - r.centre_hz));
    if (verbose_opt_) r.dec->option("verbose", "1");
    r.dec->on_c_assign([this](double hz) { on_assign(hz); });
    if (debug_)
        host.log("open " + r.name + " (" + mode + ") at " + std::to_string(r.opened).substr(0, 6) + " s, input " +
                 std::to_string(std::lround(r.centre_hz)) + " Hz, id " + std::to_string(r.id));
    ch_[r.id] = std::move(c);
    return &r;
}

void AllChannels::close(int id, bool blacklist) {
    auto it = ch_.find(id);
    if (it == ch_.end()) return;
    Chan& c = *it->second;
    bank_.close(c.bank_id);
    if (!c.shown_key.empty()) host.fact(c.shown_key, "");
    if (blacklist) blacklist_[c.centre_hz] = now() + BLACKLIST_S;
    if (debug_)
        host.log("close " + c.name + (c.validated ? " (quiet)" : c.ever_locked ? " (no CRC passed)" : " (no lock)") + " after " +
                 std::to_string(now() - c.opened).substr(0, 5) + " s");
    if (c.validated && !c.dup) {
        // what it brought, as an event (the list forgets it now)
        uint64_t ok = 0, tot = 0;
        char b[64];
        snprintf(b, sizeof b, ", heard %.0f s", std::max(0.0, c.last_valid - c.first_valid));
        host.event(c.name + " · channel ended (quiet): " + summary(c, &ok, &tot) + b, 0);
        closed_ok_ += ok;
        closed_tot_ += tot;
    }
    for (auto r = rows_.begin(); r != rows_.end(); ++r) r->second.erase(id);   // the merged rows stay as they were
    if (!c.voice.empty()) {
        c.voice.clear();
        ch_.erase(it);
        voice_state();
        return;
    }
    ch_.erase(it);
}

double AllChannels::carrier_abs(const Chan& c) const {
    return c.centre_hz + (c.dec && c.dec->locked() ? c.dec->carrier_hz() : 0.0);
}

std::string AllChannels::name_of(const Chan& c) const {
    char b[48];
    const double rf = host.rf_hz();
    const double f = carrier_abs(c);
    if (!std::isfinite(rf)) snprintf(b, sizeof b, "%+.1f kHz", f / 1e3);
    else if (std::isfinite(offset_hz_)) snprintf(b, sizeof b, "%.4f", std::round((rf + f - offset_hz_) / 100) * 100 / 1e6);
    else snprintf(b, sizeof b, "~%.4f", (rf + f) / 1e6);
    return b;
}

void AllChannels::survey() {
    std::vector<float> p;
    if (bank_.take_spectrum(p) == 0) return;
    const int N = static_cast<int>(p.size());
    const double B = bank_.bin_hz();
    // frequency order: q[i] at (i - N/2) B
    std::vector<float> q(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) q[static_cast<size_t>(i)] = p[static_cast<size_t>((i + N / 2) % N)];
    // the local floor: medians of 64-bin pieces, then the median of 7 pieces around
    const int S = 64, ns = std::max(1, N / S);
    std::vector<float> med(static_cast<size_t>(ns));
    for (int s = 0; s < ns; s++) {
        std::vector<float> v(q.begin() + s * S, q.begin() + std::min(N, (s + 1) * S));
        std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
        med[static_cast<size_t>(s)] = std::max(v[v.size() / 2], 1e-30f);
    }
    std::vector<float> fl(static_cast<size_t>(N));
    for (int s = 0; s < ns; s++) {
        std::vector<float> v;
        for (int d = -3; d <= 3; d++)
            if (s + d >= 0 && s + d < ns) v.push_back(med[static_cast<size_t>(s + d)]);
        std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
        for (int i = s * S; i < std::min(N, (s + 1) * S); i++) fl[static_cast<size_t>(i)] = v[v.size() / 2];
    }
    for (int i = ns * S; i < N; i++) fl[static_cast<size_t>(i)] = med.back();
    auto avg = [&](int i, int h) {
        double s = 0;
        int k = 0;
        for (int d = -h; d <= h; d++)
            if (i + d >= 0 && i + d < N) s += q[static_cast<size_t>(i + d)], k++;
        return static_cast<float>(s / std::max(1, k));
    };
    std::vector<float> rn(static_cast<size_t>(N)), rw(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        rn[static_cast<size_t>(i)] = 10 * std::log10(avg(i, 1) / fl[static_cast<size_t>(i)]);
        rw[static_cast<size_t>(i)] = 10 * std::log10(avg(i, 8) / fl[static_cast<size_t>(i)]);
    }
    ratio_ = rn;
    for (auto& [id, c] : ch_) {
        const int i = static_cast<int>(std::lround(carrier_abs(*c) / B)) + N / 2;
        if (i >= 0 && i < N) c->level_db = c->level_db == 0 ? rn[static_cast<size_t>(i)] : 0.8f * c->level_db + 0.2f * rn[static_cast<size_t>(i)];
    }
    const double t = now();
    for (auto it = blacklist_.begin(); it != blacklist_.end();)
        it = it->second < t ? blacklist_.erase(it) : std::next(it);
    // the usable part: the VFO's flat band, a channel's half width in from its edge
    const double edge = usable(rate_) * rate_ - 12000;
    struct Cand {
        double f;
        float db;
        bool wide;
    };
    std::vector<Cand> cands;
    for (int i = 0; i < N; i++) {
        const double f = (i - N / 2) * B;
        if (std::fabs(f) > edge) continue;
        auto peak = [&](const std::vector<float>& r, int h) {
            for (int d = -h; d <= h; d++)
                if (d && i + d >= 0 && i + d < N && r[static_cast<size_t>(i + d)] > r[static_cast<size_t>(i)]) return false;
            return true;
        };
        const float n_db = rn[static_cast<size_t>(i)], w_db = rw[static_cast<size_t>(i)];
        if (n_db >= NARROW_DB && peak(rn, 6)) cands.push_back({f, n_db, false});
        if (w_db >= WIDE_DB && peak(rw, 16)) {
            // a wide signal is flat-topped: a bump made of narrow carriers
            // (a group of 600 bit/s channels) has peaks far over its mean,
            // and those are narrow candidates of their own
            float top = n_db;
            for (int d = -8; d <= 8; d++)
                if (i + d >= 0 && i + d < N) top = std::max(top, rn[static_cast<size_t>(i + d)]);
            if (top - w_db < 2.5f) cands.push_back({f, w_db, true});
        }
    }
    // the strongest first (wide and narrow trials are counted apart below)
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.wide != b.wide ? a.wide : a.db > b.db;
    });
    if (debug_ && !cands.empty()) host.log("survey: " + std::to_string(cands.size()) + " candidates");
    // trials on narrow peaks and on wide bumps are counted apart: the few
    // wide ones (10.5 / 8.4 kbit/s) never wait behind the many narrow ones
    int trials[2] = {0, 0};
    for (auto& [id, c] : ch_)
        if (!c->validated) trials[c->open_wide ? 1 : 0]++;
    // a trial that has not locked yet gives way to a candidate of its kind
    // clearly stronger (narrow: 3 dB, wide: 0.75 dB; a band's first quarter
    // second may show the weak carriers first)
    auto weak_trial = [&](const Chan& c, const Cand& cd) {
        return !c.validated && !c.assigned && c.open_wide == cd.wide && !c.ever_locked && !c.dec->locked() &&
               cd.db > c.open_db + (cd.wide ? 0.75f : 3.0f);
    };
    for (const auto& cd : cands) {
        bool near = false;
        std::vector<int> give_way;
        for (auto& [id, c] : ch_) {
            const double d = std::min(std::fabs(cd.f - c->centre_hz), std::fabs(cd.f - carrier_abs(*c)));
            // a wide signal is that channel if a wide trial is on it, a channel
            // has locked a wide rate on it or a locked one sits on its centre
            // (a narrow trial that has not locked does not keep it waiting);
            // a narrow one if any channel is near
            const bool lk = c->dec->locked() || c->validated;
            const bool wide_there = lk && c->dec->locked_rate() >= 4800;
            const bool wide_near = (c->open_wide && d < 4500) || (wide_there && d < 4500) || (lk && d < 1500);
            if (!(cd.wide ? wide_near : d < SEPARATE_HZ)) continue;
            if (weak_trial(*c, cd)) give_way.push_back(id);
            else near = true;
        }
        for (const auto& [f, until] : blacklist_)
            if (std::fabs(cd.f - f) < SEPARATE_HZ) near = true;
        if (survey_dbg_)
            host.log("cand " + std::to_string(cd.f) + (cd.wide ? " wide " : " narrow ") + std::to_string(cd.db) + (near ? " near" : ""));
        if (near) continue;
        int& tr = trials[cd.wide ? 1 : 0];
        if (tr - static_cast<int>(give_way.size()) >= (cd.wide ? MAX_WIDE_TRIALS : MAX_TRIALS)) {
            // full: the weakest trial that has not locked, if this is clearly stronger
            int weakest = 0;
            float wdb = 1e9f;
            for (auto& [id, c] : ch_)
                if (weak_trial(*c, cd) && c->open_db < wdb) wdb = c->open_db, weakest = id;
            if (!weakest) continue;
            give_way.push_back(weakest);
        }
        for (int id : give_way) close(id, false);
        tr -= static_cast<int>(give_way.size());
        // a narrow peak is the carrier within a bin or two: the search stays
        // near it (two synchronous neighbours 5 kHz apart make a false
        // line pair half way between them)
        Chan* c = open(cd.f, "auto", false, cd.wide ? 2900 : NARROW_SEARCH_HZ);
        c->open_db = cd.db;
        c->open_wide = cd.wide;
        tr++;
    }
}

void AllChannels::on_assign(double to_aircraft_hz) {
    const double rf = host.rf_hz();
    if (!std::isfinite(rf)) return;
    const double off = std::isfinite(offset_hz_) ? offset_hz_ : 0.0;
    const double f = to_aircraft_hz + off - rf;   // where it is in the input
    if (std::fabs(f) > usable(rate_) * rate_ - 12000) return;   // outside the VFO: not followed (no retune)
    for (auto& [id, c] : ch_)
        if (std::fabs(c->centre_hz - f) < 2000 || std::fabs(carrier_abs(*c) - f) < 2000) return;   // already there
    for (auto it = blacklist_.begin(); it != blacklist_.end();)
        it = std::fabs(it->first - f) < SEPARATE_HZ ? blacklist_.erase(it) : std::next(it);
    Chan* c = open(f, "c8400", true);
    host.log("C channel assignment: " + c->name + " opened");
}

void AllChannels::housekeeping() {
    const double t = now();
    std::vector<std::pair<int, bool>> drop;
    for (auto& [id, c] : ch_) {
        c->name = name_of(*c);
        if (c->dup) {
            drop.push_back({id, false});
            continue;
        }
        if (!c->validated) {
            if (c->dec->locked()) c->ever_locked = true;
            const double lim = c->assigned ? ASSIGNED_S : c->ever_locked ? TRIAL_S : TRIAL_NOLOCK_S;
            if (t - c->opened > lim) drop.push_back({id, !c->assigned});
        } else if (t - c->last_valid > (c->dec->is_cchannel() || c->assigned ? QUIET_C_S : QUIET_P_S)) {
            drop.push_back({id, false});
        }
    }
    // two channels on one carrier: keep the older / the validated one
    for (auto a = ch_.begin(); a != ch_.end(); ++a)
        for (auto b = std::next(a); b != ch_.end(); ++b) {
            Chan& x = *a->second;
            Chan& y = *b->second;
            if (!x.dec->locked() || !y.dec->locked() || x.dec->locked_rate() != y.dec->locked_rate()) continue;
            if (std::fabs(carrier_abs(x) - carrier_abs(y)) > 400) continue;
            const bool keep_x = x.validated || !y.validated;
            drop.push_back({keep_x ? y.id : x.id, false});
        }
    for (const auto& [id, bl] : drop) close(id, bl);
    // the receiver's offset: the locked P channels against the frequencies the signalling advertises
    std::vector<double> adv;
    for (auto& [id, c] : ch_) {
        auto v = c->dec->su().advertised_hz();
        adv.insert(adv.end(), v.begin(), v.end());
    }
    const double rf = host.rf_hz();
    std::vector<double> d;
    if (std::isfinite(rf) && !adv.empty())
        for (auto& [id, c] : ch_) {
            if (!c->validated || !c->dec->locked() || c->dec->is_cchannel()) continue;
            const double f = rf + carrier_abs(*c);
            double best = 1e9;
            for (double a : adv)
                if (std::fabs(f - a) < std::fabs(best)) best = f - a;
            if (std::fabs(best) < 3000) d.push_back(best);
        }
    if (!d.empty()) {
        std::sort(d.begin(), d.end());
        offset_hz_ = d[d.size() / 2];
        offset_n_ = static_cast<int>(d.size());
    }
}

// a channel as the list shows it: kind and rate, GES, signal units (C channel: frames)
std::string AllChannels::summary(const Chan& c, uint64_t* ok, uint64_t* tot) const {
    const bool cm = c.dec->is_cchannel();
    std::string v;
    char b[96];
    if (cm) v = "C 8400 bit/s voice";
    else {
        snprintf(b, sizeof b, "P %d bit/s", static_cast<int>(c.dec->locked_rate()));
        v = b;
    }
    auto g = c.facts.find("GES");
    if (g != c.facts.end()) v += " · GES " + g->second.substr(0, g->second.find(' '));
    int a = 0, n = 0;
    auto su = c.facts.find("Signal units");
    if (su != c.facts.end() && (a = parse_two(su->second, "(\\d+) of (\\d+)", n)) >= 0) {
        snprintf(b, sizeof b, " · %d/%d SUs", a, n);
        v += b;
        if (ok) *ok = static_cast<uint64_t>(a);
        if (tot) *tot = static_cast<uint64_t>(n);
    }
    auto cf = c.facts.find("C channel frames");
    if (cm && cf != c.facts.end()) {
        int clean = 0;
        const int fr = parse_two(cf->second, "(\\d+) frames \\((\\d+) clean\\)", clean);
        if (fr >= 0) {
            snprintf(b, sizeof b, " · %d/%d frames", clean, fr);
            v += b;
        }
    }
    return v;
}

void AllChannels::show() {
    const double t = now();
    int np = 0, nc = 0, trials = 0;
    std::map<int, int> by_rate;
    uint64_t ok = 0, tot = 0;
    for (auto& [id, c] : ch_) {
        if (!c->validated) {
            trials++;
            continue;
        }
        c->name = name_of(*c);
        const bool cm = c->dec->is_cchannel();
        cm ? nc++ : (np++, by_rate[static_cast<int>(c->dec->locked_rate())]++);
        uint64_t a = 0, n = 0;
        std::string v = summary(*c, &a, &n);
        ok += a;
        tot += n;
        char b[96];
        snprintf(b, sizeof b, " · %+.1f dB · %.0f s ago", c->level_db, std::max(0.0, t - c->last_valid));
        v += b;
        if (!c->voice.empty()) v += " · " + c->voice;
        const std::string key = "Ch " + c->name;
        if (!c->shown_key.empty() && c->shown_key != key) host.fact(c->shown_key, "");
        c->shown_key = key;
        host.fact(key, v);
    }
    std::string s = std::to_string(np + nc) + " channels";
    if (np + nc) {
        s += " (";
        bool first = true;
        for (auto& [r, k] : by_rate) {
            s += (first ? "" : ", ") + std::to_string(k) + " P " + std::to_string(r);
            first = false;
        }
        if (nc) s += (first ? "" : ", ") + std::to_string(nc) + " C";
        s += ")";
    }
    if (trials) s += ", trying " + std::to_string(trials) + " more carrier" + (trials > 1 ? "s" : "");
    char b[96];
    snprintf(b, sizeof b, "; %.0f kHz of input", rate_ / 1e3);
    host.fact("Channels", s + b);
    global_facts_.insert("Channels");
    ok += closed_ok_;
    tot += closed_tot_;
    if (tot) {
        snprintf(b, sizeof b, "%llu of %llu passed the CRC (all channels)", static_cast<unsigned long long>(ok),
                 static_cast<unsigned long long>(tot));
        host.fact("Signal units", b);
        global_facts_.insert("Signal units");
    }
    if (std::isfinite(offset_hz_)) {
        snprintf(b, sizeof b, "%+.2f kHz (from %d P channels against the advertised frequencies)", offset_hz_ / 1e3, offset_n_);
        host.fact("Receiver offset", b);
        global_facts_.insert("Receiver offset");
    }
    if (!rows_.empty()) {
        host.fact("Aircraft heard", std::to_string(rows_.size()));
        global_facts_.insert("Aircraft heard");
    }
}

void AllChannels::table_row(int ch, const std::string& key, const std::vector<std::string>& cells) {
    auto& per = rows_[key];
    per[ch] = cells;
    std::vector<std::string> m = cells;
    // Messages (column 4) summed over the channels; empty cells from any channel that has them
    long long msgs = 0;
    bool have = false;
    for (const auto& [id, c] : per) {
        if (c.size() > 4) {
            char* e = nullptr;
            const long long v = std::strtoll(c[4].c_str(), &e, 10);
            if (e && e != c[4].c_str()) msgs += v, have = true;
        }
        for (size_t i = 1; i < m.size() && i < c.size(); i++)
            if (m[i].empty() && !c[i].empty()) m[i] = c[i];
    }
    if (have && m.size() > 4) m[4] = std::to_string(msgs);
    host.table_row(key, m);
}

// The calls' audio to the VFO, clocked by the input: each call's decoder
// hands its sound over a C frame (0.5 s) at a time, at its own moments;
// summed as it came, two calls would give twice real time. So: what the
// input's time allows (8 kHz, MIX_LAT behind), each call's next samples
// summed (silence where one has none yet); idle while no call has sound.
void AllChannels::mix() {
    const int64_t target = static_cast<int64_t>(std::llround((now() - t_base_) * 8000.0)) - MIX_LAT;
    std::vector<Chan*> act;
    for (auto& [id, c] : ch_) {
        if (!c->audio.empty()) act.push_back(c.get());
        else if (c->audio_start >= 0 && c->voice.empty()) c->audio_start = -1;   // that call ended
        while (c->audio.size() > 16000) c->audio.pop_front();                     // > 2 s behind: let go
    }
    if (act.empty()) {
        aud_out_ = target;
        return;
    }
    if (listen_ == "newest" && act.size() > 1) {
        Chan* nw = *std::max_element(act.begin(), act.end(), [](Chan* a, Chan* b) { return a->audio_start < b->audio_start; });
        for (Chan* c : act)
            if (c != nw) c->audio.clear();
        act = {nw};
    }
    int64_t n = target - aud_out_;
    n -= n % 160;
    if (n <= 0) return;
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (Chan* c : act) {
        const size_t k = std::min(static_cast<size_t>(n), c->audio.size());
        for (size_t i = 0; i < k; i++) out[i] += c->audio[i];
        c->audio.erase(c->audio.begin(), c->audio.begin() + static_cast<long>(k));
    }
    if (act.size() > 1)
        for (size_t i = 0; i < out.size(); i += 160) lim_.process(&out[i], 160);
    host.audio(out.data(), out.size());
    aud_out_ += n;
}

void AllChannels::voice_state() {
    std::vector<const Chan*> v;
    for (auto& [id, c] : ch_)
        if (!c->voice.empty()) v.push_back(c.get());
    if (v.empty()) host.voice_state("");
    else if (v.size() == 1) host.voice_state(v[0]->name + ": " + v[0]->voice);
    else {
        std::string s = std::to_string(v.size()) + " calls, " + (listen_ == "newest" ? "the newest heard" : "mixed") + ":";
        for (const Chan* c : v) s += " " + c->name;
        host.voice_state(s);
    }
}

}  // namespace aero
