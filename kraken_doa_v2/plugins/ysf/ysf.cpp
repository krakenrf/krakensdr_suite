// Yaesu System Fusion (YSF, "C4FM Fusion"), written from Yaesu's "Amateur
// Radio Digital Standards" (rev. 1.02, 2015-07-29; section numbers below are
// that document's). C4FM (3.2): 4800 Bd 4FSK, root-raised-cosine 0.2 at both
// ends, +-900 / +-2700 Hz (wide) or half that (narrow). 100 ms frames of 960
// bits (4.1): FS (40 bits, D471C9634D), FICH (200: 32 data bits, CRC-16,
// four Golay(24,12), the rate-1/2 K=5 code, 5 x 20 dibit interleave), then
// 720 payload bits laid out by the FICH's frame and data type:
//   Header / Terminator, Data FR: 5 x (DCH-1 72 + DCH-2 72) - two 160-bit
//     units (HC / TC: CSD1 = destination + source, CSD2 = downlink + uplink)
//   V/D mode 1: 5 x (DCH 72 + VCH 72) - one 160-bit unit, five 72-bit
//     AMBE+2 3600x2450 frames
//   V/D mode 2 ("DN"): 5 x (DCH 40 + VCH 72 + VeCH 32) - one 80-bit unit,
//     five 49-bit AMBE+2 frames with their own majority protection
//   Voice FR ("VW"): 5 x VCH 144 - 7200 bit/s full-rate frames (88 voice +
//     56 FEC bits); the sub header after the HC (FT=1 FN=0) carries CSD3 in
//     a DCH and only VCH 3 and 4
// DCH unit (4.5 (2)): PN(9,5) whitening, CRC-16, the K=5 code, 9 (or 5) x 20
// dibit interleave. Voice: VW through the built-in IMBE decoder, DN / V/D
// mode 1 through a user-installed mbelib.

#include "ysf.hpp"
#include "dig_fec.hpp"
#include "kraken_dsp.hpp"
#include "mbe_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dig {

namespace {

constexpr int SPS = 10;                       // 48 kHz / 4800 Bd
constexpr int FRAME_SYMS = 480;               // 960 bits
constexpr int FS_SYMS = 20;                   // 40 bits
constexpr int64_t FRAME = FRAME_SYMS * SPS;   // 4800 samples = 100 ms
constexpr int PAYLOAD = 240;                  // first payload bit (FS 40 + FICH 200)
constexpr float THRESHOLD = 0.75f;            // normalized FS correlation (20 symbols)
constexpr int WIN = 3;                        // the frame grid's search window, +- samples
constexpr int N2 = 4;                         // out of sync after 4 missed frames (4.7 (2))
constexpr int RRC_SPAN = 4;
// samples between an input sample and its RRC output: this RRC + FmFrontEnd's
// 63-tap channel filter
constexpr int64_t DELAY = RRC_SPAN * SPS + 31;
// Trellis code (Appendix B, Figure B-1): G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2
// + D^4, G1 sent first
const std::vector<uint32_t> POLYS = {0x19, 0x17};

// FS D471C9634D as symbols (Table 3-1: dibit 00 +1, 01 +3, 10 -1, 11 -3)
struct FsSyms {
    float v[FS_SYMS];
    FsSyms() {
        const uint64_t w = 0xD471C9634DULL;
        static const float L[4] = {1, 3, -1, -3};
        for (int i = 0; i < FS_SYMS; i++) v[i] = L[(w >> (38 - 2 * i)) & 3];
    }
};
const FsSyms FS;

// CRC-16 x^16+x^12+x^5+1, registers start at zero, inverted at the end
// (Figures 4-25 / 4-29), sent S15 first
bool crc_ok(const uint8_t* b, int n) {
    return static_cast<uint16_t>(crc16_ccitt_bits(b, n, 0) ^ 0xFFFF) == bits_to_u32(b + n, 16);
}

// Bytes as text: trailing spaces / NULs off; non-printable content as hex
std::string text(const uint8_t* bits, int nbytes) {
    std::string s;
    for (int i = 0; i < nbytes; i++) s += static_cast<char>(bits_to_u32(bits + 8 * i, 8));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E) {
            std::string h;
            char b[4];
            for (int i = 0; i < nbytes; i++) {
                snprintf(b, sizeof b, "%02X", bits_to_u32(bits + 8 * i, 8));
                h += b;
            }
            return h;
        }
    return s;
}

void dump_bits(const char* what, int64_t at, int k, const uint8_t* b, int n) {
    std::string s(static_cast<size_t>(n), '0');
    for (int i = 0; i < n; i++) s[static_cast<size_t>(i)] = static_cast<char>('0' + (b[i] & 1));
    fprintf(stderr, "YSF-VOICE t=%.4f mode=%s k=%d bits=%s\n", static_cast<double>(at) / 48000.0, what, k, s.c_str());
}

}  // namespace

