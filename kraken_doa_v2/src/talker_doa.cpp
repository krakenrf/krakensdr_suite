#include "talker_doa.hpp"

#include "signal_processing/music_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace tdoa {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

constexpr size_t MAX_FRAMES = 400;   // waiting frames (a few seconds at the narrowest bandwidths)
constexpr size_t MAX_SPANS = 200;
constexpr int64_t SPAN_KEEP_MS = 120000;
constexpr int64_t RECOMPUTE_MS = 2000;   // every talker's bearing again (follows array setting changes)

}  // namespace

void TalkerDoa::set_active(bool on) {
    if (active_.exchange(on) == on) return;
    if (!on) reset();
}

void TalkerDoa::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    reset_locked();
}

void TalkerDoa::reset_locked() {
    frames_.clear();
    spans_.clear();
    talkers_.clear();
    freq_ = 0;
}

void TalkerDoa::add_frame(const Eigen::MatrixXcd& R, uint64_t a, uint64_t b, float rate_hz, double freq_hz,
                          float /*eig_ratio*/) {
    if (!active() || b <= a || !(rate_hz > 0)) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (freq_ != 0 && std::fabs(freq_hz - freq_) > RETUNE_RESET_HZ) reset_locked();   // another signal
    freq_ = freq_hz;
    frames_.push_back({R, a, b, rate_hz, freq_hz, now_ms()});
    while (frames_.size() > MAX_FRAMES) frames_.pop_front();
}

void TalkerDoa::add_span(const dig::TalkerSpan& s) {
    if (!active() || s.id.empty()) return;
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mu_);
    Span* sp = nullptr;
    for (auto it = spans_.rbegin(); it != spans_.rend(); ++it)
        if (it->id == s.id && it->plugin == s.plugin && it->start == s.start) { sp = &*it; break; }
    if (!sp) {
        spans_.push_back({s.plugin, s.id, s.label, s.start, s.end, s.closed, s.channel, next_seq_++, t});
        sp = &spans_.back();
        if (unnamed_talker(s.id)) {
            prune_locked(t);
            return;
        }
        Talker& tk = talkers_[s.id];
        if (tk.info.id.empty()) {
            tk.info.id = s.id;
            tk.info.first_ms = t;
        }
        tk.info.tx++;
    } else {
        sp->end = std::max(sp->start, s.closed ? s.end : std::max(sp->end, s.end));
        sp->closed = sp->closed || s.closed;
        sp->updated_ms = t;
        if (!s.label.empty()) sp->label = s.label;
    }
    if (unnamed_talker(s.id)) return;
    Talker& tk = talkers_[s.id];
    tk.info.plugin = s.plugin;
    if (!s.label.empty()) tk.info.label = s.label;
    tk.info.last_ms = t;
    tk.open_ms = s.closed ? 0 : t;
    prune_locked(t);
}

void TalkerDoa::prune_locked(int64_t now) {
    while (spans_.size() > MAX_SPANS || (!spans_.empty() && spans_.front().closed &&
                                         now - spans_.front().updated_ms > SPAN_KEEP_MS))
        spans_.pop_front();
    for (auto it = talkers_.begin(); it != talkers_.end();)
        if (now - it->second.info.last_ms > TALKER_TTL_MS) it = talkers_.erase(it);
        else ++it;
    while (talkers_.size() > MAX_TALKERS) {
        auto oldest = talkers_.begin();
        for (auto it = talkers_.begin(); it != talkers_.end(); ++it)
            if (it->second.info.last_ms < oldest->second.info.last_ms) oldest = it;
        talkers_.erase(oldest);
    }
}

