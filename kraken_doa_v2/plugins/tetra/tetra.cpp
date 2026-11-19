// TETRA downlink (EN 300 392-2): pi/4-DQPSK at 18 ksym/s, continuous
// base-station carrier of 510-bit timeslots. Differential detection at
// 4 samples/symbol; the slot boundary is found from the training sequences
// (synchronization "y" or normal "n"/"p") and then tracked slot by slot.
//
// Decoded: BSCH (MCC, MNC, colour code, TDMA time), BNCH SYSINFO (carrier,
// location area, services), AACH (timeslot usage) and the MAC PDUs of the
// signalling channels (MCCH / SCCH SCH/F + SCH/HD, STCH on traffic slots):
// MAC-RESOURCE with its channel allocation element, fragmented TM-SDUs
// (MAC-RESOURCE + MAC-FRAG + MAC-END) reassembled per timeslot, the basic
// link LLC (FCS checked when present), MLE and the CMCE call control PDUs
// with their elements. Speech: ACELP through the user's ETSI codec.
// Air-interface-encrypted signalling shows only its address.
//
// Trunking (kp::Host::trunk_system / grant / grant_end / call): every
// channel allocation that comes with a call control PDU becomes a grant
// (carrier -> Hz with the band / offset of SYSINFO or of the element's
// extended numbering, uplink from the duplex spacing of TS 100 392-15);
// D-RELEASE ends it. An instance given a timeslot (the grant's options) is
// a TRAFFIC instance: it plays, tracks and reports only that timeslot.

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
constexpr double RATE = 72000.0;

const uint8_t Y_BITS[38] = {1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1,
                            0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1};
const uint8_t N_BITS[22] = {1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0};
const uint8_t P_BITS[22] = {0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0};
constexpr int Y_POS = 214;    // SB: q(12) ph(2) f(80) sb1(120)
constexpr int NP_POS = 244;   // NDB: q(12) ph(2) bkn1(216) bb1(14)

// CMCE downlink PDU types (EN 300 392-2 table 14.66)
enum : int {
    D_ALERT = 0, D_CALL_PROCEEDING = 1, D_CONNECT = 2, D_CONNECT_ACK = 3, D_DISCONNECT = 4, D_INFO = 5,
    D_RELEASE = 6, D_SETUP = 7, D_STATUS = 8, D_TX_CEASED = 9, D_TX_CONTINUE = 10, D_TX_GRANTED = 11,
    D_TX_WAIT = 12, D_TX_INTERRUPT = 13, D_CALL_RESTORE = 14, D_SDS_DATA = 15, D_FACILITY = 16
};

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
    return t >= 0 && t < 17 ? n[t] : "CMCE";
}

bool call_pdu(int t) { return t >= 0 && t <= D_CALL_RESTORE && t != D_STATUS; }

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

// Duplex spacing in MHz by frequency band (1..9 = base 100..900 MHz) and
// the 3-bit duplex spacing code (ETSI TS 100 392-15 V1.5.1 table 2); < 0 =
// reserved / "defined by other means"
double duplex_mhz(int band, int code) {
    static const double t[10][8] = {
        {-1, -1, -1, -1, -1, -1, -1, -1},
        {1.6, 4.5, 0, -1, -1, -1, -1, -1},    // 100 MHz
        {10, -1, 0, -1, -1, -1, -1, -1},      // 200 MHz
        {10, 36, 0, 8, 18, -1, -1, -1},       // 300 MHz
        {10, 7, 0, 8, 5, 9.5, -1, -1},        // 400 MHz
        {10, -1, 0, -1, -1, -1, -1, -1},      // 500 MHz
        {10, -1, 0, -1, 30, -1, -1, -1},      // 600 MHz
        {-1, -1, 0, -1, 30, -1, -1, -1},      // 700 MHz
        {-1, 45, 0, 18, -1, -1, -1, -1},      // 800 MHz
        {-1, 45, 0, 18, 39, -1, -1, -1}};     // 900 MHz
    if (band < 1 || band > 9 || code < 0 || code > 7) return -1;
    return t[band][code];
}

// downlink carrier frequency (EN 300 392-2 21.4.4.1 / 21.5.2): base (band x
// 100 MHz, TS 100 392-15 table 1) + carrier x 25 kHz + offset + the
// allocated bandwidth's centre; NAN for a reserved band
double carrier_hz(int band, int carrier, int offset, int bw_khz) {
    static const double offs[4] = {0, 6250, -6250, 12500};
    if (band < 1 || band > 9) return NAN;
    return band * 100e6 + carrier * 25e3 + offs[offset & 3] + (bw_khz / 2.0 - 12.5) * 1e3;
}

// FCS of the basic link (EN 300 392-2 annex C): CRC-32 of G(x) =
// 0x04C11DB7, the first 32 bits complemented (initial value all ones), the
// remainder complemented
bool llc_fcs_ok(const uint8_t* b, int n, const uint8_t* fcs) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((crc >> 31) ^ b[i]) & 1u;
        crc <<= 1;
        if (fb) crc ^= 0x04C11DB7u;
    }
    crc = ~crc;
    return crc == bits_to_u32(fcs, 32);
}

// the TM-SDU [a, b) without its fill bits (a 1, then zeros up to b); -1 when
// fill bits are indicated but there is no 1
int strip_fill(const uint8_t* t, int a, int b) {
    for (int i = b - 1; i >= a; i--)
        if (t[i] & 1) return i;
    return -1;
}

std::string ssi_str(uint32_t s) { return std::to_string(s); }

}  // namespace

uint32_t TetraBits::get(int k) {
    if (k <= 0) return 0;
    if (pos + k > n) {
        bad = true;
        pos = n;
        return 0;
    }
    uint32_t v = bits_to_u32(b + pos, k);
    pos += k;
    return v;
}

void TetraBits::skip(int k) {
    if (pos + k > n) {
        bad = true;
        pos = n;
    } else {
        pos += k;
    }
}

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
    cell_from_bsch_ = false;
    tn_ = fn_ = mn_ = 0;
    packet_slot_ = -1;
    own_ok_ = -1000000000;
    foreign_bsch_ = 0;
    voice_tn_ = 0;
    voice_tn_ms_ = 0;
    voice_ch_sent_ = -2;
    encrypted_ms_ = -1000000;
    last_tch_ms_ = 0;
    if (codec_) codec_->stop();
    tr_ = Traffic();   // (the host forgets the call with the RESET)
    clear_cell();
    // the statistics are the channel's: a retune starts them again
    n_alloc_ = n_alloc_parsed_ = n_alloc_call_ = n_alloc_enc_ = n_grants_ = 0;
    unparsed_.clear();
    stats_s_ = -1e9;
    for (const char* k : {"Channel allocations", "Grants", "Not parsed", "Trunk following"})
        ctx_.report->erase(Mode::TETRA, k);
    // the grant's identity stays: the next carrier descrambles at once
    set_identity(opt_mcc_, opt_mnc_, opt_cc_);
}

// Everything learned from one cell: on a RESET and when the BSCH names
// another cell (a VFO without the grant's identity: the user's own)
void TetraReceiver::clear_cell() {
    if (tr_.active || tr_.talker_on) traffic_end(kp::Call::ENDED, slot_start_ < 0 ? tr_.last_tch : slot_start_);
    tr_ = Traffic();
    last_sysinfo_.clear();
    have_sys_ = false;
    have_la_ = false;
    carrier_.clear();
    for (int& u : slot_usage_) u = -1;
    for (bool& e : slot_e2e_) e = false;
    for (Frag& f : frag_) f = Frag();
    labels_.clear();
    calls_.clear();
    sys_sent_ = -1;
    sys_sent_s_ = -1e9;
    mcch_seen_ = false;
    main_sent_.clear();
    control_sent_.clear();
    air_enc_noted_ = false;
}

// a CRC-valid block / BSCH of the cell in force within the last 1.5 s (the
// AACH, not CRC-protected, decides a traffic instance's call only then:
// under a stronger co-channel cell its words are the other cell's)
bool TetraReceiver::own_recent() const {
    return slot_start_ >= 0 && slot_start_ - own_ok_ < static_cast<int64_t>(1.5 * RATE);
}

