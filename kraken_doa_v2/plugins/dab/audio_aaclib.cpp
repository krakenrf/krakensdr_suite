// libfdk-aac loaded at run time (see audio_aaclib.hpp).

#include "audio_aaclib.hpp"

#include <dlfcn.h>   // Windows: dlfcn-win32 (MSYS2 package dlfcn)

#include <cstdlib>
#include <cstring>
#include <vector>

namespace dab::aac {

namespace {

// --- the parts of the FDK AAC decoder API used here (aacdecoder_lib.h,
// FDK_audio.h; values checked against libfdk-aac 2.0.2) ---
constexpr int TT_MP4_RAW = 0;                       // TRANSPORT_TYPE: raw access units
constexpr int AAC_PCM_LIMITER_ENABLE = 0x0004;      // AACDEC_PARAM
constexpr int AAC_PCM_MAX_OUTPUT_CHANNELS = 0x0012; // AACDEC_PARAM
constexpr unsigned AACDEC_CONCEAL = 1;              // aacDecoder_DecodeFrame flags
constexpr int AAC_DEC_OK = 0x0000;
constexpr int AAC_DEC_NOT_ENOUGH_BITS = 0x1002;
constexpr int AAC_DEC_DECODE_ERROR_START = 0x4000, AAC_DEC_DECODE_ERROR_END = 0x4FFF;   // output valid, concealed
constexpr int FDK_AACDEC = 3;                       // FDK_MODULE_ID of the AAC decoder

// LIB_INFO (FDK_audio.h)
struct LibInfo {
    const char* title;
    const char* build_date;
    const char* build_time;
    int module_id;          // FDK_MODULE_ID (an enum: int sized)
    int version;            // LIB_VERSION(lev0, lev1, lev2): lev0 << 24 | lev1 << 16 | lev2 << 8
    unsigned flags;
    char versionStr[32];
};
// aacDecoder_GetLibInfo fills the first free entries (module_id == FDK_NONE)
// of an array of FDK_MODULE_LAST (2.0.2: 39) entries: a generous zeroed array
constexpr int LIB_INFO_ENTRIES = 128;

using OpenFn = void* (*)(int transport, unsigned layers);
using ConfigRawFn = int (*)(void* h, unsigned char* conf[], const unsigned length[]);
using FillFn = int (*)(void* h, unsigned char* buf[], const unsigned size[], unsigned* valid);
using DecodeFn = int (*)(void* h, int16_t* pcm, int size, unsigned flags);
using CloseFn = void (*)(void* h);
using InfoFn = void* (*)(void* h);
using SetParamFn = int (*)(void* h, int param, int value);
using LibInfoFn = int (*)(LibInfo* info);

struct Lib {
    bool ok = false;
    std::string status;
    OpenFn open = nullptr;
    ConfigRawFn config_raw = nullptr;
    FillFn fill = nullptr;
    DecodeFn decode = nullptr;
    CloseFn close = nullptr;
    InfoFn info = nullptr;
    SetParamFn set_param = nullptr;

