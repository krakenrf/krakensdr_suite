#include "signal_processing/audio_mixer.hpp"
#include "config.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* mode_name(DemodulatorMode m) {
    switch (m) {
        case DemodulatorMode::WBFM: return "WBFM";
        case DemodulatorMode::NBFM: return "NBFM";
        case DemodulatorMode::AM: return "AM";
        case DemodulatorMode::DIGITAL: return "DIGI";
    }
    return "?";
}
}  // namespace

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer() {
    if (enc_) opus_encoder_destroy(enc_);
}

std::shared_ptr<AudioMixer::Channel> AudioMixer::channel(int vfo) {
    if (!enabled_.load()) return nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& c : ch_)
        if (c->vfo == vfo) return c;
    auto c = std::make_shared<Channel>(vfo);
    c->fed_ms = steady_ms();
    ch_.push_back(c);
    std::cout << "Audio: D" << vfo << " joins the mix (" << ch_.size() << " VFO"
              << (ch_.size() == 1 ? "" : "s") << ")" << std::endl;
    return c;
}

void AudioMixer::retain(const std::vector<int>& vfos) {
    std::lock_guard<std::mutex> lk(mu_);
    ch_.erase(std::remove_if(ch_.begin(), ch_.end(), [&](const std::shared_ptr<Channel>& c) {
                  return std::find(vfos.begin(), vfos.end(), c->vfo) == vfos.end();
              }),
              ch_.end());
}

void AudioMixer::reset() {
    std::vector<std::shared_ptr<Channel>> chs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        chs = ch_;
    }
    for (const auto& c : chs) c->demod.reset_audio_buffer();
}

void AudioMixer::enable_processing() {
    enabled_ = true;
    std::cout << "Audio processing enabled" << std::endl;
}

void AudioMixer::disable_processing() {
    enabled_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    ch_.clear();
}

std::vector<unsigned char> AudioMixer::get_opus_packet(size_t n) {
    if (n == 0) return {};
    if (!enc_ && !enc_failed_) {
        // the live audio's settings (were FMDemodulatorRobust's): 48 kHz mono,
        // 32 kbit/s VBR, complexity 5
        int err = 0;
        enc_ = opus_encoder_create(static_cast<opus_int32>(AUDIO_SAMPLE_RATE), 1, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc_) {
            std::cerr << "Failed to create the audio Opus encoder: " << opus_strerror(err) << std::endl;
            enc_ = nullptr;
            enc_failed_ = true;
        } else {
            opus_encoder_ctl(enc_, OPUS_SET_BITRATE(32000));
            opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
            opus_encoder_ctl(enc_, OPUS_SET_VBR_CONSTRAINT(0));
            out_.resize(4000);   // the largest Opus packet
        }
    }
    if (!enc_) return {};

    // The channels heard now; those not fed for IDLE_MS leave the mix
    std::vector<std::shared_ptr<Channel>> chs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = steady_ms();
        ch_.erase(std::remove_if(ch_.begin(), ch_.end(), [&](const std::shared_ptr<Channel>& c) {
                      return now - c->fed_ms.load() > IDLE_MS;
                  }),
                  ch_.end());
        chs = ch_;
    }

    // Sum: when one VFO is open at a time (the usual case with squelch) it
    // plays at its own level; overlapping ones are soft-limited like a
    // single demodulator's output
    mix_.assign(n, 0.0f);
    for (const auto& c : chs) {
        const std::vector<float> a = c->demod.get_audio_buffer(n);
        const size_t m = std::min(n, a.size());
        for (size_t i = 0; i < m; i++) mix_[i] += a[i];
    }
    pcm_.resize(n);
    for (size_t i = 0; i < n; i++) {
        float v = mix_[i];
        if (std::fabs(v) > 0.95f) v = 0.95f * std::tanh(v / 0.95f);
        pcm_[i] = static_cast<opus_int16>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    }

    const opus_int32 bytes = opus_encode(enc_, pcm_.data(), static_cast<int>(n), out_.data(),
                                         static_cast<opus_int32>(out_.size()));
    if (bytes < 0) {
        std::cerr << "Opus encoding failed: " << opus_strerror(bytes) << std::endl;
        return {};
    }
    return std::vector<unsigned char>(out_.begin(), out_.begin() + bytes);
}

AudioMixer::Stats AudioMixer::stats() const {
    std::vector<std::shared_ptr<Channel>> chs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        chs = ch_;
    }
    Stats st;
    st.channels = static_cast<int>(chs.size());
    for (const auto& c : chs) {
        size_t w, r, over, under;
        c->demod.get_buffer_stats(w, r, over, under);
        st.over += over;
        st.under += under;
        st.fill = std::max(st.fill, c->demod.get_buffer_fill());
        st.signal = std::max(st.signal, c->demod.get_signal_strength());
        if (!st.modes.empty()) st.modes += ", ";
        st.modes += "D" + std::to_string(c->vfo) + " " + mode_name(c->mode.load());
    }
    return st;
}
