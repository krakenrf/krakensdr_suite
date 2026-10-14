// OGN tracker packet: LDPC check / soft decoding, de-whitening, fields.
// Field layout from the OGN tracker's published protocol description (bit
// fields of its OGN1_Packet); own implementation.

#include "ogn.hpp"

#include "ogn_ldpc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ogn {

namespace {

inline int bit_of(const uint8_t* p, int i) { return (p[i >> 3] >> (i & 7)) & 1; }

struct Graph {
    std::vector<int> row_start, cols;      // check -> variables
    Graph() {
        row_start.push_back(0);
        for (int r = 0; r < 48; r++) {
            for (int c = 0; c < 208; c++)
                if ((LDPC_H[r][c >> 5] >> (c & 31)) & 1) cols.push_back(c);
            row_start.push_back(static_cast<int>(cols.size()));
        }
    }
};
const Graph& graph() {
    static const Graph g;
    return g;
}

// variable-resolution numbers: 'bits' of mantissa, 2 bits of range (x1 x2 x4 x8)
int vr_decode(int v, int bits) {
    const int thr = 1 << bits, range = v >> bits;
    v &= thr - 1;
    if (range == 0) return v;
    if (range == 1) return thr + 1 + (v << 1);
    if (range == 2) return 3 * thr + 2 + (v << 2);
    return 7 * thr + 4 + (v << 3);
}
int svr_decode(int v, int bits) {   // sign bit above mantissa + range
    const int sign = 1 << (bits + 2);
    const int m = vr_decode(v & (sign - 1), bits);
    return (v & sign) ? -m : m;
}

// TEA (Wheeler & Needham), all-zero key: the OGN tracker's "whitening"
void tea_decrypt_key0(uint32_t* d, int loops) {
    uint32_t v0 = d[0], v1 = d[1];
    const uint32_t delta = 0x9e3779b9u;
    uint32_t sum = delta * static_cast<uint32_t>(loops);
    for (int i = 0; i < loops; i++) {
        v1 -= (v0 << 4) ^ (v0 + sum) ^ (v0 >> 5);
        v0 -= (v1 << 4) ^ (v1 + sum) ^ (v1 >> 5);
        sum -= delta;
    }
    d[0] = v0;
    d[1] = v1;
}

int32_t sext24(uint32_t v) { return static_cast<int32_t>(v << 8) >> 8; }

}  // namespace

int ldpc_errors(const uint8_t* pkt) {
    int err = 0;
    for (int r = 0; r < 48; r++) {
        uint32_t acc = 0;
        for (int w = 0; w < 7; w++) {
            uint32_t word = 0;
            for (int b = 0; b < 4; b++) {
                const int idx = 4 * w + b;
                if (idx < 26) word |= static_cast<uint32_t>(pkt[idx]) << (8 * b);
            }
            acc ^= word & LDPC_H[r][w];
        }
        err += __builtin_popcount(acc) & 1;
    }
    return err;
}

