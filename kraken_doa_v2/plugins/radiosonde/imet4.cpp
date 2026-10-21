// InterMet iMet-4 / iMet-1-RSB (family "iMet-4").
//
// FM carrying Bell 202 AFSK: 1200 baud, mark 1200 Hz = 1, space 2200 Hz = 0,
// asynchronous 8N1 bytes LSB first. Packets: SOH, packet id, data (fields LSB
// first), CRC-16 (CCITT, seed 0x1D0F) big-endian - PTU, PTUX, GPS, GPSX and
// XDATA (ozone ...). Protocol from the NOAA "iMet-1-RSB Radiosonde Protocol"
// v1.11 and ra-firmware (DF9DQ, BSD-3-Clause) for the CRC seed. The sondes
// send no serial: every iMet-4 is "iMet" (and so gets no talker).
//
// Demodulator on the wide discriminator: the carrier offset removed with a
// slow mean, mixed to 0 Hz at both tones, a one-bit moving sum each, soft bit
// = (|mark| - |space|) / (|mark| + |space|); a UART on that.

#include "families.hpp"

#include <complex>
#include <cstdio>

namespace sonde {

namespace {

constexpr double BAUD = 1200;
constexpr int SPB = 40;                      // samples per bit (48000 / 1200)
constexpr int HIST = 1024;                   // soft history (> 12 bits)
constexpr double GPS_HOLD_S = 3;             // PTU alone when no GPS for this long

class Imet4 : public Family {
public:
    Imet4() { reset(); }

    void reset() override {
        mean_ = 0;
        have_mean_ = false;
        pm_ = ps_ = std::complex<double>(1, 0);
        wm_ = std::polar(1.0, -2 * M_PI * 1200 / FS);
        ws_ = std::polar(1.0, -2 * M_PI * 2200 / FS);
        for (auto& v : rm_) v = 0;
        for (auto& v : rs_) v = 0;
        sm_ = ss_ = 0;
        k_ = 0;
        n_ = 0;
        for (auto& v : soft_) v = 0;
        prev_ = 0;
        start_ = -1;
        buf_.clear();
        pending_ = false;
        last_gps_ = -1e18;
        ozone_.clear();
    }

    void process(const float*, const float* wide, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        for (size_t i = 0; i < n; i++) {
            const int64_t k = idx + static_cast<int64_t>(i);
            const double x = wide[i];
            if (!have_mean_) {
                mean_ = x;
                have_mean_ = true;
            }
            mean_ += 1e-4 * (x - mean_);
            const double a = x - mean_;
            // the two tones mixed to 0 Hz, a one-bit moving sum each
            const std::complex<float> m(static_cast<float>(a * pm_.real()), static_cast<float>(a * pm_.imag()));
            const std::complex<float> s(static_cast<float>(a * ps_.real()), static_cast<float>(a * ps_.imag()));
            pm_ *= wm_;
            ps_ *= ws_;
            if ((k_ & 1023) == 0) {   // renormalise the oscillators
                pm_ /= std::abs(pm_);
                ps_ /= std::abs(ps_);
            }
            const int j = k_ % SPB;
            sm_ += m - rm_[j];
            ss_ += s - rs_[j];
            rm_[j] = m;
            rs_[j] = s;
            if (++k_ % (SPB * 4096) == 0) {   // re-sum: no rounding drift
                sm_ = ss_ = 0;
                for (int q = 0; q < SPB; q++) {
                    sm_ += rm_[q];
                    ss_ += rs_[q];
                }
            }
            const float am = std::abs(sm_), as = std::abs(ss_);
            const float sb = am + as > 0 ? (am - as) / (am + as) : 0.0f;
            soft_[k & (HIST - 1)] = sb;
            n_++;
            uart(k, sb, ctx, out);
        }
    }

private:
    double mean_ = 0;
    bool have_mean_ = false;
    std::complex<double> pm_, ps_, wm_, ws_;
    std::complex<float> rm_[SPB], rs_[SPB], sm_, ss_;
    int64_t k_ = 0, n_ = 0;
    float soft_[HIST];
    float prev_ = 0;
    int64_t start_ = -1;                     // the start bit's crossing (sample index), -1 = idle
    std::vector<uint8_t> buf_;               // packet bytes
    // the latest PTU, waiting for a GPS packet
    bool pending_ = false;
    Record ptu_;
    double last_gps_ = -1e18;                // sample position of the last GPS packet
    std::string ozone_;                      // the latest ozone XDATA, for the next GPS record

    float s_at(int64_t k) const { return soft_[k & (HIST - 1)]; }

    void uart(int64_t k, float sb, const Ctx& ctx, std::vector<Record>& out) {
        if (start_ < 0) {
            if (n_ > SPB && prev_ > 0 && sb <= 0) start_ = k;   // a start bit
            prev_ = sb;
            return;
        }
        prev_ = sb;
        // bit k (0 start, 1..8 data, 9 stop) at the crossing + (k + 0.5) bits
        const int64_t last = start_ + static_cast<int64_t>(9.5 * SPB);
        if (k < last) return;
        const float st = s_at(start_ + SPB / 2), sp = s_at(last);
        if (st < 0 && sp > 0) {
            unsigned v = 0;
            for (int b = 1; b <= 8; b++) v |= (s_at(start_ + static_cast<int64_t>((b + 0.5) * SPB)) > 0 ? 1u : 0u) << (b - 1);
            byte(static_cast<uint8_t>(v), k, ctx, out);
        } else {
            buf_.clear();   // framing error
        }
        start_ = -1;
    }

