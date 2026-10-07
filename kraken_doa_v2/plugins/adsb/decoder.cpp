// ADS-B / Mode S decoder (1090 MHz), for a VFO of the full 2.4 MHz bandwidth.
//
// Physical layer: pulse-position modulation at 1 Mbit/s - every bit is a
// 0.5 us pulse in the first ("1") or second ("0") half of its 1 us slot -
// after an 8 us preamble with pulses at 0, 1, 3.5 and 4.5 us. 56-bit (short)
// or 112-bit (long) messages, protected by a 24-bit CRC whose parity field
// is either plain (DF11 all-call / DF17-18 extended squitter) or XORed with
// the aircraft address (replies DF0/4/5/16/20/21).
//
// Demodulator: at 2.4 MHz one 0.5 us chip is 1.2 samples, so the magnitude
// is integrated over each chip at five sub-sample timings (0.2-sample steps,
// box-filter weights precomputed per chip). A cheap amplitude gate picks
// preamble candidates; the preamble is then scored at the five timings and
// the best two are sliced into bits (first half > second half) and checked
// by CRC. DF17/18 messages with one bad bit (syndrome table) or two bad bits
// among the eight least certain ones are repaired - only for aircraft
// already heard cleanly, which keeps noise from inventing aircraft.
//
// Decoding: identification (callsign, category), airborne and surface
// positions (CPR, global even/odd decoding, then local decoding against the
// aircraft's last position; range and speed checks), velocity (ground speed /
// track, airspeed / heading, vertical rate), emergency / squawk (TC 28),
// altitude and squawk from DF4/5/20/21 replies. Aircraft with a position
// are sent to the web UI's map.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "modes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double FS = 2.4e6;
constexpr double SPC = FS * 0.5e-6;
// unusual-activity events (update_outputs)
constexpr float VRATE_ALERT_FPM = 6000;
constexpr float FAST_LOW_KT = 400, FAST_LOW_FT = 10000;            // samples per 0.5 us chip (1.2)
constexpr int NPHASE = 5;                      // sub-sample timings, 0.2 sample apart
constexpr int NCHIP = 16 + 2 * 112;            // preamble + longest message
constexpr int NEED = static_cast<int>(NCHIP * 1.2) + 8;   // samples a candidate needs
constexpr double AIRCRAFT_TIMEOUT_S = 60;      // forgotten after this long unheard
constexpr double POS_TIMEOUT_S = 60;           // map marker kept this long after the last position
// the panel's aircraft table (kp::Host::table_*), one row per aircraft
const std::vector<std::string> TABLE_COLS = {"ICAO", "Callsign", "Category", "Squawk", "Altitude ft", "GNSS alt ft",
                                             "V/S ft/min", "GS kt", "Track °", "Heading °", "IAS kt", "TAS kt",
                                             "Lat", "Lon", "Distance km", "Bearing °", "Air / ground", "Emergency",
                                             "Signal dBFS", "Messages", "Position age s"};

// box-filter weights of one chip at one timing: samples off, off+1, off+2
struct ChipW {
    int off;
    float w[3];
};

struct Cpr {
    int lat = 0, lon = 0;
    double t = -1e9;
    bool surface = false;
};

struct Aircraft {
    uint32_t addr = 0;
    bool non_icao = false;          // DF18 anonymous / non-ICAO address
    bool confirmed = false;         // two or more messages
    double first = 0, last = 0;
    uint64_t msgs = 0;
    float rssi = -99;               // dBFS, smoothed
    std::string callsign, category;
    float alt_baro = NAN, alt_geom = NAN;   // ft
    double alt_t = -1e9;
    float gs = NAN, track = NAN, ias = NAN, tas = NAN, heading = NAN, vrate = NAN;
    double vel_t = -1e9;
    int squawk = -1, emergency = 0;
    bool ground = false;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    int pos_rejects = 0;
    Cpr cpr[2];                     // even, odd
    double map_t = -1e9, table_t = -1e9;
    bool on_map = false, in_table = false, said_callsign = false, said_pos = false;
    int said_emergency = 0;
    // unusual things, reported once each (update_outputs)
    int sq_pending = -1;            // a new squawk from a reply, waiting for a second one
    double sq_pending_t = -1e9;
    int said_special = -1;          // special squawk (7400 / 7777 / 0000) reported
    bool said_vrate = false, said_fastlow = false, said_category = false;
    double spi_t = -1e9, said_spi_t = -1e9;   // IDENT (SPI): last seen, last reported
    int spi_replies = 0;
};

std::string hex6(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}

const char* category_name(const std::string& c) {
    static const std::pair<const char*, const char*> n[] = {
        {"A1", "light"}, {"A2", "small"}, {"A3", "large"}, {"A4", "high vortex large"}, {"A5", "heavy"},
        {"A6", "high performance"}, {"A7", "rotorcraft"}, {"B1", "glider"}, {"B2", "lighter than air"},
        {"B3", "parachutist"}, {"B4", "ultralight"}, {"B6", "UAV"}, {"B7", "space vehicle"},
        {"C1", "emergency vehicle"}, {"C2", "service vehicle"}, {"C3", "obstacle"}};
    for (const auto& x : n)
        if (c == x.first) return x.second;
    return "";
}

