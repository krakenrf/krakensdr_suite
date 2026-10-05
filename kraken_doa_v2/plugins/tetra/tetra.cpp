// TETRA downlink (EN 300 392-2): pi/4-DQPSK at 18 ksym/s, continuous
// base-station carrier of 510-bit timeslots. Differential detection at
// 4 samples/symbol; the slot boundary is found from the training sequences
// (synchronization "y" or normal "n"/"p") and then tracked slot by slot.
//
// Decoded: BSCH (MCC, MNC, colour code, TDMA time), BNCH SYSINFO (carrier,
// location area, services), AACH (timeslot usage) and the MAC-RESOURCE
// headers of the control channel with the addresses (SSI) and, for clear
// signalling, the MM / CMCE PDU types (call setup, SDS...). Speech (ACELP)
// is not decoded; air-interface-encrypted signalling shows only its address.

#include "tetra.hpp"
#include "dig_fec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dig {

namespace {

constexpr int SPS = 4;
constexpr int SLOT_SYMS = 255;
constexpr int SLOT_SAMPLES = SLOT_SYMS * SPS;   // 1020 at 72 kHz

const uint8_t Y_BITS[38] = {1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1,
                            0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1};
const uint8_t N_BITS[22] = {1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0};
const uint8_t P_BITS[22] = {0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0};
constexpr int Y_POS = 214;    // SB: q(12) ph(2) f(80) sb1(120)
constexpr int NP_POS = 244;   // NDB: q(12) ph(2) bkn1(216) bb1(14)

int hamming(const uint8_t* a, const uint8_t* b, int n) {
    int d = 0;
    for (int i = 0; i < n; i++) d += (a[i] ^ b[i]) & 1;
    return d;
}

const char* cmce_name(int t) {
    static const char* n[17] = {"D-ALERT", "D-CALL-PROCEEDING", "D-CONNECT", "D-CONNECT-ACK", "D-DISCONNECT",
                                "D-INFO", "D-RELEASE", "D-SETUP", "D-STATUS", "D-TX-CEASED", "D-TX-CONTINUE",
                                "D-TX-GRANTED", "D-TX-WAIT", "D-TX-INTERRUPT", "D-CALL-RESTORE", "D-SDS-DATA",
                                "D-FACILITY"};
    return t < 17 ? n[t] : "CMCE";
}

const char* mm_name(int t) {
    switch (t) {
        case 0: return "D-OTAR";
        case 1: return "D-AUTHENTICATION";
        case 2: return "D-CK-CHANGE-DEMAND";
        case 3: return "D-DISABLE";
        case 4: return "D-ENABLE";
        case 5: return "D-LOCATION-UPDATE-ACCEPT";
        case 6: return "D-LOCATION-UPDATE-COMMAND";
        case 7: return "D-LOCATION-UPDATE-REJECT";
        case 9: return "D-LOCATION-UPDATE-PROCEEDING";
        case 10: return "D-ATTACH-DETACH-GROUP-IDENTITY";
        case 11: return "D-ATTACH-DETACH-GROUP-IDENTITY-ACK";
        case 12: return "D-MM-STATUS";
        default: return "MM";
    }
}

}  // namespace

TetraReceiver::TetraReceiver(RxContext& c) : ctx_(c) {}

void TetraReceiver::reset() {
    hist_.clear();
    hist_base_ = 0;
    conj_ = false;
    slot_start_ = -1;
    locked_slots_ = 0;
    miss_ = 0;
    dphi_ = 0;
    have_cell_ = false;
    last_sysinfo_.clear();
    for (int& u : slot_usage_) u = -1;
    voice_tn_ = 0;
    if (codec_) codec_->stop();
}

