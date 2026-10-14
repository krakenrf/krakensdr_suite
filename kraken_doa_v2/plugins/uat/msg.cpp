// UAT payload decoding: bit positions as in RTCA DO-282B (MOPS for UAT
// ADS-B), as publicly described; dump978 (GPL) was used as documentation
// and as a test reference only (tools / README), none of its code here.

#include "msg.hpp"

#include <cmath>
#include <cstdio>

namespace uat {

const char* aq_name(int aq) {
    static const char* n[] = {"ADS-B (ICAO)", "ADS-B (self-assigned)", "TIS-B (ICAO)", "TIS-B (track file)",
                              "surface vehicle", "fixed beacon", "ADS-R", "reserved"};
    return (aq >= 0 && aq < 8) ? n[aq] : "";
}

namespace {

// 23-bit latitude / 24-bit longitude in units of 360 / 2^24 degrees
void latlon(const uint8_t* f, double* lat, double* lon) {
    const uint32_t rl = (uint32_t(f[0]) << 15) | (uint32_t(f[1]) << 7) | (f[2] >> 1);
    const uint32_t ro = (uint32_t(f[2] & 1) << 23) | (uint32_t(f[3]) << 15) | (uint32_t(f[4]) << 7) | (f[5] >> 1);
    *lat = rl * (360.0 / 16777216.0);
    if (*lat > 90) *lat -= 180;
    *lon = ro * (360.0 / 16777216.0);
    if (*lon > 180) *lon -= 360;
}

// signed velocity: bit 10 = sign (south / west), bits 0-9 = |v| + 1 (0 = none)
bool velocity(int raw, int* v) {
    if ((raw & 0x3FF) == 0) return false;
    *v = (raw & 0x3FF) - 1;
    if (raw & 0x400) *v = -*v;
    return true;
}

const char BASE40[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ  ..";

// ground vehicles / aircraft on the surface: length / width code
const float LEN_M[16] = {0, 15, 25, 25, 35, 35, 45, 45, 55, 55, 65, 65, 75, 75, 85, 85};
const float WID_M[16] = {0, 23, 28.5f, 34, 33, 38, 39.5f, 45, 45, 52, 59.5f, 67, 72.5f, 80, 80, 90};

}  // namespace

bool decode_adsb(const uint8_t* f, int len, AdsbMsg& m) {
    m = AdsbMsg();
    m.type = f[0] >> 3;
    m.aq = f[0] & 7;
    m.addr = (uint32_t(f[1]) << 16) | (uint32_t(f[2]) << 8) | f[3];
    if ((len == 18) != (m.type == 0)) return false;   // a basic frame carries type 0 only
    if (m.type > 10) return false;                     // reserved payload types
    // state vector (bytes 4..16)
    m.nic = f[11] & 15;
    double lat, lon;
    latlon(f + 4, &lat, &lon);
    const bool any = ((f[4] | f[5] | f[6] | f[7] | f[8]) != 0) || (f[9] >> 1) != 0;
    if (m.nic != 0 || any) {
        m.pos_valid = true;
        m.lat = lat;
        m.lon = lon;
    }
    const int ra = (f[10] << 4) | (f[11] >> 4);
    if (ra) {
        m.alt_kind = (f[9] & 1) ? 2 : 1;
        m.alt_ft = (ra - 1) * 25 - 1000;
    }
    m.ag = f[12] >> 6;
    const int r1 = ((f[12] & 0x1F) << 6) | (f[13] >> 2);
    const int r2 = ((f[13] & 3) << 9) | (f[14] << 1) | (f[15] >> 7);
    if (m.ag == 0 || m.ag == 1) {
        m.ns_ok = velocity(r1, &m.ns_kt);
        m.ew_ok = velocity(r2, &m.ew_kt);
        if (m.ag == 1) {   // supersonic: 4 kt steps
            m.ns_kt *= 4;
            m.ew_kt *= 4;
        }
        if (m.ns_ok && m.ew_ok) {
            m.speed_ok = true;
            m.speed_kt = static_cast<int>(std::lround(std::hypot(m.ns_kt, m.ew_kt)));
            if (m.ns_kt || m.ew_kt) {
                m.track_kind = 1;
                m.track = std::fmod(std::atan2(m.ew_kt, m.ns_kt) * 180 / M_PI + 360, 360);
            }
        }
        const int rv = ((f[15] & 0x7F) << 4) | (f[16] >> 4);
        if (rv & 0x1FF) {
            m.vr_kind = (rv & 0x400) ? 1 : 2;
            m.vrate_fpm = ((rv & 0x1FF) - 1) * 64;
            if (rv & 0x200) m.vrate_fpm = -m.vrate_fpm;
        }
    } else if (m.ag == 2) {
        if (r1) {
            m.speed_ok = true;
            m.speed_kt = (r1 & 0x3FF) - 1;
        }
        m.track_kind = (r2 >> 9) & 3;
        if (m.track_kind) m.track = (r2 & 0x1FF) * 360.0 / 512;
        const int code = (f[15] >> 3) & 15;
        if (code) {
            m.dims = true;
            m.length_m = LEN_M[code];
            m.width_m = WID_M[code];
        }
    }
    if (aq_tisb(m.aq)) m.site = f[16] & 0x0F;
    else m.utc = (f[16] & 0x08) != 0;
    if (len < 34) return true;
    // mode status (bytes 17..28): payload types 1 and 3
    if (m.type == 1 || m.type == 3) {
        m.has_ms = true;
        char cs[9];
        const unsigned v1 = (f[17] << 8) | f[18], v2 = (f[19] << 8) | f[20], v3 = (f[21] << 8) | f[22];
        m.cat = (v1 / 1600) % 40;
        cs[0] = BASE40[(v1 / 40) % 40];
        cs[1] = BASE40[v1 % 40];
        cs[2] = BASE40[(v2 / 1600) % 40];
        cs[3] = BASE40[(v2 / 40) % 40];
        cs[4] = BASE40[v2 % 40];
        cs[5] = BASE40[(v3 / 1600) % 40];
        cs[6] = BASE40[(v3 / 40) % 40];
        cs[7] = BASE40[v3 % 40];
        int e = 8;
        while (e > 0 && cs[e - 1] == ' ') e--;
        int b = 0;
        while (b < e && cs[b] == ' ') b++;
        m.callsign.assign(cs + b, cs + e);
        m.emergency = f[23] >> 5;
        m.version = (f[23] >> 2) & 7;
        m.sil = f[23] & 3;
        m.mso = f[24] >> 2;
        m.nacp = f[25] >> 4;
        m.nacv = (f[25] >> 1) & 7;
        m.nic_baro = f[25] & 1;
        m.cdti = f[26] & 0x80;
        m.acas = f[26] & 0x40;
        m.acas_ra = f[26] & 0x20;
        m.ident = f[26] & 0x10;
        m.atc = f[26] & 0x08;
        m.hdg_magnetic = f[26] & 0x04;
        m.cs_callsign = (f[26] & 0x02) != 0;
    }
    // auxiliary state vector (bytes 29..33): payload types 1, 2, 5, 6 - the
    // other kind of altitude
    if (m.type == 1 || m.type == 2 || m.type == 5 || m.type == 6) {
        m.has_aux = true;
        const int r = (f[29] << 4) | (f[30] >> 4);
        if (r) {
            m.sec_alt_kind = (f[9] & 1) ? 1 : 2;
            m.sec_alt_ft = (r - 1) * 25 - 1000;
        }
    }
    return true;
}

std::string category_code(int cat) {
    if (cat <= 0 || cat >= 40) return "";
    char b[4];
    snprintf(b, sizeof b, "%c%d", "ABCDE"[cat / 8], cat % 8);
    return b;
}

const char* category_name(int cat) {
    switch (cat) {
    case 1: return "light";
    case 2: return "small";
    case 3: return "large";
    case 4: return "high vortex large";
    case 5: return "heavy";
    case 6: return "highly manoeuvrable";
    case 7: return "rotorcraft";
    case 9: return "glider";
    case 10: return "lighter than air";
    case 11: return "parachutist";
    case 12: return "ultralight / hang glider / paraglider";
    case 14: return "UAV";
    case 15: return "space vehicle";
    case 17: return "surface emergency vehicle";
    case 18: return "surface service vehicle";
    case 19: return "point obstacle";
    case 20: return "cluster obstacle";
    case 21: return "line obstacle";
    default: return "";
    }
}

const char* emergency_name(int e) {
    static const char* n[] = {"", "general emergency", "lifeguard / medical", "minimum fuel", "no communications",
                              "unlawful interference", "downed aircraft", "reserved"};
    return (e >= 0 && e <= 7) ? n[e] : "";
}

bool decode_uplink(const uint8_t* f, UplinkMsg& m) {
    m = UplinkMsg();
    latlon(f, &m.lat, &m.lon);
    m.pos_valid = f[5] & 1;
    m.utc = f[6] & 0x80;
    m.app_valid = f[6] & 0x20;
    m.slot = f[6] & 0x1F;
    m.site = f[7] >> 4;
    if (!m.app_valid) return true;
    // information frames: 9-bit length, 4-bit type, then the data
    size_t p = 8;
    while (p + 2 <= 432 && m.frames.size() < 64) {
        const int len = (f[p] << 1) | (f[p + 1] >> 7);
        const int type = f[p + 1] & 0x0F;
        if (len == 0 && type == 0) break;
        if (p + 2 + len > 432) return !m.frames.empty();   // overrun: keep the complete ones
        InfoFrame fr;
        fr.type = type;
        fr.data.assign(f + p + 2, f + p + 2 + len);
        if (type == 0 && len >= 4) {
            const uint8_t* d = fr.data.data();
            FisbApdu& a = fr.fisb;
            a.a = d[0] & 0x80;
            a.g = d[0] & 0x40;
            a.p = d[0] & 0x20;
            a.product = ((d[0] & 0x1F) << 6) | (d[1] >> 2);
            a.s = d[1] & 0x02;
            a.t_opt = ((d[1] & 1) << 1) | (d[2] >> 7);
            size_t h = 0;
            switch (a.t_opt) {
            case 0:   // hours, minutes
                a.hour = (d[2] >> 2) & 0x1F;
                a.minute = ((d[2] & 3) << 4) | (d[3] >> 4);
                h = 4;
                break;
            case 1:   // + seconds
                if (len < 5) break;
                a.hour = (d[2] >> 2) & 0x1F;
                a.minute = ((d[2] & 3) << 4) | (d[3] >> 4);
                a.second = ((d[3] & 0x0F) << 2) | (d[4] >> 6);
                h = 5;
                break;
            case 2:   // month, day, hours, minutes
                if (len < 5) break;
                a.month = (d[2] >> 3) & 0x0F;
                a.day = ((d[2] & 7) << 2) | (d[3] >> 6);
                a.hour = (d[3] >> 1) & 0x1F;
                a.minute = ((d[3] & 1) << 5) | (d[4] >> 3);
                h = 5;
                break;
            default:  // + seconds
                if (len < 6) break;
                a.month = (d[2] >> 3) & 0x0F;
                a.day = ((d[2] & 7) << 2) | (d[3] >> 6);
                a.hour = (d[3] >> 1) & 0x1F;
                a.minute = ((d[3] & 1) << 5) | (d[4] >> 3);
                a.second = ((d[4] & 3) << 3) | (d[5] >> 5);
                h = 6;
                break;
            }
            if (h) {
                fr.is_fisb = true;
                a.data.assign(fr.data.begin() + static_cast<long>(h), fr.data.end());
            }
        }
        m.frames.push_back(std::move(fr));
        p += 2 + len;
    }
    return true;
}

const char* product_name(int id) {
    switch (id) {
    case 0: case 20: return "METAR / SPECI";
    case 1: case 21: return "TAF";
    case 2: case 22: return "SIGMET";
    case 3: case 23: return "Convective SIGMET";
    case 4: case 24: return "AIRMET";
    case 5: case 25: return "PIREP";
    case 6: case 26: return "Severe weather warning (AWW)";
    case 7: case 27: return "Winds and temperatures aloft";
    case 8: return "NOTAM / TFR / service status";
    case 9: return "D-ATIS";
    case 10: return "TWIP";
    case 11: return "AIRMET (graphic)";
    case 12: return "SIGMET / convective SIGMET (graphic)";
    case 13: return "Special use airspace status";
    case 14: return "G-AIRMET";
    case 15: return "Center weather advisory";
    case 16: return "NOTAM-TRA";
    case 17: return "NOTAM-TMOA";
    case 51: case 52: case 53: case 54: return "National NEXRAD";
    case 55: case 56: case 57: case 58: return "Regional NEXRAD";
    case 59: case 60: case 61: case 62: return "Individual NEXRAD";
    case 63: return "Regional NEXRAD (block)";
    case 64: return "CONUS NEXRAD (block)";
    case 70: return "Icing forecast (low)";
    case 71: return "Icing forecast (high)";
    case 81: case 82: return "Radar echo tops";
    case 83: return "Storm tops and velocity";
    case 84: return "Cloud tops";
    case 90: return "Turbulence forecast (low)";
    case 91: return "Turbulence forecast (high)";
    case 101: case 102: case 103: return "Lightning";
    case 151: return "Point phenomena";
    case 201: return "Surface conditions / winter precipitation";
    case 202: return "Surface weather systems";
    case 254: return "AIRMET / SIGMET bitmap";
    case 351: return "System time";
    case 352: return "Operational status";
    case 353: return "Ground station status";
    case 401: return "Generic raster product";
    case 402: case 411: case 405: case 413: return "Text (METAR, TAF, PIREP, winds)";
    case 403: return "Generic vector product";
    case 404: case 412: return "Generic symbolic product";
    default: return "product";
    }
}

bool product_is_text(int id) {
    return (id >= 20 && id <= 27) || id == 411 || id == 413;
}

std::string dlac(const uint8_t* d, size_t n) {
    // index -> character (0 = end of text, 27 = change cipher, 28 = tab: the
    // next character's index is a count of spaces, 29 = record separator)
    static const char T[] = "\x03" "ABCDEFGHIJKLMNOPQRSTUVWXYZ\x1a\t\x1e\n| !\"#$%&'()*+,-./0123456789:;<=>?";
    std::string s;
    bool tab = false;
    const size_t nch = n / 3 * 4 + (n % 3 == 2 ? 2 : n % 3 == 1 ? 1 : 0);
    for (size_t i = 0; i < nch; i++) {
        const size_t g = i / 4 * 3;
        int c;
        switch (i % 4) {
        case 0: c = d[g] >> 2; break;
        case 1: c = ((d[g] & 3) << 4) | (g + 1 < n ? d[g + 1] >> 4 : 0); break;
        case 2: c = ((d[g + 1] & 0x0F) << 2) | (g + 2 < n ? d[g + 2] >> 6 : 0); break;
        default: c = d[g + 2] & 0x3F; break;
        }
        if (tab) {
            s.append(static_cast<size_t>(c), ' ');
            tab = false;
            continue;
        }
        const char ch = T[c];
        if (ch == '\t') { tab = true; continue; }
        if (ch == '\x1a') continue;
        s += ch;
    }
    return s;
}

std::vector<std::string> text_records(const uint8_t* d, size_t n) {
    const std::string t = dlac(d, n);
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        while (!cur.empty() && cur.back() == ' ') cur.pop_back();
        if (cur.size() >= 3) out.push_back(cur);
        cur.clear();
    };
    for (char ch : t) {
        if (ch == '\x1e' || ch == '\x03') { flush(); continue; }
        if (ch == '\n' || ch == ' ') {
            if (!cur.empty() && cur.back() != ' ') cur += ' ';
            continue;
        }
        cur += ch;
    }
    flush();
    return out;
}

std::string dump(const AdsbMsg& m) {
    char b[512];
    int n = snprintf(b, sizeof b, "ADSB type=%d aq=%d addr=%06X", m.type, m.aq, m.addr & 0xFFFFFF);
    auto add = [&](const char* f, auto... v) { n += snprintf(b + n, sizeof b - static_cast<size_t>(n), f, v...); };
    if (m.pos_valid) add(" lat=%.4f lon=%.4f", m.lat, m.lon);
    if (m.alt_kind) add(" alt=%d%s", m.alt_ft, m.alt_kind == 1 ? "b" : "g");
    add(" nic=%d ag=%d", m.nic, m.ag);
    if (m.ns_ok) add(" ns=%d", m.ns_kt);
    if (m.ew_ok) add(" ew=%d", m.ew_kt);
    if (m.speed_ok) add(" spd=%d", m.speed_kt);
    if (m.track_kind) add(" trk=%d/%d", static_cast<int>(m.track), m.track_kind);
    if (m.vr_kind) add(" vr=%d%s", m.vrate_fpm, m.vr_kind == 1 ? "b" : "g");
    if (m.dims) add(" dims=%.0fx%.1f", m.length_m, m.width_m);
    add(" utc=%d site=%d", m.utc ? 1 : 0, m.site);
    if (m.has_ms)
        add(" cs='%s'/%d cat=%d em=%d ver=%d sil=%d nacp=%d nacv=%d nicb=%d cap=%d%d%d%d%d mag=%d", m.callsign.c_str(),
            m.cs_callsign ? 1 : 0, m.cat, m.emergency, m.version, m.sil, m.nacp, m.nacv, m.nic_baro, m.cdti, m.acas,
            m.acas_ra, m.ident, m.atc, m.hdg_magnetic);
    if (m.has_aux && m.sec_alt_kind) add(" alt2=%d%s", m.sec_alt_ft, m.sec_alt_kind == 1 ? "b" : "g");
    return b;
}

std::string dump(const UplinkMsg& u) {
    char b[8192];
    int n = snprintf(b, sizeof b, "UPLINK lat=%.4f lon=%.4f pv=%d utc=%d slot=%d site=%d", u.lat, u.lon, u.pos_valid ? 1 : 0,
                     u.utc ? 1 : 0, u.slot, u.site);
    for (const auto& f : u.frames) {
        if (n > 7500) break;
        if (f.is_fisb) {
            n += snprintf(b + n, sizeof b - static_cast<size_t>(n), " [%d %d/%d %d:%d len=%zu]", f.fisb.product,
                          f.fisb.month, f.fisb.day, f.fisb.hour, f.fisb.minute, f.data.size());
            if (product_is_text(f.fisb.product))
                for (const auto& r : text_records(f.fisb.data.data(), f.fisb.data.size()))
                    n += snprintf(b + n, sizeof b - static_cast<size_t>(n), " {%.300s}", r.c_str());
        }
        else
            n += snprintf(b + n, sizeof b - static_cast<size_t>(n), " [type %d len=%zu]", f.type, f.data.size());
    }
    return b;
}

}  // namespace uat
