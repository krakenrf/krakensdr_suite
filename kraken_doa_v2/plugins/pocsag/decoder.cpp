// POCSAG pager decoder - the example plugin (see plugins/SDK.md).
//
// POCSAG (ITU-R M.584): NRZ 2-FSK, +-4.5 kHz deviation, 512 / 1200 / 2400
// bit/s - all three rates are demodulated in parallel. A transmission is a
// >= 576-bit 1010 preamble, then batches of the sync codeword 0x7CD215D8
// followed by 8 frames x 2 codewords. Codeword (32 bits, first sent = bit
// 31): flag (0 = address, 1 = message), 20 payload bits, BCH(31,21) check
// (g = x^10+x^9+x^8+x^6+x^5+x^3+1), even parity. An address codeword holds
// the top 18 bits of the 21-bit RIC (the low 3 bits = the frame number) and
// 2 function bits; the message codewords that follow carry numeric (4-bit
// BCD, LSB first) or alphanumeric (7-bit ASCII, LSB first) text. Idle
// codeword 0x7A89C197.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double FS = 38400;          // 75 / 32 / 16 samples per bit
constexpr uint32_t SYNC = 0x7CD215D8;
constexpr uint32_t IDLE = 0x7A89C197;
constexpr uint32_t BCH_G = 0x769;     // degree 10

uint32_t bch_syndrome(uint32_t cw31) {
    for (int i = 30; i >= 10; i--)
        if (cw31 & (1u << i)) cw31 ^= BCH_G << (i - 10);
    return cw31 & 0x3FF;
}

// syndrome -> error pattern for every 1- and 2-bit error of the 31-bit word
const std::vector<uint32_t>& bch_table() {
    static const std::vector<uint32_t> t = [] {
        std::vector<uint32_t> v(1024, 0);
        for (int i = 0; i < 31; i++) {
            v[bch_syndrome(1u << i)] = 1u << i;
            for (int j = i + 1; j < 31; j++) {
                uint32_t e = (1u << i) | (1u << j);
                v[bch_syndrome(e)] = e;
            }
        }
        return v;
    }();
    return t;
}

// Corrects up to 2 bit errors in a 32-bit codeword. Returns the number of
// corrected bits, or -1 if uncorrectable. The even-parity bit catches 3-bit
// errors the BCH decoder would "correct" into the wrong word.
int correct(uint32_t& cw) {
    uint32_t c31 = cw >> 1;
    uint32_t s = bch_syndrome(c31);
    int fixed = 0;
    if (s) {
        uint32_t e = bch_table()[s];
        if (!e) return -1;
        c31 ^= e;
        fixed = __builtin_popcount(e);
    }
    bool parity_ok = ((__builtin_popcount(c31) + (cw & 1)) & 1) == 0;
    if (!parity_ok && fixed == 2) return -1;
    cw = (c31 << 1) | (__builtin_popcount(c31) & 1);
    return fixed + (parity_ok ? 0 : 1);
}

class Chain {
public:
    Chain(int baud, kp::Host& host) : baud_(baud), host_(host), cr_(static_cast<float>(FS / baud), 0.08f) {
        // post-discriminator integrate filter: half a bit
        smooth_.set_taps(kp::fir_boxcar(std::max(1, static_cast<int>(FS / baud / 2))));
    }

    void reset() {
        cr_.reset();
        smooth_.reset();
        sync_.reset();
        collecting_ = false;
        end_message();
    }

    void push(float disc_hz) {
        float y = smooth_.push(disc_hz);
        if (!cr_.push(y)) return;
        // level tracking: deviation estimate from the symbol magnitudes
        float v = cr_.symbol() - cr_.centre();
        dev_ += 0.01f * (std::fabs(v) - dev_);
        uint8_t bit = v < 0 ? 1 : 0;   // POCSAG: 1 = lower frequency
        on_bit(bit);
    }

    int baud() const { return baud_; }

private:
    int baud_;
    kp::Host& host_;
    kp::ClockRecovery cr_;
    kp::Fir<float> smooth_;
    kp::SyncWord sync_{SYNC, 32};
    float dev_ = 0;
    bool collecting_ = false, inverted_ = false;
    int nbits_ = 0, bad_ = 0;   // uncorrectable codewords in this batch
    uint32_t word_ = 0;
    int cw_index_ = 0;

    // message under assembly
    bool in_msg_ = false;
    uint32_t addr_ = 0;
    int func_ = 0;
    std::vector<uint32_t> payload_;   // 20-bit chunks
    bool damaged_ = false;             // a codeword of it was uncorrectable