// Differential detection of one slot whose symbol 0 sits at sample `start`.
// bits/soft: 510 entries, soft > 0 means bit 0. Returns false if the samples
// aren't buffered.
bool TetraReceiver::demod_slot(int64_t start, int /*phase*/, float dphi, uint8_t* bits, float* soft) {
    if (start - SPS < hist_base_ || start + SLOT_SAMPLES > hist_base_ + static_cast<int64_t>(hist_.size())) return false;
    const std::complex<float> rot = std::polar(1.0f, -dphi);
    float mag = 0;
    std::complex<float> z[SLOT_SYMS];
    for (int k = 0; k < SLOT_SYMS; k++) {
        int64_t i = start + static_cast<int64_t>(k) * SPS - hist_base_;
        z[k] = hist_[static_cast<size_t>(i)] * std::conj(hist_[static_cast<size_t>(i - SPS)]) * rot;
        if (conj_) z[k] = std::conj(z[k]);
        mag += std::abs(z[k]);
    }
    mag = mag > 0 ? mag / SLOT_SYMS : 1.0f;
    for (int k = 0; k < SLOT_SYMS; k++) {
        float im = z[k].imag() / mag, re = z[k].real() / mag;
        bits[2 * k] = im < 0;
        bits[2 * k + 1] = re < 0;
        soft[2 * k] = im;
        soft[2 * k + 1] = re;
    }
    return true;
}

// Frequency offset (rad/symbol) and timing quality of a sampling phase over a
// span of symbols: pi/4-DQPSK differentials raised to the 4th power all point
// to -1 (rotated by 4x the offset).
static void phase_metric(const std::vector<std::complex<float>>& h, size_t from, int nsym, int p,
                         float* dphi, float* quality) {
    std::complex<float> acc(0, 0);
    float norm = 0;
    for (int k = 1; k < nsym; k++) {
        size_t i = from + static_cast<size_t>(p) + static_cast<size_t>(k) * SPS;
        if (i >= h.size()) break;
        std::complex<float> z = h[i] * std::conj(h[i - SPS]);
        float a = std::norm(z);
        if (a <= 0) continue;
        z /= std::sqrt(a);   // unit phasors: amplitude-independent
        std::complex<float> z2 = z * z;
        acc += -(z2 * z2);
        norm += 1.0f;
    }
    *dphi = std::arg(acc) / 4.0f;
    *quality = norm > 0 ? std::abs(acc) / norm : 0;
}

void TetraReceiver::search() {
    // need two slots + margin in the buffer
    const int span_syms = 2 * SLOT_SYMS + 40;
    if (static_cast<int64_t>(hist_.size()) < static_cast<int64_t>(span_syms + 2) * SPS) return;
    int best_p = 0;
    float best_q = 0, best_d = 0;
    for (int p = 0; p < SPS; p++) {
        float d, q;
        phase_metric(hist_, SPS, span_syms, p, &d, &q);
        if (q > best_q) { best_q = q; best_p = p; best_d = d; }
    }
    if (best_q > 0.25f) {
        // bits over the span at that phase, both spectral orientations
        for (int cj = 0; cj < 2; cj++) {
            conj_ = cj == 1;
            std::vector<uint8_t> bits(static_cast<size_t>(span_syms) * 2);
            std::complex<float> rot = std::polar(1.0f, -best_d);
            for (int k = 0; k < span_syms; k++) {
                size_t i = static_cast<size_t>(SPS + best_p + k * SPS);
                std::complex<float> z = hist_[i] * std::conj(hist_[i - SPS]) * rot;
                if (conj_) z = std::conj(z);
                bits[2 * k] = z.imag() < 0;
                bits[2 * k + 1] = z.real() < 0;
            }
            const int nb = static_cast<int>(bits.size());
            for (int b = 0; b + 510 + 40 < nb; b += 2) {
                int slot_bit = -1;
                if (b + 38 <= nb && hamming(bits.data() + b, Y_BITS, 38) <= 3) slot_bit = b - Y_POS;
                else if (hamming(bits.data() + b, N_BITS, 22) <= 1 || hamming(bits.data() + b, P_BITS, 22) <= 1)
                    slot_bit = b - NP_POS;
                if (slot_bit < 0) continue;
                // confirm with the next slot's training sequence
                int nxt = slot_bit + 510;
                bool conf = false;
                if (nxt + Y_POS + 38 <= nb && hamming(bits.data() + nxt + Y_POS, Y_BITS, 38) <= 4) conf = true;
                if (nxt + NP_POS + 22 <= nb && (hamming(bits.data() + nxt + NP_POS, N_BITS, 22) <= 2 ||
                                                 hamming(bits.data() + nxt + NP_POS, P_BITS, 22) <= 2)) conf = true;
                if (!conf) continue;
                slot_start_ = hist_base_ + SPS + best_p + static_cast<int64_t>(slot_bit / 2) * SPS;
                dphi_ = best_d;
                locked_slots_ = 0;
                miss_ = 0;
                ctx_.report->set(Mode::TETRA, "Carrier", "pi/4-DQPSK 18 ksym/s, slot sync acquired");
                return;
            }
        }
        conj_ = false;
    }
    // nothing: drop one slot of history and wait for more
    size_t drop = SLOT_SAMPLES;
    hist_.erase(hist_.begin(), hist_.begin() + static_cast<long>(drop));
    hist_base_ += static_cast<int64_t>(drop);
}