void TalkerDoa::update(const MUSICProcessor& mp, std::vector<TalkerFrame>* out) {
    struct Job {
        std::string id, label;
        uint64_t seq;             // talker jobs: the transmission summed
        bool talker;              // false = one frame (a mobile DF record)
        int64_t stamp_ms;
        Eigen::MatrixXcd R;
    };
    std::vector<Job> jobs;
    const int64_t t = now_ms();
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 1. waiting frames -> talkers
        std::deque<Frame> keep;
        for (auto& f : frames_) {
            const double g = GUARD_S * f.rate;
            const double fa = static_cast<double>(f.a), fb = static_cast<double>(f.b);
            std::set<std::string> ids;
            const Span* inside = nullptr;
            bool may_extend = false;
            for (const auto& s : spans_) {
                const double sa = static_cast<double>(s.start), se = static_cast<double>(s.end);
                const bool touches = fa < se + g && fb + g > sa;
                const bool extend = !s.closed && fa >= sa + g && fb + g > se;   // could still end up inside
                if (!touches && !extend) continue;
                ids.insert(s.id);
                if (sa + g <= fa && fb + g <= se) inside = &s;
                if (extend) may_extend = true;
            }
            if (ids.size() > 1) continue;                  // two radios in one frame: unusable
            const bool young = t - f.stamp_ms < static_cast<int64_t>(FRAME_WAIT_S * 1000);
            if (inside && (unnamed_talker(inside->id) ||
                           (inside->channel != 0 && t - f.stamp_ms < CHANNEL_HOLD_MS))) {
                // its radio not named yet / another channel's radio may still be reported over it
                if (young) keep.push_back(std::move(f));
                continue;
            }
            if (inside) {
                const Span& s = *inside;
                Talker& tk = talkers_[s.id];
                if (tk.info.id.empty()) {
                    tk.info.id = s.id;
                    tk.info.first_ms = t;
                    tk.info.last_ms = t;
                }
                if (tk.seq != s.seq || tk.R.rows() != f.R.rows()) {
                    // a new transmission: the previous one goes to the history
                    if (tk.info.tx_frames > 0 && tk.info.doa >= 0) {
                        tk.info.hist.push_back({tk.info.tx_end_ms, tk.info.doa, tk.info.conf, tk.info.tx_frames});
                        if (tk.info.hist.size() > MAX_HIST) tk.info.hist.erase(tk.info.hist.begin());
                    }
                    tk.seq = s.seq;
                    tk.R = Eigen::MatrixXcd::Zero(f.R.rows(), f.R.cols());
                    tk.w = 0;
                    tk.info.tx_frames = 0;
                    tk.info.tx_start_ms = f.stamp_ms;
                    tk.info.spec.clear();
                    tk.info.doa = -1;
                    tk.info.conf = 0;
                }
                const double n = fb - fa;
                tk.R += n * f.R;
                tk.w += n;
                tk.info.tx_frames++;
                tk.info.frames++;
                tk.info.tx_end_ms = f.stamp_ms;
                tk.dirty = true;
                jobs.push_back({s.id, s.label, 0, false, f.stamp_ms, f.R});
                continue;
            }
            // not decided yet: its talker may still be reported, or extended over it
            if ((ids.empty() && young) || (may_extend && young)) keep.push_back(std::move(f));
        }
        frames_.swap(keep);
        // 2. the bearings to (re)compute
        const bool all = t - recompute_ms_ > RECOMPUTE_MS;
        if (all) recompute_ms_ = t;
        for (auto& [id, tk] : talkers_) {
            if (tk.w <= 0 || (!tk.dirty && !all)) continue;
            tk.dirty = false;
            jobs.push_back({id, tk.info.label, tk.seq, true, tk.info.tx_end_ms, tk.R / tk.w});
        }
        prune_locked(t);
    }
    if (jobs.empty()) return;

    // 3. MUSIC (the processor's lock, not ours)
    struct Res { Eigen::VectorXd spec; float peak = -1, conf = 0; bool ok = false; };
    std::vector<Res> res(jobs.size());
    for (size_t i = 0; i < jobs.size(); i++)
        res[i].ok = mp.spectrumFromCovariance(jobs[i].R, &res[i].spec, &res[i].peak, &res[i].conf);
    const double step = mp.getAngularResolution();

    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < jobs.size(); i++) {
        if (!res[i].ok) continue;
        const Job& j = jobs[i];
        std::vector<float> spec(static_cast<size_t>(res[i].spec.size()));
        for (size_t k = 0; k < spec.size(); k++) spec[k] = static_cast<float>(res[i].spec(static_cast<Eigen::Index>(k)));
        if (!j.talker) {
            if (out) out->push_back({j.id, j.label, j.stamp_ms, std::move(spec), step, res[i].conf});
            continue;
        }
        auto it = talkers_.find(j.id);
        if (it == talkers_.end() || it->second.seq != j.seq) continue;   // reset / a newer transmission meanwhile
        TalkerInfo& in = it->second.info;
        in.spec = std::move(spec);
        in.res = step;
        in.doa = res[i].peak;
        in.conf = res[i].conf;
    }
}

std::vector<TalkerInfo> TalkerDoa::snapshot() const {
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TalkerInfo> v;
    v.reserve(talkers_.size());
    for (const auto& [id, tk] : talkers_) {
        v.push_back(tk.info);
        v.back().active = tk.open_ms != 0 && t - tk.open_ms < ACTIVE_MS;
    }
    std::sort(v.begin(), v.end(), [](const TalkerInfo& a, const TalkerInfo& b) { return a.last_ms > b.last_ms; });
    return v;
}

}  // namespace tdoa
