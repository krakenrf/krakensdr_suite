#pragma once

// Shared code of the digital-radio plugins that ship with the suite (P25,
// DMR, TETRA, D-STAR, NXDN, MPT1327): the receiver-side types those decoders
// were written against (RxContext, Report, SampleBuf, SymSrc), mapped onto
// the plugin API (kp::Host), plus the front ends they share. Linked into
// every plugin as plugins/lib/build/libkrakendig.a - a plugin of your own
// can use it too (FEC: dig_fec.hpp, 4FSK sync: dig_fsk4.hpp, vocoders:
// dig_vocoder.hpp).

#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kraken_plugin.hpp"

namespace dig {

// Which protocol a call is about. Inside one plugin it only matters to code
// that handles two protocols at once (the 4FSK receiver: P25 / DMR).
enum class Mode : int { OFF = 0, AUTO = 1, P25 = 2, DMR = 3, TETRA = 4, DSTAR = 5, NXDN = 6, MPT1327 = 7 };

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Decoder options (the panel's settings). verbose comes from the host; the
// rest are the plugins' own options (Info::options).
struct Options {
    bool verbose = false;      // also log repeating broadcasts / idle / CSBK chatter
    int dmr_slot = 0;          // DMR: 0 = both, 1 / 2 = only that timeslot's calls
    int p25_nac = -1;          // P25: -1 = any; else only frames with this NAC
};

// Facts / events of the decoder -> the host
class Report {
public:
    explicit Report(kp::Host& h) : host_(h) {}
    void set(Mode, const std::string& key, const std::string& value) { host_.fact(key, value); }
    void erase(Mode, const std::string& key) { host_.fact(key, ""); }
    void event(Mode, const std::string& text, double dedup_s = 2.0) { host_.event(text, dedup_s); }
    // A position for the web UI's map (kp::Host::map_point)
    void map(Mode, const kp::MapPoint& p) { host_.map_point(p); }
    // The receiver's location, if known (kp::Host::station)
    bool station(double* lat, double* lon) const { return host_.station(lat, lon); }
private:
    kp::Host& host_;
};

struct RxContext {
    Report* report = nullptr;
    const Options* opts = nullptr;
    // A frame of this protocol passed its FEC/CRC checks
    std::function<void(Mode)> valid;
    // Carrier offset measured by a receiver (Hz, + = above the VFO centre)
    std::function<void(Mode, float)> freq_error;
    // Decoded voice, 8 kHz mono (nominal +-1)
    std::function<void(Mode, const float*, size_t)> voice;
    // Voice status for the UI ("" = no call)
    std::function<void(const std::string&)> voice_state;
    // true while someone listens - the receivers run their vocoders only then
    std::function<bool()> voice_wanted;
    // Who transmits (kp::Host::talker / talker_end). Positions are the
    // receiver's own input sample indexes since its last reset; the plugin
    // maps them to Host::time(). Unset = not reported.
    std::function<void(const std::string& id, const std::string& label, int64_t start, int64_t end)> talker;
    std::function<void(int64_t at)> talker_end;
};

// RxContext + Report + Options wired to a kp::Host
class Bridge {
public:
    explicit Bridge(kp::Host& h) : report(h) {
        ctx.report = &report;
        ctx.opts = &opts;
        ctx.valid = [&h](Mode) { h.valid(); };
        ctx.freq_error = [&h](Mode, float hz) { h.freq_error(hz); };
        ctx.voice = [&h](Mode, const float* x, size_t n) { h.audio(x, n); };
        ctx.voice_state = [&h](const std::string& s) { h.voice_state(s); };
        ctx.voice_wanted = [&h] { return h.voice_wanted(); };
    }
    // "verbose" = 0|1, "slot" = 0|1|2 (DMR), "nac" = 3 hex digits or "" / "any" (P25)
    void option(const std::string& key, const std::string& value);
    Report report;
    Options opts;
    RxContext ctx;
};

// Sample history with absolute indexing (only the newest few hundred ms kept)
class SampleBuf {
public:
    void push(float v) { buf_.push_back(v); }
    float at(int64_t abs) const { return buf_[static_cast<size_t>(abs - base_)]; }
    int64_t begin() const { return base_; }
    int64_t end() const { return base_ + static_cast<int64_t>(buf_.size()); }
    bool has(int64_t abs) const { return abs >= base_ && abs < end(); }
    void trim_before(int64_t abs);
private:
    std::vector<float> buf_;
    int64_t base_ = 0;
};

// Soft symbols around a detected sync: symbol k (0 = last sync symbol,
// negative = before it) sampled at sync_end + k*sps, normalized so the
// nominal levels are +-1 / +-3 (or +-1 for binary).
struct SymSrc {
    const SampleBuf* buf = nullptr;
    int64_t sync_end = 0;
    int sps = 10;
    float scale = 1.0f;   // Hz per level unit
    float center = 0.0f;  // Hz
    float pol = 1.0f;
    bool has(int k) const { return buf->has(sync_end + static_cast<int64_t>(k) * sps); }
    float sym(int k) const {
        return pol * (buf->at(sync_end + static_cast<int64_t>(k) * sps) - center) / scale;
    }
    // 4FSK dibit: +3 -> 01, +1 -> 00, -1 -> 10, -3 -> 11
    static uint8_t dibit(float s) { return s >= 2.0f ? 1 : (s >= 0.0f ? 0 : (s >= -2.0f ? 2 : 3)); }
};

// --- Front ends -------------------------------------------------------------
// 48 kHz complex -> 12.5 kHz channel filter -> FM discriminator (Hz): the
// input of the 4FSK / GMSK / FFSK receivers
class FmFrontEnd {
public:
    FmFrontEnd();
    ~FmFrontEnd();
    FmFrontEnd(const FmFrontEnd&) = delete;
    FmFrontEnd& operator=(const FmFrontEnd&) = delete;
    // returns the discriminator output for x (same length)
    const std::vector<float>& process(const kp::cf* x, size_t n);
    void reset();
    static constexpr double RATE = 48000;
private:
    void* lp_ = nullptr;           // liquid firfilt_crcf
    kp::cf prev_{1, 0};
    std::vector<float> out_;
};

// 72 kHz complex -> RRC 0.35 matched filter (TETRA, 4 samples/symbol)
class RrcFrontEnd {
public:
    RrcFrontEnd(unsigned sps, unsigned span, float beta);
    ~RrcFrontEnd();
    RrcFrontEnd(const RrcFrontEnd&) = delete;
    RrcFrontEnd& operator=(const RrcFrontEnd&) = delete;
    const std::vector<kp::cf>& process(const kp::cf* x, size_t n);
    void reset();
private:
    void* f_ = nullptr;            // liquid firfilt_crcf
    std::vector<float> taps_;
    std::vector<kp::cf> out_;
};

}  // namespace dig
