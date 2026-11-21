// PAD, X-PAD and the dynamic label (see audio_pad.hpp). Written from ETSI EN
// 300 401 V2.2.1 clause 7.4, ETSI TS 102 980 V2.1.2 clause 7 and ETSI TS 101
// 756 V2.5.1 (table 1, annex C).

#include "audio_pad.hpp"
#include "audio_bits.hpp"

#include <algorithm>
#include <cstring>

namespace dab::audio {

// ETSI TS 101 756 V2.5.1 table C.2, transcribed from the standard and checked
// against its table C.1 (codes 0x00, 0x0A, 0x0B and 0x1F are reserved)
const uint16_t EBU_LATIN[256] = {
    0x0000, 0x0118, 0x012E, 0x0172, 0x0102, 0x0116, 0x010E, 0x0218,
    0x021A, 0x010A, 0x0000, 0x0000, 0x0120, 0x0139, 0x017B, 0x0143,
    0x0105, 0x0119, 0x012F, 0x0173, 0x0103, 0x0117, 0x010F, 0x0219,
    0x021B, 0x010B, 0x0147, 0x011A, 0x0121, 0x013A, 0x017C, 0x0000,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0142, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x016E, 0x005D, 0x0141, 0x005F,
    0x0104, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00AB, 0x016F, 0x00BB, 0x013D, 0x0126,
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2,
    0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x00DF, 0x00A1, 0x0178,
    0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6,
    0x00FB, 0x00FC, 0x00F1, 0x00E7, 0x015F, 0x011F, 0x0131, 0x00FF,
    0x0136, 0x0145, 0x00A9, 0x0122, 0x011E, 0x011B, 0x0148, 0x0151,
    0x0150, 0x20AC, 0x00A3, 0x0024, 0x0100, 0x0112, 0x012A, 0x016A,
    0x0137, 0x0146, 0x013B, 0x0123, 0x013C, 0x0130, 0x0144, 0x0171,
    0x0170, 0x00BF, 0x013E, 0x00B0, 0x0101, 0x0113, 0x012B, 0x016B,
    0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC, 0x00D3, 0x00D2,
    0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F,
    0x00C2, 0x00C4, 0x00CA, 0x00CB, 0x00CE, 0x00CF, 0x00D4, 0x00D6,
    0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140,
    0x00C3, 0x00C5, 0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8,
    0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0164, 0x00F0,
    0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8,
    0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A, 0x0165, 0x0127,
};

namespace {

void put_cp(std::string& o, uint32_t cp) {
    if (cp == 0x0A || cp == 0x0B) cp = 0x20;                       // line break / end of headline
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return;   // other controls, word break
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// application type that continues a data sub-field of type `app`
int continuation(int app) {
    if (app == 2 || app == 12 || app == 14) return app + 1;
    return app;
}

constexpr size_t XPAD_LEN[8] = {4, 6, 8, 12, 16, 24, 32, 48};

}   // namespace

std::string dl_to_utf8(const uint8_t* d, size_t n, int charset) {
    std::string o;
    o.reserve(n * 2);
    if (charset == 6) {               // UCS-2 / UTF-16 big endian, BMP only
        for (size_t i = 0; i + 1 < n; i += 2) put_cp(o, (static_cast<uint32_t>(d[i]) << 8) | d[i + 1]);
    } else if (charset == 15) {       // UTF-8, validated
        size_t i = 0;
        while (i < n) {
            uint8_t c = d[i];
            int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
            uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
            bool ok = len > 0 && i + static_cast<size_t>(len) <= n;
            for (int k = 1; ok && k < len; k++) {
                if ((d[i + k] & 0xC0) != 0x80) ok = false;
                else cp = (cp << 6) | (d[i + k] & 0x3Fu);
            }
            static constexpr uint32_t MIN[5] = {0, 0, 0x80, 0x800, 0x10000};
            if (ok && (cp < MIN[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
            if (!ok) { put_cp(o, 0xFFFD); i++; continue; }
            put_cp(o, cp);
            i += static_cast<size_t>(len);
        }
    } else {                          // 0: EBU Latin based repertoire (also for unregistered values)
        for (size_t i = 0; i < n; i++) {
            uint8_t c = d[i];
            if (c == 0x0A || c == 0x0B) put_cp(o, c);   // control codes of the label
            else if (EBU_LATIN[c]) put_cp(o, EBU_LATIN[c]);
        }
    }
    size_t a = o.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    size_t b = o.find_last_not_of(' ');
    return o.substr(a, b - a + 1);
}

void PadDecoder::reset() {
    last_app_ = -1;
    last_len_ = 0;
    dg_len_ = 0;
    dg_active_ = false;
    seg_have_ = 0;
    seg_total_ = 0;
    toggle_ = -1;
    suppress_toggle_ = -1;
    charset_ = 0;
    label_.clear();
}

void PadDecoder::lost() {
    last_app_ = -1;
    dg_active_ = false;
    dg_len_ = 0;
}

void PadDecoder::feed(const uint8_t fpad[2], const uint8_t* xpad, size_t avail, bool exact_len) {
    if ((fpad[0] >> 6) != 0) return;            // F-PAD type other than "00": no X-PAD indicator
    const int ind = (fpad[0] >> 4) & 3;
    const bool ci = (fpad[1] >> 1) & 1;
    // logical X-PAD byte i (the field is transmitted in reverse byte order)
    uint8_t L[196 + 4];
    const size_t navail = std::min<size_t>(avail, sizeof L);
    for (size_t i = 0; i < navail; i++) L[i] = xpad[avail - 1 - i];

    if (ind == 1) {                              // short X-PAD: 4 bytes
        if (navail < 4) { last_app_ = -1; return; }
        if (ci) {
            int app = L[0] & 0x1F;
            if (app == 0) return;                // end marker: no data in this field
            last_app_ = app;
            subfield(app, L + 1, 3);
        } else if (last_app_ >= 0) {
            last_app_ = continuation(last_app_);
            subfield(last_app_, L, 4);
        }
        return;
    }
    if (ind != 2) {                              // no X-PAD (or reserved)
        return;
    }
    // variable size X-PAD
    if (ci) {
        int apps[4];
        size_t lens[4];
        int nsub = 0;
        size_t i = 0;
        while (i < 4 && i < navail) {
            uint8_t c = L[i++];
            int app = c & 0x1F;
            if (app == 0) break;                 // end marker
            apps[nsub] = app;
            lens[nsub] = XPAD_LEN[c >> 5];
            nsub++;
        }
        size_t total = i;
        for (int k = 0; k < nsub; k++) total += lens[k];
        if (total > navail) {                    // inconsistent with the available bytes: discard
            last_app_ = -1;
            last_len_ = 0;
            return;
        }
        size_t off = i;
        for (int k = 0; k < nsub; k++) {
            subfield(apps[k], L + off, lens[k]);
            off += lens[k];
        }
        last_len_ = total;
        if (nsub > 0) last_app_ = apps[nsub - 1];
        return;
    }
    // no contents indicator: one sub-field continuing the last application,
    // as long as the whole field (the previous field's length, DAB+: known)
    if (last_app_ < 0) return;
    size_t len = exact_len ? navail : last_len_;
    if (len == 0 || len > navail) { last_app_ = -1; return; }
    last_app_ = continuation(last_app_);
    last_len_ = len;
    subfield(last_app_, L, len);
}

void PadDecoder::subfield(int app, const uint8_t* d, size_t n) {
    if (app == 2) {                              // dynamic label segment: start of a data group
        dg_len_ = 0;
        dg_active_ = true;
    } else if (app != 3 || !dg_active_) {
        return;                                  // other applications: skipped
    }
    size_t take = std::min(n, sizeof dg_ - dg_len_);
    std::memcpy(dg_ + dg_len_, d, take);
    dg_len_ += take;
    if (dg_len_ < 2) return;
    // the group's length follows from its prefix
    const int c = (dg_[0] >> 4) & 1, f1 = dg_[0] & 0x0F, f3 = dg_[1] & 0x0F;
    size_t need;
    if (c == 0) need = 2 + static_cast<size_t>(f1 + 1) + 2;   // message segment: Length + 1 characters
    else if (f1 == 1) need = 4;                               // clear display: no data field
    else need = 2 + static_cast<size_t>(f3 + 1) + 2;          // DL Plus (field 3 = length - 1)
    if (dg_len_ < need) {
        if (dg_len_ == sizeof dg_) dg_active_ = false;        // cannot complete
        return;
    }
    dg_active_ = false;                                       // the rest is padding
    dls_group(dg_, need);
}

void PadDecoder::dls_group(const uint8_t* d, size_t n) {
    if (crc16_ccitt(d, n - 2) != ((static_cast<uint16_t>(d[n - 2]) << 8) | d[n - 1])) return;
    const int t = d[0] >> 7, first = (d[0] >> 6) & 1, last = (d[0] >> 5) & 1, c = (d[0] >> 4) & 1;
    const int f1 = d[0] & 0x0F, f2 = d[1] >> 4;
    if (c) {
        if (f1 == 1) {                           // clear display: and don't show this message again
            suppress_toggle_ = toggle_;
            seg_have_ = 0;
            seg_total_ = 0;
            if (!label_.empty()) {
                label_.clear();
                sink_.label(label_);
            }
        }
        return;                                  // DL Plus and reserved commands: skipped
    }
    const int seg = first ? 0 : (f2 & 7);
    if (!first && seg == 0) return;              // SegNum 0 is reserved
    if (t != toggle_) {                          // a new message
        toggle_ = t;
        suppress_toggle_ = -1;
        seg_have_ = 0;
        seg_total_ = 0;
    }
    const size_t len = n - 4;                    // 1..16
    std::memcpy(seg_[seg], d + 2, len);
    seg_len_[seg] = static_cast<uint8_t>(len);
    seg_have_ |= 1u << seg;
    if (first) charset_ = f2;
    if (last) seg_total_ = seg + 1;
    if (seg_total_ == 0) return;
    const unsigned mask = (1u << seg_total_) - 1u;
    if ((seg_have_ & mask) != mask) return;
    uint8_t all[8 * 16];
    size_t na = 0;
    for (int i = 0; i < seg_total_; i++) {
        std::memcpy(all + na, seg_[i], seg_len_[i]);
        na += seg_len_[i];
    }
    seg_have_ = 0;
    seg_total_ = 0;
    if (toggle_ == suppress_toggle_) return;
    std::string text = dl_to_utf8(all, na, charset_);
    if (text != label_) {
        label_ = text;
        sink_.label(label_);
    }
}

}   // namespace dab::audio
