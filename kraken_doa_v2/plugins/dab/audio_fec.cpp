// DAB+ Reed-Solomon RS(120, 110), the Fire code and CRC-16 (see audio_fec.hpp,
// audio_bits.hpp). Written from ETSI TS 102 563 clause 6 and EN 300 401
// annex E.

#include "audio_fec.hpp"
#include "audio_bits.hpp"

#include <array>
#include <cstring>

namespace dab::audio {

namespace {

struct Gf {
    std::array<uint8_t, 512> exp{};   // doubled: exp[i + j] without a modulo
    std::array<int, 256> log{};
    std::array<uint8_t, Rs120::NPAR + 1> gen{};   // g(x) coefficients, gen[i] of x^i
    constexpr Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;   // x^8 + x^4 + x^3 + x^2 + 1
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = -1;
        // g(x) = prod (x + alpha^i), i = 0..9
        gen[0] = 1;
        for (int i = 0; i < Rs120::NPAR; i++) {
            uint8_t root = exp[i];
            for (int k = i + 1; k >= 0; k--) {
                uint8_t prev = k > 0 ? gen[k - 1] : 0;
                gen[k] = static_cast<uint8_t>(prev ^ mul(gen[k], root));
            }
        }
    }
    constexpr uint8_t mul(uint8_t a, uint8_t b) const {
        return (a && b) ? exp[log[a] + log[b]] : 0;
    }
    constexpr uint8_t div(uint8_t a, uint8_t b) const {   // b != 0
        return a ? exp[log[a] + 255 - log[b]] : 0;
    }
};

constexpr Gf GF{};

// CRC-16 CCITT table, MSb first
struct CrcTab {
    std::array<uint16_t, 256> t{};
    constexpr CrcTab() {
        for (int i = 0; i < 256; i++) {
            uint16_t r = static_cast<uint16_t>(i << 8);
            for (int b = 0; b < 8; b++) r = static_cast<uint16_t>((r & 0x8000) ? (r << 1) ^ 0x1021 : (r << 1));
            t[i] = r;
        }
    }
};
constexpr CrcTab CRC{};

}   // namespace

uint16_t crc16_ccitt(const uint8_t* d, size_t n) {
    uint16_t r = 0xFFFF;
    for (size_t i = 0; i < n; i++) r = static_cast<uint16_t>((r << 8) ^ CRC.t[((r >> 8) ^ d[i]) & 0xFF]);
    return static_cast<uint16_t>(~r);
}

uint16_t fire_code(const uint8_t* d, size_t n) {
    uint32_t r = 0;
    for (size_t i = 0; i < n; i++) r = crc_bits<16>(r, 0x782F, d[i], 8);
    return static_cast<uint16_t>(r);
}

void Rs120::encode(const uint8_t* data, uint8_t* par) {
    uint8_t r[NPAR] = {};   // r[i] = coefficient of x^i of the remainder
    for (int k = 0; k < K; k++) {
        uint8_t fb = data[k] ^ r[NPAR - 1];
        for (int i = NPAR - 1; i > 0; i--) r[i] = r[i - 1] ^ GF.mul(fb, GF.gen[i]);
        r[0] = GF.mul(fb, GF.gen[0]);
    }
    for (int i = 0; i < NPAR; i++) par[i] = r[NPAR - 1 - i];
}

int Rs120::decode(uint8_t* cw) {
    // syndromes S_j = c(alpha^j), byte k = coefficient of x^(119 - k)
    uint8_t S[NPAR];
    bool all0 = true;
    for (int j = 0; j < NPAR; j++) {
        uint8_t s = 0;
        const uint8_t aj = GF.exp[j];
        for (int k = 0; k < N; k++) s = static_cast<uint8_t>(GF.mul(s, aj) ^ cw[k]);
        S[j] = s;
        all0 = all0 && s == 0;
    }
    if (all0) return 0;

    // Berlekamp-Massey
    uint8_t lam[NPAR + 1] = {1}, B[NPAR + 1] = {1}, Tm[NPAR + 1];
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < NPAR; n++) {
        uint8_t d = S[n];
        for (int i = 1; i <= L; i++) d ^= GF.mul(lam[i], S[n - i]);
        if (d == 0) { m++; continue; }
        uint8_t coef = GF.div(d, b);
        std::memcpy(Tm, lam, sizeof lam);
        for (int i = 0; i + m <= NPAR; i++) lam[i + m] ^= GF.mul(coef, B[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            std::memcpy(B, Tm, sizeof B);
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L > T) return -1;
    for (int i = L + 1; i <= NPAR; i++)
        if (lam[i]) return -1;

    // Chien search over the 120 positions of the shortened word
    int pos[T];
    int nroot = 0;
    for (int p = 0; p < N; p++) {
        // lambda(alpha^-p)
        uint8_t v = 0;
        int e = (255 - p) % 255;
        for (int i = L; i >= 0; i--) v = static_cast<uint8_t>(GF.mul(v, GF.exp[e]) ^ lam[i]);
        if (v == 0) {
            if (nroot == T) return -1;
            pos[nroot++] = p;
        }
    }
    if (nroot != L) return -1;

    // Omega(x) = S(x) lambda(x) mod x^10
    uint8_t om[NPAR] = {};
    for (int i = 0; i < NPAR; i++)
        for (int k = 0; k <= i && k <= L; k++) om[i] ^= GF.mul(lam[k], S[i - k]);

    // Forney (first consecutive root alpha^0): e = X omega(X^-1) / lambda'(X^-1)
    uint8_t val[T];
    for (int r = 0; r < nroot; r++) {
        int p = pos[r];
        uint8_t xinv = GF.exp[(255 - p) % 255];
        uint8_t o = 0;
        for (int i = NPAR - 1; i >= 0; i--) o = static_cast<uint8_t>(GF.mul(o, xinv) ^ om[i]);
        uint8_t dl = 0;   // formal derivative: odd terms, lambda_i x^(i-1)
        uint8_t x2 = GF.mul(xinv, xinv), pw = 1;
        for (int i = 1; i <= L; i += 2) {
            dl ^= GF.mul(lam[i], pw);
            pw = GF.mul(pw, x2);
        }
        if (dl == 0) return -1;
        val[r] = GF.mul(GF.exp[p], GF.div(o, dl));
        if (val[r] == 0) return -1;
    }
    for (int r = 0; r < nroot; r++) cw[N - 1 - pos[r]] ^= val[r];

    // re-check: a corrected word has zero syndromes
    for (int j = 0; j < NPAR; j++) {
        uint8_t s = 0;
        const uint8_t aj = GF.exp[j];
        for (int k = 0; k < N; k++) s = static_cast<uint8_t>(GF.mul(s, aj) ^ cw[k]);
        if (s) {
            for (int r = 0; r < nroot; r++) cw[N - 1 - pos[r]] ^= val[r];   // undo
            return -1;
        }
    }
    return nroot;
}

}   // namespace dab::audio
