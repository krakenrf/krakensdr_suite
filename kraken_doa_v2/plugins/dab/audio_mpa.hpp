#pragma once
// MPEG Audio Layer II PCM decoding for DAB through minimp3 (vendored
// minimp3.h, CC0, github.com/lieff/minimp3). The frames given here have
// already passed the DAB checks (header, CRC, ScF-CRC).

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dab::audio {

class MpaDecoder {
public:
    static constexpr int MAX_SAMPLES = 1152 * 2;   // interleaved floats per frame
    MpaDecoder();
    ~MpaDecoder();
    void reset();
    // One complete audio frame. Returns samples per channel (0 = failed);
    // pcm: interleaved, +-1
    int decode(const uint8_t* frame, size_t n, float* pcm, int* channels, int* rate);
private:
    struct State;
    std::unique_ptr<State> s_;
};

}   // namespace dab::audio
