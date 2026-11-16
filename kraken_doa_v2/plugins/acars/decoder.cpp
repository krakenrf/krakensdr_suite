// ACARS (Aircraft Communications Addressing and Reporting System) on VHF:
// the ARINC 618 air / ground character-oriented link ("plain old ACARS"),
// 2400 bit/s MSK on an AM voice channel (129-137 MHz).
//
// Physical layer: double-sideband AM with carrier; the audio is MSK at 2400
// bit/s with the tones 1200 Hz (half a cycle per bit) and 2400 Hz (one whole
// cycle). Line code NRZI: a 2400 Hz bit repeats the previous bit, a 1200 Hz
// bit inverts it. Against a 2400 Hz reference the audio's phase at each bit
// boundary is therefore 0 or 180 degrees = the bit itself (a 2400 Hz bit
// keeps the phase, a 1200 Hz bit turns it by half a cycle): the bits are
// detected coherently, like BPSK - one wrong decision is one bit error, not
// an inverted rest of the block as when the tones are decided one by one.
//
// Block (ARINC 618, VHF): pre-key (1 bits), '+' '*' SYN SYN SOH, mode,
// address (7: the aircraft registration, right-aligned, '.' padded),
// technical acknowledgement, label (2), block ID, STX (ETX: no text), text
// (<= 220), ETX / ETB (more blocks follow), BCS (16 bits), DEL. Characters:
// 7-bit ASCII + odd parity, LSB first; the BCS has no parity. BCS =
// CRC-16/KERMIT (x^16 + x^12 + x^5 + 1 reflected, init 0, no xorout) over
// mode .. ETX / ETB, low byte first - measured on a real recording (the CRC
// over the block and its BCS is 0), the standard's text isn't public.
//
// Demodulator: 5.5 kHz channel filter -> envelope (AM, any carrier offset)
// -> DC (= carrier level) removed and divided out -> mixed by 1800 Hz and
// filtered (the MSK at +-600 Hz) -> DPLL bit clock from the zero crossings
// of the one-bit phase change -> each bit boundary interpolated, turned
// onto the 2400 Hz reference, phase from the squared samples (decision
// free, 180-degree ambiguity resolved by the sync's polarity) -> sign = bit,
// magnitude = its reliability. Characters with a parity error are repaired
// by flipping their least reliable bits until the BCS matches.

#include "acars_msg.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr double FS = 24000;   // 10 samples per bit
constexpr int SPB = 10;
constexpr double BAUD = 2400;
constexpr int TAB = 40;        // 1800 Hz and 600 Hz both repeat every 40 samples
constexpr int RF_TAPS = 49, BB_TAPS = 33;
constexpr int HDR = 13;        // mode, address 7, ack, label 2, block ID, STX / ETX
constexpr int MAX_TEXT = 220;
constexpr int MAX_CHARS = HDR + MAX_TEXT + 1 + 2 + 4;   // + suffix + BCS, a little margin
constexpr int MAX_BAD = 4;     // parity errors repaired per block (a few CRC trials each)
constexpr uint8_t ETX = 0x83, ETB = 0x97;   // on air, with the odd parity bit
constexpr uint8_t SOH = 0x01;
using acars::DEL;
using acars::NAK;

