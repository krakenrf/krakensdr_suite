// Reed-Solomon over GF(2^8) (errors only), written from the textbook
// algorithms: syndromes by Horner's rule, Berlekamp-Massey for the error
// locator, Chien search for its roots, Forney for the error values.

#include "rscode.hpp"

#include <cstring>

namespace sonde {

ReedSolomon::ReedSolomon(unsigned poly, int fcr, int prim, int nroots) : fcr_(fcr), prim_(prim), nroots_(nroots) {
    unsigned v = 1;
    for (int i = 0; i < 255; i++) {
        exp_[i] = static_cast<uint8_t>(v);
        log_[v] = i;
        v <<= 1;
        if (v & 0x100) v ^= poly;
    }
    for (int i = 255; i < 512; i++) exp_[i] = exp_[i - 255];
    log_[0] = -1;
    // g(x) = prod_j (x - beta^(fcr+j))
    std::memset(gen_, 0, sizeof gen_);
    gen_[0] = 1;
    for (int j = 0; j < nroots_; j++) {
        const uint8_t r = pw(prim_ * (fcr_ + j));
        for (int k = j + 1; k > 0; k--) gen_[k] = static_cast<uint8_t>(gen_[k - 1] ^ mul(gen_[k], r));
        gen_[0] = mul(gen_[0], r);
    }
}

bool ReedSolomon::syndromes(const uint8_t* cw, int used, uint8_t* s) const {
    bool zero = true;
    for (int j = 0; j < nroots_; j++) {
        const int e = (prim_ * (fcr_ + j)) % 255;   // x = alpha^e
        uint8_t acc = 0;
        for (int i = used - 1; i >= 0; i--) {
            // acc = acc * x + cw[i]
            acc = static_cast<uint8_t>((acc ? exp_[log_[acc] + e] : 0) ^ cw[i]);
        }
        s[j] = acc;
        if (acc) zero = false;
    }
    return zero;
}

int ReedSolomon::decode(uint8_t* cw, int used) const {
    uint8_t s[64];
    if (syndromes(cw, used, s)) return 0;
    const int T2 = nroots_;
    // Berlekamp-Massey: lambda(x) with lambda(X_k^-1) = 0
    uint8_t lam[65] = {1}, b[65] = {1}, t[65];
    int L = 0, m = 1;
    uint8_t bb = 1;
    for (int n = 0; n < T2; n++) {
        uint8_t d = s[n];
        for (int i = 1; i <= L; i++) d ^= mul(lam[i], s[n - i]);
        if (d == 0) {
            m++;
            continue;
        }
        const uint8_t coef = exp_[(log_[d] - log_[bb] + 255) % 255];
        std::memcpy(t, lam, sizeof t);
        for (int i = 0; i + m <= T2; i++) lam[i + m] ^= mul(coef, b[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            std::memcpy(b, t, sizeof b);
            bb = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L == 0 || 2 * L > T2) return -1;
    // Chien search over every position of the full code: lambda(beta^-p)
    int pos[32], nf = 0;
    for (int p = 0; p < 255; p++) {
        uint8_t v = lam[0];
        for (int i = 1; i <= L; i++)
            if (lam[i]) v ^= exp_[(log_[lam[i]] + ((-prim_ * p * i) % 255 + 255) % 255) % 255];
        if (v == 0) {
            if (nf >= 32) return -1;
            pos[nf++] = p;
        }
    }
    if (nf != L) return -1;
    // omega(x) = S(x) lambda(x) mod x^2t
    uint8_t om[64] = {0};
    for (int i = 0; i < T2; i++) {
        uint8_t acc = 0;
        for (int k = 0; k <= i && k <= L; k++) acc ^= mul(lam[k], s[i - k]);
        om[i] = acc;
    }
    uint8_t fix[32];
    for (int k = 0; k < nf; k++) {
        const int p = pos[k];
        if (p >= used) return -1;   // outside the (shortened) code
        const int xinv = ((-prim_ * p) % 255 + 255) % 255;   // log of X^-1
        // omega(X^-1)
        uint8_t o = 0;
        for (int i = 0; i < T2; i++)
            if (om[i]) o ^= exp_[(log_[om[i]] + xinv * i) % 255];
        // lambda'(X^-1): the odd terms
        uint8_t dl = 0;
        for (int i = 1; i <= L; i += 2)
            if (lam[i]) dl ^= exp_[(log_[lam[i]] + xinv * (i - 1)) % 255];
        if (dl == 0) return -1;
        // e = X^(1 - fcr) omega(X^-1) / lambda'(X^-1)
        const int lx = (prim_ * p) % 255;
        int le = (lx * ((1 - fcr_) % 255 + 255) % 255) % 255;
        if (o == 0) return -1;
        le = (le + log_[o] - log_[dl]) % 255;
        if (le < 0) le += 255;
        fix[k] = exp_[le];
    }
    uint8_t keep[32];
    for (int k = 0; k < nf; k++) {
        keep[k] = cw[pos[k]];
        cw[pos[k]] ^= fix[k];
    }
    if (!syndromes(cw, used, s)) {   // doesn't verify
        for (int k = 0; k < nf; k++) cw[pos[k]] = keep[k];
        return -1;
    }
    return nf;
}

void ReedSolomon::encode(uint8_t* cw, int used) const {
    // remainder of d(x) x^nroots by g(x), long division from the top
    uint8_t r[64] = {0};
    for (int i = used - 1; i >= nroots_; i--) {
        const uint8_t fb = static_cast<uint8_t>(cw[i] ^ r[nroots_ - 1]);
        for (int k = nroots_ - 1; k > 0; k--) r[k] = static_cast<uint8_t>(r[k - 1] ^ mul(fb, gen_[k]));
        r[0] = mul(fb, gen_[0]);
    }
    for (int k = 0; k < nroots_; k++) cw[k] = r[k];
}

}  // namespace sonde
