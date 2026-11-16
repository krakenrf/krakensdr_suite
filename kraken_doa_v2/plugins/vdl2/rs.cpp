#include "rs.hpp"

#include <cstring>

namespace vdl2 {

namespace {
constexpr int FCR = 120;   // first consecutive root: alpha^120 .. alpha^125
constexpr int NN = 255;
}

ReedSolomon::ReedSolomon() {
    int v = 1;
    for (int i = 0; i < NN; i++) {
        exp_[i] = static_cast<uint8_t>(v);
        log_[v] = i;
        v <<= 1;
        if (v & 0x100) v ^= 0x187;
    }
    for (int i = NN; i < 512; i++) exp_[i] = exp_[i - NN];
    log_[0] = 0;
}

bool ReedSolomon::syndromes(const uint8_t* cw, uint8_t* s) const {
    bool zero = true;
    for (int j = 0; j < NROOTS; j++) {
        // Horner from the highest degree: s = cw(alpha^(FCR + j))
        const int r = FCR + j;
        uint8_t acc = 0;
        for (int i = NN - 1; i >= 0; i--) acc = static_cast<uint8_t>(mul(acc, pw(r)) ^ cw[i]);
        s[j] = acc;
        if (acc) zero = false;
    }
    return zero;
}

int ReedSolomon::decode(uint8_t* cw, const int* erasures, int ne, int fill_lo, int fill_hi) const {
    uint8_t s[NROOTS];
    if (ne > NROOTS) return -1;
    const bool clean = syndromes(cw, s);
    if (clean) return 0;   // erased positions hold 0 and are consistent

    // erasure locator: lambda(x) = prod (1 + X_k x), X_k = alpha^pos
    uint8_t lambda[NROOTS + 1] = {1};
    for (int k = 0; k < ne; k++) {
        const uint8_t x = pw(erasures[k]);
        for (int j = k + 1; j > 0; j--) lambda[j] ^= mul(x, lambda[j - 1]);
    }
    uint8_t b[NROOTS + 1];
    memcpy(b, lambda, sizeof b);
    int el = ne;
    // Berlekamp-Massey over the remaining syndromes
    for (int r = ne + 1; r <= NROOTS; r++) {
        uint8_t discr = 0;
        for (int i = 0; i < r; i++) discr ^= mul(lambda[i], s[r - i - 1]);
        if (discr == 0) {
            memmove(b + 1, b, NROOTS);
            b[0] = 0;
            continue;
        }
        uint8_t t[NROOTS + 1];
        t[0] = lambda[0];
        for (int i = 0; i < NROOTS; i++) t[i + 1] = lambda[i + 1] ^ mul(discr, b[i]);
        if (2 * el <= r + ne - 1) {
            el = r + ne - el;
            const uint8_t inv = exp_[(NN - log_[discr]) % NN];
            for (int i = 0; i <= NROOTS; i++) b[i] = mul(lambda[i], inv);
        } else {
            memmove(b + 1, b, NROOTS);
            b[0] = 0;
        }
        memcpy(lambda, t, sizeof t);
    }
    int deg = 0;
    for (int i = 0; i <= NROOTS; i++)
        if (lambda[i]) deg = i;
    if (deg == 0) return -1;

    // Chien search: lambda(X^-1) = 0 at an error / erasure position
    int pos[NROOTS];
    int nroots = 0;
    for (int i = 0; i < NN; i++) {
        uint8_t v = 0;
        for (int j = 0; j <= deg; j++) v ^= mul(lambda[j], pw(-i * j));
        if (v == 0) {
            if (nroots == NROOTS) return -1;
            pos[nroots++] = i;
        }
    }
    if (nroots != deg) return -1;

    // omega(x) = s(x) lambda(x) mod x^NROOTS
    uint8_t omega[NROOTS] = {};
    for (int i = 0; i < NROOTS; i++)
        for (int j = 0; j <= i && j <= deg; j++) omega[i] ^= mul(s[i - j], lambda[j]);

    uint8_t fix[NN];
    memcpy(fix, cw, NN);
    for (int k = 0; k < nroots; k++) {
        const int p = pos[k];
        if (p >= fill_lo && p <= fill_hi) return -1;   // a "correction" in the virtual fill
        const int xinv = -p;
        uint8_t num = 0;
        for (int i = 0; i < NROOTS; i++) num ^= mul(omega[i], pw(xinv * i));
        uint8_t den = 0;   // lambda'(X^-1): the odd terms
        for (int i = 1; i <= deg; i += 2) den ^= mul(lambda[i], pw(xinv * (i - 1)));
        if (den == 0) return -1;
        // Forney: e = X^(1 - FCR) omega(X^-1) / lambda'(X^-1)
        const uint8_t e = mul(mul(num, exp_[(NN - log_[den]) % NN]), pw(p * (1 - FCR)));
        fix[p] ^= e;
    }
    uint8_t chk[NROOTS];
    if (!syndromes(fix, chk)) return -1;
    memcpy(cw, fix, NN);
    return nroots;
}

}  // namespace vdl2