uint16_t crc_kermit(const uint8_t* p, int n) {
    static const std::array<uint16_t, 256> tab = [] {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            uint16_t c = static_cast<uint16_t>(i);
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint16_t c = 0;
    for (int i = 0; i < n; i++) c = (c >> 8) ^ tab[(c ^ p[i]) & 0xFF];
    return c;
}

bool odd_parity(uint8_t c) { return __builtin_popcount(c) & 1; }
int hamming8(uint8_t a, uint8_t b) { return __builtin_popcount(static_cast<unsigned>(a ^ b)); }

// '+' '*' SYN SYN SOH with their parity bits, LSB first: the last 40 bits
// before the mode character (the first two 1s of '+' run on from the pre-key)
uint64_t sync_pattern() {
    const uint8_t s[5] = {0x2B | 0x80, 0x2A, 0x16, 0x16, SOH};
    uint64_t p = 0;
    for (uint8_t c : s)
        for (int k = 0; k < 8; k++) p = (p << 1) | ((c >> k) & 1);
    return p;
}
constexpr uint64_t MASK40 = (1ull << 40) - 1;
constexpr int SYNC_ERRS = 4;

class Acars : public kp::Decoder {
public:
    explicit Acars(kp::Host& h)
        : Decoder(h), rf_(kp::fir_lowpass(RF_TAPS, 5500, FS)), bb_(kp::fir_lowpass(BB_TAPS, 1300, FS)),
          sync_(sync_pattern()) {
        for (int i = 0; i < TAB; i++) {
            const double t = i / FS;
            mix_[i] = kp::cf(std::cos(2 * M_PI * 1800 * t), -std::sin(2 * M_PI * 1800 * t));
            rot_[i] = kp::cf(std::cos(2 * M_PI * 600 * t), -std::sin(2 * M_PI * 600 * t));
        }
        host.table_columns({"Aircraft", "Flight", "Label", "Blocks", "Last text"});
        host.table_keep(3600);
        reset();
    }

    void reset() override {
        rf_.reset();
        bb_.reset();
        car_ = 0;
        cprev_ = {0, 0};
        cacc_ = {0, 0};
        n_ = 0;
        zd_.fill({});
        zi_ = 0;
        dprev_ = 0;
        phase_ = 0;
        zrprev_ = {0, 0};
        acc_ = {0, 0};
        phi_ = 0;
        sr_ = 0;
        in_frame_ = false;
        pending_.clear();
    }

    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        for (size_t i = 0; i < n; i++) {
            now_ = t0 + static_cast<double>(i) / FS;
            const kp::cf y = rf_.push(x[i]);
            // carrier offset: the carrier dominates the AM signal's average
            // phase step (the sidebands are symmetric); ~40 ms average
            cacc_ += 0.001f * (y * std::conj(cprev_) - cacc_);
            cprev_ = y;
            // envelope -> modulation (carrier level removed and divided out)
            const float a = std::abs(y);
            car_ += 0.004f * (a - car_);
            const float au = (a - car_) / (car_ + 1e-20f);
            const int ti = static_cast<int>(n_ % TAB);
            n_++;
            const kp::cf m = bb_.push(au * mix_[ti]);   // the MSK at +-600 Hz
            const kp::cf zr = m * rot_[ti];              // against 2400 Hz
            // phase change over one bit: + = 2400 Hz, - = 1200 Hz; it crosses
            // zero half a bit after a boundary between unlike bits
            const kp::cf old = zd_[zi_];
            zd_[zi_] = m;
            if (++zi_ == SPB) zi_ = 0;
            const float d = (m * std::conj(old)).imag();
            if ((d > 0) != (dprev_ > 0)) {
                const float frac = dprev_ / (dprev_ - d);
                const float pos = phase_ - (1.0f - frac) / SPB;
                phase_ -= (in_frame_ ? 0.15f : 0.5f) * (pos - 0.5f);
            }
            dprev_ = d;
            phase_ += 1.0f / SPB;
            if (phase_ >= 1.0f) {
                phase_ -= 1.0f;
                const float o = std::clamp(phase_ * SPB, 0.0f, 1.0f);   // the boundary was o samples ago
                sym_t_ = now_ - (DELAY + o) / FS;
                symbol(zr + o * (zrprev_ - zr));
            }
            zrprev_ = zr;
        }
    }

private:
    // the bit boundaries lag the input by the two filters
    static constexpr double DELAY = (RF_TAPS - 1) / 2 + (BB_TAPS - 1) / 2;

    kp::Fir<kp::cf> rf_, bb_;
    std::array<kp::cf, TAB> mix_, rot_;
    float car_ = 0;
    kp::cf cprev_, cacc_;
    uint64_t n_ = 0;
    std::array<kp::cf, SPB> zd_{};
    int zi_ = 0;
    float dprev_ = 0, phase_ = 0;
    kp::cf zrprev_;
    kp::cf acc_;
    float phi_ = 0;
    double now_ = 0, sym_t_ = 0;

    // bits -> sync -> characters
    const uint64_t sync_;
    uint64_t sr_ = 0;
    bool in_frame_ = false, inv_ = false;
    double f_start_ = 0;
    int nbits_ = 0, nchars_ = 0, nbad_ = 0;
    uint8_t cur_ = 0;
    std::array<float, 8> cur_soft_{};
    std::array<uint8_t, MAX_CHARS> ch_{};
    std::array<std::array<float, 8>, MAX_CHARS> soft_{};

    // counters, aircraft, multi-block messages
    long blocks_ = 0, downs_ = 0, ups_ = 0, repaired_blocks_ = 0;
    std::set<std::string> aircraft_;
    struct Row { std::string flight, label, text; long blocks = 0; };
    std::map<std::string, Row> rows_;
    struct Pending { std::string label, flight, text; char last_bid = 0; int blocks = 0; double t = 0; };
    std::map<std::string, Pending> pending_;

    void symbol(kp::cf z) {
        // carrier phase: the squared samples lose the bit (decision free);
        // the half angle is followed continuously so it never jumps by 180 degrees
        const float mag = std::abs(z);
        if (mag > 0) {
            const float b = in_frame_ ? 1.0f / 32 : 1.0f / 16;
            acc_ += b * (z * z / mag - acc_);
            phi_ += 0.5f * std::remainder(std::arg(acc_) - 2 * phi_, 2 * kp::PI);
            phi_ = std::remainder(phi_, 2 * kp::PI);
        }
        const float s = (z * std::polar(1.0f, -phi_)).real();
        const int bit = s > 0;
        if (!in_frame_) {
            sr_ = (sr_ << 1) | static_cast<uint64_t>(bit);
            const int e = __builtin_popcountll((sr_ ^ sync_) & MASK40);
            if (e <= SYNC_ERRS || 40 - e <= SYNC_ERRS) {
                in_frame_ = true;
                inv_ = e > SYNC_ERRS;
                f_start_ = sym_t_ - 40 / BAUD;
                nbits_ = nchars_ = nbad_ = 0;
                cur_ = 0;
            }
            return;
        }
        cur_ |= static_cast<uint8_t>((bit ^ (inv_ ? 1 : 0)) << nbits_);
        cur_soft_[nbits_] = std::fabs(s);
        if (++nbits_ < 8) return;
        nbits_ = 0;
        ch_[nchars_] = cur_;
        soft_[nchars_] = cur_soft_;
        cur_ = 0;
        if (!odd_parity(ch_[nchars_]) && ++nbad_ > MAX_BAD + 2) { in_frame_ = false; sr_ = 0; return; }
        nchars_++;
        // a suffix two characters back (its BCS is complete): check it
        const int e = nchars_ - 3;
        if (e >= HDR - 1 && (hamming8(ch_[e], ETX) <= 1 || hamming8(ch_[e], ETB) <= 1)) {
            if (try_block(e, hamming8(ch_[e], ETX) <= 1 ? ETX : ETB)) {
                in_frame_ = false;
                sr_ = 0;
                return;
            }
        }
        if (nchars_ >= MAX_CHARS) { in_frame_ = false; sr_ = 0; }
    }

    // The block ends at ch_[e] (the suffix, as `suffix`): repair parity
    // errors from the bits' reliabilities until the BCS matches
    bool try_block(int e, uint8_t suffix) {
        std::array<uint8_t, MAX_CHARS> c = ch_;
        int fixed = hamming8(c[e], suffix);
        c[e] = suffix;
        std::vector<int> bad;
        for (int j = 0; j < e; j++)
            if (!odd_parity(c[j])) bad.push_back(j);
        if (static_cast<int>(bad.size()) > MAX_BAD) return false;
        // the two least reliable bits of each bad character
        std::vector<std::array<int, 2>> weak;
        for (int j : bad) {
            std::array<int, 8> k{0, 1, 2, 3, 4, 5, 6, 7};
            std::sort(k.begin(), k.end(), [&](int a, int b) { return soft_[j][a] < soft_[j][b]; });
            weak.push_back({k[0], k[1]});
        }
        const uint16_t bcs = static_cast<uint16_t>(c[e + 1] | (c[e + 2] << 8));
        bool ok = false;
        const int nb = static_cast<int>(bad.size());
        for (int combo = 0; combo < (1 << nb) && !ok; combo++) {
            std::array<uint8_t, MAX_CHARS> t = c;
            for (int q = 0; q < nb; q++) t[bad[q]] ^= static_cast<uint8_t>(1 << weak[q][(combo >> q) & 1]);
            if (crc_kermit(t.data(), e + 1) == bcs) {
                c = t;
                fixed += nb;
                ok = true;
            }
        }
        // no parity error: a bit error may be in the BCS itself (no parity) -
        // its three least reliable bits, one at a time
        if (!ok && nb == 0) {
            const uint16_t crc = crc_kermit(c.data(), e + 1);
            std::array<int, 16> k{};
            for (int i = 0; i < 16; i++) k[i] = i;
            auto rel = [&](int i) { return soft_[e + 1 + i / 8][i % 8]; };
            std::partial_sort(k.begin(), k.begin() + 3, k.end(), [&](int a, int b) { return rel(a) < rel(b); });
            for (int q = 0; q < 3 && !ok; q++)
                if ((bcs ^ (1u << k[q])) == crc) {
                    fixed++;
                    ok = true;
                }
        }
        if (!ok) return false;
        acars::Block b;
        acars::parse(c.data(), e + 1, b);
        // the block's samples: from '+' (the sync that proved it) to the end
        // of the BCS; the carrier offset of this transmitter
        const double end = sym_t_;
        const float off = std::arg(cacc_) * static_cast<float>(FS / (2 * M_PI));
        on_block(b, fixed, f_start_, end, off);
        return true;
    }

    // --- reporting ---------------------------------------------------------

    void talker(const std::string& id, const std::string& label, double start, double end, float off) {
        kp::Talker t;
        t.id = id.substr(0, 32);
        t.label = label.substr(0, 64);
        t.end_s = end;
        t.start_s = std::max(start, end - 0.24);   // packets <= 0.25 s: the block's last part
        if (!(t.end_s > t.start_s)) return;
        t.packet = true;
        t.freq_hz = off;       // this transmitter's AM carrier
        t.bw_hz = 7000;        // carrier +- the 1200 / 2400 Hz tones
        t.avg_s = 10;          // blocks come seconds to minutes apart
        t.keep_s = 1800;
        host.talker(t);
    }

    // repaired: bits repaired by the parity / BCS search
    void on_block(const acars::Block& b, int repaired, double start, double end, float off) {
        host.valid(start, end);
        blocks_++;
        if (repaired) repaired_blocks_++;
        (b.down ? downs_ : ups_)++;

        const std::string reg = acars::registration(b.addr);
        const std::string lab = acars::label_text(b.label);
        const char* lname = acars::label_name(b.label);
        std::string msn, flight, body = b.text;
        if (b.down) acars::split_downlink(b.text, msn, flight, body);
        std::string net, iata;
        const bool sq = !b.down && acars::squitter(b, net, iata);

        // ID separation: a downlink is the aircraft's transmission (its
        // registration); an uplink the ground station's, which names itself
        // only in its squitters - other uplinks get no talker
        std::string key;
        if (b.down && !reg.empty()) {
            key = reg;
            aircraft_.insert(reg);
            talker(reg, flight.empty() ? "label " + lab : kp::printable(flight), start, end, off);
        } else if (sq) {
            key = "GS " + iata;
            talker(key, net + " ground station", start, end, off);
        } else if (!b.down && !reg.empty()) {
            key = reg;   // the aircraft an uplink is addressed to: its row
        }

        // the event: direction, aircraft / flight, label, block, text
        std::string ev = b.down ? "↓ " : "↑ ";
        if (sq) ev += net + " ground station " + iata + " · squitter";
        else ev += (reg.empty() ? std::string("(all)") : reg) + (flight.empty() ? "" : " " + kp::printable(flight));
        if (!sq) ev += " · " + lab + (*lname ? std::string(" ") + lname : "");
        if (b.bid > ' ' && b.bid < DEL) ev += std::string(" · blk ") + b.bid;
        if (b.ack != NAK && b.ack > ' ' && b.ack < DEL) ev += std::string(" · ack ") + b.ack;
        if (!msn.empty()) ev += " · " + msn;
        const std::string txt = acars::one_line(body);
        if (!txt.empty()) ev += ": " + txt;
        if (b.etb) ev += " …";
        if (repaired) ev += " (" + std::to_string(repaired) + (repaired == 1 ? " bit" : " bits") + " repaired)";
        // blocks without text (acknowledgements) only with "Log all messages";
        // a retransmission (no acknowledgement came) once in 30 s
        if (!b.text.empty() || host.verbose()) host.event(ev, 30);

        if (host.raw_wanted()) {
            char hdr[64];
            snprintf(hdr, sizeof hdr, "ACARS %s %c ", b.down ? "DL" : "UL", b.mode > ' ' && b.mode < DEL ? b.mode : '?');
            host.raw(std::string(hdr) + kp::printable(b.addr) + " " + (b.ack == NAK ? std::string("NAK") : kp::printable(std::string(1, b.ack))) +
                     " " + lab + " " + kp::printable(std::string(1, b.bid)) + (b.etb ? " ETB " : " ETX ") + kp::printable(b.text));
        }

        multi_block(b, reg, flight, body);
        if (!key.empty()) row(key, b, sq ? net + " " + iata : kp::printable(flight), lab, txt, off);

        host.fact("Blocks", std::to_string(blocks_) + " (" + std::to_string(downs_) + " down, " + std::to_string(ups_) + " up)");
        if (repaired_blocks_) host.fact("Repaired blocks", std::to_string(repaired_blocks_));
        host.fact("Aircraft heard", std::to_string(aircraft_.size()));
        if (b.down && !reg.empty()) {
            host.fact("Last aircraft", reg + (flight.empty() ? "" : " (" + kp::printable(flight) + ")"));
        }
        if (sq) host.fact("Ground station", net + " " + iata);
        host.fact("Last label", lab + (*lname ? std::string(" (") + lname + ")" : ""));
        host.fact("Last text", txt.empty() ? "-" : txt.substr(0, 200));
    }

    void row(const std::string& key, const acars::Block& b, const std::string& flight, const std::string& lab,
             const std::string& txt, float off) {
        Row& r = rows_[key];
        r.blocks++;
        if (!flight.empty()) r.flight = flight;
        r.label = lab;
        if (!txt.empty()) r.text = txt;
        host.table_row(key, {key, r.flight, r.label, std::to_string(r.blocks), r.text.substr(0, 200)});
        char o[32];
        snprintf(o, sizeof o, "%+.2f kHz", off / 1000.0f);
        std::vector<std::pair<std::string, std::string>> det = {
            {"Last block", std::string(b.down ? "downlink" : "uplink") + ", mode " + kp::printable(std::string(1, b.mode)) +
                               ", block " + kp::printable(std::string(1, b.bid))},
            {"Carrier offset", o}};
        if (!r.text.empty()) det.push_back({"Last text", r.text.substr(0, 400)});
        host.table_detail(key, det);
        if (rows_.size() > 2000) rows_.erase(rows_.begin());   // a long run: bounded
    }

    // A message of several blocks (ETB on all but the last): put together
    // and reported whole when its last block (ETX) arrives
    void multi_block(const acars::Block& b, const std::string& reg, const std::string& flight, const std::string& body) {
        const double t = host.time();
        for (auto it = pending_.begin(); it != pending_.end();)
            it = t - it->second.t > 120 ? pending_.erase(it) : std::next(it);
        const std::string k = std::string(b.down ? "D" : "U") + b.addr + b.label;
        auto it = pending_.find(k);
        if (b.etb) {
            Pending& p = pending_[k];
            if (p.blocks == 0) { p.label = b.label; p.flight = flight; }
            if (p.blocks == 0 || b.bid != p.last_bid) {   // a retransmitted block once
                p.text += body;
                p.blocks++;
                p.last_bid = b.bid;
            }
            p.t = t;
            return;
        }
        if (it == pending_.end()) return;
        Pending p = it->second;
        pending_.erase(it);
        if (b.bid == p.last_bid) return;
        p.text += body;
        p.blocks++;
        std::string ev = std::string(b.down ? "↓ " : "↑ ") + (reg.empty() ? std::string("(all)") : reg) +
                         (p.flight.empty() ? "" : " " + kp::printable(p.flight)) + " · " + acars::label_text(p.label) +
                         " · message of " + std::to_string(p.blocks) + " blocks: " + acars::one_line(p.text);
        host.event(ev, 30);
    }
};

}  // namespace

KRAKEN_PLUGIN(Acars, {.id = "acars",
                      .name = "ACARS (VHF 2400 bit/s)",
                      .category = "Aviation",
                      .description = "Aircraft datalink on VHF AM (129-137 MHz, ARINC 618): 2400 bit/s MSK, "
                                     "registrations, flight IDs, labels, message text; a DoA per aircraft",
                      .version = "1.0",
                      .sample_rate = FS,
                      .min_vfo_rate = 12500,
                      .author = "KrakenRF",
                      .talkers = true})