bool TetraReceiver::decode_block(const float* soft5, int n, int k1, int a, uint32_t scr, std::vector<uint8_t>& type1) {
    std::vector<uint8_t> s(n, 0);
    tetra_scramble(s.data(), n, scr);
    std::vector<float> t4(n), t3(n);
    for (int i = 0; i < n; i++) t4[i] = s[i] ? -soft5[i] : soft5[i];
    for (int i = 1; i <= n; i++) t3[i - 1] = t4[(a * i) % n];   // k = 1 + (a*i) mod K
    Bits t2 = tetra_rcpc23_decode(t3.data(), n, k1 + 16 + 4);
    if (!tetra_crc_ok(t2.data(), k1)) return false;
    type1.assign(t2.begin(), t2.begin() + k1);
    return true;
}

void TetraReceiver::bsch(const uint8_t* t) {
    uint32_t cc = bits_to_u32(t + 4, 6);
    int tn = static_cast<int>(bits_to_u32(t + 10, 2)) + 1;
    int fn = static_cast<int>(bits_to_u32(t + 12, 5));
    int mn = static_cast<int>(bits_to_u32(t + 17, 6));
    uint32_t mcc = bits_to_u32(t + 31, 10), mnc = bits_to_u32(t + 41, 14);
    tn_ = tn;
    fn_ = fn;
    mn_ = mn;
    Report& r = *ctx_.report;
    if (!have_cell_ || mcc != mcc_ || mnc != mnc_ || cc != cc_) {
        have_cell_ = true;
        mcc_ = mcc;
        mnc_ = mnc;
        cc_ = cc;
        scramb_ = tetra_scramb_init(mcc, mnc, cc);
        r.event(Mode::TETRA, "Cell: MCC " + std::to_string(mcc) + " MNC " + std::to_string(mnc) + " colour code " +
                                 std::to_string(cc), 60.0);
    }
    r.set(Mode::TETRA, "Network (MCC / MNC)", std::to_string(mcc) + " / " + std::to_string(mnc));
    r.set(Mode::TETRA, "Colour code", std::to_string(cc));
    static const char* lvl[4] = {"unknown", "low", "medium", "high"};
    r.set(Mode::TETRA, "Cell service level", lvl[bits_to_u32(t + 57, 2)]);
    if (ctx_.valid) ctx_.valid(Mode::TETRA);
}

void TetraReceiver::sysinfo(const uint8_t* t) {
    const uint8_t* c = t + 4;   // after PDU type (2) + broadcast type (2)
    uint32_t carrier = bits_to_u32(c, 12); c += 12;
    uint32_t band = bits_to_u32(c, 4); c += 4;
    uint32_t off = bits_to_u32(c, 2); c += 2;
    uint32_t duplex = bits_to_u32(c, 3); c += 3;
    c += 1;   // reverse operation
    uint32_t nsec = bits_to_u32(c, 2); c += 2;
    uint32_t txpwr = bits_to_u32(c, 3); c += 3;
    (void)txpwr;
    c += 4 + 4 + 4;   // rxlev access min, access parameter, radio downlink timeout
    bool cck = *c++;
    c += 16;          // hyperframe / CCK id
    (void)cck;
    // D-MLE-SYSINFO in the last 42 bits of the 124
    const uint8_t* m = t + 124 - 42;
    uint32_t la = bits_to_u32(m, 14), svc = bits_to_u32(m + 30, 12);
    static const double offs[4] = {0, 6250, -6250, 12500};
    double fdl = band * 100e6 + carrier * 25e3 + offs[off];
    char b[64];
    snprintf(b, sizeof b, "%.4f MHz (carrier %u)", fdl / 1e6, carrier);
    Report& r = *ctx_.report;
    r.set(Mode::TETRA, "Main carrier", b);
    r.set(Mode::TETRA, "Location area", std::to_string(la));
    r.set(Mode::TETRA, "Secondary control channels", std::to_string(nsec));
    std::string s;
    static const struct { int bit; const char* name; } sv[] = {
        {11, "registration"}, {9, "priority cell"}, {6, "system-wide services"}, {5, "voice"},
        {4, "circuit data"}, {2, "packet data (SNDCP)"}, {1, "air encryption"}, {0, "advanced link"}};
    for (const auto& x : sv)
        if (svc & (1u << x.bit)) s += std::string(s.empty() ? "" : ", ") + x.name;
    r.set(Mode::TETRA, "Services", s);
    r.set(Mode::TETRA, "Air encryption", (svc & 2) ? "supported/enabled by the cell" : "not offered");
    std::string sig = std::string(b) + " LA " + std::to_string(la);
    if (sig != last_sysinfo_) {
        last_sysinfo_ = sig;
        r.event(Mode::TETRA, "SYSINFO: main carrier " + std::string(b) + ", LA " + std::to_string(la) +
                                 ", duplex spacing code " + std::to_string(duplex), 60.0);
    }
}

