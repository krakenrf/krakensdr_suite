// D-STAR (JARL) digital voice: GMSK 4800 bit/s. Decodes the RF header
// (scrambled, interleaved, rate-1/2 convolutional code, CRC) and the slow
// data channel of the voice frames - text message, GPS / DPRS, header copy.
// Voice (AMBE 2400) is not synthesized.

#include "dstar.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dig {

namespace {

// "ddmm.mmmm" (deg_digits = 2) / "dddmm.mmmm" (3) + N/S/E/W -> degrees
bool nmea_deg(const std::string& v, const std::string& h, int deg_digits, double* out) {
    if (v.size() < static_cast<size_t>(deg_digits) + 2 || h.size() != 1) return false;
    for (size_t i = 0; i < v.size(); i++)
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] == '.' && i > static_cast<size_t>(deg_digits) + 1))) return false;
    double d = atof(v.substr(0, deg_digits).c_str()), m = atof(v.substr(deg_digits).c_str());
    if (m >= 60) return false;
    d += m / 60.0;
    if (h[0] == 'S' || h[0] == 'W') d = -d;
    else if (h[0] != 'N' && h[0] != 'E') return false;
    *out = d;
    return true;
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> f;
    size_t p = 0, q;
    while ((q = s.find(c, p)) != std::string::npos) { f.push_back(s.substr(p, q - p)); p = q + 1; }
    f.push_back(s.substr(p));
    return f;
}

// A GPS slow-data line -> position. NMEA ($xxRMC / $xxGGA) only with a good
// checksum, DPRS ($$CRCxxxx,CALL>...:!DDMM.mmN/DDDMM.mmW...) only with a good
// CRC (the D-STAR header's CRC over the APRS text) - slow data has no FEC.
// *call = the DPRS packet's source callsign.
bool gps_position(const std::string& line, double* lat, double* lon, std::string* call) {
    if (line.rfind("$$CRC", 0) == 0) {
        if (line.size() < 12 || line[9] != ',') return false;
        const std::string body = line.substr(10);
        const unsigned want = static_cast<unsigned>(strtoul(line.substr(5, 4).c_str(), nullptr, 16));
        auto crc = [](const std::string& b) {
            return crc16_x25(reinterpret_cast<const uint8_t*>(b.data()), b.size());
        };
        if (crc(body) != want && crc(body + "\r") != want) return false;
        size_t gt = body.find('>'), colon = body.find(':');
        if (gt == std::string::npos || colon == std::string::npos || colon + 20 > body.size()) return false;
        *call = body.substr(0, gt);
        size_t p = colon + 1;
        if (body[p] == '/' || body[p] == '@') p += 7;   // timestamp
        if (body[p] != '!' && body[p] != '=' && body[p] != '/' && body[p] != '@') return false;
        p++;
        if (p + 19 > body.size()) return false;
        return nmea_deg(body.substr(p, 7), body.substr(p + 7, 1), 2, lat) &&
               nmea_deg(body.substr(p + 9, 8), body.substr(p + 17, 1), 3, lon);
    }
    if (line.size() < 10 || line[0] != '$') return false;
    size_t star = line.rfind('*');
    if (star == std::string::npos || star + 3 > line.size()) return false;
    uint8_t x = 0;
    for (size_t i = 1; i < star; i++) x ^= static_cast<uint8_t>(line[i]);
    if (x != static_cast<uint8_t>(strtoul(line.substr(star + 1, 2).c_str(), nullptr, 16))) return false;
    auto f = split(line.substr(0, star), ',');
    const std::string type = f[0].size() >= 6 ? f[0].substr(3) : "";
    if (type == "RMC" && f.size() >= 7 && f[2] == "A")
        return nmea_deg(f[3], f[4], 2, lat) && nmea_deg(f[5], f[6], 3, lon);
    if (type == "GGA" && f.size() >= 7 && !f[6].empty() && f[6] != "0")
        return nmea_deg(f[2], f[3], 2, lat) && nmea_deg(f[4], f[5], 3, lon);
    return false;
}
constexpr int SPS = 10;
constexpr float THRESHOLD = 0.80f;
constexpr int SUPERFRAME_BITS = 21 * 96;   // data sync repeats every 420 ms

