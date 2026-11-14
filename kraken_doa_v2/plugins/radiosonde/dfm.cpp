// Graw DFM (family "DFM").
//
// 2-FSK, 2500 chips/s Manchester, continuous: a frame every 224 ms = the sync
// (Manchester of 0x45CF; both FSK polarities occur) + 264 bits in three
// bit-interleaved Hamming(8,4) blocks - config (7 nibbles: a channel number
// and its 24-bit value), data 1 and data 2 (12 payload nibbles + a packet id
// each). The config channels cycle: N fast channels 0..N-1 every cycle, then
// one slow channel counting up from N to a maximum M and wrapping; channel M
// carries the serial number, other channels the sensor readings, battery and
// CPU temperature. The data packets 0..8 of about one second make a GPS fix.
// Layouts and the channel structure from ra-firmware (DF9DQ, BSD-3-Clause);
// the temperature fit and the model names are the old plugin's (README).

#include "families.hpp"

#include <algorithm>
#include <cstdio>

namespace sonde {

namespace {

const char* DFM_SYNC = "10011010100110010101101001010101";
// Hamming(8,4) codeword of each nibble, MSB first = d3 d2 d1 d0 p0 p1 p2 p3
const uint8_t DFM_HAM[16] = {0x00, 0x1E, 0x2D, 0x33, 0x4B, 0x55, 0x66, 0x78, 0x87, 0x99, 0xAA, 0xB4, 0xCC, 0xD2, 0xE1, 0xFF};
constexpr int NSYM = 528;                    // 264 bits after the sync
constexpr double FRAME_S = 0.224;
constexpr double FRAME_SAMPLES = FRAME_S * FS;
constexpr int NCH = 16;

// one block: L codewords, bit n of the block is bit (n div L) (from the MSB)
// of codeword (n mod L); every codeword decoded by soft correlation with all
// 16, the block rejected if the choice differs from the hard bits in > 1 bit
bool decode_block(const float* soft, int L, uint8_t* nib) {
    for (int m = 0; m < L; m++) {
        unsigned hard = 0;
        float sb[8];
        for (int j = 0; j < 8; j++) {
            sb[j] = soft[j * L + m];
            hard |= (sb[j] > 0 ? 1u : 0u) << (7 - j);
        }
        int best = 0;
        float bs = -1e30f;
        for (int k = 0; k < 16; k++) {
            float s = 0;
            for (int j = 0; j < 8; j++) s += (DFM_HAM[k] >> (7 - j) & 1) ? sb[j] : -sb[j];
            if (s > bs) {
                bs = s;
                best = k;
            }
        }
        if (__builtin_popcount(DFM_HAM[best] ^ hard) > 1) return false;
        nib[m] = static_cast<uint8_t>(best);
    }
    return true;
}

// a 24-bit config value as the floating reading: low 20 bits / 2^(top 4 bits)
double ch_float(uint32_t v) { return static_cast<double>(v & 0xFFFFF) / static_cast<double>(1u << (v >> 20)); }

class Dfm : public Family {
public:
    Dfm() {
        FskParams p;
        p.rate = 2500;
        p.sync = DFM_SYNC;
        p.max_err = 2;
        p.nsym = NSYM;
        p.phases = 8;
        rx_.init(p);
        reset();
    }

    void reset() override {
        rx_.reset();
        dup_.reset();
        have_slot_ = false;
        hist_.clear();
        prev0_ = INT64_MIN;
        n_last_ = -1;
        n_count_ = 0;
        N_ = -1;
        clear_structure();
        for (auto& v : val_) v = 0;
        for (auto& h : have_) h = false;
        serial_known_ = false;
        serial_ = 0;
        pend_ = 0;
        pend_count_ = 0;
        clear_cycle();
        have_date_ = false;
        prev_utc_ = NAN;
    }

    void process(const float* narrow, const float*, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        caps_.clear();
        rx_.process(narrow, n, idx, caps_);
        for (const auto& c : caps_) frame(c, ctx, out);
    }

private:
    FskRx rx_;
    std::vector<FskCapture> caps_;
    DupWindow dup_;

