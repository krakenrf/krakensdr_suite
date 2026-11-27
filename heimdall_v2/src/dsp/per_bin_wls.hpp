#pragma once
// Per-bin EQ design: a weighted least-squares fit of a short complex FIR to the
// phase-only correction exp(-j arg H(f)), on a coarse grid of K = 128 groups of
// N / 128 bins, with a don't-care band beyond fb (docs/dev/calibration.md
// *A faster per-bin EQ*). A pure function: the per-bin accumulators in, the
// taps out - no heimdall globals, so tools/per_bin_design/per_bin_design_test.cpp
// can check it.
//
// Inputs as PerBinCalibration holds them: acc[ch][k] = sum over snapshots of
// X_ch[k] conj(X_0[k]), ref[k] = sum of |X_0[k]|^2, N = acc[0].size() bins in
// natural FFT order (bin k <-> f = k/N, k >= N/2 negative). Output: taps[ch][n],
// n < L, applied as y[t] = sum_n taps[n] x[t - n]; CH0 a delta at D = L/2.
#include <complex>
#include <string>
#include <vector>

struct PerBinWlsInfo {
    double median = 0.0;      // median coarse CH0 power over 0 < |f| <= 0.3
    int usable = 0;           // groups above the gate (of K)
    int inband = 0;           // groups with 0 < |f_k| <= fb (gated or not)
    int inband_usable = 0;    // ... of those above the gate
    // per channel: the phase (deg) of the lag-0 cross-correlation with CH0 once
    // the taps are applied, from the same cross-spectra - what the dominant-
    // eigenvector phase reads on the equalized noise source if nothing drifted
    // (the band edges the fit leaves out and a clipped noise source move it off
    // 0). 0 for CH0.
    std::vector<double> band_phase_deg;
    std::string why;          // reason when the design refuses
};

// Returns false (taps untouched) when the median is 0, when fewer than half of
// the in-band groups pass the gate, on a non-positive Cholesky pivot, or on a
// non-finite tap.
bool design_per_bin_wls(const std::vector<std::vector<std::complex<float>>>& acc, const std::vector<double>& ref,
                        int nch, int L, double fb, std::vector<std::vector<std::complex<double>>>& taps,
                        PerBinWlsInfo* info = nullptr);
