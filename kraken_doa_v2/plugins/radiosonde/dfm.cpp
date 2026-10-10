// Graw DFM-06 / DFM-09 / DFM-17: 2-FSK, 2500 baud Manchester (1250 bit/s;
// chips "10" = 0, "01" = 1). A frame (every 0.5 s) is a 16-bit sync 0x45CF
// and three blocks of Hamming(8,4) codewords, bit-interleaved per block:
//   CONF  7 codewords: a 4-bit channel id + 24-bit value - measurement
//         channels 0..8 (float: 20-bit mantissa / 2^4-bit exponent) and,
//         on the higher channels, the serial number in two halves
//   DAT1 / DAT2  13 codewords each: 48 data bits + a 4-bit packet id 0..8:
//         time, latitude + speed, longitude + direction, altitude + climb,
//         date (packet 8); the layout depends on the "position mode" in
//         packet 0 (2 = older firmware, 3 / 4 = DFM-09 / -17)
// No CRC: every codeword must decode (soft decision, at most one bit error)
// before a block is used. Temperature: NTC thermistor between two reference
// resistors (measurement channels 0, 3, 4 - or 1, 5, 6 on the variants with
// a pressure sensor), Steinhart-Hart fit.
//
// Protocol as documented by the open-source radiosonde community (rs1729/RS
// "dfm09mod"); independent implementation, validated against the
// radiosonde_auto_rx test recording.

#include "sonde.hpp"

#include <array>
#include <memory>

namespace sonde {
namespace {

const char* RAWHEAD = "10011010100110010101101001010101";
constexpr int CONF_BITS = 56, DAT_BITS = 104;
constexpr int FRAME_BITS = CONF_BITS + 2 * DAT_BITS;     // after the sync

// Hamming(8,4) codeword of a nibble (data bits first, msb first)
uint8_t ham_encode(int d) {
    const int d0 = d >> 3 & 1, d1 = d >> 2 & 1, d2 = d >> 1 & 1, d3 = d & 1;
    const int p0 = d1 ^ d2 ^ d3, p1 = d0 ^ d2 ^ d3, p2 = d0 ^ d1 ^ d3, p3 = d0 ^ d1 ^ d2;
    return static_cast<uint8_t>(d << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3);
}

class Dfm : public Type {
public:
    explicit Dfm(double fs) : fs_(fs) {
        for (int d = 0; d < 16; d++) cw_[d] = ham_encode(d);
        FskConfig c;
        c.baud = 2500;
        for (const char* p = RAWHEAD; *p; p++) c.sync.push_back(*p == '1');
        c.max_sync_err = 2;
        c.frame_syms = 2 * FRAME_BITS;
        rx_.init(c, fs);
        rx_.on_frame = [this](const FskFrame& f) { frame(f); };
        reset();
    }
    void push(const float* narrow, const float*, size_t n, double t0) override {
        rx_.clock(t0);
        for (size_t i = 0; i < n; i++) rx_.push(narrow[i]);
    }
    void reset() override {
        rx_.reset();
        last_t0_ = -1e12;
        posmode_ = -1;
        sn_ = 0; snx_[0] = snx_[1] = 0; snbits_ = 0; sn_ch_ = 0; sn_conf_ = 0;
        sn6_ = 0;
        max_ch_ = 0;
        cfg_.fill(false);
        meas_.fill(0);
        clear_pos();
        date_ok_ = false;
    }

private:
    void frame(const FskFrame& f);
    double frm_ts_ = NAN, frm_te_ = NAN;   // the FSK frame being decoded: its time span
    // nibbles of one block (L codewords); false if a codeword has 2+ errors
    bool block(const float* soft, int L, uint8_t* nib);
    void conf(const uint8_t* nib);
    void dat(const uint8_t* nib);
    void clear_pos() { lat_ = lon_ = alt_ = vh_ = dir_ = vv_ = sec_ = NAN; got_ = 0; }
    void emit(double dc);