    void on_bit(uint8_t bit) {
        if (!collecting_) {
            int d = sync_.push(bit);
            int di = sync_.inverted_distance();
            if (d <= 2 || di <= 2) {
                inverted_ = di < d;
                collecting_ = true;
                bad_ = 0;
                nbits_ = 0;
                word_ = 0;
                cw_index_ = 0;
                host_.fact("Bit rate", std::to_string(baud_) + " bit/s");
                host_.fact("Polarity", inverted_ ? "inverted" : "normal");
                char b[32];
                snprintf(b, sizeof b, "%.1f kHz", dev_ / 1000.0f);
                host_.fact("Deviation", b);
                host_.freq_error(cr_.centre());
            }
            return;
        }
        word_ = (word_ << 1) | (inverted_ ? bit ^ 1 : bit);
        if (++nbits_ < 32) return;
        nbits_ = 0;
        if (cw_index_ == 16) {
            // after a batch: either the next sync or the end of the transmission
            uint32_t w = word_;
            if (__builtin_popcount(w ^ SYNC) <= 2 && bad_ < 12) {
                cw_index_ = 0;
                bad_ = 0;
                return;
            }
            collecting_ = false;
            sync_.reset();
            end_message();
            return;
        }
        handle_codeword(word_, cw_index_);
        cw_index_++;
    }

    void handle_codeword(uint32_t cw, int index) {
        int fixed = correct(cw);
        if (fixed < 0) {
            bad_++;
            damaged_ = true;
            end_message();   // a lost codeword breaks the message
            return;
        }
        if (cw == IDLE) {
            end_message();
            return;
        }
        if (!(cw & 0x80000000u)) {
            end_message();
            in_msg_ = true;
            addr_ = (((cw >> 13) & 0x3FFFF) << 3) | static_cast<uint32_t>(index / 2);
            func_ = (cw >> 11) & 3;
            payload_.clear();
            damaged_ = false;
            host_.valid();
        } else if (in_msg_) {
            payload_.push_back((cw >> 11) & 0xFFFFF);
            if (payload_.size() > 400) end_message();   // runaway
        }
    }

    static std::string numeric(const std::vector<uint32_t>& p) {
        static const char* map = "0123456789*U -)(";
        std::string s;
        for (uint32_t w : p)
            for (int d = 0; d < 5; d++) {
                uint32_t nib = (w >> (16 - 4 * d)) & 0xF;
                // digits are sent LSB first
                uint32_t r = ((nib & 1) << 3) | ((nib & 2) << 1) | ((nib & 4) >> 1) | ((nib & 8) >> 3);
                s += map[r];
            }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    static std::string alpha(const std::vector<uint32_t>& p) {
        std::string s;
        uint32_t acc = 0;
        int n = 0;
        for (uint32_t w : p)
            for (int b = 19; b >= 0; b--) {
                acc |= ((w >> b) & 1) << n;   // 7-bit characters, LSB first
                if (++n == 7) {
                    if (acc) s += static_cast<char>(acc);
                    acc = 0;
                    n = 0;
                }
            }
        // trailing fill (EOT / ETX / NUL)
        while (!s.empty() && (s.back() == 4 || s.back() == 3 || s.back() == 0)) s.pop_back();
        return s;
    }

    static int printable_ratio(const std::string& s) {
        if (s.empty()) return 0;
        int ok = 0;
        for (unsigned char c : s) ok += (c >= 32 && c < 127) || c == '\n' || c == '\r';
        return 100 * ok / static_cast<int>(s.size());
    }

    void end_message() {
        if (!in_msg_) return;
        in_msg_ = false;
        char head[64];
        snprintf(head, sizeof head, "%d bit/s RIC %07u F%d", baud_, addr_, func_);
        std::string text;
        if (payload_.empty()) {
            text = std::string(head) + (damaged_ ? ": (message lost)" : ": tone only");
        } else {
            std::string a = alpha(payload_), num = numeric(payload_);
            // function 0 is usually numeric, 3 alphanumeric - but networks
            // differ, so fall back on whichever decodes as clean text
            bool use_alpha = func_ == 3 || (func_ != 0 && printable_ratio(a) >= 90);
            if (func_ == 0 && printable_ratio(a) >= 95 && a.size() > num.size() / 2 + 2) use_alpha = true;
            text = std::string(head) + (use_alpha ? " alpha: " + kp::printable(a) : " numeric: " + num);
            if (damaged_) text += " [incomplete]";
        }
        host_.event(text, 10.0);
        host_.fact("Last RIC", std::to_string(addr_));
        host_.fact("Last message", text.substr(text.find(':') + 2));
        messages_++;
        host_.fact("Messages", std::to_string(messages_));
        payload_.clear();
    }

    static inline int messages_ = 0;
};

class Pocsag : public kp::Decoder {
public:
    explicit Pocsag(kp::Host& h) : Decoder(h), lp_(kp::fir_lowpass(63, 8000, FS)), fm_(FS) {
        for (int b : {512, 1200, 2400}) chains_.emplace_back(b, host);
    }
    void process(const kp::cf* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            float f = fm_.push(lp_.push(x[i]));
            for (auto& c : chains_) c.push(f);
        }
    }
    void reset() override {
        lp_.reset();
        fm_.reset();
        for (auto& c : chains_) c.reset();
    }

private:
    kp::Fir<kp::cf> lp_;
    kp::FmDemod fm_;
    std::vector<Chain> chains_;
};

}  // namespace

KRAKEN_PLUGIN(Pocsag, {.id = "pocsag",
                       .name = "POCSAG",
                       .description = "POCSAG pagers, 512/1200/2400 bit/s (numeric + alphanumeric)",
                       .version = "1.0",
                       .sample_rate = FS,
                       .min_vfo_rate = 12500,
                       .author = "KrakenSDR example"})