    static int packet_len(const std::vector<uint8_t>& b) {
        if (b.size() < 2) return 0;
        switch (b[1]) {
            case 0x01: return 14;
            case 0x02: return 18;
            case 0x04: return 20;
            case 0x05: return 30;
            case 0x03: return b.size() < 3 ? 0 : b[2] + 5;
            default: return -1;
        }
    }

    void byte(uint8_t v, int64_t k, const Ctx& ctx, std::vector<Record>& out) {
        buf_.push_back(v);
        for (;;) {
            // resynchronise on SOH
            size_t s = 0;
            while (s < buf_.size() && buf_[s] != 0x01) s++;
            if (s) buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(s));
            if (buf_.size() < 2) return;
            const int len = packet_len(buf_);
            if (len == 0) return;
            if (len < 0 || len > 60) {
                buf_.erase(buf_.begin());
                continue;
            }
            if (static_cast<int>(buf_.size()) < len) return;
            const uint16_t crc = u16be(buf_.data() + len - 2);
            if (crc16_aug_ccitt(buf_.data(), static_cast<size_t>(len - 2)) != crc) {
                buf_.erase(buf_.begin());
                continue;
            }
            handle(buf_.data(), len, k, ctx, out);
            buf_.erase(buf_.begin(), buf_.begin() + len);
        }
    }

    static Record base() {
        Record r;
        r.family = "iMet-4";
        r.model = "iMet-4";
        r.serial = "iMet";
        return r;
    }

    void handle(const uint8_t* p, int len, int64_t k, const Ctx& ctx, std::vector<Record>& out) {
        const int id = p[1];
        Record span = base();
        span.offset = mean_;
        span.t_end = ctx.clk.at(static_cast<double>(k));
        span.t_first = span.t_end - (len + 4) * 10 / BAUD;
        if (ctx.raw) span.hex = hex_upper(p, static_cast<size_t>(len));
        if (id == 0x01 || id == 0x04) {
            Record r = span;
            r.frame = u16le(p + 2);
            const double pr = u24le(p + 4) / 100.0;
            if (pr > 0 && pr < 1100) r.P = pr;
            const double t = static_cast<int16_t>(u16le(p + 7)) / 100.0;
            if (t > -100 && t < 70) r.T = t;
            const double rh = u16le(p + 9) / 100.0;
            if (rh >= 0 && rh <= 100) r.RH = rh;
            r.batt = p[11] / 10.0;
            if (id == 0x04) {
                r.Ti = static_cast<int16_t>(u16le(p + 12)) / 100.0;
                r.TU = static_cast<int16_t>(u16le(p + 16)) / 100.0;
                char b[48];
                snprintf(b, sizeof b, "%.2f °C", static_cast<int16_t>(u16le(p + 14)) / 100.0);
                r.extras.emplace_back("Pressure sensor temp.", b);
            }
            ptu_ = r;
            pending_ = true;
            if (static_cast<double>(k) - last_gps_ > GPS_HOLD_S * FS) out.push_back(r);
            return;
        }
        if (id == 0x03) {
            // XDATA: an ozone sensor (instrument 0x01, N = 8), data MSB first
            if (p[2] == 8 && p[3] == 0x01) {
                char b[96];
                snprintf(b, sizeof b, "#%d: cell %.3f uA, pump %.1f °C %d mA", p[4], u16be(p + 5) / 1000.0,
                         static_cast<int16_t>(u16be(p + 7)) / 100.0, p[9]);
                ozone_ = b;
            }
            return;
        }
        if (id != 0x02 && id != 0x05) return;
        last_gps_ = static_cast<double>(k);
        Record r = pending_ ? ptu_ : base();
        pending_ = false;
        r.offset = span.offset;
        r.t_first = span.t_first;
        r.t_end = span.t_end;
        r.hex = span.hex;
        const double lat = f32le(p + 2), lon = f32le(p + 6);
        const double alt = static_cast<double>(u16le(p + 10)) - 5000;
        r.sats = p[12];
        const bool pos = r.sats >= 3 && std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90 &&
                         std::fabs(lon) <= 180 && !(lat == 0 && lon == 0);
        if (pos) {
            r.lat = lat;
            r.lon = lon;
            r.alt = alt;
        }
        const int t = id == 0x02 ? 13 : 25;
        const int hh = p[t], mm = p[t + 1], ss = p[t + 2];
        if (hh < 24 && mm < 60 && ss < 60) r.tod = hh * 3600.0 + mm * 60.0 + ss;
        if (id == 0x05 && pos) {
            const double ve = f32le(p + 13), vn = f32le(p + 17), vu = f32le(p + 21);
            if (std::isfinite(ve) && std::isfinite(vn)) speed_dir(ve, vn, &r.vh, &r.hdg);
            if (std::isfinite(vu)) r.vv = vu;
        }
        if (!ozone_.empty()) {
            r.extras.emplace_back("Ozone sensor", ozone_);
            ozone_.clear();
        }
        out.push_back(std::move(r));
    }
};

}  // namespace

std::unique_ptr<Family> make_imet4() { return std::make_unique<Imet4>(); }

}  // namespace sonde
