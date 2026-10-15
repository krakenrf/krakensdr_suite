// LoRaWAN MAC header parsing - CLEAR fields only (MHDR, DevAddr, FCtrl,
// FCnt, FPort; join-request DevEUI / JoinEUI / DevNonce). The FRMPayload is
// encrypted with keys we do not have and the MIC cannot be checked: headers
// are shown, nothing is decrypted. From the LoRaWAN 1.0.x / 1.1 MAC spec
// (public). Clean-room.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lorawan {

enum MType { JOIN_REQ = 0, JOIN_ACC = 1, UNCONF_UP = 2, UNCONF_DN = 3, CONF_UP = 4, CONF_DN = 5, RFU = 6, PROPRIETARY = 7 };

inline const char* mtype_name(int m) {
    static const char* n[8] = {"Join request", "Join accept", "Unconfirmed up", "Unconfirmed down",
                               "Confirmed up", "Confirmed down", "RFU", "Proprietary"};
    return n[m & 7];
}

struct Frame {
    bool ok = false;
    int mtype = 0;
    int major = 0;
    // data frames
    uint32_t devaddr = 0;
    uint8_t fctrl = 0;
    uint16_t fcnt = 0;
    int fport = -1;        // -1 = none
    bool adr = false;
    int fopts_len = 0;
    int frmpayload_len = 0;
    // join request
    uint64_t joineui = 0, deveui = 0;
    uint16_t devnonce = 0;
};

// A plausibility check: LoRaWAN uses LoRa with explicit header + CRC; the
// MHDR major must be 0, mtype one of the known ones, and the lengths add up.
inline Frame parse(const uint8_t* p, size_t n) {
    Frame f;
    if (n < 5) return f;
    const uint8_t mhdr = p[0];
    f.mtype = mhdr >> 5;
    f.major = mhdr & 0x3;
    if (f.major != 0) return f;
    if (f.mtype == JOIN_REQ) {
        if (n < 1 + 8 + 8 + 2 + 4) return f;
        auto u64 = [&](int o) {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[o + i]) << (8 * i);
            return v;
        };
        f.joineui = u64(1);
        f.deveui = u64(9);
        f.devnonce = static_cast<uint16_t>(p[17] | (p[18] << 8));
        f.ok = true;
        return f;
    }
    if (f.mtype == UNCONF_UP || f.mtype == UNCONF_DN || f.mtype == CONF_UP || f.mtype == CONF_DN) {
        // MHDR(1) DevAddr(4) FCtrl(1) FCnt(2) [FOpts] [FPort(1) FRMPayload] MIC(4)
        if (n < 1 + 4 + 1 + 2 + 4) return f;
        f.devaddr = static_cast<uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16) | (uint32_t(p[4]) << 24));
        f.fctrl = p[5];
        f.adr = f.fctrl & 0x80;
        f.fopts_len = f.fctrl & 0x0F;
        f.fcnt = static_cast<uint16_t>(p[6] | (p[7] << 8));
        const int after_fhdr = 8 + f.fopts_len;
        const int macpl_end = static_cast<int>(n) - 4;   // minus MIC
        if (after_fhdr > macpl_end) return f;
        if (after_fhdr < macpl_end) {
            f.fport = p[after_fhdr];
            f.frmpayload_len = macpl_end - after_fhdr - 1;
        } else {
            f.fport = -1;
            f.frmpayload_len = 0;
        }
        f.ok = true;
        return f;
    }
    return f;
}

}  // namespace lorawan