    // frame slots (a frame every 224 ms)
    bool have_slot_ = false;
    double slot_pos_ = 0;
    int64_t slot_ = 0;
    std::deque<std::pair<int64_t, int>> hist_;   // decoded config channels by slot
    // the channel structure
    int64_t prev0_ = INT64_MIN;                  // slot of the last channel 0
    int n_last_ = -1, n_count_ = 0;              // N measurements
    int N_ = -1, M_ = -1;
    struct Slow { int64_t slot; int ch, c1, c6; };
    Slow prev_slow_{INT64_MIN, -1, -1, -1};
    int cand_ = -1, cand_c1_ = -1;
    // the channels' latest values (24 bit) and the serial halves
    uint32_t val_[NCH] = {};
    bool have_[NCH] = {};
    uint16_t half_[NCH][2] = {};
    bool have_half_[NCH][2] = {};
    bool serial_known_ = false, serial_bcd_ = false;
    uint32_t serial_ = 0;
    uint32_t pend_ = 0;                          // a different number waiting for its repeat
    int pend_count_ = 0;
    uint32_t bcd_last_ = 0;
    int bcd_count_ = 0;
    // the GPS packets of one cycle
    struct FrameInfo { float centre; double pos_sync, pos_end; std::string nib; };
    struct Pk { bool have; int32_t i32; uint16_t i16; };
    Pk pk_[9] = {};
    bool cyc_any_ = false;
    int last_id_ = -1;
    double last_pk_pos_ = 0;
    FrameInfo cyc_frame_;
    // date + time
    bool have_date_ = false;
    int y_ = 0, mo_ = 0, d_ = 0, hh_ = 0, mi_ = 0;
    double prev_utc_ = NAN;

    void clear_structure() {
        M_ = -1;
        prev_slow_ = {INT64_MIN, -1, -1, -1};
        cand_ = cand_c1_ = -1;
        for (auto& h : have_half_) h[0] = h[1] = false;
        bcd_count_ = 0;
    }
    void clear_cycle() {
        for (auto& p : pk_) p.have = false;
        cyc_any_ = false;
        last_id_ = -1;
    }

    void frame(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        float soft[NSYM / 2];
        for (int i = 0; i < NSYM / 2; i++) soft[i] = c.sym[static_cast<size_t>(2 * i + 1)] - c.sym[static_cast<size_t>(2 * i)];
        uint8_t cfg[7], d1[13], d2[13];
        const bool okc = decode_block(soft, 7, cfg), ok1 = decode_block(soft + 56, 13, d1), ok2 = decode_block(soft + 160, 13, d2);
        if (!okc && !ok1 && !ok2) return;
        if (dup_.near(c.pos0, 0.1)) return;
        dup_.accept(c.pos0);

        // this frame's slot
        if (!have_slot_) {
            have_slot_ = true;
            slot_ = 0;
        } else {
            slot_ += std::llround((c.pos_sync - slot_pos_) / FRAME_SAMPLES);
        }
        slot_pos_ = c.pos_sync;

        FrameInfo fi{c.centre, c.pos_sync, c.pos_end, ""};
        if (ctx.raw) {
            static const char* H = "0123456789ABCDEF";
            for (int i = 0; i < 7; i++) fi.nib += okc ? H[cfg[i]] : '0';
            fi.nib += ' ';
            for (int i = 0; i < 13; i++) fi.nib += ok1 ? H[d1[i]] : '0';
            fi.nib += ' ';
            for (int i = 0; i < 13; i++) fi.nib += ok2 ? H[d2[i]] : '0';
        }
        if (okc) config(cfg);
        if (ok1) packet(d1, fi, c, ctx, out);
        if (ok2) packet(d2, fi, c, ctx, out);
    }

    // --- config channels ------------------------------------------------------------
    void config(const uint8_t* c) {
        const int ch = c[0];
        const uint32_t v = static_cast<uint32_t>(c[1]) << 20 | c[2] << 16 | c[3] << 12 | c[4] << 8 | c[5] << 4 | c[6];
        val_[ch] = v;
        have_[ch] = true;
        if (c[1] == 0xC && c[6] <= 1) {
            half_[ch][c[6]] = static_cast<uint16_t>(v >> 4 & 0xFFFF);
            have_half_[ch][c[6]] = true;
        }
        hist_.emplace_back(slot_, ch);
        while (hist_.size() > 48) hist_.pop_front();

        if (ch == 0) {
            measure_n();
            prev0_ = slot_;
        } else if (N_ > 0 && prev0_ != INT64_MIN && slot_ - prev0_ == N_ && ch >= N_) {
            slow(ch, c[1], c[6]);
        }
        if (M_ >= 0 && ch == M_) serial_update();
    }

    // N = the number of fast channels, from the spacing of channel 0
    void measure_n() {
        if (prev0_ == INT64_MIN) return;
        const int64_t D = slot_ - prev0_;
        if (D < 2 || D > 16) return;
        // the channels between must fit: 1, 2, .. D-2, then a slow one (>= D-1)
        for (const auto& [s, ch] : hist_) {
            if (s <= prev0_ || s >= slot_) continue;
            const int64_t j = s - prev0_;
            if (j < D - 1 ? ch != j : ch < D - 1) return;
        }
        const int n = static_cast<int>(D - 1);
        if (n == n_last_) n_count_++;
        else {
            n_last_ = n;
            n_count_ = 1;
        }
        if (n_count_ >= 2 && n != N_) {
            N_ = n;
            clear_structure();
            if (N_ >= 7) M_ = N_;   // PS-15
            if (M_ >= 0) serial_update();
        }
    }

