#pragma once

// sigclass internals shared by analyzer.cpp, detector.cpp, channelizer.cpp
// and features.cpp. Not a public header.

#include "sigclass/sigclass.hpp"
#include "sc_util.hpp"

#include <map>
#include <string>
#include <vector>

namespace sigclass::detail {

// ---- front end: one set of long FFTs per snapshot (detector PSD + channelizer)
// Overlap-save blocks of LONG_N = 2^6 * 3 * 5^4 (50 ms at 2.4 MHz) with
// LONG_OV2 samples of reach on each side (the views' zero-phase filters are
// at most 2 * LONG_OV2 + 1 taps long); block b covers input samples
// [b * LONG_HOP - LONG_OV2, b * LONG_HOP - LONG_OV2 + LONG_N).
constexpr int LONG_N = 120000;
constexpr int LONG_OV2 = 2000;
constexpr int LONG_HOP = LONG_N - 2 * LONG_OV2;   // 116000: divisible by every VIEW_DECIM
constexpr int BASE_BINS = 8;                      // fine bins per detector base bin
constexpr int NBASE = LONG_N / BASE_BINS;         // 15000 base bins (160 Hz at 2.4 MHz)
constexpr int CELL_BASE = 30;                     // base bins per floor cell
constexpr int NCELL = NBASE / CELL_BASE;          // 500 cells (4.8 kHz)
constexpr int SPEC_N = 1024;                      // short spectrogram frame (427 us)
constexpr int SPEC_BLK = 16, SPEC_BLKS = SPEC_N / SPEC_BLK;   // block sums for band powers

struct FrontEnd {
    double fs = 0.0;
    size_t n = 0;
    int nblocks = 0;
    avec<cf> x;            // the snapshot, mean removed
    avec<cf> blocks;       // nblocks x LONG_N spectra, natural order
    std::vector<float> psd;   // LONG_N, centred (index 0 = -fs/2): Hann PSD, E = noise variance
    int nframes = 0, hop = 512;
    double spec_ms = 0.0;     // time of the short spectrogram in the last run()
    std::vector<float> spec;  // nframes x SPEC_N, centred, same units as psd
    std::vector<float> spec_mean;   // SPEC_N: the frames' mean
    std::vector<double> spec_blk;   // nframes x SPEC_BLKS: sums of SPEC_BLK bins

    Fft long_fft, spec_fft;
    avec<cf> tmp, tmp2;
    std::vector<float> spec_win;
    std::vector<double> w2cum;   // prefix sums of the long block's Hann window squared
    std::vector<float> acc, macc;   // PSD / spectrogram-mean accumulators

    void run(size_t n_samples, double fs_hz, int spec_hop);   // x already filled
};

// ---- detector
struct Cand {
    Detection det;
    int centre_bin = 0;     // fine bin of the view centre, -LONG_N/2 .. LONG_N/2-1
};

class Detector {
public:
    void detect(const FrontEnd& fe, const Config& cfg, std::vector<Cand>& out);
    const std::vector<float>& floor_fine() const { return floor_f_; }   // centred, LONG_N
    double sigma1() const { return sigma1_; }
    double fill_corr() const { return fill_corr_; }   // the band-filling statistic of the last call

private:
    struct Region {
        int a = 0, b = 0;          // base bins, inclusive
        double power = 0.0;        // sum of (r - 1) over the region (base bins)
        std::vector<std::pair<int, int>> clip;   // fine-bin ranges clipped to a plateau
        std::vector<float> clip_level;           // ... their plateau r
        float floor_mult = 1.0f;                 // a peak on a plateau: floor x plateau
        bool comb = false;                       // formed from a comb of lines
    };
    void estimate_floor(const FrontEnd& fe, const Config& cfg);
    void segment(const FrontEnd& fe, const Config& cfg, const float* rr, int lo, int hi, float min_ratio,
                 std::vector<Region>& regs);
    void band_series(const FrontEnd& fe, int k0, int k1, std::vector<float>& e, double* sigma) const;
    bool measure(const FrontEnd& fe, const Region& rg, Cand& c, const Config& cfg);
    bool spectro_stats(const FrontEnd& fe, Cand& c, bool continuous);   // true: visible frame by frame
    void burst_search(const FrontEnd& fe, const Config& cfg, const std::vector<Cand>& cont,
                      std::vector<Cand>& out);
    bool band_fill(const FrontEnd& fe, const Config& cfg, const std::vector<Cand>& found, Cand& c, int force);   // force: 0 the test, 1 an irregular line comb (prominence only), 2 a regular comb (no test), 3 bursty pieces kept (prominence x 1.25)

    std::vector<float> p_;        // fine PSD with the DC zone bridged (centred)
    std::vector<float> pb_;       // base PSD (sum of BASE_BINS fine bins)
    std::vector<float> fb_;       // base floor
    std::vector<float> floor_f_;  // fine floor
    std::vector<float> r_;        // pb / fb
    std::vector<float> rr_;       // r_ / a wide region's plateau
    std::vector<uint8_t> valid_;  // base bins inside the guards
    std::vector<float> fs_floor_; // floor per short-spectrogram bin
    double sigma1_ = 0.1;         // relative std of a base bin in noise
    std::vector<float> scratch_, scratch2_;
    std::vector<double> prefix_;
    std::vector<uint8_t> mask_, mask2_;
    avec<cf> fbuf_, acf_;
    Fft fill_fft_;
    std::map<std::pair<int, double>, double> thr_cache_;   // gamma thresholds
    double fill_corr_ = 0.0;
};

// ---- channelizer: views from the front end's blocks
class Channelizer {
public:
    Channelizer();
    // centre_bin: fine bin (-LONG_N/2 .. LONG_N/2-1); out gets n / VIEW_DECIM[view] samples
    void extract(const FrontEnd& fe, int centre_bin, int view, avec<cf>& out);
    // noise variance per view sample given a fine floor (centred)
    double noise_var(const std::vector<float>& floor_fine, int centre_bin, int view) const;

private:
    std::vector<float> h_[3];   // zero-phase response, M_v bins, index j + M_v / 2 for j in [-M/2, M/2)
    Fft ifft_[3];
    avec<cf> tmp_, tmp2_;
    std::vector<cf> nco_;       // e^{-j 2 pi k / LONG_N}
};

// ---- features
struct ViewInfo {
    int view = 0;
    double fs_v = 0.0;
    double noise_var = 0.0;     // per view sample
    Detection det;
};

class Features {
public:
    Features();
    // y: the view (n samples); out: feature_dim() floats, every one finite
    void compute(const cf* y, size_t n, const ViewInfo& vi, float* out);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace sigclass::detail
