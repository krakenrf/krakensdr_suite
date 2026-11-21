#include "aero_burst.hpp"

#include "aero_fec.hpp"
#include "aero_pchan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aero {

namespace {
constexpr double FS = 24000;
constexpr double RING_S = 14.0;        // longest burst kept (a T burst of 31 SUs at 600 bit/s is ~11 s)
constexpr float TONE_DB = 14.0f;       // the preamble's tone over the median of the spectrum
constexpr float TONE_ISO_DB = 10.0f;   // ... and over the bins 3..6 away (a tone, not a modulated spectrum)

struct BurstRate {
    double rate;
    int unmod, mod;   // preamble: unmodulated bit periods, modulated bits (table 4-12)
};
const BurstRate RATES[] = {{600, 150, 74}, {1200, 126, 74}, {2400, 78, 74}, {10500, 248, 256}};
}  // namespace

BurstRx::BurstRx(kp::Host& h, SuDecoder& su) : host(h), su_(su) {
    in_ = kfft_alloc_complex(NFFT);
    out_ = kfft_alloc_complex(NFFT);
    plan_ = kfft_plan_dft_1d(NFFT, in_, out_, KFFT_FORWARD);
    win_.resize(NFFT);
    for (int i = 0; i < NFFT; i++) win_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2 * kp::PI * static_cast<float>(i) / NFFT);
    pw_.resize(NFFT);
    reset();
}

BurstRx::~BurstRx() {
    kfft_destroy_plan(plan_);
    kfft_free(in_);
    kfft_free(out_);
}

void BurstRx::reset() {
    ring_.clear();
    ring_base_ = nin_ = 0;
    next_fft_ = NFFT;
    tbase_ = false;
    active_ = false;
    tone_bin_ = -1;
    tone_hits_ = quiet_ = 0;
    bursts_ = decoded_ = 0;
}

void BurstRx::process(const cf* x, size_t n, double t0) {
    if (!tbase_) {
        t0_base_ = t0;
        tbase_ = true;
    }
    ring_.insert(ring_.end(), x, x + n);
    nin_ += static_cast<int64_t>(n);
    const size_t keep = static_cast<size_t>(RING_S * FS);
    if (ring_.size() > keep && !active_) {
        const size_t d = ring_.size() - keep;
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(d));
        ring_base_ += static_cast<int64_t>(d);
    }
    while (next_fft_ <= nin_) {
        fft_at(next_fft_);
        next_fft_ += HOP;
    }
}

