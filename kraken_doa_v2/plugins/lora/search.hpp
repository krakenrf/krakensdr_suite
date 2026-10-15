// Plan-free LoRa search, stage 1: where in the 2.4 MHz is there a signal
// shaped like LoRa? Works on the channelizer's forward FFTs (already
// computed for the plan channels, 520.8 Hz bins every 1.44 ms), so it
// costs next to nothing:
//  - power per bin summed over GROUP blocks (11.5 ms), divided by a slowly
//    tracked per-bin floor (the RTL's passband shape, spurs and DC drop out)
//  - per LoRa bandwidth B, a window of W groups (>= a symbol of SF10 at B,
//    46 ms - 0.14 s), centres every B/8: a candidate is a band of width B
//    that stands z sigma above the noise, is FLAT inside (a chirp sweeps
//    the whole band evenly: the four quarters within a factor of 2.5 - a
//    CW or FSK tone fails) and band-limited (just outside it < 30 % of
//    inside - a wider signal fails; the B it fits is its bandwidth), a
//    local maximum over the centres, reported once on its rising edge.
// Stage 2 (decoder.cpp) dechirps a candidate at every SF and both IQ
// orientations from the FFT history, so the preamble that set it off is
// decoded too. Sensitivity: an energy test - a few dB below the noise in
// the band at 125 kHz (the window's B x T is large), less for short / wide
// bursts; weaker signals are found once they are learned channels (a
// decoded transmitter keeps a CSS receiver of its own) or on plan channels.
#pragma once
#include "channelizer.hpp"
#include "regions.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lora_search {

constexpr int NB = lora_chan::NFFT;
constexpr int GROUP = 8;      // front blocks per group (11.5 ms)
constexpr int MAXW = 16;      // groups kept

struct Candidate {
    double off_hz;            // centre, Hz from the VFO centre
    double bw;
    double z;                 // sigma above the noise
    double snr_db;            // in-band SNR estimate
    int64_t group;            // the group that set it off
    int win_groups;           // its window length
};

class Stage1 {
public:
    double df = 0;            // bin width (Hz)
    double z_th = 9;          // detection threshold (sigma)
    std::vector<double> bws;  // the bandwidths searched

    void init(double fs) {
        df = fs / NB;
        acc_.assign(NB, 0.f);
        floor_.assign(NB, 0.f);
        ring_.assign(static_cast<size_t>(MAXW) * NB, 0.f);
        win_.assign(NB, 0.f);
        pre_.assign(NB + 1, 0.0);
        nacc_ = 0;
        groups_ = 0;
        last_.clear();
    }
    int64_t groups() const { return groups_; }

    // one forward FFT block (FFT order); candidates are appended on group ends
    void block(const fftwf_complex* X, std::vector<Candidate>& out) {
        for (int k = 0; k < NB; k++) {
            const int i = (k + NB / 2) % NB;    // frequency order
            acc_[i] += X[k][0] * X[k][0] + X[k][1] * X[k][1];
        }
        if (++nacc_ < GROUP) return;
        nacc_ = 0;
        group_end(out);
        std::fill(acc_.begin(), acc_.end(), 0.f);
    }

private:
    std::vector<float> acc_, floor_, ring_, win_;
    std::vector<double> pre_;
    int nacc_ = 0;
    int64_t groups_ = 0;
    struct Last { double bw; double off; int64_t until; };
    std::vector<Last> last_;   // active candidates (rising-edge reporting)

    static int win_for(double bw) {
        // >= one SF10 symbol at bw (the flatness needs the chirp's whole sweep), 4..12 groups
        const double sym = 1024.0 / bw;
        const int w = static_cast<int>(std::ceil(sym / (GROUP * lora_chan::HOP / 2.4e6)));
        return std::clamp(w, 4, 12);
    }

