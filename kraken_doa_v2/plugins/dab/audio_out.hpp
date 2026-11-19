#pragma once
// DAB audio layer output: mono PCM at the decoder's rate (48 / 32 / 24 /
// 16 kHz) -> anti-alias low-pass + integer decimation -> 8 kHz, +-1, to
// AudioSink::pcm. Lost frames become silence with short fades (no clicks):
// the stage holds back the last 4 ms so it can fade them out when the next
// frame is lost, and fades the first 4 ms in after a gap or at the start.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dab {
struct AudioSink;
}

namespace dab::audio {

class Pcm8k {
public:
    static constexpr int OUT_RATE = 8000;
    static constexpr size_t MAX_CHUNK = 4096;   // input samples per internal step
    explicit Pcm8k(AudioSink& sink) : sink_(sink) {}
    // in: mono samples at `rate` (a multiple of 8000, 16..48 kHz). Returns
    // false (nothing played) for an unsupported rate.
    bool push(const float* in, size_t n, int rate);
    // silence in place of lost audio: `ms` milliseconds (fades the held tail out)
    void gap(int ms);
    // drop everything (no output); the next push fades in
    void reset();
    bool active() const { return rate_ != 0; }

private:
    bool configure(int rate);
    void emit(const float* y, size_t n);
    void emit_zeros(size_t n);

    AudioSink& sink_;
    int rate_ = 0, decim_ = 1;
    std::vector<float> taps_;     // odd length, symmetric
    std::vector<float> x_;        // history (taps - 1) + one chunk
    size_t skip_ = 0;             // input samples to skip before the next output
    std::vector<float> y_;        // decimated chunk
    std::vector<float> work_;     // tail + chunk
    static constexpr size_t HOLD = 32;   // 4 ms at 8 kHz
    float tail_[HOLD] = {};
    size_t tail_n_ = 0;
    size_t fade_in_ = 0;          // samples of the fade-in already played (HOLD = done)
};

}   // namespace dab::audio