// --- channel coding (4.5) ------------------------------------------------------
namespace ysf {

// PN(9,5) (Figure 4-28): S08..S00 = 111001001, output S00, S08 <- S00 ^ S04;
// restarted for each unit
const uint8_t* pn9() {
    static const struct Seq {
        uint8_t b[160];
        Seq() {
            uint32_t r = 0x1C9;
            for (int i = 0; i < 160; i++) {
                b[i] = r & 1;
                uint32_t fb = (r ^ (r >> 4)) & 1;
                r = (r >> 1) | (fb << 8);
            }
        }
    } seq;
    return seq.b;
}

// FICH (4.5 (1), Figure 4-26): the 100 dibits were written in rows of 5 and
// sent by columns (S00 S05 S10 ... S95 S01 ...)
bool decode_fich(const float* soft, Fich& f) {
    float c[200];
    for (int t = 0; t < 100; t++) {
        const int s = (t % 20) * 5 + t / 20;
        c[2 * s] = soft[2 * t];
        c[2 * s + 1] = soft[2 * t + 1];
    }
    Bits b = viterbi_decode(c, 100, 5, POLYS, true);
    // 4 x Golay(24,12) (Appendix A: d11..d0 p11..p0) -> 32 data + 16 CRC
    uint8_t d[48];
    for (int w = 0; w < 4; w++) {
        uint32_t msg = 0;
        if (golay24().decode(bits_to_u32(b.data() + 24 * w, 24), &msg, 3) < 0) return false;
        u64_to_bits(msg, 12, d + 12 * w);
    }
    if (!crc_ok(d, 32)) return false;
    const uint32_t v = bits_to_u32(d, 32);
    f.raw = v;
    f.fi = (v >> 30) & 3;
    f.cs = (v >> 28) & 3;
    f.cm = (v >> 26) & 3;
    f.bn = (v >> 24) & 3;
    f.bt = (v >> 22) & 3;
    f.fn = (v >> 19) & 7;
    f.ft = (v >> 16) & 7;
    f.dev = (v >> 14) & 1;
    f.mr = (v >> 11) & 7;
    f.voip = (v >> 10) & 1;
    f.dt = (v >> 8) & 3;
    f.sql = (v >> 7) & 1;
    f.sc = v & 0x7F;
    return true;
}

// DCH unit (4.5 (2), Figures 4-30 / 4-31): rows of 9 (360 bits) or 5 (200)
// dibits, 20 rows, sent by columns. Whitening came before the CRC (Figure
// 4-27): the CRC covers the whitened data
bool decode_dch(const float* soft, int ncoded, uint8_t* data) {
    const int nd = ncoded / 2, m = nd / 20;
    float c[360];
    for (int t = 0; t < nd; t++) {
        const int s = (t % 20) * m + t / 20;
        c[2 * s] = soft[2 * t];
        c[2 * s + 1] = soft[2 * t + 1];
    }
    Bits b = viterbi_decode(c, nd, 5, POLYS, true);   // data + CRC + 4 tail bits
    const int ndata = nd - 20;
    if (!crc_ok(b.data(), ndata)) return false;
    const uint8_t* pn = pn9();
    for (int i = 0; i < ndata; i++) data[i] = b[static_cast<size_t>(i)] ^ pn[i];
    return true;
}

// V/D mode 2 voice (4.5 (3), Figure 4-32): 49 bits = 27 sent three times in
// a row (81) + 22 + one 0 bit, whitened (PN(9,5) from its start), written
// in 4 rows of 26 and sent by columns (S0 S26 S52 S78 S1 ...); VCH = the
// first 72 bits sent, VeCH the last 32
int decode_vch2(const float* soft, uint8_t* out) {
    float s[104];
    for (int t = 0; t < 104; t++) s[(t % 4) * 26 + t / 4] = soft[t];
    const uint8_t* pn = pn9();
    for (int i = 0; i < 104; i++)
        if (pn[i]) s[i] = -s[i];
    int dissent = 0;
    for (int k = 0; k < 27; k++) {
        const float a = s[3 * k], b = s[3 * k + 1], c = s[3 * k + 2];
        out[k] = (a + b + c) < 0;   // soft majority
        const int ones = (a < 0) + (b < 0) + (c < 0);
        if (ones == 1 || ones == 2) dissent++;
    }
    for (int k = 0; k < 22; k++) out[27 + k] = s[81 + k] < 0;
    return dissent;
}

}  // namespace ysf

