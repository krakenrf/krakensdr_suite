#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The bit-row helpers, checks and slicers declared in this
// file (bits.cpp) follow rtl_433's (https://github.com/merbanan/rtl_433,
// GPL-2.0-or-later), re-implemented in C++ for the KrakenSDR plugin API
// (2026):
//   src/bitbuffer.c - Copyright (C) 2015 Tommy Vestermark
//   src/bit_util.c - Copyright (C) 2015 Tommy Vestermark
//   src/pulse_slicer.c - Copyright (C) 2015 Tommy Vestermark

// ISM band device decoder: shared types.
//
//   front end (frontend.cpp)  bursts -> OOK / FSK pulse trains (Burst)
//   slicers  (bits.cpp)       pulse trains -> bit rows (Rows), cached per
//                             parameter set in a Pkt
//   protocols (p_*.cpp)       bit rows -> messages (Msg), each registered
//                             with ISM_PROTOCOL(...)
//   decoder.cpp               the kp::Decoder: devices, table, events, DoA
//
// Bit conventions follow the common ones of the ISM decoding world (and
// rtl_433's, so its protocol notes read the same): OOK pulse = carrier on;
// FSK pulse = the HIGHER of the two frequencies; PWM short pulse = 1; PPM
// long gap = 1; Manchester (zero-bit slicer) mid-bit high->low = 1 with a 0
// prepended to every row; bits MSB first into bytes in row order.

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ism {

using cf = std::complex<float>;

constexpr double FS = 240000.0;                   // input rate (VFO "240 kHz": no resampling)
inline int us2s(double us) { return static_cast<int>(std::lround(us * FS * 1e-6)); }
inline double s2us(double s) { return s * 1e6 / FS; }

// ---------------------------------------------------------------- pulses ---
struct Pulse {
    int64_t t = 0;      // absolute input sample where the pulse starts
    int32_t on = 0;     // pulse length (samples); 0 = the train starts with a gap
    int32_t off = 0;    // the gap after it (samples)
};

// One burst (package): everything between the first and the last sign of
// the signal, with the gaps of OOK coding inside it
struct Burst {
    int64_t a = 0, b = 0;           // absolute input samples demodulated
    double fc = 0;                  // carrier (OOK) / centre of the two tones (FSK), Hz in the input
    double bw = 0;                  // occupied bandwidth (Hz)
    double snr_db = 0;              // signal amplitude over the noise in the demodulation filter
    double fsk_sep = 0;             // FSK: distance of the two tones (Hz), 0 = not seen
    std::vector<Pulse> ook, fsk;    // the two pulse trains
    int antennas = 1;               // combined from this many (FrontEnd::push_multi)
    double comb_db = 0;             // their combined power over antenna 0's (dB)
};

// ------------------------------------------------------------------ bits ---
struct Row {
    std::vector<uint8_t> b;         // MSB first
    int n = 0;                      // bits
    int64_t t0 = 0, t1 = 0;         // absolute samples: first pulse start .. last pulse end
    int msg = 0;                    // its message: rows split by a gap > reset_limit are in different ones

    void push(int bit) {
        if ((n & 7) == 0) b.push_back(0);
        if (bit) b[static_cast<size_t>(n >> 3)] |= static_cast<uint8_t>(0x80 >> (n & 7));
        n++;
    }
    int bit(int i) const { return i >= 0 && i < n ? (b[static_cast<size_t>(i >> 3)] >> (7 - (i & 7))) & 1 : 0; }
    // n_bits (<= 64) bits from bit position pos, MSB first; past the end = 0
    uint64_t get(int pos, int nbits) const {
        uint64_t v = 0;
        for (int i = 0; i < nbits; i++) v = (v << 1) | static_cast<uint64_t>(bit(pos + i));
        return v;
    }
    uint8_t byte(int pos) const { return static_cast<uint8_t>(get(pos, 8)); }
    // nbytes bytes from bit position pos
    std::vector<uint8_t> bytes(int pos, int nbytes) const {
        std::vector<uint8_t> v(static_cast<size_t>(std::max(0, nbytes)));
        for (int i = 0; i < nbytes; i++) v[static_cast<size_t>(i)] = byte(pos + 8 * i);
        return v;
    }
    // first position >= from where the pattern (MSB first) starts, -1 = none
    int find(const uint8_t* pat, int patbits, int from = 0) const;
    int find(uint64_t pat, int patbits, int from = 0) const;
    Row inverted() const;
    // Manchester decoding of pairs from pos: "10" -> 1, "01" -> 0 (one_is_10),
    // or the opposite; stops at the first "00" / "11" or after max bits
    Row manchester(int pos, int max = 100000, bool one_is_10 = false) const;
    Row sub(int pos, int nbits) const;
    std::string hex() const;
    bool same(const Row& o) const { return n == o.n && b == o.b; }
};

struct Rows {
    std::vector<Row> r;
    bool empty() const { return r.empty(); }
    size_t size() const { return r.size(); }
    const Row& operator[](size_t i) const { return r[i]; }
    // index of a row of >= min_bits bits that occurs >= min_count times, -1 = none
    int repeated(int min_count, int min_bits) const;
    int count_same(size_t i) const;
    // the rows message by message (as a decoder is called per message, i.e.
    // per run of rows not separated by a gap > reset_limit)
    std::vector<Rows> split() const;
};

// ---------------------------------------------------------------- checks ---
uint8_t crc8(const uint8_t* p, int n, uint8_t poly, uint8_t init);           // MSB first
uint8_t crc8le(const uint8_t* p, int n, uint8_t poly, uint8_t init);         // reflected (poly given normal)
uint16_t crc16(const uint8_t* p, int n, uint16_t poly, uint16_t init);       // MSB first
uint16_t crc16lsb(const uint8_t* p, int n, uint16_t poly, uint16_t init);    // reflected (poly given reflected)
// LFSR digests ("Galois" key stream): message bits MSB first, the key
// shifted right per bit (xor gen when a 1 drops out)
uint8_t lfsr_digest8(const uint8_t* p, int n, uint8_t gen, uint8_t key);
// message bits LSB first, the key shifted left (xor gen when bit 7 drops out)
uint8_t lfsr_digest8_reflect(const uint8_t* p, int n, uint8_t gen, uint8_t key);
uint16_t lfsr_digest16(const uint8_t* p, int n, uint16_t gen, uint16_t key);
int add_bytes(const uint8_t* p, int n);
int xor_bytes(const uint8_t* p, int n);
int add_nibbles(const uint8_t* p, int n);
int parity8(uint8_t v);
int parity_bytes(const uint8_t* p, int n);
uint8_t reverse8(uint8_t v);

// ---------------------------------------------------------------- slicing ---
enum class Mod { OOK, FSK };

// A package's slicings, made on demand and kept while it is decoded
class Pkt {
public:
    explicit Pkt(const Burst& b) : burst(b) {}
    const Burst& burst;
    const std::vector<Pulse>& pulses(Mod m) const { return m == Mod::FSK ? burst.fsk : burst.ook; }

    // The slicers take the parameters the usual way (microseconds): short /
    // long width, gap_limit (a gap longer than it starts a new row; 0 = none),
    // reset_limit (a gap longer than it ends the message: the rows after it
    // belong to the next message - see Rows::split), sync width, tolerance
    // (0 = thresholds at the midpoints). One burst can hold several messages.
    //
    // PWM: pulse ~short = 1, ~long = 0, ~sync = row break
    const Rows& pwm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int sync_us = 0, int tol_us = 0);
    // PPM: the gap after each pulse ~short = 0, ~long = 1; gap_limit <= gap <
    // reset_limit = a new row (also an empty one); the last pulse gives no bit
    const Rows& ppm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int sync_us = 0, int tol_us = 0);
    // PCM: NRZ when short == long (every pulse = round(w / bit) ones, every
    // gap zeros), RZ when short < long (a ~short pulse in a ~long period = 1,
    // empty periods 0). A gap adds at most gap_limit / long zeros. The bit
    // width is refined from a run of single bits (a preamble) or from all
    // in-tolerance widths. RZ: a pulse off by > tol drops the message so far
    const Rows& pcm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int tol_us = 0);
    // Manchester with mid-bit edges (zero-bit slicer): half bit ~ short,
    // high->low = 1, low->high = 0, a 0 prepended to every row; with tol a
    // pulse / gap outside [short - tol, 2 short + tol] ends the row (the next
    // row is started at once and the gap sliced into it)
    const Rows& manchester_zerobit(Mod m, int short_us, int reset_us, int tol_us = 0);
    // Differential Manchester (bi-phase) as one stream of pulse and gap
    // durations: ~short + ~short = 1 (a mid-cell transition), ~long = 0; a
    // lone short symbol starts a new row; >= reset - tol ends the message
    const Rows& dmc(Mod m, int short_us, int long_us, int reset_us, int tol_us = 0);
    // PIWM: every pulse AND every gap is a bit: ~short = 1, ~long = 0, other
    // durations < reset_limit start a new row
    const Rows& piwm(Mod m, int short_us, int long_us, int reset_us, int tol_us = 0);

