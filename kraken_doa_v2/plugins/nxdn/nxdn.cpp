// NXDN (Kenwood NEXEDGE / Icom IDAS) conventional / traffic channels:
// 4FSK, 192-symbol frames at 4800 Bd (NXDN96, 40 ms) or 2400 Bd (NXDN48,
// 80 ms). Both rates are searched at once on the discriminator, each with
// its own root-raised-cosine (0.2) filter. Per frame: frame sync word, LICH
// (channel type / payload layout), SACCH (RAN + slow signalling, CRC-6) and
// FACCH1 (fast signalling, CRC-12) - both rate-1/2 K=5 convolutional,
// punctured - and four AMBE+2 voice frames (played through a user-installed
// mbelib, like DMR). The trunking control channel's CAC is not decoded.

#include "nxdn.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace dig {

namespace {

constexpr int FRAME_SYMS = 192;
constexpr int FSW_SYMS = 10;
constexpr float THRESHOLD = 0.88f;   // 10 symbols only: keep it tight
// FSW 0xCDF59 (dibits 11 00 11 01 11 11 01 01 10 01): -3 +1 -3 +3 -3 -3 +3 +3 -1 +3
const float FSW[FSW_SYMS] = {-3, 1, -3, 3, -3, -3, 3, 3, -1, 3};
// Whitening of the whole frame (flips the first bit of a dibit = negates
// the symbol), from the NXDN common air interface
const uint8_t SCRAMBLER[48] = {
    0x00, 0x00, 0x00, 0x82, 0xA0, 0x88, 0x8A, 0x00, 0xA2, 0xA8, 0x82, 0x8A, 0x82, 0x02, 0x20, 0x08,
    0x8A, 0x20, 0xAA, 0xA2, 0x82, 0x08, 0x22, 0x8A, 0xAA, 0x08, 0x28, 0x88, 0x28, 0x28, 0x00, 0x0A,
    0x02, 0x82, 0x20, 0x28, 0x82, 0x2A, 0xAA, 0x20, 0x22, 0x80, 0xA8, 0x8A, 0x08, 0xA0, 0xAA, 0x02};

std::vector<float> rrc(int sps, int span, float alpha) {
    std::vector<float> h;
    for (int i = -span * sps; i <= span * sps; i++) {
        float t = static_cast<float>(i) / sps, v;
        const float pi = static_cast<float>(M_PI);
        if (std::fabs(t) < 1e-6f) v = 1.0f - alpha + 4.0f * alpha / pi;
        else if (std::fabs(std::fabs(4.0f * alpha * t) - 1.0f) < 1e-6f)
            v = alpha / std::sqrt(2.0f) * ((1 + 2 / pi) * std::sin(pi / (4 * alpha)) + (1 - 2 / pi) * std::cos(pi / (4 * alpha)));
        else
            v = (std::sin(pi * t * (1 - alpha)) + 4 * alpha * t * std::cos(pi * t * (1 + alpha))) /
                (pi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h.push_back(v);
    }
    float s = 0;
    for (float v : h) s += v;
    for (float& v : h) v /= s;
    return h;
}

// Punctured rate-1/2 K=5 code (G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4): the
// received bits are deinterleaved (written in `rows` rows, read by columns),
// the punctured positions - `offsets` within every `period` coded bits - are
// inserted as erasures, then Viterbi. Returns nout decoded bits (tail incl.)
Bits decode_conv(const float* soft, int n, int rows, int period, std::initializer_list<int> offsets, int nout) {
    std::vector<float> d(n);
    const int cols = n / rows;
    for (int i = 0; i < n; i++) d[i] = soft[(i % cols) * rows + i / cols];
    std::vector<float> m;
    m.reserve(2 * nout);
    int i = 0;
    while (i < n) {
        int pos = static_cast<int>(m.size()) % period;
        bool punct = false;
        for (int o : offsets) punct |= (pos == o);
        m.push_back(punct ? 0.0f : d[i++]);
    }
    while (static_cast<int>(m.size()) < 2 * nout) m.push_back(0.0f);
    return viterbi_decode(m.data(), nout, 5, {0x19, 0x17}, true);
}

// CAC CRC-16 (x^16+x^12+x^5+1) as run over the 155 info + 16 CRC bits: the
// register is seeded with 0xC3EE and must end at zero
bool cac_crc_ok(const uint8_t* b, int n) {
    uint32_t crc = 0xC3EE;
    for (int i = 0; i < n; i++) {
        crc = ((crc << 1) | b[i]) & 0x1FFFF;
        if (crc & 0x10000) crc = (crc & 0xFFFF) ^ 0x1021;
    }
    return ((crc ^ 0xFFFF) & 0xFFFF) == 0;
}

// Location ID -> "category system/site"
std::string location(uint32_t id) {
    char b[96];
    int cat = static_cast<int>(id >> 22);
    uint32_t sys = 0, site = 0;
    const char* c = "reserved";
    if (cat == 0) { c = "global"; sys = (id >> 12) & 0x3FF; site = id & 0xFFF; }
    else if (cat == 2) { c = "regional"; sys = (id >> 8) & 0x3FFF; site = id & 0xFF; }
    else if (cat == 1) { c = "local"; sys = (id >> 5) & 0x1FFFF; site = id & 0x1F; }
    snprintf(b, sizeof b, "system %u, site %u (%s, location ID %06X)", sys, site, c, id);
    return b;
}

std::string id_str(uint32_t v) { return std::to_string(v); }

}  // namespace

NxdnReceiver::NxdnReceiver(RxContext& c) : ctx_(c) {
    br_[0].sps = 10;
    br_[0].name = "NXDN96 (4800 Bd, 12.5 kHz)";
    br_[1].sps = 20;
    br_[1].name = "NXDN48 (2400 Bd, 6.25 kHz)";
    for (auto& b : br_) {
        b.taps = rrc(b.sps, 3, 0.2f);
        b.hist.assign(b.taps.size(), 0.0f);
    }
    float m = 0;
    for (float v : FSW) m += v;
    m /= FSW_SYMS;
    float e = 0;
    for (float v : FSW) { fsw_t_.push_back(v - m); e += (v - m) * (v - m); }
    for (float& v : fsw_t_) v /= std::sqrt(e);
}

void NxdnReceiver::reset() {
    for (auto& b : br_) {
        std::fill(b.hist.begin(), b.hist.end(), 0.0f);
        b.buf = SampleBuf();
        b.pk = Branch::Peak();
    }
    jobs_.clear();
    ran_ = -1;
    sacch_have_ = 0;
    last_frame_[0] = last_frame_[1] = -1;
    last_locked_[0] = last_locked_[1] = false;
    call_.clear();
    cipher_ = 0;
    if (ambe_) ambe_->reset();
    // the host resets its own talker state together with ours
    call_start_ = -1;
    talker_id_.clear();
    talker_label_.clear();
}

// --- talkers (RxContext::talker) ----------------------------------------------
// A transmission = valid voice / VCALL frames less than GAP apart; its talker
// is the VCALL's source unit ID (SACCH superframe or FACCH1). Reported with
// the frame boundaries, so the host can cut exactly this radio's samples out
// of the VFO's stream for the DoA.
void NxdnReceiver::begin_call_if_new() {
    if (call_start_ >= 0 && frame_a_ - voice_end_ <= GAP) return;
    end_talker(voice_end_);
    call_start_ = frame_a_;
    voice_end_ = frame_a_;
}

void NxdnReceiver::talker_frame() {
    begin_call_if_new();
    voice_end_ = std::max(voice_end_, frame_b_);
    if (!talker_id_.empty() && ctx_.talker) ctx_.talker(talker_id_, talker_label_, call_start_, voice_end_, 0);
}

void NxdnReceiver::set_talker(uint32_t src, const std::string& label) {
    if (src == 0) return;
    begin_call_if_new();
    const std::string id = std::to_string(src);
    if (!talker_id_.empty() && talker_id_ != id) {
        // another radio without a TX_REL in between: the frames since the old
        // one was last named can't be told apart - leave them out
        end_talker(lc_end_);
        call_start_ = frame_a_;
        voice_end_ = frame_a_;
    }
    talker_id_ = id;
    talker_label_ = label;
    lc_end_ = frame_b_;
}

void NxdnReceiver::end_talker(int64_t at) {
    if (!talker_id_.empty() && ctx_.talker_end) ctx_.talker_end(at, 0);
    talker_id_.clear();
    call_start_ = -1;
}

void NxdnReceiver::tick(int64_t now) {
    if (call_start_ >= 0 && now - voice_end_ > GAP) end_talker(voice_end_);
}

void NxdnReceiver::layer3(const uint8_t* m, int nbits, const char* via) {
    if (nbits < 56) return;
    uint8_t b[10] = {0};
    for (int i = 0; i < std::min(nbits, 80) / 8; i++) b[i] = static_cast<uint8_t>(bits_to_u32(m + 8 * i, 8));
    int type = b[0] & 0x3F;
    Report& r = *ctx_.report;
    switch (type) {
        case 0x01: {   // VCALL
            bool group = (b[2] & 0x80) == 0;
            uint32_t src = (b[3] << 8) | b[4], dst = (b[5] << 8) | b[6];
            int cipher = nbits >= 64 ? b[7] >> 6 : 0, key = nbits >= 64 ? b[7] & 0x3F : 0;
            static const char* cn[4] = {"", "scrambler", "DES", "AES"};
            std::string d = (group ? "TG " : "unit ") + id_str(dst) + " <- " + id_str(src);
            if (cipher) d += std::string(" [encrypted, ") + cn[cipher] + " key " + std::to_string(key) + "]";
            cipher_ = cipher;
            if (d != call_) {
                call_ = d;
                r.event(Mode::NXDN, std::string("Voice: ") + d, 5.0);
            }
            r.set(Mode::NXDN, "Current call", d);
            r.set(Mode::NXDN, "Last talkgroup", group ? id_str(dst) : "-");
            r.set(Mode::NXDN, "Last source", id_str(src));
            r.set(Mode::NXDN, "Encryption", cipher ? cn[cipher] : "clear");
            set_talker(src, (group ? "TG " : "unit call to ") + id_str(dst) + (frame_outbound_ ? " · via repeater" : ""));
            frame_vcall_ = true;
            break;
        }
        case 0x08: {   // TX_REL
            // the release frames are still this radio's
            if (call_start_ >= 0) {
                voice_end_ = std::max(voice_end_, frame_b_);
                end_talker(voice_end_);
            }
            frame_rel_ = true;
            if (!call_.empty()) r.event(Mode::NXDN, "Call ended: " + call_, 2.0);
            call_.clear();
            cipher_ = 0;
            r.erase(Mode::NXDN, "Current call");
            if (ctx_.voice_state) ctx_.voice_state("");
            break;
        }
        case 0x09: {   // DCALL_HDR
            uint32_t src = (b[3] << 8) | b[4], dst = (b[5] << 8) | b[6];
            r.event(Mode::NXDN, "Data call: " + id_str(src) + " -> " + id_str(dst), 3.0);
            break;
        }
        case 0x10:     // IDLE
            break;
        default:
            if (ctx_.opts->verbose) {
                char h[8];
                snprintf(h, sizeof h, "0x%02X", type);
                r.event(Mode::NXDN, std::string(via) + " message type " + h, 5.0);
            }
            break;
    }
}

// Outbound control channel message (CAC)
void NxdnReceiver::cac_message(const uint8_t* m) {
    Report& r = *ctx_.report;
    int type = static_cast<int>(bits_to_u32(m + 2, 6));
    auto f = [m](int pos, int len) { return bits_to_u32(m + pos, len); };
    const double bcast = ctx_.opts->verbose ? 5.0 : 120.0;
    char h[16];
    switch (type) {
        case 0x18: {   // SITE_INFO
            std::string loc = location(f(8, 24));
            snprintf(h, sizeof h, "%04X", f(48, 16));
            r.set(Mode::NXDN, "Site", loc);
            r.set(Mode::NXDN, "Control channels", std::to_string(f(124, 10)) + (f(134, 10) ? ", " + std::to_string(f(134, 10)) : ""));
            r.event(Mode::NXDN, "Site information: " + loc + ", control channel " + std::to_string(f(124, 10)), bcast);
            break;
        }
        case 0x19: {   // SRV_INFO
            std::string loc = location(f(8, 24));
            snprintf(h, sizeof h, "%04X", f(32, 16));
            r.set(Mode::NXDN, "Site", loc);
            r.set(Mode::NXDN, "Service info", std::string("0x") + h);
            r.event(Mode::NXDN, "Service information: " + loc, bcast);
            break;
        }
        case 0x1B:     // ADJ_SITE_INFO
            r.event(Mode::NXDN, "Adjacent site: " + location(f(8, 24)), bcast);
            break;
        case 0x04:     // VCALL_ASSGN
        case 0x0E: {   // DCALL_ASSGN
            bool group = f(16, 3) != 4;
            std::string d = std::string(type == 0x04 ? "Voice" : "Data") + " grant: " + (group ? "TG " : "unit ") +
                            std::to_string(f(40, 16)) + " <- " + std::to_string(f(24, 16)) + " on channel " +
                            std::to_string(f(62, 10));
            r.event(Mode::NXDN, d, 3.0);
            r.set(Mode::NXDN, "Last grant", d);
            break;
        }
        case 0x10:     // IDLE
            break;
        case 0x11:
            r.event(Mode::NXDN, "Disconnect: unit " + std::to_string(f(40, 16)), 3.0);
            break;
        default:
            if (ctx_.opts->verbose) {
                snprintf(h, sizeof h, "0x%02X", type);
                r.event(Mode::NXDN, std::string("CAC message type ") + h, 5.0);
            }
            break;
    }
}

void NxdnReceiver::voice(const uint8_t* bits, int nframes) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    std::string who = call_.empty() ? std::string("NXDN") : call_;
    if (!MbeLib::instance().available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": AMBE+2 codec not installed (README: Digital voice codecs)");
        return;
    }
    if (cipher_) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": encrypted, muted");
        return;
    }
    if (!ambe_) ambe_ = std::make_unique<AmbeStream>(false);
    std::vector<float> pcm;
    int clean = 0;
    for (int f = 0; f < nframes; f++) {
        const uint8_t* v = bits + 72 * f;
        char fr[4][24] = {{0}};
        for (int i = 0; i < 36; i++) {
            fr[mbe_tables::nW[i]][mbe_tables::nX[i]] = static_cast<char>(v[2 * i]);
            fr[mbe_tables::nY[i]][mbe_tables::nZ[i]] = static_cast<char>(v[2 * i + 1]);
        }
        float out[160];
        if (ambe_->decode(fr, out) <= 2) clean++;
        pcm.insert(pcm.end(), out, out + 160);
    }
    if (clean == 0) return;
    if (ctx_.voice) ctx_.voice(Mode::NXDN, pcm.data(), pcm.size());
    if (ctx_.voice_state)
        ctx_.voice_state(who + ": playing (AMBE+2, " + std::to_string(clean) + "/" + std::to_string(nframes) + " frames clean)");
}

