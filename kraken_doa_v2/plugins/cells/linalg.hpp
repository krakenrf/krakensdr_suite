#pragma once

// Small dense complex linear algebra for the cells plugin (m <= 8 antennas):
// Hermitian matrices as row-major std::vector<cd>. Eigen-decomposition by
// cyclic Jacobi on the real symmetric 2m x 2m embedding [[Re, -Im], [Im, Re]]
// (each eigenvalue appears twice; the eigenvector (x; y) gives x + j y).

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace cells {

using cd = std::complex<double>;
using Mat = std::vector<cd>;   // m x m, row-major

inline Mat mat_zero(int m) { return Mat(static_cast<size_t>(m * m), cd(0, 0)); }

inline double trace_re(const Mat& A, int m) {
    double t = 0;
    for (int i = 0; i < m; i++) t += A[static_cast<size_t>(i * m + i)].real();
    return t;
}

// A += v v^H
inline void add_outer(Mat& A, const cd* v, int m, double w = 1.0) {
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++) A[static_cast<size_t>(i * m + j)] += w * v[i] * std::conj(v[j]);
}

// Eigen-decomposition of a Hermitian matrix: eigenvalues ascending, the
// matching unit eigenvectors in vecs (m vectors of m)
inline void eig_herm(const Mat& A, int m, std::vector<double>* vals, std::vector<std::vector<cd>>* vecs) {
    const int n = 2 * m;
    std::vector<double> S(static_cast<size_t>(n * n)), V(static_cast<size_t>(n * n), 0.0);
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++) {
            const cd a = 0.5 * (A[static_cast<size_t>(i * m + j)] + std::conj(A[static_cast<size_t>(j * m + i)]));
            S[static_cast<size_t>(i * n + j)] = a.real();
            S[static_cast<size_t>((i + m) * n + (j + m))] = a.real();
            S[static_cast<size_t>(i * n + (j + m))] = -a.imag();
            S[static_cast<size_t>((i + m) * n + j)] = a.imag();
        }
    for (int i = 0; i < n; i++) V[static_cast<size_t>(i * n + i)] = 1;
    for (int sweep = 0; sweep < 60; sweep++) {
        double off = 0;
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++) off += S[static_cast<size_t>(p * n + q)] * S[static_cast<size_t>(p * n + q)];
        if (off < 1e-26) break;
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++) {
                const double apq = S[static_cast<size_t>(p * n + q)];
                if (std::fabs(apq) < 1e-300) continue;
                const double app = S[static_cast<size_t>(p * n + p)], aqq = S[static_cast<size_t>(q * n + q)];
                const double tau = (aqq - app) / (2 * apq);
                const double t = (tau >= 0 ? 1.0 : -1.0) / (std::fabs(tau) + std::sqrt(1 + tau * tau));
                const double c = 1 / std::sqrt(1 + t * t), s = t * c;
                for (int k = 0; k < n; k++) {
                    const double skp = S[static_cast<size_t>(k * n + p)], skq = S[static_cast<size_t>(k * n + q)];
                    S[static_cast<size_t>(k * n + p)] = c * skp - s * skq;
                    S[static_cast<size_t>(k * n + q)] = s * skp + c * skq;
                }
                for (int k = 0; k < n; k++) {
                    const double spk = S[static_cast<size_t>(p * n + k)], sqk = S[static_cast<size_t>(q * n + k)];
                    S[static_cast<size_t>(p * n + k)] = c * spk - s * sqk;
                    S[static_cast<size_t>(q * n + k)] = s * spk + c * sqk;
                }
                for (int k = 0; k < n; k++) {
                    const double vkp = V[static_cast<size_t>(k * n + p)], vkq = V[static_cast<size_t>(k * n + q)];
                    V[static_cast<size_t>(k * n + p)] = c * vkp - s * vkq;
                    V[static_cast<size_t>(k * n + q)] = s * vkp + c * vkq;
                }
            }
    }
    // the 2m eigenvalues come in pairs: sort, take every other one
    std::vector<int> idx(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) idx[static_cast<size_t>(i)] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return S[static_cast<size_t>(a * n + a)] < S[static_cast<size_t>(b * n + b)]; });
    vals->clear();
    vecs->clear();
    for (int r = 0; r < n; r += 2) {
        // of a pair, the vector whose complex version is better conditioned
        int best = idx[static_cast<size_t>(r)];
        double bn = -1;
        for (int c : {idx[static_cast<size_t>(r)], idx[static_cast<size_t>(r + 1)]}) {
            double nn = 0;
            for (int i = 0; i < m; i++)
                nn += std::norm(cd(V[static_cast<size_t>(i * n + c)], V[static_cast<size_t>((i + m) * n + c)]));
            if (nn > bn) { bn = nn; best = c; }
        }
        std::vector<cd> v(static_cast<size_t>(m));
        double nn = 0;
        for (int i = 0; i < m; i++) {
            v[static_cast<size_t>(i)] = cd(V[static_cast<size_t>(i * n + best)], V[static_cast<size_t>((i + m) * n + best)]);
            nn += std::norm(v[static_cast<size_t>(i)]);
        }
        nn = std::sqrt(std::max(nn, 1e-300));
        for (auto& x : v) x /= nn;
        vals->push_back(0.5 * (S[static_cast<size_t>(idx[static_cast<size_t>(r)] * n + idx[static_cast<size_t>(r)])] +
                               S[static_cast<size_t>(idx[static_cast<size_t>(r + 1)] * n + idx[static_cast<size_t>(r + 1)])]));
        vecs->push_back(std::move(v));
    }
}

