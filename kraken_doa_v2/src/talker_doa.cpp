#include "talker_doa.hpp"

#include "signal_processing/music_processor.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace tdoa {

namespace {

// a packet talker is forgotten after PACKET_TTL_MS, or 30 averaging times
// (AIS: a moored ship reports every 3 min)
int64_t packet_ttl_ms(const TalkerInfo& in) {
    return std::max<int64_t>(PACKET_TTL_MS, static_cast<int64_t>(30 * in.avg_s * 1000));
}

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
    {
        std::lock_guard<std::mutex> lk(mu_);
        reset_locked();
    }
    want_blocks_ = false;
    free_ring();
}

void TalkerDoa::reset_locked() {
    frames_.clear();
    spans_.clear();
    talkers_.clear();
    packets_.clear();
    freq_ = 0;
}

// --- packets (kp::Talker::packet) -----------------------------------------------
void TalkerDoa::free_ring() {
    std::lock_guard<std::mutex> lk(ring_mu_);
    std::vector<std::complex<float>>().swap(ring_);
    std::vector<uint64_t>().swap(ring_tag_);
    std::vector<std::complex<float>>().swap(raw_);
    raw_cap_ = 0;
    raw_lo_ = raw_hi_ = 0;
    raw_mode_ = false;
    ring_cap_ = 0;
    ring_m_ = ring_tri_ = 0;
    ring_rate_ = 0;
    part_chunk_ = ~0ull;
    part_n_ = 0;
    ring_next_ = ~0ull;
}

void TalkerDoa::add_block(const SharedDecimator::MultiChannelDecimated& d) {
    if (!wants_blocks()) return;
    const int m = static_cast<int>(std::min<size_t>(std::min(d.num_channels, d.channels.size()), 8));
    const size_t n = d.min_samples;
    const float rate = d.output_rate_hz;
    if (m < 2 || n == 0 || !(rate > 0)) return;
    const std::complex<float>* ch[8];
    for (int k = 0; k < m; k++) {
        if (d.channels[static_cast<size_t>(k)].samples.size() < n) return;
        ch[k] = d.channels[static_cast<size_t>(k)].samples.data();
    }
    std::lock_guard<std::mutex> lk(ring_mu_);
    if (rate <= RAW_MAX_RATE) {
        // keep the samples themselves
        if (!raw_mode_ || m != ring_m_ || rate != ring_rate_ || raw_cap_ == 0) {
            std::vector<std::complex<float>>().swap(ring_);
            std::vector<uint64_t>().swap(ring_tag_);
            ring_cap_ = 0;
            raw_mode_ = true;
            ring_m_ = m;
            ring_rate_ = rate;
            raw_cap_ = static_cast<size_t>(std::ceil(RING_S * rate)) + 1;
            raw_.assign(raw_cap_ * static_cast<size_t>(m), {});
            raw_lo_ = raw_hi_ = d.stream_pos;
        }
        if (d.stream_pos != raw_hi_) raw_lo_ = raw_hi_ = d.stream_pos;   // a gap: start over
        for (size_t i = 0; i < n; i++) {
            std::complex<float>* q = raw_.data() + ((d.stream_pos + i) % raw_cap_) * static_cast<size_t>(m);
            for (int k = 0; k < m; k++) q[k] = ch[k][i];
        }
        raw_hi_ = d.stream_pos + n;
        if (raw_hi_ - raw_lo_ > raw_cap_) raw_lo_ = raw_hi_ - raw_cap_;
        return;
    }
    if (raw_mode_) {
        std::vector<std::complex<float>>().swap(raw_);
        raw_cap_ = 0;
        raw_mode_ = false;
        ring_cap_ = 0;
    }
    if (m != ring_m_ || rate != ring_rate_ || ring_cap_ == 0) {
        ring_m_ = m;
        ring_tri_ = m * (m + 1) / 2;
        ring_rate_ = rate;
        ring_cap_ = static_cast<size_t>(std::ceil(RING_S * rate / CHUNK)) + 2;
        ring_.assign(ring_cap_ * static_cast<size_t>(ring_tri_), {});
        ring_tag_.assign(ring_cap_, 0);
        part_.assign(static_cast<size_t>(ring_tri_), {});
        part_chunk_ = ~0ull;
        part_n_ = 0;
    }
    if (d.stream_pos != ring_next_) part_chunk_ = ~0ull;   // a gap: the chunk in progress is incomplete
    ring_next_ = d.stream_pos + n;
    std::complex<float>* acc = part_.data();
    size_t i = 0;
    while (i < n) {
        const uint64_t p = d.stream_pos + i;
        const uint64_t c = p / CHUNK;
        const int at = static_cast<int>(p % CHUNK);
        if (c != part_chunk_) {
            if (at != 0) {   // joined mid-chunk: start with the next one
                i += static_cast<size_t>(CHUNK - at);
                continue;
            }
            part_chunk_ = c;
            part_n_ = 0;
            std::fill(part_.begin(), part_.end(), std::complex<float>{});
        }
        const size_t take = std::min(n - i, static_cast<size_t>(CHUNK - at));
        for (size_t s = i; s < i + take; s++) {
            std::complex<float> x[8];
            for (int k = 0; k < m; k++) x[k] = ch[k][s];
            int idx = 0;
            for (int a = 0; a < m; a++)
                for (int b = a; b < m; b++) acc[idx++] += x[a] * std::conj(x[b]);
        }
        part_n_ += static_cast<int>(take);
        i += take;
        if (part_n_ == CHUNK) {
            const size_t slot = static_cast<size_t>(c % ring_cap_);
            std::copy(part_.begin(), part_.end(), ring_.begin() + static_cast<long>(slot * static_cast<size_t>(ring_tri_)));
            ring_tag_[slot] = c + 1;
            part_chunk_ = ~0ull;
        }
    }
}

