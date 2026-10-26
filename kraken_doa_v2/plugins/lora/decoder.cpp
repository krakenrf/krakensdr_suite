// LoRa / Meshtastic / LoRaWAN decoder for a full-2.4 MHz (lock_vfo) VFO -
// anywhere in the world.
//
// Every watched channel in the 2.4 MHz the tuner receives is cut out of one
// FFT channelizer (channelizer.hpp) at 2x its bandwidth and run through a
// chirp-spread-spectrum receiver per SF and IQ orientation (lora_phy.cpp):
// preamble detect (dechirped, so weak below-noise packets are caught), sync,
// demod, and the LoRa coding chain (coding.hpp). The PHY payload is then
// parsed as Meshtastic (clear header + AES-CTR with the PUBLISHED default
// key only + Data protobuf), LoRaWAN (clear MAC header only - the FRMPayload
// is encrypted with keys we don't have; class-B beacons) or shown as raw
// LoRa.
//
// Which channels: region-neutral by default (regions.hpp) - `region` /
// `lorawan` Auto = every Meshtastic region / LoRaWAN plan whose band holds
// the tuned frequency, all their channels inside the VFO (902-928 MHz: US +
// ANZ Meshtastic, US915 + AU915 + AS923 LoRaWAN ...), within a CPU budget
// (`cpu`), by priority. Plus the PLAN-FREE SEARCH (search.hpp + the probes
// below): LoRa anywhere in the VFO at any centre, SF5-12, BW 7.8-500 kHz,
// both IQ orientations - a found transmitter becomes a LEARNED channel with
// a receiver of its own.
//
// ID separation: every decoded frame is a kp::Talker packet of its
// transmitter - the Meshtastic node that actually SENT these samples (the
// relaying node when hop_start != hop_limit, from the relay_node byte; else
// the originator's `from`), the LoRaWAN DevAddr / DevEUI of an uplink, the
// gateway (by channel) of a downlink / beacon, or for other LoRa its channel
// + SF + sync word - on its own channel (freq_hz / bw_hz), so transmitters
// on different channels / SFs at the same moment keep their own bearings.
//
// Clean-room from the public LoRa PHY literature and the Meshtastic /
// LoRaWAN protocol documentation (see coding.hpp, meshtastic.hpp,
// lorawan.hpp, regions.hpp). GPL projects (gr-lora, rtl_433, ...) were read
// only as documentation / test reference, never copied.

#include "kraken_plugin.hpp"
#include "channelizer.hpp"
#include "coding.hpp"
#include "despread.hpp"
#include "split.hpp"
#include "lora_phy.hpp"
#include "lorawan.hpp"
#include "meshtastic.hpp"
#include "regions.hpp"
#include "search.hpp"

#include "kraken_dsp.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#ifndef __APPLE__
#include <malloc.h>   // mallopt (glibc); macOS has <malloc/malloc.h>
#endif
#include <time.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
double lora_thread_cpu_s_win();   // cpu_time_win.cpp
#endif

namespace {

constexpr double FS = 2.4e6;
constexpr int MAX_RX = 128;           // hard cap on (channel x SF x IQ) receivers
constexpr double AC_KEEP_S = 3600;
constexpr double VFO_HALF = 1.05e6;   // usable half-width of the 2.4 MHz
constexpr double HIST_S = 0.8;        // FFT history kept for the search's probes
constexpr int MAX_PROBES = 4;         // probes alive at once
constexpr int MAX_LEARNED = 8;
// a channel's ring holds the sync / header stage of its largest SF (the
// payload is demodulated block by block as it arrives): detection lookback +
// a 16-symbol Meshtastic preamble + sync + SFD + header + margin
constexpr double RING_SYMS = 60;
constexpr double LEARNED_KEEP_S = 1800;

using lora_reg::LW_PLANS;
using lora_reg::LwPlan;
using lora_reg::MESH_REGIONS;
using lora_reg::MeshRegion;
using lora_reg::PRESETS;
using lora_reg::Preset;

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

// receiver priority when the CPU budget can't hold every plan channel
enum Prio { P_EXPLICIT = 0, P_LEARNED, P_MESH, P_LW_DOWN, P_BEACON, P_MESH_OTHER, P_LW_UP };

struct Spec {
    double freq_hz;   // absolute centre (or offset from the VFO centre when the RF is unknown)
    int sf;
    double bw;
    int iq = 0;       // 0 normal, 1 inverted, 2 both
    bool lorawan = false;
    int prio = P_EXPLICIT;
    int beacon_len = 0;   // > 0: a LoRaWAN class-B beacon receiver (implicit header, CR 4/5, no CRC)
    int sync = -1;        // learned channels: the transmitter's sync word
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
    bool found_by_search = false, decoded = true;
};

// one resampled channel feeding one or more LoRa receivers (SFs, normal /
// inverted IQ) that share its sample ring
struct Channel {
    double center_off = 0;       // Hz from the VFO centre
    double name_hz = 0;          // its nominal frequency (absolute Hz; = center_off when the RF is unknown)
    double bw = 0;
    int os = 2;
    double rate_out = 0;
    bool lorawan = false;
    int beacon_len = 0;
    double t0 = 0;               // Host::time() of the channel's first sample
    int64_t first_block = 0;     // the front end's block its first sample came from (input index / HOP)
    lora_chan::Chan ch;
    lora_phy::Ring ring;
    std::vector<lora_phy::cf> outbuf;
    std::vector<std::unique_ptr<lora_phy::LoraRx>> rx;
    // plan-free search
    bool probe = false, learned = false;
    double probe_end_t = 0;      // a probe waiting for a packet's tail gives up here
    double probe_t0 = 0;
    std::vector<lora_phy::Detect> dets;
    int sync = -1;               // learned: the transmitter's sync word
    std::unique_ptr<lora_phy::Despreader> desp;   // separation by code (its receivers' symbols on every antenna)
};


// the longest packet worth keeping in a channel's ring: 255 bytes at CR 4/5
// (+ preamble)
inline double max_airtime_s(int sf, double bw) {
    const int N = 1 << sf;
    const bool ldro = N / bw >= 0.016;
    const int sfa = (ldro || sf < 7) ? std::max(sf - 2, 5) : sf;
    const int nib = 2 * 257 - (sf - 7);
    const int syms = 8 + (nib + sfa - 1) / sfa * 5 + 23;
    return syms * N / bw;
}
// ring samples for a channel: the longest packet, <= 4 s (wide) / 2^21 samples (narrow)
inline int ring_len(double secs, double rate) {
    const double cap = std::max(4.0 * rate, std::min(static_cast<double>(1 << 21), 60.0 * rate));
    return static_cast<int>(std::min(secs * rate, cap)) + 4096;
}

// this thread's CPU time (s): the search's budget is in the plugin's OWN CPU
// time per second of input - a wall clock also counts the time the thread
// was preempted, and on a busy Pi (load 3-5) that "paused" the search for good
inline double thread_cpu_s() {
#ifdef _WIN32
    // winpthreads' CLOCK_THREAD_CPUTIME_ID only moves every 15.6 ms
    return lora_thread_cpu_s_win();
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec);
#endif
}

// Separation by code: a channel's receivers' symbols de-spread on every
// antenna's input (despread.hpp) - channel sample a = input index
// first_block x HOP + a x FS / rate_out (the channelizer has no delay)
struct ChanDesp : lora_phy::Despreader {
    const lora_ds::RawRing* raw = nullptr;
    const Channel* c = nullptr;
    double* cpu_s = nullptr;                   // the de-spreading's thread CPU time (s)
    int antennas() const override { return raw ? raw->m : 0; }
    int despread(const lora_phy::LoraRx& rx, int64_t a, int bin, double frac, lora_phy::cf* y) override {
        if (!raw || raw->m < 2) return 0;
        const double t0 = thread_cpu_s();
        const double spc = FS / c->rate_out;   // input samples per channel sample
        const double p = static_cast<double>(c->first_block) * lora_chan::HOP + static_cast<double>(a) * spc;
        const int r = lora_ds::despread_sym(*raw, p, rx.window() * spc, c->center_off / FS, c->bw / FS, bin + frac,
                                            1 << rx.sf(), rx.inverted(), y);
        if (cpu_s) *cpu_s += thread_cpu_s() - t0;
        return r;
    }
};

// the LoRaWAN beacon's CRC (CRC-16, poly 0x1021, init 0, MSB first; stored LSB first)
inline uint16_t crc16_ccitt(const uint8_t* p, size_t n) { return lora_code::crc16(p, n); }

class LoraDecoder : public kp::Decoder {
public:
    explicit LoraDecoder(kp::Host& h) : Decoder(h) {
        // rings / probe rings (0.1-4 MB) always from mmap: calloc then gets
        // fresh zero pages (no memset, committed only as written) and free
        // returns them. glibc's dynamic threshold rose to the first freed
        // ring's size, after which rings came from the heap - zeroed by
        // calloc and kept by fragmentation (live RSS 128 MB). A glibc knob:
        // the Windows heap has none (it gives blocks over ~0.5 MB their own
        // VirtualAlloc and returns them on free)
#ifdef __GLIBC__
        mallopt(M_MMAP_THRESHOLD, 256 * 1024);
#endif
        host.table_columns({"Node", "Proto", "Name", "SF/BW", "Freq MHz", "From", "To", "Hops", "Last", "Signal dB", "Pkts"});
        host.table_keep(AC_KEEP_S);
        s1_.init(FS);
    }

    ~LoraDecoder() override {
        if (xtmp_) kfft_free(xtmp_);
    }

