// Two transmitters behind ONE talker ID (lora plugin, with separation by
// code): an ID that names no transmitter - a LoRaWAN downlink's "GW 927.5"
// (by its channel: two gateways of a network can answer on one RX1
// channel), a raw "LoRa <MHz> SF<n> <sync>" (devices sharing channel / SF /
// sync word) - may hold several. Each de-spread packet's spatial signature
// (the main eigenvector of its own covariance: one transmitter's array
// response) tells them apart: when the last WIN signatures of the ID fall
// into two clusters - each >= MIN_EACH packets, both in the older AND the
// newer half of the window (persistent, interleaved - not one transmitter
// that moved), alike inside (median |u_i^H c|^2 >= SAME), apart from each
// other (|c1^H c2|^2 <= APART: on a 5-element UCA of r 0.12 m at 927 MHz
// that is >= ~30 deg apart), few in between - the ID is split: its packets go
// to "<id> #1" / "<id> #2" (#1 = the one heard first), a packet fitting
// neither clearly gets no talker; MISS_RESET such packets in a row = back to
// one ID. Only with that evidence - a single transmitter's signatures agree
// to |u^H c|^2 ~ 0.99.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <utility>
#include <vector>

namespace lora_split {

using cd = std::complex<double>;
using Vec = std::vector<cd>;

constexpr int WIN = 30;            // signatures looked at
constexpr int MIN_EACH = 6;        // packets per transmitter
constexpr int MISS_RESET = 12;     // packets in a row fitting neither: one ID again
constexpr double SAME = 0.9, APART = 0.5, ASSIGN = 0.8, MARGIN = 0.2;

inline double sim(const Vec& a, const Vec& b) {   // unit vectors: |a^H b|^2
    if (a.size() != b.size() || a.empty()) return 0;
    cd d = 0;
    for (size_t k = 0; k < a.size(); k++) d += std::conj(a[k]) * b[k];
    return std::norm(d);
}
// the main eigenvector of sum u u^H
inline Vec centroid(const std::vector<const Vec*>& us) {
    if (us.empty()) return {};
    const size_t m = us[0]->size();
    Vec x = *us[0], y(m);
    for (int it = 0; it < 30; it++) {
        std::fill(y.begin(), y.end(), cd(0));
        for (const Vec* u : us) {
            cd d = 0;
            for (size_t k = 0; k < m; k++) d += std::conj((*u)[k]) * x[k];
            for (size_t k = 0; k < m; k++) y[k] += (*u)[k] * d;
        }
        double nn = 0;
        for (const cd& v : y) nn += std::norm(v);
        nn = std::sqrt(nn);
        if (!(nn > 0)) return {};
        for (size_t k = 0; k < m; k++) x[k] = y[k] / nn;
    }
    return x;
}

struct State {
    std::deque<Vec> win;
    bool on = false;
    Vec cen[2];
    int misses = 0;

    // 0 = one transmitter (the base id), 1 / 2 = which of the two, -1 = split
    // but this packet fits neither clearly (no talker). *event: 1 = it split
    // with this packet, -1 = it went back to one ID
    int add(const Vec& u, int* event) {
        *event = 0;
        if (u.empty()) return on ? -1 : 0;
        win.push_back(u);
        while (static_cast<int>(win.size()) > WIN) win.pop_front();
        if (!on) {
            if (!try_split()) return 0;
            on = true;
            misses = 0;
            *event = 1;
        }
        const double s0 = sim(cen[0], u), s1 = sim(cen[1], u);
        const int k = s0 >= s1 ? 0 : 1;
        const double sb = std::max(s0, s1), so = std::min(s0, s1);
        if (sb >= ASSIGN && sb - so >= MARGIN) {
            misses = 0;
            // follow it slowly (the main eigenvector of 0.95 c c^H + 0.05 u u^H)
            Vec& c = cen[k];
            cd d = 0;
            for (size_t i = 0; i < c.size(); i++) d += std::conj(u[i]) * c[i];
            const cd ph = std::abs(d) > 0 ? d / std::abs(d) : cd(1);
            double nn = 0;
            for (size_t i = 0; i < c.size(); i++) {
                c[i] = 0.95 * c[i] + 0.05 * u[i] * ph;
                nn += std::norm(c[i]);
            }
            nn = std::sqrt(nn);
            for (auto& v : c) v /= nn;
            return k + 1;
        }
        if (++misses >= MISS_RESET) {   // neither any more: one ID again
            on = false;
            win.clear();
            *event = -1;
            return 0;
        }
        return -1;
    }

private:
    bool try_split() {
        const int n = static_cast<int>(win.size());
        if (n < 2 * MIN_EACH) return false;
        std::vector<std::vector<double>> M(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(n), 1.0));
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) M[i][j] = M[j][i] = sim(win[static_cast<size_t>(i)], win[static_cast<size_t>(j)]);
        // seeds: the most typical signature, and the one least like it
        int a = 0;
        double best = -1;
        for (int i = 0; i < n; i++) {
            double m = 0;
            for (int j = 0; j < n; j++) m += M[i][j];
            if (m > best) { best = m; a = i; }
        }
        int b = 0;
        for (int j = 0; j < n; j++)
            if (M[a][j] < M[a][b]) b = j;
        if (M[a][b] > APART) return false;   // all alike: one transmitter
        Vec c[2] = {win[static_cast<size_t>(a)], win[static_cast<size_t>(b)]};
        std::vector<int> lab(static_cast<size_t>(n), 0);
        for (int it = 0; it < 3; it++) {
            std::vector<const Vec*> g[2];
            for (int i = 0; i < n; i++) {
                lab[static_cast<size_t>(i)] = sim(c[0], win[static_cast<size_t>(i)]) >= sim(c[1], win[static_cast<size_t>(i)]) ? 0 : 1;
                g[lab[static_cast<size_t>(i)]].push_back(&win[static_cast<size_t>(i)]);
            }
            if (static_cast<int>(g[0].size()) < MIN_EACH || static_cast<int>(g[1].size()) < MIN_EACH) return false;
            c[0] = centroid(g[0]);
            c[1] = centroid(g[1]);
        }
        if (c[0].empty() || c[1].empty() || sim(c[0], c[1]) > APART) return false;
        // alike inside, few in between, both in the older and the newer half
        std::vector<double> in[2];
        int unclear = 0, half[2][2] = {{0, 0}, {0, 0}};
        for (int i = 0; i < n; i++) {
            const int k = lab[static_cast<size_t>(i)];
            const double s = sim(c[k], win[static_cast<size_t>(i)]);
            in[k].push_back(s);
            if (s < ASSIGN || s - sim(c[1 - k], win[static_cast<size_t>(i)]) < MARGIN) unclear++;
            half[k][i >= n / 2]++;
        }
        for (int k = 0; k < 2; k++) {
            if (static_cast<int>(in[k].size()) < MIN_EACH) return false;
            std::nth_element(in[k].begin(), in[k].begin() + static_cast<long>(in[k].size() / 2), in[k].end());
            if (in[k][in[k].size() / 2] < SAME) return false;
            if (half[k][0] < 2 || half[k][1] < 2) return false;
        }
        if (unclear * 5 > n) return false;
        // #1 = the one heard first in the window
        const int first = lab[0];
        cen[0] = c[first];
        cen[1] = c[1 - first];
        return true;
    }
};

}  // namespace lora_split
