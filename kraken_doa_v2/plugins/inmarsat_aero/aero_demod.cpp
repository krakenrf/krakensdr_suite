#include "aero_demod.hpp"

#include <algorithm>
#include <cstdlib>

namespace aero {

std::vector<float> rrc_taps(double sps, int span, double beta) {
    const int half = static_cast<int>(std::ceil(span * sps));
    std::vector<float> h(2 * half + 1);
    double sum = 0;
    for (int i = -half; i <= half; i++) {
        const double t = i / sps;
        double v;
        if (std::fabs(t) < 1e-9) {
            v = 1 - beta + 4 * beta / M_PI;
        } else if (beta > 0 && std::fabs(std::fabs(4 * beta * t) - 1) < 1e-6) {
            v = beta / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * beta)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * beta)));
        } else {
            v = (std::sin(M_PI * t * (1 - beta)) + 4 * beta * t * std::cos(M_PI * t * (1 + beta))) /
                (M_PI * t * (1 - 16 * beta * beta * t * t));
        }
        h[i + half] = static_cast<float>(v);
        sum += v;
    }
    for (float& v : h) v = static_cast<float>(v / sum);
    return h;
}

void Demod::gains(float bw) {
    // second-order loop, damping 0.707, noise bandwidth bw x bit period;
    // phase detector gain ~1 (normalised decisions)
    const float zeta = 0.707f;
    const float th = bw / (zeta + 0.25f / zeta);
    const float den = 1 + 2 * zeta * th + th * th;
    kp_ = 4 * zeta * th / den;
    ki_ = 4 * th * th / den;
}

void Demod::start(const DemodCfg& c, double f0_hz, double t0_samples) {
    cfg_ = c;
    on_ = true;
    rate_ = c.rate;
    fsc_ = c.fs / c.decim;
    sps_ = fsc_ / rate_;
    f_nco_ = f0_hz;
    ph_nco_ = 0;
    // decimator: low-pass at 0.4 x the output rate
    if (c.decim > 1) {
        hd_ = kp::fir_lowpass(8 * c.decim + 1, static_cast<float>(0.4 * fsc_), static_cast<float>(c.fs));
    } else {
        hd_.clear();
    }
    dbuf_.assign(2 * std::max<size_t>(hd_.size(), 1), cf(0, 0));
    dpos_ = dcnt_ = 0;
    // matched filter: root raised cosine over the symbol (A-QPSK: a branch
    // symbol is two bits)
    const double sym = c.mod == Mod::AQPSK ? 2 * sps_ : sps_;
    hm_ = rrc_taps(sym, 5, c.rolloff);
    mbuf_.assign(2 * hm_.size(), cf(0, 0));
    mpos_ = 0;
    for (auto& v : ring_) v = cf(0, 0);
    nmf_ = 0;
    tn_ = static_cast<double>(hm_.size()) + 4;   // first bit once the filter is full
    t0_ = t0_samples;
    delay_in_ = (static_cast<double>(hm_.size()) - 1) / 2 * c.decim - (c.decim - 1) +
                (hd_.empty() ? 0 : (static_cast<double>(hd_.size()) - 1) / 2);
    w_ = -2 * M_PI * f_nco_ / c.fs;
    theta_ = nu_ = 0;
    amp_ = 0;
    q_ = 0.8f;
    ad_ = ad2_ = 0;
    dprev_[0] = dprev_[1] = 0;
    yprev_ = cf(0, 0);
    nbits_ = 0;
    axis_ = 0;
    gains(c.pll_bw * 4);
    kt_ = c.tim_gain * 4;
}

cf Demod::interp(double t) const {
    // cubic (Catmull-Rom) through the 4 outputs around t
    const int64_t i0 = static_cast<int64_t>(std::floor(t));
    const float mu = static_cast<float>(t - static_cast<double>(i0));
    const cf a = ring_[(i0 - 1) & (RING - 1)], b = ring_[i0 & (RING - 1)], c = ring_[(i0 + 1) & (RING - 1)],
             d = ring_[(i0 + 2) & (RING - 1)];
    const cf c1 = 0.5f * (c - a);
    const cf c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
    const cf c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * mu + c2) * mu + c1) * mu + b;
}

