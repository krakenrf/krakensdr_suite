// LoRa / Meshtastic / LoRaWAN decoder for a full-2.4 MHz (lock_vfo) VFO.
//
// Every watched channel in the 2.4 MHz the tuner receives is mixed down,
// resampled to 2x its bandwidth and run through a chirp-spread-spectrum
// receiver (lora_phy.cpp): preamble detect (dechirped, so weak below-noise
// packets are caught), sync, demod, and the LoRa coding chain (coding.hpp).
// The PHY payload is then parsed as Meshtastic (clear header + AES-CTR with
// the PUBLISHED default key only + Data protobuf) or LoRaWAN (clear MAC
// header only - the FRMPayload is encrypted with keys we don't have), or
// shown as raw LoRa.
//
// ID separation: every decoded frame is a kp::Talker packet of its
// transmitter - the Meshtastic node that actually SENT these samples (the
// relaying node when hop_start != hop_limit, from the relay_node byte; else
// the originator's `from`), or the LoRaWAN DevAddr / DevEUI - on its own
// channel (freq_hz / bw_hz), so nodes on different channels / SFs at the
// same moment keep their own bearings.
//
// Clean-room from the public LoRa PHY literature and the Meshtastic /
// LoRaWAN protocol documentation (see coding.hpp, meshtastic.hpp,
// lorawan.hpp). GPL projects (gr-lora, rtl_433, ...) were read only as
// documentation / test reference, never copied.

#include "kraken_plugin.hpp"
#include "lora_phy.hpp"
#include "lorawan.hpp"
#include "meshtastic.hpp"

#include "kraken_dsp.hpp"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double FS = 2.4e6;
constexpr int MAX_RX = 28;            // cap on (channel x SF) receivers
constexpr double AC_KEEP_S = 3600;

