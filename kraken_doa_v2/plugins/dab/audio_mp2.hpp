#pragma once
// DAB audio frames: MPEG-1 / MPEG-2 LSF Audio Layer II as ETSI TS 103 466
// carries them. Header validation (clause 5.3.1.3), the header CRC (annex
// B.2) and the scale factor CRC (ScF-CRC, annex B.3), which needs the bit
// allocation, ScFSI and scale factors parsed here (the PCM decoding itself is
// minimp3's).

#include <cstddef>
#include <cstdint>

namespace dab::audio {

struct Mp2Header {
    bool mpeg1 = false;       // ID: 1 = ISO/IEC 11172-3 (48 kHz), 0 = 13818-3 LSF (24 kHz)
    bool protection = false;  // header CRC present (protection_bit == 0)
    int bitrate_kbps = 0;
    int sample_rate = 0;      // 48000 / 24000
    int mode = 0;             // 0 stereo, 1 joint stereo, 2 dual channel, 3 single channel
    int mode_ext = 0;
    int channels = 0;
    int frame_bytes = 0;      // 3 * bit rate (48 kHz) or 6 * bit rate (24 kHz)
    int scf_crcs = 0;         // number of ScF-CRC bytes (2 or 4) in front of the F-PAD
};

// Parses the 4 header bytes. Accepts only what DAB uses: Layer II, the
// header CRC present (protection_bit 0), the sampling_frequency code "01",
// a bit rate index 1..14, emphasis "00".
bool parse_mp2_header(const uint8_t* h, Mp2Header& out);

struct Mp2SideInfo {
    bool crc_present = false;
    bool crc_ok = false;          // header CRC matched (only when present)
    uint8_t scf_crc[4] = {};      // the ScF-CRC of this frame's scale factors, per sub-band group
    int groups = 0;               // 2 or 4
    bool overrun = false;         // side information ran past the frame: corrupt
};

// Parses the bit allocation, ScFSI and scale factors of a frame (d, n:
// the whole audio frame) and computes the header CRC check and this frame's
// ScF-CRC words.
void parse_mp2_side_info(const uint8_t* d, size_t n, const Mp2Header& h, Mp2SideInfo& out);

// The ScF-CRC words a frame carries for the FOLLOWING frame: crc[i] =
// ScF-CRC i (bytes L-3-i of the frame, L = frame length).
inline void mp2_carried_scf_crc(const uint8_t* d, size_t n, int groups, uint8_t* crc) {
    for (int i = 0; i < groups && i < 4; i++) crc[i] = d[n - 3 - static_cast<size_t>(i)];
}

}   // namespace dab::audio