    // a slow channel (the slot after the fast ones): M from the wrap of its sequence
    void slow(int ch, int c1, int c6) {
        if (prev_slow_.ch >= 0 && slot_ - prev_slow_.slot == N_ + 1 && ch < prev_slow_.ch) {
            const int cand = prev_slow_.ch, cc1 = prev_slow_.c1;
            // the serial channel is the highest one, a split serial's first
            // nibble is 0xC: such a candidate is taken at its first wrap, any
            // other one when the same candidate wraps again
            int m = -1;
            if (cc1 == 0xC && prev_slow_.c6 <= 1) m = cand;
            else if (cand == cand_ && cc1 == cand_c1_) m = cand;
            cand_ = cand;
            cand_c1_ = cc1;
            if (m >= 0 && m != M_) {
                M_ = m;
                serial_update();
            }
        }
        prev_slow_ = {slot_, ch, c1, c6};
    }

    // the serial number on channel M
    void serial_update() {
        if (M_ < 6 || M_ >= NCH || !have_[M_]) return;   // M < 6: an old DFM-06 without one
        const uint32_t v = val_[M_];
        if (v >> 20 == 0xC) {
            if (!have_half_[M_][0] || !have_half_[M_][1]) return;
            accept(static_cast<uint32_t>(half_[M_][0]) << 16 | half_[M_][1], false);
            return;
        }
        // DFM-06 style: six BCD digits
        uint32_t num = 0;
        for (int k = 5; k >= 0; k--) {
            const unsigned dgt = v >> (4 * k) & 15;
            if (dgt > 9) return;
            num = num * 10 + dgt;
        }
        if (num == bcd_last_) bcd_count_++;
        else {
            bcd_last_ = num;
            bcd_count_ = 1;
        }
        if (bcd_count_ >= 2) accept(num, true);
    }

    void accept(uint32_t num, bool bcd) {
        if (!serial_known_ || (num == serial_ && bcd == serial_bcd_)) {
            serial_known_ = true;
            serial_ = num;
            serial_bcd_ = bcd;
            pend_count_ = 0;
            return;
        }
        // another number: only after it came twice in a row
        if (num == pend_) pend_count_++;
        else {
            pend_ = num;
            pend_count_ = 1;
        }
        if (pend_count_ >= 2) {
            serial_ = num;
            serial_bcd_ = bcd;
            pend_count_ = 0;
        }
    }

    std::string model() const {
        if (N_ >= 7) return "PS-15";
        if (serial_bcd_) return M_ == 8 ? "DFM-06P" : "DFM-06";
        switch (M_) {
            case 0xA: return serial_ >= 23000000 ? "DFM-17" : "DFM-09";
            case 0xB: return "DFM-17";
            case 0xC: return have_[6] && ch_float(val_[6]) < 220000 ? "DFM-09P" : "DFM-17";
            case 0xD: return "DFM-17P";
            default: return M_ >= 11 ? "DFM-17" : "DFM-09";
        }
    }

    // --- GPS packets ----------------------------------------------------------------------
    void packet(const uint8_t* d, const FrameInfo& fi, const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        const int id = d[12];
        if (id > 8) return;
        uint64_t p = 0;
        for (int k = 0; k < 12; k++) p = p << 4 | d[k];
        if (cyc_any_ && (id < last_id_ || pk_[id].have || c.pos_sync - last_pk_pos_ > 4.5 * FRAME_SAMPLES)) {
            cycle(fi, ctx, out);
            clear_cycle();
        }
        pk_[id] = {true, static_cast<int32_t>(p >> 16), static_cast<uint16_t>(p & 0xFFFF)};
        cyc_any_ = true;
        last_id_ = id;
        last_pk_pos_ = c.pos_sync;
        cyc_frame_ = fi;
        // packet 8 is the cycle's last: nothing can join it any more
        if (id == 8) {
            bool all = true;
            for (int i = 0; i < 8; i++) all = all && pk_[i].have;
            if (all) {
                cycle(fi, ctx, out);
                clear_cycle();
            }
        }
    }

