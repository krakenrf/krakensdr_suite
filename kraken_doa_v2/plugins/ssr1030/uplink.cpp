#include "uplink.hpp"

#include <array>
#include <cstdio>

namespace ssr {

namespace {
constexpr uint32_t POLY = 0xFFF409;

// the inverse of the (linear) uplink address encoding, column by column
struct Inverse {
    std::array<uint32_t, 24> col{};   // decode_address(1 << i)
    Inverse() {
        // Gauss-Jordan over GF(2): rows = enc(e_i) with their tags
        std::array<std::pair<uint32_t, uint32_t>, 24> basis{};
        std::array<bool, 24> have{};
        for (int i = 0; i < 24; i++) {
            uint32_t v = encode_address(1u << i), tag = 1u << i;
            for (int p = 23; p >= 0 && v; p--) {
                if (!((v >> p) & 1)) continue;
                if (have[static_cast<size_t>(p)]) {
                    v ^= basis[static_cast<size_t>(p)].first;
                    tag ^= basis[static_cast<size_t>(p)].second;
                } else {
                    basis[static_cast<size_t>(p)] = {v, tag};
                    have[static_cast<size_t>(p)] = true;
                    break;
                }
            }
        }
        for (int i = 0; i < 24; i++) {
            uint32_t x = 1u << i, t = 0;
            for (int p = 23; p >= 0; p--) {
                if (!((x >> p) & 1) || !have[static_cast<size_t>(p)]) continue;
                x ^= basis[static_cast<size_t>(p)].first;
                t ^= basis[static_cast<size_t>(p)].second;
            }
            col[static_cast<size_t>(i)] = t;
        }
    }
};
const Inverse& inverse() {
    static const Inverse inv;
    return inv;
}
std::string fmt(const char* f, long v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}
const char* uds_text(int uds) {
    switch (uds) {
    case 0x30: return "ACAS coordination (resolution message)";
    case 0x31: return "ACAS coordination reply request";
    case 0x32: return "ACAS broadcast";
    default: return nullptr;
    }
}
}  // namespace

uint32_t crc24(const uint8_t* bits, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t top = ((r >> 23) & 1) ^ (bits[i] & 1);
        r = (r << 1) & 0xFFFFFF;
        if (top) r ^= POLY;
    }
    return r;
}

uint32_t encode_address(uint32_t a) {
    // A(x) G(x), G = x^24 + POLY; the coefficients of x^47 .. x^24
    const uint64_t g = (uint64_t(1) << 24) | POLY;
    uint64_t prod = 0;
    for (int i = 0; i < 25; i++)
        if ((g >> i) & 1) prod ^= static_cast<uint64_t>(a & 0xFFFFFF) << i;
    return static_cast<uint32_t>((prod >> 24) & 0xFFFFFF);
}

uint32_t decode_address(uint32_t enc) {
    const Inverse& inv = inverse();
    uint32_t a = 0;
    for (int i = 0; i < 24; i++)
        if ((enc >> i) & 1) a ^= inv.col[static_cast<size_t>(i)];
    return a;
}

uint32_t Uplink::field(int first, int len) const {
    uint32_t v = 0;
    for (int i = 0; i < len; i++) v = (v << 1) | (b[first - 1 + i] & 1u);
    return v;
}

namespace {
// what flipping bit k does to crc24(data) ^ AP (the code is linear)
struct Syndromes {
    std::array<uint32_t, 56> s56{};
    std::array<uint32_t, 112> s112{};
    Syndromes() {
        for (int n : {56, 112}) {
            uint8_t b[112] = {};
            for (int k = 0; k < n; k++) {
                uint32_t v;
                if (k < n - 24) {
                    b[k] = 1;
                    v = crc24(b, n - 24);
                    b[k] = 0;
                } else {
                    v = 1u << (n - 1 - k);
                }
                (n == 56 ? s56[static_cast<size_t>(k)] : s112[static_cast<size_t>(k)]) = v;
            }
        }
    }
};
const Syndromes& syndromes() {
    static const Syndromes s;
    return s;
}
}  // namespace

// An all-call (UF11) / ACAS broadcast (UF16) has a KNOWN address: its parity
// tells where the bits went wrong. The sequence estimator's errors are a
// run of chips with the wrong sign - two bit errors 1-3 bits apart (a phase
// reversal moved, missed or added) - or, for a wrong run up to the frame's
// end, one bit in its last 8: those are corrected (never in the UF field).
// ~3 x 56..112 + 8 patterns of 2^24 syndromes: a random frame passes as
// corrected ~1 in 50000
static bool correct_known(Uplink& u, uint32_t want) {
    const int n = u.nbits;
    const uint32_t x = crc24(u.b, n - 24) ^ u.field(n - 23, 24) ^ encode_address(want);
    if (!x) return true;
    const uint32_t* sy = n == 56 ? syndromes().s56.data() : syndromes().s112.data();
    for (int i = 5; i < n; i++) {
        if (sy[i] == x && i >= n - 8) {
            u.b[i] ^= 1;
            u.corrected = 1;
            return true;
        }
        for (int j = i + 1; j < n && j <= i + 3; j++)
            if ((sy[i] ^ sy[j]) == x) {
                u.b[i] ^= 1;
                u.b[j] ^= 1;
                u.corrected = 2;
                return true;
            }
    }
    return false;
}

