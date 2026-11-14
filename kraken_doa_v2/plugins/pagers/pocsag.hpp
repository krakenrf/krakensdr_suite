// POCSAG (ITU-R M.584): NRZ 2-FSK, +-4.5 kHz deviation, 512 / 1200 / 2400
// bit/s - all three rates are demodulated in parallel (a Chain each). A
// transmission is a >= 576-bit 1010 preamble, then batches of the sync
// codeword 0x7CD215D8 followed by 8 frames x 2 codewords. Codeword (32 bits,
// first sent = bit 31): flag (0 = address, 1 = message), 20 payload bits,
// BCH(31,21) check, even parity (bch.hpp). An address codeword holds the top
// 18 bits of the 21-bit RIC (the low 3 bits = the frame number) and 2
// function bits; the message codewords that follow carry numeric (4-bit BCD,
// LSB first) or alphanumeric (7-bit ASCII, LSB first) text. Idle codeword
// 0x7A89C197.
#pragma once

#include "bch.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "pages.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pagers {

class PocsagChain {
public:
    static constexpr uint32_t SYNC = 0x7CD215D8;
    static constexpr uint32_t IDLE = 0x7A89C197;

    PocsagChain(int baud, double fs, kp::Host& host, Pages& pages)
        : baud_(baud), host_(host), pages_(pages), cr_(static_cast<float>(fs / baud), 0.08f) {
        // post-discriminator integrate filter: half a bit
        smooth_.set_taps(kp::fir_boxcar(std::max(1, static_cast<int>(fs / baud / 2))));
    }

    void reset() {
        cr_.reset();
        smooth_.reset();
        sync_.reset();
        collecting_ = false;
        end_message();
    }

    // t = Host::time() of this sample (the batches are reported with their samples)
    void push(float disc_hz, double t) {
        t_ = t;
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
    Pages& pages_;
    kp::ClockRecovery cr_;
    kp::Fir<float> smooth_;
    kp::SyncWord sync_{SYNC, 32};
    float dev_ = 0;
    bool collecting_ = false, inverted_ = false;
    int nbits_ = 0, bad_ = 0;   // uncorrectable codewords in this batch
    uint32_t word_ = 0;
    int cw_index_ = 0;
    double t_ = 0, batch_t0_ = 0;   // now; the batch's sync word began

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
                batch_t0_ = t_ - 32.0 / baud_;
                bad_ = 0;
                nbits_ = 0;
                word_ = 0;
                cw_index_ = 0;
                char b[96];
                snprintf(b, sizeof b, "%d bit/s%s, deviation %.1f kHz", baud_, inverted_ ? ", inverted" : "",
                         dev_ / 1000.0f);
                host_.fact("POCSAG", b);
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
            static const bool dump = getenv("POCSAG_CW") != nullptr;
            if (dump)
                fprintf(stderr, "POCSAG_CW %.3f s %d bit/s next sync? %08X (%d bits off)\n", t_, baud_, w,
                        __builtin_popcount(w ^ SYNC));
            if (__builtin_popcount(w ^ SYNC) <= 2 && bad_ < 12) {
                cw_index_ = 0;
                bad_ = 0;
                batch_t0_ = t_ - 32.0 / baud_;
                return;
            }
            collecting_ = false;
            sync_.reset();
            end_message();
            return;
        }
        handle_codeword(word_, cw_index_);
        cw_index_++;
        // a batch (sync + 16 codewords) with nearly every codeword good: POCSAG,
        // these samples (Host::valid(start, end) - the Digital squelch)
        if (cw_index_ == 16 && bad_ <= 2) host_.valid(batch_t0_, t_);
    }

    void handle_codeword(uint32_t cw, int index) {
        const uint32_t raw = cw;
        int fixed = bch_correct(cw);
        static const bool dump = getenv("POCSAG_CW") != nullptr;   // offline diagnosis: every codeword
        if (dump)
            fprintf(stderr, "POCSAG_CW %.3f s %d bit/s #%02d %08X -> %s %08X\n", t_, baud_, index, raw,
                    fixed < 0 ? "BAD" : (fixed ? "fixed" : "ok"), cw);
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
        char addr[48];
        snprintf(addr, sizeof addr, "RIC %07u F%d", addr_, func_);
        std::string body;
        if (payload_.empty()) {
            body = damaged_ ? ": (message lost)" : ": tone only";
        } else {
            std::string a = alpha(payload_), num = numeric(payload_);
            // function 0 is usually numeric, 3 alphanumeric - but networks
            // differ, so fall back on whichever decodes as clean text
            bool use_alpha = func_ == 3 || (func_ != 0 && printable_ratio(a) >= 90);
            if (func_ == 0 && printable_ratio(a) >= 95 && a.size() > num.size() / 2 + 2) use_alpha = true;
            body = use_alpha ? " alpha: " + kp::printable(a) : " numeric: " + num;
            if (damaged_) body += " [incomplete]";
            // text pages may hold an incident address (kraken_doa's incident map)
            if (use_alpha) host_.message("RIC " + std::to_string(addr_), kp::printable(a));
        }
        host_.event("POCSAG " + std::to_string(baud_) + " bit/s " + addr + body, 10.0);
        // the decoder data log: RIC, function, rate and the message
        // codewords' 20 data bits in hex
        if (host_.raw_wanted()) {
            std::string r = "POCSAG RIC " + std::to_string(addr_) + " F" + std::to_string(func_) + " " +
                            std::to_string(baud_);
            char hx[8];
            for (uint32_t c : payload_) {
                snprintf(hx, sizeof hx, " %05X", c & 0xFFFFF);
                r += hx;
            }
            host_.raw(r);
        }
        pages_.page(false, std::string("POCSAG ") + addr, body.substr(body.find(':') + 2));
        payload_.clear();
    }
};

}  // namespace pagers