std::string fmt(const char* f, double v) {
    char b[80];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string hex8(uint32_t a) {
    char b[12];
    snprintf(b, sizeof b, "%08x", a);
    return b;
}

struct Preset {
    const char* key;
    const char* name;
    int sf;
    double bw;
};
const Preset PRESETS[] = {
    {"shortturbo", "ShortTurbo", 7, 500000}, {"shortfast", "ShortFast", 7, 250000},
    {"shortslow", "ShortSlow", 8, 250000},   {"mediumfast", "MediumFast", 9, 250000},
    {"mediumslow", "MediumSlow", 10, 250000}, {"longfast", "LongFast", 11, 250000},
    {"longmod", "LongModerate", 11, 125000},  {"longslow", "LongSlow", 12, 125000},
};

// Meshtastic regions (firmware RDEF: freqStart, freqEnd, spacing MHz) + the
// LoRaWAN 125 kHz uplink grid used there
struct Region {
    const char* key;
    const char* name;
    double f_start_mhz, f_end_mhz, spacing_mhz;
    double lw_start_hz;
    double lw_step_hz;
    int lw_n;
};
const Region REGIONS[] = {
    {"anz", "Australia / New Zealand (915-928 MHz)", 915.0, 928.0, 0, 916.8e6, 200000, 8},   // AU915 sub-band 2
    {"us", "United States (902-928 MHz)", 902.0, 928.0, 0, 903.9e6, 200000, 8},              // US915 sub-band 2
    {"eu868", "Europe 868 (869.4-869.65 MHz)", 869.4, 869.65, 0, 868.1e6, 200000, 3},
};

// Meshtastic's channel-name hash (djb2, as the firmware's RadioInterface)
inline uint32_t djb2(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}
// The firmware's frequency: numChannels = floor((end - start) / (spacing + bw)),
// slot = hash(channel name) % numChannels, f = start + bw/2 + slot * bw (MHz).
// The default (unnamed) primary channel hashes the modem preset's name, so
// ANZ LongFast = slot 19 of 52 = 919.875 MHz.
inline double mesh_freq_hz(const Region& r, double bw_hz, const std::string& name) {
    const double bw = bw_hz / 1e6;
    const int n = std::max(1, static_cast<int>(std::floor((r.f_end_mhz - r.f_start_mhz) / (r.spacing_mhz + bw) + 1e-9)));
    const uint32_t slot = djb2(name) % static_cast<uint32_t>(n);
    return (r.f_start_mhz + bw / 2 + slot * bw) * 1e6;
}

struct Spec {
    double freq_hz;   // absolute centre
    int sf;
    double bw;
};

struct Node {
    std::string id;     // table / talker / map key
    std::string label;
    double first = 0, last = 0;
    uint64_t packets = 0, relayed = 0;
    float rssi = -200;
    double freq_mhz = 0;
    int sf = 0;
    double bw = 0;
    std::string proto;        // Meshtastic / LoRaWAN / LoRa
    // meshtastic
    std::string longname, shortname;
    std::string last_text;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    int alt_m = 0;
    bool on_map = false;
    double table_t = -1e9;
};

// Per channel: mix to the channel (phasor NCO), a decimate-by-D FIR (cutoff
// ~0.58 bw) and a cubic fractional interpolation to EXACTLY rate_out = 2 bw,
// so one LoRa symbol is exactly 2 x 2^SF output samples (the demod's hop grid
// must be a whole symbol). The FIR history is double-buffered (the window is
// contiguous: no index wrap per tap) and only evaluated every D-th sample.
struct Resampler {
    int D = 1, nt = 1;
    std::vector<float> taps;          // symmetric low-pass
    std::vector<float> hist;          // interleaved re/im, 2 x nt complex (double buffer)
    int hpos = 0, phase = 0;
    double rate_dec = 0, rate_out = 0, step = 0;   // decimated samples per output
    lora_phy::cf dring[16] = {};      // recent decimated samples
    int64_t di = 0;                   // decimated samples produced
    double rpos = 0;                  // next output position (decimated-sample units)
    std::complex<double> ph{1, 0}, rot{1, 0};   // NCO phasor
    bool mix = false;
    int renorm = 0;

    void init(double fs, double bw, double ro, double off_hz) {
        rate_out = ro;
        D = std::max(1, static_cast<int>(std::floor(fs / ro)));   // fs/D >= ro
        rate_dec = fs / D;
        step = rate_dec / rate_out;   // >= 1
        nt = 8 * D + 1;
        taps.assign(static_cast<size_t>(nt), 0.f);
        double s = 0;
        const double fc = 0.58 * bw / fs;
        for (int k = 0; k < nt; k++) {
            const double m = k - (nt - 1) / 2.0;
            const double sinc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.54 - 0.46 * std::cos(2 * M_PI * k / (nt - 1));
            taps[static_cast<size_t>(k)] = static_cast<float>(sinc * w);
            s += sinc * w;
        }
        for (auto& t : taps) t = static_cast<float>(t / s);
        hist.assign(static_cast<size_t>(4 * nt), 0.f);
        hpos = 0;
        phase = 0;
        di = 0;
        rpos = 1.0;
        for (auto& d : dring) d = lora_phy::cf(0, 0);
        mix = off_hz != 0;
        rot = std::polar(1.0, -2 * M_PI * off_hz / fs);
        ph = {1, 0};
        renorm = 0;
    }
    lora_phy::cf dget(int64_t idx) const { return dring[static_cast<size_t>(idx & 15)]; }
    static lora_phy::cf cubic(lora_phy::cf pm1, lora_phy::cf p0, lora_phy::cf p1, lora_phy::cf p2, float t) {
        const lora_phy::cf a = p2 - p1 - pm1 + p0;
        const lora_phy::cf b = pm1 - p0 - a;
        const lora_phy::cf c = p1 - pm1;
        return ((a * t + b) * t + c) * t + p0;
    }
    void run(const kp::cf* x, size_t n, std::vector<lora_phy::cf>& out) {
        float* h = hist.data();
        const float* tp = taps.data();
        for (size_t i = 0; i < n; i++) {
            float re = x[i].real(), im = x[i].imag();
            if (mix) {
                const float pr = static_cast<float>(ph.real()), pi = static_cast<float>(ph.imag());
                const float r2 = re * pr - im * pi;
                im = re * pi + im * pr;
                re = r2;
                ph *= rot;
                if (++renorm == 4096) { renorm = 0; ph /= std::abs(ph); }
            }
            h[2 * hpos] = h[2 * (hpos + nt)] = re;
            h[2 * hpos + 1] = h[2 * (hpos + nt) + 1] = im;
            if (++hpos == nt) hpos = 0;
            if (++phase < D) continue;
            phase = 0;
            // window: oldest .. newest = h[hpos .. hpos+nt-1] (taps symmetric)
            const float* w = h + 2 * hpos;
            float ar = 0, ai = 0;
            for (int k = 0; k < nt; k++) {
                ar += tp[k] * w[2 * k];
                ai += tp[k] * w[2 * k + 1];
            }
            dring[di & 15] = lora_phy::cf(ar, ai);
            di++;
            while (rpos + 2 <= static_cast<double>(di) - 1) {
                const int64_t f = static_cast<int64_t>(rpos);
                const float t = static_cast<float>(rpos - static_cast<double>(f));
                out.push_back(cubic(dget(f - 1), dget(f), dget(f + 1), dget(f + 2), t));
                rpos += step;
            }
        }
    }
};

// one resampled channel feeding one or more LoRa receivers (different SF)
struct Channel {
    Spec first_spec;             // for centre/bw
    double center_off = 0;       // Hz from the VFO centre
    double bw = 0;
    double rate_out = 0;
    Resampler rs;
    std::vector<lora_phy::cf> outbuf;
    std::vector<std::unique_ptr<lora_phy::LoraRx>> rx;
};

class LoraDecoder : public kp::Decoder {
public:
    explicit LoraDecoder(kp::Host& h) : Decoder(h) {
        host.table_columns({"Node", "Proto", "Name", "SF/BW", "Freq MHz", "From", "To", "Hops", "Last", "Signal dB", "Pkts"});
        host.table_keep(AC_KEEP_S);
    }

    void reset() override {
        pend_.clear();
        chans_.clear();
        rf_used_ = NAN;
        nodes_.clear();
        n_mesh_ = n_lw_ = n_lora_ = 0;
        summary_t_ = -1e9;
        built_ = false;
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "region") region_ = v.empty() ? "anz" : v;
        else if (k == "preset") preset_ = v.empty() ? "longfast" : v;
        else if (k == "channels") channels_opt_ = v;
        else if (k == "sf") sf_opt_ = v.empty() ? "preset" : v;
        else if (k == "lorawan") lorawan_ = v == "1";
        else if (k == "channel") chan_name_ = v;
        else if (k == "others") others_ = v != "0";
        else if (k == "tune") tune_ = v != "0";
        else if (k == "sens") sens_db_ = v.empty() ? 7 : atof(v.c_str());
        built_ = false;
    }

    void process(const kp::cf* x, size_t n, const kp::cf* const*, int) {
        now_ = host.time();
        rebuild_if_needed();
        frames_.clear();
        for (auto& c : chans_) {
            c->outbuf.clear();
            c->rs.run(x, n, c->outbuf);
            for (auto& rx : c->rx) {
                frames_.clear();
                rx->push(c->outbuf.data(), c->outbuf.size(), frames_);
                for (auto& fr : frames_) queue(*c, *rx, std::move(fr));
            }
        }
        flush_pending(false);
        housekeeping();
    }
    void process(const kp::cf* x, size_t n) override { process(x, n, nullptr, 0); }

private:
    std::vector<std::unique_ptr<Channel>> chans_;
    std::vector<lora_phy::Frame> frames_;
    std::map<std::string, Node> nodes_;
    double rf_used_ = NAN, now_ = 0, summary_t_ = -1e9;
    std::string region_ = "anz", preset_ = "longfast", channels_opt_, sf_opt_ = "preset";
    bool lorawan_ = false, tune_ = true, built_ = false, tune_asked_ = false, others_ = true;
    std::string chan_name_;   // "" = the preset's own name (the default channel)
    double sens_db_ = 7;
    uint64_t n_mesh_ = 0, n_lw_ = 0, n_lora_ = 0, n_dup_ = 0;
    const char* dump_ = getenv("LORA_DUMP");

    const Preset& preset() const {
        for (auto& p : PRESETS) if (preset_ == p.key) return p;
        return PRESETS[5];   // longfast
    }
    const Region& region() const {
        for (auto& r : REGIONS) if (region_ == r.key) return r;
        if (region_ == "au915") return REGIONS[0];
        return REGIONS[0];
    }
    // the watched Meshtastic channel: the channel name's hash slot (the
    // preset's name for the default channel)
    double mesh_hz() const { return mesh_freq_hz(region(), preset().bw, chan_name_.empty() ? preset().name : chan_name_); }

    std::vector<int> sf_set() const {
        if (sf_opt_ == "auto") return {7, 8, 9, 10, 11, 12};
        if (sf_opt_ == "preset") return {preset().sf};
        const int s = atoi(sf_opt_.c_str());
        if (s >= 5 && s <= 12) return {s};
        return {preset().sf};
    }

    // build the Spec list (absolute frequencies) from the options
    std::vector<Spec> specs() const {
        std::vector<Spec> out;
        if (!channels_opt_.empty()) {
            // "F:SF:BWkHz,F:SF:BWkHz" (F in MHz)
            std::string s = channels_opt_;
            size_t p = 0;
            while (p < s.size()) {
                size_t e = s.find(',', p);
                if (e == std::string::npos) e = s.size();
                std::string tok = s.substr(p, e - p);
                double f = 0, bw = 250;
                int sf = preset().sf;
                sscanf(tok.c_str(), "%lf:%d:%lf", &f, &sf, &bw);
                if (f > 0) out.push_back({f * 1e6, sf, bw * 1e3});
                p = e + 1;
            }
            return out;
        }
        const Preset& pr = preset();
        for (int sf : sf_set()) out.push_back({mesh_hz(), sf, pr.bw});
        // the other presets' default channels that fall inside the VFO too
        // (in the ANZ LongFast window: ShortFast 918.875, ShortSlow 920.625)
        if (others_ && chan_name_.empty()) {
            const double rf = host.rf_hz();
            for (const Preset& o : PRESETS) {
                if (&o == &pr) continue;
                const double f = mesh_freq_hz(region(), o.bw, o.name);
                if (std::isfinite(rf) && std::fabs(f - rf) <= 1.05e6 - o.bw / 2) out.push_back({f, o.sf, o.bw});
            }
        }
        if (lorawan_) {
            const Region& r = region();
            for (int k = 0; k < r.lw_n; k++)
                out.push_back({r.lw_start_hz + k * r.lw_step_hz, preset().sf >= 10 ? 10 : preset().sf, 125000});
        }
        return out;
    }

    void rebuild_if_needed() {
        const double rf = host.rf_hz();
        if (built_ && (std::isfinite(rf) == std::isfinite(rf_used_)) &&
            (!std::isfinite(rf) || std::fabs(rf - rf_used_) < 1e3))
            return;
        rf_used_ = rf;
        pend_.clear();
        chans_.clear();
        built_ = true;
        std::vector<Spec> sp = specs();
        // group specs by (freq, bw); each channel then holds one LoraRx per SF
        std::string txt;
        int total_rx = 0;
        std::map<std::pair<long, long>, Channel*> by_fb;
        for (const Spec& s : sp) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            if (std::isfinite(rf) && std::fabs(off) > 1.05e6) continue;   // outside the VFO
            if (total_rx >= MAX_RX) break;
            const long fk = std::lround(off / 1000), bk = std::lround(s.bw);
            Channel* c;
            auto it = by_fb.find({fk, bk});
            if (it == by_fb.end()) {
                auto up = std::make_unique<Channel>();
                c = up.get();
                c->center_off = off;
                c->bw = s.bw;
                c->rate_out = 2 * s.bw;
                c->rs.init(FS, s.bw, c->rate_out, off);
                chans_.push_back(std::move(up));
                by_fb[{fk, bk}] = c;
            } else {
                c = it->second;
            }
            bool have = false;
            for (auto& rx : c->rx) if (rx->sf() == s.sf) have = true;
            if (have) continue;
            const uint8_t sw = 0;   // accept any sync word; reported per frame
            auto rx = std::make_unique<lora_phy::LoraRx>(s.sf, s.bw, 2, sw, false, 0, 1, true, true);
            c->rx.push_back(std::move(rx));
            total_rx++;
        }
        // description of what we watch
        txt.clear();
        for (auto& c : chans_) {
            std::string sfs;
            for (auto& rx : c->rx) sfs += (sfs.empty() ? "" : "/") + std::to_string(rx->sf());
            txt += (txt.empty() ? "" : ", ") + fmt("%.3f", (std::isfinite(rf) ? rf + c->center_off : c->center_off) / 1e6) +
                   " SF" + sfs + fmt(" BW%.0f", c->bw / 1e3);
        }
        if (chans_.empty()) {
            txt = "no LoRa channel inside the VFO";
            if (tune_ && !tune_asked_ && std::isfinite(rf)) {
                tune_asked_ = true;
                if (host.tune(mesh_hz()))
                    host.event(fmt("Tuning to %.3f MHz for ", mesh_hz() / 1e6) + region().name + " " + preset().name, 30.0);
            }
        }
        host.fact("Watching", txt);
        host.fact("Region / preset", std::string(region().name) + " · " + preset().name +
                                          fmt(" (SF%.0f", (double)preset().sf) + fmt(" BW%.0f)", preset().bw / 1e3));
    }

    // A strong packet also decodes on a watched channel one bandwidth away (a
    // whole-bandwidth offset aliases to CFO 0 after dechirping): frames with
    // the same payload ending within 0.15 s are one transmission - keep the
    // channel where it is strongest (the real one; the neighbour sees it
    // through the channel filter's skirt). Held 50 ms (both decode in the same block or the next).
    struct Pend {
        const Channel* c;
        const lora_phy::LoraRx* rx;
        lora_phy::Frame fr;
        double te, ready;
    };
    std::vector<Pend> pend_;
    void queue(const Channel& c, const lora_phy::LoraRx& rx, lora_phy::Frame&& fr) {
        const double te = tstamp(c, fr.end_sample);
        for (Pend& p : pend_)
            if (p.fr.payload == fr.payload && std::fabs(p.te - te) < 0.15) {
                n_dup_++;
                if (fr.snr_db > p.fr.snr_db) {
                    p.c = &c;
                    p.rx = &rx;
                    p.fr = std::move(fr);
                    p.te = te;
                }
                return;
            }
        pend_.push_back({&c, &rx, std::move(fr), te, now_ + 0.05});
    }
    void flush_pending(bool all) {
        for (size_t i = 0; i < pend_.size();) {
            if (all || now_ >= pend_[i].ready) {
                Pend p = std::move(pend_[i]);
                pend_.erase(pend_.begin() + static_cast<long>(i));
                report(*p.c, *p.rx, p.fr);
            } else {
                i++;
            }
        }
    }

    // map a LoRaRx stream sample to host.time() seconds (seconds of 2.4 MHz input)
    static double tstamp(const Channel& c, int64_t s) { return static_cast<double>(s) / c.rate_out; }

    void report(const Channel& c, const lora_phy::LoraRx& rx, const lora_phy::Frame& fr) {
        if (fr.payload.empty()) return;
        const double freq = std::isfinite(rf_used_) ? (rf_used_ + c.center_off + fr.cfo_hz) / 1e6 : (c.center_off) / 1e6;
        double te = tstamp(c, fr.end_sample);
        double ts = tstamp(c, fr.start_sample);
        const double plen_s = te - ts;
        if (plen_s > 0.24) ts = te - 0.24;   // kraken_doa takes packets <= 0.25 s: the latest slice

        // decide the protocol + the transmitter ID. Meshtastic is confirmed
        // when the default-key decrypt yields a valid Data protobuf, or the
        // header is a broadcast with sane hop fields (a private channel that
        // won't decrypt); otherwise LoRaWAN (clear MAC header), else raw LoRa.
        mesh::Header mh;
        std::string tid, label, proto;
        bool mdec_ok = false;
        bool is_mesh = false;
        if (fr.payload.size() >= 18) {
            mesh::parse_header(fr.payload.data(), fr.payload.size(), mh);
            std::vector<uint8_t> dec(fr.payload.begin() + 16, fr.payload.end());
            mesh::decrypt_default(mh, dec);
            mdec_ok = mesh::parse_data(dec).ok;
            is_mesh = plausible_mesh(mh, fr) && (mdec_ok || mh.to == 0xFFFFFFFF);
        }
        const lorawan::Frame lw = lorawan::parse(fr.payload.data(), fr.payload.size());

        if (is_mesh) {
            proto = "Meshtastic";
            handle_mesh(c, rx, fr, mh, freq, ts, te, tid, label);
        } else if (lw.ok) {
            proto = "LoRaWAN";
            handle_lorawan(fr, lw, freq, tid, label);
        } else {
            proto = "LoRa";
            tid = fmt("lora %.3f", freq) + "/" + std::to_string(fr.sf);
            label = fmt("%d bytes", (double)fr.payload.size());
            n_lora_++;
        }
        if (tid.empty()) return;

        Node& nd = nodes_[tid];
        const bool fresh = nd.packets == 0;
        if (fresh) { nd.id = tid; nd.first = now_; nd.proto = proto; }
        nd.last = now_;
        nd.rssi = fresh ? fr.snr_db : nd.rssi + 0.3f * (fr.snr_db - nd.rssi);
        nd.freq_mhz = freq;
        nd.sf = fr.sf;
        nd.bw = c.bw;
        if (!label.empty()) nd.label = label;
        nd.packets++;
        host.valid(ts, te);

        // ID separation: a packet talker of the actual transmitter
        kp::Talker t;
        t.id = tid;
        t.label = nd.label.empty() ? proto : nd.label;
        t.start_s = ts;
        t.end_s = te;
        t.packet = true;
        t.freq_hz = c.center_off + fr.cfo_hz;
        t.bw_hz = c.bw;
        t.avg_s = 10;
        t.keep_s = 1800;
        if (std::isfinite(nd.lat) && now_ - nd.pos_t < 0.5) { t.lat = nd.lat; t.lon = nd.lon; t.alt_m = nd.alt_m; }
        host.talker(t);

        update_row(nd, mh, lw, proto);
        if (host.raw_wanted()) host.raw(proto + " " + fmt("%.3f", freq) + " SF" + std::to_string(fr.sf) + " " + hexstr(fr.payload));
        if (dump_)
            host.log(fmt("[%.3f] ", ts) + proto + " f=" + fmt("%.3f", freq) + " SF" + std::to_string(fr.sf) +
                     fmt(" cfo=%.0f", fr.cfo_hz) + fmt(" snr=%.1f ", fr.snr_db) + hexstr(fr.payload));
    }

    static bool plausible_mesh(const mesh::Header& mh, const lora_phy::Frame& fr) {
        if (fr.payload.size() < 16 + 2) return false;   // header + a tiny Data
        // broadcast or a unicast with a sane hop configuration
        const int hs = (mh.flags >> 5) & 7, hl = mh.flags & 7;
        if (hs > 7 || hl > hs + 1) return false;
        if (mh.from == 0) return false;
        return true;
    }

    void handle_mesh(const Channel&, const lora_phy::LoraRx&, const lora_phy::Frame& fr, const mesh::Header& mh,
                     double freq, double ts, double te, std::string& tid, std::string& label) {
        (void)ts;
        (void)te;
        n_mesh_++;
        // the transmitter of THESE samples: a relay (hop_start != hop_limit) is
        // the relaying node (its id's last byte = relay_node); else `from`.
        const bool relayed = mh.hop_start() != mh.hop_limit() && mh.relay_node != 0;
        if (relayed) {
            // we only know the last byte of the relayer's id
            tid = fmt2("!relay-%02x", mh.relay_node);
            label = "relay of !" + hex8(mh.from);
        } else {
            tid = "!" + hex8(mh.from);
        }
        // decrypt the Data with the default key and parse
        std::vector<uint8_t> enc(fr.payload.begin() + 16, fr.payload.end());
        std::vector<uint8_t> dec = enc;
        mesh::decrypt_default(mh, dec);
        mesh::Decoded d = mesh::parse_data(dec);
        Node& nd = nodes_[tid];
        std::string ev;
        if (d.ok) {
            if (d.portnum == mesh::TEXT && !d.text.empty()) {
                nd.last_text = d.text;
                ev = "💬 " + ((relayed) ? ("!" + hex8(mh.from)) : nd_name(mh.from)) + ": " + d.text;
                if (!relayed && label.empty()) label = d.text.substr(0, 24);
            } else if (d.portnum == mesh::NODEINFO) {
                if (!d.longname.empty()) node_names_[mh.from] = d.longname;
                Node& o = nodes_["!" + hex8(mh.from)];
                o.longname = d.longname;
                o.shortname = d.shortname;
                if (!relayed) label = d.longname;
                ev = "👤 NodeInfo !" + hex8(mh.from) + ": " + kp::printable(d.longname) + " (" + kp::printable(d.shortname) + ")";
            } else if (d.portnum == mesh::POSITION && d.has_pos) {
                Node& o = nodes_["!" + hex8(mh.from)];
                o.lat = d.lat;
                o.lon = d.lon;
                o.alt_m = d.alt_m;
                o.pos_t = now_;
                if (!relayed) { nd.lat = d.lat; nd.lon = d.lon; nd.alt_m = d.alt_m; nd.pos_t = now_; }
                map_point(mh.from, d);
                ev = fmt2("📍 Position !%s: ", hex8(mh.from).c_str()) + fmt("%.5f", d.lat) + fmt(", %.5f", d.lon);
            } else {
                ev = "📦 " + std::string(mesh::port_name(d.portnum)) + " from !" + hex8(mh.from);
            }
        } else {
            ev = "🔒 Meshtastic (not the default channel) from !" + hex8(mh.from) +
                 fmt(" ch#%02x", mh.chan_hash) + " - header only";
        }
        if (!label.empty()) label = kp::printable(label);
        host.event(ev + fmt(" · %.3f MHz", freq) + " SF" + std::to_string(fr.sf), 2.0);
    }

    void handle_lorawan(const lora_phy::Frame& fr, const lorawan::Frame& lw, double freq, std::string& tid,
                        std::string& label) {
        n_lw_++;
        if (lw.mtype == lorawan::JOIN_REQ) {
            tid = fmt2("DevEUI %016llx", (unsigned long long)lw.deveui);
            label = "join request";
            host.event(fmt2("🔑 LoRaWAN join: DevEUI %016llx", (unsigned long long)lw.deveui) +
                           fmt2(" JoinEUI %016llx", (unsigned long long)lw.joineui) + fmt(" · %.3f MHz", freq), 2.0);
        } else {
            tid = fmt2("DevAddr %08X", lw.devaddr);
            label = std::string(lorawan::mtype_name(lw.mtype)) + (lw.fport >= 0 ? fmt(" · FPort %.0f", (double)lw.fport) : "");
            host.event(fmt2("📡 LoRaWAN %s", lorawan::mtype_name(lw.mtype)) + fmt2(" DevAddr %08X", lw.devaddr) +
                           fmt(" FCnt %.0f", (double)lw.fcnt) + (lw.fport >= 0 ? fmt(" FPort %.0f", (double)lw.fport) : "") +
                           fmt(" · %.3f MHz", freq) + " SF" + std::to_string(fr.sf),
                       2.0);
        }
    }

    std::map<uint32_t, std::string> node_names_;
    std::string nd_name(uint32_t from) {
        auto it = node_names_.find(from);
        return it != node_names_.end() ? it->second : ("!" + hex8(from));
    }

    void map_point(uint32_t from, const mesh::Decoded& d) {
        kp::MapPoint p;
        p.id = "!" + hex8(from);
        p.lat = d.lat;
        p.lon = d.lon;
        p.kind = "station";
        p.label = nd_name(from);
        if (d.alt_m) p.altitude_m = d.alt_m;
        p.info = "Node: !" + hex8(from) + "\nType: Meshtastic";
        p.ttl_s = 3600;
        host.map_point(p);
        nodes_["!" + hex8(from)].on_map = true;
    }

    void update_row(Node& nd, const mesh::Header& mh, const lorawan::Frame& lw, const std::string& proto) {
        if (now_ - nd.table_t < 0.5 && nd.packets > 1) return;
        nd.table_t = now_;
        std::string name = nd.longname.empty() ? nd.label : nd.longname;
        std::string from = "", to = "", hops = "";
        if (proto == "Meshtastic") {
            from = "!" + hex8(mh.from);
            to = mh.to == 0xFFFFFFFF ? "bcast" : "!" + hex8(mh.to);
            hops = std::to_string(mh.hop_limit()) + "/" + std::to_string(mh.hop_start());
        } else if (proto == "LoRaWAN") {
            from = nd.id;
            hops = "";
        }
        host.table_row(nd.id, {nd.id, proto, kp::printable(name), fmt("%d/", (double)nd.sf) + fmt("%.0f", nd.bw / 1e3),
                               fmt("%.3f", nd.freq_mhz), from, to, hops,
                               kp::printable(nd.last_text.substr(0, 40)), fmt("%.0f", nd.rssi),
                               std::to_string(nd.packets)});
        std::vector<std::pair<std::string, std::string>> det;
        det.push_back({"Protocol", proto});
        if (!nd.longname.empty()) det.push_back({"Long name", kp::printable(nd.longname)});
        if (!nd.shortname.empty()) det.push_back({"Short name", kp::printable(nd.shortname)});
        if (proto == "Meshtastic") {
            det.push_back({"From", "!" + hex8(mh.from)});
            det.push_back({"To", mh.to == 0xFFFFFFFF ? "broadcast" : "!" + hex8(mh.to)});
            det.push_back({"Packet id", hex8(mh.id)});
            det.push_back({"Hop limit / start", std::to_string(mh.hop_limit()) + " / " + std::to_string(mh.hop_start())});
            det.push_back({"Channel hash", fmt2("0x%02x", mh.chan_hash)});
            if (mh.want_ack()) det.push_back({"Want ack", "yes"});
        } else if (proto == "LoRaWAN") {
            det.push_back({"MType", lorawan::mtype_name(lw.mtype)});
            if (lw.mtype != lorawan::JOIN_REQ) {
                det.push_back({"DevAddr", fmt2("%08X", lw.devaddr)});
                det.push_back({"FCnt", std::to_string(lw.fcnt)});
                if (lw.fport >= 0) det.push_back({"FPort", std::to_string(lw.fport)});
                det.push_back({"ADR", lw.adr ? "on" : "off"});
                det.push_back({"FRMPayload", std::to_string(lw.frmpayload_len) + " bytes (encrypted)"});
            } else {
                det.push_back({"DevEUI", fmt2("%016llx", (unsigned long long)lw.deveui)});
                det.push_back({"JoinEUI", fmt2("%016llx", (unsigned long long)lw.joineui)});
                det.push_back({"DevNonce", fmt2("%04x", lw.devnonce)});
            }
        }
        det.push_back({"SF / BW", std::to_string(nd.sf) + " / " + fmt("%.0f kHz", nd.bw / 1e3)});
        if (!nd.last_text.empty()) det.push_back({"Last text", kp::printable(nd.last_text)});
        if (std::isfinite(nd.lat)) det.push_back({"Position", fmt("%.5f", nd.lat) + fmt(", %.5f", nd.lon)});
        host.table_detail(nd.id, det);
    }

    void housekeeping() {
        if (now_ - summary_t_ < 1.0) return;
        summary_t_ = now_;
        int nm = 0, nl = 0, nr = 0;
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            Node& nd = it->second;
            if (now_ - nd.last > AC_KEEP_S) {
                if (nd.on_map) host.map_remove(nd.id);
                host.table_remove(nd.id);
                it = nodes_.erase(it);
                continue;
            }
            if (nd.proto == "Meshtastic") nm++;
            else if (nd.proto == "LoRaWAN") nl++;
            else nr++;
            ++it;
        }
        host.fact("Nodes", std::to_string(nm) + " Meshtastic, " + std::to_string(nl) + " LoRaWAN, " + std::to_string(nr) + " LoRa");
        host.fact("Packets", std::to_string(n_mesh_) + " Meshtastic, " + std::to_string(n_lw_) + " LoRaWAN, " +
                                 std::to_string(n_lora_) + " LoRa");
    }

    static std::string fmt2(const char* f, ...) {
        char b[96];
        va_list ap;
        va_start(ap, f);
        vsnprintf(b, sizeof b, f, ap);
        va_end(ap);
        return b;
    }
    static std::string hexstr(const std::vector<uint8_t>& v) {
        static const char* H = "0123456789abcdef";
        std::string s;
        for (uint8_t b : v) { s += H[b >> 4]; s += H[b & 15]; }
        return s;
    }
};

}  // namespace

