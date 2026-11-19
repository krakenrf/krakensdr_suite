// dab_gen audio payloads: a minimal DAB MPEG Layer II encoder (TS 103 466),
// a minimal AAC-LC (960) encoder in DAB+ audio super frames (TS 102 563) and
// the PAD / dynamic label channel (EN 300 401 clause 7.4). Written from the
// standards; the AAC Huffman tables come from vo-aacenc (Apache-2.0, see
// dab_gen_aac_tables.hpp). Part of plugins/dab/tools/dab_gen.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "dab_gen_aac_tables.hpp"
#include "dab_gen_tables.hpp"

namespace dabgen {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- bits / CRCs
struct BitWriter {
    std::vector<uint8_t> bytes;
    int64_t nbits = 0;
    void put(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) {
            if ((nbits & 7) == 0) bytes.push_back(0);
            if ((v >> i) & 1u) bytes.back() |= static_cast<uint8_t>(0x80u >> (nbits & 7));
            nbits++;
        }
    }
    void align() {
        while (nbits & 7) put(0, 1);
    }
    int bit(int64_t i) const { return (bytes[static_cast<size_t>(i >> 3)] >> (7 - (i & 7))) & 1; }
};

// MSb-first CRC of `width` bits over a bit range of a BitWriter (EN 300 401
// annex E shift register): init, generator without the x^width term
inline uint32_t crc_bits(const BitWriter& b, int64_t from, int64_t to, uint32_t poly, int width, uint32_t init) {
    uint32_t r = init;
    const uint32_t top = 1u << (width - 1), mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
    for (int64_t i = from; i < to; i++) {
        const uint32_t fb = ((r & top) ? 1u : 0u) ^ static_cast<uint32_t>(b.bit(i));
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return r;
}
inline uint32_t crc_bytes(const uint8_t* p, size_t n, uint32_t poly, int width, uint32_t init) {
    uint32_t r = init;
    const uint32_t top = 1u << (width - 1), mask = (1u << width) - 1;
    for (size_t k = 0; k < n; k++)
        for (int i = 7; i >= 0; i--) {
            const uint32_t fb = ((r & top) ? 1u : 0u) ^ ((p[k] >> i) & 1u);
            r = (r << 1) & mask;
            if (fb) r ^= poly;
        }
    return r;
}
// G(x) = x^16 + x^12 + x^5 + 1, registers initialised to 1, complemented (FIB, PAD data groups, AU CRC)
inline uint16_t crc16_ccitt_inv(const uint8_t* p, size_t n) {
    return static_cast<uint16_t>(~crc_bytes(p, n, 0x1021, 16, 0xFFFF) & 0xFFFF);
}

// ---------------------------------------------------------------- PAD / DLS
// One PAD slot = one DAB audio frame or DAB+ AU: a 4-byte short X-PAD field in
// LOGICAL order (contents indicator first) and the F-PAD CI flag.
struct PadSlot {
    uint8_t xpad[4] = {0, 0, 0, 0};
    bool ci = true;
};

// The X-PAD data groups of a dynamic label message (EN 300 401 7.4.5.2):
// segments of <= 16 bytes, prefix (T, First/Last, C = 0, Length, Charset or
// SegNum, text control 0), character field, CRC
inline std::vector<std::vector<uint8_t>> dls_groups(const std::string& text, int charset, int toggle) {
    std::vector<std::vector<uint8_t>> g;
    const size_t n = std::min<size_t>(text.size(), 128);
    const size_t nseg = std::max<size_t>(1, (n + 15) / 16);
    for (size_t s = 0; s < nseg; s++) {
        const size_t a = s * 16, len = std::min<size_t>(16, n - a);
        if (len == 0) break;
        std::vector<uint8_t> d;
        const int first = s == 0, last = s + 1 == nseg;
        d.push_back(static_cast<uint8_t>((toggle << 7) | (first << 6) | (last << 5) | (0 << 4) | (len - 1)));
        d.push_back(first ? static_cast<uint8_t>((charset & 15) << 4) : static_cast<uint8_t>((s & 7) << 4));
        for (size_t k = 0; k < len; k++) d.push_back(static_cast<uint8_t>(text[a + k]));
        const uint16_t c = crc16_ccitt_inv(d.data(), d.size());
        d.push_back(static_cast<uint8_t>(c >> 8));
        d.push_back(static_cast<uint8_t>(c & 0xFF));
        g.push_back(d);
    }
    return g;
}

// Short X-PAD schedule over `nslots` slots (the whole loop): each data group
// starts with a contents indicator (application type 2 = DL segment, start of
// X-PAD data group) + 3 bytes, continues 4 bytes per slot without CI, the last
// sub-field zero padded; a group is only started when it fits before the end
// of the loop, so the looped file never cuts one. Idle slots: CI = end marker.
inline std::vector<PadSlot> dls_schedule(const std::string& text, int charset, int64_t nslots) {
    std::vector<PadSlot> out(static_cast<size_t>(nslots));
    if (text.empty()) return out;
    const auto groups = dls_groups(text, charset, 0);
    size_t gi = 0;
    int64_t s = 0;
    while (s < nslots) {
        const auto& g = groups[gi];
        const int64_t need = 1 + (static_cast<int64_t>(g.size()) - 3 + 3) / 4;
        if (s + need > nslots) break;   // rest idle
        size_t p = 0;
        PadSlot& a = out[static_cast<size_t>(s++)];
        a.ci = true;
        a.xpad[0] = 2;   // Rfu 000, AppTy 2
        for (int k = 1; k < 4; k++) a.xpad[k] = p < g.size() ? g[p++] : 0;
        while (p < g.size()) {
            PadSlot& b = out[static_cast<size_t>(s++)];
            b.ci = false;
            for (int k = 0; k < 4; k++) b.xpad[k] = p < g.size() ? g[p++] : 0;
        }
        gi = (gi + 1) % groups.size();
    }
    return out;
}

// X-PAD (byte order reversed for transmission) + F-PAD bytes L-1, L:
// F-PAD type 00, X-PAD Ind 01 (short), Byte L indicator 0000; Byte L data 0,
// CI flag, Z = 0
inline void pad_bytes(const PadSlot& s, uint8_t* xpad_tx4, uint8_t* fpad2) {
    for (int k = 0; k < 4; k++) xpad_tx4[k] = s.xpad[3 - k];
    fpad2[0] = 0x10;
    fpad2[1] = s.ci ? 0x02 : 0x00;
}

// ---------------------------------------------------------------- Layer II
struct Mp2Encoder {
    int fs = 48000, kbps = 128;
    double tone_hz = 0;     // snapped to the subband centre
    int sb = 0, alloc_index = 0, nlevels = 0, scf_index = 0;
    const int (*table)[17] = nullptr;
    int sblimit = 0, nscfcrc = 4;
    int frame_bytes = 0;
    double amp = 0.35;      // subband sample amplitude (full scale 1)

    void init(int fs_, int kbps_, double tone, double amp_) {
        fs = fs_;
        kbps = kbps_;
        amp = amp_;
        if (fs == 48000) {
            static const int ok[] = {32, 48, 56, 64, 80, 96, 112, 128, 160, 192};
            if (std::find(std::begin(ok), std::end(ok), kbps) == std::end(ok))
                throw std::runtime_error("Layer II 48 kHz mono: bit rate must be 32..192 kbit/s (TS 103 466 table 12)");
            table = (kbps <= 48) ? kAlloc48Low : kAlloc48High;
            nscfcrc = kbps >= 56 ? 4 : 2;
            frame_bytes = kbps * 3;   // 24 ms
        } else if (fs == 24000) {
            static const int ok[] = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
            if (std::find(std::begin(ok), std::end(ok), kbps) == std::end(ok))
                throw std::runtime_error("Layer II 24 kHz: bit rate must be 8..160 kbit/s (TS 103 466 table 11)");
            table = kAlloc24;
            nscfcrc = 4;
            frame_bytes = kbps * 6;   // 48 ms
        } else
            throw std::runtime_error("Layer II: 48000 or 24000 Hz");
        sblimit = 0;
        for (int s = 0; s < 32; s++)
            if (table[s][0] > 0) sblimit = s + 1;
        const double w = fs / 64.0;
        sb = std::clamp(static_cast<int>(std::floor(tone / w)), 0, sblimit - 1);
        tone_hz = (sb + 0.5) * w;
        alloc_index = (1 << table[sb][0]) - 1;
        nlevels = table[sb][alloc_index];
        // subband samples +-amp/sqrt2 (a quarter-rate cosine at 45 deg)
        const double mx = amp * std::sqrt(0.5);
        scf_index = 0;
        for (int i = 62; i >= 0; i--)
            if (kScf[i] > mx) {
                scf_index = i;
                break;
            }
    }
    static int bitrate_index(int fs, int kbps) {
        static const int r48[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
        static const int r24[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        const int* r = fs == 48000 ? r48 : r24;
        for (int i = 1; i < 15; i++)
            if (r[i] == kbps) return i;
        return 0;
    }
    bool grouped() const { return nlevels == 3 || nlevels == 5 || nlevels == 9; }
    int code_bits() const {
        if (nlevels == 3) return 5;
        if (nlevels == 5) return 7;
        if (nlevels == 9) return 10;
        int b = 0;
        while ((1 << b) < nlevels + 1) b++;
        return b;
    }
    // one sample's quantizer index (TS 103 466 5.2.8: A X + B, n MSbs, MSb inverted)
    int quant(double x) const {
        double A = 0, B = 0;
        for (const auto& q : kQuantAB)
            if (static_cast<int>(q[0]) == nlevels) {
                A = q[1];
                B = q[2];
            }
        int n = 0;
        while ((1 << n) < nlevels) n++;
        const double v = A * x + B;
        long c = static_cast<long>(std::floor(v * static_cast<double>(1L << (n - 1)))) + (1L << (n - 1));
        return static_cast<int>(std::clamp<long>(c, 0, nlevels - 1));
    }
    // one DAB audio frame with the given PAD (X-PAD in transmission order)
    std::vector<uint8_t> frame(const uint8_t* xpad_tx, int nxpad, const uint8_t* fpad2) const {
        BitWriter b;
        b.put(0xFFF, 12);
        b.put(fs == 48000 ? 1 : 0, 1);
        b.put(2, 2);   // Layer II
        b.put(0, 1);   // protection_bit 0 = CRC present
        b.put(static_cast<uint64_t>(bitrate_index(fs, kbps)), 4);
        b.put(1, 2);   // 48 / 24 kHz
        b.put(0, 1);   // padding
        b.put(0, 1);   // private
        b.put(3, 2);   // single_channel
        b.put(0, 2);   // mode_extension
        b.put(0, 1);   // copyright
        b.put(1, 1);   // original
        b.put(0, 2);   // no emphasis
        const int64_t crc_pos = b.nbits;
        b.put(0, 16);
        const int64_t prot_from = b.nbits;
        for (int s = 0; s < sblimit; s++) b.put(s == sb ? static_cast<uint64_t>(alloc_index) : 0, table[s][0]);
        b.put(2, 2);   // scfsi: one scale factor for all three parts
        const int64_t prot_to = b.nbits;
        const int64_t scf_pos = b.nbits;
        b.put(static_cast<uint64_t>(scf_index), 6);
        const double sc = kScf[scf_index];
        const int cb = code_bits();
        for (int gr = 0; gr < 12; gr++) {
            int q[3];
            for (int s = 0; s < 3; s++) {
                const int j = gr * 3 + s;   // 36 per frame: a period-4 pattern, every frame identical
                const double v = amp * std::cos(kPi / 2 * j + kPi / 4) / sc;
                q[s] = quant(v);
            }
            if (grouped()) b.put(static_cast<uint64_t>(q[0] + nlevels * q[1] + nlevels * nlevels * q[2]), cb);
            else
                for (int s = 0; s < 3; s++) b.put(static_cast<uint64_t>(q[s]), cb);
        }
        // header CRC: header bits 16..31 + bit allocation + scfsi (G = x16 + x15 + x2 + 1, init all ones)
        const uint32_t crc = [&] {
            BitWriter t;
            for (int64_t i = 16; i < 32; i++) t.put(static_cast<uint64_t>(b.bit(i)), 1);
            for (int64_t i = prot_from; i < prot_to; i++) t.put(static_cast<uint64_t>(b.bit(i)), 1);
            return crc_bits(t, 0, t.nbits, 0x8005, 16, 0xFFFF);
        }();
        // ScF-CRC (for the following frame - every frame carries the same scale
        // factors): CRC-8 x8 + x4 + x3 + x2 + 1, init 0, over the 3 MSbs of the
        // scale factors of each subband group
        auto group_of = [&](int s) { return s < 4 ? 0 : s < 8 ? 1 : s < 16 ? 2 : 3; };
        uint8_t scfcrc[4] = {0, 0, 0, 0};
        {
            BitWriter t;
            t.put(static_cast<uint64_t>(b.bit(scf_pos)), 1);
            t.put(static_cast<uint64_t>(b.bit(scf_pos + 1)), 1);
            t.put(static_cast<uint64_t>(b.bit(scf_pos + 2)), 1);
            const int g = group_of(sb);
            if (g < nscfcrc) scfcrc[g] = static_cast<uint8_t>(crc_bits(t, 0, t.nbits, 0x1D, 8, 0));
        }
        const int64_t total = static_cast<int64_t>(frame_bytes) * 8;
        const int64_t tail = 8LL * (nxpad + nscfcrc + 2);
        if (b.nbits > total - tail) throw std::runtime_error("Layer II frame overflow");
        while (b.nbits < total - tail) b.put(0, 1);   // audio stuffing
        for (int k = 0; k < nxpad; k++) b.put(xpad_tx[k], 8);
        for (int k = nscfcrc - 1; k >= 0; k--) b.put(scfcrc[k], 8);   // ScF-CRC3 (or 1) .. ScF-CRC0
        b.put(fpad2[0], 8);
        b.put(fpad2[1], 8);
        for (int i = 15; i >= 0; i--) {
            const int bit = (crc >> i) & 1;
            const int64_t pos = crc_pos + (15 - i);
            if (bit) b.bytes[static_cast<size_t>(pos >> 3)] |= static_cast<uint8_t>(0x80 >> (pos & 7));
        }
        return b.bytes;
    }
};

// ---------------------------------------------------------------- AAC-LC 960
struct AacEncoder {
    static constexpr int N = 960;   // lines per frame
    double tone_hz = 0, amp = 0;    // amp in 16-bit PCM units
    std::vector<int> q;             // quantized lines (identical every AU)
    int global_gain = 100, max_sfb = 0;

    // a tone at a multiple of 50 Hz has a whole number of cycles per 960
    // samples at 48 kHz: every frame's MDCT lines are the same
    void init(double tone, double amp_pcm, int budget_bits) {
        tone_hz = std::max(50.0, std::round(tone / 50.0) * 50.0);
        amp = amp_pcm;
        const int W = 2 * N;
        std::vector<double> X(N, 0.0);
        for (int k = 0; k < N; k++) {
            double s = 0;
            for (int n = 0; n < W; n++) {
                const double w = std::sin(kPi / W * (n + 0.5));
                const double x = amp * std::cos(2 * kPi * tone_hz * n / 48000.0);
                s += w * x * std::cos(2 * kPi / W * (n + (W / 2.0 + 1) / 2.0) * (k + 0.5));
            }
            X[k] = 2 * s;   // x2: the ISO IMDCT (2/N) then reconstructs the input
        }
        // global gain: the finest step whose bitstream fits the budget
        for (int g = 60; g < 255; g++) {
            std::vector<int> qq(N);
            const double step = std::pow(2.0, 0.25 * (g - 100));
            int last = -1;
            bool clip = false;
            const int kt = static_cast<int>(tone_hz / 25.0);   // the tone's line (25 Hz per line)
            for (int k = 0; k < N; k++) {
                // lines beyond +-64 of the tone (1.6 kHz) are leakage far below the
                // quantizer step - left out; nothing above line 928 (band 48)
                if (std::abs(k - kt) > 64 || k >= 928) {
                    qq[static_cast<size_t>(k)] = 0;
                    continue;
                }
                const double a = std::fabs(X[k]) / step;
                int v = static_cast<int>(std::floor(std::pow(a, 0.75) + 0.4054));
                if (v > 8191) clip = true;
                qq[static_cast<size_t>(k)] = X[k] < 0 ? -v : v;
                if (v) last = k;
            }
            if (last < 0 || clip) continue;
            q = qq;
            global_gain = g;
            max_sfb = 0;
            while (kAacSwbOffset48[max_sfb] <= last) max_sfb++;
            if (spectral_bits() <= budget_bits) return;
        }
        throw std::runtime_error("AAC: tone does not fit the AU");
    }
    static int esc_bits(int v) {   // escape sequence length for |v| >= 16
        int n = 0, p = v;
        while ((p >>= 1) >= 16) n++;
        return n + 1 + n + 4;
    }
    bool band_zero(int b) const {
        for (int k = kAacSwbOffset48[b]; k < kAacSwbOffset48[b + 1]; k++)
            if (q[static_cast<size_t>(k)]) return false;
        return true;
    }
    int spectral_bits() const {
        BitWriter t;
        write_ics(t);
        return static_cast<int>(t.nbits);
    }
    // individual_channel_stream(): global_gain, ics_info (long window, sine),
    // section_data (ZERO_HCB / ESC_HCB runs), scale factors (all equal to the
    // global gain: delta 0), no pulse / TNS / gain control, spectral data
    void write_ics(BitWriter& b) const {
        b.put(static_cast<uint64_t>(global_gain), 8);
        b.put(0, 1);   // ics_reserved_bit
        b.put(0, 2);   // ONLY_LONG_SEQUENCE
        b.put(0, 1);   // sine window
        b.put(static_cast<uint64_t>(max_sfb), 6);
        b.put(0, 1);   // predictor_data_present
        std::vector<std::pair<int, int>> sec;   // (codebook, length)
        for (int s = 0; s < max_sfb; s++) {
            const int cbk = band_zero(s) ? 0 : 11;
            if (!sec.empty() && sec.back().first == cbk) sec.back().second++;
            else sec.push_back({cbk, 1});
        }
        for (const auto& [cbk, len] : sec) {
            b.put(static_cast<uint64_t>(cbk), 4);
            int l = len;
            while (l >= 31) {
                b.put(31, 5);
                l -= 31;
            }
            b.put(static_cast<uint64_t>(l), 5);
        }
        for (int s = 0; s < max_sfb; s++)
            if (!band_zero(s)) b.put(kAacScfCode[60], kAacScfLen[60]);
        b.put(0, 1);   // pulse_data_present
        b.put(0, 1);   // tns_data_present
        b.put(0, 1);   // gain_control_data_present
        for (int s = 0; s < max_sfb; s++) {
            if (band_zero(s)) continue;
            for (int k = kAacSwbOffset48[s]; k < kAacSwbOffset48[s + 1]; k += 2) {
                const int y = q[static_cast<size_t>(k)], z = q[static_cast<size_t>(k + 1)];
                const int ay = std::min(std::abs(y), 16), az = std::min(std::abs(z), 16);
                b.put(kAacCb11Code[ay][az], kAacCb11Len[ay][az]);
                if (y) b.put(y < 0 ? 1 : 0, 1);
                if (z) b.put(z < 0 ? 1 : 0, 1);
                for (int v : {std::abs(y), std::abs(z)}) {
                    if (v < 16) continue;
                    int n = 0, p = v;
                    while ((p >>= 1) >= 16) {
                        b.put(1, 1);
                        n++;
                    }
                    b.put(0, 1);
                    n += 4;
                    b.put(static_cast<uint64_t>(v - (1 << n)), n);
                }
            }
        }
    }
    // raw_data_block(): DSE carrying the PAD (first element, instance tag 0,
    // TS 102 563 5.4.1), SCE, FIL elements up to exactly `bytes`, END
    std::vector<uint8_t> au(const std::vector<uint8_t>& pad, int bytes) const {
        BitWriter b;
        if (!pad.empty()) {
            b.put(4, 3);   // ID_DSE
            b.put(0, 4);   // element_instance_tag
            b.put(0, 1);   // data_byte_align_flag
            b.put(pad.size(), 8);
            for (uint8_t c : pad) b.put(c, 8);
        }
        b.put(0, 3);   // ID_SCE
        b.put(0, 4);
        write_ics(b);
        int64_t room = 8LL * bytes - b.nbits - 3;
        if (room < 0) throw std::runtime_error("AAC AU overflow");
        while (room >= 15) {   // fill_element(): extension_payload EXT_FILL
            int cnt;
            int64_t used;
            if ((room - 7) / 8 <= 14) {
                cnt = static_cast<int>((room - 7) / 8);
                used = 7 + 8LL * cnt;
                b.put(6, 3);
                b.put(static_cast<uint64_t>(cnt), 4);
            } else {
                cnt = static_cast<int>(std::min<int64_t>(269, (room - 15) / 8));
                used = 15 + 8LL * cnt;
                b.put(6, 3);
                b.put(15, 4);
                b.put(static_cast<uint64_t>(cnt - 14), 8);
            }
            if (cnt > 0) {
                b.put(0, 4);   // EXT_FILL
                b.put(0, 4);   // fill_nibble
                for (int k = 1; k < cnt; k++) b.put(0xA5, 8);
            }
            room -= used;
        }
        if (room >= 7) {
            b.put(6, 3);
            b.put(0, 4);
            room -= 7;
        }
        b.put(7, 3);   // ID_END
        b.align();
        if (static_cast<int>(b.bytes.size()) != bytes) throw std::runtime_error("AAC AU size");
        return b.bytes;
    }
};

// GF(2^8), P(x) = x8 + x4 + x3 + x2 + 1, alpha = 2; RS(120, 110) shortened from RS(255, 245)
struct RsEncoder {
    uint8_t exp_[512], log_[256];
    uint8_t g[11];   // generator, g[0] = x^10 coefficient
    RsEncoder() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp_[i] = static_cast<uint8_t>(x);
            log_[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp_[i] = exp_[i - 255];
        // prod_{i=0}^{9} (x + alpha^i)
        std::vector<uint8_t> p = {1};
        for (int i = 0; i < 10; i++) {
            std::vector<uint8_t> np(p.size() + 1, 0);
            for (size_t k = 0; k < p.size(); k++) {
                np[k] ^= p[k];
                np[k + 1] ^= mul(p[k], exp_[i]);
            }
            p = np;
        }
        for (int i = 0; i < 11; i++) g[i] = p[static_cast<size_t>(i)];
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    // 110 data bytes (the first = highest power) -> 10 parity bytes
    void parity(const uint8_t* d, uint8_t* par) const {
        uint8_t r[10] = {0};
        for (int k = 0; k < 110; k++) {
            const uint8_t fb = d[k] ^ r[0];
            for (int i = 0; i < 9; i++) r[i] = r[i + 1] ^ mul(fb, g[i + 1]);
            r[9] = mul(fb, g[10]);
        }
        for (int i = 0; i < 10; i++) par[i] = r[i];
    }
    // c(alpha^j) for a 120-byte codeword (self test: all ten must be 0)
    uint8_t syndrome(const uint8_t* c, int j) const {
        uint8_t s = 0;
        for (int k = 0; k < 120; k++) s = static_cast<uint8_t>(mul(s, exp_[j]) ^ c[k]);
        return s;
    }
};

// DAB+ audio super frame (TS 102 563 5.2 / 6): 48 kHz AAC core without SBR,
// mono = 6 AUs of 960 samples; Fire code, AU CRCs, RS(120,110) over the
// virtual interleaver; returns s x 120 bytes (5 logical frames of 24 s bytes)
inline std::vector<uint8_t> superframe(int s, const std::vector<std::vector<uint8_t>>& aus, const RsEncoder& rs) {
    const int size = 110 * s, num = static_cast<int>(aus.size());
    std::vector<uint8_t> A(static_cast<size_t>(size), 0);
    std::vector<int> start(static_cast<size_t>(num));
    start[0] = 11;
    for (int n = 1; n < num; n++) start[static_cast<size_t>(n)] = start[static_cast<size_t>(n - 1)] + static_cast<int>(aus[static_cast<size_t>(n - 1)].size()) + 2;
    if (start[static_cast<size_t>(num - 1)] + static_cast<int>(aus.back().size()) + 2 != size) throw std::runtime_error("super frame size");
    BitWriter h;
    h.put(0, 16);   // Fire code, below
    h.put(0, 1);    // rfa
    h.put(1, 1);    // dac_rate 48 kHz
    h.put(0, 1);    // sbr_flag
    h.put(0, 1);    // aac_channel_mode mono
    h.put(0, 1);    // ps_flag
    h.put(0, 3);    // mpeg_surround_config
    for (int n = 1; n < num; n++) h.put(static_cast<uint64_t>(start[static_cast<size_t>(n)]), 12);
    h.align();      // num_aus 6: 60 bits of au_start + the 4 alignment bits
    std::copy(h.bytes.begin(), h.bytes.end(), A.begin());
    for (int n = 0; n < num; n++) {
        const auto& a = aus[static_cast<size_t>(n)];
        std::copy(a.begin(), a.end(), A.begin() + start[static_cast<size_t>(n)]);
        const uint16_t c = crc16_ccitt_inv(a.data(), a.size());
        A[static_cast<size_t>(start[static_cast<size_t>(n)]) + a.size()] = static_cast<uint8_t>(c >> 8);
        A[static_cast<size_t>(start[static_cast<size_t>(n)]) + a.size() + 1] = static_cast<uint8_t>(c & 0xFF);
    }
    // Fire code over bytes 2..10, G = x16 + x14 + x13 + x12 + x11 + x5 + x3 + x2 + x + 1, init 0
    const uint32_t fc = crc_bytes(A.data() + 2, 9, 0x782F, 16, 0);
    A[0] = static_cast<uint8_t>(fc >> 8);
    A[1] = static_cast<uint8_t>(fc & 0xFF);
    std::vector<uint8_t> O(static_cast<size_t>(120 * s));
    std::copy(A.begin(), A.end(), O.begin());
    for (int i = 0; i < s; i++) {
        uint8_t row[110], par[10];
        for (int j = 0; j < 110; j++) row[j] = A[static_cast<size_t>(i + j * s)];
        rs.parity(row, par);
        for (int r = 0; r < 10; r++) O[static_cast<size_t>(size + i + r * s)] = par[r];
    }
    return O;
}

}  // namespace dabgen