void TetraReceiver::set_traffic_slot(int tn) {
    tslot_ = tn >= 1 && tn <= 4 ? tn : 0;
    tr_ = Traffic();
}

void TetraReceiver::set_identity(int64_t mcc, int64_t mnc, int64_t cc) {
    opt_mcc_ = mcc >= 0 && mcc <= 1023 ? mcc : -1;
    opt_mnc_ = mnc >= 0 && mnc <= 16383 ? mnc : -1;
    opt_cc_ = cc >= 0 && cc <= 63 ? cc : -1;
    if (!locked_identity()) return;
    if (have_cell_ && (mcc_ != static_cast<uint32_t>(opt_mcc_) || mnc_ != static_cast<uint32_t>(opt_mnc_) ||
                       cc_ != static_cast<uint32_t>(opt_cc_))) {
        // another cell was in force: forget it, this one is the only one now
        clear_cell();
        cell_from_bsch_ = false;
    }
    if (cell_from_bsch_) return;
    // the grant's scrambling identity: blocks decode (and prove it with their
    // CRC) before the first BSCH; the TDMA time (timeslot numbers) still
    // comes from the BSCH, at most one multiframe (1.02 s) later
    have_cell_ = true;
    mcc_ = static_cast<uint32_t>(opt_mcc_);
    mnc_ = static_cast<uint32_t>(opt_mnc_);
    cc_ = static_cast<uint32_t>(opt_cc_);
    scramb_ = tetra_scramb_init(mcc_, mnc_, cc_);
}

void TetraReceiver::set_call_hint(int cid, int group) {
    opt_cid_ = cid > 0 && cid < 16384 ? cid : -1;
    opt_group_ = group == 0 || group == 1 ? group : -1;
}

double TetraReceiver::now_s() const { return static_cast<double>(slot_start_ < 0 ? hist_base_ : slot_start_) / RATE; }
int64_t TetraReceiver::now_ms_s() const {
    return (slot_start_ < 0 ? hist_base_ : slot_start_) * 1000 / static_cast<int64_t>(RATE);
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
            // a training sequence anywhere in the first 550 bits (more than
            // a slot: every alignment of the window to the slot grid has one)
            for (int b = 0; b + 510 + 40 < nb; b += 2) {
                int pos = -1;
                if (b + 38 <= nb && hamming(bits.data() + b, Y_BITS, 38) <= 3) pos = Y_POS;
                else if (hamming(bits.data() + b, N_BITS, 22) <= 1 || hamming(bits.data() + b, P_BITS, 22) <= 1)
                    pos = NP_POS;
                if (pos < 0) continue;
                // its slot may have begun before the window; the next slot's
                // training sequence confirms, and that slot is taken then
                const int slot_bit = b - pos;
                const int nxt = slot_bit + 510;
                bool conf = false;
                if (nxt + Y_POS >= 0 && nxt + Y_POS + 38 <= nb && hamming(bits.data() + nxt + Y_POS, Y_BITS, 38) <= 4)
                    conf = true;
                if (nxt + NP_POS >= 0 && nxt + NP_POS + 22 <= nb &&
                    (hamming(bits.data() + nxt + NP_POS, N_BITS, 22) <= 2 ||
                     hamming(bits.data() + nxt + NP_POS, P_BITS, 22) <= 2))
                    conf = true;
                if (!conf) continue;
                const int first = slot_bit >= 0 ? slot_bit : nxt;
                slot_start_ = hist_base_ + SPS + best_p + static_cast<int64_t>(first / 2) * SPS;
                dphi_ = best_d;
                locked_slots_ = 0;
                miss_ = 0;
                ctx_.report->set(Mode::TETRA, "Carrier", "pi/4-DQPSK 18 ksym/s, slot sync acquired");
                return;
            }
        }
        conj_ = false;
    }
    // nothing: drop one slot of history (process() calls again until less
    // than a window is left, so the backlog stays bounded). Before
    // 2026-11-20 a training sequence only counted where its slot began
    // inside the window: a window that kept an unlucky place in the slot
    // grid (one slot dropped each time) never found a clean carrier after
    // a RESET; half a slot dropped per 10 ms block instead let the history
    // grow without limit while no carrier was there (review, 2026-11-20)
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
    if (scr != 3) own_ok_ = slot_start_;   // a block under the cell's own code (3 = the BSCH's, any cell)
    return true;
}

void TetraReceiver::bsch(const uint8_t* t) {
    uint32_t cc = bits_to_u32(t + 4, 6);
    int tn = static_cast<int>(bits_to_u32(t + 10, 2)) + 1;
    int fn = static_cast<int>(bits_to_u32(t + 12, 5));
    int mn = static_cast<int>(bits_to_u32(t + 17, 6));
    uint32_t mcc = bits_to_u32(t + 31, 10), mnc = bits_to_u32(t + 41, 14);
    // An instance given the grant's identity (a traffic instance) accepts
    // that cell only: another cell's BSCH (a co-channel cell in frequency
    // reuse) changes nothing - not the TDMA time, not the scrambling, no
    // report. Under a stronger such cell the wanted one may simply not
    // decode (its blocks fail their CRC), but the other's calls are never
    // reported as this channel's.
    if (locked_identity() && (mcc != static_cast<uint32_t>(opt_mcc_) || mnc != static_cast<uint32_t>(opt_mnc_) ||
                              cc != static_cast<uint32_t>(opt_cc_))) {
        if (++foreign_bsch_ == 1 || foreign_bsch_ % 50 == 0)
            ctx_.report->event(Mode::TETRA, "Another cell on this carrier (MCC " + std::to_string(mcc) + " MNC " +
                                                std::to_string(mnc) + " colour code " + std::to_string(cc) +
                                                ") ignored: following " + std::to_string(opt_mcc_) + "-" +
                                                std::to_string(opt_mnc_) + " CC" + std::to_string(opt_cc_), 60.0);
        return;
    }
    tn_ = tn;
    fn_ = fn;
    mn_ = mn;
    own_ok_ = slot_start_;
    Report& r = *ctx_.report;
    if (!cell_from_bsch_ || mcc != mcc_ || mnc != mnc_ || cc != cc_) {
        if (cell_from_bsch_ || (have_cell_ && (mcc != mcc_ || mnc != mnc_ || cc != cc_))) {
            // another cell (a VFO without the grant's identity): its own
            // SYSINFO, allocations and calls follow - nothing of this one
            // carries over (a call in progress ends here)
            clear_cell();
        }
        have_cell_ = true;
        cell_from_bsch_ = true;
        mcc_ = mcc;
        mnc_ = mnc;
        cc_ = cc;
        scramb_ = tetra_scramb_init(mcc, mnc, cc);
        r.event(Mode::TETRA, "Cell: MCC " + std::to_string(mcc) + " MNC " + std::to_string(mnc) + " colour code " +
                                 std::to_string(cc), 60.0);
        sys_sent_ = -1;   // the system now, not in a few seconds
    }
    r.set(Mode::TETRA, "Network (MCC / MNC)", std::to_string(mcc) + " / " + std::to_string(mnc));
    r.set(Mode::TETRA, "Colour code", std::to_string(cc));
    static const char* lvl[4] = {"unknown", "low", "medium", "high"};
    r.set(Mode::TETRA, "Cell service level", lvl[bits_to_u32(t + 57, 2)]);
    if (slot_reported()) report_valid(ctx_, Mode::TETRA, slot_start_, slot_start_ + SLOT_SAMPLES);
    cell_packet();
}

