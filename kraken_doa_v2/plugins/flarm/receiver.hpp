#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KrakenRF Inc. - part of the KrakenSDR `flarm` decoder
// plugin, licensed under the GNU General Public License version 3 (LICENSE in
// this folder; README.md "Licence").

// FLARM / OGN receiver over the whole 2.4 MHz VFO.
//
// Both protocols share one physical layer: GFSK BT 0.5, +-50 kHz, 100 kchip/s
// Manchester (IEEE: a 1 = chips "0 1", chip 1 = the +50 kHz tone) = 50
// kbit/s; a sync word and 26 bytes, ~5 ms on air.
//
// Burst detection: a 256-point FFT (Hann) of every 256 input samples (106.7
// us, 9375 Hz bins); per listened channel the power of the 17 bins within
// +-75 kHz against the channel's own noise floor. A burst of 2-14 ms above
// the threshold becomes a candidate; it is decided 4 frames later:
//  - broadband rule: 2+ other candidates starting within +-2 frames and the
//    lot spanning >= 600 kHz = a noise burst (calibration noise source,
//    impulse noise): skipped;
//  - leakage rule: a stronger overlapping candidate within 250 kHz still
//    pending = this is its spectral copy: skipped.
// Demodulation: the burst +-3 frames from the input history, mixed to 0 Hz,
// a 49-tap 110 kHz low-pass decimated by 6 to 400 kS/s (4 samples per chip),
// FM discriminator, summed over each chip; the last 28 bits (56 chips) of
// either sync word by correlation, then the 208 data bits as "second chip
// minus first chip". The packet layer (packet.hpp) checks them; the timing
// is tried at the sync's best sample and one either side.

#include "kraken_fft.hpp"
#include "kraken_plugin.hpp"
#include "packet.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace flarm {

constexpr double FS = 2.4e6;           // the plugin's input rate
constexpr int NFFT = 256;              // detection FFT = one frame of input
constexpr double BIN_HZ = FS / NFFT;   // 9375 Hz
constexpr double FRAME_S = NFFT / FS;  // 106.67 us
constexpr int DEC = 6;                 // -> 400 kS/s
constexpr double FSD = FS / DEC;
constexpr int SPC = 4;                 // decimated samples per chip
constexpr int SYNC_CHIPS = 56;         // the sync word's last 28 bits
constexpr int PACKET_CHIPS = SYNC_CHIPS + 2 * pkt::BITS;   // 472

// One listened channel
struct Window {
    double off_hz = 0;                 // from the input's 0 Hz
    double mhz = NAN;                  // its channel (NAN = the RF is unknown)
};

// One packet that passed its checks
struct RxPacket {
    pkt::Frame frame;
    double t0 = 0, t1 = 0;             // its span: input sample indexes since reset (fractional)
    double carrier_hz = 0;             // measured carrier, Hz from the input's 0 Hz
    double level_db = 0;               // dBFS
    double snr_db = 0;                 // burst power over the channel's floor
    int win = 0;
};

class Receiver {
public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // drops everything but the counters (they count since the plugin started)
    void reset();
    // a new channel list: pending candidates dropped, the floors learnt again
    void set_windows(const std::vector<Window>& w);
    const std::vector<Window>& windows() const { return win_; }
    void set_threshold_db(double db);
    // FLARM_DUMP: every demodulated burst's 400 kHz baseband to this folder
    void set_dump_dir(const std::string& dir) { dump_dir_ = dir; }

    // n input samples; now = Host::time() of the first one
    void process(const kp::cf* x, size_t n, double now, std::vector<RxPacket>* out);
    // Host::time() of an input sample index
    double time_of(double abs_idx) const { return t_now_ + (abs_idx - static_cast<double>(t_abs_)) / FS; }

    uint64_t abs() const { return abs_; }                // input samples since reset
    uint64_t demodulated() const { return demodulated_; }
    uint64_t broadband() const { return broadband_; }
    std::vector<std::string> log;                        // FLARM_DUMP lines for host.log

private:
    struct Chan {                      // per window: floor + burst state
        int k0 = 0;                    // centre bin (0..NFFT-1)
        float floor = 0;
        int learnt = 0;                // frames seen (64 = learnt)
        bool on = false;
        int64_t f0 = 0, f1 = 0;        // first / last active frame of the burst
        float peak = 0;
        int quiet = 0;                 // inactive frames since the last active one
    };
    struct Cand {
        int win = 0;
        int64_t f0 = 0, f1 = 0;
        double snr = 0;                // peak power / floor
        int64_t decide = 0;            // the frame it is decided at
    };
    struct Decided {
        int64_t at = 0, f0 = 0;
        double off = 0;
    };

    std::vector<Window> win_;
    std::vector<Chan> ch_;
    std::vector<Cand> pending_;
    std::deque<Decided> recent_;
    double thr_ = 3.98;                // power ratio
    std::string dump_dir_;

    // input history (written as it comes, frame by frame)
    std::vector<kp::cf> ring_;
    uint64_t abs_ = 0;
    int64_t frame_ = 0;                // frames completed
    double t_now_ = 0;                 // Host::time() of input sample t_abs_
    uint64_t t_abs_ = 0;
    // detection FFT
    float hann_[NFFT];
    kfft_complex *fin_, *fout_;
    kfft_plan plan_;
    float pw_[NFFT] = {};
    std::vector<int> bins_;            // the bins the windows add up
    // demodulation scratch
    std::vector<float> lp_;
    std::vector<kp::cf> mix_, y_;
    std::vector<float> d_, m_;
    float tmpl_[2][SYNC_CHIPS];        // sync chips (+-1): FLARM, OGN

    uint64_t demodulated_ = 0, broadband_ = 0;

    void frame(std::vector<RxPacket>* out);
    void demod(const Cand& c, std::vector<RxPacket>* out);
    bool try_bits(int proto, int n0, float P, RxPacket& pk);
    void dump(const Cand& c, uint64_t first, int N, double best_f, double best_o, double level_db);
};

}  // namespace flarm
