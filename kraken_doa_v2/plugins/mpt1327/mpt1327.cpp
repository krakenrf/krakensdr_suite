// MPT1327 analogue trunking signalling (UK Radiocommunications Agency
// MPT 1327 / 1343): 1200 bit/s FFSK - a "1" is one cycle of 1200 Hz, a "0"
// one and a half cycles of 1800 Hz - on an NBFM carrier.
//
// Demodulation: two sliding one-bit (40-sample) correlators on the FM
// discriminator output, one per tone; the normalized difference of their
// energies is the soft bit at every sample. Sync: the 4 preamble bits +
// SYNC 0xC4D7 (control channel) / SYNT 0x3B28 (traffic channel) correlated
// at bit spacing. On the control channel the SYNC is the check field of the
// control channel system codeword (CCSC: 0, 15-bit system identity SYS, ...),
// which is followed by an address codeword carrying the message (Aloha,
// channel grants, acknowledgements, clear-down, broadcasts...). Every
// codeword: 48 information bits, BCH(63,48) check (last bit inverted) and an
// even parity bit; single-bit errors are corrected. Voice is analogue NBFM on
// the traffic channels the control channel assigns.

#include "mpt1327.hpp"
#include "dig_fec.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace dig {

namespace {
constexpr int FS = 48000;
constexpr int SPB = 40;            // samples per bit
constexpr float THRESHOLD = 0.2f;   // soft floor; the hard 18-of-20 bit match decides
constexpr uint32_t SYNC_CC = 0xC4D7, SYNC_TC = 0x3B28;

// MPT1327 codeword check: BCH remainder (g = x^15+x^14+x^13+x^11+x^4+x^2+1)
// with the last check bit inverted, plus even parity over all 64 bits
bool cw_ok(uint64_t cw) {
    if (__builtin_popcountll(cw) & 1) return false;
    uint64_t info = cw >> 16;
    uint64_t r = info << 15;
    for (int i = 62; i >= 15; i--)
        if ((r >> i) & 1) r ^= static_cast<uint64_t>(0xE815) << (i - 15);
    return ((r ^ 1) & 0x7FFF) == ((cw >> 1) & 0x7FFF);
}

std::string ident(int prefix, int id) {
    switch (id) {
        case 0: return "dummy";
        case 8101: return "PSTN gateway";
        case 8102: return "PABX gateway";
        case 8103: return "data network gateway";
        case 8185: return "registration";
        case 8186: return "include";
        case 8187: return "divert";
        case 8188: return "short data";
        case 8189: return "inter-prefix";
        case 8190: return "TSC";
        case 8191: return "all radios";
        default: break;
    }
    char b[24];
    snprintf(b, sizeof b, "%03d-%04d", prefix, id);
    return b;
}
}  // namespace

Mpt1327Receiver::Mpt1327Receiver(RxContext& c) : ctx_(c) {
    w12_ = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * 1200.0f / FS);
    w18_ = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * 1800.0f / FS);
    reset();
}

void Mpt1327Receiver::reset() {
    m_ = SampleBuf();
    for (auto& v : ring12_) v = {0, 0};
    for (auto& v : ring18_) v = {0, 0};
    hpos_ = 0;
    c12_ = c18_ = {0, 0};
    r12_ = r18_ = {1, 0};
    lvl12_ = lvl18_ = 1.0f;
    n_ = 0;
    pk_ = Peak();
    jobs_.clear();
    sys_ = 0xFFFFFFFF;
    last_aloha_.clear();
}

bool Mpt1327Receiver::codeword(int64_t first_end, uint64_t* info) {
    uint64_t cw = 0;
    for (int j = 0; j < 64; j++) {
        int64_t at = first_end + static_cast<int64_t>(j) * SPB;
        if (!m_.has(at)) return false;
        cw = (cw << 1) | (m_.at(at) > 0 ? 1u : 0u);
    }
    if (!cw_ok(cw)) {
        bool fixed = false;
        for (int b = 0; b < 64 && !fixed; b++)
            if (cw_ok(cw ^ (1ULL << b))) { cw ^= 1ULL << b; fixed = true; }
        if (!fixed) return false;
    }
    *info = cw >> 16;
    return true;
}