void BurstRx::fft_at(int64_t end) {
    const int64_t a = end - NFFT;
    if (a < ring_base_) return;
    for (int i = 0; i < NFFT; i++) {
        const cf v = ring_[static_cast<size_t>(a - ring_base_ + i)] * win_[static_cast<size_t>(i)];
        in_[i][0] = v.real();
        in_[i][1] = v.imag();
    }
    kfft_execute(plan_);
    for (int k = 0; k < NFFT; k++) pw_[static_cast<size_t>(k)] = out_[k][0] * out_[k][0] + out_[k][1] * out_[k][1];
    std::vector<float> tmp(pw_);
    std::nth_element(tmp.begin(), tmp.begin() + NFFT / 2, tmp.end());
    const float med = std::max(tmp[NFFT / 2], 1e-30f);
    // the strongest bin, and whether it is a tone
    int k = 0;
    for (int i = 1; i < NFFT; i++)
        if (pw_[static_cast<size_t>(i)] > pw_[static_cast<size_t>(k)]) k = i;
    auto at = [&](int i) { return pw_[static_cast<size_t>(((i % NFFT) + NFFT) % NFFT)]; };
    float side = 0;
    for (int d = 3; d <= 6; d++) side = std::max({side, at(k - d), at(k + d)});
    const bool tone = 10 * std::log10(at(k) / med) >= TONE_DB && 10 * std::log10(at(k) / std::max(side, 1e-30f)) >= TONE_ISO_DB;
    auto bin_hz = [&](double b) {
        double f = b * FS / NFFT;
        return f >= FS / 2 ? f - FS : f;
    };
    if (!active_) {
        if (!tone) {
            tone_hits_ = 0;
            return;
        }
        if (tone_hits_ > 0 && std::abs(k - tone_bin_) <= 1) {
            tone_hits_++;
        } else {
            tone_hits_ = 1;
            start_ = a;
        }
        tone_bin_ = k;
        tone_seen_ = end;
        // two FFTs in a row - or one, well above the floor (the 10.5 kbit/s
        // burst's unmodulated preamble lasts only 24 ms)
        if (tone_hits_ >= 2 || 10 * std::log10(at(k) / med) >= TONE_DB + 4) {
            // the carrier: parabolic interpolation of the peak
            const float l = at(k - 1), c = at(k), r = at(k + 1);
            const float den = l - 2 * c + r;
            const double d = den != 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
            f0_ = bin_hz(k + d);
            active_ = true;
            quiet_ = 0;
        }
        return;
    }
    // following a burst: the band around the carrier above the noise?
    if (tone && std::abs(k - tone_bin_) <= 2) tone_seen_ = end;
    const int c = static_cast<int>(std::lround(f0_ * NFFT / FS));
    float narrow = 0, wide = 0;
    const int hn = 30, hw = 250;   // +-1.4 kHz, +-11.7 kHz
    for (int i = -hw; i <= hw; i++) {
        const float v = at(c + i);
        wide += v;
        if (std::abs(i) <= hn) narrow += v;
    }
    const bool present = narrow > 2.5f * med * (2 * hn + 1) / 0.69f || wide > 1.6f * med * (2 * hw + 1) / 0.69f;
    quiet_ = present ? 0 : quiet_ + 1;
    const bool too_long = end - start_ > static_cast<int64_t>((RING_S - 1) * FS);
    if (quiet_ >= 3 || too_long) {
        active_ = false;
        tone_hits_ = 0;
        bursts_++;
        if (debug_) {
            char b[120];
            snprintf(b, sizeof b, "burst at %.3f s, %.3f s long, carrier %+.1f Hz", host_t(static_cast<double>(start_)),
                     static_cast<double>(end - start_) / FS, f0_);
            host.log(b);
        }
        decode(start_ - 512, end, f0_);   // end: 3 hops after the signal left
    }
}

void BurstRx::decode(int64_t s0, int64_t s1, double f) {
    s0 = std::max(s0, ring_base_);
    s1 = std::min(s1, nin_);
    if (s1 - s0 < 1200) return;
    // rates in order of the tone's length (the unmodulated preamble)
    const double tone_s = static_cast<double>(tone_seen_ - start_) / FS;
    std::vector<std::pair<double, double>> order;
    for (const auto& r : RATES) order.push_back({std::fabs(r.unmod / r.rate - tone_s), r.rate});
    std::sort(order.begin(), order.end());
    for (const auto& o : order)
        if (try_rate(o.second, s0, s1, f)) {
            decoded_++;
            return;
        }
}

