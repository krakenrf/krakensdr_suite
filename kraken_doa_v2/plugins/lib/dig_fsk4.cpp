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
//
// The discriminator is limited to +-6 kHz per sample: against noise, the
// limited boxcar decodes weak C4FM better than the exact one. But P25 CQPSK
// transmitters are read through the same discriminator, and their envelope
// passes near zero between symbols, where the phase steps faster than 6 kHz
// per sample: on a real CQPSK voice channel the limit took up to 1.4 kHz off
// half of the symbols, squeezed the outer levels from +-1.8 to +-1.3 kHz
// (more on the side the carrier is offset to), the centre fitted on the
// sync scattered by +-110 Hz, and clean frames were lost. So a second P25
// boxcar (bpu_) gets back what the limit took off - the phase advance over
// the symbol as it really is, taken modulo one turn (4800 Hz here) nearest
// to the limited value, which also drops a whole noise click. Each frame is
// sliced from the limited boxcar unless the limit squeezed its sync (all
// +-3 symbols) by more than 15 %: then its transmitter's own modulation
// steps that fast (CQPSK: 0.65-0.78 of the exact scale at 12 dB SNR to
// clean; C4FM / simulcast LSM: 0.9-1.0 down to 4 dB, 1.0 clean) and the
// frame comes from bpu_. Sync detection stays on the limited boxcar, and
// DMR's RRC path keeps the plain limit, so C4FM / LSM / DMR are unchanged.
//
// A P25 frame is handed over as soon as its own length is in: the NID's
// DUID gives it (a TSDU: up to its first failing or its last block), so
// short frames - trunking blocks, terminators - are not held back for an
// LDU's 180 ms, and those at the end of a recording are not lost. Frames
// still go out in sync order.

#include "dig_fsk4.hpp"
#include "dig_fec.hpp"

#include <algorithm>
#include <cmath>