// --- receiver ------------------------------------------------------------------
YsfReceiver::YsfReceiver(RxContext& c) : ctx_(c) {
    taps_ = kp::fir_rrc(SPS, RRC_SPAN, 0.2f);
    hist_.assign(taps_.size(), 0.0f);
    float m = 0;
    for (float v : FS.v) m += v;
    m /= FS_SYMS;
    float e = 0;
    for (float v : FS.v) {
        fs_t_.push_back(v - m);
        e += (v - m) * (v - m);
    }
    for (float& v : fs_t_) v /= std::sqrt(e);
    const char* d = std::getenv("KRAKEN_YSF_DUMP");
    dump_ = d && *d && *d != '0';
}

void YsfReceiver::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    pos_ = 0;
    buf_ = SampleBuf();
    pk_ = Peak();
    jobs_.clear();
    std::fill(std::begin(cring_), std::end(cring_), 0.0f);
    expect_ = -1;
    misses_ = 0;
    last_sync_ = -1;
    call_ = Call();
    for (auto& d : data_) d.clear();
    last_end_ = 0;
    facts_.clear();
    imbe_.reset();
    if (ambe_) ambe_->reset();
    // the host resets its own talker state together with ours
    call_start_ = -1;
    talker_id_.clear();
}

void YsfReceiver::fact(const std::string& k, const std::string& v) {
    auto it = facts_.find(k);
    if (it != facts_.end() && it->second == v) return;
    facts_[k] = v;
    ctx_.report->set(Mode::YSF, k, v);   // "" removes it
}

std::string YsfReceiver::mode_name(bool brief) const {
    static const char* lng[4] = {"V/D mode 1", "Data FR (high-speed data)", "V/D mode 2 (DN)", "Voice FR (VW)"};
    static const char* shrt[4] = {"V/D mode 1", "Data FR", "DN", "VW"};
    return (brief ? shrt : lng)[call_.dt & 3];
}

// The repeater / VoIP station the transmission came through (CSD2, else MR)
std::string YsfReceiver::via() const {
    if (!call_.down.empty()) return call_.down;
    if (!call_.up.empty()) return call_.up;
    if (call_.mr == 1 || call_.mr == 2) return "repeater";
    return "";
}

// --- talkers (RxContext::talker) ----------------------------------------------
// A transmission = valid frames less than GAP apart, from the header to the
// terminator; its talker is the source callsign (CSD1 of the header /
// terminator / V/D mode 1, the V/D mode 2 DCH cycle). Reported with the frame
// boundaries, so the host can cut exactly this radio's samples out of the
// VFO's stream for the DoA.
void YsfReceiver::talker_frame() {
    if (call_start_ < 0) {
        call_start_ = frame_a_;
        voice_end_ = frame_a_;
    }
    voice_end_ = std::max(voice_end_, frame_b_);
    if (talker_id_.empty() || !ctx_.talker) return;
    const std::string v = via();
    std::string label = (call_.have_dest && !call_.dest.empty() ? call_.dest : std::string("?")) + " · " + mode_name(true);
    if (!v.empty()) label += " · via " + v;
    ctx_.talker(talker_id_, label, call_start_, voice_end_, 0);
}

void YsfReceiver::set_talker(const std::string& id) {
    if (id.empty()) return;
    if (!talker_id_.empty() && talker_id_ != id) {
        // another radio without a terminator in between: the frames since
        // the old one was last named can't be told apart - leave them out
        end_talker(lc_end_);
        call_start_ = frame_a_;
        voice_end_ = frame_a_;
    }
    talker_id_ = id;
    lc_end_ = frame_b_;
}

void YsfReceiver::end_talker(int64_t at) {
    if (!talker_id_.empty() && ctx_.talker_end) ctx_.talker_end(at, 0);
    talker_id_.clear();
    call_start_ = -1;
}

void YsfReceiver::tick(int64_t now) {
    if (call_.active && now - last_end_ > GAP) end_call("no terminator");
}

// --- calls -------------------------------------------------------------------------
void YsfReceiver::begin_call() {
    if (call_.active) end_call("no terminator");
    call_ = Call();
    call_.active = true;
    for (auto& d : data_) d.clear();
    for (const char* k : {"Source", "Destination", "Downlink", "Uplink", "Downlink ID", "Uplink ID", "VoIP relay ID",
                          "Source radio ID", "Destination radio ID", "Data (DT)"})
        fact(k, "");
    call_start_ = frame_a_;
    voice_end_ = frame_a_;
    imbe_.reset();
    if (ambe_) ambe_->reset();
}

void YsfReceiver::end_call(const char* why) {
    if (!call_.active) return;
    if (!call_.announced && !call_.dch_ok && call_.frames < 2) {
        // one lone FICH and nothing else checkable (a FICH passes on random
        // input ~3 times in a million): no call
        call_.active = false;
        end_talker(voice_end_);
        return;
    }
    announce(true);
    std::string e = (call_.dt == 1 ? "Transmission ended: " : "Call ended: ") + call_.desc;
    if (why && *why) e += std::string(" (") + why + ")";
    ctx_.report->event(Mode::YSF, e, 1.0);
    fact("Current call", "");
    if (ctx_.voice_state) ctx_.voice_state("");
    end_talker(voice_end_);
    call_.active = false;
}