bool TalkerDoa::packet_cov(uint64_t a, uint64_t b, double freq, double bw, Eigen::MatrixXcd* R) const {
    std::lock_guard<std::mutex> lk(ring_mu_);
    if (raw_mode_) return packet_cov_raw(a, b, freq, bw, R);
    if (!ring_cap_) return false;
    const uint64_t c0 = (a + CHUNK - 1) / CHUNK, c1 = b / CHUNK;   // chunks entirely inside
    if (c1 < c0 + MIN_PACKET_CHUNKS || c1 - c0 >= ring_cap_) return false;
    std::vector<std::complex<double>> sum(static_cast<size_t>(ring_tri_));
    for (uint64_t c = c0; c < c1; c++) {
        const size_t slot = static_cast<size_t>(c % ring_cap_);
        if (ring_tag_[slot] != c + 1) return false;   // not taken (calibration, gap) or overwritten
        const std::complex<float>* q = ring_.data() + slot * static_cast<size_t>(ring_tri_);
        for (int k = 0; k < ring_tri_; k++) sum[static_cast<size_t>(k)] += std::complex<double>(q[k]);
    }
    const int m = ring_m_;
    R->resize(m, m);
    int idx = 0;
    double tr = 0;
    for (int i = 0; i < m; i++)
        for (int j = i; j < m; j++) {
            const std::complex<double> v = sum[static_cast<size_t>(idx++)];
            (*R)(i, j) = v;
            (*R)(j, i) = std::conj(v);
            if (i == j) tr += v.real();
        }
    if (!(tr > 1e-30)) return false;
    *R /= tr;
    return true;
}

