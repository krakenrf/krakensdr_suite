// DAB Layer II frame checks (see audio_mp2.hpp). Tables from ETSI TS 103 466:
// bit rates tables 10 / 11, sampling frequencies table 13, the nbal columns of
// the bit allocation tables 4 / 5 (48 kHz) and 6 (24 kHz), ScF-CRC sub-band
// groups annex B.3.

#include "audio_mp2.hpp"
#include "audio_bits.hpp"

namespace dab::audio {

namespace {

constexpr int BR48[16] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
constexpr int BR24[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};

// nbal per sub-band: table 4 (48 kHz, >= 56 kbit/s per channel, sblimit 27),
// table 5 (48 kHz, < 56 kbit/s per channel, sblimit 8), table 6 (24 kHz, sblimit 30)
constexpr uint8_t NBAL_T4[27] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2};
constexpr uint8_t NBAL_T5[8] = {4, 4, 3, 3, 3, 3, 3, 3};
constexpr uint8_t NBAL_T6[30] = {4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

inline bool high_rate_48k(const Mp2Header& h) {
    return h.channels == 1 ? h.bitrate_kbps >= 56 : h.bitrate_kbps >= 112;
}

}   // namespace

bool parse_mp2_header(const uint8_t* p, Mp2Header& h) {
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) return false;   // syncword
    if (((p[1] >> 1) & 3) != 2) return false;                   // Layer II
    if (((p[2] >> 2) & 3) != 1) return false;                   // 48 / 24 kHz
    if ((p[3] & 3) != 0) return false;                          // no emphasis
    if (p[1] & 1) return false;                                 // DAB always has the header CRC
    const int bri = p[2] >> 4;
    h.mpeg1 = (p[1] >> 3) & 1;
    h.protection = !(p[1] & 1);
    h.bitrate_kbps = h.mpeg1 ? BR48[bri] : BR24[bri];
    if (h.bitrate_kbps == 0) return false;
    h.sample_rate = h.mpeg1 ? 48000 : 24000;
    h.mode = p[3] >> 6;
    h.mode_ext = (p[3] >> 4) & 3;
    h.channels = h.mode == 3 ? 1 : 2;
    h.frame_bytes = h.mpeg1 ? h.bitrate_kbps * 3 : h.bitrate_kbps * 6;
    h.scf_crcs = (!h.mpeg1 || high_rate_48k(h)) ? 4 : 2;
    return true;
}

void parse_mp2_side_info(const uint8_t* d, size_t n, const Mp2Header& h, Mp2SideInfo& o) {
    o = Mp2SideInfo{};
    o.groups = h.scf_crcs;
    if (n < 6) { o.overrun = true; return; }
    const uint8_t* nbal;
    int sblimit;
    if (!h.mpeg1) { nbal = NBAL_T6; sblimit = 30; }
    else if (high_rate_48k(h)) { nbal = NBAL_T4; sblimit = 27; }
    else { nbal = NBAL_T5; sblimit = 8; }
    const int nch = h.channels;
    int bound = sblimit;
    if (h.mode == 1) bound = 4 * (h.mode_ext + 1);
    if (bound > sblimit) bound = sblimit;

    // the header CRC covers header bits 16..31, the bit allocation and the ScFSI
    uint32_t crc = 0xFFFF;
    crc = crc_bits<16>(crc, 0x8005, (static_cast<uint32_t>(d[2]) << 8) | d[3], 16);
    const size_t start = h.protection ? 6 : 4;
    BitReader br(d + start, n - start);

    uint8_t alloc[2][32] = {}, scfsi[2][32] = {};
    for (int sb = 0; sb < sblimit; sb++) {
        int nb = nbal[sb];
        if (sb < bound) {
            for (int ch = 0; ch < nch; ch++) {
                alloc[ch][sb] = static_cast<uint8_t>(br.get(nb));
                crc = crc_bits<16>(crc, 0x8005, alloc[ch][sb], nb);
            }
        } else {
            uint8_t a = static_cast<uint8_t>(br.get(nb));
            crc = crc_bits<16>(crc, 0x8005, a, nb);
            alloc[0][sb] = alloc[1][sb] = a;
        }
    }
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++)
            if (alloc[ch][sb]) {
                scfsi[ch][sb] = static_cast<uint8_t>(br.get(2));
                crc = crc_bits<16>(crc, 0x8005, scfsi[ch][sb], 2);
            }
    if (h.protection) {
        o.crc_present = true;
        o.crc_ok = crc == ((static_cast<uint32_t>(d[4]) << 8) | d[5]);
    }
    // scale factors, in bit stream order; their 3 MSbs feed the group's CRC-8
    uint32_t g[4] = {0, 0, 0, 0};
    for (int sb = 0; sb < sblimit; sb++) {
        // ScF-CRC groups: sub-bands 0-3, 4-7, 8-15, 16-(sblimit-1)
        int grp = sb < 4 ? 0 : sb < 8 ? 1 : sb < 16 ? 2 : 3;
        for (int ch = 0; ch < nch; ch++) {
            if (!alloc[ch][sb]) continue;
            int cnt = scfsi[ch][sb] == 0 ? 3 : scfsi[ch][sb] == 2 ? 1 : 2;
            for (int k = 0; k < cnt; k++) {
                uint32_t scf = br.get(6);
                g[grp] = crc_bits<8>(g[grp], 0x1D, scf >> 3, 3);
            }
        }
    }
    for (int i = 0; i < 4; i++) o.scf_crc[i] = static_cast<uint8_t>(g[i]);
    // the side information must end before the PAD / ScF-CRC / F-PAD bytes
    if (br.overrun() || start * 8 + br.pos() > (n - 2 - static_cast<size_t>(h.scf_crcs)) * 8) o.overrun = true;
}

}   // namespace dab::audio