// The call-start event, once the source, destination and repeater (CSD2)
// are known - at once from a header; a late entry waits for its DCH cycle
// (at most 8 frames) - or when the call ends
void YsfReceiver::announce(bool force) {
    if (call_.announced) return;
    if (!force && !(call_.have_src && ((call_.have_dest && call_.have_csd2) || call_.frames >= 8))) return;
    call_.announced = true;
    const std::string src = call_.have_src && !call_.src.empty() ? call_.src : "?";
    const std::string dst = call_.have_dest && !call_.dest.empty() ? call_.dest : "?";
    call_.desc = src + " → " + dst;
    std::string e = call_.desc + ", " + mode_name(true);
    if (call_.sc) e += ", DG-ID " + std::to_string(call_.sc);
    const std::string v = via();
    if (!v.empty()) e += ", via " + v;
    if (call_.voip) e += ", via internet";
    ctx_.report->event(Mode::YSF, std::string(call_.dt == 1 ? "Data: " : "Call: ") + e, 1.0);
    fact("Current call", e);
}

void YsfReceiver::apply_fich(const ysf::Fich& f) {
    call_.dt = f.dt;
    call_.cm = f.cm;
    call_.mr = f.mr;
    call_.voip = f.voip;
    call_.sql = f.sql;
    call_.sc = f.sc;
    static const char* cm[4] = {"Group / CQ", "Radio ID", "reserved (2)", "Individual"};
    static const char* mr[8] = {"direct", "repeater downlink (uplink not busy)", "repeater downlink (uplink busy)",
                                "reserved (3)", "reserved (4)", "reserved (5)", "reserved (6)", "reserved (7)"};
    fact("Mode", mode_name(false));
    fact("Call type", cm[f.cm]);
    // SC 0 = no value; SQL type 1 = the code is disabled (Table 4-1)
    std::string sq = f.sc == 127 ? std::string("break-out code (127)") : std::to_string(f.sc);
    if (f.sql) sq = f.sc ? sq + " (squelch code disabled)" : "off";
    fact("DG-ID / squelch code", sq);
    fact("Deviation", f.dev ? "narrow (±1350 Hz)" : "wide (±2700 Hz)");
    fact("Message route", mr[f.mr]);
    fact("VoIP", f.voip ? "via internet" : "local");
}

// One CSD field (Table 4-2): 0 Dest, 1 Src, 2 Downlink, 3 Uplink (10 bytes),
// 4..7 Rem1..Rem4 (5 bytes)
void YsfReceiver::csd(int id, const uint8_t* bits) {
    const int n = id < 4 ? 10 : 5;
    const std::string t = text(bits, n);
    switch (id) {
        case 0:
            call_.dest = t;
            call_.have_dest = true;
            if (call_.cm == 1) {
                // Radio ID mode: the field holds the destination and source radio IDs
                const std::string rid = text(bits, 5);
                call_.dest = rid.empty() ? "" : "ID " + rid;
                fact("Destination", "");
                fact("Destination radio ID", rid);
                fact("Source radio ID", text(bits + 40, 5));
            } else {
                fact("Destination", t);
            }
            break;
        case 1:
            if (call_.have_src && !t.empty() && !call_.src.empty() && t != call_.src) {
                // another station without a terminator in between: a new
                // call (this frame's FICH stays)
                const Call c = call_;
                begin_call();
                call_.dt = c.dt;
                call_.cm = c.cm;
                call_.mr = c.mr;
                call_.voip = c.voip;
                call_.sql = c.sql;
                call_.sc = c.sc;
                call_.frames = 1;
                call_.only_hc = false;
                call_.dch_ok = true;
            }
            call_.src = t;
            call_.have_src = true;
            fact("Source", t);
            break;
        case 2:
            call_.down = t;
            call_.have_csd2 = true;
            fact("Downlink", t);
            break;
        case 3:
            call_.up = t;
            call_.have_csd2 = true;
            fact("Uplink", t);
            break;
        default: {
            static const char* rk[4] = {"Downlink ID", "Uplink ID", "VoIP relay ID", "Source radio ID"};
            call_.rem[id - 4] = t;
            if (!t.empty() || id != 7 || call_.cm != 1) fact(rk[id - 4], t);
            break;
        }
    }
    if (dump_)
        fprintf(stderr, "YSF-CSD t=%.4f field=%d text=%s\n", static_cast<double>(frame_a_) / 48000.0, id,
                kp::printable(t).c_str());
}

void YsfReceiver::data_unit(int idx, const uint8_t* bits, int nbytes) {
    if (idx < 0 || idx >= 13) return;
    std::string s;
    for (int i = 0; i < nbytes; i++) s += static_cast<char>(bits_to_u32(bits + 8 * i, 8));
    data_[idx] = s;
}

