// V.23 AFSK 1200 bit/s asynchronous telemetry (SCADA / alarm radio modems).
//
// Physical layer (measured, see README.md): NBFM carrier, audio-frequency
// shift keying mark = 1300 Hz, space = 2100 Hz (ITU-T V.23 mode 2 tones),
// 1200 baud, continuous phase. A transmission opens with ~0.5 s of plain mark
// tone (transmitter key-up / squelch opening).
// Characters: asynchronous UART, idle = mark, start bit 0, 8 data bits LSB
// first, parity (odd on the observed links), 1 or 2 stop bits.
// Packet: 0xFF x N (byte sync) | 0x00 | L | payload | CRC16, where L counts the
// bytes from L itself through the CRC, and the CRC is CRC-16/CCITT-FALSE
// (poly 0x1021, init 0xFFFF, no reflection, xorout 0) over L + payload, sent
// high byte first. The payload format is proprietary and shown as hex.
//
// Demodulator: FM discriminator -> DC removal -> quadrature correlators for
// both tones over one bit -> three slicers with different mark/space weights
// (pre-/de-emphasis tilt) -> per-slicer DPLL bit clock -> UART deframers for
// 8O1 / 8E1 / 8N1 -> packet assembler (one damaged character repaired against
// the CRC) -> CRC. A packet found by several slicers / formats is reported
// once, with the strictest format that passed.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr double FS = 19200;   // 16 samples per bit
constexpr int SPB = 16;
constexpr float F_MARK = 1300, F_SPACE = 2100;
constexpr int MIN_LEN = 4;     // L + 1 payload byte + CRC
constexpr int MAX_LEN = 255;

enum Parity { ODD = 0, EVEN = 1, NONE = 2 };
const char* const FMT_NAME[3] = {"8O", "8E", "8N"};

uint16_t crc_ccitt_false(const uint8_t* p, int n) {
    static const std::array<uint16_t, 256> tab = [] {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            uint16_t c = static_cast<uint16_t>(i << 8);
            for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
            t[i] = c;
        }
        return t;
    }();
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) c = static_cast<uint16_t>((c << 8) ^ tab[((c >> 8) ^ p[i]) & 0xFF]);
    return c;
}

// UART deframer + packet assembler for one character format.
// Single-error repair: with a parity format, a character with one bit error
// fails its parity check, which locates it. A packet may contain one such
// character (or one with a broken stop bit); the 8 single-bit flips and the
// unchanged value are tried against the CRC. A damaged length byte gives up to
// 9 candidate packet lengths, each checked when enough bytes have arrived.
class Deframer {
public:
    explicit Deframer(Parity p) : par_(p) {}

    void reset() {
        ustate_ = 0;
        idle_ = 0;
        hunt();
    }

    // one bit from the slicer; returns true when a packet passed its CRC
    bool push(int b) {
        if (ustate_ == WAIT_MARK) {   // after a framing error: wait for idle
            if (b) {
                ustate_ = 0;
                idle_ = 1;
            }
            return false;
        }
        if (ustate_ == 0) {   // idle: wait for a start bit
            if (b) {
                if (idle_ < 1000) idle_++;
                if (pstate_ == BODY && idle_ > 20) give_up();   // gap inside a packet
                return false;
            }
            ustate_ = 1;
            sh_ = 0;
            ones_ = 0;
            return false;
        }
        int k = ustate_++;   // 1..8 data, then parity, then stop
        if (k <= 8) {
            sh_ |= b << (k - 1);
            ones_ += b;
            return false;
        }
        if (par_ != NONE && k == 9) {
            ones_ += b;
            return false;
        }
        // stop bit
        ustate_ = 0;
        int gap = idle_;
        idle_ = 0;
        bool par_ok = par_ == NONE || (ones_ & 1) == (par_ == ODD ? 1 : 0);
        bool stop_ok = b == 1;
        if (!stop_ok && pstate_ != BODY) {
            // framing error outside a packet: resynchronise on the next mark
            hunt();
            ustate_ = WAIT_MARK;
            return false;
        }
        // inside a packet the characters are back to back: a stop bit read as
        // 0 is taken as a bit error and the character marked damaged
        return on_char(static_cast<uint8_t>(sh_), par_ok && stop_ok, gap);
    }

