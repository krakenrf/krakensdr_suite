#include "lora_phy.hpp"

#include "coding.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace lora_phy {

// Detection (the cost of every watched SF): one symbol window every
// DET_HOP symbols at ONE sample per chip (every other sample: an N-point
// FFT), DETECT_SYMS of them on the same bin (+-1) = a preamble candidate.
// A LoRaWAN preamble is 8 symbols (+ 4.25 sync / SFD): 3 windows 2 symbols
// apart always fit. The candidate is then aligned at os=2 by decode_from.
static constexpr int DET_HOP = 2;
static constexpr int DETECT_SYMS = 3;      // stable preamble up-peaks to trigger
// peak / mean power threshold: N bins of noise put their maximum above 3x
// the mean most of the time at small N - SF5 / SF6 need more
static float detect_ratio(int N) { return N >= 128 ? 3.0f : N >= 64 ? 3.5f : 4.0f; }
static constexpr int MAX_PAYLOAD = 255;
static constexpr int SCAN_MAX_SYMS = 40;   // symbols to scan for the SFD (Meshtastic sends a 16-symbol preamble)

static constexpr double TRACK_ALPHA = 0.2;  // fractional-offset tracking gain per data symbol

LoraRx::LoraRx(int sf, double bw, int os, Ring* ring, bool invert, bool implicit, int impl_len, int impl_cr,
               bool impl_crc, bool ldro_auto)
    : sf_(sf), os_(os), impl_len_(impl_len), impl_cr_(impl_cr), bw_(bw), invert_(invert), implicit_(implicit),
      impl_crc_(impl_crc), ldro_auto_(ldro_auto), ring_(ring) {
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
    down1_.resize(N_);
    for (int n = 0; n < N_; n++) {
        const double ph = 2.0 * M_PI * (static_cast<double>(n) * n / (2.0 * N_) - n / 2.0);
        down1_[n] = cf(std::cos(ph), -std::sin(ph));
    }
    fin1_ = fftwf_alloc_complex(N_);
    fout1_ = fftwf_alloc_complex(N_);
    plan1_ = fftwf_plan_dft_1d(N_, fin1_, fout1_, FFTW_FORWARD, FFTW_ESTIMATE);
    next_eval_ = ring_->abs + L_;
}

int LoraRx::detect_bin(int64_t a, float* pk, float* mean) {
    const cf* rb = ring_->buf;
    const int64_t mask = ring_->mask;
    const float qs = invert_ ? -1.f : 1.f;
    for (int i = 0; i < N_; i++) {
        const cf x = rb[static_cast<size_t>((a + static_cast<int64_t>(os_) * i) & mask)];
        const cf v = cf(x.real(), qs * x.imag()) * down1_[i];
        fin1_[i][0] = v.real();
        fin1_[i][1] = v.imag();
    }
    fftwf_execute(plan1_);
    float best = -1;
    int bestv = 0;
    double sum = 0;
    for (int v = 0; v < N_; v++) {
        const float q = fout1_[v][0] * fout1_[v][0] + fout1_[v][1] * fout1_[v][1];
        sum += q;
        if (q > best) { best = q; bestv = v; }
    }
    *pk = best;
    *mean = static_cast<float>(sum / N_);
    return bestv;
}

LoraRx::~LoraRx() {
    fftwf_destroy_plan(plan_);
    fftwf_free(fin_);
    fftwf_free(fout_);
    fftwf_destroy_plan(plan1_);
    fftwf_free(fin1_);
    fftwf_free(fout1_);
}

void LoraRx::reset() {
    pk_.active = false;
    det_count_ = 0;
    det_bin_ = -1;
    det_start_ = -1;
    next_eval_ = ring_->abs + L_;
    pending_ = false;
}