// The DT units of a cycle (Tables 4-3 .. 4-5). The standard doesn't define
// their content: shown as text when printable, else as hex
void YsfReceiver::flush_data(const ysf::Fich& f) {
    std::string all;
    for (auto& d : data_) {
        all += d;
        d.clear();
    }
    while (!all.empty() && (all.back() == ' ' || all.back() == '\0')) all.pop_back();
    if (all.empty()) return;
    bool printable = true;
    for (unsigned char c : all) printable &= c >= 0x20 && c <= 0x7E;
    std::string v;
    if (printable) {
        v = "\"" + all + "\"";
    } else {
        char b[4];
        for (unsigned char c : all) {
            snprintf(b, sizeof b, "%02X", c);
            v += b;
        }
    }
    if (f.dt == 1) v += " (Data FR block " + std::to_string(f.bn + 1) + "/" + std::to_string(f.bt + 1) + ")";
    fact("Data (DT)", v);
    if (printable || ctx_.opts->verbose)
        ctx_.report->event(Mode::YSF, "Data" + std::string(call_.have_src ? " from " + call_.src : "") + ": " + v, 30.0);
}

// --- voice ---------------------------------------------------------------------
void YsfReceiver::voice_out(const std::vector<float>& pcm, const char* codec, int clean, int n) {
    if (clean == 0) return;
    if (ctx_.voice) ctx_.voice(Mode::YSF, pcm.data(), pcm.size());
    const std::string who = !call_.desc.empty() ? call_.desc : (call_.have_src ? call_.src : std::string("YSF"));
    if (ctx_.voice_state)
        ctx_.voice_state(who + ": playing (" + codec + ", " + std::to_string(clean) + "/" + std::to_string(n) + " frames clean)");
}

// Voice FR: each VCH is a 144-bit 7200x4400 full-rate frame. The standard
// names only the rate and the 88 + 56 split, which is the IMBE frame (4
// Golay(23,12) + 3 Hamming(15,11) + 7 bits); its bits are taken in the
// vocoder's own interleaved order (the one P25 carries, TIA-102.BABA)
void YsfReceiver::voice_vw(const uint8_t* bits, int nframes, int first) {
    if (dump_)
        for (int k = 0; k < nframes; k++) dump_bits("VW", frame_a_, first + k, bits + 144 * k, 144);
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    std::vector<float> pcm(static_cast<size_t>(160 * nframes));
    int clean = 0;
    for (int k = 0; k < nframes; k++) {
        const uint8_t* v = bits + 144 * k;
        char fr[8][23] = {{0}};
        for (int j = 0; j < 72; j++) {
            fr[mbe_tables::iW[j]][mbe_tables::iX[j]] = static_cast<char>(v[2 * j]);
            fr[mbe_tables::iY[j]][mbe_tables::iZ[j]] = static_cast<char>(v[2 * j + 1]);
        }
        if (imbe_.decode(fr, pcm.data() + 160 * k) >= 0) clean++;
    }
    voice_out(pcm, "IMBE full rate", clean, nframes);
}

static bool mbe_ready(const RxContext& ctx, const std::string& who) {
    if (MbeLib::instance().available()) return true;
    if (ctx.voice_state) ctx.voice_state(who + ": AMBE+2 codec not installed (README: Digital voice codecs)");
    return false;
}

// V/D mode 1: five 72-bit AMBE+2 3600x2450 frames, in the vocoder's
// interleaved order like DMR's / NXDN's
void YsfReceiver::voice_vd1(const uint8_t* bits) {
    if (dump_)
        for (int k = 0; k < 5; k++) dump_bits("VD1", frame_a_, k, bits + 72 * k, 72);
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    const std::string who = call_.desc.empty() ? std::string("YSF") : call_.desc;
    if (!mbe_ready(ctx_, who)) return;
    if (!ambe_) ambe_ = std::make_unique<AmbeStream>(false);
    std::vector<float> pcm(5 * 160);
    int clean = 0;
    for (int k = 0; k < 5; k++) {
        const uint8_t* v = bits + 72 * k;
        char fr[4][24] = {{0}};
        for (int i = 0; i < 36; i++) {
            fr[mbe_tables::rW[i]][mbe_tables::rX[i]] = static_cast<char>(v[2 * i]);
            fr[mbe_tables::rY[i]][mbe_tables::rZ[i]] = static_cast<char>(v[2 * i + 1]);
        }
        if (ambe_->decode(fr, pcm.data() + 160 * k) <= 2) clean++;
    }
    voice_out(pcm, "AMBE+2", clean, 5);
}