// ID separation. The downlink is the base station's: its identity is the
// cell - MCC / MNC + colour code (the colour code exists to tell co-channel
// cells apart: it seeds the scrambling, so a block of another cell fails its
// CRC here) - and every slot whose block passed its CRC under this cell's
// code (or that carried its BSCH) is a packet of it, the whole 255-symbol
// slot, in the 25 kHz channel. Cells sharing a carrier in frequency reuse,
// heard in turn as propagation changes or while driving, then each keep their
// own bearing. One packet per slot. A traffic instance: its timeslot's only.
void TetraReceiver::cell_packet() {
    if (!have_cell_ || !ctx_.packet || slot_start_ == packet_slot_ || !slot_reported()) return;
    packet_slot_ = slot_start_;
    RxContext::Packet p;
    p.id = std::to_string(mcc_) + "-" + std::to_string(mnc_) + " CC" + std::to_string(cc_);
    p.label = have_la_ ? "LA " + std::to_string(la_) + (carrier_.empty() ? "" : " · " + carrier_) : "TETRA cell";
    p.start = slot_start_ - input_delay;
    p.end = p.start + SLOT_SAMPLES;
    p.freq_hz = 0;
    p.bw_hz = 25000;
    p.avg_s = 10;      // a fixed base station, ~20-70 slots a second
    p.keep_s = 1800;
    ctx_.packet(p);
}

void TetraReceiver::sysinfo(const uint8_t* t) {
    const uint8_t* c = t + 4;   // after PDU type (2) + broadcast type (2)
    uint32_t carrier = bits_to_u32(c, 12); c += 12;
    uint32_t band = bits_to_u32(c, 4); c += 4;
    uint32_t off = bits_to_u32(c, 2); c += 2;
    uint32_t duplex = bits_to_u32(c, 3); c += 3;
    bool reverse = *c++;
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
    la_ = la;
    have_la_ = true;
    have_sys_ = true;
    sys_carrier_ = static_cast<int>(carrier);
    sys_band_ = static_cast<int>(band);
    sys_offset_ = static_cast<int>(off);
    sys_duplex_ = static_cast<int>(duplex);
    sys_reverse_ = reverse;
    char mhz[32];
    snprintf(mhz, sizeof mhz, "%.4f MHz", fdl / 1e6);
    carrier_ = mhz;
    r.set(Mode::TETRA, "Secondary control channels", std::to_string(nsec));
    std::string s;
    static const struct { int bit; const char* name; } sv[] = {
        {11, "registration"}, {9, "priority cell"}, {6, "system-wide services"}, {5, "voice"},
        {4, "circuit data"}, {2, "packet data (SNDCP)"}, {1, "air encryption"}, {0, "advanced link"}};
    for (const auto& x : sv)
        if (svc & (1u << x.bit)) s += std::string(s.empty() ? "" : ", ") + x.name;
    r.set(Mode::TETRA, "Services", s);
    if (now_ms_s() - encrypted_ms_ >= 60000)
        r.set(Mode::TETRA, "Air encryption", (svc & 2) ? "supported/enabled by the cell" : "not offered");
    std::string sig = std::string(b) + " LA " + std::to_string(la);
    if (sig != last_sysinfo_) {
        last_sysinfo_ = sig;
        r.event(Mode::TETRA, "SYSINFO: main carrier " + std::string(b) + ", LA " + std::to_string(la) +
                                 ", duplex spacing code " + std::to_string(duplex), 60.0);
        sys_sent_ = -1;   // the site may have changed
    }
}

void TetraReceiver::aach(const uint8_t* b30) {
    uint32_t m;
    if (rm_30_14().decode(bits_to_u32(b30, 30), &m, 3) < 0) return;
    aach_ok_ = true;
    int hdr = static_cast<int>(m >> 12), f1 = static_cast<int>((m >> 6) & 63);
    if (fn_ == 18) return;   // frame 18 is always control
    std::string use;
    if (hdr == 0) use = "common control";
    else if (f1 == 0) use = "unallocated";
    else if (f1 == 1) use = "assigned control";
    else if (f1 == 2) use = "common control (MCCH)";
    else if (f1 == 3) use = "common + assigned control";
    else use = "traffic (usage marker " + std::to_string(f1) + ")";
    if (tn_ >= 1 && tn_ <= 4) {
        ctx_.report->set(Mode::TETRA, "Timeslot " + std::to_string(tn_), use);
        slot_usage_[tn_] = hdr == 0 ? 2 : f1;
        if (hdr != 0 && f1 == 0) slot_e2e_[tn_] = false;
        // the main carrier's timeslot 1 in frames 1-17 carries the MCCH
        if (tn_ == 1 && (hdr == 0 || f1 == 2 || f1 == 3)) mcch_seen_ = true;
        if (tslot_ && tn_ == tslot_ && own_recent()) {
            if (hdr != 0 && f1 == 0) {
                // the slot is free again: the call is over (released)
                if (++tr_.unalloc >= 2 && (tr_.known || tr_.active)) traffic_end(kp::Call::RELEASED, slot_start_);
            } else {
                tr_.unalloc = 0;
                // hang time: assigned control instead of traffic
                if (hdr != 0 && f1 == 1 && tr_.active && slot_start_ - tr_.last_tch > static_cast<int64_t>(0.25 * RATE))
                    traffic_end(kp::Call::ENDED, tr_.last_tch);
            }
        }
    }
}

// --- channel allocation element (EN 300 392-2 21.5.2, table 21.87) ----------
bool TetraReceiver::parse_alloc(TetraBits& r, TetraChanAlloc& a) {
    a = TetraChanAlloc();
    a.type = static_cast<int>(r.get(2));
    a.ts_mask = static_cast<int>(r.get(4));
    a.updown = static_cast<int>(r.get(2));
    a.clch = r.flag();
    a.cell_change = r.flag();
    a.carrier = static_cast<int>(r.get(12));
    a.ext = r.flag();
    if (a.ext) {
        a.band = static_cast<int>(r.get(4));
        a.offset = static_cast<int>(r.get(2));
        a.duplex = static_cast<int>(r.get(3));
        a.reverse = r.flag();
    }
    int mon = static_cast<int>(r.get(2));
    if (mon == 0) r.skip(2);   // frame 18 monitoring pattern
    if (a.updown == 0) {
        // augmented channel allocation
        int ud = static_cast<int>(r.get(2));
        int bw = static_cast<int>(r.get(3));
        a.modulation = static_cast<int>(r.get(3));
        r.skip(3);   // maximum uplink QAM modulation level, or reserved
        r.skip(3);   // conforming channel status
        r.skip(4);   // BS link imbalance
        r.skip(5);   // BS transmit power relative to the main carrier
        int nap = static_cast<int>(r.get(2));
        if (nap == 1) r.skip(11);   // napping information
        r.skip(4);   // reserved
        if (r.flag()) r.skip(16);   // conditional element A
        if (r.flag()) r.skip(16);   // conditional element B
        bool further = r.flag();
        static const int bws[8] = {25, 50, 100, 150, 0, 0, 0, 0};
        a.bw_khz = bws[bw & 7];
        // notes 5 / 15: discard the allocation and its TM-SDU
        a.discard = ud == 0 || further || a.bw_khz == 0;
        a.updown = ud;
    }
    return !r.bad;
}

void TetraReceiver::frag_abandon(int tn) {
    if (tn < 0 || tn > 4) return;
    Frag& f = frag_[tn];
    if (f.on) not_parsed("fragmented TM-SDU lost (a block failed its CRC / missing)");
    f = Frag();
}

void TetraReceiver::not_parsed(const std::string& what) {
    auto it = unparsed_.find(what);
    if (it != unparsed_.end()) it->second++;
    else if (unparsed_.size() < 40) unparsed_[what] = 1;
}

void TetraReceiver::stats_fact(bool force) {
    double t = now_s();
    if (!force && t - stats_s_ < 2.0) return;
    stats_s_ = t;
    Report& r = *ctx_.report;
    if (n_alloc_) {
        r.set(Mode::TETRA, "Channel allocations",
              std::to_string(n_alloc_) + " (" + std::to_string(n_alloc_parsed_) + " parsed to the end, " +
                  std::to_string(n_alloc_call_) + " with call control, " + std::to_string(n_alloc_enc_) +
                  " air-encrypted)");
    }
    if (n_grants_) r.set(Mode::TETRA, "Grants", std::to_string(n_grants_));
    if (!unparsed_.empty()) {
        std::vector<std::pair<uint64_t, std::string>> v;
        for (const auto& kv : unparsed_) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        std::string s;
        for (size_t i = 0; i < v.size() && i < 8; i++)
            s += (s.empty() ? "" : "; ") + v[i].second + ": " + std::to_string(v[i].first);
        r.set(Mode::TETRA, "Not parsed", s);
    }
}

