#include "aero_bank.hpp"

#include <complex>
#include <liquid/liquid.h>

#include <algorithm>
#include <cmath>

namespace aero {

namespace {
constexpr double OUT_RATE = 24000;
constexpr double BIN_HZ = 375;          // the bins' target width
constexpr double FLAT_HZ = 8600;        // a channel's flat part (+-): 10.5k A-QPSK +-4.2 kHz + 2.5 kHz offset + margin
constexpr double STOP_HZ = 12000;       // and where it is down to zero

bool fft_ok(int n) { return kfft_direct(n); }

// the pffft size nearest to n (2^a 3^b 5^c, a multiple of 16)
int nice_size(double n, bool at_least) {
    int best = 16;
    double bd = 1e18;
    for (int m = 16; m <= 1 << 16; m += 16) {
        if (!fft_ok(m)) continue;
        if (at_least && m < n - 1e-9) continue;
        const double d = std::fabs(m - n);
        if (d < bd) bd = d, best = m;
        if (m > 2 * n + 64) break;
    }
    return best;
}
}  // namespace

FilterBank::FilterBank(double rate) : R_(rate) {
    N_ = nice_size(R_ / BIN_HZ, false);
    const double b = R_ / N_;
    M_ = nice_size(OUT_RATE / b, true);
    if (M_ >= N_) M_ = N_;              // a narrow VFO: a channel is the whole input
    L_ = N_ / 2;
    Lo_ = static_cast<int>(static_cast<int64_t>(L_) * M_ / N_);
    fo_ = R_ * M_ / N_;
    buf_.assign(static_cast<size_t>(N_), cf(0, 0));
    fin_ = kfft_alloc_complex(static_cast<size_t>(N_));
    fout_ = kfft_alloc_complex(static_cast<size_t>(N_));
    fplan_ = kfft_plan_dft_1d(N_, fin_, fout_, KFFT_FORWARD);
    cin_ = kfft_alloc_complex(static_cast<size_t>(M_));
    cout_ = kfft_alloc_complex(static_cast<size_t>(M_));
    cplan_ = kfft_plan_dft_1d(M_, cin_, cout_, KFFT_BACKWARD);
    win_.resize(static_cast<size_t>(M_));
    for (int m = 0; m < M_; m++) {
        const double f = std::fabs((m - M_ / 2) * b);
        const double a = std::clamp((f - FLAT_HZ) / (STOP_HZ - FLAT_HZ), 0.0, 1.0);
        win_[static_cast<size_t>(m)] = static_cast<float>((0.5 + 0.5 * std::cos(M_PI * a)) / N_);
    }
    if (M_ == N_)
        for (auto& w : win_) w = 1.0f / N_;
    spec_.assign(static_cast<size_t>(N_), 0.0f);
}

FilterBank::~FilterBank() {
    for (auto& [id, c] : ch_)
        if (c.rs) msresamp_crcf_destroy(static_cast<msresamp_crcf>(c.rs));
    kfft_destroy_plan(fplan_);
    kfft_destroy_plan(cplan_);
    kfft_free(fin_);
    kfft_free(fout_);
    kfft_free(cin_);
    kfft_free(cout_);
}

int FilterBank::bin_of(double hz) const {
    long k = std::lround(hz / (R_ / N_));
    return static_cast<int>(((k % N_) + N_) % N_);
}

double FilterBank::hz_of(int bin) const {
    const int k = bin >= N_ / 2 ? bin - N_ : bin;
    return k * (R_ / N_);
}

int FilterBank::open(int bin) {
    Chan c;
    c.bin = ((bin % N_) + N_) % N_;
    c.b0 = blocks_;
    if (std::fabs(fo_ - OUT_RATE) > 0.5) c.rs = msresamp_crcf_create(static_cast<float>(OUT_RATE / fo_), 60.0f);
    const int id = next_id_++;
    ch_[id] = std::move(c);
    return id;
}

void FilterBank::close(int id) {
    auto it = ch_.find(id);
    if (it == ch_.end()) return;
    if (it->second.rs) msresamp_crcf_destroy(static_cast<msresamp_crcf>(it->second.rs));
    ch_.erase(it);
}

int FilterBank::take_spectrum(std::vector<float>& p) {
    p.resize(spec_.size());
    const int n = nspec_;
    for (size_t k = 0; k < spec_.size(); k++) p[k] = n ? spec_[k] / static_cast<float>(n) : 0.0f;
    std::fill(spec_.begin(), spec_.end(), 0.0f);
    nspec_ = 0;
    return n;
}

void FilterBank::block() {
    // an idle band (no channel, nothing found for a while - set_idle): only
    // the survey needs the spectrum, one block in four is enough for it
    if (idle_ && ch_.empty() && (blocks_ & 3) != 0) {
        blocks_++;
        return;
    }
    for (int i = 0; i < N_; i++) {
        fin_[i][0] = buf_[static_cast<size_t>(i)].real();
        fin_[i][1] = buf_[static_cast<size_t>(i)].imag();
    }
    kfft_execute(fplan_);
    for (int k = 0; k < N_; k++) spec_[static_cast<size_t>(k)] += fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];
    nspec_++;
    std::vector<cf> tmp(static_cast<size_t>(Lo_));
    for (auto& [id, c] : ch_) {
        // bins bin - M/2 .. bin + M/2 - 1, weighted, to the inverse FFT's natural order
        for (int m = 0; m < M_; m++) {
            const int k = ((c.bin - M_ / 2 + m) % N_ + N_) % N_;
            const int j = ((m - M_ / 2) % M_ + M_) % M_;
            const float w = win_[static_cast<size_t>(m)];
            cin_[j][0] = fout_[k][0] * w;
            cin_[j][1] = fout_[k][1] * w;
        }
        kfft_execute(cplan_);
        // the frequency shift: the block's start (b L) in the absolute phase
        // of bin `bin`: e^(-2 pi i bin b L / N)
        const int64_t q = (static_cast<int64_t>(c.bin) * ((blocks_ * L_) % N_)) % N_;
        const double ph = -2 * M_PI * static_cast<double>(q) / N_;
        const cf rot(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        // the zero-phase filter's valid outputs: the middle half of the block
        for (int j = 0; j < Lo_; j++) {
            const int s = M_ / 4 + j;
            tmp[static_cast<size_t>(j)] = cf(cout_[s][0], cout_[s][1]) * rot;
        }
        if (c.rs) {
            c.y.resize(static_cast<size_t>(Lo_) * 2 + 16);
            unsigned int ny = 0;
            msresamp_crcf_execute(static_cast<msresamp_crcf>(c.rs), tmp.data(), static_cast<unsigned int>(Lo_), c.y.data(), &ny);
            c.y.resize(ny);
        } else {
            c.y = tmp;
        }
    }
    blocks_++;
}

}  // namespace aero
