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
#include "channelizer.hpp"
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
constexpr int MAX_RX = 72;            // cap on (channel x SF x IQ) receivers
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
// LoRaWAN channel plan used there (LoRaWAN Regional Parameters RP002):
// uplinks on a 125 kHz grid (+ a 500 kHz grid), downlinks (gateway ->
// device, INVERTED IQ) on their own 500 kHz grid (AU915 / US915) or on the
// uplink channels + RX2 (EU868). Every channel of the plan inside the VFO is
// watched, at every data rate's SF.
struct LwGrid {
    double f0_hz, step_hz;
    int n;
    double bw;
    int sf_lo, sf_hi;
    int iq;           // 0 normal (uplink), 1 inverted (downlink), 2 both
};
struct Region {
    const char* key;
    const char* name;
    double f_start_mhz, f_end_mhz, spacing_mhz;
    const char* lw_name;
    double band_lo_mhz, band_hi_mhz;   // the region's LoRa band: inside it the plugin never retunes
    LwGrid lw[4];
};
const Region REGIONS[] = {
    {"anz", "Australia / New Zealand (915-928 MHz)", 915.0, 928.0, 0, "AU915", 915.0, 928.0,
     {{915.2e6, 200000, 64, 125000, 7, 12, 0},     // uplink DR0-5
      {915.9e6, 1600000, 8, 500000, 8, 8, 0},      // uplink DR6
      {923.3e6, 600000, 8, 500000, 7, 12, 1},      // downlink DR8-13
      {0, 0, 0, 0, 0, 0, 0}}},
    {"us", "United States (902-928 MHz)", 902.0, 928.0, 0, "US915", 902.0, 928.0,
     {{902.3e6, 200000, 64, 125000, 7, 10, 0},     // uplink DR0-3
      {903.0e6, 1600000, 8, 500000, 8, 8, 0},      // uplink DR4
      {923.3e6, 600000, 8, 500000, 7, 12, 1},      // downlink DR8-13
      {0, 0, 0, 0, 0, 0, 0}}},
    {"eu868", "Europe 868 (869.4-869.65 MHz)", 869.4, 869.65, 0, "EU868", 863.0, 870.0,
     {{868.1e6, 200000, 3, 125000, 7, 12, 2},      // default channels: uplink + RX1 downlink
      {867.1e6, 200000, 5, 125000, 7, 12, 2},      // the usual extra channels (TTN)
      {869.525e6, 0, 1, 125000, 7, 12, 1},         // RX2
      {0, 0, 0, 0, 0, 0, 0}}},
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
    int iq = 0;       // 0 normal, 1 inverted, 2 both
    bool lorawan = false;
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
    // PHY of the last frame
    bool inverted = false, has_crc = false;
    int sync_word = -1, fec_bad = 0, fec_total = 0;
    bool hdr_clean = true;    // the first 9 bytes' codewords passed their parity
};

// one resampled channel feeding one or more LoRa receivers (SFs, normal /
// inverted IQ) that share its sample ring
struct Channel {
    double center_off = 0;       // Hz from the VFO centre
    double bw = 0;
    double rate_out = 0;
    bool lorawan = false;
    double t0 = 0;               // Host::time() of the channel's first sample
    lora_chan::Chan ch;
    lora_phy::Ring ring;
    std::vector<lora_phy::cf> outbuf;
    std::vector<std::unique_ptr<lora_phy::LoraRx>> rx;
};