const char* emergency_name(int e) {
    static const char* n[] = {"", "general emergency", "lifeguard / medical", "minimum fuel", "no communications",
                              "unlawful interference", "downed aircraft", "reserved"};
    return (e >= 0 && e <= 7) ? n[e] : "";
}

class Adsb : public kp::Decoder {
public:
    explicit Adsb(kp::Host& h) : Decoder(h) {
        for (int ph = 0; ph < NPHASE; ph++)
            for (int c = 0; c < NCHIP; c++) {
                const double s = ph * (1.0 / NPHASE) + c * SPC, e = s + SPC;
                ChipW& w = w_[ph][c];
                w.off = static_cast<int>(std::floor(s));
                for (int q = 0; q < 3; q++) {
                    const double a = w.off + q, b = a + 1;
                    w.w[q] = static_cast<float>(std::max(0.0, std::min(e, b) - std::max(s, a)));
                }
            }
        mag_.reserve(1 << 16);
        host.table_columns(TABLE_COLS);
    }

    // VFO retuned / decoder restarted: kraken_doa has cleared the facts and
    // the map, so the aircraft go too
    void reset() override {
        mag_.clear();
        pos_ = 0;
        noise_ = 0;
        ac_.clear();
        host.table_columns(TABLE_COLS);
        max_range_ = 0;
        max_range_who_.clear();
        house_t_ = summary_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "range") range_km_ = std::max(0.0, atof(value.c_str()));
        else if (key == "fix") fix_ = value != "0";
    }

    void process(const kp::cf* x, size_t n) override {
        const size_t old = mag_.size();
        mag_.resize(old + n);
        for (size_t i = 0; i < n; i++) mag_[old + i] = std::sqrt(std::norm(x[i]));
        update_noise(mag_.data() + old, n);
        now_ = host.time();

        const float gate = 1.6f * noise_;
        const float* m = mag_.data();
        const size_t end = mag_.size() > static_cast<size_t>(NEED) ? mag_.size() - NEED : 0;
        size_t j = pos_;
        while (j < end) {
            const float* p = m + j;
            // quick gate: pulses 1, 2 and 3 above the noise, the gap between
            // pulses 2 and 3 (samples 5..7 at any timing) quieter than both
            const float p1 = std::max(p[0], p[1]), p2 = std::max(p[2], p[3]);
            if (p1 < gate || p2 < gate || std::max(std::max(p[8], p[9]), p[10]) < gate) { j++; continue; }
            const float q = std::max(std::max(p[5], p[6]), p[7]);
            if (q >= p1 || q >= p2) { j++; continue; }
            int len = try_decode(j);
            j += len > 0 ? static_cast<size_t>((16 + 2 * len) * SPC) : 1;
        }
        // keep the tail a candidate still needs
        const size_t drop = std::min(j, mag_.size() > static_cast<size_t>(NEED) ? mag_.size() - NEED : 0);
        mag_.erase(mag_.begin(), mag_.begin() + static_cast<long>(drop));
        pos_ = j - drop;

        housekeeping();
    }