namespace dig {

static constexpr int SPS = 10;
static constexpr int SYNC_SYMS = 24;
static constexpr float SYNC_THRESHOLD = 0.80f;
static constexpr int64_t KEEP_SAMPLES = 16000;   // P25 frame (8640) + margin
static constexpr float LIMIT_HZ = 6000.0f;       // discriminator limit per sample
static constexpr float TURN_HZ = 48000.0f / SPS; // a full turn of phase over a symbol, on the boxcar
static constexpr float SQUEEZED = 0.85f;         // sync scale limited / exact below this: CQPSK
// a DMR burst's symbols from its sync's end: -89 (CACH) .. +54 (dmr.cpp),
// one symbol of margin
static constexpr int DMR_BEFORE_SYMS = 90;

// P25 frame lengths in dibits from the first sync dibit, status symbols
// included: sf status-free dibits plus a status symbol after every 35
static constexpr int p25_raw_len(int sf) { return sf + (sf - 1) / 35; }
static constexpr int p25_raw_index(int sf) { return sf + sf / 35; }   // status-free dibit -> frame dibit
static constexpr int P25_NID_END = p25_raw_len(56);   // sync 24 + NID 32 dibits

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
    bpu_ = SampleBuf();
    bd_ = SampleBuf();
    std::fill(rrc_hist_.begin(), rrc_hist_.end(), 0.0f);
    std::fill(std::begin(box_hist_), std::end(box_hist_), 0.0f);
    std::fill(std::begin(clip_hist_), std::end(clip_hist_), 0.0f);
    box_sum_ = 0;
    p25_hint_nac_ = -1;
    pk_p25_ = Peak();
    pk_dmr_ = Peak();
    jobs_.clear();
    dmr_expect_ = -1;
    dmr_confirmed_ = false;
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
    bool voice_on_grid = false;   // a voice sync on a confirmed sequence's grid
    if (proto == Mode::DMR) {
        static const uint64_t words[4] = {DMR_BS_VOICE, DMR_MS_VOICE, DMR_TS1_VOICE, DMR_TS2_VOICE};
        static const DmrSync v[4] = {DmrSync::BS_VOICE, DmrSync::MS_VOICE, DmrSync::TS1_VOICE, DmrSync::TS2_VOICE};
        static const DmrSync d[4] = {DmrSync::BS_DATA, DmrSync::MS_DATA, DmrSync::TS1_DATA, DmrSync::TS2_DATA};
        // a negative correlation is the complementary (data) sync; DMR
        // polarity is taken as normal (heimdall delivers upright spectra)
        word = sign > 0 ? words[pat] : (words[pat] ^ 0xAAAAAAAAAAAAULL);   // +3 <-> -3
        st = sign > 0 ? v[pat] : d[pat];
        // Nothing checks a voice sync (burst A carries no signalling), and
        // random voice payload does correlate >= 0.8 with a sync word now and
        // then (~1 in 50 bursts of random AMBE data; a BS pattern 10 ms after
        // a radio's MS sync in a real recording): taken as burst A, it moved
        // the extrapolation of the following bursts B..F off their timing and
        // they were lost. While a CONFIRMED burst sequence is followed (its
        // last bursts checked out: EMB / slot type + FEC, or a voice sync on
        // its grid), every burst of the channel lies on its 30 ms grid: a
        // voice sync off that grid is ignored. A sequence a bare voice sync
        // started is not confirmed (it may be that false one): any sync
        // re-anchors it, as before.
        // A DMR burst is read from 89 symbols BEFORE its sync's end (its
        // CACH and first half): a sync that ends sooner than that after the
        // start of the stream or a reset() - the samples there were never
        // received - would read in front of the buffer. Its burst is not
        // complete anyway: skipped (the first burst after a retune, at most).
        if (!b.has(sync_end - DMR_BEFORE_SYMS * SPS)) return;
        if (sign > 0 && dmr_expect_ >= 0) {
            const int64_t per = 144 * SPS;
            const int64_t off = ((sync_end - dmr_expect_) % per + per) % per;
            const bool on_grid = std::min(off, per - off) <= 3 * SPS;
            if (dmr_confirmed_ && !on_grid) return;
            voice_on_grid = dmr_confirmed_ && on_grid;
        }
    }
    // least-squares fit of the sync samples to their nominal levels
    const std::vector<float> s = sync_symbols(word);
    const int64_t first = sync_end - static_cast<int64_t>(SYNC_SYMS - 1) * SPS;
    auto fit = [&](const SampleBuf& fb, float* scale_out, float* center_out) {
        float sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int k = 0; k < SYNC_SYMS; k++) {
            float y = fb.at(first + k * SPS);
            sx += s[k]; sy += y; sxx += s[k] * s[k]; sxy += s[k] * y;
        }
        const float N = SYNC_SYMS;
        float den = N * sxx - sx * sx;
        *scale_out = (N * sxy - sx * sy) / den;
        *center_out = (sy - *scale_out * sx) / N;
    };
    float scale, center;
    fit(b, &scale, &center);
    const SampleBuf* src_buf = &b;
    if (proto == Mode::P25) {
        // the limit squeezed the sync: a CQPSK transmitter - slice the
        // frame from the boxcar that has its phase steps back (header)
        float scale_u, center_u;
        fit(bpu_, &scale_u, &center_u);
        if (std::fabs(scale) < SQUEEZED * std::fabs(scale_u)) {
            scale = scale_u;
            center = center_u;
            src_buf = &bpu_;
        }
    }
    if (std::fabs(scale) < 50.0f) return;   // < 150 Hz outer deviation: noise

    SymSrc src;
    src.buf = src_buf;
    src.sync_end = sync_end;
    src.sps = SPS;
    src.scale = scale;
    src.center = center;
    if (proto == Mode::P25) {
        jobs_.push_back({Mode::P25, src, P25_NID_END - SYNC_SYMS, DmrSync::NONE, false});
    } else {
        // a real sync replaces an extrapolated burst at the same place
        if (dmr_expect_ >= 0 && std::llabs(sync_end - dmr_expect_) < SPS * 3) dmr_expect_ = -1;
        jobs_.push_back({Mode::DMR, src, 54, st, voice_on_grid});
    }
}

// A P25 job's samples up to need_k are in: work out how far the frame goes
void Fsk4Receiver::p25_length(Job& j) {
    const SymSrc& s = j.src;
    // status-free dibit sf of the frame (frame dibit i = symbol i - 23)
    auto soft = [&](int sf) { return s.sym(p25_raw_index(sf) - (SYNC_SYMS - 1)); };
    auto set_len = [&](int raw, bool done) {
        j.need_k = raw - SYNC_SYMS;
        j.done = done;
    };
    if (j.stage == 0) {   // the NID is in
        uint8_t bits[112];
        for (int sf = 0; sf < 56; sf++) {
            const uint8_t d = SymSrc::dibit(soft(sf));
            bits[2 * sf] = d >> 1;
            bits[2 * sf + 1] = d & 1;
        }
        uint32_t msg = 0;
        const int e = p25_nid_decode(bits_to_u64(bits + 48, 63), p25_hint_nac_, &msg);
        if (e > 9) { set_len(P25_NID_END, true); return; }   // the frame decoder rejects it too
        if (e <= 4) p25_hint_nac_ = static_cast<int>(msg >> 4);
        switch (msg & 0xF) {
            case 0x0: set_len(396, true); break;                         // HDU
            case 0x3: set_len(72, true); break;                          // TDU
            case 0xF: set_len(216, true); break;                         // TDULC
            case 0x5: case 0xA: set_len(P25_FRAME_DIBITS, true); break;  // LDU1 / LDU2
            case 0x7:                                                    // TSDU: block by block
                j.stage = 1;
                set_len(p25_raw_len(56 + 98), false);
                break;
            case 0xC: set_len(p25_raw_len(56 + 98), true); break;        // PDU: its header block
            default: set_len(P25_NID_END, true); break;                  // no such DUID: rejected
        }
        return;
    }
    // TSDU block stage - 1 is in; the next one belongs to the frame only if
    // this one is good and not the last (what the frame decoder reads)
    float sym98[98];
    for (int k = 0; k < 98; k++) sym98[k] = soft(56 + 98 * (j.stage - 1) + k);
    uint8_t t[12];
    float metric;
    p25_trellis_half(sym98, t, &metric);
    const bool good = crc16_ccitt_bytes(t, 10, 0x0000) == ((t[10] << 8) | t[11]);
    if (!good || (t[0] & 0x80) || j.stage == 3) {
        j.done = true;
        return;
    }
    j.stage++;
    set_len(p25_raw_len(56 + 98 * j.stage), false);
}