    void reset() override {
        pend_.clear();
        chans_.clear();
        rf_used_ = NAN;
        nodes_.clear();
        n_mesh_ = n_lw_ = n_lw_dn_ = n_lora_ = n_bcn_ = 0;
        summary_t_ = -1e9;
        built_ = false;
        learned_.clear();
        backoff_.clear();
        recent_det_.clear();
        det_seen_.clear();
        heard_.clear();
        n_probes_ = n_probe_hits_ = n_probe_det_ = n_probe_alias_ = 0;
        split_.clear();
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "region") {
            region_ = v.empty() ? "auto" : v;
            if (region_ == "eu868") region_ = "eu_868";   // the old keys
        } else if (k == "preset") preset_ = v.empty() ? "longfast" : v;
        else if (k == "channels") channels_opt_ = v;
        else if (k == "sf") sf_opt_ = v.empty() ? "preset" : v;
        else if (k == "lorawan") lorawan_ = (v.empty() || v == "1") ? "auto" : v;
        else if (k == "channel") chan_name_ = v;
        else if (k == "others") others_ = v != "0";
        else if (k == "beacons") beacons_ = v != "0";
        else if (k == "tune") tune_ = v != "0";
        else if (k == "search") search_ = v.empty() ? "1" : v;
        else if (k == "cpu") cpu_ = v.empty() ? "normal" : v;
        built_ = false;
    }

    void process(const kp::cf* x, size_t n, const kp::cf* const* xs, int m) {
        now_ = host.time();
        rebuild_if_needed();
        frames_.clear();
        for (auto& c : chans_) c->outbuf.clear();
        cands_.clear();
        const bool searching = search_on();
        if (!chans_.empty() || searching) {
            // every antenna's input for the separation by code, indexed like the front end's
            if (xs && m >= 2) {
                const int64_t at = front_.blocks * lora_chan::HOP + front_.fill;
                const double t_a = thread_cpu_s();
                if (raw_.m != m || raw_.abs != at) raw_.start(m, at);
                raw_.push(xs, n);
                raw_cpu_s_ += thread_cpu_s() - t_a;
            } else {
                raw_.off();
            }
            front_.push(x, n, [&] {
                if (searching) {
                    const double t_a = thread_cpu_s();
                    keep_history(front_.X);
                    s1_.block(front_.X, cands_);
                    search_cpu_s_ += thread_cpu_s() - t_a;
                }
                for (auto& c : chans_) c->ch.block(front_.X, c->outbuf);
            });
        }
        for (auto& c : chans_) {
            c->ring.push(c->outbuf.data(), c->outbuf.size());
            for (auto& rx : c->rx) {
                frames_.clear();
                rx->poll(frames_);
                for (auto& fr : frames_) queue(*c, *rx, std::move(fr));
            }
        }
        if (searching) run_search();
        flush_pending(false);
        housekeeping();
    }
    void process(const kp::cf* x, size_t n) override { process(x, n, nullptr, 0); }
    // every antenna (coherent mode with DoA on): antenna 0 is decoded as
    // before, all of them de-spread for each decoded packet's DoA
    void process_multi(const kp::cf* const* x, int m, size_t n) override {
        process(x[0], n, m >= 2 ? x : nullptr, m);
    }