void Demod::process(const cf* x, size_t n, std::vector<SoftBit>& out) {
    if (!on_) return;
    const size_t nd = hd_.size(), nm = hm_.size();
    for (size_t i = 0; i < n; i++) {
        const cf v = x[i] * cf(static_cast<float>(std::cos(ph_nco_)), static_cast<float>(std::sin(ph_nco_)));
        ph_nco_ += w_;
        if (ph_nco_ > M_PI) ph_nco_ -= 2 * M_PI;
        if (ph_nco_ < -M_PI) ph_nco_ += 2 * M_PI;
        cf s = v;
        if (nd) {
            dbuf_[dpos_] = dbuf_[dpos_ + nd] = v;
            dpos_ = (dpos_ + 1) % static_cast<int>(nd);
            if (++dcnt_ < cfg_.decim) continue;
            dcnt_ = 0;
            cf acc(0, 0);
            const cf* p = &dbuf_[dpos_];
            for (size_t k = 0; k < nd; k++) acc += p[k] * hd_[nd - 1 - k];
            s = acc;
        }
        // matched filter
        mbuf_[mpos_] = mbuf_[mpos_ + nm] = s;
        mpos_ = (mpos_ + 1) % static_cast<int>(nm);
        cf acc(0, 0);
        const cf* p = &mbuf_[mpos_];
        for (size_t k = 0; k < nm; k++) acc += p[k] * hm_[nm - 1 - k];
        ring_[nmf_ & (RING - 1)] = acc;
        nmf_++;
        // a bit instant whose 4 neighbours are in
        while (tn_ + 2 < static_cast<double>(nmf_)) bit(out);
    }
}

void Demod::bit(std::vector<SoftBit>& out) {
    const cf y = interp(tn_);
    const cf rot(static_cast<float>(std::cos(theta_)), static_cast<float>(-std::sin(theta_)));
    // amplitude (AGC on the matched filter's output)
    const float p = std::norm(y);
    amp_ = amp_ == 0 ? p : amp_ + 0.01f * (p - amp_);
    const float g = 1.0f / std::sqrt(amp_ + 1e-30f);
    const cf z = y * rot * g;
    // axis
    cf a;
    if (cfg_.mod == Mod::AQPSK) a = (axis_ & 1) ? cf(0, 1) : cf(1, 0);
    else a = (axis_ & 3) == 0 ? cf(1, 0) : (axis_ & 3) == 1 ? cf(0, 1) : (axis_ & 3) == 2 ? cf(-1, 0) : cf(0, -1);
    const cf v = z * std::conj(a);
    const float d = v.real(), o = v.imag();
    // quality (bimodality of d)
    const float ad = std::fabs(d);
    ad_ = ad_ == 0 ? ad : ad_ + 0.01f * (ad - ad_);
    ad2_ = ad2_ == 0 ? d * d : ad2_ + 0.01f * (d * d - ad2_);
    q_ = ad_ / std::sqrt(ad2_ + 1e-30f);
    const float dn = d / std::max(ad_, 1e-6f);
    // carrier: decision-directed Costas
    const float e = std::clamp((d > 0 ? 1.0f : -1.0f) * o / std::max(ad_, 1e-6f), -1.0f, 1.0f);
    nu_ += ki_ * e;
    theta_ += kp_ * e + nu_;
    if (theta_ > M_PI) theta_ -= 2 * M_PI;
    if (theta_ < -M_PI) theta_ += 2 * M_PI;
    // timing: Gardner
    float te = 0;
    if (cfg_.mod == Mod::AQPSK) {
        // the previous bit's sample on this bit's axis = the branch's mid point
        const float m = (yprev_ * rot * g * std::conj(a)).real();
        te = (dprev_[0] - d) * m / std::max(ad_ * ad_, 1e-12f);
        dprev_[0] = dprev_[1];
        dprev_[1] = d;
    } else {
        const cf ym = interp(tn_ - sps_ / 2) * rot * g;
        // the mid point between bit n-1 (axis a/j) and bit n (axis a): axis a e^-j45
        const cf am = a * cf(0.70710678f, -0.70710678f);
        const float m = (ym * std::conj(am)).real();
        te = (dprev_[1] - d) * m / std::max(ad_ * ad_, 1e-12f) * tsign_;
        dprev_[1] = d;
    }
    te = std::clamp(te, -2.0f, 2.0f);
    yprev_ = y;
    out.push_back({dn, t0_ + tn_ * cfg_.decim - delay_in_});
    nbits_++;
    axis_ = (axis_ + 1) & 3;
    tn_ += sps_ * (1.0 + kt_ * te);
    if (nbits_ == 256) {
        gains(cfg_.pll_bw);
        kt_ = cfg_.tim_gain;
    }
    if ((nbits_ & 31) == 0) {
        f_nco_ += nu_ * rate_ / (2 * M_PI);
        nu_ = 0;
        w_ = -2 * M_PI * f_nco_ / cfg_.fs;
    }
}

}  // namespace aero
