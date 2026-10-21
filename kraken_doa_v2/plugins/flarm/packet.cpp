// FLARM / OGN packet checks and the OGN tracker protocol's fields (see
// packet.hpp). Field layout, units and "absent" codes from the OGN wiki's
// tracking-protocol page; TEA from Wheeler & Needham (1994, public domain);
// normalised min-sum after Chen & Fossorier (2002).

#include "packet.hpp"

#include "kraken_dsp.hpp"
#include "ogn_ldpc_matrix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pkt {

namespace {

// --- the LDPC code's Tanner graph, from the matrix table ------------------------
struct Graph {
    uint8_t mask[ogn::LDPC_ROWS][BYTES] = {};
    int row0[ogn::LDPC_ROWS + 1] = {};   // edges of row r: row0[r] .. row0[r + 1] - 1
    std::vector<uint16_t> col;           // the column (transmitted bit) of every edge
};

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

const Graph& graph() {
    static const Graph g = [] {
        Graph x;
        for (int r = 0; r < ogn::LDPC_ROWS; r++) {
            const char* h = ogn::LDPC_MATRIX_HEX[r];
            x.row0[r] = static_cast<int>(x.col.size());
            for (int k = 0; k < BYTES; k++) {
                x.mask[r][k] = static_cast<uint8_t>(hexval(h[2 * k]) << 4 | hexval(h[2 * k + 1]));
                for (int b = 0; b < 8; b++)
                    if (x.mask[r][k] & (0x80 >> b)) x.col.push_back(static_cast<uint16_t>(8 * k + b));
            }
        }
        x.row0[ogn::LDPC_ROWS] = static_cast<int>(x.col.size());
        return x;
    }();
    return g;
}

// every check on hard bits, one bit per byte (0 / 1)
bool checks_ok_bits(const uint8_t* bit) {
    const Graph& g = graph();
    for (int r = 0; r < ogn::LDPC_ROWS; r++) {
        int p = 0;
        for (int e = g.row0[r]; e < g.row0[r + 1]; e++) p ^= bit[g.col[static_cast<size_t>(e)]];
        if (p) return false;
    }
    return true;
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// --- variable-resolution numbers ---------------------------------------------------
// A B-bit mantissa under a 2-bit range: steps of 1 / 2 / 4 / 8, the middle of
// the step returned (the plugin's convention since 1.0)
int vr(uint32_t v, int B) {
    const int T = 1 << B;
    const int m = static_cast<int>(v & static_cast<uint32_t>(T - 1));
    switch ((v >> B) & 3) {
        case 0: return m;
        case 1: return T + 1 + 2 * m;
        case 2: return 3 * T + 2 + 4 * m;
        default: return 7 * T + 4 + 8 * m;
    }
}
// signed: the sign bit right above the range field
int svr(uint32_t v, int B) {
    const int mag = vr(v & ((1u << (B + 2)) - 1), B);
    return (v >> (B + 2)) & 1 ? -mag : mag;
}
int32_t s24(uint32_t v) {
    v &= 0xFFFFFF;
    return v & 0x800000 ? static_cast<int32_t>(v) - (1 << 24) : static_cast<int32_t>(v);
}

// info-report field names by type code; nullptr = personal data, never kept
const char* const INFO_NAMES[15] = {nullptr,          "Manufacturer", "Model", "Type",    "Serial",
                                    "Registration",   "Competition ID", "Class", nullptr, "Base",
                                    nullptr,          nullptr,        "Hardware", "Software", nullptr};

void decode_position(const uint32_t* D, OgnContent* c) {
    c->report = -1;
    const int32_t la = s24(D[0]), lo = s24(D[1]);
    c->second = static_cast<int>((D[0] >> 24) & 63);
    c->fix_quality = static_cast<int>(D[0] >> 30);
    const double lat = (8.0 * la + 4) * 1e-4 / 60, lon = (16.0 * lo + 8) * 1e-4 / 60;
    c->dop = (10 + vr((D[1] >> 24) & 63, 4)) / 10.0;
    const bool baro_msb = (D[1] >> 30) & 1;
    c->fix3d = (D[1] >> 31) & 1;
    c->alt_m = vr(D[2] & 0x3FFF, 12);
    c->speed_ms = vr((D[2] >> 14) & 0x3FF, 8) / 10.0;
    const uint32_t tc = D[2] >> 24;
    c->turn_dps = tc == 0x80 ? NAN : svr(tc, 5) / 10.0;
    c->track_deg = (D[3] & 0x3FF) * 360.0 / 1024;
    const uint32_t cc = (D[3] >> 10) & 0x1FF;
    c->climb_ms = cc == 0x100 ? NAN : svr(cc, 6) / 10.0;
    c->stealth = (D[3] >> 19) & 1;
    c->acft_type = static_cast<int>((D[3] >> 20) & 15);
    // pressure altitude: 9-bit difference to the GNSS altitude, the MSB in
    // W1 (set = 0..255), both zero = absent
    const int b = static_cast<int>(D[3] >> 24);
    c->baro = baro_msb || b != 0;
    if (c->baro) c->palt_m = c->alt_m + (baro_msb ? b : b - 256);
    c->lat = lat;
    c->lon = lon;
    c->fix = c->fix_quality > 0 && std::fabs(lat) <= 90 && std::fabs(lon) <= 180;
}

void decode_status(const uint32_t* D, OgnContent* c) {
    const int fw = static_cast<int>(D[3] & 0xFF), hw = static_cast<int>((D[3] >> 8) & 0xFF);
    const double tx_dbm = ((D[3] >> 16) & 15) + 4.0;
    const double batt_v = vr(D[3] >> 24, 6) / 64.0;
    const double sats = (D[2] >> 28) & 15;
    char s[128];
    snprintf(s, sizeof s, "firmware %d, hardware %d, battery %.2f V, %.0f dBm, %.0f satellites", fw, hw, batt_v, tx_dbm,
             sats);
    c->status = s;
}

void decode_info(const uint32_t* D, OgnContent* c) {
    uint8_t b[16];
    for (int w = 0; w < 4; w++)
        for (int k = 0; k < 4; k++) b[4 * w + k] = static_cast<uint8_t>(D[w] >> (8 * k));
    uint8_t x = 0;
    for (int k = 0; k < 15; k++) x ^= b[k];
    c->info_ok = x == b[15];
    if (!c->info_ok) return;
    // 7-bit characters packed LSB first into the 112-bit little-endian c0..c13
    const int n = (b[14] & 0x0F) + 1;
    std::string cur;
    for (int i = 0; i < n; i++) {
        int ch = 0;
        for (int k = 0; k < 7; k++) {
            const int bit = 7 * i + k;
            ch |= ((b[bit / 8] >> (bit % 8)) & 1) << k;
        }
        if (ch >= 0x20) {
            cur.push_back(static_cast<char>(ch));
            continue;
        }
        // a control character ends the string and names its field
        if (!cur.empty() && ch < 15 && INFO_NAMES[ch]) c->info.emplace_back(INFO_NAMES[ch], kp::printable(cur));
        cur.clear();
    }
}

}  // namespace

const char* type_name(int t) {
    static const char* const N[16] = {"unknown",    "glider",      "tow plane", "helicopter", "parachute", "drop plane",
                                      "hang glider", "paraglider", "powered aircraft", "jet", "UFO",        "balloon",
                                      "airship",    "UAV",         "ground support", "static object"};
    return t >= 0 && t < 16 ? N[t] : "";
}

uint16_t crc16_flarm(const uint8_t* payload24) {
    // over the nRF905 address 31 FA B6, then the payload
    static const uint8_t ADDR[3] = {0x31, 0xFA, 0xB6};
    uint8_t buf[27];
    std::memcpy(buf, ADDR, 3);
    std::memcpy(buf + 3, payload24, 24);
    return static_cast<uint16_t>(kp::crc_bytes(buf, sizeof buf, 16, 0x1021, 0xFFFF, false, false, 0));
}

bool accept_flarm(const uint8_t* bytes, Frame* f) {
    uint8_t b[BYTES];
    bool inv = false;
    std::memcpy(b, bytes, BYTES);
    if (crc16_flarm(b) != (b[24] << 8 | b[25])) {
        for (auto& v : b) v = static_cast<uint8_t>(~v);
        if (crc16_flarm(b) != (b[24] << 8 | b[25])) return false;
        inv = true;
    }
    *f = Frame{};
    f->proto = Proto::FLARM;
    std::memcpy(f->b, b, BYTES);
    f->inverted = inv;
    f->addr = static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 | static_cast<uint32_t>(b[2]) << 16;
    f->addr_type = (b[3] >> 4) & 3;
    return true;
}

bool ldpc_ok(const uint8_t* bytes) {
    const Graph& g = graph();
    for (int r = 0; r < ogn::LDPC_ROWS; r++) {
        int p = 0;
        for (int k = 0; k < BYTES; k++) p += __builtin_popcount(bytes[k] & g.mask[r][k]);
        if (p & 1) return false;
    }
    return true;
}

void tea_decrypt(uint32_t* v0, uint32_t* v1) {
    // all-zero key, 8 cycles: the round function is (v << 4) ^ (v + sum) ^ (v >> 5)
    constexpr uint32_t DELTA = 0x9E3779B9u;
    uint32_t a = *v0, b = *v1, sum = DELTA * 8u;
    for (int i = 0; i < 8; i++) {
        b -= (a << 4) ^ (a + sum) ^ (a >> 5);
        a -= (b << 4) ^ (b + sum) ^ (b >> 5);
        sum -= DELTA;
    }
    *v0 = a;
    *v1 = b;
}

bool accept_ogn(const uint8_t* bytes, const float* soft, float amp, Frame* f) {
    uint8_t b[BYTES];
    std::memcpy(b, bytes, BYTES);
    int corrected = 0;
    if (!ldpc_ok(b)) {
        // normalised min-sum, flooding schedule
        constexpr int MAX_ITER = 16, MAX_CHANGED = 16;
        constexpr float SCALE = 0.75f;
        const Graph& g = graph();
        const size_t ne = g.col.size();
        float L[BITS], tot[BITS];
        uint8_t hard0[BITS], hard[BITS];
        const float k = 2.0f / std::max(amp, 1e-6f);
        for (int j = 0; j < BITS; j++) {
            L[j] = -soft[j] * k;   // > 0 = a 0
            hard0[j] = soft[j] > 0;
        }
        thread_local std::vector<float> q, r;
        q.resize(ne);
        r.assign(ne, 0.0f);
        for (size_t e = 0; e < ne; e++) q[e] = L[g.col[e]];
        bool ok = false;
        for (int it = 0; it < MAX_ITER && !ok; it++) {
            for (int row = 0; row < ogn::LDPC_ROWS; row++) {
                const int e0 = g.row0[row], e1 = g.row0[row + 1];
                float m1 = INFINITY, m2 = INFINITY;
                int i1 = -1;
                bool neg = false;
                for (int e = e0; e < e1; e++) {
                    const float v = q[static_cast<size_t>(e)], a = std::fabs(v);
                    neg ^= v < 0;
                    if (a < m1) {
                        m2 = m1;
                        m1 = a;
                        i1 = e;
                    } else if (a < m2) {
                        m2 = a;
                    }
                }
                for (int e = e0; e < e1; e++) {
                    const float v = q[static_cast<size_t>(e)];
                    const float mag = SCALE * (e == i1 ? m2 : m1);
                    r[static_cast<size_t>(e)] = (neg ^ (v < 0)) ? -mag : mag;
                }
            }
            std::copy(L, L + BITS, tot);
            for (size_t e = 0; e < ne; e++) tot[g.col[e]] += r[e];
            for (size_t e = 0; e < ne; e++) q[e] = tot[g.col[e]] - r[e];
            for (int j = 0; j < BITS; j++) hard[j] = tot[j] < 0;
            ok = checks_ok_bits(hard);
        }
        if (!ok) return false;
        for (int j = 0; j < BITS; j++) corrected += hard[j] != hard0[j];
        if (corrected > MAX_CHANGED) return false;
        std::memset(b, 0, BYTES);
        for (int j = 0; j < BITS; j++)
            if (hard[j]) b[j / 8] |= static_cast<uint8_t>(0x80 >> (j % 8));
    }
    const uint32_t w0 = le32(b);
    // header parity: bits 0..27 of W0 even
    if (__builtin_popcount(w0 & 0x0FFFFFFFu) & 1) return false;
    *f = Frame{};
    f->proto = Proto::OGN;
    std::memcpy(f->b, b, BYTES);
    f->corrected = corrected;
    f->addr = w0 & 0xFFFFFF;
    f->addr_type = static_cast<int>((w0 >> 24) & 3);
    f->non_position = (w0 >> 26) & 1;
    f->relay = static_cast<int>((w0 >> 28) & 3);
    f->encrypted = (w0 >> 30) & 1;
    f->emergency = (w0 >> 31) & 1;
    if (f->encrypted) return true;   // the owner's own format: address only
    uint32_t D[4] = {le32(b + 4), le32(b + 8), le32(b + 12), le32(b + 16)};
    tea_decrypt(&D[0], &D[1]);
    tea_decrypt(&D[2], &D[3]);
    if (!f->non_position) {
        decode_position(D, &f->ogn);
    } else {
        f->ogn.report = static_cast<int>((D[3] >> 20) & 15);
        if (f->ogn.report == 0) decode_status(D, &f->ogn);
        else if (f->ogn.report == 1) decode_info(D, &f->ogn);
    }
    return true;
}

}  // namespace pkt