// the longest packet worth keeping in a channel's ring: 255 bytes at CR 4/5
// (+ preamble), capped at 4 s
inline double max_airtime_s(int sf, double bw) {
    const int N = 1 << sf;
    const bool ldro = N / bw >= 0.016;
    const int sfa = ldro ? sf - 2 : sf;
    const int nib = 2 * 257 - (sf - 7);
    const int syms = 8 + (nib + sfa - 1) / sfa * 5 + 21;
    return std::min(4.0, syms * N / bw);
}

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
        n_mesh_ = n_lw_ = n_lw_dn_ = n_lora_ = 0;
        summary_t_ = -1e9;
        built_ = false;
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "region") region_ = v.empty() ? "anz" : v;
        else if (k == "preset") preset_ = v.empty() ? "longfast" : v;
        else if (k == "channels") channels_opt_ = v;
        else if (k == "sf") sf_opt_ = v.empty() ? "preset" : v;
        else if (k == "lorawan") lorawan_ = v != "0";
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
        for (auto& c : chans_) c->outbuf.clear();
        if (!chans_.empty())
            front_.push(x, n, [&] {
                for (auto& c : chans_) c->ch.block(front_.X, c->outbuf);
            });
        for (auto& c : chans_) {
            c->ring.push(c->outbuf.data(), c->outbuf.size());
            for (auto& rx : c->rx) {
                frames_.clear();
                rx->poll(frames_);
                for (auto& fr : frames_) queue(*c, *rx, std::move(fr));
            }
        }
        flush_pending(false);
        housekeeping();
    }
    void process(const kp::cf* x, size_t n) override { process(x, n, nullptr, 0); }