// One MAC block (type-1 bits, k1 of them) of a signalling channel
void TetraReceiver::mac_pdus(const uint8_t* t, int k1, const char* chan, bool second_half) {
    Report& r = *ctx_.report;
    const int tnk = tn_ >= 1 && tn_ <= 4 ? tn_ : 0;
    TetraBits rd(t, k1);
    while (rd.left() >= 16) {
        const int start = rd.pos;
        int type = static_cast<int>(rd.get(2));
        if (type == 2) {   // broadcast
            int bt = static_cast<int>(rd.get(2));
            if (bt == 0 && k1 >= 124 && start == 0) sysinfo(t);
            else if (bt != 0) not_parsed(bt == 1 ? "ACCESS-DEFINE" : "broadcast type " + std::to_string(bt));
            return;
        }
        if (type == 3) {   // MAC-D-BLCK (SCH/F) / MAC-U-SIGNAL (STCH)
            not_parsed(k1 >= 268 ? "MAC-D-BLCK" : "MAC-U-SIGNAL");
            return;
        }
        if (type == 1) {
            bool is_end = rd.flag();
            bool fill = rd.flag();
            Frag& f = frag_[tnk];
            if (!is_end) {
                // MAC-FRAG: the TM-SDU continues to the end of the block
                int e = fill ? strip_fill(t, rd.pos, k1) : k1;
                if (!f.on) { not_parsed("MAC-FRAG without its start"); return; }
                if (e < rd.pos) { frag_abandon(tnk); return; }
                f.sdu.insert(f.sdu.end(), t + rd.pos, t + e);
                f.idle_slots = 0;
                if (f.sdu.size() > 2700) frag_abandon(tnk);   // longer than the longest TM-SDU (N.202)
                return;
            }
            // MAC-END (downlink, pi/4-DQPSK: no napping flag, no multiple slot granting)
            rd.skip(1);   // position of grant
            int li = static_cast<int>(rd.get(6));
            if (rd.flag()) rd.skip(8);   // basic slot granting
            TetraMacInfo mi = f.mac;
            bool ca = rd.flag();
            if (ca) {
                n_alloc_++;
                if (!parse_alloc(rd, mi.alloc)) { not_parsed("MAC-END channel allocation cut"); frag_abandon(tnk); return; }
                mi.has_alloc = true;
            }
            int pdu_end = start + li * 8;
            if (rd.bad || li == 0 || li > 0x3A || pdu_end > k1 || pdu_end < rd.pos) {
                not_parsed("MAC-END length");
                frag_abandon(tnk);
                return;
            }
            int e = fill ? strip_fill(t, rd.pos, pdu_end) : pdu_end;
            if (!f.on) {
                not_parsed("MAC-END without its start");
            } else if (e >= rd.pos) {
                f.sdu.insert(f.sdu.end(), t + rd.pos, t + e);
                std::vector<uint8_t> sdu;
                sdu.swap(f.sdu);
                std::string ch = f.chan + " (reassembled)";
                f = Frag();
                tm_sdu(sdu.data(), static_cast<int>(sdu.size()), mi, ch.c_str());
            }
            frag_[tnk] = Frag();
            rd.pos = pdu_end;
            continue;
        }
        // MAC-RESOURCE (EN 300 392-2 21.4.3.1, table 21.55; pi/4-DQPSK fields)
        bool fill = rd.flag();
        rd.skip(1);   // position of grant
        int enc = static_cast<int>(rd.get(2));
        rd.skip(1);   // random access flag
        int li = static_cast<int>(rd.get(6));
        int at = static_cast<int>(rd.get(3));
        if (rd.bad || at == 0) return;   // null PDU: the rest of the block is fill
        TetraMacInfo mi;
        mi.addr_type = at;
        mi.enc = enc;
        switch (at) {
            case 1: case 3: case 4: mi.ssi = rd.get(24); break;
            case 2: {
                mi.event_label = static_cast<int>(rd.get(10));
                auto it = labels_.find(mi.event_label);
                if (it != labels_.end()) mi.ssi = it->second;
                break;
            }
            case 5: case 7:
                mi.ssi = rd.get(24);
                mi.event_label = static_cast<int>(rd.get(10));
                if (!rd.bad && enc == 0) {
                    if (labels_.size() >= 64 && !labels_.count(mi.event_label)) labels_.erase(labels_.begin());
                    labels_[mi.event_label] = mi.ssi;
                }
                break;
            case 6:
                mi.ssi = rd.get(24);
                mi.usage_marker = static_cast<int>(rd.get(6));
                break;
            default: break;
        }
        if (rd.flag()) rd.skip(4);   // power control
        if (rd.flag()) rd.skip(8);   // basic slot granting
        bool ca = rd.flag();
        const bool rest = li == 0x3E || li == 0x3F;   // second half slot stolen / start of fragmentation
        if (rd.bad || li == 0 || (li > 0x3A && !rest)) { not_parsed("MAC-RESOURCE length"); return; }
        const int pdu_end = rest ? k1 : start + li * 8;
        if (pdu_end > k1) { not_parsed("MAC-RESOURCE longer than its block"); return; }
        std::string who = at == 2 ? (mi.ssi ? "SSI " + ssi_str(mi.ssi) + " (event label)" : "event label " + std::to_string(mi.event_label))
                                  : "SSI " + ssi_str(mi.ssi);
        if (enc) {
            // air-interface encryption: the channel allocation element and the
            // TM-SDU are encrypted - nothing after the header can be read, the
            // calls can't be followed
            encrypted_ms_ = now_ms_s();
            r.set(Mode::TETRA, "Air encryption", "in use (encryption class " + std::to_string(enc) + ")");
            r.event(Mode::TETRA, std::string(chan) + " " + who + ": encrypted signalling" +
                                     (ca ? " + channel allocation" : ""), ctx_.opts->verbose ? 2.0 : 10.0);
            if (ca) { n_alloc_++; n_alloc_enc_++; }
            if (!air_enc_noted_ && trunk) {
                air_enc_noted_ = true;
                r.set(Mode::TETRA, "Trunk following",
                      "not possible: air-interface encryption hides the channel allocations and call set-up");
            }
            if (rest) return;
            rd.pos = pdu_end;
            continue;
        }
        if (ca) {
            n_alloc_++;
            if (!parse_alloc(rd, mi.alloc)) { not_parsed("channel allocation cut"); return; }
            mi.has_alloc = true;
        }
        if (rd.pos > pdu_end) { not_parsed("MAC-RESOURCE header longer than the PDU"); return; }
        int e = fill ? strip_fill(t, rd.pos, pdu_end) : pdu_end;
        if (e < rd.pos) { not_parsed("fill bits without their marker"); return; }
        if (li == 0x3F) {
            // start of a fragmented TM-SDU: MAC-FRAG / MAC-END follow on this timeslot
            Frag& f = frag_[tnk];
            f = Frag();
            f.on = true;
            f.sdu.assign(t + rd.pos, t + e);
            f.mac = mi;
            f.chan = chan;
            (void)second_half;
            return;
        }
        if (e > rd.pos) {
            tm_sdu(t + rd.pos, e - rd.pos, mi, chan);
        } else {
            if (mi.has_alloc) {
                n_alloc_parsed_++;
                not_parsed("channel allocation without a TM-SDU");
            }
            if (at != 2) r.set(Mode::TETRA, "Last address", who);
            if (ctx_.opts->verbose && at != 2)
                r.event(Mode::TETRA, std::string(chan) + " " + who + (mi.has_alloc ? ": channel allocation" : ""), 2.0);
        }
        if (li == 0x3E) return;   // the second half slot is a block of its own
        rd.pos = pdu_end;
    }
}

