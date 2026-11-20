// minimp3 (CC0) compiled here, once, with float output (see audio_mpa.hpp).

#include "audio_mpa.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dab::audio {

struct MpaDecoder::State {
    mp3dec_t dec;
};

MpaDecoder::MpaDecoder() : s_(std::make_unique<State>()) { reset(); }
MpaDecoder::~MpaDecoder() = default;

void MpaDecoder::reset() { mp3dec_init(&s_->dec); }

int MpaDecoder::decode(const uint8_t* frame, size_t n, float* pcm, int* channels, int* rate) {
    *channels = 0;
    *rate = 0;
    if (!frame || n < 4 || n > 4096) return 0;
    mp3dec_frame_info_t info{};
    int s = mp3dec_decode_frame(&s_->dec, frame, static_cast<int>(n), pcm, &info);
    // the whole buffer must have been taken as this one frame
    if (s <= 0 || info.frame_offset != 0 || info.frame_bytes != static_cast<int>(n) || info.layer != 2) return 0;
    if (info.channels < 1 || info.channels > 2 || s * info.channels > MAX_SAMPLES) return 0;
    *channels = info.channels;
    *rate = info.hz;
    return s;
}

}   // namespace dab::audio