// Raw mode (ring_mu_ held): the packet's samples of every antenna; with a
// channel, each antenna is mixed down by freq and low-pass filtered (Hamming
// windowed sinc, cut-off bw / 2, taps ~ 3 fs / bw), the filter's run-in taken
// from the samples before the packet
bool TalkerDoa::packet_cov_raw(uint64_t a, uint64_t b, double freq, double bw, Eigen::MatrixXcd* R) const {
    const int m = ring_m_;
    if (!raw_cap_ || m < 2 || b <= a) return false;
    const bool filt = is_finite_value(freq) && bw > 0 && bw < ring_rate_;
    int L = 0;
    std::vector<float> h;
    if (filt) {
        L = std::clamp(2 * static_cast<int>(std::ceil(3.0 * ring_rate_ / bw)) + 1, 15, 401);
        h.resize(static_cast<size_t>(L));
        const double fc = bw / 2 / ring_rate_;
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double x = i - (L - 1) / 2.0;
            const double sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            h[static_cast<size_t>(i)] = static_cast<float>(sinc * (0.54 - 0.46 * std::cos(2 * M_PI * i / (L - 1))));
            sum += h[static_cast<size_t>(i)];
        }
        for (auto& v : h) v = static_cast<float>(v / sum);
    }
    const uint64_t half = static_cast<uint64_t>(L / 2);
    if (a < raw_lo_ + half || b + half > raw_hi_) return false;
    const size_t n = static_cast<size_t>(b - a);
    // the packet's samples (+ the filter's margins), mixed to the channel
    const uint64_t s0 = a - half;
    const size_t ns = n + 2 * static_cast<size_t>(half);
    std::vector<std::complex<float>> x(ns * static_cast<size_t>(m));
    const double w = filt ? -2 * M_PI * freq / ring_rate_ : 0;
    for (size_t i = 0; i < ns; i++) {
        const std::complex<float>* q = raw_.data() + ((s0 + i) % raw_cap_) * static_cast<size_t>(m);
        const std::complex<float> rot = filt ? std::polar(1.0f, static_cast<float>(std::remainder(w * static_cast<double>(s0 + i), 2 * M_PI)))
                                             : std::complex<float>(1, 0);
        for (int k = 0; k < m; k++) x[i * static_cast<size_t>(m) + static_cast<size_t>(k)] = q[k] * rot;
    }
    Eigen::MatrixXcd acc = Eigen::MatrixXcd::Zero(m, m);
    std::vector<std::complex<float>> y(static_cast<size_t>(m));
    for (size_t i = 0; i < n; i++) {
        if (filt) {
            std::fill(y.begin(), y.end(), std::complex<float>{});
            for (int t = 0; t < L; t++) {
                const std::complex<float>* q = x.data() + (i + static_cast<size_t>(t)) * static_cast<size_t>(m);
                const float g = h[static_cast<size_t>(L - 1 - t)];
                for (int k = 0; k < m; k++) y[static_cast<size_t>(k)] += q[k] * g;
            }
        } else {
            for (int k = 0; k < m; k++) y[static_cast<size_t>(k)] = x[i * static_cast<size_t>(m) + static_cast<size_t>(k)];
        }
        for (int r = 0; r < m; r++)
            for (int c = r; c < m; c++)
                acc(r, c) += std::complex<double>(y[static_cast<size_t>(r)] * std::conj(y[static_cast<size_t>(c)]));
    }
    double tr = 0;
    for (int r = 0; r < m; r++) {
        tr += acc(r, r).real();
        for (int c = r + 1; c < m; c++) acc(c, r) = std::conj(acc(r, c));
    }
    if (!(tr > 1e-30)) return false;
    *R = acc / tr;
    return true;
}