int ldpc_decode(const float* llr, uint8_t* pkt, int iterations) {
    const Graph& g = graph();
    const size_t E = g.cols.size();
    std::vector<float> q(E), rr(E, 0.f), tot(llr, llr + 208);
    for (int r = 0; r < 48; r++)
        for (int e = g.row_start[r]; e < g.row_start[r + 1]; e++) q[static_cast<size_t>(e)] = llr[g.cols[static_cast<size_t>(e)]];
    uint8_t hard0[26];
    std::memset(hard0, 0, sizeof hard0);
    for (int i = 0; i < 208; i++)
        if (llr[i] < 0) hard0[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
    for (int it = 0; it <= iterations; it++) {
        // hard decision + syndrome
        std::memset(pkt, 0, 26);
        for (int i = 0; i < 208; i++)
            if (tot[static_cast<size_t>(i)] < 0) pkt[i >> 3] |= static_cast<uint8_t>(1 << (i & 7));
        if (ldpc_errors(pkt) == 0) {
            int changed = 0;
            for (int i = 0; i < 26; i++) changed += __builtin_popcount(pkt[i] ^ hard0[i]);
            return changed;
        }
        if (it == iterations) break;
        // check -> variable (normalised min-sum)
        for (int r = 0; r < 48; r++) {
            const int a = g.row_start[r], b = g.row_start[r + 1];
            float m1 = 1e30f, m2 = 1e30f;
            int arg = -1, sgn = 0;
            for (int e = a; e < b; e++) {
                const float v = q[static_cast<size_t>(e)], av = std::fabs(v);
                sgn ^= v < 0;
                if (av < m1) { m2 = m1; m1 = av; arg = e; }
                else if (av < m2) m2 = av;
            }
            for (int e = a; e < b; e++) {
                const float v = q[static_cast<size_t>(e)];
                const int s = sgn ^ (v < 0);
                const float mag = 0.75f * (e == arg ? m2 : m1);
                rr[static_cast<size_t>(e)] = s ? -mag : mag;
            }
        }
        // variable totals, variable -> check
        std::copy(llr, llr + 208, tot.begin());
        for (size_t e = 0; e < E; e++) tot[static_cast<size_t>(g.cols[e])] += rr[e];
        for (size_t e = 0; e < E; e++) q[e] = tot[static_cast<size_t>(g.cols[e])] - rr[e];
    }
    return -1;
}

const char* acft_type_name(int t) {
    static const char* n[16] = {"unknown", "glider", "tow plane", "helicopter", "parachute", "drop plane",
                                "hang glider", "paraglider", "powered aircraft", "jet", "UFO", "balloon",
                                "airship", "UAV", "ground support", "static object"};
    return (t >= 0 && t < 16) ? n[t] : "";
}

const char* addr_prefix(int addr_type) {
    static const char* n[4] = {"RND", "ICA", "FLR", "OGN"};
    return n[addr_type & 3];
}

bool decode(const uint8_t* pkt, Packet& p) {
    p = Packet();
    uint32_t w[5];
    for (int i = 0; i < 5; i++)
        w[i] = pkt[4 * i] | (uint32_t(pkt[4 * i + 1]) << 8) | (uint32_t(pkt[4 * i + 2]) << 16) | (uint32_t(pkt[4 * i + 3]) << 24);
    const uint32_t h = w[0];
    p.addr = h & 0xFFFFFF;
    p.addr_type = (h >> 24) & 3;
    p.non_pos = (h >> 26) & 1;
    p.parity_ok = (__builtin_popcount(h & 0x0FFFFFFF) & 1) == 0;
    p.relay = (h >> 28) & 3;
    p.encrypted = (h >> 30) & 1;
    p.emergency = (h >> 31) & 1;
    if (!p.parity_ok) return false;              // the header's own parity bit
    if (p.encrypted) return true;                // the owner's key: address only
    uint32_t* d = w + 1;
    tea_decrypt_key0(d, 8);
    tea_decrypt_key0(d + 2, 8);
    if (!p.non_pos) {
        p.fix_quality = d[0] >> 30;
        p.time_s = (d[0] >> 24) & 0x3F;
        p.fix_mode = d[1] >> 31;
        p.dop = (10 + vr_decode((d[1] >> 24) & 0x3F, 4)) / 10.0;
        const int32_t lat = (sext24(d[0] & 0xFFFFFF) << 3) + 4;    // 0.0001 / 60 degrees
        const int32_t lon = (sext24(d[1] & 0xFFFFFF) << 4) + 8;
        p.lat = lat * (1e-4 / 60);
        p.lon = lon * (1e-4 / 60);
        p.pos = p.fix_quality > 0 && std::fabs(p.lat) <= 90 && std::fabs(p.lon) <= 180;
        p.alt_m = vr_decode(d[2] & 0x3FFF, 12);
        p.speed_ms = vr_decode((d[2] >> 14) & 0x3FF, 8) * 0.1;
        const int turn = (d[2] >> 24) & 0xFF;
        p.has_turn = turn != 0x80;
        p.turn_dps = svr_decode(turn, 5) * 0.1;
        p.heading = (d[3] & 0x3FF) * 360.0 / 1024;
        const int climb = (d[3] >> 10) & 0x1FF;
        p.has_climb = climb != 0x100;
        p.climb_ms = svr_decode(climb, 6) * 0.1;
        p.stealth = (d[3] >> 19) & 1;
        p.acft_type = (d[3] >> 20) & 0xF;
        const int bdiff = d[3] >> 24, bmsb = (d[1] >> 30) & 1;
        p.has_baro = bmsb || bdiff;
        if (p.has_baro) p.baro_alt_m = p.alt_m + static_cast<int16_t>(bdiff | (bmsb ? 0 : 0xFF00));
        return true;
    }
    p.report_type = (d[3] >> 20) & 0xF;
    if (p.report_type == 0) {          // device status
        p.firmware = d[3] & 0xFF;
        p.hardware = (d[3] >> 8) & 0xFF;
        p.tx_dbm = static_cast<int>((d[3] >> 16) & 0xF) + 4;
        p.voltage = vr_decode((d[3] >> 24) & 0xFF, 6) / 64.0;
        p.satellites = (d[2] >> 28) & 0xF;
        p.time_s = (d[0] >> 24) & 0x3F;
    } else if (p.report_type == 1) {   // info strings: 7-bit characters, a type code ends each
        uint8_t b[16];
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) b[4 * i + k] = static_cast<uint8_t>(d[i] >> (8 * k));
        uint8_t chk = 0;
        for (int i = 0; i < 15; i++) chk ^= b[i];
        if (chk != b[15]) return true;
        const int nch = b[14] & 0x0F;
        auto ch = [&](int idx) {
            const int bit = idx * 7, by = bit >> 3, of = bit & 7;
            int v = b[by] >> of;
            if (of > 1 && by + 1 < 14) v |= b[by + 1] << (8 - of);
            return v & 0x7F;
        };
        static const char* name[15] = {"Pilot", "Manufacturer", "Model", "Type", "Serial", "Registration",
                                       "Competition ID", "Class", "Task", "Base", "ICE", "Pilot ID",
                                       "Hardware", "Software", "Crew"};
        std::string val;
        for (int i = 0; i <= nch && i < 16; i++) {
            const int c = ch(i);
            if (c >= 0x20) { val += static_cast<char>(c); continue; }
            // personal data (pilot / crew names, emergency contact, pilot ID,
            // task) is not shown
            const bool personal = c == 0 || c == 8 || c == 10 || c == 11 || c == 14;
            if (!val.empty() && c < 15 && !personal) p.info.push_back({name[c], val});
            val.clear();
        }
    }
    return true;
}

}  // namespace ogn