void Mpt1327Receiver::address(uint64_t info, bool control) {
    auto f = [info](int pos, int len) { return static_cast<int>((info >> (48 - pos - len)) & ((1ULL << len) - 1)); };
    if (!f(0, 1)) return;   // not an address codeword
    Report& r = *ctx_.report;
    const int prefix = f(1, 7), id1 = f(8, 13), type = f(21, 9);
    const bool verbose = ctx_.opts->verbose;
    const double chatter = verbose ? 5.0 : 1e9;   // effectively verbose-only
    std::string to = ident(prefix, id1);
    char h[96];
    if (type < 256) {   // GTC: go to channel
        int chan = f(23, 10), id2 = f(33, 13);
        std::string d = ident(prefix, id2) + " -> " + to + " on channel " + std::to_string(chan);
        r.event(Mode::MPT1327, "Grant: " + d, 3.0);
        r.set(Mode::MPT1327, "Last grant", d);
        return;
    }
    switch (type) {
        case 256: case 257: case 258: case 259: case 260: case 261: case 262: {
            static const char* an[7] = {"all calls", "standard data excluded", "simple calls excluded",
                                        "emergency calls only", "emergency or registration",
                                        "registration excluded", "fallback mode"};
            std::string a = std::string("Aloha (") + an[type - 256] + ")";
            r.set(Mode::MPT1327, "Access", a);
            if (a != last_aloha_) {
                last_aloha_ = a;
                r.event(Mode::MPT1327, "Random access: " + a, 60.0);
            }
            if (verbose) r.event(Mode::MPT1327, "ALOHA to " + to, chatter);
            break;
        }
        case 264: case 265: case 266: case 267: case 268: case 269: case 270: case 271: {
            static const char* kn[8] = {"ACK", "ACKI (more to follow)", "ACKQ (call queued)", "ACKX (rejected)",
                                        "ACKV (called unit unavailable)", "ACKE (emergency)", "ACKT (try given address)",
                                        "ACKB (call back)"};
            std::string e = std::string(kn[type - 264]) + ": " + ident(prefix, f(30, 13)) + " -> " + to;
            r.event(Mode::MPT1327, e, (type == 266 || type == 267 || type == 269) ? 3.0 : chatter);
            break;
        }
        case 272: r.event(Mode::MPT1327, "AHOY (availability check) to " + to, chatter); break;
        case 274: r.event(Mode::MPT1327, "AHYX (cancel alert) to " + to, chatter); break;
        case 277: r.event(Mode::MPT1327, "AHYP (presence check) to " + to, chatter); break;
        case 278:
            snprintf(h, sizeof h, " status %d", f(43, 5));
            r.event(Mode::MPT1327, "Status message: " + ident(prefix, f(30, 13)) + " -> " + to + h, 3.0);
            break;
        case 279: r.event(Mode::MPT1327, "Short data message to " + to, 3.0); break;
        case 280:
            r.set(Mode::MPT1327, "Control channel", std::to_string(f(15, 10)));
            break;
        case 281: r.event(Mode::MPT1327, "Call maintenance on channel " + std::to_string(f(15, 10)), chatter); break;
        case 282: {
            std::string e = "Clear down: traffic channel " + std::to_string(f(1, 10)) + ", back to control channel " +
                            std::to_string(f(11, 10));
            r.event(Mode::MPT1327, e, 3.0);
            break;
        }
        case 283: r.event(Mode::MPT1327, "Move to channel " + std::to_string(f(15, 10)) + ": " + to, 3.0); break;
        case 284: {   // BCAST: system parameters
            static const char* sd[6] = {"announce control channel", "withdraw control channel",
                                        "call maintenance parameters", "registration parameters",
                                        "adjacent site control channel", "vote now advice"};
            int sysdef = f(1, 5), sys = f(6, 15);
            snprintf(h, sizeof h, "System broadcast: %s (SYS 0x%04X)", sysdef < 6 ? sd[sysdef] : "SYSDEF ?", sys);
            r.event(Mode::MPT1327, h, verbose ? 5.0 : 120.0);
            if ((sysdef == 4 || sysdef == 5) && static_cast<uint32_t>(sys) != sys_) {
                snprintf(h, sizeof h, "0x%04X", sys);
                r.set(Mode::MPT1327, "Neighbour site SYS", h);
            }
            break;
        }
        default:
            if (type >= 288 && type <= 303) r.event(Mode::MPT1327, "Single address data message to " + to, 3.0);
            else if (type >= 304 && type <= 319) r.event(Mode::MPT1327, "Data message header to " + to, 3.0);
            else if (type >= 320 && type <= 335) r.event(Mode::MPT1327, "GTT (go to transaction) " + to, chatter);
            else if (verbose) {
                snprintf(h, sizeof h, "Message type %d to ", type);
                r.event(Mode::MPT1327, h + to, 5.0);
            }
            break;
    }
    (void)control;
}