// A complete TM-SDU (TMA-UNITDATA): basic link LLC -> MLE -> MM / CMCE / SNDCP
void TetraReceiver::tm_sdu(const uint8_t* sdu, int n, const TetraMacInfo& m, const char* chan) {
    Report& r = *ctx_.report;
    std::string who = m.addr_type == 2 && !m.ssi ? "event label " + std::to_string(m.event_label)
                                                 : "SSI " + ssi_str(m.ssi) + (m.addr_type == 2 ? " (event label)" : "");
    TetraBits rd(sdu, n);
    int llc = static_cast<int>(rd.get(4));
    if (rd.bad) return;
    if (llc >= 8) {
        static const char* al[8] = {"AL-SETUP", "AL-DATA", "AL-UDATA", "AL-ACK", "AL-RECONNECT", "LLC supplementary",
                                    "LLC layer 2 signalling", "AL-DISC"};
        not_parsed(al[llc - 8]);
        return;
    }
    static const int llc_hdr[8] = {2, 1, 0, 1, 2, 1, 0, 1};
    rd.skip(llc_hdr[llc]);
    int end = n;
    if (llc >= 4) {
        // basic link with FCS: the last 32 bits check the TL-SDU
        if (n - rd.pos < 32) { not_parsed("LLC FCS cut"); return; }
        end = n - 32;
        if (!llc_fcs_ok(sdu + rd.pos, end - rd.pos, sdu + end)) { not_parsed("LLC FCS failed"); return; }
    }
    if ((llc == 3 || llc == 7) && end - rd.pos <= 0) return;   // BL-ACK alone
    if (end - rd.pos < 3) return;
    int pd = static_cast<int>(rd.get(3));
    const uint8_t* p = sdu + rd.pos;
    const int pn = end - rd.pos;
    std::string what;
    if (pd == 2) {
        TetraCmce c;
        // a call control PDU counts only when its elements end exactly where
        // the TM-SDU (fill bits removed) does - every one in the real
        // recordings does; a corrupted one (that passed the CRC) hardly would
        bool ok = parse_cmce(p, pn, c) && (c.exact || !call_pdu(c.type));
        if (m.has_alloc) {
            if (ok) n_alloc_parsed_++;
            if (ok && call_pdu(c.type)) n_alloc_call_++;
        }
        if (!ok) {
            not_parsed(std::string(cmce_name(c.type)) + " (cut / malformed)");
            what = cmce_name(c.type);
        } else {
            on_cmce(c, m, chan);
            return;
        }
    } else if (pd == 1) {
        what = pn >= 4 ? mm_name(static_cast<int>(bits_to_u32(p, 4))) : "MM";
        if (m.has_alloc) n_alloc_parsed_++;
    } else if (pd == 4) {
        what = "SNDCP (packet data)";
        if (m.has_alloc) {
            n_alloc_parsed_++;
            // a packet data channel assignment: reported as a data grant
            int tn0 = 0;
            kp::TrunkChannel ch = channel_of(m.alloc, &tn0);
            if (trunk && !m.alloc.discard && m.alloc.ts_mask && (tslot_ == 0 || tn_ == tslot_) && m.ssi) {
                CallRec k;
                k.target = m.ssi;
                k.group = 0;
                k.data = true;
                send_grant(k, m, tn0, ch, true);
            }
        }
    } else if (pd == 5) {
        what = "MLE";
        if (m.has_alloc) n_alloc_parsed_++;
    } else {
        not_parsed("MLE protocol " + std::to_string(pd));
        if (m.has_alloc) n_alloc_parsed_++;
    }
    if (!what.empty())
        r.event(Mode::TETRA, std::string(chan) + " " + who + ": " + what, ctx_.opts->verbose ? 2.0 : 5.0);
    if (m.addr_type != 2) r.set(Mode::TETRA, "Last address", who);
}

// --- CMCE downlink PDUs (EN 300 392-2 14.7.1, encoding annex E.1.1) -------
bool TetraReceiver::parse_cmce(const uint8_t* p, int n, TetraCmce& c) {
    TetraBits r(p, n);
    c = TetraCmce();
    c.type = static_cast<int>(r.get(5));
    // type 2 element: its P-bit, then the element; -1 = not present
    auto t2 = [&r](int len) -> int64_t { return r.flag() ? static_cast<int64_t>(r.get(len)) : -1; };
    // type 3 / 4 elements: M-bit, identifier (4), length (11), the element
    auto t3 = [&r]() {
        while (!r.bad && r.flag()) {
            r.skip(4);
            int len = static_cast<int>(r.get(11));
            if (len == 0) { r.bad = true; break; }
            r.skip(len);
        }
    };
    // calling / transmitting party type identifier (type 2) + its conditional address
    auto party = [&r, &c]() {
        if (!r.flag()) return;
        int id = static_cast<int>(r.get(2));
        if (id == 1 || id == 2) c.party = r.get(24);
        if (id == 2) r.skip(24);   // the extension (MCC + MNC)
    };
    switch (c.type) {
        case D_ALERT:
            c.cid = static_cast<int>(r.get(14));
            r.skip(3);   // call time-out, set-up phase
            r.skip(1);   // reserved
            c.simplex = static_cast<int>(r.get(1));
            r.skip(1);   // call queued
            if (r.flag()) { c.bsi = static_cast<int>(t2(8)); t2(6); t3(); }
            break;
        case D_CALL_PROCEEDING:
            c.cid = static_cast<int>(r.get(14));
            r.skip(3);
            c.hook = static_cast<int>(r.get(1));
            c.simplex = static_cast<int>(r.get(1));
            if (r.flag()) { c.bsi = static_cast<int>(t2(8)); t2(3); t2(6); t3(); }
            break;
        case D_CONNECT:
            c.cid = static_cast<int>(r.get(14));
            c.timeout = static_cast<int>(r.get(4));
            c.hook = static_cast<int>(r.get(1));
            c.simplex = static_cast<int>(r.get(1));
            c.txgrant = static_cast<int>(r.get(2));
            r.skip(1);   // transmission request permission
            r.skip(1);   // call ownership
            if (r.flag()) {
                c.priority = static_cast<int>(t2(4));
                c.bsi = static_cast<int>(t2(8));
                t2(24);   // temporary address
                t2(6);    // notification indicator
                t3();
            }
            break;
        case D_CONNECT_ACK:
            c.cid = static_cast<int>(r.get(14));
            c.timeout = static_cast<int>(r.get(4));
            c.txgrant = static_cast<int>(r.get(2));
            r.skip(1);
            if (r.flag()) { t2(6); t3(); }
            break;
        case D_DISCONNECT:
        case D_RELEASE:
            c.cid = static_cast<int>(r.get(14));
            c.cause = static_cast<int>(r.get(5));
            if (r.flag()) { t2(6); t3(); }
            break;
        case D_INFO:
            c.cid = static_cast<int>(r.get(14));
            r.skip(1);   // reset call time-out timer
            r.skip(1);   // poll request
            if (r.flag()) {
                c.new_cid = static_cast<int>(t2(14));
                c.timeout = static_cast<int>(t2(4));
                t2(3); t2(1); t2(9); t2(3); t2(24); t2(6); t2(6); t2(6);
                t3();
            }
            break;
        case D_SETUP:
            c.cid = static_cast<int>(r.get(14));
            c.timeout = static_cast<int>(r.get(4));
            c.hook = static_cast<int>(r.get(1));
            c.simplex = static_cast<int>(r.get(1));
            c.bsi = static_cast<int>(r.get(8));
            c.txgrant = static_cast<int>(r.get(2));
            r.skip(1);   // transmission request permission
            c.priority = static_cast<int>(r.get(4));
            if (r.flag()) {
                t2(6);    // notification indicator
                t2(24);   // temporary address
                party();  // calling party
                t3();
            }
            break;
        case D_TX_CEASED:
        case D_TX_WAIT:
            c.cid = static_cast<int>(r.get(14));
            r.skip(1);
            if (r.flag()) { t2(6); t3(); }
            break;
        case D_TX_CONTINUE:
            c.cid = static_cast<int>(r.get(14));
            r.skip(1);   // continue
            r.skip(1);
            if (r.flag()) { t2(6); t3(); }
            break;
        case D_TX_GRANTED:
        case D_TX_INTERRUPT:
            c.cid = static_cast<int>(r.get(14));
            c.txgrant = static_cast<int>(r.get(2));
            r.skip(1);   // transmission request permission
            c.enc_ctrl = static_cast<int>(r.get(1));
            r.skip(1);   // reserved
            if (r.flag()) {
                t2(6);
                party();  // transmitting party
                t3();
            }
            break;
        case D_CALL_RESTORE:
            c.cid = static_cast<int>(r.get(14));
            c.txgrant = static_cast<int>(r.get(2));
            r.skip(1);
            r.skip(1);
            if (r.flag()) {
                c.new_cid = static_cast<int>(t2(14));
                c.timeout = static_cast<int>(t2(4));
                t2(3); t2(9); t2(6);
                t3();
            }
            break;
        default:
            // D-STATUS, D-SDS-DATA, D-FACILITY...: no call control - the type only
            return !r.bad;
    }
    c.exact = r.pos == n;
    return !r.bad;
}