    const uint8_t* frame() const { return buf_; }
    int frame_len() const { return len_; }
    int min_gap() const { return min_gap_; }   // idle bits between characters
    bool repaired() const { return fixed_; }
    long crc_failures() const { return crc_fail_; }
    Parity parity() const { return par_; }

private:
    enum { HUNT, LEN, BODY };
    static constexpr int WAIT_MARK = 100;
    Parity par_;
    int ustate_ = 0, idle_ = 0, sh_ = 0, ones_ = 0;
    int pstate_ = HUNT, ffrun_ = 0, n_ = 0, len_ = 0, min_gap_ = 0;
    bool fixed_ = false;
    int bad_ = -1;                 // index of the damaged character, -1 = none
    int ncand_ = 0, maxcand_ = 0;  // candidate packet lengths
    int cand_[9];
    long crc_fail_ = 0;
    uint8_t buf_[MAX_LEN + 1];

    void hunt() {
        pstate_ = HUNT;
        ffrun_ = 0;
    }

    // a complete-length packet (for some candidate) failed: count it
    void give_up() {
        if (n_ >= MIN_LEN) {
            len_ = n_;
            crc_fail_++;
        }
        hunt();
    }

    static bool crc_ok(const uint8_t* p, int len) {
        return crc_ccitt_false(p, len - 2) == ((p[len - 2] << 8) | p[len - 1]);
    }

    bool on_char(uint8_t v, bool ok, int gap) {
        switch (pstate_) {
        case HUNT: {
            bool ff = ok ? v == 0xFF : (par_ != NONE && __builtin_popcount(v) >= 7);
            bool zero = ok ? v == 0x00 : (par_ != NONE && ffrun_ >= 2 && __builtin_popcount(v) <= 1);
            if (ff) {
                if (ffrun_ < 1000) ffrun_++;
            } else if (zero && ffrun_ >= 1) {
                pstate_ = LEN;
            } else {
                ffrun_ = 0;
            }
            return false;
        }
        case LEN:
            ncand_ = 0;
            bad_ = -1;
            if (v >= MIN_LEN) cand_[ncand_++] = v;
            if (!ok) {
                if (par_ == NONE) ncand_ = 0;
                else
                    for (int i = 0; i < 8; i++) {
                        int c = v ^ (1 << i);
                        if (c >= MIN_LEN) cand_[ncand_++] = c;
                    }
                bad_ = 0;
            }
            if (!ncand_) {
                hunt();
                return false;
            }
            maxcand_ = *std::max_element(cand_, cand_ + ncand_);
            buf_[0] = v;
            n_ = 1;
            min_gap_ = gap;
            pstate_ = BODY;
            return false;
        default:   // BODY
            if (!ok) {
                if (par_ == NONE || bad_ >= 0) {   // a second damaged character
                    hunt();
                    return false;
                }
                bad_ = n_;
            }
            buf_[n_++] = v;
            if (gap < min_gap_) min_gap_ = gap;
            for (int i = 0; i < ncand_; i++)
                if (cand_[i] == n_ && check(n_)) {
                    hunt();
                    return true;
                }
            if (n_ >= maxcand_) give_up();
            return false;
        }
    }

    // CRC over a packet of length len, repairing the damaged character
    bool check(int len) {
        len_ = len;
        fixed_ = false;
        if (bad_ < 0) return crc_ok(buf_, len);
        uint8_t orig = buf_[bad_];
        if (bad_ == 0) {   // length byte: the candidate length is its repair
            buf_[0] = static_cast<uint8_t>(len);
            if (crc_ok(buf_, len)) return fixed_ = true;
        } else {
            for (int i = -1; i < 8; i++) {
                buf_[bad_] = static_cast<uint8_t>(i < 0 ? orig : orig ^ (1 << i));
                if (crc_ok(buf_, len)) return fixed_ = true;
            }
        }
        buf_[bad_] = orig;
        return false;
    }
};

// mark/space decision + bit clock + deframers
class Slicer {
public:
    explicit Slicer(float space_gain) : g_(space_gain) {
        for (int p = 0; p < 3; p++) df_.emplace_back(static_cast<Parity>(p));
    }
    void reset() {
        prev_ = 0;
        phase_ = 0;
        for (auto& d : df_) d.reset();
    }
    // returns a bitmask of deframers that completed a valid packet
    int push(float mark, float space) {
        float d = mark - g_ * space;   // > 0: mark = 1
        int got = 0;
        if ((d > 0) != (prev_ > 0)) {
            float frac = prev_ / (prev_ - d);
            float pos = phase_ - (1.0f - frac) / SPB;
            phase_ -= 0.3f * (pos - 0.5f);
        }
        prev_ = d;
        phase_ += 1.0f / SPB;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            int b = d > 0 ? 1 : 0;
            for (int i = 0; i < 3; i++)
                if (df_[i].push(b)) got |= 1 << i;
        }
        return got;
    }
    const Deframer& deframer(int i) const { return df_[i]; }