void Fsk4Receiver::run_jobs() {
    bool p25_waiting = false;   // an earlier P25 frame is not complete yet: keep the order
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const SampleBuf& b = it->proto == Mode::P25 ? bp_ : bd_;
        int64_t need = it->src.sync_end + static_cast<int64_t>(it->need_k) * SPS;
        if (it->proto == Mode::P25) {
            if (p25_waiting) { ++it; continue; }
            while (need < b.end() && !it->done) {
                p25_length(*it);
                need = it->src.sync_end + static_cast<int64_t>(it->need_k) * SPS;
            }
            if (need >= b.end()) { p25_waiting = true; ++it; continue; }
            // the frame decoder reads P25_FRAME_DIBITS: a short frame whose
            // tail is not in yet gets it as 0 (= the centre)
            const int64_t last = it->src.sync_end + static_cast<int64_t>(P25_FRAME_DIBITS - SYNC_SYMS) * SPS;
            if (last < b.end()) {
                sink_.p25_frame(it->src);
            } else {
                const SampleBuf& fb = *it->src.buf;   // bp_ or bpu_
                const int64_t first = std::max(fb.begin(), it->src.sync_end - static_cast<int64_t>(SYNC_SYMS) * SPS);
                p25_pad_.restart(first);
                for (int64_t a = first; a < fb.end(); a++) p25_pad_.push(fb.at(a));
                while (p25_pad_.end() <= last) p25_pad_.push(it->src.center);
                SymSrc s = it->src;
                s.buf = &p25_pad_;
                sink_.p25_frame(s);
            }
        } else {
            if (need >= b.end()) { ++it; continue; }
            int period = 144;
            if (sink_.dmr_burst(it->src, it->sync, &period)) {
                dmr_last_src_ = it->src;
                dmr_period_ = period;
                dmr_expect_ = it->src.sync_end + static_cast<int64_t>(period) * SPS;
                dmr_expect_left_ = 12;   // keep extrapolating through voice bursts B..F
                // data bursts are checked (slot type + FEC); a voice sync only
                // confirms a sequence it continues on the grid
                const bool voice = it->sync == DmrSync::BS_VOICE || it->sync == DmrSync::MS_VOICE ||
                                   it->sync == DmrSync::TS1_VOICE || it->sync == DmrSync::TS2_VOICE;
                dmr_confirmed_ = !voice || it->on_grid;
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
        bool ok = bd_.has(s.sync_end - DMR_BEFORE_SYMS * SPS) && sink_.dmr_burst(s, DmrSync::NONE, &period);
        dmr_expect_ += static_cast<int64_t>(dmr_period_) * SPS;
        if (ok) dmr_confirmed_ = true;   // its EMB / slot type checked out
        if (!ok && --dmr_expect_left_ <= 0) {
            dmr_expect_ = -1;
            dmr_confirmed_ = false;
        }
    }
}

void Fsk4Receiver::process(const float* d, size_t n) {
    const size_t nt = rrc_taps_.size();
    for (size_t i = 0; i < n; i++) {
        float x = d[i];
        // limit discriminator spikes (noise clicks) to +-6 kHz
        if (x > LIMIT_HZ) x = LIMIT_HZ;
        if (x < -LIMIT_HZ) x = -LIMIT_HZ;
        box_sum_ += x - box_hist_[box_pos_];
        box_hist_[box_pos_] = x;
        clip_hist_[box_pos_] = d[i] - x;
        box_pos_ = (box_pos_ + 1) % SPS;
        const float v = box_sum_ / SPS;
        bp_.push(v);
        if (en_p25_) {
            // the same with what the limit took off given back, modulo one
            // turn (header comment)
            float clip = 0;
            for (float c : clip_hist_) clip += c;
            if (clip != 0.0f) {
                clip /= SPS;
                bpu_.push(v + (clip - TURN_HZ * std::round(clip / TURN_HZ)));
            } else {
                bpu_.push(v);
            }
        }
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
        bpu_.trim_before(keep);
        bd_.trim_before(keep);
    }
}

}  // namespace dig
