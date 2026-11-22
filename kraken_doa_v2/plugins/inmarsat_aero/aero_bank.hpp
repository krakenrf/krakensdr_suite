#pragma once

// The "all channels" mode's channelizer: an overlap-save FFT filter bank.
//
// The input (the VFO's own rate R, up to 2.4 MS/s) goes through ONE
// forward FFT of N points every N/2 samples (bins of ~375 Hz). A channel is
// M of those bins around its centre, weighted by a flat-topped window
// (flat to +-8.6 kHz, zero at +-12 kHz) and brought back by an M-point
// inverse FFT: its samples at M x R / N (24 kHz for R = 2.4 MHz / 1.2 MHz /
// 600 / 300 kHz ...; other rates are resampled to 24 kHz per channel). The
// forward FFT is shared by every channel and is the whole cost of an idle
// band; a channel costs one small inverse FFT per block. The bins' power is
// the survey's spectrum for free (sum since the last take_spectrum()).

#include "aero_demod.hpp"
#include "kraken_fft.hpp"

#include <algorithm>
#include <complex>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace aero {

class FilterBank {
public:
    explicit FilterBank(double rate);
    ~FilterBank();
    FilterBank(const FilterBank&) = delete;
    FilterBank& operator=(const FilterBank&) = delete;

    double rate() const { return R_; }
    double bin_hz() const { return R_ / N_; }
    int bins() const { return N_; }
    // a channel centred on bin `bin` (0 .. N-1, natural order); returns its id
    int open(int bin);
    void close(int id);
    int bin_of(double hz) const;          // the bin nearest to hz (-R/2 .. R/2)
    double hz_of(int bin) const;          // a bin's centre frequency (-R/2 .. R/2)
    // input samples; per block, `out(id, samples, n)` for every channel (24 kHz)
    template <class F>
    void push(const cf* x, size_t n, F&& out) {
        for (size_t i = 0; i < n;) {
            const size_t k = std::min(n - i, static_cast<size_t>(N_ - fill_));
            std::copy(x + i, x + i + k, buf_.begin() + fill_);
            fill_ += static_cast<int>(k);
            i += k;
            if (fill_ == N_) {
                block();
                for (auto& [id, c] : ch_)
                    if (!c.y.empty()) out(id, c.y.data(), c.y.size());
                // keep the last N - L for the next block
                std::copy(buf_.begin() + L_, buf_.end(), buf_.begin());
                fill_ = N_ - L_;
            }
        }
    }
    // the bins' mean power since the last call (natural order), blocks summed
    int take_spectrum(std::vector<float>& p);
    // input samples consumed into whole blocks (the time of a channel's next sample)
    int64_t blocks() const { return blocks_; }
    int hop() const { return L_; }
    // nothing in the band for a while: the survey's spectrum from one block in four
    void set_idle(bool idle) { idle_ = idle; }
    // a block's first output is N/4 input samples after its start
    int first_offset() const { return N_ / 4; }

private:
    double R_;
    int N_, M_, L_, Lo_;                  // FFT size, channel bins, hop, outputs per block
    double fo_;                           // a channel's rate before its resampler
    std::vector<cf> buf_;
    int fill_ = 0;
    int64_t blocks_ = 0;
    kfft_complex *fin_ = nullptr, *fout_ = nullptr, *cin_ = nullptr, *cout_ = nullptr;
    kfft_plan fplan_ = nullptr, cplan_ = nullptr;
    std::vector<float> win_;              // M bin weights, centre at M/2
    std::vector<float> spec_;
    int nspec_ = 0;
    bool idle_ = false;
    struct Chan {
        int bin = 0;
        int64_t b0 = 0;                   // the block it was opened at
        void* rs = nullptr;               // msresamp_crcf to 24 kHz (or none)
        std::vector<cf> y;                // this block's output (24 kHz)
    };
    std::map<int, Chan> ch_;
    int next_id_ = 1;
    void block();
};

}  // namespace aero