void NxdnReceiver::decode_frame(const Branch& b, const SymSrc& s) {
    // frame symbol j (0 = first FSW symbol) is k = j - 9
    uint8_t bits[384];
    float soft[384];
    for (int j = 0; j < FRAME_SYMS; j++) {
        float v = s.sym(j - (FSW_SYMS - 1));
        uint8_t d = SymSrc::dibit(v);
        bits[2 * j] = d >> 1;
        bits[2 * j + 1] = d & 1;
        // soft bits (> 0 = 0): first bit = sign, second bit = outer level
        soft[2 * j] = v;
        soft[2 * j + 1] = 2.0f - std::fabs(v);
    }
    // descramble: flipping the first bit negates the symbol
    for (int i = 0; i < 384; i++) {
        if ((SCRAMBLER[i / 8] >> (7 - i % 8)) & 1) {
            bits[i] ^= 1;
            soft[i] = -soft[i];
        }
    }
    // LICH: 8 bits on the first bit of symbols 10..17, the second bit is 1
    uint32_t lich = 0;
    int ones = 0;
    for (int i = 0; i < 8; i++) {
        lich = (lich << 1) | bits[20 + 2 * i];
        ones += bits[21 + 2 * i];
    }
    int parity = ((lich >> 7) ^ (lich >> 6) ^ (lich >> 5) ^ (lich >> 4)) & 1;
    if (ones < 7 || parity != static_cast<int>(lich & 1)) return;
    int rfct = (lich >> 6) & 3, usc = (lich >> 4) & 3, option = (lich >> 2) & 3;
    bool outbound = (lich >> 1) & 1;
    Report& r = *ctx_.report;
    const int bi = &b == &br_[0] ? 0 : 1;
    if (rfct == 0) {
        // RCCH (trunking control channel): CAC, 300 bits -> 175
        Bits cac = decode_conv(soft + 36, 300, 25, 14, {3, 11}, 175);
        if (!cac_crc_ok(cac.data(), 171)) return;
        r.set(Mode::NXDN, "Channel type", "Trunking control channel");
        r.set(Mode::NXDN, "Variant", b.name);
        int ran = static_cast<int>(bits_to_u32(cac.data() + 2, 6));
        if (ran != ran_) {
            ran_ = ran;
            r.event(Mode::NXDN, "RAN " + std::to_string(ran), 30.0);
        }
        r.set(Mode::NXDN, "RAN", std::to_string(ran));
        cac_message(cac.data() + 8);
        if (ctx_.valid) ctx_.valid(Mode::NXDN);
        if (ctx_.freq_error) ctx_.freq_error(Mode::NXDN, s.center);
        return;
    }
    // SACCH: 60 bits -> 36 (26 info + CRC-6 + tail)
    bool sacch_ok = false;
    Bits sa;
    if (usc != 1) {
        sa = decode_conv(soft + 36, 60, 5, 6, {5}, 36);
        sacch_ok = crc_bits_ones(sa.data(), 26, 6, 0x27) == bits_to_u32(sa.data() + 26, 6);
    }
    // payload: option = which halves FACCH1 steals (0 both, 1 first, 2 second)
    bool facch_half[2] = {option == 0 || option == 1, option == 0 || option == 2};
    bool facch_ok[2] = {false, false};
    Bits fa[2];
    for (int h = 0; h < 2; h++) {
        if (!facch_half[h]) continue;
        fa[h] = decode_conv(soft + 96 + 144 * h, 144, 9, 4, {1}, 96);
        facch_ok[h] = crc_bits_ones(fa[h].data(), 80, 12, 0x80F) == bits_to_u32(fa[h].data() + 80, 12);
    }
    // Validity: a 6-bit CRC passes 1 random word in 64, so a SACCH counts
    // only on a frame that follows the previous good one exactly one frame
    // later (the stream is locked); FACCH1's CRC-12 counts on its own.
    const int64_t period = static_cast<int64_t>(FRAME_SYMS) * b.sps;
    bool adjacent = last_frame_[bi] >= 0 && std::llabs(s.sync_end - last_frame_[bi] - period) <= b.sps;
    bool locked = sacch_ok && adjacent;
    if (sacch_ok) last_frame_[bi] = s.sync_end;
    else if (!adjacent || s.sync_end - last_frame_[bi] > 4 * period) last_frame_[bi] = -1;
    last_locked_[bi] = locked;
    bool valid = locked || facch_ok[0] || facch_ok[1];
    if (!valid) return;

    // this frame's samples (frame symbol 0 is centred FSW_SYMS - 1 symbols before the sync end)
    frame_a_ = s.sync_end - static_cast<int64_t>((FSW_SYMS - 1) * b.sps + b.sps / 2);
    frame_b_ = frame_a_ + static_cast<int64_t>(FRAME_SYMS) * b.sps;
    frame_vcall_ = frame_rel_ = false;
    frame_outbound_ = outbound;

    static const char* ct[4] = {"", "Trunked traffic (RTCH)", "Conventional (RDCH)", "Composite control/traffic (RTCH-C)"};
    r.set(Mode::NXDN, "Channel type", ct[rfct]);
    r.set(Mode::NXDN, "Variant", b.name);
    r.set(Mode::NXDN, "Direction", outbound ? "outbound (repeater)" : "inbound / direct");
    if (sacch_ok) {
        int sr = static_cast<int>(bits_to_u32(sa.data(), 2));
        int ran = static_cast<int>(bits_to_u32(sa.data() + 2, 6));
        // the RAN comes from superframe SACCHs; the non-superframe / idle
        // ones of a call's release carry 0
        if (usc != 2 && ran_ >= 0) ran = ran_;
        if (ran != ran_) {
            ran_ = ran;
            r.event(Mode::NXDN, "RAN " + std::to_string(ran), 30.0);
        }
        r.set(Mode::NXDN, "RAN", std::to_string(ran));
        if (usc == 2) {
            // superframe: 4 fragments, SR 3 (first) .. 0 (last)
            int idx = 3 - sr;
            if (idx == 0) sacch_have_ = 0;
            std::copy(sa.begin() + 8, sa.begin() + 26, sacch_sf_ + 18 * idx);
            sacch_have_ |= 1 << idx;
            if (sr == 0 && sacch_have_ == 15) layer3(sacch_sf_, 72, "SACCH");
        }
    }
    for (int h = 0; h < 2; h++) {
        if (facch_ok[h]) layer3(fa[h].data(), 80, "FACCH1");
        else if (!facch_half[h]) voice(bits + 96 + 144 * h, 2);   // RTCH / RDCH / RTCH-C all carry voice
    }
    // voice in a half (option != 0; not UDCH data) or the call's VCALL: part
    // of the transmission. Idle frames between calls are not.
    if (!frame_rel_ && usc != 1 && (option != 0 || frame_vcall_)) talker_frame();
    if (ctx_.valid) ctx_.valid(Mode::NXDN);
    if (ctx_.freq_error) ctx_.freq_error(Mode::NXDN, s.center);
}