// sync words in air bit order
constexpr uint32_t FRAME_SYNC = 0x557650;   // preamble tail + 111011001010000
constexpr uint32_t DATA_SYNC = 0xAAB468;    // 0x55 0x2D 0x16, LSB first
constexpr uint32_t END_SYNC = 0xAAAA135E;   // ...1010 + 000100110101111

std::vector<float> template_of(uint32_t word, int nbits) {
    std::vector<float> t(nbits);
    float m = 0;
    for (int i = 0; i < nbits; i++) { t[i] = ((word >> (nbits - 1 - i)) & 1) ? 1.0f : -1.0f; m += t[i]; }
    m /= nbits;
    float e = 0;
    for (float& v : t) { v -= m; e += v * v; }
    e = std::sqrt(e);
    for (float& v : t) v /= e;
    return t;
}

std::string callsign(const uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += (p[i] >= 32 && p[i] < 127) ? static_cast<char>(p[i]) : '?';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// "F1ZIL  B" (8-character field, module letter last) -> "F1ZIL B"
std::string squeezed(const std::string& in) {
    std::string s;
    for (char c : in)
        if (c != ' ' || (!s.empty() && s.back() != ' ')) s += c;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string printable(const std::string& in) {
    std::string s;
    for (char c : in) s += (c >= 32 && c < 127) ? c : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
}  // namespace

DstarReceiver::DstarReceiver(RxContext& c) : ctx_(c) {
    t_hdr_ = template_of(FRAME_SYNC, 24);
    t_data_ = template_of(DATA_SYNC, 24);
    t_end_ = template_of(END_SYNC, 32);
}

void DstarReceiver::reset() {
    b_ = SampleBuf();
    std::fill(std::begin(hist_), std::end(hist_), 0.0f);
    sum_ = 0;
    pk_ = Peak();
    jobs_.clear();
    frame_sync_end_ = -1;
    for (auto& m : msg_) m.clear();
    header_desc_.clear();
    my_.clear();
    msg_text_.clear();
    end_sync_end_ = -1;
    if (ambe_) ambe_->reset();
    // the host resets its own talker state together with ours
    call_start_ = last_sync_ = tx_hdr_sync_ = hdr_cand_ = hdr_ok_sync_ = -1;
    talker_id_.clear();
}

// --- talkers (RxContext::talker) ----------------------------------------------
// A transmission = data syncs in the 420 ms superframe cadence, from the RF
// header right before the first one (or that first sync on a late entry) to
// the end pattern; its talker = the MY callsign of the header (RF, or the
// copy in the slow data). Sync events come in sample order from correlate();
// the header's CRC is checked later (its job needs the 660 header bits), so
// the talker may be named after the transmission began.
void DstarReceiver::tx_sync(int64_t se) {
    const int64_t sf = static_cast<int64_t>(SUPERFRAME_BITS) * SPS;
    if (call_start_ >= 0) {
        const int64_t dt = se - last_sync_;
        if (dt > 0 && dt <= 2 * sf + 3 * SPS) {
            const int64_t rem = dt % sf;
            if (rem < 3 * SPS || sf - rem < 3 * SPS) {   // in the cadence: everything up to it is confirmed
                voice_end_ = se;
                last_sync_ = se;
                tx_report();
            }
            return;   // else a false sync inside the transmission
        }
        tx_end(voice_end_);
    }
    // a new transmission. The header + voice frame 0 come before the first
    // data sync (756 bits from the header's frame sync to it): that exact
    // spacing confirms them.
    last_sync_ = se;
    if (hdr_cand_ >= 0 && std::llabs(se - hdr_cand_ - 756 * SPS) < 3 * SPS) {
        tx_hdr_sync_ = hdr_cand_;
        call_start_ = hdr_cand_ - 24 * SPS;
        voice_end_ = se;
        if (hdr_ok_sync_ == tx_hdr_sync_) tx_talker(hdr_id_, hdr_label_);
    } else {
        tx_hdr_sync_ = -1;
        call_start_ = voice_end_ = se;   // late entry: from this sync on
    }
}

void DstarReceiver::tx_end_pattern(int64_t se) {
    if (call_start_ < 0) return;
    // the voice frames up to the end pattern (32 bits ending at se) are the
    // transmission's last
    const int64_t end = se - 32 * SPS;
    if (end > voice_end_ && end - last_sync_ <= static_cast<int64_t>(SUPERFRAME_BITS) * SPS + 3 * SPS) {
        voice_end_ = end;
        tx_report();
    }
    tx_end(voice_end_);
}

void DstarReceiver::tx_talker(const std::string& id, const std::string& label) {
    if (id.empty() || call_start_ < 0) return;
    talker_id_ = id;
    talker_label_ = label;
    tx_report();
}

void DstarReceiver::tx_report() {
    if (!talker_id_.empty() && ctx_.talker) ctx_.talker(talker_id_, talker_label_, call_start_, voice_end_, 0);
}

void DstarReceiver::tx_end(int64_t at) {
    if (!talker_id_.empty() && ctx_.talker_end) ctx_.talker_end(at, 0);
    talker_id_.clear();
    call_start_ = -1;
}

void DstarReceiver::correlate() {
    const int64_t n = b_.end() - 1;
    const std::vector<float>* tp[3] = {&t_hdr_, &t_data_, &t_end_};
    float best = 0;
    int which = 0;
    for (int w = 0; w < 3; w++) {
        const auto& t = *tp[w];
        const int L = static_cast<int>(t.size());
        const int64_t first = n - static_cast<int64_t>(L - 1) * SPS;
        if (first < b_.begin()) continue;
        float c = 0, m = 0, e = 0;
        float x[32];
        for (int k = 0; k < L; k++) { x[k] = b_.at(first + k * SPS); m += x[k]; }
        m /= L;
        for (int k = 0; k < L; k++) { float v = x[k] - m; e += v * v; c += t[k] * v; }
        if (e < 1e-3f) continue;
        c /= std::sqrt(e);
        if (std::fabs(c) > std::fabs(best)) { best = c; which = w; }
    }
    if (pk_.active) {
        pk_.age++;
        if (std::fabs(best) > std::fabs(pk_.best)) { pk_.best = best; pk_.idx = n; pk_.which = which; }
        if (pk_.age < SPS) return;
        pk_.active = false;
        // level / centre from the sync bits
        const auto& t = *tp[pk_.which];
        const uint32_t words[3] = {FRAME_SYNC, DATA_SYNC, END_SYNC};
        const int L = static_cast<int>(t.size());
        float s1 = 0, s0 = 0;
        int n1 = 0, n0 = 0;
        for (int k = 0; k < L; k++) {
            float v = b_.at(pk_.idx - static_cast<int64_t>(L - 1 - k) * SPS);
            if ((words[pk_.which] >> (L - 1 - k)) & 1) { s1 += v; n1++; } else { s0 += v; n0++; }
        }
        SymSrc src;
        src.buf = &b_;
        src.sync_end = pk_.idx;
        src.sps = SPS;
        float hi = s1 / n1, lo = s0 / n0;
        src.center = 0.5f * (hi + lo);
        src.scale = 0.5f * (hi - lo);   // negative = inverted signal
        if (std::fabs(src.scale) < 100.0f) return;
        if (pk_.which == 0) {
            hdr_cand_ = pk_.idx;
            jobs_.push_back({0, src, 660 + 1});
        } else if (pk_.which == 1) {
            tx_sync(pk_.idx);
            jobs_.push_back({1, src, 20 * 96});
        } else {
            tx_end_pattern(pk_.idx);
            end_sync_end_ = pk_.idx;
            if (ctx_.voice_state) ctx_.voice_state("");
            if (!header_desc_.empty() || frame_sync_end_ >= 0) {
                ctx_.report->event(Mode::DSTAR, "End of transmission" + (header_desc_.empty() ? std::string() : ": " + header_desc_), 2.0);
                ctx_.report->erase(Mode::DSTAR, "Current call");
            }
            header_desc_.clear();
            frame_sync_end_ = -1;
        }
    } else if (std::fabs(best) >= THRESHOLD) {
        pk_.active = true;
        pk_.best = best;
        pk_.idx = n;
        pk_.which = which;
        pk_.age = 0;
    }
}

// GPS / DPRS slow data -> a map point for the calling station
void DstarReceiver::gps_point(const std::string& line) {
    double lat = 0, lon = 0;
    std::string call;
    if (!gps_position(line, &lat, &lon, &call)) return;
    if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || (lat == 0 && lon == 0)) return;
    const std::string who = !my_.empty() ? my_ : call;
    if (who.empty()) return;
    kp::MapPoint p;
    p.id = who;
    p.lat = lat;
    p.lon = lon;
    p.label = who;
    p.kind = "person";
    p.info = "Callsign: " + who + (call.empty() || call == who ? "" : "\nDPRS callsign: " + call) +
             (msg_text_.empty() ? "" : "\nMessage: " + msg_text_) + "\nSource: " +
             (line.rfind("$$CRC", 0) == 0 ? "DPRS" : "GPS (NMEA)");
    p.ttl_s = 3600;
    ctx_.report->map(Mode::DSTAR, p);
}

void DstarReceiver::decode_header_bytes(const uint8_t* h, bool from_slow_data, int64_t hdr_sync) {
    uint16_t crc = crc16_x25(h, 39);
    if (crc != (h[39] | (h[40] << 8))) return;
    std::string my = callsign(h + 27, 8), sfx = callsign(h + 35, 4);
    std::string ur = callsign(h + 19, 8), r1 = callsign(h + 11, 8), r2 = callsign(h + 3, 8);
    std::string d = my + (sfx.empty() ? "" : "/" + sfx) + " -> " + ur;
    if (!r1.empty() || !r2.empty()) d += " via " + r1 + (r2.empty() ? "" : " / " + r2);
    Report& r = *ctx_.report;
    r.set(Mode::DSTAR, "MY (caller)", my + (sfx.empty() ? "" : " /" + sfx));
    if (my != my_) msg_text_.clear();
    my_ = my;
    r.set(Mode::DSTAR, "UR (destination)", ur);
    r.set(Mode::DSTAR, "RPT1", r1);
    r.set(Mode::DSTAR, "RPT2", r2);
    std::string flags;
    if (h[0] & 0x80) flags += "data ";
    if (h[0] & 0x40) flags += "via repeater ";
    if (h[0] & 0x20) flags += "interrupted ";
    if (h[0] & 0x08) flags += "urgent ";
    if (!flags.empty()) { flags.pop_back(); r.set(Mode::DSTAR, "Flags", flags); }
    r.set(Mode::DSTAR, "Current call", d);
    // the talker: MY callsign (the suffix is the radio model / a note)
    if (!my.empty() && my.find('?') == std::string::npos) {
        const std::string id = squeezed(my);
        std::string label = (sfx.empty() ? "" : "/" + sfx + " ") + "to " + squeezed(ur);
        if (!r1.empty() || !r2.empty()) label += " via " + squeezed(r1.empty() ? r2 : r1);
        if (!from_slow_data) {
            hdr_ok_sync_ = hdr_sync;
            hdr_id_ = id;
            hdr_label_ = label;
            if (call_start_ >= 0 && tx_hdr_sync_ == hdr_sync) tx_talker(id, label);
        } else if (call_start_ >= 0 && talker_id_.empty()) {
            tx_talker(id, label);   // late entry: the header copy in the slow data
        }
    }
    if (d != header_desc_) {
        header_desc_ = d;
        r.event(Mode::DSTAR, std::string(from_slow_data ? "Header (slow data): " : "Header: ") + d, 5.0);
    }
    if (from_slow_data) {
        if (ctx_.valid) ctx_.valid(Mode::DSTAR);   // inside voice frames: their superframe is reported
    } else {
        report_valid(ctx_, Mode::DSTAR, hdr_sync - 24 * SPS, hdr_sync + 661 * SPS);
    }
}

void DstarReceiver::decode_header(const SymSrc& s) {
    // 660 scrambled + interleaved bits follow the frame sync
    // x^7 + x^4 + 1 whitening, all-ones seed
    static const std::array<uint8_t, 660> scr = [] {
        std::array<uint8_t, 660> a{};
        uint8_t st[7] = {1, 1, 1, 1, 1, 1, 1};
        for (int i = 0; i < 660; i++) {
            uint8_t b = st[6] ^ st[3];
            a[i] = b;
            for (int j = 6; j > 0; j--) st[j] = st[j - 1];
            st[0] = b;
        }
        return a;
    }();
    float soft[660];
    for (int i = 0; i < 660; i++) {
        float v = s.sym(i + 1);   // > 0 = bit 1
        float sv = -v;            // viterbi convention: > 0 = bit 0
        if (scr[i]) sv = -sv;
        int row, col;
        if (i < 336) { col = i / 28; row = i % 28; }
        else { col = 12 + (i - 336) / 27; row = (i - 336) % 27; }
        soft[row * 24 + col] = sv;
    }
    Bits d = viterbi_decode(soft, 330, 3, {0x7, 0x5}, true);
    uint8_t h[41] = {0};
    for (int i = 0; i < 328; i++)
        if (d[i]) h[i / 8] |= static_cast<uint8_t>(1 << (i % 8));
    decode_header_bytes(h, false, s.sync_end);
    if (ctx_.freq_error) ctx_.freq_error(Mode::DSTAR, s.center);
}

void DstarReceiver::slow_data_block(const uint8_t* b) {
    Report& r = *ctx_.report;
    int type = b[0] & 0xF0, low = b[0] & 0x0F;
    if (type == 0x40 && low < 4) {
        msg_[low].assign(reinterpret_cast<const char*>(b + 1), 5);
        if (!msg_[0].empty() && !msg_[1].empty() && !msg_[2].empty() && !msg_[3].empty()) {
            std::string m = printable(msg_[0] + msg_[1] + msg_[2] + msg_[3]);
            if (!m.empty()) {
                r.set(Mode::DSTAR, "Message", m);
                msg_text_ = m;
                r.event(Mode::DSTAR, "Message: " + m, 60.0);
            }
            for (auto& x : msg_) x.clear();
        }
    } else if (type == 0x30 && low >= 1 && low <= 5) {
        std::string& gps = gps_line_;
        for (int i = 0; i < low; i++) {
            char c = static_cast<char>(b[1 + i]);
            if (c == '\r' || c == '\n') {
                std::string line = printable(gps);
                if (line.size() > 6) {
                    if (line.rfind("$GP", 0) == 0 || line.rfind("$$CRC", 0) == 0 || line.rfind("$GN", 0) == 0) {
                        r.set(Mode::DSTAR, line.rfind("$$CRC", 0) == 0 ? "DPRS" : "GPS", line.substr(0, 80));
                        if (ctx_.opts->verbose) r.event(Mode::DSTAR, "GPS: " + line.substr(0, 80), 10.0);
                        gps_point(line);
                    }
                }
                gps.clear();
            } else if (gps.size() < 200) {
                gps += c;
            }
        }
    } else if (type == 0x50 && low >= 1 && low <= 5) {
        // header copy, 5 bytes per block, 41 bytes in total
        uint8_t* hdr = hdr_copy_;
        int& pos = hdr_copy_pos_;
        if (pos + low > 45) pos = 0;
        std::copy(b + 1, b + 1 + low, hdr + pos);
        pos += low;
        if (pos >= 41) {
            // find the start: try every offset (the copy runs continuously)
            uint8_t cand[41];
            for (int off = 0; off + 41 <= pos; off++) {
                std::copy(hdr + off, hdr + off + 41, cand);
                if (crc16_x25(cand, 39) == (cand[39] | (cand[40] << 8))) { decode_header_bytes(cand, true); break; }
            }
            pos = 0;
        }
    }
}

void DstarReceiver::decode_superframe(const SymSrc& s) {
    // frames 1..20 after the sync frame; slow data = bits 72..95 of each
    static const uint8_t scr[3] = {0x70, 0x4F, 0x93};
    uint8_t sd[60];
    for (int f = 1; f <= 20; f++) {
        for (int byte = 0; byte < 3; byte++) {
            uint8_t v = 0;
            for (int bit = 0; bit < 8; bit++) {
                int k = 1 + (f - 1) * 96 + 72 + byte * 8 + bit;
                if (s.sym(k) > 0) v |= static_cast<uint8_t>(1 << bit);
            }
            sd[(f - 1) * 3 + byte] = v ^ scr[byte];
        }
    }
    for (int blk = 0; blk < 10; blk++) slow_data_block(sd + blk * 6);
}

// AMBE (3600x2400) voice frames 0..20 of the superframe whose data sync
// ends at s.sync_end -> the user's mbelib
void DstarReceiver::voice_superframe(const SymSrc& s) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    std::string who = header_desc_.empty() ? std::string("D-STAR") : header_desc_;
    if (!MbeLib::instance().available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": AMBE codec not installed (README: Digital voice codecs)");
        return;
    }
    if (!ambe_) ambe_ = std::make_unique<AmbeStream>(true);
    std::vector<float> pcm;
    pcm.reserve(21 * 160);
    int clean = 0;
    for (int f = 0; f <= 20; f++) {
        // frame 0's voice sits right before the sync, frame f >= 1 after it
        int k0 = f == 0 ? -95 : 1 + (f - 1) * 96;
        if (end_sync_end_ > s.sync_end && s.sync_end + static_cast<int64_t>(k0 + 71) * s.sps > end_sync_end_ - 48 * s.sps)
            break;   // transmission ended
        char fr[4][24] = {{0}};
        for (int i = 0; i < 72; i++)
            fr[mbe_tables::dW[i]][mbe_tables::dX[i]] = s.sym(k0 + i) > 0 ? 1 : 0;
        float out[160];
        if (ambe_->decode(fr, out) <= 2) clean++;
        pcm.insert(pcm.end(), out, out + 160);
    }
    if (pcm.empty() || clean == 0) return;
    if (ctx_.voice) ctx_.voice(Mode::DSTAR, pcm.data(), pcm.size());
    if (ctx_.voice_state)
        ctx_.voice_state(who + ": playing (AMBE, " + std::to_string(clean) + "/" + std::to_string(pcm.size() / 160) +
                         " frames clean)");
}

