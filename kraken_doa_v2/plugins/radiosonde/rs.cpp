#include "rs.hpp"

#include <cstring>

namespace sonde {

ReedSolomon::ReedSolomon(unsigned gfpoly, int fcr, int prim, int nroots) : fcr_(fcr), prim_(prim), nroots_(nroots) {
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        exp_[i] = exp_[i + 255] = static_cast<uint8_t>(x);
        log_[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= gfpoly;
    }
    exp_[510] = exp_[511] = exp_[0];
    log_[0] = 0;   // never used (mul checks for 0)
}

uint8_t ReedSolomon::pow_beta(int e) const {
    long k = (static_cast<long>(e) * prim_) % 255;
    if (k < 0) k += 255;
    return exp_[k];
}

void ReedSolomon::syndromes(const uint8_t* cw, uint8_t* s) const {
    for (int j = 0; j < nroots_; j++) {
        const uint8_t r = pow_beta(fcr_ + j);
        uint8_t acc = 0;
        for (int i = 254; i >= 0; i--) acc = mul(acc, r) ^ cw[i];   // Horner, highest power first
        s[j] = acc;
    }
}

bool ReedSolomon::check(const uint8_t* cw) const {
    uint8_t s[64];
    syndromes(cw, s);
    for (int j = 0; j < nroots_; j++)
        if (s[j]) return false;
    return true;
}

int ReedSolomon::decode(uint8_t* cw) const {
    uint8_t s[64];
    syndromes(cw, s);
    bool any = false;
    for (int j = 0; j < nroots_; j++) any |= s[j] != 0;
    if (!any) return 0;

    // Berlekamp-Massey: error locator lambda(x) = prod(1 - X_k x)
    uint8_t lam[65] = {1}, b[65] = {1}, t[65];
    int L = 0, m = 1;
    uint8_t bb = 1;
    for (int n = 0; n < nroots_; n++) {
        uint8_t d = s[n];
        for (int i = 1; i <= L; i++) d ^= mul(lam[i], s[n - i]);
        if (!d) { m++; continue; }
        const uint8_t coef = exp_[(log_[d] + 255 - log_[bb]) % 255];   // d / bb
        std::memcpy(t, lam, sizeof t);
        for (int i = 0; i + m <= nroots_; i++) lam[i + m] ^= mul(coef, b[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            std::memcpy(b, t, sizeof b);
            bb = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L == 0 || L > nroots_ / 2) return -1;

    // omega(x) = S(x) lambda(x) mod x^nroots
    uint8_t om[64] = {0};
    for (int i = 0; i < nroots_; i++)
        for (int j = 0; j <= L && j <= i; j++) om[i] ^= mul(s[i - j], lam[j]);

    // Chien search over every position, Forney for the values
    int pos[32], nf = 0;
    uint8_t val[32];
    for (int i = 0; i < 255; i++) {
        const uint8_t xinv = pow_beta(-i);        // X^-1, X = beta^i
        uint8_t acc = 0, xp = 1;
        for (int k = 0; k <= L; k++) { acc ^= mul(lam[k], xp); xp = mul(xp, xinv); }
        if (acc) continue;
        if (nf == 32) return -1;
        // lambda'(X^-1): the odd terms (characteristic 2)
        uint8_t den = 0;
        xp = 1;
        for (int k = 1; k <= L; k += 2) {
            den ^= mul(lam[k], xp);
            xp = mul(xp, mul(xinv, xinv));
        }
        if (!den) return -1;
        uint8_t num = 0;
        xp = 1;
        for (int k = 0; k < nroots_; k++) { num ^= mul(om[k], xp); xp = mul(xp, xinv); }
        // Y = X^(1-fcr) omega(X^-1) / lambda'(X^-1)
        const uint8_t xf = pow_beta(i * (1 - fcr_));
        uint8_t y = mul(xf, num);
        y = y ? exp_[(log_[y] + 255 - log_[den]) % 255] : 0;
        pos[nf] = i;
        val[nf] = y;
        nf++;
    }
    if (nf != L) return -1;
    for (int k = 0; k < nf; k++) cw[pos[k]] ^= val[k];
    return check(cw) ? nf : -1;
}

}  // namespace sonde