    Lib() {
        std::vector<std::string> names;
        if (const char* e = std::getenv("KRAKEN_AAC_LIB")) {
            std::string v = e;
            if (v == "none" || v == "off") {
                status = "disabled (KRAKEN_AAC_LIB=" + v + ")";
                return;
            }
            if (!v.empty()) names.push_back(v);
        }
#if defined(_WIN32)
        for (const char* n : {"libfdk-aac-2.dll", "fdk-aac.dll", "libfdk-aac.dll"})
#elif defined(__APPLE__)
        for (const char* n : {"libfdk-aac.2.dylib", "/opt/homebrew/lib/libfdk-aac.2.dylib", "/usr/local/lib/libfdk-aac.2.dylib"})
#else
        for (const char* n : {"libfdk-aac.so.2", "/usr/local/lib/libfdk-aac.so.2", "libfdk-aac.so.1"})
#endif
            names.push_back(n);
        void* h = nullptr;
        std::string used;
        for (const auto& n : names) {
            h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (h) { used = n; break; }
        }
        if (!h) {
#if defined(_WIN32)
            status = "not installed (libfdk-aac-2.dll)";
#elif defined(__APPLE__)
            status = "not installed (brew install fdk-aac)";
#else
            status = "not installed (libfdk-aac2)";
#endif
            return;
        }
        // keep only the file name in the status (a path may be long)
        std::string shown = used;
        size_t sl = shown.find_last_of("/\\");
        if (sl != std::string::npos) shown = shown.substr(sl + 1);
        open = reinterpret_cast<OpenFn>(dlsym(h, "aacDecoder_Open"));
        config_raw = reinterpret_cast<ConfigRawFn>(dlsym(h, "aacDecoder_ConfigRaw"));
        fill = reinterpret_cast<FillFn>(dlsym(h, "aacDecoder_Fill"));
        decode = reinterpret_cast<DecodeFn>(dlsym(h, "aacDecoder_DecodeFrame"));
        close = reinterpret_cast<CloseFn>(dlsym(h, "aacDecoder_Close"));
        info = reinterpret_cast<InfoFn>(dlsym(h, "aacDecoder_GetStreamInfo"));
        set_param = reinterpret_cast<SetParamFn>(dlsym(h, "aacDecoder_SetParam"));
        auto lib_info = reinterpret_cast<LibInfoFn>(dlsym(h, "aacDecoder_GetLibInfo"));
        if (!open || !config_raw || !fill || !decode || !close || !info || !set_param || !lib_info) {
            status = shown + " lacks the AAC decoder functions";
            return;
        }
        // the decoder module's version. Tested with DAB+ (960-sample frames,
        // SBR, PS): FDK AAC decoder 2.4.7 (fdk-aac 0.1.2), 2.5.17 (0.1.6),
        // 3.2.0 (2.0.2) - the API used here is the same in all of them
        std::vector<LibInfo> li(LIB_INFO_ENTRIES);
        std::memset(li.data(), 0, li.size() * sizeof(LibInfo));
        int v = -1;
        std::string vs;
        if (lib_info(li.data()) == 0) {
            for (const auto& e : li) {
                if (e.module_id == FDK_AACDEC) {
                    v = e.version;
                    char s[33];
                    std::memcpy(s, e.versionStr, 32);
                    s[32] = 0;
                    for (char* p = s; *p; p++)
                        if (static_cast<unsigned char>(*p) < 32 || static_cast<unsigned char>(*p) > 126) *p = '?';
                    vs = s;
                    break;
                }
            }
        }
        if (v < 0) {
            status = shown + " (unknown decoder version - not used)";
            return;
        }
        const int major = (v >> 24) & 0xFF, minor = (v >> 16) & 0xFF;
        if (vs.empty()) vs = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string((v >> 8) & 0xFF);
        if (major < 2) {
            status = shown + " (FDK AAC decoder " + vs + " - too old, not used)";
            return;
        }
        status = shown + " (FDK AAC decoder " + vs + ")";
        ok = true;
    }
};

Lib& lib() {
    static Lib l;
    return l;
}

}   // namespace

bool library_available() { return lib().ok; }
const std::string& library_status() { return lib().status; }

Decoder::~Decoder() { close(); }

void Decoder::close() {
    if (h_) lib().close(h_);
    h_ = nullptr;
    channels_ = rate_ = 0;
}

bool Decoder::open(const uint8_t* asc, size_t asc_len, int max_channels) {
    close();
    Lib& l = lib();
    if (!l.ok || !asc || asc_len == 0 || asc_len > 64) return false;
    h_ = l.open(TT_MP4_RAW, 1);
    if (!h_) return false;
    unsigned char conf[64];
    std::memcpy(conf, asc, asc_len);
    unsigned char* cp[1] = {conf};
    const unsigned cl[1] = {static_cast<unsigned>(asc_len)};
    if (l.config_raw(h_, cp, cl) != AAC_DEC_OK) {
        close();
        return false;
    }
    // optional: mono downmix inside the library, no limiter (its delay, its CPU)
    if (max_channels > 0) l.set_param(h_, AAC_PCM_MAX_OUTPUT_CHANNELS, max_channels);
    l.set_param(h_, AAC_PCM_LIMITER_ENABLE, 0);
    return true;
}

int Decoder::decode(const uint8_t* au, size_t n) {
    channels_ = rate_ = 0;
    if (!h_) return 0;
    Lib& l = lib();
    unsigned flags = 0;
    if (au && n > 0 && n <= 8192) {
        unsigned char* bp[1] = {const_cast<unsigned char*>(au)};
        const unsigned bs[1] = {static_cast<unsigned>(n)};
        unsigned valid = static_cast<unsigned>(n);
        if (l.fill(h_, bp, bs, &valid) != AAC_DEC_OK) flags = AACDEC_CONCEAL;
    } else {
        flags = AACDEC_CONCEAL;
    }
    // the size is given as half the buffer: safe whether a library version
    // counts it in samples or in bytes
    int err = l.decode(h_, pcm_, PCM_CAPACITY / 2, flags);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (err != AAC_DEC_OK && !(err >= AAC_DEC_DECODE_ERROR_START && err <= AAC_DEC_DECODE_ERROR_END)) return 0;
    const void* si = l.info(h_);
    if (!si) return 0;
    int f[3];   // CStreamInfo: sampleRate, frameSize, numChannels
    std::memcpy(f, si, sizeof f);
    const int rate = f[0], frame = f[1], ch = f[2];
    if (rate < 8000 || rate > 96000 || frame <= 0 || ch <= 0 || ch > 8 || frame * ch > PCM_CAPACITY / 2) return 0;
    channels_ = ch;
    rate_ = rate;
    return frame;
}

}   // namespace dab::aac