void TetraReceiver::aach(const uint8_t* b30) {
    uint32_t m;
    if (rm_30_14().decode(bits_to_u32(b30, 30), &m, 3) < 0) return;
    int hdr = static_cast<int>(m >> 12), f1 = static_cast<int>((m >> 6) & 63);
    if (fn_ == 18) return;   // frame 18 is always control
    std::string use;
    if (hdr == 0) use = "common control";
    else if (f1 == 0) use = "unallocated";
    else if (f1 == 1) use = "assigned control";
    else if (f1 == 2) use = "common control (MCCH)";
    else if (f1 == 3) use = "reserved";
    else use = "traffic (usage marker " + std::to_string(f1) + ")";
    if (tn_ >= 1 && tn_ <= 4) {
        ctx_.report->set(Mode::TETRA, "Timeslot " + std::to_string(tn_), use);
        slot_usage_[tn_] = hdr == 0 ? 2 : f1;
    }
}

void TetraReceiver::mac_pdus(const uint8_t* t, int k1, const char* chan) {
    Report& r = *ctx_.report;
    int off = 0;
    while (off + 2 <= k1 - 16) {
        const uint8_t* p = t + off;
        int type = static_cast<int>(bits_to_u32(p, 2));
        if (type == 2) {   // broadcast
            if (bits_to_u32(p + 2, 2) == 0 && k1 >= 124) sysinfo(p);
            return;
        }
        if (type != 0) return;   // MAC-FRAG/END, MAC-U-SIGNAL: not reassembled
        // MAC-RESOURCE
        const uint8_t* c = p + 2;
        c += 1;   // fill bit indication
        c += 1;   // position of grant
        int enc = static_cast<int>(bits_to_u32(c, 2)); c += 2;
        c += 1;   // random access flag
        int li = static_cast<int>(bits_to_u32(c, 6)); c += 6;
        int at = static_cast<int>(bits_to_u32(c, 3)); c += 3;
        if (at == 0) return;   // null PDU: rest of the block is fill
        static const int alen[8] = {0, 24, 10, 24, 24, 34, 30, 34};
        uint32_t ssi = 0;
        if (at != 2) ssi = bits_to_u32(c, 24);
        c += alen[at];
        if (static_cast<int>(c - t) + 3 > k1) return;
        if (*c++) c += 4;           // power control
        if (*c++) c += 8;           // slot granting
        bool challoc = *c++;
        std::string who = at == 2 ? "event label" : "SSI " + std::to_string(ssi);
        if (enc) {
            encrypted_ms_ = now_ms();
            r.set(Mode::TETRA, "Air encryption", "in use (encryption class " + std::to_string(enc) + ")");
            r.event(Mode::TETRA, std::string(chan) + " " + who + ": encrypted signalling" +
                                     (challoc ? " + channel allocation" : ""), ctx_.opts->verbose ? 2.0 : 10.0);
        } else if (!challoc && li != 0x3F && li != 0x3E) {
            // TM-SDU: LLC -> MLE -> MM / CMCE
            const uint8_t* s = c;
            int llc = static_cast<int>(bits_to_u32(s, 4)); s += 4;
            static const int llc_hdr[8] = {2, 1, 0, 1, 2, 1, 0, 1};
            std::string what;
            if (llc < 8) {
                s += llc_hdr[llc];
                int pd = static_cast<int>(bits_to_u32(s, 3)); s += 3;
                if (pd == 2) {
                    int ct = static_cast<int>(bits_to_u32(s, 5)); s += 5;
                    what = cmce_name(ct);
                    if (ct != 8 && ct != 15 && ct != 16)
                        what += " call " + std::to_string(bits_to_u32(s, 14));
                    if (ct == 7 || ct == 2 || ct == 11) {
                        r.set(Mode::TETRA, "Last call", who + ": " + what);
                        last_call_ = who;
                    }
                } else if (pd == 1) {
                    what = mm_name(static_cast<int>(bits_to_u32(s, 4)));
                } else if (pd == 4) {
                    what = "SNDCP (packet data)";
                } else if (pd == 5) {
                    what = "MLE";
                }
            }
            if (!what.empty())
                r.event(Mode::TETRA, std::string(chan) + " " + who + ": " + what, ctx_.opts->verbose ? 2.0 : 5.0);
            if (at != 2) r.set(Mode::TETRA, "Last address", who);
        } else if (at != 2) {
            r.set(Mode::TETRA, "Last address", who);
            if (ctx_.opts->verbose) r.event(Mode::TETRA, std::string(chan) + " " + who + (challoc ? ": channel allocation" : ""), 2.0);
        }
        if (li == 0 || li >= 0x3B) return;   // fragment / stolen / reserved: stop
        off += li * 8;
    }
}