void DstarReceiver::process(const float* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float x = std::clamp(d[i], -4000.0f, 4000.0f);
        sum_ += x - hist_[pos_];
        hist_[pos_] = x;
        pos_ = (pos_ + 1) % SPS;
        b_.push(sum_ / SPS);
        correlate();
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!it->src.has(it->need_k)) { ++it; continue; }
        if (it->which == 0) {
            decode_header(it->src);
        } else {
            // a data sync 420 ms after the previous one confirms D-STAR
            if (frame_sync_end_ >= 0) {
                int64_t dt = it->src.sync_end - frame_sync_end_;
                int64_t sf = static_cast<int64_t>(SUPERFRAME_BITS) * SPS;
                int64_t rem = dt % sf;
                if (dt > 0 && (rem < 3 * SPS || sf - rem < 3 * SPS)) {
                    // everything since the previous data sync is D-STAR (at most 2 superframes back)
                    report_valid(ctx_, Mode::DSTAR, std::max(frame_sync_end_, it->src.sync_end - 2 * sf), it->src.sync_end);
                    if (ctx_.freq_error) ctx_.freq_error(Mode::DSTAR, it->src.center);
                    ctx_.report->set(Mode::DSTAR, "Voice", "receiving");
                }
            }
            frame_sync_end_ = it->src.sync_end;
            decode_superframe(it->src);
            voice_superframe(it->src);
        }
        it = jobs_.erase(it);
    }
    // data syncs stopped (two missed in a row) without an end pattern: over
    if (call_start_ >= 0 && b_.end() - last_sync_ > 2 * static_cast<int64_t>(SUPERFRAME_BITS) * SPS + 10 * SPS)
        tx_end(voice_end_);
    int64_t keep = b_.end() - 30000;
    for (const auto& j : jobs_) keep = std::min(keep, j.src.sync_end - 2000);
    if (keep > b_.begin() + 4096) b_.trim_before(keep);
}

}  // namespace dig