// V/D mode 2: the 49 parameter bits straight into mbelib (no 72-bit frame)
void YsfReceiver::voice_vd2(const float* soft) {
    const bool want = ctx_.voice_wanted && ctx_.voice_wanted();
    if (!dump_ && !want) return;
    uint8_t b[5][49];
    int dissent[5];
    for (int k = 0; k < 5; k++) {
        dissent[k] = ysf::decode_vch2(soft + 104 * k, b[k]);
        if (dump_) dump_bits("VD2", frame_a_, k, b[k], 49);
    }
    if (!want) return;
    const std::string who = call_.desc.empty() ? std::string("YSF") : call_.desc;
    if (!mbe_ready(ctx_, who)) return;
    if (!MbeLib::instance().has_ambe49()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": this mbelib lacks mbe_processAmbe2450Dataf (DN voice)");
        return;
    }
    if (!ambe_) ambe_ = std::make_unique<AmbeStream>(false);
    std::vector<float> pcm(5 * 160);
    int clean = 0;
    for (int k = 0; k < 5; k++) {
        char d[49];
        for (int i = 0; i < 49; i++) d[i] = static_cast<char>(b[k][i]);
        ambe_->decode49(d, dissent[k], pcm.data() + 160 * k);
        if (dissent[k] <= 3) clean++;
    }
    voice_out(pcm, "AMBE+2 DN", clean, 5);
}

// --- frames ----------------------------------------------------------------------
bool YsfReceiver::dch(const float* soft, int ncoded, uint8_t* d, int slot) {
    const bool ok = ysf::decode_dch(soft, ncoded, d);
    call_.dch_ok |= ok;
    if (dump_) {
        std::string h;
        char b[4];
        for (int i = 0; ok && i < (ncoded - 40) / 16; i++) {
            snprintf(b, sizeof b, "%02X", bits_to_u32(d + 8 * i, 8));
            h += b;
        }
        fprintf(stderr, "YSF-DCH t=%.4f slot=%d ok=%d hex=%s\n", static_cast<double>(frame_a_) / 48000.0, slot, ok ? 1 : 0,
                h.c_str());
    }
    return ok;
}