TetraReceiver::CallRec* TetraReceiver::find_call(int cid) {
    for (auto& k : calls_)
        if (k.cid == cid) return &k;
    return nullptr;
}

TetraReceiver::CallRec& TetraReceiver::call_rec(int cid) {
    if (CallRec* k = find_call(cid)) return *k;
    if (calls_.size() >= 64) {
        auto old = std::min_element(calls_.begin(), calls_.end(),
                                    [](const CallRec& a, const CallRec& b) { return a.last_s < b.last_s; });
        calls_.erase(old);
    }
    CallRec k;
    k.cid = cid;
    k.last_s = now_s();
    calls_.push_back(k);
    return calls_.back();
}

// A call record lives CALL_KEEP_S after it was last heard. EN 300 392-2
// sets no "call heard" timer the receiver could use (T310, the call
// time-out, runs to 30 min or without limit); back-up D-SETUPs came every
// ~15 s on air, and the longest set-up timer (T301 / T302) is 60 s - a
// record silent for 60 s belongs to a call released where this VFO
// doesn't hear it (on its traffic carrier). Bounded to 64 besides.
void TetraReceiver::expire_calls() {
    const double t = now_s();
    calls_.erase(std::remove_if(calls_.begin(), calls_.end(),
                                [t](const CallRec& k) { return t - k.last_s > CALL_KEEP_S; }),
                 calls_.end());
}

void TetraReceiver::forget_call(int cid) {
    calls_.erase(std::remove_if(calls_.begin(), calls_.end(), [cid](const CallRec& k) { return k.cid == cid; }),
                 calls_.end());
}

// The channel an allocation points to: name "<carrier>/<timeslot(s)>"
// (TETRA timeslot numbers 1-4; "<band>:" in front when the element's
// extended numbering names another band / offset than SYSINFO), the
// downlink from SYSINFO's band / offset or the element's, the uplink from
// the duplex spacing table. slot = the first assigned timeslot, 0-based.
kp::TrunkChannel TetraReceiver::channel_of(const TetraChanAlloc& a, int* first_tn) const {
    kp::TrunkChannel ch;
    std::string tns;
    int first = 0;
    for (int i = 0; i < 4; i++)
        if (a.ts_mask & (8 >> i)) {
            if (!first) first = i + 1;
            tns += (tns.empty() ? "" : "+") + std::to_string(i + 1);
        }
    *first_tn = first;
    const bool known = a.ext || have_sys_;
    const int band = a.ext ? a.band : sys_band_;
    const int off = a.ext ? a.offset : sys_offset_;
    const int dup = a.ext ? a.duplex : sys_duplex_;
    const bool rev = a.ext ? a.reverse : sys_reverse_;
    std::string pre;
    if (a.ext && have_sys_ && (a.band != sys_band_ || a.offset != sys_offset_))
        pre = std::to_string(a.band) + ":";
    ch.name = pre + std::to_string(a.carrier) + "/" + (tns.empty() ? "-" : tns);
    ch.number = a.carrier;
    if (known) {
        ch.freq_hz = carrier_hz(band, a.carrier, off, a.bw_khz);
        double d = duplex_mhz(band, dup);
        if (std::isfinite(ch.freq_hz) && d > 0) ch.uplink_hz = ch.freq_hz + (rev ? d : -d) * 1e6;
    }
    ch.slot = first ? first - 1 : -1;
    return ch;
}

void TetraReceiver::send_grant(CallRec& k, const TetraMacInfo& m, int first_tn, const kp::TrunkChannel& ch, bool data) {
    if (!trunk || !first_tn) return;
    (void)m;
    // the system first: never a grant the host can't file under its system
    if (sys_sent_ != sys_state()) send_system();
    kp::Grant g;
    g.channel = ch;
    g.target = k.target ? ssi_str(k.target) : "";
    g.group = k.group != 0;   // unknown: a group call (most TETRA calls are)
    g.source = k.source ? ssi_str(k.source) : "";
    g.encrypted = k.enc;
    g.emergency = k.emerg;
    g.data = data || k.data;
    g.update = k.granted && k.ch.name == ch.name;
    // what the traffic instance needs: its timeslot (it then plays and
    // reports only that one), the scrambling identity, the call
    g.options.push_back({"timeslot", std::to_string(first_tn)});
    g.options.push_back({"mcc", std::to_string(mcc_)});
    g.options.push_back({"mnc", std::to_string(mnc_)});
    g.options.push_back({"cc", std::to_string(cc_)});
    if (k.cid > 0) g.options.push_back({"call", std::to_string(k.cid)});
    if (k.group >= 0) g.options.push_back({"group", std::to_string(k.group)});
    trunk->grant(g);
    n_grants_++;
    stats_fact(true);
    if (!g.update) {
        char f[48] = "";
        if (std::isfinite(ch.freq_hz)) snprintf(f, sizeof f, " (%.4f MHz)", ch.freq_hz / 1e6);
        ctx_.report->event(Mode::TETRA, "Grant: " + std::string(g.data ? "data " : "") +
                                            (g.target.empty() ? "call" : (g.group ? "TG " : "unit ") + g.target) +
                                            (g.source.empty() ? "" : " from " + g.source) + " -> carrier " + ch.name + f +
                                            (g.encrypted ? ", end-to-end encrypted" : "") +
                                            (g.emergency ? ", emergency" : ""), 10.0);
    }
    k.granted = true;
    k.ch = ch;
}