void NxdnReceiver::process(const float* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float x = std::clamp(d[i], -4000.0f, 4000.0f);
        for (int bi = 0; bi < 2; bi++) {
            Branch& b = br_[bi];
            const size_t nt = b.taps.size();
            b.hist[b.pos] = x;
            float acc = 0;
            size_t p = b.pos;
            for (size_t k = 0; k < nt; k++) {
                acc += b.taps[k] * b.hist[p];
                p = p == 0 ? nt - 1 : p - 1;
            }
            b.pos = (b.pos + 1) % nt;
            b.buf.push(acc);
            // FSW correlation at symbol spacing
            const int64_t last = b.buf.end() - 1;
            const int64_t first = last - static_cast<int64_t>(FSW_SYMS - 1) * b.sps;
            if (first < b.buf.begin()) continue;
            float w[FSW_SYMS], m = 0;
            for (int k = 0; k < FSW_SYMS; k++) { w[k] = b.buf.at(first + k * b.sps); m += w[k]; }
            m /= FSW_SYMS;
            float e = 0, c = 0;
            for (int k = 0; k < FSW_SYMS; k++) { float v = w[k] - m; e += v * v; c += fsw_t_[k] * v; }
            c = e > 1e-3f ? c / std::sqrt(e) : 0.0f;
            auto& pk = b.pk;
            if (pk.active) {
                pk.age++;
                if (std::fabs(c) > std::fabs(pk.best)) { pk.best = c; pk.idx = last; }
                if (pk.age >= b.sps) {
                    pk.active = false;
                    // level fit on the FSW symbols (they include +-1 levels)
                    float sx = 0, sy = 0, sxx = 0, sxy = 0;
                    const int64_t f0 = pk.idx - static_cast<int64_t>(FSW_SYMS - 1) * b.sps;
                    for (int k = 0; k < FSW_SYMS; k++) {
                        float y = b.buf.at(f0 + k * b.sps);
                        sx += FSW[k]; sy += y; sxx += FSW[k] * FSW[k]; sxy += FSW[k] * y;
                    }
                    float scale = (FSW_SYMS * sxy - sx * sy) / (FSW_SYMS * sxx - sx * sx);
                    float center = (sy - scale * sx) / FSW_SYMS;
                    if (std::fabs(scale) > 40.0f) {
                        SymSrc src;
                        src.buf = &b.buf;
                        src.sync_end = pk.idx;
                        src.sps = b.sps;
                        src.scale = scale;
                        src.center = center;
                        jobs_.push_back({bi, src});
                    }
                }
            } else if (std::fabs(c) >= THRESHOLD) {
                pk.active = true;
                pk.best = c;
                pk.idx = last;
                pk.age = 0;
            }
        }
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!it->src.has(FRAME_SYMS - FSW_SYMS)) { ++it; continue; }
        decode_frame(br_[it->branch], it->src);
        it = jobs_.erase(it);
    }
    tick(br_[0].buf.end());
    for (auto& b : br_) {
        int64_t keep = b.buf.end() - 3 * FRAME_SYMS * b.sps;
        for (const auto& j : jobs_) keep = std::min(keep, j.src.sync_end - 20 * b.sps);
        if (keep > b.buf.begin() + 8192) b.buf.trim_before(keep);
    }
}

}  // namespace dig