void TalkerDoa::add_packet(const dig::TalkerSpan& s) {
    want_blocks_ = true;   // the stream is kept from now on (this first packet may come too early)
    Eigen::MatrixXcd R;
    const bool ok = packet_cov(s.start, s.end, s.freq_hz, s.bw_hz, &R);
    const int64_t t = now_ms();
    std::lock_guard<std::mutex> lk(mu_);
    last_packet_ms_ = t;
    if (!ok) return;
    Packet p{s.plugin, s.id, s.label, s.start, s.end, s.rate, s.freq_hz, s.bw_hz, s.avg_s, std::move(R), t, false};
    // overlapping another talker's packet on the same channel (both decoded
    // through each other): neither holds one talker's signal alone. Packets
    // come in stream order, so an overlapping one is among the latest.
    auto same_channel = [](const Packet& x, const Packet& y) {
        if (!is_finite_value(x.freq) || !is_finite_value(y.freq)) return true;
        return std::fabs(x.freq - y.freq) < 0.5 * std::max(x.bw, y.bw) + 1;
    };
    int k = 0;
    for (auto it = packets_.rbegin(); it != packets_.rend() && k < 64; ++it, ++k)
        if (it->a < p.b && p.a < it->b && it->id != p.id && same_channel(*it, p)) it->bad = p.bad = true;
    packets_.push_back(std::move(p));
    while (packets_.size() > 20000) packets_.pop_front();
}

void TalkerDoa::commit_packet_locked(Packet& p, int64_t now) {
    Talker& tk = talkers_[p.id];
    if (tk.info.id.empty()) {
        tk.info.id = p.id;
        tk.info.first_ms = now;
    }
    tk.info.packet = true;
    tk.info.avg_s = p.avg > 0 ? p.avg : PACKET_TAU_S;
    tk.info.plugin = p.plugin;
    if (!p.label.empty()) tk.info.label = p.label;
    const double ts = static_cast<double>(p.a) / p.rate;
    if (tk.R.rows() != p.R.rows()) {
        tk.R = Eigen::MatrixXcd::Zero(p.R.rows(), p.R.cols());
        tk.w = 0;
        tk.last_s = ts;
    } else if (ts > tk.last_s) {
        const double k = std::exp(-(ts - tk.last_s) / tk.info.avg_s);
        tk.R *= k;
        tk.w *= k;
    }
    tk.R += p.R;
    tk.w += 1;
    tk.last_s = std::max(tk.last_s, ts);
    tk.info.tx++;
    tk.info.frames++;
    tk.info.tx_frames = static_cast<int>(std::lround(tk.w));
    if (!tk.info.tx_start_ms) tk.info.tx_start_ms = p.stamp_ms;
    tk.info.tx_end_ms = p.stamp_ms;
    tk.info.last_ms = now;
    tk.open_ms = now;
    tk.dirty = true;
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
    if (s.packet) {
        add_packet(s);
        return;
    }
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
        if (now - it->second.info.last_ms > (it->second.info.packet ? packet_ttl_ms(it->second.info) : TALKER_TTL_MS))
            it = talkers_.erase(it);
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
    bool idle = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 0. packets that waited long enough for an overlapping one
        while (!packets_.empty() && t - packets_.front().stamp_ms >= PACKET_HOLD_MS) {
            if (!packets_.front().bad) commit_packet_locked(packets_.front(), t);
            packets_.pop_front();
        }
        idle = want_blocks_.load() && t - last_packet_ms_ > PACKET_IDLE_MS;
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
            if (tk.w <= 0) continue;
            if (tk.info.packet) {   // follows its packets (and the array settings with them), throttled
                if (!tk.dirty || t - tk.computed_ms < PACKET_RECOMPUTE_MS) continue;
                tk.computed_ms = t;
            } else if (!tk.dirty && !all) {
                continue;
            }
            tk.dirty = false;
            jobs.push_back({id, tk.info.label, tk.seq, true, tk.info.tx_end_ms, tk.R / tk.w});
        }
        prune_locked(t);
    }
    if (idle) {   // no packets for a while: stop keeping the stream
        want_blocks_ = false;
        free_ring();
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
        if (in.packet && in.doa >= 0 && t - it->second.hist_ms >= PACKET_HIST_MS) {
            // an aircraft's bearing track: the previous bearing every PACKET_HIST_MS
            if (it->second.hist_ms) {
                in.hist.push_back({in.spec_ms, in.doa, in.conf, in.tx_frames});
                if (in.hist.size() > MAX_HIST) in.hist.erase(in.hist.begin());
            }
            it->second.hist_ms = t;
        }
        in.spec_ms = t;
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