int LoraRx::demod_bin(int64_t a, bool use_up, float* pk, float* mean, float* frac, double derot_bins) {
    const cf* ref = use_up ? up_.data() : down_.data();
    const cf* rb = ring_->buf;
    const int64_t mask = ring_->mask;
    // inverted IQ: the packet is conjugated - read conj(x)
    const float qs = invert_ ? -1.f : 1.f;
    if (derot_bins != 0) {
        const std::complex<double> st = std::polar(1.0, -2.0 * M_PI * derot_bins / L_);
        std::complex<double> ph(1, 0);
        for (int i = 0; i < L_; i++) {
            const cf x = rb[static_cast<size_t>((a + i) & mask)];
            const cf v = cf(x.real(), qs * x.imag()) * ref[i] * cf(static_cast<float>(ph.real()), static_cast<float>(ph.imag()));
            ph *= st;
            fin_[i][0] = v.real();
            fin_[i][1] = v.imag();
        }
    } else {
        for (int i = 0; i < L_; i++) {
            const cf x = rb[static_cast<size_t>((a + i) & mask)];
            const cf v = cf(x.real(), qs * x.imag()) * ref[i];
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

void LoraRx::poll(std::vector<Frame>& out) {
    const int64_t abs_ = ring_->abs;

    // a packet whose payload is being demodulated block by block
    if (pk_.active) {
        if (abs_ < need_abs_) return;
        const int r = continue_packet(out);
        if (r == 2) return;
        pending_ = false;
    }
    // a packet's sync / header is being waited on: retry it as samples arrive
    if (pending_) {
        if (abs_ < need_abs_ && abs_ <= pend_deadline_) return;   // not enough samples yet
        const int r = decode_from(pend_start_, out);
        if (r == 2 && pk_.active) { pending_ = false; return; }   // header decoded: the payload follows block by block
        if (r == 1 || r == 0 || abs_ > pend_deadline_) pending_ = false;
        if (pending_) return;   // keep waiting; no new detection meanwhile
    }

    // after a packet the detection resumes where the ring still holds
    // samples (it holds the sync / header stage only: catching up over
    // overwritten indices read newer samples at the wrong times - a later
    // preamble was then "found" outside the ring and lost)
    {
        const int64_t first_ok = abs_ - (ring_->mask + 1) + 4LL * L_;
        if (next_eval_ < first_ok) {   // whole hops: the grid's phase stays (the decodes depend on it)
            const int64_t hop = static_cast<int64_t>(DET_HOP) * L_;
            next_eval_ += (first_ok - next_eval_ + hop - 1) / hop * hop;
            det_count_ = 0;
            det_bin_ = -1;
        }
    }
    // dechirp one symbol per hop, looking for a stable preamble up-peak
    while (next_eval_ + L_ <= abs_) {
        float pk, mean;
        const int v = detect_bin(next_eval_, &pk, &mean);
        const bool strong = pk > detect_ratio(N_) * mean && mean > 0;
        if (strong && (det_bin_ < 0 || std::abs(((v - det_bin_ + N_ + N_ / 2) % N_) - N_ / 2) <= 1)) {
            if (det_count_ == 0) det_start_ = next_eval_;
            det_bin_ = v;
            det_count_++;
            if (det_count_ >= (sf_ <= 6 ? DETECT_SYMS + 1 : DETECT_SYMS)) {   // SF5 / 6: 32 / 64 bins match by chance more often
                const int64_t start = det_start_;
                det_count_ = 0;
                det_bin_ = -1;
                triggers_++;
                const int r = decode_from(start, out);
                if (r == 2 && pk_.active) {   // header decoded: the payload follows block by block
                    next_eval_ += static_cast<int64_t>(DET_HOP) * L_;
                    break;
                }
                if (r == 2) {   // a real packet, waiting for its tail
                    pending_ = true;
                    pend_start_ = start;
                    // worst case: 8 header + 255-byte payload at this SF/CR
                    pend_deadline_ = start + static_cast<int64_t>(2060) * L_;
                    next_eval_ += static_cast<int64_t>(DET_HOP) * L_;
                    break;
                }
                if (r == 1) next_eval_ = std::max(next_eval_, out.back().end_sample - static_cast<int64_t>(DET_HOP) * L_);   // past the decoded packet
            }
        } else {
            det_count_ = 0;
            det_bin_ = -1;
        }
        next_eval_ += static_cast<int64_t>(DET_HOP) * L_;
    }
}

// Locate the SFD, split CFO/STO, decode header (explicit) and payload.
int LoraRx::decode_from(int64_t preamble_start, std::vector<Frame>& out) {
    const int64_t abs_ = ring_->abs;
    const int64_t oldest = abs_ - (ring_->mask + 1) + L_;
    if (preamble_start < oldest) return 0;

    auto cmod = [&](int d) { return ((d % N_) + N_) % N_; };

    // --- sample-accurate symbol alignment ---
    // The coarse detector hops by L, so preamble_start straddles the real
    // symbol boundary by some samples delta. For a value-0 preamble the
    // dechirp peak bin b0 encodes that offset: delta ~= b0 * os samples.
    // Shift the start by that, then fine-search +-os samples for the true
    // boundary (max preamble peak).
    if (preamble_start + 2LL * L_ > abs_) { need_abs_ = preamble_start + 2LL * L_; return 2; }
    const int b0 = demod_bin(preamble_start, false, nullptr, nullptr, nullptr);
    int64_t astart = preamble_start - static_cast<int64_t>(b0) * os_;
    while (astart < oldest) astart += L_;
    if (astart + 2LL * L_ > abs_) { need_abs_ = astart + 2LL * L_; return 2; }
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
    int64_t sfd = -1;
    int b_down = 0;
    for (int s = 0; s < SCAN_MAX_SYMS; s++) {
        const int64_t a = astart + static_cast<int64_t>(s) * L_;
        if (a + 2 * L_ > abs_) { need_abs_ = a + 2LL * L_; return 2; }
        float pu, pd;
        demod_bin(a, false, &pu, nullptr, nullptr);
        const int vd = demod_bin(a, true, &pd, nullptr, &bdown_f);
        if (pd > 1.8f * pu) { sfd = a; b_down = vd; break; }
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
    // The preamble residual is measured on the LAST preamble symbol (3
    // symbols before the SFD: the 2 sync symbols are in between) - astart may
    // be one symbol before the preamble when the first stable hop fell in its
    // first symbol, so b_up at astart could be noise.
    if (sfd - 3 * L_ < oldest) return 0;
    const int b_up = demod_bin(sfd - 3 * L_, false, nullptr, nullptr, &bup_f);

    // The b0-based alignment shifts the start by -(cfo)*os samples, which
    // exactly cancels the carrier offset in the dechirped data symbols (the
    // timing error and the frequency offset are equal and opposite in CSS) -
    // so the data needs NO further bin subtraction. b_up ~= 0 and b_down ~= 2*cfo.
    const int64_t data_start = sfd + static_cast<int64_t>(std::llround(2.25 * L_));
    // report the carrier offset: half the SFD down-peak (relative to b_up),
    // wrapped to +-N/2, plus the sub-bin part (inverted IQ: mirrored).
    int d2 = cmod(b_down - b_up);
    double cfo_val = 0.5 * (d2 > N_ / 2 ? d2 - N_ : d2) + 0.5 * (bdown_f - bup_f);
    const double cfo_hz = (invert_ ? -1.0 : 1.0) * cfo_val / N_ * bw_;

    const bool ldro = ldro_auto_ && ((double)N_ / bw_ >= 0.016);   // symbol time >= 16 ms
    // Values relative to the preamble's bin (resid); the radios send data
    // symbols one bin up (value + 1) and reduced-rate symbols as 4 value + 1.
    // The fractional offset (measured on the preamble, then TRACKED per
    // symbol: the sample clock drifts against the transmitter's - ~0.4 bin
    // over a 0.5 s SF10 packet at 2 ppm, enough to flip the last symbols of
    // a long one) is de-rotated before each FFT.
    const int resid = cmod(b_up);
    double track = bup_f;
    std::vector<SymRef> hdr_refs;   // the header block's symbols (separation by code)
    auto demod_sym = [&](int64_t a, bool reduced) {
        float fr = 0;
        const double tr0 = track;
        const int b = demod_bin(a, false, nullptr, nullptr, &fr, track);
        track += TRACK_ALPHA * fr;
        if (desp) hdr_refs.push_back({a, b, tr0});
        const int v = reduced ? (cmod(b - resid - 1 + 2) >> 2) : cmod(b - resid - 1);
        return static_cast<uint16_t>(lora_code::gray_map(static_cast<uint32_t>(v)));
    };

    // sync word: the two symbols before the SFD carry its nibbles x 8
    int sync_word = -1;
    const int sb1 = demod_bin(sfd - 2 * L_, false, nullptr, nullptr, nullptr, bup_f);
    const int sb2 = demod_bin(sfd - L_, false, nullptr, nullptr, nullptr, bup_f);
    {
        const int s1 = cmod(sb1 - resid);
        const int s2 = cmod(sb2 - resid);
        const int n1 = (s1 + 4) / 8, n2 = (s2 + 4) / 8;
        if (std::abs(s1 - 8 * n1) <= 1 && std::abs(s2 - 8 * n2) <= 1 && n1 < 16 && n2 < 16) sync_word = n1 << 4 | n2;
    }

    // separation by code: the preamble's last up-chirps (value 0 - checked:
    // at the preamble's bin, a clear peak) and the two sync symbols
    auto collect_pre = [&](std::vector<SymRef>& v) {
        v.clear();
        for (int k = DESPREAD_PRE - 1; k >= 0; k--) {
            const int64_t a = sfd - 3LL * L_ - static_cast<int64_t>(k) * L_;
            if (a < astart || a < oldest) continue;
            float pk = 0, mean = 0;
            const int b = demod_bin(a, false, &pk, &mean, nullptr, bup_f);
            const int d = std::abs(((b - resid + N_ + N_ / 2) % N_) - N_ / 2);
            if (d <= 1 && pk > 3.f * mean) v.push_back({a, b, bup_f});
        }
        v.push_back({sfd - 2LL * L_, sb1, bup_f});
        v.push_back({sfd - L_, sb2, bup_f});
    };

    // ---- header block: 8 symbols at CR 4/8, reduced rate (sf_app = sf-2) ----
    // SF5 / SF6 (SX126x): sf - 2 bits can't carry the 5 header nibbles in a
    // block, so the header block is read at the FULL rate, with or without
    // two extra symbols after the SFD (both tried - the header checksum and
    // the 8 codewords decide; not verified on a real SX126x here).
    const bool low_sf = sf_ < 7;
    const int sf_app_hdr = low_sf ? sf_ : sf_ - 2;
    const bool hdr_reduced = !low_sf;
    Frame fr;
    fr.sf = sf_;
    fr.start_sample = sfd - 10 * L_;   // >= 8 preamble + 2 sync symbols before the SFD
    fr.cfo_hz = cfo_hz;
    fr.sync_word = sync_word;
    fr.inverted = invert_;

    int length = impl_len_, cr = impl_cr_;
    bool has_crc = impl_crc_;
    std::vector<uint8_t> nibbles, nib_ok;
    int fec_bad = 0, fec_total = 0;

    auto block_nibbles = [&](int64_t base, int nsym, int sf_app, int blk_cr, bool reduced, std::vector<uint8_t>& nb,
                             int* bad, std::vector<uint8_t>* okv) {
        uint16_t sym[8];
        for (int i = 0; i < nsym; i++) sym[i] = demod_sym(base + static_cast<int64_t>(i) * L_, reduced);
        uint8_t cw[12];
        lora_code::deinterleave(sym, blk_cr, sf_app, cw);
        for (int j = 0; j < sf_app; j++) {
            bool cok = false;
            nb.push_back(lora_code::hamming_decode(cw[j], blk_cr, &cok));
            if (okv) okv->push_back(cok ? 1 : 0);
            if (!cok && bad) (*bad)++;
        }
    };
    auto log_detect = [&](bool hok) {
        if (!det_log) return;
        Detect d;
        d.sfd = sfd;
        d.sync_word = sync_word;
        d.cfo_hz = cfo_hz;
        float pk = 0, mean = 0;
        demod_bin(sfd - 3 * L_, false, &pk, &mean, nullptr);
        d.snr_db = mean > 0 ? 10.0f * std::log10(std::max(pk / (mean * N_), 1e-6f)) : 0.f;
        d.header_ok = hok;
        d.sf = sf_;
        d.inverted = invert_;
        if (desp) {
            std::vector<SymRef> pre;
            collect_pre(pre);
            for (const SymRef& r : pre) despread_into(r, d.sym_y, &d.ant);
        }
        det_log->push_back(d);
    };

    const int64_t hdr_wait = data_start + (low_sf ? 10LL : 8LL) * L_;
    if (hdr_wait > abs_) { need_abs_ = hdr_wait; return 2; }
    int64_t hdr_start = data_start;
    const double track0 = track;
    if (!implicit_) {
        bool found = false;
        for (int h = 0; h < (low_sf ? 2 : 1) && !found; h++) {
            const int64_t hs = data_start + (low_sf && h == 0 ? 2LL * L_ : 0);
            track = track0;
            hdr_refs.clear();
            std::vector<uint8_t> hn;
            int hbad = 0;
            block_nibbles(hs, 8, sf_app_hdr, 4, hdr_reduced, hn, &hbad, nullptr);
            if (hn.size() < 5) continue;
            const int plen = (hn[0] << 4) | hn[1];
            const int hcr = (hn[2] >> 1) & 0x7;
            const bool hcrc = hn[2] & 1;
            const uint8_t chk = static_cast<uint8_t>(((hn[3] & 1) << 4) | hn[4]);
            const uint8_t want = lora_code::header_checksum(hn[0], hn[1], hn[2]);
            // a real header: the checksum AND every header codeword valid (CR
            // 4/8, single errors corrected) - noise passes both ~1 in 10^7
            if (!((chk == want) && hbad == 0 && hcr >= 1 && hcr <= 4 && plen >= 1 && plen <= MAX_PAYLOAD)) continue;
            found = true;
            hdr_start = hs;
            length = plen;
            cr = hcr;
            has_crc = hcrc;
            // the header block also carries sf_app_hdr nibbles; the first 5 are the
            // header, the remaining (sf_app_hdr - 5) are the first payload nibbles.
            for (size_t k = 5; k < hn.size(); k++) {
                nibbles.push_back(hn[k]);
                nib_ok.push_back(1);
            }
        }
        fr.header_ok = found;
        log_detect(found);
        if (!found) return 0;   // not a real header - drop the candidate
    } else {
        // implicit header: the first block is still 8 symbols at CR 4/8
        if (low_sf) hdr_start = data_start + 2LL * L_;
        block_nibbles(hdr_start, 8, sf_app_hdr, 4, hdr_reduced, nibbles, &fec_bad, &nib_ok);
        fec_total += sf_app_hdr;
        fr.header_ok = true;
        log_detect(true);
    }
    fr.cr = cr;
    fr.has_crc = has_crc;
    fr.implicit = implicit_;

    // SNR estimate from the preamble peak/mean (the preamble is still in the ring)
    {
        float pk = 0, mean = 0;
        demod_bin(sfd - 3 * L_, false, &pk, &mean, nullptr);
        fr.snr_db = mean > 0 ? 10.0f * std::log10(std::max(pk / (mean * N_), 1e-6f)) : 0.f;
    }
    // the payload is demodulated INCREMENTALLY from here (continue_packet):
    // the ring only has to hold the sync / header stage (<= ~56 symbols), not
    // the whole packet - a 255-byte SF12 packet is seconds of samples
    pk_ = Pk();
    pk_.active = true;
    pk_.fr = std::move(fr);
    pk_.base = hdr_start + 8LL * L_;
    pk_.total_syms = 8;
    pk_.nib = std::move(nibbles);
    pk_.ok = std::move(nib_ok);
    pk_.fec_bad = fec_bad;
    pk_.fec_total = fec_total;
    pk_.track = track;
    pk_.resid = resid;
    pk_.ldro = ldro;
    pk_.cr = cr;
    pk_.length = length;
    pk_.has_crc = has_crc;
    pk_.low_sf = low_sf;
    pk_.sf_app = ldro ? sf_ - 2 : sf_;
    pk_.need_nibbles = 2 * (length + (has_crc ? 2 : 0));
    if (desp && desp->antennas() >= 2) {
        // separation by code: every stride-th symbol of the packet (its
        // de-spreading held to DESPREAD_BUDGET_S of airtime), starting with the
        // preamble / sync / header symbols that are still in the input kept
        std::vector<SymRef> pre;
        collect_pre(pre);
        const int rem = std::max(0, pk_.need_nibbles - static_cast<int>(pk_.nib.size()));
        const int payload_syms = (rem + pk_.sf_app - 1) / pk_.sf_app * (4 + cr);
        const int total = static_cast<int>(pre.size() + hdr_refs.size()) + payload_syms;
        const double sym_s = static_cast<double>(N_) / bw_;
        pk_.stride = std::max(1, std::min(static_cast<int>(std::ceil(total * sym_s / DESPREAD_BUDGET_S)),
                                          total / DESPREAD_MIN_SYMS));
        pk_.sym_total = total;
        int j = 0;
        for (const SymRef& r : pre)
            if (j++ % pk_.stride == 0) despread_into(r, pk_.ys, &pk_.ym);
        for (const SymRef& r : hdr_refs)
            if (j++ % pk_.stride == 0) despread_into(r, pk_.ys, &pk_.ym);
        pk_.sym_i = j;
    }
    return continue_packet(out);
}

void LoraRx::despread_into(const SymRef& r, std::vector<cf>& ys, int* ym) {
    if (!desp) return;
    cf y[8];
    const int m = desp->despread(*this, r.a, r.bin, r.frac, y);
    if (m < 2 || m > 8) return;
    if (*ym == 0) *ym = m;
    if (m != *ym) return;
    ys.insert(ys.end(), y, y + m);
}

// 1 = emitted, 0 = dropped, 2 = waiting for more samples
int LoraRx::continue_packet(std::vector<Frame>& out) {
    const int64_t abs_ = ring_->abs;
    const int64_t oldest = abs_ - (ring_->mask + 1);
    auto cmod = [&](int d) { return ((d % N_) + N_) % N_; };
    while (static_cast<int>(pk_.nib.size()) < pk_.need_nibbles) {
        const int nsym = 4 + pk_.cr;
        if (pk_.base + static_cast<int64_t>(nsym) * L_ > abs_) {   // the next block isn't complete yet
            need_abs_ = pk_.base + static_cast<int64_t>(nsym) * L_;
            return 2;
        }
        if (pk_.base < oldest) { pk_.active = false; return 0; }   // overwritten (the host stalled)
        uint16_t sym[8];
        for (int i = 0; i < nsym; i++) {
            float frac = 0;
            const int64_t a = pk_.base + static_cast<int64_t>(i) * L_;
            const double tr0 = pk_.track;
            const int bb = demod_bin(a, false, nullptr, nullptr, &frac, pk_.track);
            pk_.track += TRACK_ALPHA * frac;
            if (desp && pk_.stride > 0 && pk_.sym_i++ % pk_.stride == 0) despread_into({a, bb, tr0}, pk_.ys, &pk_.ym);
            const int v = pk_.ldro ? (cmod(bb - pk_.resid - 1 + 2) >> 2) : cmod(bb - pk_.resid - 1);
            sym[i] = static_cast<uint16_t>(lora_code::gray_map(static_cast<uint32_t>(v)));
        }
        uint8_t cw[12];
        lora_code::deinterleave(sym, pk_.cr, pk_.sf_app, cw);
        for (int j = 0; j < pk_.sf_app; j++) {
            bool cok = false;
            pk_.nib.push_back(lora_code::hamming_decode(cw[j], pk_.cr, &cok));
            pk_.ok.push_back(cok ? 1 : 0);
            if (!cok) pk_.fec_bad++;
        }
        pk_.fec_total += pk_.sf_app;
        pk_.base += static_cast<int64_t>(nsym) * L_;
        pk_.total_syms += nsym;
        if (pk_.total_syms > 2000) { pk_.active = false; return 0; }   // runaway guard
    }
    pk_.active = false;
    Frame fr = std::move(pk_.fr);
    const int length = pk_.length;
    const bool has_crc = pk_.has_crc;
    const int need_bytes = length + (has_crc ? 2 : 0);
    fr.nsym = pk_.total_syms;
    fr.end_sample = pk_.base;
    fr.ant = pk_.ym;
    fr.sym_y = std::move(pk_.ys);
    fr.sym_total = pk_.sym_total;
    fr.fec_bad = pk_.fec_bad;
    fr.fec_total = pk_.fec_total;

    // nibbles -> bytes (low nibble first), de-whiten the payload (not the CRC)
    std::vector<uint8_t> bytes(static_cast<size_t>(need_bytes), 0);
    for (int i = 0; i < need_bytes; i++) {
        const uint8_t lo = pk_.nib[static_cast<size_t>(2 * i)] & 0xF;
        const uint8_t hi = pk_.nib[static_cast<size_t>(2 * i + 1)] & 0xF;
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(lo | (hi << 4));
    }
    lora_code::Whitening wh;
    for (int i = 0; i < length; i++) bytes[static_cast<size_t>(i)] ^= wh.next();

    fr.payload.assign(bytes.begin(), bytes.begin() + length);
    fr.byte_ok.assign(static_cast<size_t>(length), 1);
    for (int i = 0; i < length; i++)
        fr.byte_ok[static_cast<size_t>(i)] = pk_.ok[static_cast<size_t>(2 * i)] && pk_.ok[static_cast<size_t>(2 * i + 1)];
    if (has_crc) {
        const uint16_t rx = static_cast<uint16_t>(bytes[static_cast<size_t>(length)] | (bytes[static_cast<size_t>(length + 1)] << 8));
        fr.crc_ok = rx == lora_code::payload_crc(fr.payload.data(), static_cast<size_t>(length));
    } else {
        fr.crc_ok = false;   // no CRC (LoRaWAN downlinks): the header checks + the parity count only
    }
    if (!fr.crc_ok && has_crc) return 0;   // only emit frames that passed the CRC
    // no CRC from an unknown transmitter (the search) or at SF5 / SF6: the
    // payload codewords must hold their parity (4/7 detects nothing: refused)
    if (!has_crc && !implicit_ && (strict_nocrc || pk_.low_sf) && (pk_.cr == 3 || pk_.fec_bad * 8 > pk_.fec_total)) return 0;
    out.push_back(std::move(fr));
    return 1;
}

}  // namespace lora_phy