// Inverse of a Hermitian positive definite matrix (Cholesky); loading
// added to the diagonal. false: not positive definite
inline bool inv_herm(const Mat& A, int m, Mat* out, double loading = 0) {
    Mat L = mat_zero(m);
    for (int j = 0; j < m; j++) {
        double d = A[static_cast<size_t>(j * m + j)].real() + loading;
        for (int k = 0; k < j; k++) d -= std::norm(L[static_cast<size_t>(j * m + k)]);
        if (!(d > 0)) return false;
        const double ljj = std::sqrt(d);
        L[static_cast<size_t>(j * m + j)] = ljj;
        for (int i = j + 1; i < m; i++) {
            cd s = A[static_cast<size_t>(i * m + j)];
            for (int k = 0; k < j; k++) s -= L[static_cast<size_t>(i * m + k)] * std::conj(L[static_cast<size_t>(j * m + k)]);
            L[static_cast<size_t>(i * m + j)] = s / ljj;
        }
    }
    // inverse of L, then A^-1 = L^-H L^-1
    Mat Li = mat_zero(m);
    for (int i = 0; i < m; i++) {
        Li[static_cast<size_t>(i * m + i)] = 1.0 / L[static_cast<size_t>(i * m + i)];
        for (int j = 0; j < i; j++) {
            cd s = 0;
            for (int k = j; k < i; k++) s -= L[static_cast<size_t>(i * m + k)] * Li[static_cast<size_t>(k * m + j)];
            Li[static_cast<size_t>(i * m + j)] = s / L[static_cast<size_t>(i * m + i)];
        }
    }
    *out = mat_zero(m);
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++) {
            cd s = 0;
            for (int k = std::max(i, j); k < m; k++) s += std::conj(Li[static_cast<size_t>(k * m + i)]) * Li[static_cast<size_t>(k * m + j)];
            (*out)[static_cast<size_t>(i * m + j)] = s;
        }
    return true;
}

// |u^H v|^2 / (|u|^2 |v|^2)
inline double sig_corr(const std::vector<cd>& u, const std::vector<cd>& v) {
    cd d = 0;
    double nu = 0, nv = 0;
    for (size_t i = 0; i < u.size() && i < v.size(); i++) {
        d += std::conj(u[i]) * v[i];
        nu += std::norm(u[i]);
        nv += std::norm(v[i]);
    }
    return nu > 0 && nv > 0 ? std::norm(d) / (nu * nv) : 0.0;
}

}  // namespace cells