// A CMCE PDU that passed every check: events, the call table, grants,
// and on a traffic instance's own timeslot the call it carries
void TetraReceiver::on_cmce(const TetraCmce& c, const TetraMacInfo& m, const char* chan) {
    Report& r = *ctx_.report;
    std::string who = m.addr_type == 2 && !m.ssi ? "event label " + std::to_string(m.event_label)
                                                 : "SSI " + ssi_str(m.ssi) + (m.addr_type == 2 ? " (event label)" : "");
    std::string what = cmce_name(c.type);
    if (call_pdu(c.type) && c.cid >= 0) what += " call " + std::to_string(c.cid);
    int tn0 = 0;
    kp::TrunkChannel ach;
    const bool alloc = m.has_alloc && !m.alloc.discard && m.alloc.ts_mask != 0;
    if (alloc) ach = channel_of(m.alloc, &tn0);
    if (m.has_alloc) what += m.alloc.ts_mask ? " + channel " + ach.name : " + back to the control channel";
    r.event(Mode::TETRA, std::string(chan) + " " + who + ": " + what, ctx_.opts->verbose ? 2.0 : 5.0);
    if (m.addr_type != 2) r.set(Mode::TETRA, "Last address", who);
    if (c.type == D_SETUP || c.type == D_CONNECT || c.type == D_TX_GRANTED) {
        r.set(Mode::TETRA, "Last call", who + ": " + what);
        last_call_ = who;
    }
    if (!call_pdu(c.type) || c.cid <= 0) return;   // 0 = the dummy call identifier
    const bool mine = tslot_ == 0 || tn_ == tslot_;
    if (c.type == D_RELEASE) {
        if (CallRec* k = find_call(c.cid)) {
            if (k->granted && trunk && mine) trunk->grant_end(k->ch);
            forget_call(c.cid);
        }
        if (tn_ >= 1 && tn_ <= 4) slot_e2e_[tn_] = false;
        if (tslot_ && tn_ == tslot_) traffic_end(kp::Call::RELEASED, slot_start_);
        return;
    }
    expire_calls();
    CallRec& k = call_rec(c.cid);
    if (c.type == D_SETUP) {
        // a D-SETUP describes the whole call: the record starts from it
        // alone. It stays "granted" (its next grant an update) only as the
        // back-up of the same call - same called party, or a record of
        // this call identifier without one yet (its D-CONNECT came first,
        // within the 60 s a record lives) - on the same channel
        const bool same = k.target == 0 || k.target == m.ssi;
        CallRec fresh;
        fresh.cid = c.cid;
        if (same) {
            fresh.granted = k.granted;
            fresh.ch = k.ch;
        }
        k = fresh;
    }
    k.last_s = now_s();
    if (c.bsi >= 0) {
        // basic service information: circuit mode type (3), end-to-end
        // encryption flag (1), communication type (2), slots / speech (2)
        int cmt = c.bsi >> 5, comm = (c.bsi >> 2) & 3;
        k.enc = ((c.bsi >> 4) & 1) != 0;
        k.group = comm != 0 ? 1 : 0;
        k.data = cmt != 0;
    }
    if (c.priority >= 0) k.emerg = c.priority == 15;   // pre-emptive priority 4 (emergency), when the PDU says
    switch (c.type) {
        case D_SETUP:
            // sent to the called party: the GSSI of a group call, the called ISSI
            if (m.ssi) k.target = m.ssi;
            if (c.party >= 0) k.source = static_cast<uint32_t>(c.party);
            break;
        case D_CONNECT:
        case D_ALERT:
        case D_CALL_PROCEEDING:
            // sent to the calling party
            if (m.ssi && !k.source) k.source = m.ssi;
            break;
        case D_CONNECT_ACK:
            // sent to the called unit of an individual call
            if (m.ssi && !k.target) { k.target = m.ssi; if (k.group < 0) k.group = 0; }
            break;
        case D_TX_GRANTED:
            if (c.txgrant == 0 || c.txgrant == 3) {
                // a new transmission: its party, else (granted to the
                // addressee) the addressee, else unknown - never the last talker
                if (c.party >= 0) k.source = static_cast<uint32_t>(c.party);
                else k.source = c.txgrant == 0 ? m.ssi : 0;
                // granted to another user: sent to the others (the group)
                if (c.txgrant == 3 && m.ssi && !k.target) k.target = m.ssi;
            }
            if (c.enc_ctrl == 1) k.enc = true;
            break;
        default: break;
    }
    if (tn_ >= 1 && tn_ <= 4 && (c.type == D_TX_GRANTED || c.type == D_SETUP || c.type == D_CONNECT)) slot_e2e_[tn_] = k.enc;
    // a channel allocation with call set-up / a transmission grant: a grant
    if (alloc && mine && c.type != D_DISCONNECT && c.type != D_TX_CEASED && c.type != D_TX_WAIT &&
        c.type != D_TX_INTERRUPT)
        send_grant(k, m, tn0, ach, false);
    // a traffic instance: the call on its own timeslot
    if (!tslot_ || tn_ != tslot_) return;
    switch (c.type) {
        case D_TX_GRANTED:
            if (c.txgrant == 0 || c.txgrant == 3) {
                // another party (or an unnamed one after a named one): a new transmission
                uint32_t src = k.source;
                if (tr_.active && src != tr_.source) traffic_end(kp::Call::ENDED, slot_start_);
                tr_.known = true;
                tr_.cid = c.cid;
                tr_.source = src;
                if (k.target) tr_.target = k.target;
                else if (m.ssi && m.ssi != src) tr_.target = m.ssi;
                tr_.enc = k.enc;
                tr_.emerg = k.emerg;
                tr_.group = (c.cid == opt_cid_ && opt_group_ >= 0) ? opt_group_ : k.group;
                if (!tr_.active) traffic_start(slot_start_);
                else traffic_call(kp::Call::ACTIVE);
            }
            break;
        case D_SETUP:
        case D_CONNECT:
        case D_CONNECT_ACK:
        case D_CALL_RESTORE:
        case D_INFO:
            tr_.known = true;
            tr_.cid = c.cid;
            if (k.target) tr_.target = k.target;
            if (k.source && !tr_.active) tr_.source = k.source;
            tr_.enc = k.enc;
            tr_.emerg = k.emerg;
            tr_.group = (c.cid == opt_cid_ && opt_group_ >= 0) ? opt_group_ : k.group;
            if (tr_.active) traffic_call(kp::Call::ACTIVE);
            break;
        case D_TX_CEASED:
        case D_TX_WAIT:
        case D_TX_INTERRUPT:
            if (tr_.active) traffic_end(kp::Call::ENDED, slot_start_);
            break;
        default: break;
    }
}

// --- the traffic instance's call (kp::Host::call) ----------------------------
void TetraReceiver::traffic_call(int state) {
    tr_.last_report_s = now_s();
    if (!trunk) return;
    kp::Call c;
    c.state = state;
    c.target = tr_.target ? ssi_str(tr_.target) : "";
    c.group = tr_.group != 0;
    c.source = tr_.source ? ssi_str(tr_.source) : "";
    c.slot = tslot_ - 1;
    c.encrypted = tr_.enc;
    c.emergency = tr_.emerg;
    trunk->call(c);
}

void TetraReceiver::traffic_start(int64_t at) {
    if (!tr_.target && !tr_.source) {
        // late entry: the call the grant named, if this instance heard it set
        // up itself (a traffic carrier that is also the main carrier)
        const int cid = tr_.cid > 0 ? tr_.cid : opt_cid_;
        if (const CallRec* k = cid > 0 ? find_call(cid) : nullptr) {
            tr_.cid = cid;
            tr_.target = k->target;
            tr_.source = k->source;
            tr_.enc = k->enc;
            tr_.emerg = k->emerg;
            tr_.group = (cid == opt_cid_ && opt_group_ >= 0) ? opt_group_ : k->group;
        }
    }
    tr_.active = true;
    tr_.known = true;
    tr_.start = at;
    tr_.last_tch = at;
    tr_.unalloc = 0;
    traffic_call(kp::Call::ACTIVE);
}

void TetraReceiver::traffic_end(int state, int64_t at) {
    if (tr_.active || (state == kp::Call::RELEASED && tr_.known)) traffic_call(state);
    if (tr_.talker_on && ctx_.talker_end) ctx_.talker_end(at - input_delay, tslot_ - 1);
    tr_.talker_on = false;
    tr_.active = false;
    if (state == kp::Call::RELEASED) {
        tr_ = Traffic();
        tr_.unalloc = 2;   // no second RELEASED for the same free slot
    }
}

// one per slot on a traffic instance: transmissions that stopped without a
// terminator end, an ongoing one is repeated once a second
void TetraReceiver::traffic_tick() {
    if (!tslot_ || !tr_.active) return;
    if (slot_start_ - tr_.last_tch > static_cast<int64_t>(1.0 * RATE)) {
        traffic_end(kp::Call::ENDED, tr_.last_tch);
        return;
    }
    if (now_s() - tr_.last_report_s >= 1.0) traffic_call(kp::Call::ACTIVE);
}

// the transmission's talker on the traffic timeslot: the transmitting
// party's SSI ("?" until a D-TX GRANTED names it), labelled with the group
void TetraReceiver::traffic_talker(int64_t end) {
    if (!ctx_.talker) return;
    std::string id = tr_.source ? ssi_str(tr_.source) : "?";
    std::string label = tr_.target ? (tr_.group != 0 ? "TG " : "to ") + ssi_str(tr_.target) : "TETRA call";
    ctx_.talker(id, label, tr_.start - input_delay, end - input_delay, tslot_ - 1);
    tr_.talker_on = true;
}