private:
    std::array<std::array<ChipW, NCHIP>, NPHASE> w_;
    std::vector<float> mag_;
    float conf_[112] = {};
    size_t pos_ = 0;
    float noise_ = 0;
    double now_ = 0;
    double range_km_ = 500;
    bool fix_ = true;

    std::unordered_map<uint32_t, Aircraft> ac_;
    uint64_t total_ = 0, total_rate_mark_ = 0;
    double rate_t_ = 0, rate_ = 0, house_t_ = -1e9, summary_t_ = -1e9;   // summary rows first
    double max_range_ = 0;
    std::string max_range_who_;

    // noise floor: lowest mean magnitude of 512-sample stretches (ADS-B is
    // bursty, so that is the noise between messages), smoothed
    void update_noise(const float* x, size_t n) {
        if (n == 0) return;
        const size_t seg = std::min<size_t>(512, n);
        float lo = 0;
        bool have = false;
        for (size_t s = 0; s + seg <= n; s += seg) {
            float sum = 0;
            for (size_t i = 0; i < seg; i++) sum += x[s + i];
            float mean = sum / static_cast<float>(seg);
            if (!have || mean < lo) { lo = mean; have = true; }
        }
        noise_ = noise_ <= 0 ? lo : noise_ + 0.05f * (lo - noise_);
    }

    float chip(const float* p, int ph, int c) const {
        const ChipW& w = w_[ph][c];
        const float* q = p + w.off;
        return w.w[0] * q[0] + w.w[1] * q[1] + w.w[2] * q[2];
    }

    // Returns the message length in bits if a valid message starts at j, else 0
    int try_decode(size_t j) {
        const float* p = mag_.data() + j;
        struct Cand { int ph; float score, hi; };
        Cand cand[NPHASE];
        int nc = 0;
        for (int ph = 0; ph < NPHASE; ph++) {
            const float h0 = chip(p, ph, 0), h2 = chip(p, ph, 2), h7 = chip(p, ph, 7), h9 = chip(p, ph, 9);
            const float hi = (h0 + h2 + h7 + h9) * 0.25f;
            float lo = 0, lmax = 0;
            for (int c : {1, 3, 4, 5, 6, 8, 10, 11, 12, 13, 14, 15}) {
                float v = chip(p, ph, c);
                lo += v;
                lmax = std::max(lmax, v);
            }
            lo /= 12;
            // ~6 dB between the pulses and the gaps, and no gap as strong as a pulse
            if (hi < 2.0f * lo || lmax >= std::min(std::min(h0, h2), std::min(h7, h9))) continue;
            cand[nc++] = {ph, hi - lo, hi};
        }
        if (!nc) return 0;
        // the two best timings first
        for (int k = 0; k < std::min(nc, 2); k++)
            for (int i = k + 1; i < nc; i++)
                if (cand[i].score > cand[k].score) std::swap(cand[i], cand[k]);
        for (int k = 0; k < std::min(nc, 2); k++) {
            uint8_t msg[14];
            int len = slice(p, cand[k].ph, msg, conf_);
            if (!len) continue;
            float level = cand[k].hi / static_cast<float>(SPC);   // mean pulse amplitude
            if (handle(msg, len, level)) return len;
        }
        return 0;
    }

    // conf[i]: |first half - second half| of bit i (how certain it is)
    int slice(const float* p, int ph, uint8_t* msg, float* conf) const {
        auto bit = [&](int i) {
            const float d = chip(p, ph, 16 + 2 * i) - chip(p, ph, 17 + 2 * i);
            conf[i] = std::fabs(d);
            return d > 0;
        };
        int df = 0;
        for (int i = 0; i < 5; i++) df = (df << 1) | bit(i);
        int len;
        switch (df) {
            case 0: case 4: case 5: case 11: len = 56; break;
            case 16: case 17: case 18: case 20: case 21: len = 112; break;
            default: return 0;
        }
        memset(msg, 0, 14);
        for (int i = 0; i < len; i++)
            if (bit(i)) msg[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
        return len;
    }

    // --- message level ----------------------------------------------------
    Aircraft* find(uint32_t a) {
        auto it = ac_.find(a);
        return it == ac_.end() ? nullptr : &it->second;
    }

    Aircraft& get(uint32_t a, bool non_icao) {
        auto it = ac_.find(a);
        if (it != ac_.end()) return it->second;
        Aircraft& n = ac_[a];
        n.addr = a;
        n.non_icao = non_icao;
        n.first = now_;
        return n;
    }

    // CRC check + dispatch. true if the message was accepted
    bool handle(uint8_t* msg, int len, float level) {
        const int df = msg[0] >> 3;
        uint32_t syn = modes::syndrome(msg, len);
        Aircraft* a = nullptr;
        bool repaired = false;
        if (df == 17 || df == 18) {
            if (syn != 0) {
                if (!fix_ || !repair(msg, syn)) return false;
                repaired = true;
            }
            const uint32_t addr = modes::bits(msg, 9, 32);
            const int cf = msg[0] & 7;
            if (df == 18 && cf != 0 && cf != 1 && cf != 6) return false;   // TIS-B / reserved
            a = repaired ? find(addr) : &get(addr, df == 18 && cf == 1);
            if (!a) return false;
        } else if (df == 11) {
            const uint32_t addr = modes::bits(msg, 9, 32);
            if (syn == 0) a = &get(addr, false);
            else if ((syn & 0xFFFF80) == 0) a = find(addr);   // interrogator id in the parity
            if (!a) return false;
        } else {
            // address / parity: the syndrome is the address
            a = find(syn);
            if (!a) return false;
        }
        accept(*a, level);
        if (df == 17 || df == 18) extended_squitter(*a, msg, df);
        else if (df == 4 || df == 20 || df == 0 || df == 16) reply_altitude(*a, msg, df);
        else if (df == 5 || df == 21) reply_identity(*a, msg);
        if ((df == 4 || df == 5 || df == 20 || df == 21) && ((msg[0] & 7) == 4 || (msg[0] & 7) == 5))
            note_spi(*a, false);   // flight status 4 / 5 = SPI
        else if (df == 11) {
            const int ca = msg[0] & 7;
            if (ca == 4) a->ground = true;
            else if (ca == 5) a->ground = false;
        }
        if (host.verbose()) host.event(describe(*a, msg, len, df, repaired), 0.0);
        if (host.raw_wanted()) {
            // the decoder data log: the message in hex + its level (dBFS)
            std::string h;
            for (int i = 0; i < len / 8; i++) h += fmt("%02X", msg[i]);
            host.raw(h + fmt(" %.1f", 20.0 * std::log10(std::max(level, 1e-6f))));
        }
        if (a->confirmed) update_outputs(*a);
        return true;
    }

    // One bad bit anywhere (syndrome table), or two among the eight least
    // certain bits; never in the DF field
    bool repair(uint8_t* msg, uint32_t syn) const {
        auto flip = [&](int b) { msg[b >> 3] ^= static_cast<uint8_t>(0x80 >> (b & 7)); };
        int b = modes::single_bit_error(syn);
        if (b >= 5) {
            flip(b);
            return true;
        }
        if (b >= 0) return false;
        std::array<int, 107> idx;
        for (int i = 0; i < 107; i++) idx[i] = i + 5;
        std::partial_sort(idx.begin(), idx.begin() + 8, idx.end(), [&](int x, int y) { return conf_[x] < conf_[y]; });
        const int* weak = idx.data();
        for (int x = 0; x < 8; x++)
            for (int y = x + 1; y < 8; y++)
                if ((syn ^ modes::bit_syndrome(weak[x]) ^ modes::bit_syndrome(weak[y])) == 0) {
                    flip(weak[x]);
                    flip(weak[y]);
                    return true;
                }
        return false;
    }

    void accept(Aircraft& a, float level) {
        a.last = now_;
        a.msgs++;
        total_++;
        const float db = 20.0f * std::log10(std::max(level, 1e-6f));
        a.rssi = a.msgs == 1 ? db : a.rssi + 0.2f * (db - a.rssi);
        if (!a.confirmed && a.msgs >= 2) {
            a.confirmed = true;
            host.event("New aircraft " + std::string(a.non_icao ? "~" : "") + hex6(a.addr), 5.0);
        }
        if (a.confirmed) host.valid();
    }

    void extended_squitter(Aircraft& a, const uint8_t* msg, int df) {
        const uint8_t* me = msg + 4;
        const int tc = static_cast<int>(modes::bits(me, 1, 5));
        if (df == 17) {
            const int ca = msg[0] & 7;
            if (ca == 4) a.ground = true;
            else if (ca == 5) a.ground = false;
        }
        if (tc >= 1 && tc <= 4) {
            static const char set[] = {'?', 'D', 'C', 'B', 'A'};
            const int cat = static_cast<int>(modes::bits(me, 6, 8));
            std::string cs = modes::callsign(me);
            if (cs.find('#') == std::string::npos && !cs.empty()) a.callsign = cs;
            if (cat) a.category = std::string(1, set[tc]) + std::to_string(cat);
        } else if ((tc >= 5 && tc <= 8) || (tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            const bool surface = tc <= 8;
            if (surface) {
                a.ground = true;
                const float gs = modes::surface_speed_kt(static_cast<int>(modes::bits(me, 6, 12)));
                if (std::isfinite(gs)) a.gs = gs;
                if (modes::bits(me, 13, 13)) a.track = modes::bits(me, 14, 20) * 360.0f / 128.0f;
                a.vel_t = now_;
            } else {
                a.ground = false;
                const uint32_t ac = modes::bits(me, 9, 20);
                if (tc <= 18) {
                    float alt = modes::ac12_altitude_ft(ac);
                    if (std::isfinite(alt)) { a.alt_baro = alt; a.alt_t = now_; }
                } else if (ac) {
                    a.alt_geom = static_cast<float>(ac * 3.28084);   // GNSS height, metres
                }
            }
            if (!surface && modes::bits(me, 6, 7) == 3) note_spi(a, true);   // surveillance status 3 = SPI
            const int odd = static_cast<int>(modes::bits(me, 22, 22));
            Cpr& c = a.cpr[odd];
            c.lat = static_cast<int>(modes::bits(me, 23, 39));
            c.lon = static_cast<int>(modes::bits(me, 40, 56));
            c.t = now_;
            c.surface = surface;
            position(a, odd != 0);
        } else if (tc == 19) {
            velocity(a, me);
        } else if (tc == 28) {
            if (modes::bits(me, 6, 8) == 1) {
                a.emergency = static_cast<int>(modes::bits(me, 9, 11));
                const uint32_t id = modes::bits(me, 12, 24);
                if (id) set_squawk(a, modes::squawk(id), true);
            }
        }
    }

    void velocity(Aircraft& a, const uint8_t* me) {
        const int st = static_cast<int>(modes::bits(me, 6, 8));
        if (st == 1 || st == 2) {
            const int mult = st == 2 ? 4 : 1;
            const int vew = static_cast<int>(modes::bits(me, 15, 24)), vns = static_cast<int>(modes::bits(me, 26, 35));
            if (vew && vns) {
                float ve = static_cast<float>((vew - 1) * mult), vn = static_cast<float>((vns - 1) * mult);
                if (modes::bits(me, 14, 14)) ve = -ve;
                if (modes::bits(me, 25, 25)) vn = -vn;
                a.gs = std::hypot(ve, vn);
                float trk = std::atan2(ve, vn) * 180.0f / static_cast<float>(M_PI);
                a.track = trk < 0 ? trk + 360.0f : trk;
            }
        } else if (st == 3 || st == 4) {
            const int mult = st == 4 ? 4 : 1;
            if (modes::bits(me, 14, 14)) a.heading = modes::bits(me, 15, 24) * 360.0f / 1024.0f;
            const int as = static_cast<int>(modes::bits(me, 26, 35));
            if (as) {
                float v = static_cast<float>((as - 1) * mult);
                if (modes::bits(me, 25, 25)) a.tas = v;
                else a.ias = v;
            }
        } else {
            return;
        }
        const int vr = static_cast<int>(modes::bits(me, 38, 46));
        if (vr) a.vrate = static_cast<float>((vr - 1) * 64 * (modes::bits(me, 37, 37) ? -1 : 1));
        a.vel_t = now_;
    }

    void reply_altitude(Aircraft& a, const uint8_t* msg, int df) {
        if (df == 4 || df == 20) {
            const int fs = msg[0] & 7;
            if (fs == 1 || fs == 3) a.ground = true;
            else if (fs == 0 || fs == 2) a.ground = false;
        }
        // the extended squitter's altitude is preferred while it is fresh
        if (now_ - a.alt_t < 30) return;
        float alt = modes::ac13_altitude_ft(modes::bits(msg, 20, 32));
        if (!std::isfinite(alt)) return;
        // replies are only address-checked: ignore an implausible jump
        if (std::isfinite(a.alt_baro) && std::fabs(alt - a.alt_baro) > 3000 && now_ - a.alt_t < 120) return;
        a.alt_baro = alt;
    }

    void reply_identity(Aircraft& a, const uint8_t* msg) {
        const uint32_t id = modes::bits(msg, 20, 32);
        if (id) set_squawk(a, modes::squawk(id), false);
    }

    // A squawk from an extended squitter (CRC-checked) is taken at once; one
    // from a reply (only address / parity-checked - a bit error changes the
    // code) only when the next reply within 30 s has the same new code. A
    // change of a known squawk is reported (ATC assigns one per flight)
    void set_squawk(Aircraft& a, int v, bool checked) {
        if (v == a.squawk) { a.sq_pending = -1; return; }
        if (!checked && (v != a.sq_pending || now_ - a.sq_pending_t > 30)) {
            a.sq_pending = v;
            a.sq_pending_t = now_;
            return;
        }
        const int old = a.squawk;
        a.squawk = v;
        a.sq_pending = -1;
        if (old >= 0 && a.confirmed)
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") squawk changed " + fmt("%04.0f", old) + " -> " +
                       fmt("%04.0f", v) + where(a), 5.0);
    }

    // IDENT: the pilot pressed the ident button (SPI) - from an extended
    // squitter's surveillance status, or two replies with flight status 4/5
    // within 10 s (replies are only parity-checked)
    void note_spi(Aircraft& a, bool checked) {
        if (!checked) {
            a.spi_replies = now_ - a.spi_t < 10 ? a.spi_replies + 1 : 1;
            a.spi_t = now_;
            if (a.spi_replies < 2) return;
        }
        a.spi_t = now_;
        if (a.confirmed && now_ - a.said_spi_t > 120) {
            a.said_spi_t = now_;
            host.event(label(a) + " (" + id_of(a) + ") IDENT (SPI)" + where(a), 5.0);
        }
    }

    // --- positions --------------------------------------------------------
    bool station(double* lat, double* lon) const { return host.station(lat, lon); }

    bool plausible(const Aircraft& a, double lat, double lon) const {
        double slat, slon;
        if (range_km_ > 0 && station(&slat, &slon) && modes::distance_km(slat, slon, lat, lon) > range_km_) return false;
        if (std::isfinite(a.lat) && now_ - a.pos_t < 120) {
            // no faster than ~Mach 2.5 (+ slack for CPR / timing)
            const double d = modes::distance_km(a.lat, a.lon, lat, lon);
            if (d > 3.0 + (now_ - a.pos_t) * 0.85) return false;
        }
        return true;
    }

    void position(Aircraft& a, bool odd) {
        const Cpr& c = a.cpr[odd];
        const Cpr& o = a.cpr[!odd];
        double slat = 0, slon = 0;
        const bool have_station = station(&slat, &slon);
        // reference for surface decoding / local decoding
        bool have_ref = false;
        double rlat = 0, rlon = 0;
        if (std::isfinite(a.lat) && now_ - a.pos_t < 60) { rlat = a.lat; rlon = a.lon; have_ref = true; }
        else if (have_station) { rlat = slat; rlon = slon; have_ref = true; }

        double lat, lon;
        bool ok = false, global = false;
        const double pair_window = c.surface ? 25.0 : 10.0;
        if (o.surface == c.surface && now_ - o.t <= pair_window) {
            const Cpr& e = odd ? o : c;
            const Cpr& d = odd ? c : o;
            ok = modes::cpr_global(e.lat, e.lon, d.lat, d.lon, odd, c.surface, have_ref, rlat, rlon, &lat, &lon);
            global = ok;
        }
        if (!ok && std::isfinite(a.lat) && now_ - a.pos_t < 60)
            ok = modes::cpr_local(c.lat, c.lon, odd, c.surface, a.lat, a.lon, &lat, &lon);
        if (!ok) return;
        if (!plausible(a, lat, lon)) {
            // a run of rejected global fixes: the old position was the wrong one
            if (!global || ++a.pos_rejects < 4) return;
        }
        a.pos_rejects = 0;
        a.lat = lat;
        a.lon = lon;
        a.pos_t = now_;
        if (have_station) {
            const double d = modes::distance_km(slat, slon, lat, lon);
            if (d > max_range_ && a.confirmed) {
                max_range_ = d;
                max_range_who_ = label(a);
            }
        }
    }

    // --- output -----------------------------------------------------------
    std::string label(const Aircraft& a) const {
        return a.callsign.empty() ? std::string(a.non_icao ? "~" : "") + hex6(a.addr) : a.callsign;
    }

    static std::string fmt(const char* f, double v) {
        char b[48];
        snprintf(b, sizeof b, f, v);
        return b;
    }

    std::string alt_text(const Aircraft& a) const {
        if (a.ground) return "ground";
        if (std::isfinite(a.alt_baro)) return fmt("%.0f ft", a.alt_baro);
        if (std::isfinite(a.alt_geom)) return fmt("%.0f ft (GNSS)", a.alt_geom);
        return "";
    }

    std::string id_of(const Aircraft& a) const { return std::string(a.non_icao ? "~" : "") + hex6(a.addr); }

    // ", at -36.8812, 174.7712, 3500 ft" (what is known) for the unusual events
    std::string where(const Aircraft& a) const {
        std::string w;
        if (std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S) w = fmt("%.4f", a.lat) + ", " + fmt("%.4f", a.lon);
        const std::string alt = alt_text(a);
        if (!alt.empty()) w += (w.empty() ? "" : ", ") + alt;
        return w.empty() ? "" : " at " + w;
    }

    // the aircraft's row of the panel's table (TABLE_COLS)
    std::vector<std::string> table_cells(const Aircraft& a) const {
        auto num = [&](float v, const char* f) { return std::isfinite(v) ? fmt(f, v) : std::string(); };
        const bool pos = std::isfinite(a.lat);
        double slat, slon, dist = NAN, brg = NAN;
        if (pos && station(&slat, &slon)) {
            dist = modes::distance_km(slat, slon, a.lat, a.lon);
            const double r = M_PI / 180;
            const double y = std::sin((a.lon - slon) * r) * std::cos(a.lat * r);
            const double x = std::cos(slat * r) * std::sin(a.lat * r) -
                             std::sin(slat * r) * std::cos(a.lat * r) * std::cos((a.lon - slon) * r);
            brg = std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
        }
        std::string emerg = a.emergency ? emergency_name(a.emergency)
                          : a.squawk == 7500 ? "hijack" : a.squawk == 7600 ? "radio failure"
                          : a.squawk == 7700 ? "emergency" : "";
        return {id_of(a),
                a.callsign,
                a.category.empty() ? "" : a.category + " " + category_name(a.category),
                a.squawk >= 0 ? fmt("%04.0f", a.squawk) : "",
                a.ground ? "ground" : num(a.alt_baro, "%.0f"),
                num(a.alt_geom, "%.0f"),
                num(a.vrate, "%+.0f"),
                num(a.gs, "%.0f"),
                num(a.track, "%.0f"),
                num(a.heading, "%.0f"),
                num(a.ias, "%.0f"),
                num(a.tas, "%.0f"),
                pos ? fmt("%.5f", a.lat) : "",
                pos ? fmt("%.5f", a.lon) : "",
                std::isfinite(dist) ? fmt("%.1f", dist) : "",
                std::isfinite(brg) ? fmt("%.0f", brg) : "",
                a.ground ? "ground" : "air",
                emerg,
                fmt("%.1f", a.rssi),
                std::to_string(a.msgs),
                pos ? fmt("%.0f", now_ - a.pos_t) : ""};
    }

    std::string info_text(const Aircraft& a) const {
        std::string s = "ICAO: " + std::string(a.non_icao ? "~" : "") + hex6(a.addr);
        auto line = [&](const char* k, const std::string& v) { if (!v.empty()) s += std::string("\n") + k + ": " + v; };
        line("Callsign", a.callsign);
        if (!a.category.empty()) line("Category", a.category + " " + category_name(a.category));
        line("Altitude", alt_text(a));
        if (std::isfinite(a.gs)) line("Ground speed", fmt("%.0f kt", a.gs));
        if (std::isfinite(a.track)) line("Track", fmt("%.0f°", a.track));
        if (std::isfinite(a.heading)) line("Heading", fmt("%.0f° (magnetic)", a.heading));
        if (std::isfinite(a.ias)) line("IAS", fmt("%.0f kt", a.ias));
        if (std::isfinite(a.tas)) line("TAS", fmt("%.0f kt", a.tas));
        if (std::isfinite(a.vrate)) line("Vertical rate", fmt("%+.0f ft/min", a.vrate));
        if (a.squawk >= 0) line("Squawk", fmt("%04.0f", a.squawk));
        if (a.emergency) line("Emergency", emergency_name(a.emergency));
        line("Signal", fmt("%.1f dBFS", a.rssi) + " · " + std::to_string(a.msgs) + " messages");
        return s;
    }

    void update_outputs(Aircraft& a) {
        if (!a.callsign.empty() && !a.said_callsign) {
            a.said_callsign = true;
            host.event(hex6(a.addr) + " is " + a.callsign +
                       (a.category.empty() ? "" : " (" + a.category + " " + category_name(a.category) + ")"), 5.0);
        }
        const bool emerg = a.emergency || a.squawk == 7500 || a.squawk == 7600 || a.squawk == 7700;
        if (emerg) {
            int key = a.emergency ? a.emergency : a.squawk;
            if (key != a.said_emergency) {
                a.said_emergency = key;
                std::string why = a.emergency ? emergency_name(a.emergency)
                                : a.squawk == 7500 ? "hijack" : a.squawk == 7600 ? "radio failure" : "emergency";
                host.event("⚠ EMERGENCY " + label(a) + " (" + hex6(a.addr) + "): " + why +
                           (a.squawk >= 0 ? fmt(", squawk %04.0f", a.squawk) : "") + where(a), 30.0);
            }
        } else if (a.said_emergency) {
            a.said_emergency = 0;
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") emergency over" +
                       (a.squawk >= 0 ? fmt(", squawk %04.0f", a.squawk) : "") + where(a), 30.0);
        }
        // other special squawks: 7400 lost link (unmanned aircraft, ICAO),
        // 7777 (US: military interceptor operations), 0000 (not a valid code)
        const char* special = a.squawk == 7400 ? "lost link (unmanned aircraft)"
                            : a.squawk == 7777 ? "military interception (US)"
                            : a.squawk == 0 ? "code 0000 (not a valid squawk)" : nullptr;
        if (special && a.said_special != a.squawk) {
            a.said_special = a.squawk;
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") squawk " + fmt("%04.0f", a.squawk) + ": " + special +
                       where(a), 30.0);
        } else if (!special) {
            a.said_special = -1;
        }
        // unusual aircraft types
        if (!a.said_category && !a.category.empty()) {
            a.said_category = true;
            static const char* odd[] = {"A6", "B2", "B3", "B4", "B6", "B7"};
            for (const char* c : odd)
                if (a.category == c)
                    host.event("⚠ " + label(a) + " (" + id_of(a) + ") is a " + category_name(a.category) +
                               " (category " + a.category + ")" + where(a), 30.0);
        }
        // extreme vertical rate (airliners climb / descend at ~1000-3000
        // ft/min; an emergency descent is 6000+)
        if (!a.ground && std::isfinite(a.vrate) && now_ - a.vel_t < 30) {
            if (!a.said_vrate && std::fabs(a.vrate) >= VRATE_ALERT_FPM) {
                a.said_vrate = true;
                host.event("⚠ " + label(a) + " (" + id_of(a) + ") " + (a.vrate < 0 ? "descending " : "climbing ") +
                           fmt("%.0f ft/min", std::fabs(a.vrate)) + where(a), 30.0);
            } else if (a.said_vrate && std::fabs(a.vrate) < VRATE_ALERT_FPM * 2 / 3) {
                a.said_vrate = false;
            }
        }
        // fast and low: >= 400 kt below 10000 ft (civil traffic keeps to
        // 250 kt there - military jets don't)
        if (!a.ground && std::isfinite(a.gs) && std::isfinite(a.alt_baro) && now_ - a.vel_t < 30 &&
            now_ - a.alt_t < 30) {
            if (!a.said_fastlow && a.gs >= FAST_LOW_KT && a.alt_baro < FAST_LOW_FT) {
                a.said_fastlow = true;
                host.event("⚠ " + label(a) + " (" + id_of(a) + ") fast and low: " + fmt("%.0f kt", a.gs) + where(a),
                           30.0);
            } else if (a.said_fastlow && (a.gs < FAST_LOW_KT - 50 || a.alt_baro > FAST_LOW_FT + 1000)) {
                a.said_fastlow = false;
            }
        }
        const bool has_pos = std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S;
        if (has_pos && !a.said_pos) {
            a.said_pos = true;
            std::string t = label(a) + " first position " + fmt("%.4f", a.lat) + ", " + fmt("%.4f", a.lon);
            std::string alt = alt_text(a);
            if (!alt.empty()) t += ", " + alt;
            double slat, slon;
            if (station(&slat, &slon)) t += fmt(", %.0f km away", modes::distance_km(slat, slon, a.lat, a.lon));
            host.event(t, 5.0);
        }
        if (has_pos && now_ - a.map_t >= 0.5) {
            a.map_t = now_;
            kp::MapPoint p;
            p.id = std::string(a.non_icao ? "~" : "") + hex6(a.addr);
            p.lat = a.lat;
            p.lon = a.lon;
            p.label = label(a);
            p.kind = (a.category == "C1" || a.category == "C2") ? "vehicle" : "aircraft";
            p.heading = std::isfinite(a.track) ? a.track : a.heading;
            if (a.ground) p.altitude_m = 0;
            else if (std::isfinite(a.alt_baro)) p.altitude_m = a.alt_baro * 0.3048f;
            else if (std::isfinite(a.alt_geom)) p.altitude_m = a.alt_geom * 0.3048f;
            if (std::isfinite(a.gs)) p.speed_kmh = a.gs * 1.852f;
            p.info = info_text(a);
            p.ttl_s = POS_TIMEOUT_S;
            host.map_point(p);
            a.on_map = true;
        }
        if (now_ - a.table_t >= 1.0) {
            a.table_t = now_;
            a.in_table = true;
            host.table_row(id_of(a), table_cells(a));
        }
    }

    void forget(Aircraft& a) {
        const std::string id = std::string(a.non_icao ? "~" : "") + hex6(a.addr);
        if (a.on_map) host.map_remove(id);
        if (a.in_table) host.table_remove(id);
    }

    void housekeeping() {
        if (now_ - house_t_ < 1.0) return;
        const double dt = now_ - rate_t_;
        if (dt >= 5.0) {
            rate_ = (total_ - total_rate_mark_) / dt;
            total_rate_mark_ = total_;
            rate_t_ = now_;
        }
        house_t_ = now_;
        int n = 0, npos = 0;
        for (auto it = ac_.begin(); it != ac_.end();) {
            Aircraft& a = it->second;
            const double idle = now_ - a.last;
            if (idle > AIRCRAFT_TIMEOUT_S || (!a.confirmed && idle > 10)) {
                forget(a);
                it = ac_.erase(it);
                continue;
            }
            if (a.on_map && now_ - a.pos_t >= POS_TIMEOUT_S) {
                host.map_remove(std::string(a.non_icao ? "~" : "") + hex6(a.addr));
                a.on_map = false;
            }
            if (a.confirmed) {
                n++;
                if (std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S) npos++;
            }
            ++it;
        }
        if (now_ - summary_t_ < 2.0) return;
        summary_t_ = now_;
        host.fact("Aircraft", std::to_string(n) + " (" + std::to_string(npos) + " with position)");
        host.fact("Messages", std::to_string(total_) + fmt(" (%.0f/s)", rate_));
        double slat, slon;
        if (station(&slat, &slon)) {
            host.fact("Max range", max_range_ > 0 ? fmt("%.0f km", max_range_) + " (" + max_range_who_ + ")" : "-");
        } else {
            host.fact("Max range", "set the station location (Station Information) for distances");
        }
        host.fact("Noise floor", fmt("%.1f dBFS", 20.0 * std::log10(std::max(noise_, 1e-6f))));
    }

    std::string describe(const Aircraft& a, const uint8_t* msg, int len, int df, bool repaired) const {
        std::string h;
        for (int i = 0; i < len / 8; i++) h += fmt("%02X", msg[i]);
        std::string s = "DF" + std::to_string(df) + " " + hex6(a.addr) + " " + h;
        if (df == 17 || df == 18) s += " TC" + std::to_string(modes::bits(msg + 4, 1, 5));
        if (repaired) s += " (1 bit repaired)";
        s += fmt(" %.1f dBFS", a.rssi);
        return s;
    }
};

}  // namespace

KRAKEN_PLUGIN(Adsb, {.id = "adsb",
                     .name = "ADS-B (1090 MHz)",
                     .description = "ADS-B / Mode S aircraft on 1090 MHz: callsign, position, altitude, speed, squawk - "
                                    "picking it tunes the VFO and its tuner to 1090 MHz at 2.4 MHz; aircraft appear on the 🗺 Map",
                     .version = "1.0",
                     .sample_rate = FS,
                     .min_vfo_rate = 2.0e6,
                     .author = "KrakenSDR",
                     .options = {{"range", "Max range", "500", "300=300 km|500=500 km|1000=1000 km|0=No limit",
                                  "Positions further than this from the station (Station Information) are rejected "
                                  "as decoding errors"},
                                 {"fix", "Repair 1-bit errors", "1", "1=On|0=Off",
                                  "Repair extended squitters with one bad bit (only for aircraft already heard cleanly)"}},
                     .map = true,
                     .manual_only = true,
                     .fixed_freq_hz = 1090e6})