    FskRx rx_;
    double fs_;
    uint8_t cw_[16];
    double last_t0_;
    int posmode_;
    // serial number: two 16-bit halves on channel sn_ch_ (DFM-09/17), or
    // decimal digits on channel 6/8 (DFM-06)
    uint32_t sn_, snx_[2], sn6_, sn_conf_;
    int snbits_, sn_ch_, max_ch_ = 0;
    std::array<bool, 9> cfg_;
    std::array<double, 9> meas_;
    double lat_, lon_, alt_, vh_, dir_, vv_, sec_;
    int got_;                         // bit 0 lat, 1 lon, 2 alt
    int y_ = 0, mo_ = 0, d_ = 0, h_ = 0, mi_ = 0, sats_ = -1;
    bool date_ok_;
    double batt_ = NAN, tint_ = NAN;
    long frnr_ = -1;
    float dc_ = 0;
    double last_utc_ = NAN;
    std::string raw_;
};

bool Dfm::block(const float* soft, int L, uint8_t* nib) {
    // de-interleave: transmitted bit j of every codeword, then j+1 ...
    for (int i = 0; i < L; i++) {
        float s[8];
        int hard = 0;
        for (int j = 0; j < 8; j++) {
            s[j] = soft[L * j + i];
            hard = hard << 1 | (s[j] > 0);
        }
        // maximum-likelihood codeword (soft correlation); reject if it needed
        // more than one hard bit flipped
        int best = 0;
        float bc = -1e30f;
        for (int d = 0; d < 16; d++) {
            float c = 0;
            for (int j = 0; j < 8; j++) c += ((cw_[d] >> (7 - j)) & 1) ? s[j] : -s[j];
            if (c > bc) { bc = c; best = d; }
        }
        if (__builtin_popcount(cw_[best] ^ hard) > 1) return false;
        nib[i] = static_cast<uint8_t>(best);
    }
    return true;
}

void Dfm::frame(const FskFrame& f) {
    frm_ts_ = f.t_start;
    frm_te_ = f.t_end;
    // Manchester: bit = second chip minus first ("01" = 1)
    float bits[FRAME_BITS];
    for (int i = 0; i < FRAME_BITS; i++) bits[i] = f.soft[2 * i + 1] - f.soft[2 * i];
    uint8_t conf_n[7], d1[13], d2[13];
    const bool okc = block(bits, 7, conf_n);
    const bool ok1 = block(bits + CONF_BITS, 13, d1);
    const bool ok2 = block(bits + CONF_BITS + DAT_BITS, 13, d2);
    if (!okc && !ok1 && !ok2) return;
    if (std::fabs(f.t0 - last_t0_) < 0.1 * fs_) return;    // same frame, another timing phase
    last_t0_ = f.t0;
    if (want_raw) {
        uint8_t all[33];
        int k = 0;
        for (int i = 0; i < 7; i++) all[k++] = conf_n[i];
        for (int i = 0; i < 13; i++) all[k++] = d1[i];
        for (int i = 0; i < 13; i++) all[k++] = d2[i];
        static const char* H = "0123456789ABCDEF";
        raw_.clear();
        for (int i = 0; i < 33; i++) { raw_ += H[all[i] & 15]; if (i == 6 || i == 19) raw_ += ' '; }
    }
    dc_ = f.dc_hz;
    if (okc) conf(conf_n);
    if (ok1) dat(d1);
    if (ok2) dat(d2);
}

static uint32_t nibs(const uint8_t* n, int from, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) v = v << 4 | n[from + i];
    return v;
}

void Dfm::conf(const uint8_t* n) {
    const int id = n[0];
    // measurement channels 0..8: 24-bit float (4-bit exponent, 20-bit mantissa)
    if (id <= 8) {
        const uint32_t v = nibs(n, 1, 6);
        meas_[id] = (v & 0xFFFFF) / static_cast<double>(1u << (v >> 20 & 0xF));
        cfg_[id] = true;
    }
    // serial number channel: the highest channel id seen with second nibble
    // 0xC (the channel numbers below it carry measurements, which can look
    // alike); value = 16-bit half + 4-bit half index
    if (id > 5 && id > max_ch_ && n[1] == 0xC) max_ch_ = id;
    if (id > 5 && id == max_ch_ && (n[1] == 0xC || n[1] == 0x0)) {
        const uint32_t v = nibs(n, 2, 5);
        const int half = v & 0xF;
        if (half < 2) {
            if (sn_ch_ != id) { snbits_ = 0; sn_ch_ = id; }
            snx_[half] = (v >> 4) & 0xFFFF;
            snbits_ |= 1 << half;
            if (snbits_ == 3) {
                const uint32_t sn = snx_[0] << 16 | snx_[1];
                if (sn == sn_conf_ || sn_conf_ == 0) sn_ = sn;   // a different one later must repeat to take over
                sn_conf_ = sn;
                snbits_ = 0;
            }
        }
    }
    // DFM-06: serial in decimal digits on channel 6 (DFM-06P: 8)
    if ((id == 6 || id == 8) && n[1] <= 9 && n[2] <= 9 && n[3] <= 9 && n[4] <= 9 && n[5] <= 9 && n[6] <= 9 && sn_ == 0 &&
        max_ch_ == 0 && sn_ch_ <= 0) {
        const uint32_t v = nibs(n, 1, 6);
        if (v == sn6_ && v) sn_ch_ = -id;
        sn6_ = v;
    }
    // housekeeping (DFM-09 / -17): battery and internal temperature
    const bool ptype = sn_ch_ == 0xD || (sn_ch_ == 0xC && cfg_[6] && meas_[6] < 220e3);
    const int ofs = ptype ? 2 : 0;
    if (sn_ch_ >= 0xA) {
        if (id == 5 + ofs) batt_ = nibs(n, 2, 4) / 1000.0;
        if (id == 6 + ofs) tint_ = nibs(n, 2, 4) / 100.0 - 273.15;   // board (STM32) temperature, sent in K
    }
}

void Dfm::dat(const uint8_t* n) {
    const int id = n[12];
    if (id > 8) return;
    auto bits = [&](int from, int count) -> uint32_t {   // msb-first bits of the 48-bit payload
        uint32_t v = 0;
        for (int i = from; i < from + count; i++) v = v << 1 | ((n[i / 4] >> (3 - i % 4)) & 1);
        return v;
    };
    // one cycle of packets 0..8 per second: a position is complete at its end
    // (packet 8, the date) - or, if that was lost, when the next cycle starts
    if (id == 0 && (got_ & 7) == 7) emit(dc_);
    if (id == 0) {
        const int mode = bits(16, 8);
        posmode_ = (mode > 1 && mode < 5) ? mode : -1;
        frnr_ = bits(24, 8);
    }
    if (id == 8) {
        y_ = bits(0, 12); mo_ = bits(12, 4); d_ = bits(16, 5); h_ = bits(21, 5); mi_ = bits(26, 6);
        sats_ = bits(32, 8);
        date_ok_ = y_ > 2000 && mo_ >= 1 && mo_ <= 12 && d_ >= 1 && d_ <= 31 && h_ < 24 && mi_ < 60;
        if ((got_ & 7) == 7) emit(dc_);
        return;
    }
    if (posmode_ < 0) return;
    if (posmode_ <= 2) {
        switch (id) {
        case 1: sec_ = bits(32, 16) / 1000.0; break;
        case 2: lat_ = static_cast<int32_t>(bits(0, 32)) / 1e7; vh_ = static_cast<int16_t>(bits(32, 16)) / 100.0; got_ |= 1; break;
        case 3: lon_ = static_cast<int32_t>(bits(0, 32)) / 1e7; dir_ = bits(32, 16) / 100.0; got_ |= 2; break;
        case 4: alt_ = static_cast<int32_t>(bits(0, 32)) / 100.0; vv_ = static_cast<int16_t>(bits(32, 16)) / 100.0; got_ |= 4; break;
        default: break;
        }
    } else {
        switch (id) {
        case 0: sec_ = bits(0, 16) / 1000.0; vh_ = static_cast<int16_t>(bits(32, 16)) / 100.0; break;
        case 1: lat_ = static_cast<int32_t>(bits(0, 32)) / 1e7; dir_ = bits(32, 16) / 100.0; got_ |= 1; break;
        case 2: lon_ = static_cast<int32_t>(bits(0, 32)) / 1e7; vv_ = static_cast<int16_t>(bits(32, 16)) / 100.0; got_ |= 2; break;
        case 3: alt_ = static_cast<int32_t>(bits(0, 32)) / 100.0; got_ |= 4; break;
        default: break;
        }
    }
}

void Dfm::emit(double dc) {
    Frame o;
    o.type = "DFM";
    o.dc_hz = static_cast<float>(dc);
    o.t_start = frm_ts_;   // the FSK frame that completed the position
    o.t_end = frm_te_;
    // type from the serial channel; DFM-17 also by its serial range
    std::string sub;
    double rf = 220e3;
    bool ptype = false;
    switch (sn_ch_) {
    case 0xA: sub = sn_ >= 23000000 ? "DFM-17" : "DFM-09"; if (sn_ >= 23000000) rf = 332e3; break;
    case 0xB: sub = "DFM-17"; rf = 332e3; break;
    case 0xC: ptype = cfg_[6] && meas_[6] < 220e3; sub = ptype ? "DFM-09P" : "DFM-17"; if (!ptype) rf = 332e3; break;
    case 0xD: sub = "DFM-17P"; rf = 332e3; ptype = true; break;
    case -6: sub = "DFM-06"; break;
    case -8: sub = "DFM-06P"; ptype = true; break;
    default: break;
    }
    if (sn_) o.serial = std::to_string(sn_);
    else if (sn_ch_ < 0) o.serial = std::to_string(sn6_);   // DFM-06: decimal digits
    if (o.serial.empty()) { clear_pos(); return; }        // no serial yet: nothing to show
    o.subtype = sub;
    o.frame_no = frnr_;
    if (std::isfinite(lat_) && std::isfinite(lon_) && std::fabs(lat_) <= 90 && std::fabs(lon_) <= 180 && alt_ > -500 && alt_ < 50000 &&
        !(lat_ == 0 && lon_ == 0)) {
        o.lat = lat_; o.lon = lon_; o.alt = alt_; o.vh = vh_; o.heading = dir_; o.vv = vv_;
    }
    if (date_ok_ && std::isfinite(sec_)) {
        o.utc = utc_seconds(y_, mo_, d_, h_, mi_, std::fmod(sec_, 60.0));
        // packet 8 of this cycle lost (emitted at the next cycle's start): its
        // date is the previous cycle's - a minute behind once the seconds wrapped
        if (std::isfinite(last_utc_) && o.utc < last_utc_ - 1) o.utc += 60;
        last_utc_ = o.utc;
    }
    else if (std::isfinite(sec_)) o.tod = std::fmod(sec_, 60.0);
    o.sats = sats_;
    // temperature: thermistor R = (f - f1) / (f2 / Rf), Steinhart-Hart fit
    const int a = ptype ? 1 : 0, b = ptype ? 5 : 3, c = ptype ? 6 : 4;
    if (cfg_[a] && cfg_[b] && cfg_[c] && meas_[c] > 0) {
        const double r = (meas_[a] - meas_[b]) / (meas_[c] / rf);
        if (r > 0) {
            const double l = std::log(r);
            const double t = 1 / (1.09698417e-03 + 2.39564629e-04 * l + 2.48821437e-06 * l * l + 5.84354921e-08 * l * l * l) - 273.15;
            if (t > -100 && t < 70) o.temp = t;
        }
    }
    o.batt = batt_;
    o.temp_int = tint_;
    o.raw = raw_;
    clear_pos();
    if (out) out(o);
}

}  // namespace

std::unique_ptr<Type> make_dfm(double fs) { return std::make_unique<Dfm>(fs); }

}  // namespace sonde