void Mpt1327Receiver::decode_slot(int64_t sync_end, int which) {
    Report& r = *ctx_.report;
    uint64_t ccsc = 0, addr = 0;
    bool valid = false, addr_ok = false;
    if (which == 0) {
        // the CCSC ends with the SYNC: it starts 44 bits before the pattern
        if (!codeword(sync_end - 63 * SPB, &ccsc)) return;
        if ((ccsc >> 47) & 1) return;
        uint32_t sys = static_cast<uint32_t>((ccsc >> 32) & 0x7FFF);
        if (sys != sys_) {
            sys_ = sys;
            char b[64];
            snprintf(b, sizeof b, "Control channel, system identity 0x%04X", sys);
            r.event(Mode::MPT1327, b, 60.0);
        }
        char b[32];
        snprintf(b, sizeof b, "0x%04X (%u)", sys, sys);
        r.set(Mode::MPT1327, "System identity (SYS)", b);
        r.set(Mode::MPT1327, "Channel type", "Control channel");
        valid = true;
    }
    if (codeword(sync_end + SPB, &addr)) {
        valid = addr_ok = true;
        if (which == 1) r.set(Mode::MPT1327, "Channel type", "Traffic channel (voice is analogue NBFM)");
        address(addr, which == 0);
    }
    // the slot's samples: the CCSC (64 bits, ending with the SYNC) or preamble + SYNT, then the address codeword
    if (valid)
        report_valid(ctx_, Mode::MPT1327, sync_end - (which == 0 ? 64 : 20) * static_cast<int64_t>(SPB),
                     sync_end + (addr_ok ? 65 : 1) * static_cast<int64_t>(SPB));
}

void Mpt1327Receiver::process(const float* d, size_t n) {
    // sync templates (+1 = 1200 Hz = bit 1)
    static const auto tmpl = [] {
        std::array<std::array<float, 20>, 2> t{};
        const uint32_t w[2] = {SYNC_CC, SYNC_TC};
        for (int k = 0; k < 2; k++) {
            uint32_t v = (0xAu << 16) | w[k];
            for (int i = 0; i < 20; i++) t[k][i] = ((v >> (19 - i)) & 1) ? 1.0f : -1.0f;
        }
        return t;
    }();
    for (size_t i = 0; i < n; i++) {
        // one-bit sliding correlators (absolute phase reference)
        std::complex<float> p12 = d[i] * r12_, p18 = d[i] * r18_;
        r12_ *= w12_;
        r18_ *= w18_;
        if ((n_ & 1023) == 0) { r12_ /= std::abs(r12_); r18_ /= std::abs(r18_); }
        size_t k = hpos_;
        c12_ += p12 - ring12_[k];
        c18_ += p18 - ring18_[k];
        ring12_[k] = p12;
        ring18_[k] = p18;
        hpos_ = (hpos_ + 1) % SPB;
        if ((n_ % 4096) == 0) {   // refresh the sums (rounding drift)
            c12_ = c18_ = {0, 0};
            for (int j = 0; j < SPB; j++) { c12_ += ring12_[j]; c18_ += ring18_[j]; }
        }
        n_++;
        // each tone against its own running level: the radios' pre-emphasis
        // makes 1800 Hz louder than 1200 Hz
        float e12 = std::sqrt(std::norm(c12_)), e18 = std::sqrt(std::norm(c18_));
        lvl12_ += 0.0005f * (e12 - lvl12_);
        lvl18_ += 0.0005f * (e18 - lvl18_);
        float a = e12 / (lvl12_ + 1e-9f), b = e18 / (lvl18_ + 1e-9f);
        m_.push((a - b) / (a + b + 1e-9f));
        // sync at bit spacing
        const int64_t last = m_.end() - 1;
        const int64_t first = last - 19 * SPB;
        if (first < m_.begin()) continue;
        // sync: hard bit decisions, at most 2 of the 20 bits wrong; the soft
        // correlation picks the best sample within a bit
        float best = 0;
        int which = 0;
        for (int w = 0; w < 2; w++) {
            float c = 0;
            int errs = 0;
            for (int b = 0; b < 20; b++) {
                float v = m_.at(first + b * SPB);
                c += tmpl[w][b] * v;
                errs += (v > 0) != (tmpl[w][b] > 0);
            }
            c /= 20.0f;
            if (errs <= 2 && c > best) { best = c; which = w; }
        }
        if (pk_.active) {
            pk_.age++;
            if (best > pk_.best) { pk_.best = best; pk_.idx = last; pk_.which = which; }
            if (pk_.age >= SPB) {
                pk_.active = false;
                jobs_.push_back({pk_.idx, pk_.which});
            }
        } else if (best > THRESHOLD) {
            pk_.active = true;
            pk_.best = best;
            pk_.idx = last;
            pk_.which = which;
            pk_.age = 0;
        }
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!m_.has(it->end + 64 * SPB)) { ++it; continue; }
        decode_slot(it->end, it->which);
        it = jobs_.erase(it);
    }
    int64_t keep = m_.end() - 300 * SPB;
    for (const auto& j : jobs_) keep = std::min(keep, j.end - 80 * SPB);
    if (keep > m_.begin() + 16384) m_.trim_before(keep);
}

}  // namespace dig
