// InterMet iMet-4 / iMet-1-RS: FM carrying Bell 202 AFSK, 1200 baud (mark
// 1200 Hz = 1, space 2200 Hz = 0), asynchronous 8N1 bytes, LSB first.
// Packets (InterMet "Binary Radiosonde Packet Definition"): SOH 0x01, an id,
// the data (little endian), a CRC-16/CCITT (init 0x1D0F, big endian):
//   0x01 PTU: packet number, pressure (24 bit, 1/100 hPa), temperature,
//        humidity (1/100), battery (1/10 V)
//   0x04 enhanced PTU: + internal, pressure-sensor and humidity-sensor
//        temperatures
//   0x02 GPS: latitude, longitude (float), altitude (m + 5000), satellites,
//        time (h m s, GPS)
//   0x05 enhanced GPS: + east / north / up velocity (float)
//   0x03 extra data (e.g. ozone: cell current, pump temperature / current)
// The iMet-4 sends no serial number.
//
// Demodulator: the discriminator (audio) mixed down at both tones, a
// one-bit moving average of each, soft bit = mark - space magnitude; a UART
// on that (start-bit edge, bits sampled at their middle).

#include "sonde.hpp"

#include <complex>
#include <cstdio>
#include <memory>

namespace sonde {
namespace {

using cfl = std::complex<float>;

uint16_t crc_imet(const uint8_t* d, int n) {
    uint16_t c = 0x1D0F;
    for (int i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(d[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

class Imet4 : public Type {
public:
    explicit Imet4(double fs) : fs_(fs) {
        spb_ = fs / 1200.0;
        L_ = static_cast<int>(std::lround(spb_));
        ring_m_.assign(L_, cfl(0, 0));
        ring_s_.assign(L_, cfl(0, 0));
        hist_.assign(static_cast<size_t>(spb_ * 12), 0.0f);
        wm_ = std::polar(1.0f, static_cast<float>(-2 * M_PI * 1200 / fs));
        ws_ = std::polar(1.0f, static_cast<float>(-2 * M_PI * 2200 / fs));
        reset();
    }
    void push(const float*, const float* wide, size_t n, double) override {
        for (size_t i = 0; i < n; i++) sample(wide[i]);
    }
    void reset() override {
        std::fill(ring_m_.begin(), ring_m_.end(), cfl(0, 0));
        std::fill(ring_s_.begin(), ring_s_.end(), cfl(0, 0));
        sum_m_ = sum_s_ = cfl(0, 0);
        pm_ = ps_ = cfl(1, 0);
        pos_ = 0;
        n_ = 0;
        state_ = -1;
        prev_ = 0;
        buf_.clear();
        have_ptu_ = false;
        last_gps_n_ = -1;
        dc_ = 0;
    }

private:
    void sample(float x);
    void byte(uint8_t b);
    void packet(const uint8_t* p, int len);

    double fs_, spb_;
    int L_;
    std::vector<cfl> ring_m_, ring_s_;
    cfl sum_m_, sum_s_, pm_{1, 0}, ps_{1, 0}, wm_, ws_;
    int pos_ = 0;
    int64_t n_ = 0;
    std::vector<float> hist_;   // soft bits of the last ~12 bit periods (for the UART)
    // UART: -1 = idle (waiting for a start edge), else the sample of the start edge
    int64_t state_ = -1;
    float prev_ = 0;
    std::vector<uint8_t> buf_;
    float dc_ = 0;
    // the last PTU / extra data, sent with the next GPS packet
    bool have_ptu_ = false;
    Frame ptu_;
    int64_t last_gps_n_ = -1;
    std::vector<std::pair<std::string, std::string>> extra_;
};

void Imet4::sample(float x) {
    dc_ += (x - dc_) * 1e-4f;                 // carrier offset (slow mean of the discriminator)
    const float a = x - dc_;
    // mix each tone to 0 Hz, one-bit moving average
    const cfl m = pm_ * a, s = ps_ * a;
    pm_ *= wm_; ps_ *= ws_;
    if ((n_ & 1023) == 0) { pm_ /= std::abs(pm_); ps_ /= std::abs(ps_); }
    sum_m_ += m - ring_m_[pos_];
    sum_s_ += s - ring_s_[pos_];
    ring_m_[pos_] = m;
    ring_s_[pos_] = s;
    if (++pos_ == L_) pos_ = 0;
    const float mm = std::abs(sum_m_), ms = std::abs(sum_s_);
    const float soft = (mm - ms) / (mm + ms + 1e-9f);   // +1 mark, -1 space
    hist_[n_ % hist_.size()] = soft;
    // UART: a start bit begins at a mark -> space crossing (the moving average
    // lags half a bit: the crossing marks the middle of the start bit's ramp)
    if (state_ < 0) {
        if (prev_ > 0 && soft <= 0) state_ = n_;
    } else {
        // sample point k (0 = start bit, 1..8 data, 9 stop) at the bit's end
        // (the moving average then covers exactly that bit)
        const double t9 = state_ + spb_ * 9.5;
        if (n_ >= static_cast<int64_t>(t9)) {
            auto at = [&](int k) { return hist_[static_cast<int64_t>(state_ + spb_ * (k + 0.5)) % hist_.size()]; };
            if (at(0) < 0 && at(9) > 0) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++) b |= (at(k + 1) > 0) << k;
                byte(b);
            } else {
                buf_.clear();   // framing error
            }
            state_ = -1;
        }
    }
    prev_ = soft;
    n_++;
}

void Imet4::byte(uint8_t b) {
    buf_.push_back(b);
    // resynchronise on SOH + a known id
    while (!buf_.empty() && buf_[0] != 0x01) buf_.erase(buf_.begin());
    while (buf_.size() >= 2) {
        int len = 0;
        switch (buf_[1]) {
        case 0x01: len = 14; break;
        case 0x04: len = 20; break;
        case 0x02: len = 18; break;
        case 0x05: len = 30; break;
        case 0x03: if (buf_.size() < 3) return; len = buf_[2] + 5; break;
        default: break;
        }
        if (!len || len > 60) {   // not a packet start: drop the SOH
            buf_.erase(buf_.begin());
            while (!buf_.empty() && buf_[0] != 0x01) buf_.erase(buf_.begin());
            continue;
        }
        if (static_cast<int>(buf_.size()) < len) return;
        if (crc_imet(buf_.data(), len - 2) == u16be(buf_.data() + len - 2)) {
            packet(buf_.data(), len);
            buf_.erase(buf_.begin(), buf_.begin() + len);
        } else {
            buf_.erase(buf_.begin());
        }
        while (!buf_.empty() && buf_[0] != 0x01) buf_.erase(buf_.begin());
    }
}

void Imet4::packet(const uint8_t* p, int len) {
    const int id = p[1];
    if (id == 0x01 || id == 0x04) {
        ptu_ = Frame{};
        ptu_.frame_no = u16le(p + 2);
        const double pr = u24le(p + 4) / 100.0, t = i16le(p + 7) / 100.0, u = u16le(p + 9) / 100.0;
        if (pr > 0 && pr < 1100) ptu_.pressure = pr;
        if (t > -100 && t < 70) ptu_.temp = t;
        if (u >= 0 && u <= 100) ptu_.rh = u;
        ptu_.batt = p[11] / 10.0;
        if (id == 0x04) {
            ptu_.temp_int = i16le(p + 12) / 100.0;
            ptu_.temp_rh = i16le(p + 16) / 100.0;
            char b[24];
            snprintf(b, sizeof b, "%.2f °C", i16le(p + 14) / 100.0);
            extra_.push_back({"Pressure sensor temp.", b});
        }
        have_ptu_ = true;
        // no GPS for a while: report the PTU alone
        if (last_gps_n_ < 0 || n_ - last_gps_n_ > 3 * fs_) {
            Frame o = ptu_;
            o.type = "iMet-4";
            o.subtype = "iMet-4";
            o.serial = "iMet";
            o.dc_hz = dc_;
            if (want_raw) o.raw = hex(p, len);
            if (out) out(o);
        }
        return;
    }
    if (id == 0x03 && len >= 12 && p[2] == 8 && p[3] == 0x01) {   // ozonesonde
        char b[96];
        snprintf(b, sizeof b, "#%d: cell %.3f uA, pump %.1f °C %d mA", p[4], u16le(p + 5) / 1000.0, i16le(p + 7) / 100.0, p[9]);
        extra_.push_back({"Ozone sensor", b});
        return;
    }
    if (id != 0x02 && id != 0x05) return;
    last_gps_n_ = n_;
    Frame o = have_ptu_ ? ptu_ : Frame{};
    o.type = "iMet-4";
    o.subtype = "iMet-4";
    o.serial = "iMet";
    o.dc_hz = dc_;
    const float lat = f32le(p + 2), lon = f32le(p + 6);
    const double alt = u16le(p + 10) - 5000.0;
    o.sats = p[12];
    const int tp = id == 0x05 ? 25 : 13;
    if (p[tp] < 24 && p[tp + 1] < 60 && p[tp + 2] < 61) o.tod = std::fmod(p[tp] * 3600 + p[tp + 1] * 60 + p[tp + 2] - GPS_LEAP_S + 86400.0, 86400);
    if (o.sats >= 3 && std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0)) {
        o.lat = lat; o.lon = lon; o.alt = alt;
        if (id == 0x05) {
            en_to_speed_dir(f32le(p + 13), f32le(p + 17), &o.vh, &o.heading);
            o.vv = f32le(p + 21);
        }
    }
    o.extra = extra_;
    extra_.clear();
    have_ptu_ = false;
    if (want_raw) o.raw = hex(p, len);
    if (out) out(o);
}

}  // namespace

std::unique_ptr<Type> make_imet4(double fs) { return std::make_unique<Imet4>(fs); }

}  // namespace sonde
