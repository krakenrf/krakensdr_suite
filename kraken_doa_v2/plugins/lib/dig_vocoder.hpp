#pragma once

// Voice codecs of the digital decoders.
//
//  - ImbeDecoder: P25 Phase 1 full-rate IMBE (7200 bit/s with FEC, 4400 voice),
//    built in. A port of the IMBE path of mbelib (ISC licence, see
//    mbe_tables.hpp) with a per-instance random generator.
//  - MbeLib: AMBE (D-STAR, 3600x2400) and AMBE+2 (DMR, 3600x2450) through a
//    mbelib the USER installed (README "Digital voice codecs"). Loaded at run
//    time with dlopen, so kraken_doa neither ships nor links any AMBE code.
//  - TetraCodec: TETRA ACELP through the ETSI reference codec programs
//    (cdecoder / sdecoder) the user built (README), run as child processes.
//
// All decoders output 8 kHz float audio, nominally +-1.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace dig {

// MBE model parameters (layout identical to mbelib's mbe_parms)
struct MbeParms {
    float w0;
    int L;
    int K;
    int Vl[57];
    float Ml[57];
    float log2Ml[57];
    float PHIl[57];
    float PSIl[57];
    float gamma;
    int un;
    int repeat;
};

class ImbeDecoder {
public:
    ImbeDecoder() { reset(); }
    void reset();
    // fr: the 144 frame bits in mbelib's [8][23] layout (P25 deinterleave
    // with iW..iZ). Writes 160 samples. Returns the corrected bit count, or
    // -1 when the frame was unusable (repeat / mute applied).
    int decode(char fr[8][23], float* out160);
private:
    MbeParms cur_{}, prev_{}, prev_enh_{};
    std::minstd_rand rng_{12345};
#ifdef DIG_IMBE_TEST_RAND   // bit-exact comparison with mbelib (tests only)
    float rnd() { return static_cast<float>(rand()) / static_cast<float>(RAND_MAX); }
#else
    float rnd() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
#endif
    int decode_params(const char* d);
    void synthesize(float* out, MbeParms& cur, MbeParms& prev);
};

// mbelib loaded at run time (AMBE / AMBE+2). One AmbeStream per voice
// channel (DMR timeslot, D-STAR stream) keeps the codec state.
class MbeLib {
public:
    static MbeLib& instance();
    bool available() const { return ok_; }
    const std::string& status() const { return status_; }   // e.g. "libmbe.so.1 (mbelib 1.3.0)"
    // ambe_fr in mbelib's [4][24] layout; dstar = 3600x2400 (D-STAR), else
    // 3600x2450 (DMR). out: 160 samples at 8 kHz. Returns mbelib's errs2.
    int decode(bool dstar, char fr[4][24], void* state, float* out160) const;
    void init_state(void* state) const;
    static constexpr size_t STATE_BYTES = 3 * 4096;   // 3 mbe_parms, generously sized
private:
    MbeLib();
    bool ok_ = false;
    std::string status_;
    void (*init_)(void*, void*, void*) = nullptr;
    void (*ambe2450_)(float*, int*, int*, char*, char[4][24], char*, void*, void*, void*, int) = nullptr;
    void (*ambe2400_)(float*, int*, int*, char*, char[4][24], char*, void*, void*, void*, int) = nullptr;
};

class AmbeStream {
public:
    explicit AmbeStream(bool dstar);
    ~AmbeStream();
    void reset();
    int decode(char fr[4][24], float* out160);
private:
    bool dstar_;
    std::vector<uint8_t> state_;
};

// TETRA ACELP through the user-built ETSI codec (cdecoder | sdecoder).
class TetraCodec {
public:
    TetraCodec();
    ~TetraCodec();
    static bool available();          // both programs found
    static std::string status();
    // One TCH/S traffic slot: 432 type-4 bits (descrambled), or nullptr for
    // an all-erasure slot (flushes the pipes). Returns the decoded audio
    // that is ready (8 kHz), possibly delayed by the pipes.
    void feed_slot(const uint8_t* type4_432, std::vector<float>& out);
    void stop();
private:
    int to_cdec_ = -1, from_sdec_ = -1;
    int pid_c_ = -1, pid_s_ = -1;
    bool start();
    std::vector<uint8_t> pending_;   // blocks not yet accepted by the pipe
    std::vector<uint8_t> carry_;     // odd byte of a PCM sample
};

}  // namespace dig
