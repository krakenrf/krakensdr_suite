// 4FSK receiver for P25 Phase 1 (C4FM) and DMR, 4800 symbols/s at 48 kHz.
//
// The discriminator output is filtered twice: an integrate-and-dump boxcar
// (the C4FM receive filter - P25 transmitters pre-compensate it) and a root
// raised cosine 0.2 (DMR's matched filter). Each stream is correlated against
// its sync words at every sample; a peak above 0.8 (normalized) is a sync.
// The sync's own +-3 symbols give the symbol timing, the level scale and the
// centre (= carrier offset), so the frame is sliced without any timing loop:
// a P25 frame is at most 180 ms and a DMR burst 30 ms, far less than the
// symbol clock drift of a radio would matter over.

#include "dig_fsk4.hpp"
#include "dig_fec.hpp"

#include <algorithm>
#include <cmath>

namespace dig {

static constexpr int SPS = 10;
static constexpr int SYNC_SYMS = 24;
static constexpr float SYNC_THRESHOLD = 0.80f;
static constexpr int64_t KEEP_SAMPLES = 16000;   // P25 frame (8640) + margin

// 48-bit sync word (24 dibits, MSB first) -> +-3 symbol values
static std::vector<float> sync_symbols(uint64_t word) {
    std::vector<float> s(SYNC_SYMS);
    for (int i = 0; i < SYNC_SYMS; i++) {
        int d = static_cast<int>((word >> (46 - 2 * i)) & 3);
        s[i] = d == 1 ? 3.0f : (d == 0 ? 1.0f : (d == 2 ? -1.0f : -3.0f));
    }
    return s;
}

static std::vector<float> normalized(const std::vector<float>& s) {
    float m = 0;
    for (float v : s) m += v;
    m /= static_cast<float>(s.size());
    std::vector<float> t(s.size());
    float e = 0;
    for (size_t i = 0; i < s.size(); i++) { t[i] = s[i] - m; e += t[i] * t[i]; }
    e = std::sqrt(e);
    for (float& v : t) v /= e;
    return t;
}

// sync words
static constexpr uint64_t P25_SYNC = 0x5575F5FF77FFULL;
static constexpr uint64_t DMR_BS_VOICE = 0x755FD7DF75F7ULL;   // data sync = complement
static constexpr uint64_t DMR_MS_VOICE = 0x7F7D5DD57DFDULL;
static constexpr uint64_t DMR_TS1_VOICE = 0x5D577F7757FFULL;
static constexpr uint64_t DMR_TS2_VOICE = 0x7DFFD5F55D5FULL;

Fsk4Receiver::Fsk4Receiver(Fsk4Sink& sink, bool p25, bool dmr) : sink_(sink), en_p25_(p25), en_dmr_(dmr) {
    pats_p25_.push_back({normalized(sync_symbols(P25_SYNC)), Mode::P25, 0});
    const uint64_t dmr_words[4] = {DMR_BS_VOICE, DMR_MS_VOICE, DMR_TS1_VOICE, DMR_TS2_VOICE};
    for (int i = 0; i < 4; i++) pats_dmr_.push_back({normalized(sync_symbols(dmr_words[i])), Mode::DMR, i});
    // RRC alpha 0.2, +-4 symbols, unity DC gain (keeps the output in Hz)
    const float alpha = 0.2f;
    for (int i = -4 * SPS; i <= 4 * SPS; i++) {
        float t = static_cast<float>(i) / SPS, h;
        if (std::fabs(t) < 1e-6f) h = 1.0f - alpha + 4.0f * alpha / static_cast<float>(M_PI);
        else if (std::fabs(std::fabs(4.0f * alpha * t) - 1.0f) < 1e-6f)
            h = alpha / std::sqrt(2.0f) * ((1 + 2 / static_cast<float>(M_PI)) * std::sin(static_cast<float>(M_PI) / (4 * alpha)) +
                                          (1 - 2 / static_cast<float>(M_PI)) * std::cos(static_cast<float>(M_PI) / (4 * alpha)));
        else {
            float pt = static_cast<float>(M_PI) * t;
            h = (std::sin(pt * (1 - alpha)) + 4 * alpha * t * std::cos(pt * (1 + alpha))) /
                (pt * (1 - (4 * alpha * t) * (4 * alpha * t)));
        }
        rrc_taps_.push_back(h);
    }
    float s = 0;
    for (float v : rrc_taps_) s += v;
    for (float& v : rrc_taps_) v /= s;
    rrc_hist_.assign(rrc_taps_.size(), 0.0f);
}

void Fsk4Receiver::reset() {
    bp_ = SampleBuf();
    bd_ = SampleBuf();
    std::fill(rrc_hist_.begin(), rrc_hist_.end(), 0.0f);
    std::fill(std::begin(box_hist_), std::end(box_hist_), 0.0f);
    box_sum_ = 0;
    pk_p25_ = Peak();
    pk_dmr_ = Peak();
    jobs_.clear();
    dmr_expect_ = -1;
    sink_.reset();
}

void Fsk4Receiver::correlate(SampleBuf& b, std::vector<Pattern>& pats, Peak& pk, Mode proto) {
    const int64_t n = b.end() - 1;
    const int64_t first = n - static_cast<int64_t>(SYNC_SYMS - 1) * SPS;
    if (first < b.begin()) return;
    float w[SYNC_SYMS];
    float m = 0;
    for (int k = 0; k < SYNC_SYMS; k++) { w[k] = b.at(first + k * SPS); m += w[k]; }
    m /= SYNC_SYMS;
    float e = 0;
    for (int k = 0; k < SYNC_SYMS; k++) { w[k] -= m; e += w[k] * w[k]; }
    if (e < 1e-3f) return;
    const float inv = 1.0f / std::sqrt(e);
    float best = 0;
    int best_pat = 0;
    for (size_t p = 0; p < pats.size(); p++) {
        float c = 0;
        const float* t = pats[p].t.data();
        for (int k = 0; k < SYNC_SYMS; k++) c += t[k] * w[k];
        c *= inv;
        if (std::fabs(c) > std::fabs(best)) { best = c; best_pat = static_cast<int>(p); }
    }
    if (pk.active) {
        pk.age++;
        if (std::fabs(best) > std::fabs(pk.best)) { pk.best = best; pk.idx = n; pk.pat = best_pat; }
        if (pk.age >= SPS) {   // peak window of one symbol
            pk.active = false;
            on_sync(proto, pk.pat, pk.idx, pk.best > 0 ? 1.0f : -1.0f);
        }
    } else if (std::fabs(best) >= SYNC_THRESHOLD) {
        pk.active = true;
        pk.best = best;
        pk.idx = n;
        pk.pat = best_pat;
        pk.age = 0;
    }
}

void Fsk4Receiver::on_sync(Mode proto, int pat, int64_t sync_end, float sign) {
    SampleBuf& b = proto == Mode::P25 ? bp_ : bd_;
    uint64_t word = P25_SYNC;
    DmrSync st = DmrSync::NONE;
    if (proto == Mode::DMR) {
        static const uint64_t words[4] = {DMR_BS_VOICE, DMR_MS_VOICE, DMR_TS1_VOICE, DMR_TS2_VOICE};
        static const DmrSync v[4] = {DmrSync::BS_VOICE, DmrSync::MS_VOICE, DmrSync::TS1_VOICE, DmrSync::TS2_VOICE};
        static const DmrSync d[4] = {DmrSync::BS_DATA, DmrSync::MS_DATA, DmrSync::TS1_DATA, DmrSync::TS2_DATA};
        // a negative correlation is the complementary (data) sync; DMR
        // polarity is taken as normal (heimdall delivers upright spectra)
        word = sign > 0 ? words[pat] : (words[pat] ^ 0xAAAAAAAAAAAAULL);   // +3 <-> -3
        st = sign > 0 ? v[pat] : d[pat];
    }
    // least-squares fit of the sync samples to their nominal levels
    std::vector<float> s = sync_symbols(word);
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    const int64_t first = sync_end - static_cast<int64_t>(SYNC_SYMS - 1) * SPS;
    for (int k = 0; k < SYNC_SYMS; k++) {
        float y = b.at(first + k * SPS);
        sx += s[k]; sy += y; sxx += s[k] * s[k]; sxy += s[k] * y;
    }
    const float N = SYNC_SYMS;
    float den = N * sxx - sx * sx;
    float scale = (N * sxy - sx * sy) / den;
    float center = (sy - scale * sx) / N;
    if (std::fabs(scale) < 50.0f) return;   // < 150 Hz outer deviation: noise

    SymSrc src;
    src.buf = &b;
    src.sync_end = sync_end;
    src.sps = SPS;
    src.scale = scale;
    src.center = center;
    if (proto == Mode::P25) {
        jobs_.push_back({Mode::P25, src, P25_FRAME_DIBITS - SYNC_SYMS, DmrSync::NONE});
    } else {
        // a real sync replaces an extrapolated burst at the same place
        if (dmr_expect_ >= 0 && std::llabs(sync_end - dmr_expect_) < SPS * 3) dmr_expect_ = -1;
        jobs_.push_back({Mode::DMR, src, 54, st});
    }
}

void Fsk4Receiver::run_jobs() {
    const int64_t end = bp_.end();
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const SampleBuf& b = it->proto == Mode::P25 ? bp_ : bd_;
        int64_t need = it->src.sync_end + static_cast<int64_t>(it->need_k) * SPS;
        if (need >= b.end()) { ++it; continue; }
        if (it->proto == Mode::P25) {
            sink_.p25_frame(it->src);
        } else {
            int period = 144;
            if (sink_.dmr_burst(it->src, it->sync, &period)) {
                dmr_last_src_ = it->src;
                dmr_period_ = period;
                dmr_expect_ = it->src.sync_end + static_cast<int64_t>(period) * SPS;
                dmr_expect_left_ = 12;   // keep extrapolating through voice bursts B..F
            }
        }
        it = jobs_.erase(it);
    }
    // extrapolated DMR burst (voice B..F carry no sync): decode it once the
    // samples are in and no real sync claimed the position
    if (en_dmr_ && dmr_expect_ >= 0 && dmr_expect_ + (54 + 4) * SPS < bd_.end()) {
        SymSrc s = dmr_last_src_;
        s.sync_end = dmr_expect_;
        int period = dmr_period_;
        bool ok = bd_.has(s.sync_end - 90 * SPS) && sink_.dmr_burst(s, DmrSync::NONE, &period);
        dmr_expect_ += static_cast<int64_t>(dmr_period_) * SPS;
        if (!ok && --dmr_expect_left_ <= 0) dmr_expect_ = -1;
    }
    (void)end;
}

