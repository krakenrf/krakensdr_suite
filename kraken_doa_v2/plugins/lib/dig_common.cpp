#include "dig_common.hpp"

#include <algorithm>
#include <cmath>

#include <liquid/liquid.h>

namespace dig {

void Bridge::option(const std::string& key, const std::string& value) {
    if (key == "verbose") {
        opts.verbose = value == "1";
    } else if (key == "slot") {
        int s = atoi(value.c_str());
        opts.dmr_slot = (s >= 0 && s <= 2) ? s : 0;
    } else if (key == "nac") {
        // 3 hex digits; "" / "any" / anything else = any NAC
        char* end = nullptr;
        long v = strtol(value.c_str(), &end, 16);
        opts.p25_nac = (!value.empty() && end && *end == '\0' && v >= 0 && v <= 0xFFF) ? static_cast<int>(v) : -1;
    }
}

void SampleBuf::trim_before(int64_t abs) {
    if (abs <= base_) return;
    size_t n = static_cast<size_t>(std::min<int64_t>(abs - base_, static_cast<int64_t>(buf_.size())));
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n));
    base_ += static_cast<int64_t>(n);
}

// --- FM front end ------------------------------------------------------------
FmFrontEnd::FmFrontEnd() {
    // 12.5 kHz channel filter (4FSK +-1.9 kHz deviation + 2.4 kHz modulation
    // bandwidth, plus room for a residual offset)
    float h[63];
    liquid_firdes_kaiser(63, 6500.0f / static_cast<float>(RATE), 60.0f, 0.0f, h);
    float s = 0;
    for (float v : h) s += v;
    for (float& v : h) v /= s;
    lp_ = firfilt_crcf_create(h, 63);
}

FmFrontEnd::~FmFrontEnd() {
    if (lp_) firfilt_crcf_destroy(static_cast<firfilt_crcf>(lp_));
}

void FmFrontEnd::reset() {
    firfilt_crcf_reset(static_cast<firfilt_crcf>(lp_));
    prev_ = {1, 0};
}

const std::vector<float>& FmFrontEnd::process(const kp::cf* x, size_t n) {
    auto f = static_cast<firfilt_crcf>(lp_);
    out_.resize(n);
    const float k = static_cast<float>(RATE / (2.0 * M_PI));
    for (size_t i = 0; i < n; i++) {
        kp::cf y;
        firfilt_crcf_push(f, x[i]);
        firfilt_crcf_execute(f, &y);
        out_[i] = std::arg(y * std::conj(prev_)) * k;
        prev_ = y;
    }
    return out_;
}

// --- RRC front end -----------------------------------------------------------
RrcFrontEnd::RrcFrontEnd(unsigned sps, unsigned span, float beta) {
    taps_.resize(2 * sps * span + 1);
    liquid_firdes_rrcos(sps, span, beta, 0.0f, taps_.data());
    f_ = firfilt_crcf_create(taps_.data(), static_cast<unsigned>(taps_.size()));
}

RrcFrontEnd::~RrcFrontEnd() {
    if (f_) firfilt_crcf_destroy(static_cast<firfilt_crcf>(f_));
}

void RrcFrontEnd::reset() { firfilt_crcf_reset(static_cast<firfilt_crcf>(f_)); }

const std::vector<kp::cf>& RrcFrontEnd::process(const kp::cf* x, size_t n) {
    auto f = static_cast<firfilt_crcf>(f_);
    out_.resize(n);
    for (size_t i = 0; i < n; i++) {
        firfilt_crcf_push(f, x[i]);
        firfilt_crcf_execute(f, &out_[i]);
    }
    return out_;
}

}  // namespace dig