private:
    float g_;
    float prev_ = 0, phase_ = 0;
    std::vector<Deframer> df_;
};

class V23Telemetry : public kp::Decoder {
public:
    explicit V23Telemetry(kp::Host& h) : Decoder(h), lp_(kp::fir_lowpass(41, 7200, FS)), fm_(FS) {
        wm_ = kp::cf(std::cos(2 * M_PI * F_MARK / FS), -std::sin(2 * M_PI * F_MARK / FS));
        ws_ = kp::cf(std::cos(2 * M_PI * F_SPACE / FS), -std::sin(2 * M_PI * F_SPACE / FS));
        for (float g : {1.0f, 0.6f, 1.7f}) slicers_.emplace_back(g);
        reset();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key != "format") return;
        if (value == "8O") fmt_mask_ = 1;
        else if (value == "8E") fmt_mask_ = 2;
        else if (value == "8N") fmt_mask_ = 4;
        else fmt_mask_ = 7;
    }

    void reset() override {
        lp_.reset();
        fm_.reset();
        dc_ = 0;
        pm_ = ps_ = kp::cf(1, 0);
        hist_.fill({});
        msum_ = ssum_ = {0, 0};
        hi_ = 0;
        recompute_ = 0;
        absdev_ = 0;
        pending_ = false;
        for (auto& s : slicers_) s.reset();
    }

    void process(const kp::cf* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            float f = fm_.push(lp_.push(x[i]));
            dc_ += 0.0005f * (f - dc_);
            float a = f - dc_;
            absdev_ += 0.004f * (std::fabs(a) - absdev_);   // ~13 ms average of |deviation|
            kp::cf cm = a * pm_, cs = a * ps_;
            pm_ *= wm_;
            ps_ *= ws_;
            msum_ += cm - hist_[hi_][0];
            ssum_ += cs - hist_[hi_][1];
            hist_[hi_] = {cm, cs};
            if (++hi_ == SPB) hi_ = 0;
            if (++recompute_ >= 4096) {   // limit drift of oscillators and running sums
                recompute_ = 0;
                pm_ /= std::abs(pm_);
                ps_ /= std::abs(ps_);
                msum_ = ssum_ = {0, 0};
                for (auto& h : hist_) { msum_ += h[0]; ssum_ += h[1]; }
            }
            float mark = std::abs(msum_), space = std::abs(ssum_);
            for (auto& s : slicers_) {
                int got = s.push(mark, space) & fmt_mask_;
                for (int k = 0; got; k++, got >>= 1)
                    if (got & 1) candidate(s.deframer(k));
            }
        }
        if (pending_ && host.time() - pend_t_ > 0.05) emit();
        if (host.verbose()) log_crc_failures();
    }