void TetraReceiver::send_system() {
    if (!trunk || !have_cell_) return;
    kp::TrunkSystem s;
    s.key = std::to_string(mcc_) + "-" + std::to_string(mnc_);
    s.name = "TETRA " + s.key;
    s.site = (have_la_ ? "LA " + std::to_string(la_) + " " : "") + "CC " + std::to_string(cc_);
    s.control = tslot_ == 0 && mcch_seen_;
    trunk->trunk_system(s);
    sys_sent_ = sys_state();
    sys_sent_s_ = now_s();
    if (have_sys_ && sys_band_ >= 1 && sys_band_ <= 9) {   // (main carrier 0 / band 0: none)
        // the main carrier: its timeslot 1 is the MCCH
        TetraChanAlloc a;
        a.carrier = sys_carrier_;
        a.ts_mask = 8;
        int tn0 = 0;
        kp::TrunkChannel ch = channel_of(a, &tn0);
        if (ch.name != main_sent_) {
            main_sent_ = ch.name;
            trunk->trunk_channel(ch);
        }
        // this VFO hears the cell on another carrier: the control channel is on the main carrier
        double rf = trunk->rf_hz();
        if (tslot_ == 0 && !mcch_seen_ && std::isfinite(rf) && std::isfinite(ch.freq_hz) &&
            std::fabs(rf - ch.freq_hz) > 6250 && control_sent_ != ch.name) {
            control_sent_ = ch.name;
            trunk->control_channel(ch);
        }
    }
}

// what trunk_system() reports, packed: MCC, MNC, colour code, LA, control
int64_t TetraReceiver::sys_state() const {
    return (static_cast<int64_t>(mcc_ & 0x3FF) << 36) | (static_cast<int64_t>(mnc_ & 0x3FFF) << 22) |
           (static_cast<int64_t>(cc_ & 0x3F) << 16) | (static_cast<int64_t>(have_la_ ? (la_ & 0x3FFF) | 0x4000 : 0) << 1) |
           (tslot_ == 0 && mcch_seen_ ? 1 : 0);
}

void TetraReceiver::trunk_tick() {
    if (!trunk || !have_cell_) return;
    if (sys_state() != sys_sent_ || now_s() - sys_sent_s_ >= 5.0) send_system();
}

void TetraReceiver::decode_slot(const uint8_t* bits, const float* soft) {
    int dy = hamming(bits + Y_POS, Y_BITS, 38);
    int dn = hamming(bits + NP_POS, N_BITS, 22);
    int dp = hamming(bits + NP_POS, P_BITS, 22);
    std::vector<uint8_t> t1;
    aach_ok_ = false;
    const int tnk = tn_ >= 1 && tn_ <= 4 ? tn_ : 0;
    if (frag_[tnk].on && ++frag_[tnk].idle_slots > 8) frag_abandon(tnk);
    if (dy <= 6 && dy * 22 <= std::min(dn, dp) * 38) {
        // synchronization burst: BSCH + AACH + block 2
        if (decode_block(soft + 94, 120, 60, 11, 3, t1)) bsch(t1.data());
        if (have_cell_) {
            uint8_t bb[30];
            std::copy(bits + 252, bits + 282, bb);
            tetra_scramble(bb, 30, scramb_);
            aach(bb);
            if (decode_block(soft + 282, 216, 124, 101, scramb_, t1)) {
                if (slot_reported()) report_valid(ctx_, Mode::TETRA, slot_start_, slot_start_ + SLOT_SAMPLES);
                mac_pdus(t1.data(), 124, "BNCH/SCH-HD", true);
                cell_packet();
            } else {
                frag_abandon(tn_ >= 1 && tn_ <= 4 ? tn_ : 0);
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
    const int tk = tn_ >= 1 && tn_ <= 4 ? tn_ : 0;
    const bool traffic = fn_ != 18 && tk && slot_usage_[tk] >= 4;
    if (dn <= dp && traffic) {
        frag_abandon(tk);
        // full-slot traffic (TCH/S): the AACH (Reed-Muller checked) names it
        // traffic - on a traffic instance's own timeslot that is its call
        if (tslot_ && tk == tslot_ && aach_ok_ && own_recent()) {
            if (!tr_.active) traffic_start(slot_start_);
            tr_.last_tch = slot_start_ + SLOT_SAMPLES;
            report_valid(ctx_, Mode::TETRA, slot_start_, slot_start_ + SLOT_SAMPLES);
            traffic_talker(slot_start_ + SLOT_SAMPLES);
        }
        voice_slot(bits);
        return;
    }
    if (dn <= dp) {
        // full slot SCH/F
        std::vector<float> blk(432);
        std::copy(soft + 14, soft + 230, blk.begin());
        std::copy(soft + 282, soft + 498, blk.begin() + 216);
        if (decode_block(blk.data(), 432, 268, 103, scramb_, t1)) {
            if (slot_reported()) report_valid(ctx_, Mode::TETRA, slot_start_, slot_start_ + SLOT_SAMPLES);
            mac_pdus(t1.data(), 268, "SCH/F", false);
            cell_packet();
        } else {
            frag_abandon(tk);
        }
    } else {
        // two half slots (SCH/HD, BNCH; STCH on a traffic timeslot)
        for (int h = 0; h < 2; h++) {
            if (decode_block(soft + (h ? 282 : 14), 216, 124, 101, scramb_, t1)) {
                if (slot_reported()) report_valid(ctx_, Mode::TETRA, slot_start_, slot_start_ + SLOT_SAMPLES);
                mac_pdus(t1.data(), 124, traffic ? "STCH" : "SCH/HD", h == 1);
                cell_packet();
            } else if (!traffic || h == 0) {
                frag_abandon(tk);   // the second half of a stolen traffic slot is speech unless stolen too
            }
        }
        if (traffic && tslot_ && tk == tslot_ && tr_.active && own_recent()) {
            // a stolen slot is part of the transmission
            tr_.last_tch = slot_start_ + SLOT_SAMPLES;
            traffic_talker(slot_start_ + SLOT_SAMPLES);
        }
    }
}

// One full-slot TCH/S burst. The timeslot played is the first one carrying
// traffic, held while it keeps doing so (2 s); a traffic instance plays its
// own timeslot only.
void TetraReceiver::voice_slot(const uint8_t* bits) {
    int64_t t = now_ms_s();
    if (tslot_ && (tn_ != tslot_ || !own_recent())) return;
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) { voice_idle(); return; }
    if (!tslot_ && voice_tn_ != tn_ && voice_tn_ != 0 && t - voice_tn_ms_ < 2000) return;
    voice_tn_ = tn_;
    voice_tn_ms_ = t;
    std::string who = "Timeslot " + std::to_string(tn_) +
                      (tslot_ && tr_.source ? " (SSI " + ssi_str(tr_.source) + ")"
                                            : (last_call_.empty() || tslot_ ? "" : " (" + last_call_ + ")"));
    if (!TetraCodec::available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": ACELP codec not installed (README: Digital voice codecs)");
        return;
    }
    if (t - encrypted_ms_ < 60000) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": air-interface encryption in use, muted");
        return;
    }
    if (tn_ >= 1 && tn_ <= 4 && slot_e2e_[tn_]) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": end-to-end encrypted, muted");
        return;
    }
    uint8_t t4[432];
    std::copy(bits + 14, bits + 230, t4);
    std::copy(bits + 282, bits + 498, t4 + 216);
    tetra_scramble(t4, 432, scramb_);
    if (!codec_) codec_ = std::make_unique<TetraCodec>();
    std::vector<float> pcm;
    if (ctx_.voice_channel && voice_ch_sent_ != tn_ - 1) {
        voice_ch_sent_ = tn_ - 1;
        ctx_.voice_channel(voice_ch_sent_);
    }
    codec_->feed_slot(t4, pcm);
    last_tch_ms_ = t;
    codec_flushed_ = false;
    if (!pcm.empty() && ctx_.voice) ctx_.voice(Mode::TETRA, pcm.data(), pcm.size());
    if (ctx_.voice_state) ctx_.voice_state(who + ": playing (ACELP)");
}

// No traffic for a while: push silence through the codec pipes so the end
// of the call isn't left in their buffers, then stop the codec processes
void TetraReceiver::voice_idle() {
    int64_t t = now_ms_s();
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
    // no slot sync: search until less than a window is left (each failed
    // search drops a slot) - the backlog stays bounded whatever the block size
    while (slot_start_ < 0) {
        const int64_t base = hist_base_;
        search();
        if (slot_start_ < 0 && hist_base_ == base) return;   // not enough samples yet
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
            if (tr_.active) traffic_end(kp::Call::ENDED, tr_.last_tch);
            for (int i = 0; i < 5; i++) frag_[i] = Frag();
            return;
        }
        traffic_tick();
        trunk_tick();
        stats_fact(false);
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