private:
    struct Key {
        int kind, mod, a, b, c, d, e, f;
        bool operator==(const Key& o) const {
            return kind == o.kind && mod == o.mod && a == o.a && b == o.b && c == o.c && d == o.d && e == o.e &&
                   f == o.f;
        }
    };
    std::vector<std::pair<Key, Rows>> cache_;
    const Rows* find(const Key& k) const {
        for (const auto& kv : cache_)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    const Rows& put(const Key& k, Rows&& r) {
        cache_.emplace_back(k, std::move(r));
        return cache_.back().second;
    }
};

// --------------------------------------------------------------- messages ---
struct Val {
    std::string key;
    double num = NAN;
    std::string str;
    bool is_num = false;
    const char* fmt = "%g";
};

enum class Kind { WEATHER, TPMS, REMOTE, SECURITY, ENERGY, OTHER, UNKNOWN };

struct Msg {
    std::string model;      // device model (rtl_433 naming where it has one)
    std::string id;         // device id as shown ("" = none)
    Kind kind = Kind::OTHER;
    std::vector<Val> f;     // fields in rtl_433's key names, in output order
    int64_t t0 = 0, t1 = 0; // the confirmed rows' samples
    std::string event;      // something that happened (button, open, alarm) - "" = a reading
    bool alarm = false;     // event is unusual / alarming ("⚠")
    const char* check = ""; // what proved it: "CRC", "CHECKSUM", "DIGEST", "PARITY", "REPEAT"