private:
    std::vector<std::unique_ptr<Channel>> chans_;
    lora_chan::Front front_;
    std::vector<lora_phy::Frame> frames_;
    std::map<std::string, Node> nodes_;
    double rf_used_ = NAN, now_ = 0, summary_t_ = -1e9;
    std::string region_ = "anz", preset_ = "longfast", channels_opt_, sf_opt_ = "preset";
    bool lorawan_ = true, tune_ = true, built_ = false, tune_asked_ = false, others_ = true;
    std::string chan_name_;   // "" = the preset's own name (the default channel)
    double sens_db_ = 7;
    uint64_t n_mesh_ = 0, n_lw_ = 0, n_lw_dn_ = 0, n_lora_ = 0, n_dup_ = 0;
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
            // "F:SF:BWkHz[:inv|:both],..." (F in MHz; SF 0 = SF7-12)
            std::string s = channels_opt_;
            size_t p = 0;
            while (p < s.size()) {
                size_t e = s.find(',', p);
                if (e == std::string::npos) e = s.size();
                std::string tok = s.substr(p, e - p);
                double f = 0, bw = 250;
                int sf = preset().sf;
                char iqs[8] = "";
                sscanf(tok.c_str(), "%lf:%d:%lf:%7s", &f, &sf, &bw, iqs);
                const int iq = !strcmp(iqs, "inv") ? 1 : !strcmp(iqs, "both") ? 2 : 0;
                if (f > 0) {
                    if (sf >= 5 && sf <= 12) out.push_back({f * 1e6, sf, bw * 1e3, iq, false});
                    else
                        for (int k = 7; k <= 12; k++) out.push_back({f * 1e6, k, bw * 1e3, iq, false});
                }
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
            const double rf = host.rf_hz();
            for (const LwGrid& g : r.lw) {
                for (int k = 0; k < g.n; k++) {
                    const double f = g.f0_hz + k * g.step_hz;
                    if (std::isfinite(rf) && std::fabs(f - rf) > 1.05e6 - g.bw / 2) continue;
                    for (int sf = g.sf_lo; sf <= g.sf_hi; sf++) out.push_back({f, sf, g.bw, g.iq, true});
                }
            }
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
        front_.reset();
        built_ = true;
        std::vector<Spec> sp = specs();
        // group specs by (freq, bw); each channel then holds one LoraRx per SF
        std::string txt;
        int total_rx = 0;
        std::map<std::pair<long, long>, Channel*> by_fb;
        // channels first (the ring is sized for the largest SF on it), then
        // one receiver per SF and IQ orientation
        std::map<std::pair<long, long>, double> ring_s;
        for (const Spec& s : sp) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            if (std::isfinite(rf) && std::fabs(off) > 1.05e6) continue;   // outside the VFO
            const long fk = std::lround(off / 1000), bk = std::lround(s.bw);
            double& t = ring_s[{fk, bk}];
            t = std::max(t, max_airtime_s(s.sf, s.bw) + 4.0 * (1 << s.sf) / s.bw);
            auto it = by_fb.find({fk, bk});
            if (it == by_fb.end()) {
                auto up = std::make_unique<Channel>();
                Channel* c = up.get();
                c->center_off = off;
                c->bw = s.bw;
                c->rate_out = 2 * s.bw;
                c->t0 = now_;
                c->ch.init(FS, s.bw, off);
                chans_.push_back(std::move(up));
                by_fb[{fk, bk}] = c;
            }
            by_fb[{fk, bk}]->lorawan |= s.lorawan;
        }
        for (auto& kv : by_fb) kv.second->ring.init(static_cast<int>(ring_s[kv.first] * kv.second->rate_out));
        for (const Spec& s : sp) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            auto it = by_fb.find({std::lround(off / 1000), std::lround(s.bw)});
            if (it == by_fb.end()) continue;
            Channel* c = it->second;
            for (int inv = 0; inv <= 1; inv++) {
                if (!(s.iq == 2 || s.iq == inv)) continue;
                if (total_rx >= MAX_RX) break;
                bool have = false;
                for (auto& rx : c->rx) if (rx->sf() == s.sf && rx->inverted() == (inv == 1)) have = true;
                if (have) continue;
                c->rx.push_back(std::make_unique<lora_phy::LoraRx>(s.sf, s.bw, 2, &c->ring, inv == 1));
                total_rx++;
            }
        }
        // description of what we watch
        txt.clear();
        for (auto& c : chans_) {
            int lo = 99, hi = 0;
            bool nrm = false, inv = false;
            for (auto& rx : c->rx) {
                lo = std::min(lo, rx->sf());
                hi = std::max(hi, rx->sf());
                (rx->inverted() ? inv : nrm) = true;
            }
            const std::string sfs = lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi);
            txt += (txt.empty() ? "" : ", ") + fmt("%.3f", (std::isfinite(rf) ? rf + c->center_off : c->center_off) / 1e6) +
                   " SF" + sfs + fmt(" BW%.0f", c->bw / 1e3) + (inv ? (nrm ? " up+down" : " down") : "");
        }
        if (total_rx >= MAX_RX) txt += " (receiver cap reached)";
        if (chans_.empty()) txt = "no LoRa channel inside the VFO";
        // Retune ONCE, and only when the receiver is outside the region's
        // LoRa band (the decoder picked on some other frequency): inside the
        // band the user chose the frequency - the plugin restarts on a
        // rebuild / crash and must not pull the receiver away then
        const Region& reg = region();
        if (chans_.empty() && tune_ && !tune_asked_ && std::isfinite(rf) &&
            (rf < reg.band_lo_mhz * 1e6 || rf > reg.band_hi_mhz * 1e6)) {
            tune_asked_ = true;
            if (host.tune(mesh_hz()))
                host.event(fmt("Tuning to %.3f MHz for ", mesh_hz() / 1e6) + region().name + " " + preset().name, 30.0);
        }
        host.fact("Watching", txt);
        host.fact("Region / preset", std::string(region().name) + " · " + preset().name +
                                          fmt(" (SF%.0f", (double)preset().sf) + fmt(" BW%.0f)", preset().bw / 1e3) +
                                          (lorawan_ ? std::string(" · LoRaWAN ") + region().lw_name : std::string()));
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
    static double tstamp(const Channel& c, int64_t s) { return c.t0 + static_cast<double>(s) / c.rate_out; }

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
        // Meshtastic sends normal IQ with its own sync word (0x2B) - never a
        // LoRaWAN downlink (inverted) or a frame with the LoRaWAN public sync
        // word 0x34
        if (fr.payload.size() >= 18 && !fr.inverted && fr.sync_word != 0x34 && fr.sync_word != 0x12) {
            mesh::parse_header(fr.payload.data(), fr.payload.size(), mh);
            std::vector<uint8_t> dec(fr.payload.begin() + 16, fr.payload.end());
            mesh::decrypt_default(mh, dec);
            mdec_ok = mesh::parse_data(dec).ok;
            is_mesh = plausible_mesh(mh, fr) && (mdec_ok || mh.to == 0xFFFFFFFF);
        }
        const lorawan::Frame lw = lorawan::parse(fr.payload.data(), fr.payload.size());

        // a LoRaWAN frame's direction must match its IQ: uplinks (join
        // request, data up) normal, downlinks (join accept, data down) inverted
        const bool lw_down = lw.mtype == lorawan::JOIN_ACC || lw.mtype == lorawan::UNCONF_DN || lw.mtype == lorawan::CONF_DN;
        const bool lw_fits = lw.ok && lw_down == fr.inverted && (fr.has_crc || lw_down);
        if (is_mesh) {
            proto = "Meshtastic";
            handle_mesh(c, rx, fr, mh, freq, ts, te, tid, label);
        } else if (lw_fits) {
            proto = "LoRaWAN";
            handle_lorawan(c, fr, lw, freq, tid, label);
        } else {
            proto = "LoRa";
            tid = fmt("lora %.3f", freq) + "/" + std::to_string(fr.sf) + (fr.inverted ? " inv" : "");
            label = fmt("%.0f bytes", (double)fr.payload.size()) + (fr.has_crc ? "" : " (no CRC)");
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
        nd.inverted = fr.inverted;
        nd.sync_word = fr.sync_word;
        nd.fec_bad = fr.fec_bad;
        nd.fec_total = fr.fec_total;
        nd.has_crc = fr.has_crc;
        nd.hdr_clean = true;
        for (size_t i = 0; i < std::min<size_t>(9, fr.byte_ok.size()); i++) nd.hdr_clean = nd.hdr_clean && fr.byte_ok[i];
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
                     fmt(" BW%.0f", c.bw / 1e3) + (fr.inverted ? " inv" : "") + fmt2(" sync=%02x", fr.sync_word & 0xFF) +
                     fmt(" cfo=%.0f", fr.cfo_hz) + fmt(" snr=%.1f", fr.snr_db) +
                     (fr.has_crc ? std::string(" crc") : fmt2(" fec=%d/%d", fr.fec_bad, fr.fec_total)) + " " + hexstr(fr.payload));
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

    void handle_lorawan(const Channel& c, const lora_phy::Frame& fr, const lorawan::Frame& lw, double freq,
                        std::string& tid, std::string& label) {
        n_lw_++;
        const std::string sfbw = " SF" + std::to_string(fr.sf) + fmt(" BW%.0f", c.bw / 1e3);
        if (fr.inverted) {
            // a DOWNLINK is sent by the GATEWAY, not by the device it is
            // addressed to: its own talker, named by its channel (the frame
            // carries no gateway ID) - never the DevAddr's
            n_lw_dn_++;
            const double ch_mhz = (std::isfinite(rf_used_) ? rf_used_ : 0.0) / 1e6 + c.center_off / 1e6;
            tid = fmt("GW %.1f", ch_mhz);
            // no CRC on downlinks: the MAC header (MHDR .. FPort) is trusted
            // only when its codewords passed their parity
            bool clean = true;
            for (size_t i = 0; i < std::min<size_t>(9, fr.byte_ok.size()); i++) clean = clean && fr.byte_ok[i];
            const std::string q = clean ? "" : "?";
            const std::string to = lw.mtype == lorawan::JOIN_ACC ? std::string("join accept") : fmt2("→ %08X", lw.devaddr) + q;
            label = "gateway · " + to;
            std::string ev = std::string("📡 LoRaWAN ") + lorawan::mtype_name(lw.mtype);
            if (lw.mtype != lorawan::JOIN_ACC)
                ev += fmt2(" → DevAddr %08X", lw.devaddr) + q + (lw.fport >= 0 ? fmt(" FPort %.0f", (double)lw.fport) : "");
            if (clean || lw.mtype == lorawan::JOIN_ACC) host.event(ev + fmt(" · %.3f MHz", freq) + sfbw + " (gateway downlink)", 60.0);
            return;
        }
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
                           fmt(" · %.3f MHz", freq) + sfbw,
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
            if (nd.inverted) {
                from = "gateway";
                to = lw.mtype == lorawan::JOIN_ACC ? "join accept" : fmt2("%08X", lw.devaddr) + (nd.hdr_clean ? "" : "?");
            } else {
                from = nd.id;
            }
            hops = "";
        }
        host.table_row(nd.id, {nd.id, proto, proto == "Meshtastic" ? kp::printable(name) : name, fmt("%.0f/", (double)nd.sf) + fmt("%.0f", nd.bw / 1e3),
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
            det.push_back({"Direction", nd.inverted ? "downlink (gateway → device; inverted IQ) - the bearing is the gateway's"
                                                    : "uplink (device → network)"});
            if (lw.mtype == lorawan::JOIN_ACC) {
                det.push_back({"Join accept", "encrypted (keys not public) - header only"});
            } else if (lw.mtype != lorawan::JOIN_REQ) {
                if (!nd.has_crc && !nd.hdr_clean)
                    det.push_back({"MAC header", "a parity error in its codewords - DevAddr / FCnt / FPort may be wrong"});
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
        det.push_back({"SF / BW", std::to_string(nd.sf) + " / " + fmt("%.0f kHz", nd.bw / 1e3) +
                                      (nd.inverted ? " · inverted IQ" : "")});
        if (nd.sync_word >= 0)
            det.push_back({"Sync word", fmt2("0x%02X", nd.sync_word) +
                                            (nd.sync_word == 0x34 ? " (LoRaWAN public)" : nd.sync_word == 0x12 ? " (private / LoRaWAN private)"
                                             : nd.sync_word == 0x2B ? " (Meshtastic)" : "")});
        det.push_back({"Integrity", nd.has_crc ? std::string("payload CRC passed")
                                               : fmt2("no payload CRC (LoRaWAN downlinks send none): header checksum passed, "
                                                      "%d of %d payload codewords failed their parity", nd.fec_bad, nd.fec_total)});
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
        host.fact("Packets", std::to_string(n_mesh_) + " Meshtastic, " + std::to_string(n_lw_) + " LoRaWAN (" +
                                 std::to_string(n_lw_dn_) + " downlinks), " + std::to_string(n_lora_) + " LoRa");
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
                                           "channel) and LoRaWAN uplinks + gateway downlinks (clear MAC headers). "
                                           "SF7-12, BW 125/250/500 kHz, normal + inverted IQ. The "
                                           "VFO sits on its tuner's centre with the full bandwidth; tune the receiver "
                                           "to the band (NZ/AU Meshtastic 915-928 MHz). Each node gets its own DoA in "
                                           "the 🗺 Map's 📡 DF panel",
                            .version = "1.1",
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
                                        {"lorawan", "Watch LoRaWAN", "1", "1=On|0=Off",
                                         "Also watch the region's LoRaWAN channels inside the VFO - uplinks (125 / 500 kHz) "
                                         "and gateway downlinks (500 kHz, inverted IQ; EU868: the uplink channels + RX2), "
                                         "every data rate's SF (clear headers only)"},
                                        {"channels", "Channels", "",
                                         "", "Explicit channel list \"MHz:SF:BWkHz[:inv|:both],...\" (SF 0 = SF7-12; "
                                             "inv = inverted IQ; overrides region/preset)"},
                                        {"tune", "Tune to the band", "1", "1=On|0=Off",
                                         "Retune the receiver (once) to the preset's Meshtastic channel when it is "
                                         "outside the region's LoRa band (never inside it)"},
                                        {"sens", "Detection", "7", "4=4 dB|7=7 dB|10=10 dB|14=14 dB",
                                         "How far a preamble must stand above the noise to be demodulated"}},
                            .map = true,
                            .manual_only = true,
                            .talkers = true,
                            .lock_vfo = true,
                            .tunes = true})
