#pragma once
// The AAC decoder for DAB+ audio: Fraunhofer FDK AAC (libfdk-aac), a system
// library the USER installs - never shipped, vendored or linked. It is
// loaded at run time with dlopen at first use (Windows: dlfcn-win32's
// LoadLibrary), like the AMBE codec (dig_vocoder.cpp MbeLib):
//
//   $KRAKEN_AAC_LIB          a file name or path tried first ("none" / "off"
//                            disables the library)
//   Linux / Raspberry Pi OS  libfdk-aac.so.2 (Debian / Raspberry Pi OS package
//                            libfdk-aac2, non-free), /usr/local/lib/libfdk-aac.so.2,
//                            libfdk-aac.so.1 (older releases: libfdk-aac1)
//   Windows                  libfdk-aac-2.dll (MSYS2 package mingw-w64-ucrt-
//                            x86_64-fdk-aac) next to the program or on the PATH
//   macOS                    libfdk-aac.2.dylib (Homebrew fdk-aac)
//
// Only the few functions, constants and stream-info fields used here are
// declared, from the library's public API documentation (aacdecoder_lib.h),
// and checked against libfdk-aac 2.0.2 (decoding also tested with 0.1.6 and
// 0.1.2): the stream info is read through its
// first three int fields only (sampleRate, frameSize, numChannels), the
// decoder module version through aacDecoder_GetLibInfo.
//
// Self-contained (no other plugin file is needed), so that other programs
// (the suite's codec check) can compile it too.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dab::aac {

// Loads the library on the first call (thread-safe). The status text: e.g.
// "libfdk-aac.so.2 (FDK AAC decoder 3.2.0)", "not installed (libfdk-aac2)",
// "disabled (KRAKEN_AAC_LIB=none)".
bool library_available();
const std::string& library_status();

// One decoder instance (one DAB+ sub-channel)
class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // (Re)opens for raw access units described by an AudioSpecificConfig.
    // max_channels = 1 asks the library to downmix to mono itself.
    bool open(const uint8_t* asc, size_t asc_len, int max_channels);
    void close();
    bool is_open() const { return h_ != nullptr; }

    // Decodes one access unit, or conceals a lost one (au == nullptr).
    // Returns the samples per channel now in pcm() (0: no output).
    int decode(const uint8_t* au, size_t n);
    const int16_t* pcm() const { return pcm_; }
    int channels() const { return channels_; }
    int sample_rate() const { return rate_; }

    static constexpr int PCM_CAPACITY = 16384;   // int16 samples in pcm_

private:
    void* h_ = nullptr;
    int channels_ = 0, rate_ = 0;
    int16_t pcm_[PCM_CAPACITY];
};

}   // namespace dab::aac