    Msg& num(const char* k, double v, const char* fmt = "%g") {
        Val x;
        x.key = k;
        x.num = v;
        x.is_num = true;
        x.fmt = fmt;
        f.push_back(std::move(x));
        return *this;
    }
    Msg& str(const char* k, const std::string& v) {
        Val x;
        x.key = k;
        x.str = v;
        f.push_back(std::move(x));
        return *this;
    }
    const Val* get(const char* k) const {
        for (const auto& v : f)
            if (v.key == k) return &v;
        return nullptr;
    }
    double getn(const char* k) const {
        const Val* v = get(k);
        return v && v->is_num ? v->num : NAN;
    }
    std::string gets(const char* k) const {
        const Val* v = get(k);
        if (!v) return "";
        if (v->is_num) {
            char b[48];
            snprintf(b, sizeof b, v->fmt, v->num);
            return b;
        }
        return v->str;
    }
    std::string json() const;   // one line, rtl_433 style
};

// Message builder helpers used by the protocols
inline std::string hexs(uint64_t v, int digits) {
    char b[24];
    snprintf(b, sizeof b, "%0*llx", digits, static_cast<unsigned long long>(v));
    return b;
}
inline std::string HEXs(uint64_t v, int digits) {
    char b[24];
    snprintf(b, sizeof b, "%0*llX", digits, static_cast<unsigned long long>(v));
    return b;
}

// ----------------------------------------------------------- the registry ---
// Strength: how well a message is proven. Within a package the strong
// decoders run first; the weak ones (repeat-only, no checksum) only when
// nothing stronger decoded.
enum class Strength { STRONG, MEDIUM, WEAK };

using DecodeFn = void (*)(Pkt& pk, std::vector<Msg>& out);
struct Protocol {
    const char* name;
    Strength strength;
    DecodeFn fn;
};
std::vector<Protocol>& registry();
struct Register {
    Register(const char* name, Strength s, DecodeFn fn) { registry().push_back({name, s, fn}); }
};
#define ISM_CAT2(a, b) a##b
#define ISM_CAT(a, b) ISM_CAT2(a, b)
#define ISM_PROTOCOL(NAME, STRENGTH, FN) static ::ism::Register ISM_CAT(ism_reg_, __LINE__)(NAME, STRENGTH, FN)

// a message from a row: fills t0 / t1 with the row's span
inline Msg& emit(std::vector<Msg>& out, const Row& r, const char* model, Kind k, const char* check) {
    out.emplace_back();
    Msg& m = out.back();
    m.model = model;
    m.kind = k;
    m.t0 = r.t0;
    m.t1 = r.t1;
    m.check = check;
    return m;
}

// Offline debugging: with ISM_ROWS=<name> in the environment, a protocol
// calling dbg_rows(name, rows) prints those rows (message, bits, hex) to stderr
void dbg_rows(const char* name, const Rows& rows);

// Unknown signals (generic.cpp): a package no protocol decoded, analysed
// for a repeating code; true = *m holds an "unknown device"
bool analyse_unknown(Pkt& pk, Msg* m);

}  // namespace ism
