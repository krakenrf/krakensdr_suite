#pragma once

// Small complex linear algebra for the array combining of the adsb plugin:
// covariances of up to 8 antennas, their principal eigenvector (power
// iteration) and a linear solve (the MVDR beam). Double precision - the
// matrices are 5 x 5, a few hundred per second.

#include <cmath>
#include <complex>

namespace adsb {

constexpr int MAXM = 8;    // antennas
using cd = std::complex<double>;

struct Mat {
    int m = 0;
    cd a[MAXM][MAXM];
    void zero(int n) {
        m = n;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < n; k++) a[i][k] = 0;
    }
    // the lower triangle from the upper one (add_outer fills only the upper)
    void mirror() {
        for (int i = 0; i < m; i++)
            for (int k = 0; k < i; k++) a[i][k] = std::conj(a[k][i]);
    }
};

struct Vec {
    int m = 0;
    cd v[MAXM];
};

// R += w y y^H, upper triangle
inline void add_outer(Mat& R, const cd* y, double w) {
    for (int i = 0; i < R.m; i++) {
        const cd yi = y[i] * w;
        for (int k = i; k < R.m; k++) R.a[i][k] += yi * std::conj(y[k]);
    }
}

inline double norm2(const Vec& v) {
    double s = 0;
    for (int i = 0; i < v.m; i++) s += std::norm(v.v[i]);
    return s;
}

inline bool normalize(Vec& v) {
    const double n = norm2(v);
    if (!(n > 1e-300)) return false;
    const double k = 1.0 / std::sqrt(n);
    for (int i = 0; i < v.m; i++) v.v[i] *= k;
    return true;
}

// a^H b
inline cd dot(const Vec& a, const Vec& b) {
    cd s = 0;
    for (int i = 0; i < a.m; i++) s += std::conj(a.v[i]) * b.v[i];
    return s;
}

// The eigenvector of the largest eigenvalue of a Hermitian R (both triangles
// filled), unit length. psd = false: R may have negative eigenvalues (a
// difference of covariances) - shifted by its Frobenius norm first, so the
// iteration finds the largest, not the largest in magnitude.
inline Vec principal(const Mat& R, bool psd = true) {
    const int m = R.m;
    double fro = 0;
    int k0 = 0;
    for (int i = 0; i < m; i++) {
        for (int k = 0; k < m; k++) fro += std::norm(R.a[i][k]);
        if (R.a[i][i].real() > R.a[k0][k0].real()) k0 = i;
    }
    const double shift = psd ? 0.0 : std::sqrt(fro);
    Vec v;
    v.m = m;
    for (int i = 0; i < m; i++) v.v[i] = R.a[i][k0];
    v.v[k0] += shift;
    if (!normalize(v)) {
        for (int i = 0; i < m; i++) v.v[i] = i == 0 ? 1.0 : 0.0;
        return v;
    }
    Vec u;
    u.m = m;
    for (int it = 0; it < (psd ? 10 : 20); it++) {
        for (int i = 0; i < m; i++) {
            cd s = shift * v.v[i];
            for (int k = 0; k < m; k++) s += R.a[i][k] * v.v[k];
            u.v[i] = s;
        }
        if (!normalize(u)) break;
        v = u;
    }
    return v;
}

// A x = b by Gaussian elimination with partial pivoting (A is copied)
inline bool solve(Mat A, Vec b, Vec* x) {
    const int m = A.m;
    for (int c = 0; c < m; c++) {
        int p = c;
        for (int r = c + 1; r < m; r++)
            if (std::abs(A.a[r][c]) > std::abs(A.a[p][c])) p = r;
        if (!(std::abs(A.a[p][c]) > 1e-300)) return false;
        if (p != c) {
            for (int k = 0; k < m; k++) std::swap(A.a[p][k], A.a[c][k]);
            std::swap(b.v[p], b.v[c]);
        }
        for (int r = c + 1; r < m; r++) {
            const cd f = A.a[r][c] / A.a[c][c];
            if (f == cd(0)) continue;
            for (int k = c; k < m; k++) A.a[r][k] -= f * A.a[c][k];
            b.v[r] -= f * b.v[c];
        }
    }
    x->m = m;
    for (int r = m - 1; r >= 0; r--) {
        cd s = b.v[r];
        for (int k = r + 1; k < m; k++) s -= A.a[r][k] * x->v[k];
        x->v[r] = s / A.a[r][r];
    }
    return true;
}

}  // namespace adsb