private:
    std::vector<std::unique_ptr<Channel>> chans_;
    lora_chan::Front front_;
    double front_t0_ = 0;                 // Host::time() of the front's first sample
    lora_ds::RawRing raw_;                // every antenna's input (separation by code)
    std::map<std::string, lora_split::State> split_;   // IDs that name no transmitter: two behind one? (split.hpp)
    uint64_t n_split_skip_ = 0;
    uint64_t n_coded_ = 0, n_uncoded_ = 0, coded_syms_ = 0;
    double coded_coh_ = 0, desp_cpu_s_ = 0, raw_cpu_s_ = 0;   // thread CPU time: de-spreading, keeping the antennas' input
    std::vector<lora_phy::Frame> frames_;
    std::map<std::string, Node> nodes_;
    double rf_used_ = NAN, now_ = 0, summary_t_ = -1e9;
    std::string region_ = "auto", preset_ = "longfast", channels_opt_, sf_opt_ = "preset", lorawan_ = "auto";
    std::string search_ = "1", cpu_ = "normal";
    bool tune_ = true, built_ = false, tune_asked_ = false, others_ = true, beacons_ = true;
    std::map<std::string, int> last_sync_;   // raw LoRa: the sync word last measured per channel + SF
    std::string chan_name_;   // "" = the preset's own name (the default channel)
    uint64_t n_mesh_ = 0, n_lw_ = 0, n_lw_dn_ = 0, n_lora_ = 0, n_dup_ = 0, n_bcn_ = 0;
    const char* dump_ = getenv("LORA_DUMP");
    const char* sdump_ = getenv("LORA_SEARCH_DUMP");
    std::string plan_txt_, watch_txt_;
    int plan_rx_ = 0, plan_rx_dropped_ = 0;

    // ---- plan-free search state ----
    lora_search::Stage1 s1_;
    std::vector<lora_search::Candidate> cands_;
    // the last HIST_S of front FFT blocks as int16 I / Q with a scale per
    // block (10 MB instead of 20; ~90 dB of range per block)
    std::vector<int16_t> xhist_;
    std::vector<float> xscale_;
    kfft_complex* xtmp_ = nullptr;
    int hist_n_ = 0;
    struct Learned {
        double f_hz;          // absolute (or offset when the RF is unknown)
        double bw;
        int sf;
        bool inv;
        int sync;
        double last_t;
        uint64_t frames = 0;
    };
    std::vector<Learned> learned_;
    struct Backoff { double off, bw, until, wait; };
    std::vector<Backoff> backoff_;
    struct RecentDet { std::string txt; double t; };
    struct DetSeen { double f, bw; int sf; bool inv; int sync; double t; };
    std::vector<DetSeen> det_seen_;
    double probe_tokens_ = 6, probe_tok_t_ = 0;
    // the probes' CPU budget: thread CPU seconds, refilled at search_cpu_max()
    // per stream second (<= 0.3 s banked); over it the probes slow down (one
    // every PROBE_MIN_GAP_S at least) - the search never stops
    double probe_cpu_tok_ = 0.3, cpu_tok_t_ = 0, last_probe_t_ = -1e9, slowed_t_ = -1e9;
    uint64_t n_slowed_ = 0;
    static constexpr double PROBE_MIN_GAP_S = 2.0;
    std::vector<RecentDet> recent_det_;
    uint64_t n_probes_ = 0, n_probe_hits_ = 0, n_probe_det_ = 0, n_cand_ = 0;
    // undecoded preambles that were aliases of decoded packets (near_decoded) or sync 0x00: not reported
    uint64_t n_probe_alias_ = 0;
    // decoded packets of the last HEARD_KEEP_S: host time span + band (Hz from the VFO centre)
    struct Heard { double t0, t1, f, bw; };
    std::vector<Heard> heard_;
    static constexpr double HEARD_KEEP_S = 30;
    double search_cpu_s_ = 0, search_cpu_win_ = 0, search_cpu_t0_ = 0, search_cpu_frac_ = 0;

    bool search_on() const { return search_ != "0"; }
    double budget_cost() const { return cpu_ == "low" ? 120e6 : cpu_ == "high" ? 900e6 : 330e6; }
    double search_cpu_max() const { return cpu_ == "low" ? 0.04 : cpu_ == "high" ? 0.25 : 0.12; }

    const Preset& preset() const {
        for (auto& p : PRESETS) if (preset_ == p.key) return p;
        if (preset_ == "longmoderate") return PRESETS[7];
        return PRESETS[5];   // longfast
    }
    // the Meshtastic regions in use: Auto = every region whose band overlaps the VFO
    std::vector<const MeshRegion*> mesh_regions(double rf) const {
        std::vector<const MeshRegion*> out;
        if (region_ == "off") return out;
        for (const MeshRegion& r : MESH_REGIONS) {
            if (region_ == "auto") {
                if (std::isfinite(rf) && r.f1_mhz * 1e6 > rf - VFO_HALF && r.f0_mhz * 1e6 < rf + VFO_HALF) out.push_back(&r);
            } else if (region_ == r.key) {
                out.push_back(&r);
            }
        }
        return out;
    }
    std::vector<const LwPlan*> lw_plans(double rf) const {
        std::vector<const LwPlan*> out;
        if (lorawan_ == "0" || lorawan_ == "off") return out;
        for (const LwPlan& p : LW_PLANS) {
            if (lorawan_ == "auto") {
                if (std::isfinite(rf) && p.hi_mhz * 1e6 > rf - VFO_HALF && p.lo_mhz * 1e6 < rf + VFO_HALF) out.push_back(&p);
            } else if (lorawan_ == p.key) {
                out.push_back(&p);
            }
        }
        return out;
    }
    std::string mesh_name(const Preset& p) const { return chan_name_.empty() ? p.name : chan_name_; }

    std::vector<int> sf_set() const {
        if (sf_opt_ == "auto") return {7, 8, 9, 10, 11, 12};
        if (sf_opt_ == "preset") return {preset().sf};
        const int s = atoi(sf_opt_.c_str());
        if (s >= 5 && s <= 12) return {s};
        return {preset().sf};
    }

    static bool inside(double rf, double f, double bw) { return !std::isfinite(rf) || std::fabs(f - rf) <= VFO_HALF - bw / 2; }

    // build the Spec list (absolute frequencies) from the options
    std::vector<Spec> specs() {
        std::vector<Spec> out;
        const double rf = host.rf_hz();
        plan_txt_.clear();
        if (!channels_opt_.empty()) {
            // "F:SF:BWkHz[:inv|:both],..." (F in MHz; SF 0 = SF7-12)
            std::string s = channels_opt_;
            size_t p = 0;
            while (p < s.size()) {
                size_t e = s.find(',', p);
                if (e == std::string::npos) e = s.size();
                std::string tok = s.substr(p, e - p);
                double f = 0, bwk = 250;
                int sf = preset().sf;
                char iqs[8] = "";
                sscanf(tok.c_str(), "%lf:%d:%lf:%7s", &f, &sf, &bwk, iqs);
                const int iq = !strcmp(iqs, "inv") ? 1 : !strcmp(iqs, "both") ? 2 : 0;
                const double bw = lora_reg::snap_bw(bwk * 1e3);
                if (f > 0 && bw > 0) {
                    if (sf >= 5 && sf <= 12) out.push_back({f * 1e6, sf, bw, iq, false, P_EXPLICIT});
                    else
                        for (int k = 7; k <= 12; k++) out.push_back({f * 1e6, k, bw, iq, false, P_EXPLICIT});
                }
                p = e + 1;
            }
            plan_txt_ = "explicit channel list";
        } else {
            // Meshtastic: the primary preset's channel of every region in use,
            // then (others) every other preset's default channel inside the VFO
            std::string mtxt;
            for (const MeshRegion* r : mesh_regions(rf)) {
                mtxt += (mtxt.empty() ? "" : ", ") + std::string(r->name);
                for (const Preset& pr : PRESETS) {
                    const bool primary = &pr == &preset();
                    if (!primary && !(others_ && chan_name_.empty())) continue;
                    const double f = lora_reg::mesh_freq_hz(*r, pr.bw, primary ? mesh_name(pr) : pr.name);
                    if (f <= 0 || !inside(rf, f, pr.bw)) continue;
                    if (primary)
                        for (int sf : sf_set()) out.push_back({f, sf, pr.bw, 0, false, P_MESH});
                    else
                        out.push_back({f, pr.sf, pr.bw, 0, false, P_MESH_OTHER});
                }
            }
            std::string ltxt;
            for (const LwPlan* pl : lw_plans(rf)) {
                ltxt += (ltxt.empty() ? "" : ", ") + std::string(pl->name);
                for (const lora_reg::LwGrid& g : pl->g) {
                    for (int k = 0; k < g.n; k++) {
                        const double f = (g.f0_mhz + k * g.step_mhz) * 1e6;
                        if (!inside(rf, f, g.bw)) continue;
                        for (int sf = g.sf_lo; sf <= g.sf_hi; sf++)
                            out.push_back({f, sf, g.bw, g.iq, true, g.iq == 0 ? P_LW_UP : P_LW_DOWN});
                    }
                }
                if (beacons_ && pl->bcn.n > 0)
                    for (int k = 0; k < pl->bcn.n; k++) {
                        const double f = (pl->bcn.f0_mhz + k * pl->bcn.step_mhz) * 1e6;
                        if (!inside(rf, f, pl->bcn.bw)) continue;
                        Spec b{f, pl->bcn.sf, pl->bcn.bw, 1, true, P_BEACON};
                        b.iq = 0;   // beacons are sent with normal IQ
                        b.beacon_len = pl->bcn.len;
                        out.push_back(b);
                    }
            }
            plan_txt_ = (region_ == "auto" ? "Auto · " : "") + std::string("Meshtastic ") + (mtxt.empty() ? "-" : mtxt) +
                        " · LoRaWAN " + (ltxt.empty() ? "-" : ltxt);
            if (!std::isfinite(rf) && (region_ == "auto" || lorawan_ == "auto")) plan_txt_ = "Auto: RF unknown - search only";
        }
        for (const Learned& l : learned_) {
            if (!inside(rf, l.f_hz, l.bw)) continue;
            Spec s{l.f_hz, l.sf, l.bw, l.inv ? 1 : 0, false, P_LEARNED};
            s.sync = l.sync;
            out.push_back(s);
        }
        return out;
    }

    Channel* make_channel(double off, double name_hz, double bw, double ring_secs, int64_t first_block) {
        auto up = std::make_unique<Channel>();
        Channel* c = up.get();
        c->center_off = off;
        c->name_hz = name_hz;
        c->bw = bw;
        c->os = lora_chan::os_for(FS, bw);
        if (c->os == 0) return nullptr;
        c->rate_out = c->os * bw;
        c->t0 = front_t0_ + static_cast<double>(first_block) * lora_chan::HOP / FS;
        c->first_block = first_block;
        auto d = std::make_unique<ChanDesp>();
        d->raw = &raw_;
        d->c = c;
        d->cpu_s = &desp_cpu_s_;
        c->desp = std::move(d);
        c->ch.init(FS, bw, off, c->os);
        c->ring.init(ring_len(ring_secs, c->rate_out));
        chans_.push_back(std::move(up));
        return c;
    }

    void rebuild_if_needed() {
        const double rf = host.rf_hz();
        if (built_ && (std::isfinite(rf) == std::isfinite(rf_used_)) &&
            (!std::isfinite(rf) || std::fabs(rf - rf_used_) < 1e3))
            return;
        if (built_ && std::isfinite(rf) && std::isfinite(rf_used_) && std::fabs(rf - rf_used_) >= 1e3) backoff_.clear();
        rf_used_ = rf;
        pend_.clear();
        chans_.clear();
        front_.reset();
        front_t0_ = now_;
        s1_.init(FS);
        hist_n_ = 0;
        s1_.bws.clear();
        if (search_ != "all") s1_.bws = {500000, 250000, 125000, 62500};
        else for (int i = lora_reg::N_BANDWIDTHS - 1; i >= 0; i--) s1_.bws.push_back(lora_reg::BANDWIDTHS[i]);
        built_ = true;
        std::vector<Spec> sp = specs();
        // priority, then nearest the VFO centre first (the edges roll off), then SF
        std::stable_sort(sp.begin(), sp.end(), [&](const Spec& a, const Spec& b) {
            if (a.prio != b.prio) return a.prio < b.prio;
            const double da = std::isfinite(rf) ? std::fabs(a.freq_hz - rf) : 0, db = std::isfinite(rf) ? std::fabs(b.freq_hz - rf) : 0;
            if (std::fabs(da - db) > 1) return da < db;
            return a.sf < b.sf;
        });
        // receivers within the CPU budget (a receiver's detection costs ~ bw x sf)
        const double budget = budget_cost();
        double cost = 0;
        std::vector<Spec> use;
        plan_rx_dropped_ = 0;
        std::set<std::tuple<long, long, int, int, int>> seen;
        std::set<std::pair<long, long>> chan_seen;
        // a channel's own cost (its inverse FFT of 2 bw / 520.8 Hz points every
        // block) in the same units: ~ bw x 5.3 log2(M) - a Meshtastic channel
        // with one receiver costs more than its receiver
        auto chan_cost = [](double bw) {
            const double m = std::max(16.0, 2 * bw / (FS / lora_chan::NFFT));
            return bw * 5.3 * std::log2(m);
        };
        for (const Spec& s : sp) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            if (std::isfinite(rf) && std::fabs(off) > VFO_HALF) continue;
            for (int inv = 0; inv <= 1; inv++) {
                if (!(s.iq == 2 || s.iq == inv)) continue;
                const auto key = std::make_tuple(std::lround(off / 1000), std::lround(s.bw), s.sf, inv, s.beacon_len);
                if (seen.count(key)) continue;
                const std::pair<long, long> ck{std::lround(off / 1000), std::lround(s.bw)};
                const double c1 = s.bw * s.sf + (chan_seen.count(ck) ? 0.0 : chan_cost(s.bw));
                if (s.prio != P_EXPLICIT && s.prio != P_LEARNED && (cost + c1 > budget || static_cast<int>(use.size()) >= MAX_RX)) {
                    plan_rx_dropped_++;
                    continue;
                }
                if (static_cast<int>(use.size()) >= MAX_RX) { plan_rx_dropped_++; continue; }
                seen.insert(key);
                chan_seen.insert(ck);
                cost += c1;
                Spec u = s;
                u.iq = inv;
                use.push_back(u);
            }
        }
        // channels (the ring is sized for the largest SF on it), then one
        // receiver per SF and IQ orientation
        std::map<std::pair<long, long>, Channel*> by_fb;
        std::map<std::pair<long, long>, double> ring_s;
        for (const Spec& s : use) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            double& t = ring_s[{std::lround(off / 1000), std::lround(s.bw)}];
            t = std::max(t, RING_SYMS * (1 << s.sf) / s.bw);
        }
        for (const Spec& s : use) {
            const double off = std::isfinite(rf) ? (s.freq_hz - rf) : 0.0;
            const std::pair<long, long> key{std::lround(off / 1000), std::lround(s.bw)};
            Channel* c = nullptr;
            auto it = by_fb.find(key);
            if (it == by_fb.end()) {
                c = make_channel(off, std::isfinite(rf) ? s.freq_hz : off, s.bw, ring_s[key], 0);
                if (!c) continue;
                by_fb[key] = c;
            } else {
                c = it->second;
            }
            c->lorawan |= s.lorawan;
            if (s.prio == P_LEARNED) { c->learned = true; c->sync = s.sync; }
            auto r = s.beacon_len > 0
                         ? std::make_unique<lora_phy::LoraRx>(s.sf, s.bw, c->os, &c->ring, false, true, s.beacon_len, 1, false)
                         : std::make_unique<lora_phy::LoraRx>(s.sf, s.bw, c->os, &c->ring, s.iq == 1);
            if (s.beacon_len > 0) c->beacon_len = s.beacon_len;
            if (s.prio == P_LEARNED) r->strict_nocrc = true;
            r->desp = c->desp.get();
            c->rx.push_back(std::move(r));
        }
        plan_rx_ = static_cast<int>(use.size());
        describe();
        // Retune ONCE, and only for an explicitly chosen Meshtastic region when
        // the receiver is outside its band (the decoder picked on some other
        // frequency). Auto (the default) never retunes: the user picked the
        // frequency, and a restart (rebuild / crash) must not pull it away.
        if (region_ != "auto" && region_ != "off" && tune_ && !tune_asked_ && std::isfinite(rf) && channels_opt_.empty()) {
            const std::vector<const MeshRegion*> rs = mesh_regions(rf);
            if (!rs.empty()) {
                const MeshRegion& reg = *rs[0];
                if (rf < reg.f0_mhz * 1e6 || rf > reg.f1_mhz * 1e6) {
                    tune_asked_ = true;
                    const double f = lora_reg::mesh_freq_hz(reg, preset().bw, mesh_name(preset()));
                    if (f > 0 && host.tune(f))
                        host.event(fmt("Tuning to %.3f MHz for ", f / 1e6) + reg.name + " " + preset().name, 30.0);
                }
            }
        }
    }

    void describe() {
        const double rf = rf_used_;
        std::string txt;
        int shown = 0, nch = 0;
        for (auto& c : chans_) {
            if (c->probe) continue;
            nch++;
            if (shown >= 24) continue;
            shown++;
            int lo = 99, hi = 0;
            bool nrm = false, inv = false;
            for (auto& rx : c->rx) {
                lo = std::min(lo, rx->sf());
                hi = std::max(hi, rx->sf());
                (rx->inverted() ? inv : nrm) = true;
            }
            const std::string sfs = lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi);
            txt += (txt.empty() ? "" : ", ") + fmt("%.3f", (std::isfinite(rf) ? c->name_hz : c->center_off) / 1e6) +
                   " SF" + sfs + fmt(c->bw < 1e5 ? " BW%.1f" : " BW%.0f", c->bw / 1e3) +
                   (inv ? (nrm ? " up+down" : " down") : "") + (c->beacon_len ? " beacon" : "") + (c->learned ? " (learned)" : "");
        }
        if (nch > shown) txt += fmt(" ... (%.0f channels)", nch);
        if (plan_rx_dropped_ > 0) txt += fmt(" · %.0f plan receivers over the CPU budget (the search covers them)", plan_rx_dropped_);
        if (chans_.empty()) txt = "no plan channel inside the VFO";
        watch_txt_ = txt;
        host.fact("Watching", txt);
        host.fact("Region / plan", plan_txt_ + " · " + preset().name + fmt(" (SF%.0f", (double)preset().sf) +
                                       fmt(" BW%.0f)", preset().bw / 1e3));
    }

    // ---------------------------------------------------------------- plan-free search, stage 2
    int hist_cap() const { return static_cast<int>(HIST_S * FS / lora_chan::HOP); }
    void keep_history(const kfft_complex* X) {
        const int H = hist_cap();
        constexpr int NF = 2 * lora_chan::NFFT;
        if (xhist_.empty()) {
            xhist_.assign(static_cast<size_t>(H) * NF, 0);
            xscale_.assign(static_cast<size_t>(H), 0.f);
        }
        const float* __restrict x = &X[0][0];
        // the peak as 8 partial maxima (exact in any order; one running
        // maximum was a serial chain the compilers don't vectorise)
        float m8[8] = {};
        for (int i = 0; i < NF; i += 8)
            for (int j = 0; j < 8; j++) m8[j] = std::max(m8[j], std::fabs(x[i + j]));
        float mx = 0;
        for (float v : m8) mx = std::max(mx, v);
        const int slot = static_cast<int>(front_.blocks % H);
        const float sc = mx > 0 ? 32767.f / mx : 1.f;
        int16_t* __restrict d = &xhist_[static_cast<size_t>(slot) * NF];
        // rounded to nearest (even) as lrintf did - |x sc| <= 32767; nearbyint
        // vectorises, the lrintf call didn't
        for (int i = 0; i < NF; i++) d[i] = static_cast<int16_t>(static_cast<int>(std::nearbyint(x[i] * sc)));
        xscale_[static_cast<size_t>(slot)] = 1.f / sc;
        hist_n_ = std::min(hist_n_ + 1, H);
    }
    const kfft_complex* hist_block(int64_t b) {
        constexpr int NF = 2 * lora_chan::NFFT;
        if (!xtmp_) xtmp_ = kfft_alloc_complex(lora_chan::NFFT);
        const int slot = static_cast<int>(b % hist_cap());
        const int16_t* d = &xhist_[static_cast<size_t>(slot) * NF];
        const float g = xscale_[static_cast<size_t>(slot)];
        float* o = &xtmp_[0][0];
        for (int i = 0; i < NF; i++) o[i] = d[i] * g;
        return xtmp_;
    }

    int n_probes_alive() const {
        int n = 0;
        for (auto& c : chans_) n += c->probe && !c->learned;
        return n;
    }

    // A candidate from stage 1: dechirp it at every SF / IQ orientation the
    // channels already there don't cover, from the FFT history (so the
    // preamble that set it off is decoded too). A decoded frame makes it a
    // learned channel; a preamble + SFD without a valid header is reported
    // as a detection; nothing = a back-off for that place.
    void probe(const lora_search::Candidate& cd) {
        const double rf = rf_used_;
        // already covered?
        const int sf_hi = cd.bw >= 62500 ? 12 : 10;
        std::set<std::pair<int, int>> have;
        for (auto& c : chans_) {
            if (c->bw != cd.bw || std::fabs(c->center_off - cd.off_hz) > cd.bw / 8) continue;
            // a plan / learned channel of this bandwidth is there (its signal is
            // being decoded), or it is being probed already
            if (!c->beacon_len) return;
            for (auto& rx : c->rx) have.insert({rx->sf(), rx->inverted() ? 1 : 0});
        }
        std::vector<std::pair<int, int>> want;
        for (int sf = 5; sf <= sf_hi; sf++)
            for (int inv = 0; inv <= 1; inv++)
                if (!have.count({sf, inv})) want.push_back({sf, inv});
        if (want.empty()) return;
        for (const Backoff& b : backoff_)
            if (b.bw == cd.bw && std::fabs(b.off - cd.off_hz) < cd.bw / 4 && now_ < b.until) return;
        // look back over the window that set it off + a SF12 preamble (12.25 symbols)
        const double blk_s = lora_chan::HOP / FS;
        const double back_s = cd.win_groups * lora_search::GROUP * blk_s + 12.5 * (1 << sf_hi) / cd.bw + 0.02;
        const int64_t lb = std::min<int64_t>(hist_n_, static_cast<int64_t>(std::ceil(back_s / blk_s)));
        const int64_t b_end = front_.blocks, b0 = b_end - lb;
        Channel* c = make_channel(cd.off_hz, std::isfinite(rf) ? rf + cd.off_hz : cd.off_hz, cd.bw,
                                  lb * blk_s + RING_SYMS * (1 << sf_hi) / cd.bw, b0);
        if (!c) return;
        c->probe = true;
        c->probe_t0 = now_;
        c->probe_end_t = now_ + std::min(8.0, max_airtime_s(sf_hi, cd.bw) + 1.0);
        for (auto [sf, inv] : want) {
            auto r = std::make_unique<lora_phy::LoraRx>(sf, cd.bw, c->os, &c->ring, inv == 1);
            r->strict_nocrc = true;
            r->det_log = &c->dets;
            r->desp = c->desp.get();
            c->rx.push_back(std::move(r));
        }
        for (int64_t b = b0; b < b_end; b++) c->ch.block(hist_block(b), c->outbuf);
        c->ring.push(c->outbuf.data(), c->outbuf.size());
        c->outbuf.clear();
        for (auto& rx : c->rx) {
            frames_.clear();
            rx->poll(frames_);
            for (auto& fr : frames_) queue(*c, *rx, std::move(fr));
        }
        n_probes_++;
    }

    bool has_pend(const Channel* c) const {
        for (const Pend& p : pend_) if (p.c == c) return true;
        return false;
    }

    void run_search() {
        const double t_a = thread_cpu_s();
        probe_cpu_tok_ = std::min(0.3, probe_cpu_tok_ + search_cpu_max() * std::max(0.0, now_ - cpu_tok_t_));
        cpu_tok_t_ = now_;
        // the clearest first (the budget below may not reach them all)
        std::sort(cands_.begin(), cands_.end(), [](const auto& a, const auto& b) { return a.z > b.z; });
        for (const lora_search::Candidate& cd : cands_) {
            n_cand_++;
            if (sdump_)
                host.log(fmt("[%.3f] cand ", now_) + fmt("%+.0f", cd.off_hz) + fmt(" bw %.0f", cd.bw) + fmt(" z %.1f", cd.z) +
                         fmt(" snr %.1f", cd.snr_db) + fmt(" tokens %.1f", probe_tokens_) + fmt(" alive %.0f", n_probes_alive()));
            // <= 6 probes a second (token bucket), <= MAX_PROBES at once, within the CPU budget
            probe_tokens_ = std::min(6.0, probe_tokens_ + 6.0 * (now_ - probe_tok_t_));
            probe_tok_t_ = now_;
            if (probe_tokens_ < 1 || n_probes_alive() >= MAX_PROBES) continue;
            if (probe_cpu_tok_ <= 0 && now_ - last_probe_t_ < PROBE_MIN_GAP_S) {   // over budget: slowed, not stopped
                n_slowed_++;
                slowed_t_ = now_;
                continue;
            }
            const uint64_t before = n_probes_;
            const double c0 = thread_cpu_s();
            probe(cd);
            if (n_probes_ != before) {
                probe_tokens_ -= 1;
                probe_cpu_tok_ -= thread_cpu_s() - c0;
                last_probe_t_ = now_;
            }
        }
        // probes: decoded -> learned (the receivers that decoded stay);
        // waiting for a packet's tail -> kept; else done
        for (size_t i = 0; i < chans_.size();) {
            Channel* c = chans_[i].get();
            bool drop = false;
            if (has_pend(c)) {   // a frame of it waits in pend_ (pointers to its receivers): next round
                i++;
                continue;
            }
            if (c->probe && !c->learned) {
                // a receiver stays while its preamble may still be coming in
                // (14 of its symbols since the probe began: the trigger can
                // come early in a long preamble) or while it has a packet
                c->rx.erase(std::remove_if(c->rx.begin(), c->rx.end(),
                                           [&](const auto& r) {
                                               return !r->pending() && now_ > c->probe_t0 + 14.0 * (1 << r->sf()) / c->bw;
                                           }),
                            c->rx.end());
                bool pending = false;
                for (auto& rx : c->rx) pending |= rx->pending();
                if (!has_pend(c) && (c->rx.empty() || now_ > c->probe_end_t)) {
                    report_detections(*c);
                    Backoff* bo = nullptr;
                    for (Backoff& b : backoff_)
                        if (b.bw == c->bw && std::fabs(b.off - c->center_off) < c->bw / 4) bo = &b;
                    if (!bo) {
                        backoff_.push_back({c->center_off, c->bw, 0, 1.0});
                        bo = &backoff_.back();
                    }
                    bo->wait = std::min(60.0, bo->wait * 2);
                    bo->until = now_ + bo->wait;
                    drop = true;
                } else if (pending) {
                    // a packet in progress: the other receivers go
                    c->rx.erase(std::remove_if(c->rx.begin(), c->rx.end(), [](const auto& r) { return !r->pending(); }), c->rx.end());
                }
            } else if (c->learned && c->probe && !has_pend(c)) {
                // a probe that decoded: keep the receivers that did
                c->rx.erase(std::remove_if(c->rx.begin(), c->rx.end(),
                                           [&](const auto& r) {
                                               for (const Learned& l : learned_)
                                                   if (l.sf == r->sf() && l.inv == r->inverted() && l.bw == c->bw &&
                                                       std::fabs(l.f_hz - c->name_hz) < c->bw / 4)
                                                       return false;
                                               return !r->pending();
                                           }),
                            c->rx.end());
                for (auto& r : c->rx) { r->det_log = nullptr; }
                c->probe = false;
                describe();
            } else if (c->learned && !c->probe && !has_pend(c)) {
                // a learned channel whose entry expired
                bool known = false;
                for (const Learned& l : learned_)
                    if (l.bw == c->bw && std::fabs(l.f_hz - c->name_hz) < c->bw / 4) known = true;
                if (!known) drop = true;
            }
            if (drop) {
                chans_.erase(chans_.begin() + static_cast<long>(i));
                if (!c->probe) describe();
            } else {
                i++;
            }
        }
        backoff_.erase(std::remove_if(backoff_.begin(), backoff_.end(), [&](const Backoff& b) { return now_ > b.until + 300; }),
                       backoff_.end());
        search_cpu_s_ += thread_cpu_s() - t_a;
        // the search's share of real time, over ~10 s
        if (search_cpu_t0_ == 0) search_cpu_t0_ = now_;
        if (now_ - search_cpu_t0_ >= 1.0) {
            const double f = search_cpu_s_ / (now_ - search_cpu_t0_);
            search_cpu_frac_ = search_cpu_win_ == 0 ? f : 0.7 * search_cpu_frac_ + 0.3 * f;
            search_cpu_win_ = 1;
            search_cpu_s_ = 0;
            search_cpu_t0_ = now_;
        }
    }

    // a probe's preamble + SFD that no header followed: a LoRa signal at that
    // frequency / SF / BW / IQ / sync word, reported (and its bearing from
    // the preamble) - not decodable here (implicit header, a collision, too weak)
    void report_detections(Channel& c) {
        if (c.dets.empty()) return;
        // the strongest of each SF / IQ
        std::map<std::pair<int, int>, lora_phy::Detect> best;
        for (const lora_phy::Detect& d : c.dets) {
            if (d.header_ok) continue;
            auto& b = best[{d.sf, d.inverted ? 1 : 0}];
            if (b.sf == 0 || d.snr_db > b.snr_db) b = d;
        }
        c.dets.clear();
        // a weaker SF at the same place is a slope alias of the strongest
        // (SF7 / 250 kHz dechirps like SF9 / 500 kHz) - keep the best one
        const lora_phy::Detect* top = nullptr;
        for (auto& kv : best)
            if (!top || kv.second.snr_db > top->snr_db) top = &kv.second;
        if (!top) return;
        const lora_phy::Detect& d = *top;
        // a clean preamble (most of the window's energy in its bin) with a
        // measured sync word, seen AGAIN at the same frequency / SF / BW / IQ /
        // sync within 10 min - a one-off match of a non-LoRa burst (FSK data
        // dechirps into a chance peak now and then, at SF5 / SF6 more often)
        // is not reported
        if (d.sync_word < 0 || d.snr_db < -6) return;
        // Not another transmitter (2026-10-16: lora-mesh-five listed "LoRa
        // 921.733 SF9 0x00" and four more at the bearings of its real nodes):
        // - inside / next to a packet decoded - or being decoded - at the same
        //   time in the same band: its alias (a chirp of another SF / BW with
        //   the same slope - SF11 / 250 kHz dechirps like SF9 / 125 kHz and
        //   SF7 / 62.5 kHz - or a piece of its chirps through the probe's
        //   channel edge)
        // - sync word 0x00: both sync symbols on the preamble's own bin, no
        //   sync word told apart from the preamble - what such an alias gives;
        //   radios use 0x12 (private), 0x34 (LoRaWAN), 0x2B (Meshtastic) ...
        {
            const int L = c.os << d.sf;
            if (d.sync_word == 0 ||
                near_decoded(c, tstamp(c, d.sfd - 10LL * L), tstamp(c, d.sfd + 3LL * L), c.center_off + d.cfo_hz, c.bw)) {
                n_probe_alias_++;
                return;
            }
        }
        const double f_hz = std::isfinite(rf_used_) ? rf_used_ + c.center_off + d.cfo_hz : c.center_off + d.cfo_hz;
        bool again = false;
        for (DetSeen& sn : det_seen_)
            if (sn.bw == c.bw && sn.sf == d.sf && sn.inv == d.inverted && sn.sync == d.sync_word && std::fabs(sn.f - f_hz) < c.bw / 8 &&
                now_ - sn.t < 600) {
                again = true;
                sn.t = now_;
            }
        if (!again) {
            det_seen_.push_back({f_hz, c.bw, d.sf, d.inverted, d.sync_word, now_});
            if (det_seen_.size() > 400) det_seen_.erase(det_seen_.begin());
            return;
        }
        n_probe_det_++;
        const std::string tid = raw_tid(std::round(f_hz / 1e3) * 1e3, d.sf, d.sync_word, d.inverted);
        const int L = c.os << d.sf;
        double ts = tstamp(c, d.sfd - 8LL * L), te = tstamp(c, d.sfd + 2LL * L);
        if (te - ts > 0.24) ts = te - 0.24;
        Node& nd = nodes_[tid];
        const bool fresh = nd.packets == 0;
        if (fresh) {
            nd.id = tid;
            nd.first = now_;
            nd.proto = "LoRa";
            nd.found_by_search = true;
            nd.decoded = false;
            nd.label = "preamble only - header not decoded";
        }
        nd.last = now_;
        nd.rssi = fresh ? d.snr_db : nd.rssi + 0.3f * (d.snr_db - nd.rssi);
        nd.freq_mhz = f_hz / 1e6;
        nd.sf = d.sf;
        nd.bw = c.bw;
        nd.inverted = d.inverted;
        nd.sync_word = d.sync_word;
        nd.packets++;
        kp::Talker t;
        t.id = tid;
        t.label = nd.label;
        t.start_s = ts;
        t.end_s = te;
        t.packet = true;
        t.freq_hz = c.center_off + d.cfo_hz;
        t.bw_hz = c.bw;
        t.avg_s = 10;
        t.keep_s = 1800;
        if (d.sync_word >= 0 && set_cov(t, d.ant, d.sym_y)) host.talker(t);
        const std::string txt = fmt("%.3f MHz", f_hz / 1e6) + " SF" + std::to_string(d.sf) +
                                fmt(c.bw < 1e5 ? " BW%.1f" : " BW%.0f", c.bw / 1e3) + (d.inverted ? " inverted IQ" : "") +
                                (d.sync_word >= 0 ? fmt2(" sync 0x%02X", d.sync_word) : std::string(""));
        recent_det_.push_back({txt + " (header not decoded)", now_});
        if (fresh) host.event("🔎 LoRa found by the search: " + txt + " - preamble + SFD, the header did not decode", 30.0);
        mesh::Header mh;
        lorawan::Frame lw;
        update_row(nd, mh, lw, "LoRa");
    }

    // An undecoded preamble at host time [t0, t1] in f +- bw / 2 (Hz from the
    // VFO centre): inside / next to (50 ms, an eighth of its BW) a packet
    // decoded at the same time in the same band - reported (heard_), waiting
    // for its duplicate check (pend_) or in progress on another channel (a
    // preamble found / the header decoded; on the search's probes only a
    // decoded header counts - their preambles may be such aliases themselves)
    bool near_decoded(const Channel& self, double t0, double t1, double f, double bw) const {
        constexpr double DT = 0.05;
        auto band = [&](double f2, double bw2) { return std::fabs(f - f2) < (bw + bw2) / 2 + bw / 8; };
        auto time = [&](double a, double b) { return a < t1 + DT && b > t0 - DT; };
        for (const Heard& h : heard_)
            if (time(h.t0, h.t1) && band(h.f, h.bw)) return true;
        for (const Pend& p : pend_)
            if (time(tstamp(*p.c, p.fr.start_sample), p.te) && band(p.c->center_off + p.fr.cfo_hz, p.c->bw)) return true;
        for (const auto& o : chans_) {
            if (o.get() == &self) continue;
            for (const auto& rx : o->rx) {
                const int64_t s = o->probe && !o->learned ? rx->decoding_from() : rx->busy_from();
                if (s >= 0 && time(tstamp(*o, s), now_) && band(o->center_off, o->bw)) return true;
            }
        }
        return false;
    }

    // the talker of a LoRa frame that names no transmitter: its channel + SF + sync word
    static std::string raw_tid(double f_hz, int sf, int sync, bool inv) {
        std::string s = fmt("LoRa %.3f", f_hz / 1e6) + " SF" + std::to_string(sf);
        if (sync >= 0) s += fmt2(" 0x%02X", sync);
        if (inv) s += " inv";
        return s;
    }

    void learn(Channel& c, const lora_phy::LoraRx& rx, const lora_phy::Frame& fr) {
        const double f_abs = std::isfinite(rf_used_) ? rf_used_ + c.center_off + fr.cfo_hz : c.center_off + fr.cfo_hz;
        const double f = std::round(f_abs / 1e3) * 1e3;
        for (Learned& l : learned_)
            if (l.bw == c.bw && l.sf == rx.sf() && l.inv == rx.inverted() && std::fabs(l.f_hz - f) < c.bw / 4) {
                l.last_t = now_;
                l.frames++;
                return;
            }
        // a plan channel already has this receiver (the probe sat beside it):
        // no duplicate learned channel - the plan channel decodes it from now on
        if (!c.learned)
            for (auto& o : chans_)
                if (o.get() != &c && !o->probe && o->bw == c.bw && std::fabs((std::isfinite(rf_used_) ? o->name_hz : o->center_off) - f) < c.bw / 8)
                    for (auto& r : o->rx)
                        if (r->sf() == rx.sf() && r->inverted() == rx.inverted()) return;
        if (static_cast<int>(learned_.size()) >= MAX_LEARNED) {
            auto it = std::min_element(learned_.begin(), learned_.end(), [](const Learned& a, const Learned& b) { return a.last_t < b.last_t; });
            learned_.erase(it);
        }
        learned_.push_back({f, c.bw, rx.sf(), rx.inverted(), fr.sync_word, now_, 1});
        c.learned = true;
        c.name_hz = f;
        c.sync = fr.sync_word;
        n_probe_hits_++;
        host.event("🔎 LoRa found by the search: " + fmt("%.3f MHz", f / 1e6) + " SF" + std::to_string(rx.sf()) +
                       fmt(c.bw < 1e5 ? " BW%.1f" : " BW%.0f", c.bw / 1e3) + (rx.inverted() ? " inverted IQ" : "") +
                       (fr.sync_word >= 0 ? fmt2(" sync 0x%02X", fr.sync_word) : std::string("")) +
                       " - now a watched channel",
                   30.0);
    }

    // A strong packet also decodes on a watched channel one bandwidth away (a
    // whole-bandwidth offset aliases to CFO 0 after dechirping): frames with
    // the same payload ending within 0.15 s are one transmission - keep the
    // channel where it is strongest (the real one; the neighbour sees it
    // through the channel filter's skirt). Held 50 ms (both decode in the same block or the next).
    struct Pend {
        Channel* c;
        const lora_phy::LoraRx* rx;
        lora_phy::Frame fr;
        double te, ready;
    };
    std::vector<Pend> pend_;
    void queue(Channel& c, const lora_phy::LoraRx& rx, lora_phy::Frame&& fr) {
        const double te = tstamp(c, fr.end_sample);
        for (Pend& p : pend_)
            if (same_frame(p.fr, fr) && std::fabs(p.te - te) < 0.15) {
                n_dup_++;
                // a plan / learned channel keeps the frame over a probe (no duplicate learned channel)
                const bool better = (p.c->probe && !p.c->learned && !(c.probe && !c.learned)) ||
                                    (fr.snr_db > p.fr.snr_db && !(c.probe && !c.learned && !(p.c->probe && !p.c->learned)));
                if (better) {
                    p.c = &c;
                    p.rx = &rx;
                    p.fr = std::move(fr);
                    p.te = te;
                }
                return;
            }
        pend_.push_back({&c, &rx, std::move(fr), te, now_ + 0.05});
    }
    // the same transmission decoded twice: identical, or (no CRC: a neighbour
    // channel's copy may carry symbol errors) the same length with >= 80 %
    // of the bytes equal
    static bool same_frame(const lora_phy::Frame& a, const lora_phy::Frame& b) {
        if (a.payload == b.payload) return true;
        if (a.payload.size() != b.payload.size() || (a.has_crc && b.has_crc)) return false;
        size_t eq = 0;
        for (size_t i = 0; i < a.payload.size(); i++) eq += a.payload[i] == b.payload[i];
        return eq * 5 >= a.payload.size() * 4;
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

    // A LoRaWAN class-B beacon (implicit header, no PHY CRC): RFU1 | Param |
    // Time (4, LSB first) | CRC1 | GwSpecific (InfoDesc + 6) | RFU2 | CRC2,
    // both CRCs CRC-16 0x1021 init 0 stored LSB first; CRC1 covers RFU1 ..
    // Time, CRC2 GwSpecific + RFU2. RFU1's length differs per region: every
    // split is tried, both CRCs must hold.
    bool beacon(const Channel& c, const lora_phy::Frame& fr, double freq, double ts, double te) {
        const std::vector<uint8_t>& p = fr.payload;
        const int L = static_cast<int>(p.size());
        if (L < 16) return false;
        for (int r1 = 0; r1 + 16 <= L; r1++) {
            const int tcrc = r1 + 5;      // after RFU1 (r1) + Param (1) + Time (4)
            const uint16_t c1 = static_cast<uint16_t>(p[tcrc] | (p[tcrc + 1] << 8));
            if (crc16_ccitt(p.data(), static_cast<size_t>(tcrc)) != c1) continue;
            const int g0 = tcrc + 2;
            const uint16_t c2 = static_cast<uint16_t>(p[L - 2] | (p[L - 1] << 8));
            if (crc16_ccitt(p.data() + g0, static_cast<size_t>(L - 2 - g0)) != c2) continue;
            const uint32_t gps = static_cast<uint32_t>(p[r1 + 1] | (p[r1 + 2] << 8) | (p[r1 + 3] << 16) | (uint32_t(p[r1 + 4]) << 24));
            const int desc = p[g0];
            double lat = NAN, lon = NAN;
            if (desc <= 2) {
                auto s24 = [&](int o) {
                    int32_t v = p[o] | (p[o + 1] << 8) | (p[o + 2] << 16);
                    if (v & 0x800000) v -= 0x1000000;
                    return v;
                };
                lat = s24(g0 + 1) * 90.0 / 8388608.0;
                lon = s24(g0 + 4) * 180.0 / 8388608.0;
            }
            n_bcn_++;
            const double ch_mhz = (std::isfinite(rf_used_) ? c.name_hz : c.center_off) / 1e6;
            const std::string tid = fmt("GW %.1f", ch_mhz);
            Node& nd = nodes_[tid];
            const bool fresh = nd.packets == 0;
            if (fresh) { nd.id = tid; nd.first = now_; nd.proto = "LoRaWAN"; }
            nd.last = now_;
            nd.rssi = fresh ? fr.snr_db : nd.rssi + 0.3f * (fr.snr_db - nd.rssi);
            nd.freq_mhz = freq;
            nd.sf = fr.sf;
            nd.bw = c.bw;
            nd.sync_word = fr.sync_word;
            nd.label = "gateway · class-B beacon" + (std::isfinite(lat) ? fmt(" · %.4f", lat) + fmt(", %.4f", lon) : std::string());
            nd.packets++;
            if (std::isfinite(lat) && (std::fabs(lat) > 1e-4 || std::fabs(lon) > 1e-4)) { nd.lat = lat; nd.lon = lon; nd.pos_t = now_; }
            host.valid(ts, te);
            kp::Talker t;
            t.id = tid;
            t.label = nd.label;
            t.start_s = ts;
            t.end_s = te;
            t.packet = true;
            t.freq_hz = c.center_off + fr.cfo_hz;
            t.bw_hz = c.bw;
            t.avg_s = 10;
            t.keep_s = 1800;
            lora_ds::CovStats cst;
            if (set_cov(t, fr.ant, fr.sym_y, &cst) && split_talker(t, cst)) host.talker(t);
            host.event(fmt2("📡 LoRaWAN class-B beacon: GPS time %u s", gps) + fmt(" · %.3f MHz", freq) + " SF" + std::to_string(fr.sf) +
                           (std::isfinite(lat) ? fmt(" · gateway at %.4f", lat) + fmt(", %.4f", lon) : std::string()),
                       60.0);
            std::vector<std::pair<std::string, std::string>> det = {
                {"Protocol", "LoRaWAN class-B beacon (sent by the gateways every 128 s)"},
                {"GPS time", std::to_string(gps) + " s" + ((gps % 128) == 0 ? "" : " (not a multiple of 128 s)")},
                {"InfoDesc", std::to_string(desc) + (desc <= 2 ? " (gateway antenna position)" : desc == 3 ? " (NetID + GatewayID)" : "")},
                {"SF / BW", std::to_string(fr.sf) + " / " + fmt("%.0f kHz", c.bw / 1e3)},
                {"Integrity", "both beacon CRCs passed"}};
            if (std::isfinite(lat)) det.push_back({"Gateway position", fmt("%.5f", lat) + fmt(", %.5f", lon)});
            host.table_row(tid, {tid, "LoRaWAN", nd.label, fmt("%.0f/", (double)fr.sf) + fmt("%.0f", c.bw / 1e3), fmt("%.3f", freq),
                                 "gateway", "beacon", "", "", fmt("%.0f", nd.rssi), std::to_string(nd.packets)});
            host.table_detail(tid, det);
            return true;
        }
        return false;
    }

    // Separation by code: the packet's own spatial covariance from its
    // de-spread symbols -> t.m / t.cov (kraken_doa runs MUSIC on it instead of
    // cutting the packet out of its stream: other transmitters overlapping it
    // stay out, and it isn't dropped for overlapping them). With one antenna
    // (independent mode, DoA off) nothing to do - true. With the antennas but
    // no usable covariance (the samples gone, too few clean symbols): false,
    // the frame gives no talker packet (from the stream it would mix in
    // whatever else is in its channel - and make kraken_doa keep 2.5 s of
    // the whole 2.4 MHz for it)
    bool set_cov(kp::Talker& t, int ant, const std::vector<lora_phy::cf>& ys, lora_ds::CovStats* stp = nullptr) {
        lora_ds::CovStats st;
        if (stp) *stp = st;
        if (raw_.m < 2) return true;
        std::vector<lora_phy::cf> R;
        const bool ok = ant >= 2 && lora_ds::code_cov(ant, ys, R, &st);
        if (stp) *stp = st;
        if (!ok) {
            n_uncoded_++;
            return false;
        }
        t.m = ant;
        t.cov = std::move(R);
        n_coded_++;
        coded_syms_ += static_cast<uint64_t>(st.used);
        coded_coh_ += st.coherence;
        return true;
    }

    // Two transmitters behind an ID that names none - "GW <MHz>" (gateways
    // answering on one downlink channel), "LoRa <MHz> SF<n> ..." - told apart
    // by their packets' spatial signatures (split.hpp; only with clear,
    // persistent evidence): "<id> #1" / "#2". false = this packet fits neither
    // of the two clearly: no talker packet
    bool split_talker(kp::Talker& t, const lora_ds::CovStats& st) {
        if (t.cov.empty() || !(t.id.rfind("GW ", 0) == 0 || t.id.rfind("LoRa ", 0) == 0)) return true;
        const std::string base = t.id;
        int ev = 0;
        const int k = split_[base].add(st.u, &ev);
        if (ev == 1) {
            host.talker_forget(base);
            host.event("📶 " + base + ": two transmitters behind this ID (their packets arrive from two directions far " +
                           "apart, again and again) - now " + base + " #1 / #2 in the DF panel",
                       60.0);
        } else if (ev == -1) {
            host.talker_forget(base + " #1");
            host.talker_forget(base + " #2");
            host.event("📶 " + base + ": one transmitter again", 60.0);
        }
        if (k < 0) {
            n_split_skip_++;
            return false;
        }
        if (k > 0) {
            t.id = base + " #" + std::to_string(k);
            t.label += " · transmitter " + std::to_string(k);
        }
        return true;
    }

    void report(Channel& c, const lora_phy::LoraRx& rx, const lora_phy::Frame& fr) {
        if (fr.payload.empty()) return;
        // where + when a packet was (near_decoded: its aliases aren't transmitters)
        heard_.erase(std::remove_if(heard_.begin(), heard_.end(), [&](const Heard& h) { return h.t1 < now_ - HEARD_KEEP_S; }),
                     heard_.end());
        if (heard_.size() < 4096) heard_.push_back({tstamp(c, fr.start_sample), tstamp(c, fr.end_sample), c.center_off + fr.cfo_hz, c.bw});
        const double freq = std::isfinite(rf_used_) ? (rf_used_ + c.center_off + fr.cfo_hz) / 1e6 : (c.center_off) / 1e6;
        double te = tstamp(c, fr.end_sample);
        double ts = tstamp(c, fr.start_sample);
        const double plen_s = te - ts;
        if (plen_s > 0.24) ts = te - 0.24;   // kraken_doa takes packets <= 0.25 s: the latest slice

        if (fr.implicit) {   // the beacon receivers are the only implicit ones
            if (c.probe && !c.learned) return;
            beacon(c, fr, freq, ts, te);
            return;
        }
        if (c.probe && !c.learned) learn(c, rx, fr);
        else if (c.learned) learn(c, rx, fr);   // refresh

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
        const bool lw_fits = lw.ok && lw_down == fr.inverted && (fr.has_crc || lw_down) &&
                             (c.lorawan || fr.sync_word == 0x34 || fr.sync_word < 0);
        if (is_mesh) {
            proto = "Meshtastic";
            handle_mesh(c, rx, fr, mh, freq, ts, te, tid, label);
        } else if (lw_fits) {
            proto = "LoRaWAN";
            handle_lorawan(c, fr, lw, freq, tid, label);
        } else {
            proto = "LoRa";
            // a frame whose sync word wasn't clear keeps the one measured before on its channel / SF
            const std::string base = raw_tid(std::isfinite(rf_used_) ? c.name_hz : c.center_off, fr.sf, -1, fr.inverted);
            int sw = fr.sync_word;
            if (sw >= 0) last_sync_[base] = sw;
            else if (last_sync_.count(base)) sw = last_sync_[base];
            else if (c.sync >= 0) sw = c.sync;
            tid = raw_tid(std::isfinite(rf_used_) ? c.name_hz : c.center_off, fr.sf, sw, fr.inverted);
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
        nd.decoded = true;
        nd.found_by_search |= c.learned;
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
        lora_ds::CovStats cst;
        if (set_cov(t, fr.ant, fr.sym_y, &cst) && split_talker(t, cst)) host.talker(t);

        update_row(nd, mh, lw, proto);
        if (host.raw_wanted()) host.raw(proto + " " + fmt("%.3f", freq) + " SF" + std::to_string(fr.sf) + " " + hexstr(fr.payload));
        if (dump_)
            host.log(fmt("[%.3f] ", ts) + proto + " f=" + fmt("%.3f", freq) + " SF" + std::to_string(fr.sf) +
                     fmt(" BW%.1f", c.bw / 1e3) + (fr.inverted ? " inv" : "") + fmt2(" sync=%02x", fr.sync_word & 0xFF) +
                     fmt(" cfo=%.0f", fr.cfo_hz) + fmt(" snr=%.1f", fr.snr_db) + (c.learned ? " learned" : "") +
                     (fr.has_crc ? std::string(" crc") : fmt2(" fec=%d/%d", fr.fec_bad, fr.fec_total)) +
                     (fr.ant >= 2 ? fmt2(" code=%d/", cst.used) + fmt2("%d", cst.total) + fmt2("/%d", fr.sym_total) +
                                        fmt(" coh=%.3f", cst.coherence) + (t.cov.empty() ? " NO-COV" : "")
                                  : std::string()) +
                     " " + hexstr(fr.payload));
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
                ev = "💬 " + ((relayed) ? ("!" + hex8(mh.from)) : nd_name(mh.from)) + ": " + mesh::utf8_printable(d.text);
                if (!relayed && label.empty()) label = d.text.substr(0, 24);
            } else if (d.portnum == mesh::NODEINFO) {
                if (!d.longname.empty()) node_names_[mh.from] = d.longname;
                Node& o = nodes_["!" + hex8(mh.from)];
                o.longname = d.longname;
                o.shortname = d.shortname;
                if (!relayed) label = d.longname;
                ev = "👤 NodeInfo !" + hex8(mh.from) + ": " + mesh::utf8_printable(d.longname) + " (" + mesh::utf8_printable(d.shortname) + ")";
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
                 fmt2(" ch#%02x", mh.chan_hash) + " - header only";
        }
        if (!label.empty()) label = mesh::utf8_printable(label);
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
            const double ch_mhz = (std::isfinite(rf_used_) ? c.name_hz : c.center_off) / 1e6;
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
        return it != node_names_.end() ? mesh::utf8_printable(it->second) : ("!" + hex8(from));
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
        host.table_row(nd.id, {nd.id, proto, proto == "Meshtastic" ? mesh::utf8_printable(name) : name, fmt("%.0f/", (double)nd.sf) + fmt(nd.bw < 1e5 ? "%.1f" : "%.0f", nd.bw / 1e3),
                               fmt("%.3f", nd.freq_mhz), from, to, hops,
                               mesh::utf8_printable(nd.last_text.substr(0, 40)), fmt("%.0f", nd.rssi),
                               std::to_string(nd.packets)});
        std::vector<std::pair<std::string, std::string>> det;
        det.push_back({"Protocol", proto});
        if (!nd.longname.empty()) det.push_back({"Long name", mesh::utf8_printable(nd.longname)});
        if (!nd.shortname.empty()) det.push_back({"Short name", mesh::utf8_printable(nd.shortname)});
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
        det.push_back({"SF / BW", std::to_string(nd.sf) + " / " + fmt(nd.bw < 1e5 ? "%.1f kHz" : "%.0f kHz", nd.bw / 1e3) +
                                      (nd.inverted ? " · inverted IQ" : "")});
        if (nd.sync_word >= 0)
            det.push_back({"Sync word", fmt2("0x%02X", nd.sync_word) +
                                            (nd.sync_word == 0x34 ? " (LoRaWAN public)" : nd.sync_word == 0x12 ? " (private / LoRaWAN private)"
                                             : nd.sync_word == 0x2B ? " (Meshtastic)" : "")});
        if (nd.found_by_search) det.push_back({"Found by", "the plan-free search (not on a watched plan channel)"});
        if (!nd.decoded) {
            det.push_back({"Decoded", "no - a LoRa preamble + SFD whose header did not decode (implicit header, a collision, "
                                      "too weak); identified by its frequency, SF and sync word"});
            host.table_detail(nd.id, det);
            return;
        }
        det.push_back({"Integrity", nd.has_crc ? std::string("payload CRC passed")
                                               : fmt2("no payload CRC (LoRaWAN downlinks send none): header checksum passed, "
                                                      "%d of %d payload codewords failed their parity", nd.fec_bad, nd.fec_total)});
        if (!nd.last_text.empty()) det.push_back({"Last text", mesh::utf8_printable(nd.last_text)});
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
        // learned channels not heard for LEARNED_KEEP_S are dropped (their
        // channel goes in run_search)
        learned_.erase(std::remove_if(learned_.begin(), learned_.end(), [&](const Learned& l) { return now_ - l.last_t > LEARNED_KEEP_S; }),
                       learned_.end());
        recent_det_.erase(std::remove_if(recent_det_.begin(), recent_det_.end(), [&](const RecentDet& d) { return now_ - d.t > 600; }),
                          recent_det_.end());
        host.fact("Nodes", std::to_string(nm) + " Meshtastic, " + std::to_string(nl) + " LoRaWAN, " + std::to_string(nr) + " LoRa");
        host.fact("Packets", std::to_string(n_mesh_) + " Meshtastic, " + std::to_string(n_lw_) + " LoRaWAN (" +
                                 std::to_string(n_lw_dn_) + " downlinks), " + std::to_string(n_bcn_) + " beacons, " +
                                 std::to_string(n_lora_) + " LoRa");
        if (raw_.m >= 2 || n_coded_ + n_uncoded_ > 0)
            host.fact("Separation by code",
                      (raw_.m >= 2 ? fmt("%.0f antennas", (double)raw_.m) : std::string("one antenna now")) +
                          fmt(" · %.0f packets de-spread", (double)n_coded_) +
                          (n_coded_ ? fmt(" (%.0f symbols", (double)coded_syms_ / n_coded_) + fmt(", coherence %.2f)", coded_coh_ / n_coded_)
                                    : std::string()) +
                          fmt(" · %.0f without a bearing (samples gone / no clear direction)", (double)n_uncoded_) +
                          split_txt() +
                          (now_ > 0 ? fmt(" · %.1f %% CPU", 100 * (desp_cpu_s_ + raw_cpu_s_) / now_) +
                                          fmt(" (de-spreading %.1f %%)", 100 * desp_cpu_s_ / now_)
                                    : std::string()));
        else
            host.fact("Separation by code", "one antenna (coherent mode with DoA on gives the plugin every antenna)");
        if (search_on()) {
            std::string s = std::string(search_ == "all" ? "7.8-500 kHz" : "62.5-500 kHz") + ", SF5-12, both IQ · " +
                fmt("%.1f %% CPU", 100 * search_cpu_frac_) + fmt(" · %.0f candidates", (double)n_cand_) +
                fmt(", %.0f probed", (double)n_probes_) + fmt(", %.0f decoded", (double)n_probe_hits_) +
                fmt(", %.0f undecoded LoRa", (double)n_probe_det_) +
                (n_probe_alias_ ? fmt(" (%.0f aliases of decoded packets left out)", (double)n_probe_alias_) : std::string()) +
                fmt(" · %.0f learned channels", (double)learned_.size());
            if (now_ - slowed_t_ < 10)
                s += fmt(" · SLOWED to its CPU budget (%.0f candidates waited)", (double)n_slowed_);
            host.fact("Search", s);
            if (!recent_det_.empty()) host.fact("Search: last undecoded", recent_det_.back().txt);
        } else {
            host.fact("Search", "off");
        }
    }

    // the IDs split into two transmitters (for the fact)
    std::string split_txt() const {
        std::string o;
        for (const auto& [id, st] : split_)
            if (st.on) o += (o.empty() ? "" : ", ") + id + " #1 / #2";
        if (o.empty()) return "";
        return " · split: " + o + (n_split_skip_ ? fmt(" (%.0f packets fitting neither)", (double)n_split_skip_) : std::string());
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
                            .category = "ISM & IoT",
                            .description = "LoRa chirp-spread-spectrum packets in the 2.4 MHz the tuner receives, in any "
                                           "country: Meshtastic (node table, text / position / nodeinfo on the public default "
                                           "channel; every firmware region), LoRaWAN uplinks + gateway downlinks + class-B "
                                           "beacons (clear MAC headers; every RP002 channel plan), and any other LoRa - a "
                                           "plan-free search finds it at any centre, SF5-12, BW 7.8-500 kHz, normal + inverted "
                                           "IQ. Region Auto (default) = every plan whose band holds the tuned frequency. The "
                                           "VFO sits on its tuner's centre with the full bandwidth; tune the receiver to the "
                                           "band. Each transmitter gets its own DoA in the 🗺 Map's 📡 DF panel",
                            .version = "2.0",
                            .sample_rate = FS,
                            .min_vfo_rate = FS,
                            .author = "KrakenSDR",
                            .options = {{"region", "Meshtastic region", "auto",
                                         "auto=Auto (from the tuned frequency)|us=US 902-928|eu_433=EU_433|eu_868=EU_868|"
                                         "cn=CN 470-510|jp=JP 920.5-923.5|anz=ANZ 915-928|anz_433=ANZ_433|ru=RU 868.7-869.2|"
                                         "kr=KR 920-923|tw=TW 920-925|in=IN 865-867|nz_865=NZ_865 864-868|th=TH 920-925|"
                                         "ua_433=UA_433|ua_868=UA_868|my_433=MY_433|my_919=MY_919|sg_923=SG_923 917-925|"
                                         "ph_433=PH_433|ph_868=PH_868|ph_915=PH_915|kz_433=KZ_433|kz_863=KZ_863|"
                                         "np_865=NP_865|br_902=BR_902 902-907.5|off=Off",
                                         "The Meshtastic region(s) whose default channels are watched (the firmware's "
                                         "frequency slots). Auto: every region whose band holds the tuned frequency "
                                         "(902-928 MHz: US + ANZ ...). A chosen region also retunes the receiver once "
                                         "into its band (option Tune)"},
                                        {"preset", "Meshtastic preset", "longfast",
                                         "shortturbo=ShortTurbo (SF7 BW500)|shortfast=ShortFast (SF7 BW250)|"
                                         "shortslow=ShortSlow (SF8)|mediumfast=MediumFast (SF9)|mediumslow=MediumSlow (SF10)|"
                                         "longfast=LongFast (SF11 BW250)|longturbo=LongTurbo (SF11 BW500)|"
                                         "longmod=LongModerate (SF11 BW125)|longslow=LongSlow (SF12 BW125)",
                                         "The primary modem preset (its channel name / SF option apply to it)"},
                                        {"sf", "Spreading factor", "preset",
                                         "preset=From the preset|auto=Scan SF7-12|7=SF7|8=SF8|9=SF9|10=SF10|11=SF11|12=SF12",
                                         "Which spreading factors to demodulate on the primary preset's channel"},
                                        {"channel", "Channel name", "", "",
                                         "The Meshtastic primary channel's name (\"\" = the default channel, named after "
                                         "the preset): its hash picks the frequency slot. ANZ LongFast = 919.875 MHz"},
                                        {"others", "Other presets", "1", "1=On|0=Off",
                                         "Also watch the other presets' default channels that fall inside the VFO"},
                                        {"lorawan", "LoRaWAN plan", "auto",
                                         "auto=Auto (from the tuned frequency)|eu868=EU868|us915=US915|au915=AU915|"
                                         "as923-1=AS923-1|as923-2=AS923-2|as923-3=AS923-3|as923-4=AS923-4|kr920=KR920|"
                                         "in865=IN865|ru864=RU864|cn470=CN470|eu433=EU433|cn779=CN779|0=Off",
                                         "The LoRaWAN regional plan(s) watched inside the VFO - uplinks, gateway "
                                         "downlinks (inverted IQ) / RX2, every data rate's SF (clear headers only). Auto: "
                                         "every plan whose band holds the tuned frequency"},
                                        {"beacons", "LoRaWAN beacons", "1", "1=On|0=Off",
                                         "Also watch the plans' class-B beacon channels (implicit header, the beacon's "
                                         "own CRCs checked; gateway position when it sends one)"},
                                        {"search", "Plan-free search", "1",
                                         "1=On (62.5-500 kHz)|all=On, also 7.8-41.7 kHz|0=Off",
                                         "Find LoRa ANYWHERE in the VFO - any centre, SF5-12 (narrow: SF5-10), both IQ orientations: "
                                         "LoRa-shaped energy is dechirped at every SF from the FFT history; a decoded "
                                         "transmitter becomes a watched channel (private links, unknown plans)"},
                                        {"cpu", "CPU budget", "normal",
                                         "low=Low|normal=Normal|high=High",
                                         "How many plan receivers (channel x SF x IQ) and how much search the plugin may "
                                         "run; what doesn't fit is listed in Watching (the search still covers it)"},
                                        {"channels", "Channels", "",
                                         "", "Explicit channel list \"MHz:SF:BWkHz[:inv|:both],...\" (SF 0 = SF7-12, SF5 / "
                                             "SF6 too; BW 7.8 / 10.4 / 15.6 / 20.8 / 31.25 / 41.7 / 62.5 / 125 / 250 / 500; "
                                             "inv = inverted IQ; overrides region / plan)"},
                                        {"tune", "Tune to the band", "1", "1=On|0=Off",
                                         "With a chosen Meshtastic region (not Auto): retune the receiver once to its "
                                         "preset channel when it is outside the region's band (never inside it)"}},
                            .map = true,
                            .manual_only = true,
                            .talkers = true,
                            .lock_vfo = true,
                            .multichannel = true,
                            .tunes = true})