private:
    kp::Fir<kp::cf> lp_;
    kp::FmDemod fm_;
    float dc_ = 0, absdev_ = 0;
    kp::cf pm_, ps_, wm_, ws_;
    std::array<std::array<kp::cf, 2>, SPB> hist_{};
    kp::cf msum_, ssum_;
    int hi_ = 0, recompute_ = 0;
    int fmt_mask_ = 7;
    std::vector<Slicer> slicers_;

    // packet waiting for duplicates from other slicers / formats
    bool pending_ = false;
    double pend_t_ = 0;
    std::vector<uint8_t> pend_;
    int pend_par_ = 0, pend_gap_ = 0;
    bool pend_fixed_ = false;
    float pend_dev_ = 0, pend_dc_ = 0;
    // last emitted (duplicate suppression)
    std::vector<uint8_t> last_;
    double last_t_ = -10;

    long frames_ = 0, repaired_ = 0;
    int last_par_ = ODD;
    std::map<int, long> by_len_;
    std::array<std::array<long, 3>, 3> fails_seen_{};
    double last_fail_log_ = -10;

    // debug ("Log all messages"): packets whose CRC failed - unverified data,
    // so only to the plugin log, never as events or facts
    void log_crc_failures() {
        for (size_t s = 0; s < slicers_.size(); s++)
            for (int k = 0; k < 3; k++) {
                const Deframer& d = slicers_[s].deframer(k);
                if (d.crc_failures() == fails_seen_[s][k]) continue;
                fails_seen_[s][k] = d.crc_failures();
                // only the parity format in use (the last decoded one, odd at first)
                if (!(fmt_mask_ & (1 << k)) || k != (frames_ ? last_par_ : (fmt_mask_ == 2 ? EVEN : ODD)) ||
                    host.time() - last_fail_log_ < 0.3)
                    continue;
                last_fail_log_ = host.time();
                char b[96];
                snprintf(b, sizeof b, "t=%.2f s: CRC failed, %d bytes %s, first bytes:", host.time(), d.frame_len(),
                         FMT_NAME[k]);
                std::string m = b;
                for (int i = 0; i < std::min(d.frame_len(), 16); i++) {
                    snprintf(b, sizeof b, " %02X", d.frame()[i]);
                    m += b;
                }
                host.log(m);
            }
    }

    void candidate(const Deframer& d) {
        std::vector<uint8_t> p(d.frame(), d.frame() + d.frame_len());
        double t = host.time();
        if (p == last_ && t - last_t_ < 0.3) return;   // already reported
        if (pending_ && p == pend_) {
            if (d.parity() < pend_par_) pend_par_ = d.parity();   // prefer a parity-checked format
            if (d.min_gap() > pend_gap_) pend_gap_ = d.min_gap();
            if (!d.repaired()) pend_fixed_ = false;               // a slicer got it without repair
            return;
        }
        if (pending_) emit();
        pending_ = true;
        pend_t_ = t;
        pend_ = std::move(p);
        pend_par_ = d.parity();
        pend_gap_ = d.min_gap();
        pend_fixed_ = d.repaired();
        // sine tone: mean |a| = 2/pi * peak
        pend_dev_ = absdev_ * static_cast<float>(M_PI / 2);
        pend_dc_ = dc_;
    }

    void emit() {
        pending_ = false;
        last_ = pend_;
        last_t_ = pend_t_;
        const int len = static_cast<int>(pend_.size());
        host.valid();
        frames_++;
        if (pend_fixed_) repaired_++;
        last_par_ = pend_par_;
        by_len_[len]++;

        // with parity none, a "2nd stop bit" may just be the parity bit; only
        // report stop bits for parity-checked formats
        char fmt[8];
        if (pend_par_ == NONE) snprintf(fmt, sizeof fmt, "8N1");
        else snprintf(fmt, sizeof fmt, "%s%d", FMT_NAME[pend_par_], pend_gap_ >= 1 ? 2 : 1);

        // hex of L + payload (CRC excluded)
        std::string hex;
        const int show = host.verbose() ? len - 2 : std::min(len - 2, 48);
        char b[8];
        for (int i = 0; i < show; i++) {
            snprintf(b, sizeof b, i ? " %02X" : "%02X", pend_[i]);
            hex += b;
        }
        if (show < len - 2) hex += " ...";

        char head[112];
        snprintf(head, sizeof head, "packet L=%d %s dev %.1f kHz CRC ok%s: ", len, fmt, pend_dev_ / 1000.0f,
                 pend_fixed_ ? " (1 char repaired)" : "");
        host.event(std::string(head) + hex, 1.0);

        host.fact("Packets (CRC ok)", std::to_string(frames_));
        host.fact("Repaired packets", repaired_ ? std::to_string(repaired_) : "");
        host.fact("Character format", fmt);
        char dv[32];
        snprintf(dv, sizeof dv, "%.1f kHz", pend_dev_ / 1000.0f);
        host.fact("Deviation (last packet)", dv);
        host.fact("Last packet", std::string(head).substr(7) + hex);
        std::string lens;
        for (auto& [l, c] : by_len_) {
            if (!lens.empty()) lens += ", ";
            lens += std::to_string(l) + " B x" + std::to_string(c);
            if (lens.size() > 300) break;
        }
        host.fact("Packet lengths", lens);
        host.fact("Modem", "V.23 AFSK 1300/2100 Hz, 1200 bit/s");
        host.freq_error(pend_dc_);
    }
};

}  // namespace

KRAKEN_PLUGIN(V23Telemetry, {.id = "v23_telemetry",
                             .name = "V.23 AFSK telemetry",
                             .description = "1200 bit/s V.23 AFSK (1300/2100 Hz) async telemetry packets: FF..00 L payload CRC-16, shown as hex",
                             .version = "1.0",
                             .sample_rate = FS,
                             .min_vfo_rate = 16000,
                             .author = "AI Signal Lab",
                             .auto_detect = false,
                             .options = {{"format", "Character format", "auto",
                                          "auto=Auto|8O=8 data, odd parity|8E=8 data, even parity|8N=8 data, no parity",
                                          "UART character format of the modem link"}}})