void TetraReceiver::decode_slot(const uint8_t* bits, const float* soft) {
    int dy = hamming(bits + Y_POS, Y_BITS, 38);
    int dn = hamming(bits + NP_POS, N_BITS, 22);
    int dp = hamming(bits + NP_POS, P_BITS, 22);
    std::vector<uint8_t> t1;
    if (dy <= 6 && dy * 22 <= std::min(dn, dp) * 38) {
        // synchronization burst: BSCH + AACH + block 2
        if (decode_block(soft + 94, 120, 60, 11, 3, t1)) bsch(t1.data());
        if (have_cell_) {
            uint8_t bb[30];
            std::copy(bits + 252, bits + 282, bb);
            tetra_scramble(bb, 30, scramb_);
            aach(bb);
            if (decode_block(soft + 282, 216, 124, 101, scramb_, t1)) {
                if (ctx_.valid) ctx_.valid(Mode::TETRA);
                mac_pdus(t1.data(), 124, "BNCH/SCH-HD");
            }
        }
        return;
    }
    if (!have_cell_) return;
    uint8_t bb[30];
    std::copy(bits + 230, bits + 244, bb);
    std::copy(bits + 266, bits + 282, bb + 14);
    tetra_scramble(bb, 30, scramb_);
    aach(bb);
    if (dn <= dp && fn_ != 18 && tn_ >= 1 && tn_ <= 4 && slot_usage_[tn_] >= 4) {
        voice_slot(bits);   // full-slot traffic (TCH/S)
        return;
    }
    if (dn <= dp) {
        // full slot SCH/F
        std::vector<float> blk(432);
        std::copy(soft + 14, soft + 230, blk.begin());
        std::copy(soft + 282, soft + 498, blk.begin() + 216);
        if (decode_block(blk.data(), 432, 268, 103, scramb_, t1)) {
            if (ctx_.valid) ctx_.valid(Mode::TETRA);
            mac_pdus(t1.data(), 268, "SCH/F");
        }
    } else {
        // two half slots (SCH/HD, BNCH, STCH)
        for (int h = 0; h < 2; h++) {
            if (decode_block(soft + (h ? 282 : 14), 216, 124, 101, scramb_, t1)) {
                if (ctx_.valid) ctx_.valid(Mode::TETRA);
                mac_pdus(t1.data(), 124, "SCH/HD");
            }
        }
    }
}

