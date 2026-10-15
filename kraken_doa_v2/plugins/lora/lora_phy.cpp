#include "lora_phy.hpp"

#include "coding.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace lora_phy {

static constexpr int DETECT_SYMS = 5;      // stable preamble up-peaks to trigger
static constexpr float DETECT_RATIO = 3.0f;  // peak / mean power threshold
static constexpr int MAX_PAYLOAD = 255;
static constexpr int SCAN_MAX_SYMS = 12;   // symbols to scan for the SFD after preamble

LoraRx::LoraRx(int sf, double bw, int os, uint8_t sync_word, bool implicit, int impl_len, int impl_cr, bool impl_crc,
               bool ldro_auto)
    : sf_(sf), os_(os), impl_len_(impl_len), impl_cr_(impl_cr), bw_(bw), implicit_(implicit), impl_crc_(impl_crc),
      ldro_auto_(ldro_auto), sync_word_(sync_word) {
    N_ = 1 << sf_;
    L_ = os_ * N_;
    rate_ = os_ * bw_;
    down_.resize(L_);
    up_.resize(L_);
    for (int n = 0; n < L_; n++) {
        const double ph = 2.0 * M_PI / os_ * (static_cast<double>(n) * n / (2.0 * L_) - n / 2.0);
        up_[n] = cf(std::cos(ph), std::sin(ph));
        down_[n] = std::conj(up_[n]);
    }
    fin_ = fftwf_alloc_complex(L_);
    fout_ = fftwf_alloc_complex(L_);
    plan_ = fftwf_plan_dft_1d(L_, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
    int rs = 1;
    while (rs < static_cast<int>(rate_ * 1.8) + L_ * 4) rs <<= 1;
    ring_.assign(rs, cf(0, 0));
    ring_mask_ = rs - 1;
    next_eval_ = L_;
}

LoraRx::~LoraRx() {
    fftwf_destroy_plan(plan_);
    fftwf_free(fin_);
    fftwf_free(fout_);
}

void LoraRx::reset() {
    abs_ = 0;
    det_count_ = 0;
    det_bin_ = -1;
    det_start_ = -1;
    next_eval_ = L_;
    pending_ = false;
    std::fill(ring_.begin(), ring_.end(), cf(0, 0));
}

void LoraRx::get_window(int64_t a, cf* w) {
    for (int i = 0; i < L_; i++) w[i] = ring_[static_cast<size_t>((a + i) & ring_mask_)];
}

int LoraRx::demod_bin(int64_t a, bool use_up, float* pk, float* mean, float* frac, double derot_bins) {
    const cf* ref = use_up ? up_.data() : down_.data();
    if (derot_bins != 0) {
        const double w = -2.0 * M_PI * derot_bins / L_;
        for (int i = 0; i < L_; i++) {
            const cf r(static_cast<float>(std::cos(w * i)), static_cast<float>(std::sin(w * i)));
            const cf v = ring_[static_cast<size_t>((a + i) & ring_mask_)] * ref[i] * r;
            fin_[i][0] = v.real();
            fin_[i][1] = v.imag();
        }
    } else {
        for (int i = 0; i < L_; i++) {
            const cf v = ring_[static_cast<size_t>((a + i) & ring_mask_)] * ref[i];
            fin_[i][0] = v.real();
            fin_[i][1] = v.imag();
        }
    }
    fftwf_execute(plan_);
    // alias-combine the os halves: power at value v = sum_k |X[v + k*N]|^2
    float best = -1;
    int bestv = 0;
    double sum = 0;
    for (int v = 0; v < N_; v++) {
        float p = 0;
        for (int k = 0; k < os_; k++) {
            const int b = v + k * N_;
            p += fout_[b][0] * fout_[b][0] + fout_[b][1] * fout_[b][1];
        }
        sum += p;
        if (p > best) { best = p; bestv = v; }
    }
    if (pk) *pk = best;
    if (mean) *mean = static_cast<float>(sum / N_);
    if (frac) {   // parabolic sub-bin around the main alias bin (k=0 half)
        const int b0 = bestv;
        const auto pw = [&](int b) {
            b = (b + N_) % N_;
            float p = 0;
            for (int k = 0; k < os_; k++) p += fout_[b + k * N_][0] * fout_[b + k * N_][0] + fout_[b + k * N_][1] * fout_[b + k * N_][1];
            return p;
        };
        const float ym = pw(b0 - 1), y0 = pw(b0), yp = pw(b0 + 1);
        const float den = ym - 2 * y0 + yp;
        *frac = den != 0 ? 0.5f * (ym - yp) / den : 0.f;
    }
    return bestv;
}

void LoraRx::push(const cf* x, size_t n, std::vector<Frame>& out) {
    for (size_t i = 0; i < n; i++) ring_[static_cast<size_t>((abs_ + i) & ring_mask_)] = x[i];
    abs_ += static_cast<int64_t>(n);

    // a packet is being waited on: retry it as samples arrive
    if (pending_) {
        const int r = decode_from(pend_start_, out);
        if (r == 1 || r == 0 || abs_ > pend_deadline_) pending_ = false;
        if (pending_) return;   // keep waiting; no new detection meanwhile
    }

    // dechirp one symbol per hop, looking for a stable preamble up-peak
    while (next_eval_ + L_ <= abs_) {
        float pk, mean;
        const int v = demod_bin(next_eval_, false, &pk, &mean, nullptr);
        const bool strong = pk > DETECT_RATIO * mean && mean > 0;
        if (strong && (det_bin_ < 0 || std::abs(((v - det_bin_ + N_ + N_ / 2) % N_) - N_ / 2) <= 1)) {
            if (det_count_ == 0) det_start_ = next_eval_;
            det_bin_ = v;
            det_count_++;
            if (det_count_ >= DETECT_SYMS) {
                const int64_t start = det_start_;
                det_count_ = 0;
                det_bin_ = -1;
                const int r = decode_from(start, out);
                if (r == 2) {   // a real packet, waiting for its tail
                    pending_ = true;
                    pend_start_ = start;
                    // worst case: 8 header + 255-byte payload at this SF/CR
                    pend_deadline_ = start + static_cast<int64_t>(2060) * L_;
                    next_eval_ += L_;
                    break;
                }
                if (r == 1) next_eval_ = start + L_;   // past the decoded packet
            }
        } else {
            det_count_ = 0;
            det_bin_ = -1;
        }
        next_eval_ += L_;
    }
}

// Locate the SFD, split CFO/STO, decode header (explicit) and payload.
int LoraRx::decode_from(int64_t preamble_start, std::vector<Frame>& out) {
    // median up-peak bin over a few preamble symbols from preamble_start
    const int64_t oldest = abs_ - (ring_mask_ + 1) + L_;
    if (preamble_start < oldest) return 0;

    auto cmod = [&](int d) { return ((d % N_) + N_) % N_; };

    // --- sample-accurate symbol alignment ---
    // The coarse detector hops by L, so preamble_start straddles the real
    // symbol boundary by some samples delta. For a value-0 preamble the
    // dechirp peak bin b0 encodes that offset: delta ~= b0 * os samples.
    // Shift the start by that, then fine-search +-os samples for the true
    // boundary (max preamble peak).
    if (preamble_start + 2LL * L_ > abs_) return 2;
    const int b0 = demod_bin(preamble_start, false, nullptr, nullptr, nullptr);
    // the window straddles the true boundary by b0*os samples: shift back
    int64_t astart = preamble_start - static_cast<int64_t>(b0) * os_;
    while (astart < oldest) astart += L_;
    if (astart + 2LL * L_ > abs_) return 2;
    {
        float best = -1;
        int64_t bestd = 0;
        for (int d = -2 * os_; d <= 2 * os_; d++) {
            const int64_t a = astart + d;
            if (a < oldest || a + L_ > abs_) continue;
            float pk, mean;
            demod_bin(a, false, &pk, &mean, nullptr);
            if (pk > best) { best = pk; bestd = d; }
        }
        astart += bestd;
    }

    // scan forward from the aligned preamble for the SFD (2 down-chirps):
    // the symbol where the up-ref (down-chirp) peak exceeds the down-ref one.
    float bup_f = 0, bdown_f = 0;
    int b_up = demod_bin(astart, false, nullptr, nullptr, &bup_f);
    int64_t sfd = -1;
    int b_down = 0;
    for (int s = 0; s < SCAN_MAX_SYMS; s++) {
        const int64_t a = astart + static_cast<int64_t>(s) * L_;
        if (a + 2 * L_ > abs_) return 2;
        float pu, pd;
        const int vu = demod_bin(a, false, &pu, nullptr, nullptr);
        const int vd = demod_bin(a, true, &pd, nullptr, &bdown_f);
        if (pd > 1.8f * pu) { sfd = a; b_down = vd; break; }
        (void)vu;
    }
    if (sfd < 0) return 0;
    // walk back to the FIRST down-chirp of the SFD (detection may latch the
    // second): the previous symbol is still down-dominant.
    for (int back = 0; back < 2; back++) {
        const int64_t a = sfd - L_;
        if (a < oldest) break;
        float pu, pd;
        demod_bin(a, false, &pu, nullptr, nullptr);
        const int vd = demod_bin(a, true, &pd, nullptr, &bdown_f);
        if (pd > 1.3f * pu) { sfd = a; b_down = vd; } else break;
    }

    // The b0-based alignment shifts the start by -(cfo)*os samples, which
    // exactly cancels the carrier offset in the dechirped data symbols (the
    // timing error and the frequency offset are equal and opposite in CSS) -
    // so the data needs NO further bin subtraction. b_up ~= 0 and b_down ~= 2*cfo.
    const int64_t data_start = sfd + static_cast<int64_t>(std::llround(2.25 * L_));
    // report the carrier offset: half the SFD down-peak (relative to b_up),
    // wrapped to +-N/2, plus the sub-bin part.
    int d2 = cmod(b_down - b_up);
    double cfo_val = 0.5 * (d2 > N_ / 2 ? d2 - N_ : d2) + 0.5 * (bdown_f - bup_f);
    const double cfo_hz = cfo_val / N_ * bw_;

    const bool ldro = ldro_auto_ && ((double)N_ / bw_ >= 0.016);   // symbol time >= 16 ms
    // residual value offset left by the integer b0 alignment (b_up measured
    // at the aligned preamble). Remove the integer part per symbol and
    // de-rotate the fractional CFO (sub-bin) that would otherwise flip the
    // low bits of full-rate payload symbols.
    const int resid = cmod(b_up);
    const double cfo_fr = bup_f;   // fractional bin offset measured at the preamble
    auto demod_value = [&](int64_t a) { return cmod(demod_bin(a, false, nullptr, nullptr, nullptr, cfo_fr) - resid); };

    // ---- header block: 8 symbols at reduced rate (sf_app = sf-2), CR 4/8 ----
    const int sf_app_hdr = sf_ - 2;
    Frame fr;
    fr.sf = sf_;
    fr.start_sample = astart;
    fr.cfo_hz = cfo_hz;
    fr.sync_word = sync_word_;

    int length = impl_len_, cr = impl_cr_;
    bool has_crc = impl_crc_;
    int data_sym0 = 0;   // index (in symbols from data_start) where payload nibbles begin
    std::vector<uint8_t> nibbles;

    auto block_nibbles = [&](int64_t base, int nsym, int sf_app, int blk_cr, bool reduced, std::vector<uint8_t>& nb,
                             bool* all_ok) {
        std::vector<uint16_t> sym(static_cast<size_t>(nsym));
        for (int i = 0; i < nsym; i++) {
            int v = demod_value(base + static_cast<int64_t>(i) * L_);
            if (reduced) v = (v >> 2) & ((1 << sf_app) - 1);   // drop 2 LSBs (reduced rate / LDRO)
            sym[static_cast<size_t>(i)] = static_cast<uint16_t>(lora_code::gray_decode(static_cast<uint32_t>(v)));
        }
        std::vector<uint8_t> cw(static_cast<size_t>(sf_app));
        lora_code::deinterleave(sym.data(), blk_cr, sf_app, cw.data());
        bool ok = true;
        for (int j = 0; j < sf_app; j++) {
            bool cok = false;
            nb.push_back(lora_code::hamming_decode(cw[static_cast<size_t>(j)], blk_cr, &cok));
            ok = ok && cok;
        }
        if (all_ok) *all_ok = ok;
    };

    if (!implicit_) {
        if (data_start + 8LL * L_ > abs_) return 2;
        std::vector<uint8_t> hn;
        bool hok = false;
        block_nibbles(data_start, 8, sf_app_hdr, 4, true, hn, &hok);
        // header = first 5 nibbles
        if (hn.size() < 5) return 0;
        const int plen = (hn[0] << 4) | hn[1];
        const int hcr = (hn[2] >> 1) & 0x7;
        const bool hcrc = hn[2] & 1;
        const uint8_t chk = static_cast<uint8_t>(((hn[3] & 1) << 4) | hn[4]);   // c4 in n3 LSB, c3..c0 in n4? see gen
        const uint8_t want = lora_code::header_checksum(hn[0], hn[1], hn[2]);
        fr.header_ok = (chk == want) && hcr >= 1 && hcr <= 4 && plen <= MAX_PAYLOAD;
        if (!fr.header_ok) return 0;   // not a real header - drop the candidate
        length = plen;
        cr = hcr;
        has_crc = hcrc;
        // the header block also carries sf_app_hdr nibbles; the first 5 are the
        // header, the remaining (sf_app_hdr - 5) are the first payload nibbles.
        for (size_t k = 5; k < hn.size(); k++) nibbles.push_back(hn[k]);
        data_sym0 = 8;
    } else {
        fr.header_ok = true;
    }
    fr.cr = cr;
    fr.has_crc = has_crc;
    fr.implicit = implicit_;

    // total nibbles needed = payload length + CRC (2 bytes) in bytes*2
    const int need_bytes = length + (has_crc ? 2 : 0);
    const int need_nibbles = need_bytes * 2;
    const int sf_app = ldro ? sf_ - 2 : sf_;

    // decode subsequent blocks until we have enough nibbles
    int64_t base = data_start + static_cast<int64_t>(data_sym0) * L_;
    int total_syms = data_sym0;
    while (static_cast<int>(nibbles.size()) < need_nibbles) {
        const int nsym = 4 + cr;
        if (base + static_cast<int64_t>(nsym) * L_ > abs_) return 2;   // wait for more samples
        std::vector<uint8_t> nb;
        block_nibbles(base, nsym, sf_app, cr, ldro, nb, nullptr);
        for (uint8_t x : nb) nibbles.push_back(x);
        base += static_cast<int64_t>(nsym) * L_;
        total_syms += nsym;
        if (total_syms > 2000) return 0;   // runaway guard
    }
    fr.nsym = total_syms;
    fr.end_sample = base;

    // nibbles -> bytes (low nibble first), de-whiten the payload (not header)
    std::vector<uint8_t> bytes(static_cast<size_t>(need_bytes), 0);
    for (int i = 0; i < need_bytes; i++) {
        const uint8_t lo = nibbles[static_cast<size_t>(2 * i)] & 0xF;
        const uint8_t hi = nibbles[static_cast<size_t>(2 * i + 1)] & 0xF;
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(lo | (hi << 4));
    }
    lora_code::Whitening wh;
    for (int i = 0; i < length; i++) bytes[static_cast<size_t>(i)] ^= wh.next();
    // CRC bytes are whitened too in the standard chain
    if (has_crc) {
        for (int i = length; i < need_bytes; i++) bytes[static_cast<size_t>(i)] ^= wh.next();
    }

    fr.payload.assign(bytes.begin(), bytes.begin() + length);
    if (has_crc && need_bytes >= length + 2) {
        const uint16_t rx = static_cast<uint16_t>(bytes[static_cast<size_t>(length)] | (bytes[static_cast<size_t>(length + 1)] << 8));
        const uint16_t calc = lora_code::crc16(fr.payload.data(), static_cast<size_t>(length));
        fr.crc_ok = (rx == calc);
    } else {
        fr.crc_ok = !has_crc;   // no CRC: accept on header + Hamming
    }
    // SNR estimate from the preamble peak/mean
    float pk, mean;
    demod_bin(astart, false, &pk, &mean, nullptr);
    fr.snr_db = mean > 0 ? 10.0f * std::log10(std::max(pk / (mean * N_), 1e-6f)) : 0.f;

    if (!fr.crc_ok && has_crc) return 0;   // only emit frames that passed the CRC
    out.push_back(std::move(fr));
    return 1;
}

}  // namespace lora_phy