KRAKEN_PLUGIN(LoraDecoder, {.id = "lora",
                            .name = "LoRa / Meshtastic / LoRaWAN",
                            .description = "LoRa chirp-spread-spectrum packets in the 2.4 MHz the tuner receives: "
                                           "Meshtastic (node table, text / position / nodeinfo on the public default "
                                           "channel) and LoRaWAN (clear MAC headers). SF7-12, BW 125/250/500 kHz. The "
                                           "VFO sits on its tuner's centre with the full bandwidth; tune the receiver "
                                           "to the band (NZ/AU Meshtastic 915-928 MHz). Each node gets its own DoA in "
                                           "the 🗺 Map's 📡 DF panel",
                            .version = "1.0",
                            .sample_rate = FS,
                            .min_vfo_rate = FS,
                            .author = "KrakenSDR",
                            .options = {{"region", "Region", "anz",
                                         "anz=Australia / New Zealand (915-928)|us=United States (902-928)|eu868=Europe 868",
                                         "The Meshtastic region (its band sets the channel frequency, as the firmware "
                                         "computes it) and the LoRaWAN uplink grid"},
                                        {"preset", "Meshtastic preset", "longfast",
                                         "shortturbo=ShortTurbo (SF7 BW500)|shortfast=ShortFast (SF7 BW250)|"
                                         "shortslow=ShortSlow (SF8)|mediumfast=MediumFast (SF9)|mediumslow=MediumSlow (SF10)|"
                                         "longfast=LongFast (SF11 BW250)|longmod=LongModerate (SF11 BW125)|longslow=LongSlow (SF12 BW125)",
                                         "The modem preset to watch (sets SF and bandwidth)"},
                                        {"sf", "Spreading factor", "preset",
                                         "preset=From the preset|auto=Scan SF7-12|7=SF7|8=SF8|9=SF9|10=SF10|11=SF11|12=SF12",
                                         "Which spreading factors to demodulate on each channel (Scan costs more CPU)"},
                                        {"channel", "Channel name", "", "",
                                         "The Meshtastic primary channel's name (\"\" = the default channel, named after "
                                         "the preset): its hash picks the frequency slot. ANZ LongFast = 919.875 MHz"},
                                        {"others", "Other presets", "1", "1=On|0=Off",
                                         "Also watch the other presets' default channels that fall inside the VFO "
                                         "(ANZ LongFast window: ShortFast 918.875, ShortSlow 920.625 MHz)"},
                                        {"lorawan", "Watch LoRaWAN", "0", "0=Off|1=On",
                                         "Also watch the region's LoRaWAN 125 kHz uplink channels (headers only)"},
                                        {"channels", "Channels", "",
                                         "", "Explicit channel list \"MHz:SF:BWkHz,...\" (overrides region/preset)"},
                                        {"tune", "Tune to the band", "1", "1=On|0=Off",
                                         "Retune the receiver (once) to the preset's Meshtastic channel when no "
                                         "watched channel is inside the VFO"},
                                        {"sens", "Detection", "7", "4=4 dB|7=7 dB|10=10 dB|14=14 dB",
                                         "How far a preamble must stand above the noise to be demodulated"}},
                            .map = true,
                            .manual_only = true,
                            .talkers = true,
                            .lock_vfo = true,
                            .tunes = true})