void parse_uplink(Uplink& u) {
    const int n = u.nbits;
    u.corrected = 0;
    u.uf = static_cast<int>(u.field(1, 5));
    if (n == 112 && (u.uf >> 3) == 3) u.uf = 24;   // Comm-C ELM: "11" + RC
    if ((u.uf == 11 && n == 56) || (u.uf == 16 && n == 112)) correct_known(u, ALL_CALL);
    u.ap = u.field(n - 23, 24);
    const uint32_t enc = crc24(u.b, n - 24) ^ u.ap;
    u.address = decode_address(enc);
    u.broadcast = u.address == ALL_CALL;
    u.verified = u.broadcast;
    u.ii = u.si = -1;
    u.acas_sender = 0;
    u.uds = -1;
    if (u.uf == 11 && n == 56) {
        const int ic = static_cast<int>(u.field(10, 4)), cl = static_cast<int>(u.field(14, 3));
        if (cl == 0) u.ii = ic;
        else if (cl <= 4) u.si = (cl - 1) * 16 + ic;
    } else if ((u.uf == 4 || u.uf == 5 || u.uf == 20 || u.uf == 21) && n == (u.uf >= 20 ? 112 : 56)) {
        const int di = static_cast<int>(u.field(14, 3));
        if (di == 0 || di == 1 || di == 7) u.ii = static_cast<int>(u.field(17, 4));   // IIS
        else if (di == 3) u.si = static_cast<int>(u.field(17, 6));                    // SIS
        if (u.si == 0) u.si = -1;
    } else if (u.uf == 16 && n == 112) {
        u.uds = static_cast<int>(u.field(33, 8));
        u.acas_sender = u.field(65, 24);   // MID / the broadcasting aircraft's address
    }
}

std::string Uplink::kind() const {
    switch (uf) {
    case 0: return "UF0 short air-air surveillance (ACAS)";
    case 4: return "UF4 surveillance, altitude request";
    case 5: return "UF5 surveillance, identity request";
    case 11: return "UF11 Mode S only all-call";
    case 16: {
        const char* t = uds_text(uds);
        return std::string("UF16 long air-air (ACAS)") + (t ? std::string(", ") + t : "");
    }
    case 19: return "UF19 military";
    case 20: return "UF20 Comm-A, altitude request";
    case 21: return "UF21 Comm-A, identity request";
    case 24: return "UF24 Comm-C (ELM)";
    default: return "UF" + std::to_string(uf);
    }
}

std::string Uplink::hex() const {
    std::string s;
    for (int i = 0; i < nbits; i += 4) s += "0123456789ABCDEF"[field(i + 1, 4)];
    return s;
}

std::vector<std::pair<std::string, std::string>> Uplink::fields() const {
    std::vector<std::pair<std::string, std::string>> f;
    auto addr = [&]() {
        if (broadcast) return std::string(uf == 11 ? "all-call (FFFFFF)" : "broadcast (FFFFFF)");
        return fmt("%06lX", static_cast<long>(address));
    };
    f.push_back({"Format", kind()});
    if (uf == 0) {
        f.push_back({"RL", field(9, 1) ? "1 (reply with DF16)" : "0 (reply with DF0)"});
        f.push_back({"AQ", fmt("%ld", static_cast<long>(field(14, 1)))});
        f.push_back({"DS", fmt("%02lX", static_cast<long>(field(15, 8)))});
    } else if (uf == 4 || uf == 5 || uf == 20 || uf == 21) {
        static const char* PC[] = {"no changes", "non-selective all-call lockout", "not assigned", "not assigned",
                                   "cancel B", "cancel C", "cancel D", "not assigned"};
        f.push_back({"PC", fmt("%ld ", static_cast<long>(field(6, 3))) + PC[field(6, 3)]});
        f.push_back({"RR", fmt("%ld", static_cast<long>(field(9, 5)))});
        f.push_back({"DI", fmt("%ld", static_cast<long>(field(14, 3)))});
        f.push_back({"SD", fmt("%04lX", static_cast<long>(field(17, 16)))});
        if (ii >= 0) f.push_back({"Interrogator", fmt("II %ld", ii) + " (IIS)"});
        if (si >= 0) f.push_back({"Interrogator", fmt("SI %ld", si) + " (SIS)"});
        if (nbits == 112) f.push_back({"MA", [&] {
                                     std::string s;
                                     for (int i = 33; i < 89; i += 4) s += "0123456789ABCDEF"[field(i, 4)];
                                     return s;
                                 }()});
    } else if (uf == 11) {
        f.push_back({"PR", fmt("%ld", static_cast<long>(field(6, 4)))});
        f.push_back({"IC / CL", fmt("%ld", static_cast<long>(field(10, 4))) + fmt(" / %ld", static_cast<long>(field(14, 3)))});
        if (ii >= 0) f.push_back({"Interrogator", fmt("II %ld", ii)});
        if (si >= 0) f.push_back({"Interrogator", fmt("SI %ld", si)});
    } else if (uf == 16) {
        f.push_back({"RL", fmt("%ld", static_cast<long>(field(9, 1)))});
        f.push_back({"AQ", fmt("%ld", static_cast<long>(field(14, 1)))});
        f.push_back({"UDS", fmt("%02lX", uds)});
        if (acas_sender) f.push_back({"From (ACAS)", fmt("%06lX", static_cast<long>(acas_sender))});
    }
    f.push_back({"Address", addr() + (verified ? " - parity checked" : broadcast ? "" : " (from the parity, not checkable)")});
    f.push_back({"Bits", hex()});
    return f;
}

}  // namespace ssr