    // a complete cycle -> a record; its span is the last frame that contributed,
    // its raw text the frame that completed it (`now`)
    void cycle(const FrameInfo& now, const Ctx& ctx, std::vector<Record>& out) {
        for (int i = 0; i < 8; i++)
            if (!pk_[i].have) return;
        int sats = -1;
        if (pk_[8].have) {
            const uint32_t u = static_cast<uint32_t>(pk_[8].i32);
            const int y = static_cast<int>(u >> 20), mo = u >> 16 & 15, dd = u >> 11 & 31, hh = u >> 6 & 31, mi = u & 63;
            if (y > 2000 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh < 24 && mi < 60) {
                have_date_ = true;
                y_ = y;
                mo_ = mo;
                d_ = dd;
                hh_ = hh;
                mi_ = mi;
            }
            sats = pk_[8].i16 >> 8;
        }
        int mode = static_cast<uint32_t>(pk_[0].i32) >> 8 & 0xFF;
        if (mode < 2 || mode > 4) mode = 2;
        double lat, lon, alt, vh, hdg, vv, sec;
        if (mode == 2) {
            lat = pk_[2].i32 * 1e-7;
            lon = pk_[3].i32 * 1e-7;
            alt = pk_[4].i32 / 100.0;
            vh = static_cast<int16_t>(pk_[2].i16) / 100.0;
            hdg = pk_[3].i16 / 100.0;
            vv = static_cast<int16_t>(pk_[4].i16) / 100.0;
            sec = pk_[1].i16 / 1000.0;
        } else {
            lat = pk_[1].i32 * 1e-7;
            lon = pk_[2].i32 * 1e-7;
            alt = pk_[3].i32 / 100.0;
            vh = static_cast<int16_t>(pk_[0].i16) / 100.0;
            hdg = pk_[1].i16 / 100.0;
            vv = static_cast<int16_t>(pk_[2].i16) / 100.0;
            sec = (static_cast<uint32_t>(pk_[0].i32) >> 16) / 1000.0;
        }
        sec = std::fmod(sec, 60.0);
        Record r;
        if (have_date_) {
            double utc = unix_time(y_, mo_, d_, hh_, mi_, sec);
            if (std::isfinite(prev_utc_) && utc < prev_utc_ - 1) utc += 60;   // the seconds wrapped first
            prev_utc_ = utc;
            r.utc = utc;
        } else {
            r.tod = sec;
        }
        if (!serial_known_) return;
        r.family = "DFM";
        r.model = model();
        r.serial = std::to_string(serial_);
        r.frame = static_cast<uint32_t>(pk_[0].i32) & 0xFF;
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -500 && alt < 50000 && !(lat == 0 && lon == 0)) {
            r.lat = lat;
            r.lon = lon;
            r.alt = alt;
            r.vh = vh;
            r.hdg = hdg;
            r.vv = vv;
        }
        r.sats = sats;
        sensors(r);
        r.offset = cyc_frame_.centre;
        r.t_first = ctx.clk.at(cyc_frame_.pos_sync);
        r.t_end = ctx.clk.at(cyc_frame_.pos_end);
        if (ctx.raw) r.hex = now.nib;
        out.push_back(std::move(r));
    }

    // temperature (Steinhart-Hart over the sensor's resistance against a
    // reference), battery and CPU temperature from the config channels
    void sensors(Record& r) const {
        const std::string& m = r.model;
        const bool pmodel = m == "DFM-09P" || m == "DFM-17P" || m == "DFM-06P";
        const int R = std::max(3, N_);
        const int S = pmodel ? 1 : 0, P = pmodel ? 5 : R, Q = pmodel ? 6 : R + 1;
        const double Rref = m.rfind("DFM-17", 0) == 0 ? 332e3 : 220e3;
        if (S < NCH && P < NCH && Q < NCH && have_[S] && have_[P] && have_[Q]) {
            const double q = ch_float(val_[Q]);
            const double res = (ch_float(val_[S]) - ch_float(val_[P])) * Rref / q;
            if (q > 0 && res > 0) {
                const double l = std::log(res);
                const double t = 1 / (1.09698417e-3 + 2.39564629e-4 * l + 2.48821437e-6 * l * l + 5.84354921e-8 * l * l * l) - 273.15;
                if (t > -100 && t < 70) r.T = t;
            }
        }
        const int bch = M_ >= 11 ? M_ - 7 : M_ - 5, tch = M_ >= 11 ? M_ - 6 : M_ - 4;
        if (bch >= 0 && bch < NCH && have_[bch]) r.batt = (val_[bch] >> 4 & 0xFFFF) / 1000.0 + (M_ >= 11 ? 0.28 : 0.0);
        if (tch >= 0 && tch < NCH && have_[tch]) r.Ti = (val_[tch] >> 4 & 0xFFFF) / 100.0 - 273.16;
    }
};

}  // namespace

std::unique_ptr<Family> make_dfm() { return std::make_unique<Dfm>(); }

}  // namespace sonde