bool YsfReceiver::decode_frame(const SymSrc& s) {
    // frame symbol j (0 = first FS symbol) is k = j - 19
    float soft[960];
    uint8_t bits[960];
    for (int j = 0; j < FRAME_SYMS; j++) {
        const float v = s.sym(j - (FS_SYMS - 1));
        // soft bits (> 0 = 0): first bit = sign, second bit = outer level
        soft[2 * j] = v;
        soft[2 * j + 1] = 2.0f - std::fabs(v);
    }
    for (int i = 0; i < 960; i++) bits[i] = soft[i] < 0;
    ysf::Fich f;
    if (!ysf::decode_fich(soft + 40, f)) return false;

    last_sync_ = s.sync_end;
    expect_ = s.sync_end + FRAME;
    misses_ = 0;
    trk_scale_ = s.scale;
    trk_center_ = s.center;
    // this frame's samples (frame symbol 0 is centred FS_SYMS - 1 symbols
    // before the sync end), back to the input's sample count
    frame_a_ = s.sync_end - static_cast<int64_t>((FS_SYMS - 1) * SPS + SPS / 2) - DELAY;
    frame_b_ = frame_a_ + FRAME;
    if (dump_)
        fprintf(stderr, "YSF-FRAME t=%.4f fich=%08X fi=%d cs=%d cm=%d bn=%d bt=%d fn=%d ft=%d dev=%d mr=%d voip=%d dt=%d sql=%d sc=%d\n",
                static_cast<double>(frame_a_) / 48000.0, f.raw, f.fi, f.cs, f.cm, f.bn, f.bt, f.fn, f.ft, f.dev, f.mr, f.voip,
                f.dt, f.sql, f.sc);
    report_valid(ctx_, Mode::YSF, frame_a_, frame_b_);
    if (ctx_.freq_error) ctx_.freq_error(Mode::YSF, s.center);

    if (f.fi == 3) {   // test channel: no payload defined
        ctx_.report->event(Mode::YSF, "Test channel frame", 10.0);
        return true;
    }
    // a repeated terminator right after the one that ended the call
    if (f.fi == 2 && !call_.active && frame_a_ - last_end_ <= FRAME + 2 * SPS) {
        last_end_ = frame_b_;
        return true;
    }
    // a new transmission: a header after other frames (N0 headers in a row
    // belong together, 4.6 (1)), or the first frame after a gap
    if (!call_.active || frame_a_ - last_end_ > GAP || (f.fi == 0 && call_.frames > 0 && !call_.only_hc)) begin_call();
    call_.frames++;
    call_.only_hc = call_.only_hc && f.fi == 0;
    last_end_ = frame_b_;
    apply_fich(f);

    const float* p = soft + PAYLOAD;
    uint8_t d[160];
    if (f.fi != 1 || f.dt == 1) {
        // header / terminator (any data type), Data FR: DCH-1(k) and DCH-2(k)
        // alternate in 72-bit pieces
        float u[2][360];
        for (int k = 0; k < 5; k++)
            for (int b = 0; b < 72; b++) {
                u[0][72 * k + b] = p[144 * k + b];
                u[1][72 * k + b] = p[144 * k + 72 + b];
            }
        for (int slot = 0; slot < 2; slot++) {
            if (!dch(u[slot], 360, d, slot)) continue;
            // HC / TC: CSD1, CSD2 (4.2.1 (1)); Data FR (Table 4-5): unit
            // 2 FN + slot = CSD1, CSD2, CSD3, DT1 ..
            const int unit = f.fi != 1 ? slot : 2 * f.fn + slot;
            if (unit == 0) { csd(0, d); csd(1, d + 80); }
            else if (unit == 1) { csd(2, d); csd(3, d + 80); }
            else if (unit == 2) { for (int r = 0; r < 4; r++) csd(4 + r, d + 40 * r); }
            else data_unit(unit - 3, d, 20);
        }
    } else if (f.dt == 0) {
        // V/D mode 1 (Figure 4-8): DCH(k) VCH(k), 72 + 72
        float u[360];
        uint8_t v[360];
        for (int k = 0; k < 5; k++)
            for (int b = 0; b < 72; b++) {
                u[72 * k + b] = p[144 * k + b];
                v[72 * k + b] = bits[PAYLOAD + 144 * k + 72 + b];
            }
        // Table 4-3: FN 0 CSD1, 1 CSD2, 2 CSD3, 3.. DT1..
        if (dch(u, 360, d, 0)) {
            if (f.fn == 0) { csd(0, d); csd(1, d + 80); }
            else if (f.fn == 1) { csd(2, d); csd(3, d + 80); }
            else if (f.fn == 2) { for (int r = 0; r < 4; r++) csd(4 + r, d + 40 * r); }
            else data_unit(f.fn - 3, d, 20);
        }
        voice_vd1(v);
    } else if (f.dt == 2) {
        // V/D mode 2 (Figure 4-12): DCH(k) 40, VCH(k) 72, VeCH(k) 32
        float u[200], vs[5 * 104];
        for (int k = 0; k < 5; k++) {
            for (int b = 0; b < 40; b++) u[40 * k + b] = p[144 * k + b];
            for (int b = 0; b < 104; b++) vs[104 * k + b] = p[144 * k + 40 + b];
        }
        // Table 4-4: FN 0 Dest, 1 Src, 2 Down, 3 Up, 4 Rem1+2, 5 Rem3+4, 6 DT1, 7 DT2
        if (dch(u, 200, d, 0)) {
            if (f.fn <= 3) csd(f.fn, d);
            else if (f.fn == 4) { csd(4, d); csd(5, d + 40); }
            else if (f.fn == 5) { csd(6, d); csd(7, d + 40); }
            else data_unit(f.fn - 6, d, 10);
        }
        voice_vd2(vs);
    } else if (f.ft == 1 && f.fn == 0) {
        // Voice FR sub header (Figure 4-21): DCH(0..4) = CSD3, 72 reserved,
        // VCH(3), VCH(4)
        if (dch(p, 360, d, 0))
            for (int r = 0; r < 4; r++) csd(4 + r, d + 40 * r);
        voice_vw(bits + PAYLOAD + 432, 2, 3);
    } else {
        // Voice FR (Figure 4-22): 5 x VCH 144
        voice_vw(bits + PAYLOAD, 5, 0);
    }

    if (call_.have_src) set_talker(call_.src);
    talker_frame();
    announce(false);
    if (f.fi == 1 && f.fn == f.ft) flush_data(f);
    if (f.fi == 2) {
        flush_data(f);
        end_call("");
    }
    return true;
}

// Level fit on the FS symbols (they include the +-1 and +-3 levels)
bool YsfReceiver::fit(int64_t idx, float* scale, float* center) const {
    const int64_t f0 = idx - static_cast<int64_t>((FS_SYMS - 1) * SPS);
    if (f0 < buf_.begin() || idx >= buf_.end()) return false;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int k = 0; k < FS_SYMS; k++) {
        const float y = buf_.at(f0 + k * SPS), x = FS.v[k];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const float den = FS_SYMS * sxx - sx * sx;
    if (std::fabs(den) < 1e-6f) return false;
    *scale = (FS_SYMS * sxy - sx * sy) / den;
    *center = (sy - *scale * sx) / FS_SYMS;
    return true;
}