bool BurstRx::try_rate(double rate, int64_t s0, int64_t s1, double f) {
    const BurstRate* br = nullptr;
    for (const auto& r : RATES)
        if (r.rate == rate) br = &r;
    if (!br) return false;
    const bool qpsk = rate >= 4800;
    const int uwl = qpsk ? 64 : 32;
    DemodCfg c;
    c.mod = qpsk ? Mod::AQPSK : Mod::ABPSK;
    c.rate = rate;
    c.fs = FS;
    c.decim = rate <= 1200 ? 5 : rate <= 2400 ? 2 : 1;
    c.rolloff = qpsk ? 0.6f : 1.0f;
    c.pll_bw = static_cast<float>(std::max(0.002, 10.0 / rate));
    c.tim_gain = 4e-4f;
    c.gear_bits = br->unmod + br->mod + 64 + static_cast<int>(512 / (FS / rate));   // fast loops through the preamble
    std::vector<cf> x(ring_.begin() + static_cast<long>(s0 - ring_base_), ring_.begin() + static_cast<long>(s1 - ring_base_));
    const double sps_in = FS / rate;
    // the demodulator from four starting timing phases; the best unique word wins
    struct Best {
        int err = 1 << 30;
        std::vector<float> s;
        std::vector<double> t;
        int64_t p = 0;
        float sa = 1, sb = 1;
    } best;
    const int pre = br->unmod + br->mod;
    for (int ph = 0; ph < 4; ph++) {
        const int o = static_cast<int>(std::lround(ph * sps_in / 4));
        Demod d;
        d.start(c, f, static_cast<double>(s0 + o));
        std::vector<SoftBit> bits;
        d.process(x.data() + o, x.size() - static_cast<size_t>(o), bits);
        std::vector<float> s;
        std::vector<double> t;
        if (qpsk) {
            for (const auto& b : bits) {
                s.push_back(b.d);
                t.push_back(b.t);
            }
        } else {   // differential detection, + = "0"
            for (size_t i = 1; i < bits.size(); i++) {
                const float m = std::min(std::fabs(bits[i].d), std::fabs(bits[i - 1].d));
                s.push_back(bits[i].d * bits[i - 1].d > 0 ? -m : m);
                t.push_back(bits[i].t);
            }
        }
        const int64_t nb = static_cast<int64_t>(s.size());
        const int64_t hi = std::min<int64_t>(nb - uwl - 320, pre + 320);
        for (int64_t p = 0; p <= hi; p++) {
            int e = 0, ea = 0, eb = 0;
            if (qpsk) {
                for (int k = 0; k < 32; k++) {
                    const int u = static_cast<int>((UW_P >> (31 - k)) & 1);
                    ea += (s[static_cast<size_t>(p + 2 * k)] < 0) != u;
                    eb += (s[static_cast<size_t>(p + 2 * k + 1)] < 0) != u;
                }
                e = std::min(ea, 32 - ea) + std::min(eb, 32 - eb);
            } else {
                for (int k = 0; k < 32; k++) ea += (s[static_cast<size_t>(p + k)] < 0) != static_cast<int>((UW_P >> (31 - k)) & 1);
                e = std::min(ea, 32 - ea);
                eb = ea;
            }
            if (e < best.err) {
                best.err = e;
                best.p = p;
                best.sa = ea <= 16 ? 1.0f : -1.0f;
                best.sb = eb <= 16 ? 1.0f : -1.0f;
                best.s = s;
                best.t = t;
            }
        }
    }
    if (debug_) {
        char b[160];
        snprintf(b, sizeof b, "burst try %.0f: UW errors %d at bit %lld of %zu, carrier %+.1f Hz", rate, best.err,
                 static_cast<long long>(best.p), best.s.size(), f);
        host.log(b);
    }
    if (best.err > (qpsk ? 6 : 3)) return false;
    const std::vector<float>& s = best.s;
    const int64_t q = best.p + uwl;   // first coded bit
    const int64_t avail = static_cast<int64_t>(s.size()) - q;
    auto soft = [&](int64_t j) {
        const float v = s[static_cast<size_t>(q + j)];
        return (j & 1) ? v * best.sb : v * best.sa;
    };
    const double t_uw = host_t(best.t[static_cast<size_t>(best.p)]);   // bit times are input sample indexes
    const double bit_s = 1.0 / rate;
    // decode a list of blocks (columns each), from the start of the field
    auto run = [&](const std::vector<int>& cols, std::vector<uint8_t>& info) {
        Viterbi v;
        v.reset(true);
        int64_t j0 = 0;
        for (int cc : cols) {
            const int nbits = 64 * cc;
            for (int k = 0; k < nbits; k += 2)
                v.push(soft(j0 + deinterleave_index(k, cc)), soft(j0 + deinterleave_index(k + 1, cc)));
            j0 += nbits;
        }
        v.flush();
        info.clear();
        v.take(info);
        return j0;
    };
    auto descramble = [](std::vector<uint8_t>& b, size_t n) {
        Scrambler sc;
        for (size_t i = 0; i < n && i < b.size(); i++) b[i] ^= static_cast<uint8_t>(sc.next());
    };
    auto talker = [&](uint32_t aes, double t_end) {
        host.valid(t_uw, t_end);
        if (!aes) return;
        kp::Talker t;
        t.id = aes_hex(aes);
        const std::string l = su_.label_of(aes);
        t.label = l.empty() ? "aircraft" : l;
        t.start_s = t_uw;
        t.end_s = std::min(t_end, t_uw + 0.24);
        t.packet = true;
        t.freq_hz = static_cast<float>(f);
        t.bw_hz = static_cast<float>(qpsk ? 10000 : 2 * rate + 400);
        t.avg_s = 10;
        t.keep_s = 1800;
        host.talker(t);
    };
    if (avail < 320) return false;
    std::vector<uint8_t> info;
    // R burst: one block of 5 columns: a 19-octet SU + 8 flush bits
    run({5}, info);
    {
        std::vector<uint8_t> b(info.begin(), info.begin() + 152);
        descramble(b, 152);
        uint8_t su[19];
        pack_lsb(b.data(), 19, su);
        if (su_crc_ok(su, 19)) {
            const uint32_t aes = su_.r_su(su, t_uw, t_uw + (uwl + 320) * bit_s);
            if (host.raw_wanted()) {
                std::string r = "R SU";
                for (int i = 0; i < 19; i++) {
                    char h[4];
                    snprintf(h, sizeof h, " %02X", su[i]);
                    r += h;
                }
                host.raw(r);
            }
            talker(aes, t_uw + (uwl + 320) * bit_s);
            return true;
        }
    }
    // T burst: burst identifier (6 octets) + signal units
    std::vector<int> cols;
    if (!qpsk) {
        // as many blocks as the burst held (5 columns, then 3 each)
        const int64_t nblk = 1 + std::max<int64_t>(0, (avail - 320) / 192);
        cols.push_back(5);
        for (int64_t k = 1; k < std::min<int64_t>(nblk, 31); k++) cols.push_back(3);
        run(cols, info);
    } else {
        // one block of 2 + 3n columns: n from the burst's length
        const int64_t nest = std::clamp<int64_t>((avail / 64 - 2) / 3, 2, 31);
        bool ok = false;
        for (int64_t n : {nest, nest - 1, nest + 1, nest - 2}) {
            if (n < 2 || n > 31 || 64 * (2 + 3 * n) > avail) continue;
            run({static_cast<int>(2 + 3 * n)}, info);
            std::vector<uint8_t> b(info);
            descramble(b, b.size() - 16);
            uint8_t id[6];
            pack_lsb(b.data(), 6, id);
            if (!su_crc_ok(id, 6)) continue;
            bool all = true;
            for (int64_t j = 0; j < n && all; j++) {
                uint8_t su[12];
                pack_lsb(&b[static_cast<size_t>(48 + 96 * j)], 12, su);
                all = su_crc_ok(su, 12);
            }
            if (all) {
                ok = true;
                cols = {static_cast<int>(2 + 3 * n)};
                break;
            }
        }
        if (!ok) return false;
    }
    std::vector<uint8_t> b(info);
    descramble(b, b.size());
    uint8_t id[6];
    pack_lsb(b.data(), 6, id);
    if (!su_crc_ok(id, 6)) return false;
    std::vector<std::vector<uint8_t>> sus;
    for (size_t j = 0; 48 + 96 * (j + 1) <= b.size(); j++) {
        std::vector<uint8_t> su(12);
        pack_lsb(&b[48 + 96 * j], 12, su.data());
        if (!su_crc_ok(su.data(), 12)) break;
        sus.push_back(su);
    }
    if (sus.empty()) return false;
    std::vector<const uint8_t*> ptrs;
    for (const auto& v : sus) ptrs.push_back(v.data());
    const double t_end = t_uw + (uwl + 64 * (5 + 3 * static_cast<double>(sus.size() - 1))) * bit_s;
    su_.t_burst(id, ptrs, t_uw, t_end);
    if (host.raw_wanted()) {
        char h[64];
        snprintf(h, sizeof h, "T burst AES %06X GES %02X SUs %zu", (id[0] << 16) | (id[1] << 8) | id[2], id[3], sus.size());
        host.raw(h);
    }
    talker((static_cast<uint32_t>(id[0]) << 16) | (static_cast<uint32_t>(id[1]) << 8) | id[2], t_end);
    return true;
}

}  // namespace aero