    void group_end(std::vector<Candidate>& out) {
        const float* g = acc_.data();
        // floor: mean per block; first 2 s plain mean, then a slow EMA from
        // groups where the bin is not clearly occupied
        if (groups_ < 50) {
            const float a = 1.0f / static_cast<float>(groups_ + 1);
            for (int i = 0; i < NB; i++) floor_[i] += a * (g[i] / GROUP - floor_[i]);
        } else {
            for (int i = 0; i < NB; i++) {
                const float v = g[i] / GROUP;
                if (v < 2.0f * floor_[i]) floor_[i] += 0.01f * (v - floor_[i]);
                else floor_[i] += 0.0005f * (v - floor_[i]);   // a real floor rise is followed slowly
            }
        }
        float* slot = &ring_[static_cast<size_t>(groups_ % MAXW) * NB];
        for (int i = 0; i < NB; i++) slot[i] = floor_[i] > 0 ? g[i] / (GROUP * floor_[i]) : 1.f;
        groups_++;
        if (groups_ < 60) return;   // floors not settled yet (~0.7 s)
        int done_w = 0;
        for (double bw : bws) {
            const int W = win_for(bw);
            if (W != done_w) {
                std::fill(win_.begin(), win_.end(), 0.f);
                for (int j = 0; j < W; j++) {
                    const float* r = &ring_[static_cast<size_t>((groups_ - 1 - j) % MAXW) * NB];
                    for (int i = 0; i < NB; i++) win_[i] += r[i];
                }
                for (int i = 0; i < NB; i++) win_[i] /= static_cast<float>(W);
                pre_[0] = 0;
                for (int i = 0; i < NB; i++) pre_[i + 1] = pre_[i] + win_[i] - 1.0;
                done_w = W;
            }
            scan_bw(bw, W, out);
        }
        // forget finished candidates
        last_.erase(std::remove_if(last_.begin(), last_.end(), [&](const Last& l) { return l.until < groups_; }), last_.end());
    }

    double mean(int a, int b) const {   // mean of (s - 1) over bins [a, b)
        a = std::clamp(a, 0, NB);
        b = std::clamp(b, 0, NB);
        return b > a ? (pre_[b] - pre_[a]) / (b - a) : 0.0;
    }

    void scan_bw(double bw, int W, std::vector<Candidate>& out) {
        const double kb = bw / df;                       // bins in the band
        const int step = std::max(1, static_cast<int>(std::lround(kb / 8)));
        const int half = static_cast<int>(std::lround(kb / 2));
        const int in_h = static_cast<int>(std::lround(kb * 0.45));
        const int o_a = half + std::max(1, static_cast<int>(std::lround(kb / 16)));
        const int o_b = half + std::max(2, static_cast<int>(std::lround(kb / 4)));
        const int q = std::max(1, half / 2);
        const double sig = 1.3 / std::sqrt(static_cast<double>(2 * in_h) * W * GROUP);
        const double sig_q = 1.3 / std::sqrt(static_cast<double>(q) * W * GROUP);
        const int lo = static_cast<int>(NB * 0.055) + o_b, hi = static_cast<int>(NB * 0.945) - o_b;
        struct Hit { int c; double z, m; };
        std::vector<Hit> hits;
        double zprev = 0, zprev2 = 0;
        int cprev = -1;
        for (int c = lo; c <= hi; c += step) {
            const double mi = mean(c - in_h, c + in_h);
            double z = mi / sig;
            if (z >= z_th) {
                const double mo = 0.5 * (mean(c - o_b, c - o_a) + mean(c + o_a, c + o_b));
                double qmin = 1e9, qmax = -1e9;
                for (int j = 0; j < 4; j++) {
                    const double v = mean(c - half + j * q, c - half + (j + 1) * q);
                    qmin = std::min(qmin, v);
                    qmax = std::max(qmax, v);
                }
                if (!(mo < 0.3 * mi && qmin > 0.4 * qmax - 3 * sig_q)) z = 0;
            } else {
                z = 0;
            }
            // local maximum of the previous centre
            if (cprev >= 0 && zprev > 0 && zprev >= zprev2 && zprev > z) hits.push_back({cprev, zprev, mean(cprev - in_h, cprev + in_h)});
            zprev2 = zprev;
            zprev = z;
            cprev = c;
        }
        for (const Hit& h : hits) {
            const double off = (h.c - NB / 2) * df;
            bool seen = false;
            for (Last& l : last_)
                if (l.bw == bw && std::fabs(l.off - off) < bw / 2) {
                    l.until = groups_ + 2;
                    seen = true;
                }
            if (seen) continue;
            last_.push_back({bw, off, groups_ + 2});
            Candidate cd;
            cd.off_hz = off;
            cd.bw = bw;
            cd.z = h.z;
            cd.snr_db = 10 * std::log10(std::max(h.m, 1e-3));
            cd.group = groups_ - 1;
            cd.win_groups = W;
            out.push_back(cd);
        }
    }
};

}  // namespace lora_search