// One full-slot TCH/S burst. The timeslot played is the first one carrying
// traffic, held while it keeps doing so (2 s).
void TetraReceiver::voice_slot(const uint8_t* bits) {
    int64_t t = now_ms();
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) { voice_idle(); return; }
    if (voice_tn_ != tn_ && voice_tn_ != 0 && t - voice_tn_ms_ < 2000) return;
    voice_tn_ = tn_;
    voice_tn_ms_ = t;
    std::string who = "Timeslot " + std::to_string(tn_) + (last_call_.empty() ? "" : " (" + last_call_ + ")");
    if (!TetraCodec::available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": ACELP codec not installed (README: Digital voice codecs)");
        return;
    }
    if (t - encrypted_ms_ < 60000) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": air-interface encryption in use, muted");
        return;
    }
    uint8_t t4[432];
    std::copy(bits + 14, bits + 230, t4);
    std::copy(bits + 282, bits + 498, t4 + 216);
    tetra_scramble(t4, 432, scramb_);
    if (!codec_) codec_ = std::make_unique<TetraCodec>();
    std::vector<float> pcm;
    codec_->feed_slot(t4, pcm);
    last_tch_ms_ = t;
    codec_flushed_ = false;
    if (!pcm.empty() && ctx_.voice) ctx_.voice(Mode::TETRA, pcm.data(), pcm.size());
    if (ctx_.voice_state) ctx_.voice_state(who + ": playing (ACELP)");
}

// No traffic for a while: push silence through the codec pipes so the end
// of the call isn't left in their buffers, then stop the codec processes
void TetraReceiver::voice_idle() {
    int64_t t = now_ms();
    if (!codec_ || codec_flushed_ || t - last_tch_ms_ < 400) return;
    std::vector<float> pcm, all;
    for (int i = 0; i < 10; i++) {
        codec_->feed_slot(nullptr, pcm);   // erasures: the codec mutes them
        all.insert(all.end(), pcm.begin(), pcm.end());
    }
    if (!all.empty() && ctx_.voice) ctx_.voice(Mode::TETRA, all.data(), all.size());
    codec_flushed_ = true;
    if (ctx_.voice_state) ctx_.voice_state("");
}

void TetraReceiver::process(const std::complex<float>* x, size_t n) {
    hist_.insert(hist_.end(), x, x + n);
    if (slot_start_ < 0) {
        search();
        return;
    }
    uint8_t bits[510];
    float soft[510];
    while (slot_start_ + SLOT_SAMPLES + 2 * SPS < hist_base_ + static_cast<int64_t>(hist_.size())) {
        // timing: best of -1/0/+1 sample around the expected start
        int best_o = 0, best_d = 99;
        for (int o = -1; o <= 1; o++) {
            if (!demod_slot(slot_start_ + o, 0, dphi_, bits, soft)) continue;
            int d = std::min({hamming(bits + Y_POS, Y_BITS, 38) * 22 / 38, hamming(bits + NP_POS, N_BITS, 22),
                              hamming(bits + NP_POS, P_BITS, 22)});
            if (d < best_d) { best_d = d; best_o = o; }
        }
        slot_start_ += best_o;
        if (best_d <= 4 && demod_slot(slot_start_, 0, dphi_, bits, soft)) {
            miss_ = 0;
            locked_slots_++;
            // frequency tracking from this slot's differentials
            float d, q;
            size_t from = static_cast<size_t>(slot_start_ - hist_base_ - SPS);
            phase_metric(hist_, from, SLOT_SYMS, 0, &d, &q);
            dphi_ = 0.8f * dphi_ + 0.2f * d;
            if (ctx_.freq_error) ctx_.freq_error(Mode::TETRA, d * 18000.0f / (2.0f * static_cast<float>(M_PI)));
            decode_slot(bits, soft);
            voice_idle();
        } else if (++miss_ > 8) {
            slot_start_ = -1;
            ctx_.report->set(Mode::TETRA, "Carrier", "slot sync lost");
            return;
        }
        // TDMA time
        if (tn_ > 0 && ++tn_ > 4) {
            tn_ = 1;
            if (++fn_ > 18) { fn_ = 1; if (++mn_ > 60) mn_ = 1; }
        }
        slot_start_ += SLOT_SAMPLES;
    }
    // drop consumed history
    int64_t keep = slot_start_ - 4 * SPS;
    if (keep > hist_base_ + 8192) {
        hist_.erase(hist_.begin(), hist_.begin() + static_cast<long>(keep - hist_base_));
        hist_base_ = keep;
    }
}

}  // namespace dig
