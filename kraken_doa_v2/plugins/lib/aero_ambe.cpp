// The Aero AMBE codec, loaded at run time (aero_ambe.hpp).
//
// The 96 frame bits go to mbelib's [6][24] code-vector layout through the
// frame's de-interleave table below, which is the one in libaeroambe's
// aeroambe.h (github.com/jontio/libaeroambe, libaeroambe/LICENSE):
//
//   The MIT License (MIT)
//
//   Copyright (c) 2018 libaeroambe team
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// It is a bit order (which received bit fills which code-vector
// position), no part of the codec itself: the codec - parameter
// quantisation, FEC tables, synthesis - stays in the user's library.

#include "aero_ambe.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace dig {

namespace {

constexpr float OUT_SCALE = 7.0f / 32768.0f;   // as MbeLib: mbelib's short gain, then to +-1

// received bit i -> ambe_fr[W[i]][X[i]] (libaeroambe aeroambe.h rW / rX)
constexpr unsigned char W[96] = {0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0, 1, 1, 2, 3, 4, 5, 0, 0,
                                 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2, 2, 3, 4, 5, 0, 0, 1, 2,
                                 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 3, 4, 5, 0, 0, 1, 2, 3, 4,
                                 4, 5, 0, 0, 1, 2, 3, 4, 5, 5, 0, 0, 1, 2, 3, 4, 5, 5};
constexpr unsigned char X[96] = {23, 11, 14, 2, 5, 8, 9, 11, 22, 10, 13, 1, 4, 7, 8, 10, 21, 9, 12, 0,
                                 3, 6, 7, 9, 20, 8, 11, 14, 2, 5, 6, 8, 19, 7, 10, 13, 1, 4, 5, 7,
                                 18, 6, 9, 12, 0, 3, 4, 6, 17, 5, 8, 11, 14, 2, 3, 5, 16, 4, 7, 10,
                                 13, 1, 2, 4, 15, 3, 6, 9, 12, 0, 1, 3, 14, 2, 5, 8, 11, 12, 0, 2,
                                 13, 1, 4, 7, 10, 11, 13, 1, 12, 0, 3, 6, 9, 10, 12, 0};

}  // namespace

AeroAmbeLib& AeroAmbeLib::instance() {
    static AeroAmbeLib m;
    return m;
}

AeroAmbeLib::AeroAmbeLib() {
    std::vector<std::string> names;
    for (const char* env : {"KRAKEN_AERO_AMBE", "KRAKEN_MBELIB"}) {
        if (const char* e = std::getenv(env)) {
            const std::string v = e;
            if (v == "none" || v == "off") {
                if (std::string(env) == "KRAKEN_AERO_AMBE") {
                    status_ = "disabled (KRAKEN_AERO_AMBE=" + v + ")";
                    return;
                }
                continue;   // KRAKEN_MBELIB=none only disables the other plugins' AMBE
            }
            names.push_back(v);
        }
    }
#ifdef _WIN32
    for (const char* n : {"libmbe.dll", "mbe.dll", "libmbe-1.dll"})
#else
    for (const char* n : {"libmbe.so.1", "libmbe.so", "/usr/local/lib/libmbe.so.1", "/usr/local/lib/libmbe.so"})
#endif
        names.push_back(n);
    std::string found_plain;
    for (const auto& n : names) {
        void* h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        auto init = reinterpret_cast<decltype(init_)>(dlsym(h, "mbe_initMbeParms"));
        auto a48 = reinterpret_cast<decltype(ambe4800_)>(dlsym(h, "mbe_processAmbe4800x3600Framef"));
        if (!init || !a48) {   // an mbelib without the Aero codec: try the next
            if (found_plain.empty()) found_plain = n;
            dlclose(h);
            continue;
        }
        init_ = init;
        ambe4800_ = a48;
        auto ver = reinterpret_cast<void (*)(char*)>(dlsym(h, "mbe_printVersion"));
        char v[64] = "?";
        if (ver) ver(v);
        status_ = n + " (mbelib " + v + ", with the 4800x3600 codec)";
        ok_ = true;
        return;
    }
    status_ = found_plain.empty() ? "not installed (see README: Digital voice codecs)"
                                  : "not installed (" + found_plain + " lacks the Aero AMBE 4800x3600 functions - see README: Digital voice codecs)";
}

void AeroAmbeLib::init_state(void* state) const {
    if (!ok_) return;
    std::memset(state, 0, STATE_BYTES);
    uint8_t* s = static_cast<uint8_t*>(state);
    init_(s, s + 4096, s + 8192);
}

int AeroAmbeLib::decode(const uint8_t* bits96, void* state, float* out) const {
    if (!ok_) return -1;
    char fr[6][24];
    std::memset(fr, 0, sizeof fr);
    for (int i = 0; i < 96; i++) fr[W[i]][X[i]] = static_cast<char>(bits96[i] & 1);
    uint8_t* s = static_cast<uint8_t*>(state);
    int errs = 0, errs2 = 0;
    char err_str[96];
    char ambe_d[72];
    ambe4800_(out, &errs, &errs2, err_str, fr, ambe_d, s, s + 4096, s + 8192, 3);
    for (int n = 0; n < 160; n++) out[n] *= OUT_SCALE;
    return errs2;
}

AeroAmbeStream::AeroAmbeStream() : state_(AeroAmbeLib::STATE_BYTES) { reset(); }

void AeroAmbeStream::reset() {
    AeroAmbeLib::instance().init_state(state_.data());
    lim_.reset();
}

int AeroAmbeStream::decode(const uint8_t* bits96, float* out) {
    const int r = AeroAmbeLib::instance().decode(bits96, state_.data(), out);
    if (r >= 0) lim_.process(out, 160);
    return r;
}

}  // namespace dig