void YsfReceiver::add_job(int64_t idx) {
    for (const auto& j : jobs_)
        if (std::llabs(j.src.sync_end - idx) <= WIN + 2) return;
    if (last_sync_ >= 0 && std::llabs(idx - last_sync_) <= WIN + 2) return;
    float scale = 0, center = 0;
    // narrow deviation: 450 Hz per level unit
    if (!fit(idx, &scale, &center) || std::fabs(scale) < 100.0f) return;
    SymSrc src;
    src.buf = &buf_;
    src.sync_end = idx;
    src.sps = SPS;
    src.scale = scale;
    src.center = center;
    jobs_.push_back({src, false});
}

// The next frame on the 100 ms grid, however weak its sync: the strongest
// correlation of the expected polarity within +-WIN samples; the level of
// the last good frame when the FS's own fit is off
void YsfReceiver::tracked_job() {
    if (trk_scale_ == 0) return;
    for (const auto& j : jobs_)
        if (std::llabs(j.src.sync_end - expect_) <= WIN + 2) return;
    const float pol = trk_scale_ < 0 ? -1.0f : 1.0f;
    int64_t best = expect_;
    float bc = -2.0f;
    for (int64_t k = expect_ - WIN; k <= expect_ + WIN; k++) {
        const float c = pol * cring_[k & 31];
        if (c > bc) {
            bc = c;
            best = k;
        }
    }
    float scale = 0, center = 0;
    if (!fit(best, &scale, &center) || scale / trk_scale_ < 0.6f || scale / trk_scale_ > 1.6f) {
        scale = trk_scale_;
        center = trk_center_;
    }
    SymSrc src;
    src.buf = &buf_;
    src.sync_end = best;
    src.sps = SPS;
    src.scale = scale;
    src.center = center;
    jobs_.push_back({src, true});
}

void YsfReceiver::process(const float* d, size_t n) {
    const size_t nt = taps_.size();
    for (size_t i = 0; i < n; i++) {
        hist_[pos_] = std::clamp(d[i], -6000.0f, 6000.0f);
        float acc = 0;
        size_t p = pos_;
        for (size_t k = 0; k < nt; k++) {
            acc += taps_[k] * hist_[p];
            p = p == 0 ? nt - 1 : p - 1;
        }
        pos_ = (pos_ + 1) % nt;
        buf_.push(acc);
        // FS correlation at symbol spacing
        const int64_t last = buf_.end() - 1;
        const int64_t first = last - static_cast<int64_t>(FS_SYMS - 1) * SPS;
        if (first < buf_.begin()) continue;
        float w[FS_SYMS], m = 0;
        for (int k = 0; k < FS_SYMS; k++) {
            w[k] = buf_.at(first + k * SPS);
            m += w[k];
        }
        m /= FS_SYMS;
        float e = 0, c = 0;
        for (int k = 0; k < FS_SYMS; k++) {
            const float v = w[k] - m;
            e += v * v;
            c += fs_t_[static_cast<size_t>(k)] * v;
        }
        c = e > 1e-3f ? c / std::sqrt(e) : 0.0f;
        cring_[last & 31] = c;
        if (pk_.active) {
            pk_.age++;
            if (std::fabs(c) > std::fabs(pk_.best)) {
                pk_.best = c;
                pk_.idx = last;
            }
            if (pk_.age >= SPS) {
                pk_.active = false;
                add_job(pk_.idx);
            }
        } else if (std::fabs(c) >= THRESHOLD) {
            pk_.active = true;
            pk_.best = c;
            pk_.idx = last;
            pk_.age = 0;
        }
        if (expect_ >= 0 && last == expect_ + WIN + SPS + 1) tracked_job();
        // frames whose samples are all in
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (!it->src.has(FRAME_SYMS - FS_SYMS)) {
                ++it;
                continue;
            }
            const Job j = *it;
            it = jobs_.erase(it);
            if (last_sync_ >= 0 && std::llabs(j.src.sync_end - last_sync_) <= WIN + 2) continue;   // decoded already
            const bool near = expect_ >= 0 && std::llabs(j.src.sync_end - expect_) <= WIN + 2;
            bool ok = decode_frame(j.src);
            if (!ok && near && !j.tracked && trk_scale_ != 0) {
                SymSrc t = j.src;
                t.scale = trk_scale_;
                t.center = trk_center_;
                ok = decode_frame(t);
            }
            if (!ok && near) {
                if (++misses_ >= N2) expect_ = -1;
                else expect_ += FRAME;
            }
            if (dump_)
                fprintf(stderr, "YSF-TRY t=%.4f tracked=%d ok=%d\n", static_cast<double>(j.src.sync_end) / 48000.0,
                        j.tracked ? 1 : 0, ok ? 1 : 0);
        }
    }
    tick(buf_.end() - DELAY);
    int64_t keep = buf_.end() - 2 * FRAME;
    for (const auto& j : jobs_) keep = std::min(keep, j.src.sync_end - 25 * SPS);
    if (keep > buf_.begin() + 8192) buf_.trim_before(keep);
}

}  // namespace dig