void Fsk4Receiver::process(const float* d, size_t n) {
    const size_t nt = rrc_taps_.size();
    for (size_t i = 0; i < n; i++) {
        float x = d[i];
        // limit discriminator spikes (noise clicks) to +-6 kHz
        if (x > 6000.0f) x = 6000.0f;
        if (x < -6000.0f) x = -6000.0f;
        box_sum_ += x - box_hist_[box_pos_];
        box_hist_[box_pos_] = x;
        box_pos_ = (box_pos_ + 1) % SPS;
        bp_.push(box_sum_ / SPS);
        rrc_hist_[rrc_pos_] = x;
        float acc = 0;
        size_t p = rrc_pos_;
        for (size_t k = 0; k < nt; k++) {
            acc += rrc_taps_[k] * rrc_hist_[p];
            p = p == 0 ? nt - 1 : p - 1;
        }
        rrc_pos_ = (rrc_pos_ + 1) % nt;
        bd_.push(acc);
        if (en_p25_) correlate(bp_, pats_p25_, pk_p25_, Mode::P25);
        if (en_dmr_) correlate(bd_, pats_dmr_, pk_dmr_, Mode::DMR);
    }
    run_jobs();
    // keep the history short; never drop samples a pending job still needs
    int64_t keep = bp_.end() - KEEP_SAMPLES;
    for (const auto& j : jobs_) keep = std::min(keep, j.src.sync_end - 100 * SPS);
    if (dmr_expect_ >= 0) keep = std::min(keep, dmr_expect_ - 100 * SPS);
    if (keep > bp_.begin() + 4096) {
        bp_.trim_before(keep);
        bd_.trim_before(keep);
    }
}

}  // namespace dig
